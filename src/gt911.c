/*
   +----------------------------------------------------------------------+
   | php-baremetal: touch-gt911                                           |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/* GT911 capacitive touch controller (Goodix): up to five fingers, each with an id from its Pressed to its Released.
 * Synchronous reads (point, points) until events() starts; then the INT line kicks irq.service on the executor (or,
 * without an INT line on a GPIO, the service asks the HAL for its next run every poll_ms: polling), which reads the
 * status and the point records of the fingers down, tracks the fingers (gt911_track.c), publishes them for
 * points() and raises Baremetal\Input events through ext-input. The chip has 16-bit register addresses and ext-i2c
 * 8-bit ones: the address is written, then the bytes are read in a second transfer (the chip keeps the pointer).
 * The pins are bm_pin.h pins (a GPIO or an expander pin, "exio1"), driven only in the constructor (PHP task): the
 * service never calls bm_pin_*. The chip's configuration is read, never written. */
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "bm_i2c_driver.h"
#include "bm_i2c_hal.h"
#include "bm_input.h"
#include "bm_board.h"
#include "bm_pin.h"
#include "gt911_track.h"
#include "zend_exceptions.h"
#include "zend_interfaces.h"
#include "ext/spl/spl_exceptions.h"

#define GT_REG_PRODUCT 0x8140   /* "911\0" */
#define GT_REG_CONFIG  0x8047   /* version, x max (LE), y max (LE), touch number, module switch 1 (INT mode) */
#define GT_REG_STATUS  0x814E   /* status, then 5 records of 8 bytes */
#define GT_SAFETY_MS   100      /* while a finger is down, look again without an edge */

#define GT_CLASS       "Baremetal\\I2c\\Driver\\Gt911"
#define GT_ADDR_ALT    0x14     /* chosen by INT high at the end of the reset; 0x5D with INT low */
#define GT_REPORT_LEN  (GT_FIRST_LEN + GT_RECORD_LEN * (GT_POINTS_MAX - 1))
#define GT_INT_HIGH    3        /* INT mode 11: high level, which the falling-edge service cannot follow */

#define GT_PUB_DOWN    (1u << 31)

extern const i2c_driver_desc_t gt911_driver_desc;

/* Per-device state, allocated zeroed with the device (state_size). The pins, the mode and the resolution are written
 * once by the constructor. track belongs to the PHP task until events() starts the service, then to the service
 * alone: points() and point() then read pub[], which the service writes after each report. rec and irq are written
 * by events() on the PHP task and read by the service. */
typedef struct {
    bool inited;
    bm_pin_t int_pin, rst_pin;
    uint8_t int_mode;                /* 0x804D bits 1:0, read once, never written */
    bool polling;                    /* no INT line on a GPIO: the service runs every poll_ms */
    uint16_t poll_ms;
    bool poll_failing;               /* polling: the last look failed (the service's alone) */
    gt_track_t track;                /* the service's alone once events() started; the PHP task's before */
    _Atomic uint32_t pub[GT_POINTS_MAX];   /* published points: bit 31 down, 26..24 rank, 23..12 x, 11..0 y */
    _Atomic bool serviced;           /* events() started: points() reads pub only */
    _Atomic(void *) rec;
    _Atomic(void *) irq;
} gt_state;

/* A 16-bit register: its address written, then n bytes read (two transfers, a stop between them). */
static bm_err_t gt_read(i2c_dev_t *d, uint16_t reg, uint8_t *buf, size_t n)
{
    bm_err_t err = i2c_dev_write(d, (uint8_t[]){ (uint8_t) (reg >> 8), (uint8_t) (reg & 0xFF) }, 2);
    return err != BM_OK ? err : i2c_dev_read(d, buf, n);
}

static bm_err_t gt_write1(i2c_dev_t *d, uint16_t reg, uint8_t v)
{
    return i2c_dev_write(d, (uint8_t[]){ (uint8_t) (reg >> 8), (uint8_t) (reg & 0xFF), v }, 3);
}

/* The points of the track into pub[] (index = id), for a reader on another task. */
static void gt_publish(gt_state *st)
{
    int id[GT_POINTS_MAX], x[GT_POINTS_MAX], y[GT_POINTS_MAX];
    uint32_t v[GT_POINTS_MAX] = { 0 };
    int n = gt_track_points(&st->track, id, x, y);
    for (int i = 0; i < n; i++) {
        v[id[i]] = GT_PUB_DOWN | ((uint32_t) i << 24) | ((uint32_t) (x[i] & 0xFFF) << 12) | (uint32_t) (y[i] & 0xFFF);
    }
    for (int i = 0; i < GT_POINTS_MAX; i++) {
        atomic_store_explicit(&st->pub[i], v[i], memory_order_release);
    }
}

static void gt_emit(void *rec, const gt_event_t *ev, int n)
{
    for (int i = 0; rec && i < n; i++) {
        bm_input_touch_emit_id(rec, ev[i].id, ev[i].kind, ev[i].x, ev[i].y, (bm_input_gesture_t) 0);
    }
}

/* One look at the chip: the status and the records, the events of what changed (to rec, when not NULL), then the
 * status cleared so the chip writes the next report. The status is read with the first record (9 bytes: the common
 * case of one finger, and of no finger, costs one read); the other records only when the count says there are more.
 * The events go out before the clear, which only the chip waits for. A failed read releases every finger at its
 * last point, so a consumer never waits for a release that cannot come. */
static bm_err_t gt_poll(i2c_dev_t *d, gt_state *st, void *rec)
{
    uint8_t b[GT_REPORT_LEN];
    gt_event_t ev[GT_EVENTS_MAX];
    int n;
    bool ready = false;
    bm_err_t err = gt_read(d, GT_REG_STATUS, b, GT_FIRST_LEN);
    if (err == BM_OK && (b[0] & GT_STATUS_READY)) {
        int more = gt_track_more(b[0]);
        ready = true;
        if (more > 0) {
            err = gt_read(d, GT_REG_STATUS + GT_FIRST_LEN, b + GT_FIRST_LEN, (size_t) more);
        }
    }
    if (err != BM_OK) {
        n = gt_track_lost(&st->track, ev);
    } else {
        n = gt_track_report(&st->track, b[0], b + 1, ev);
    }
    gt_publish(st);
    gt_emit(rec, ev, n);
    if (err == BM_OK && ready) {
        err = gt_write1(d, GT_REG_STATUS, 0);
    }
    return err;
}

static bool gt_any_down(const gt_track_t *t)
{
    for (int i = 0; i < GT_POINTS_MAX; i++) {
        if (t->slot[i].down) {
            return true;
        }
    }
    return false;
}

/* irq.service, on the executor task (never touches PHP or bm_pin_*). With the INT line, the handle may still be NULL
 * when the first edge is served (bm_i2c_device_irq() has not returned yet): then the safety read is not requested for
 * that run; the next edge or report requests it. Polling, the first run is asked by events() after it stored the
 * handle, and each run asks for the next. */
static bm_err_t gt911_service(i2c_dev_t *d)
{
    gt_state *st = bm_i2c_dev_state(d);
    void *rec = atomic_load_explicit(&st->rec, memory_order_acquire);
    void *irq = atomic_load_explicit(&st->irq, memory_order_acquire);
    if (!rec) {
        return BM_OK;
    }
    bm_err_t err = gt_poll(d, st, rec);
    if (st->polling) {
        /* The HAL logs what the service returns: a chip that stops answering is reported once, not every look. */
        bool report;
        int next = gt_poll_next(&st->poll_failing, err == BM_OK, st->poll_ms, &report);
        if (irq) {
            bm_i2c_hal_irq_after(irq, (uint32_t) next);
        }
        return report ? err : BM_OK;
    }
    if (irq && gt_any_down(&st->track)) {
        /* Safety read: a release whose pulse was missed is seen by this read. */
        bm_i2c_hal_irq_after(irq, GT_SAFETY_MS);
    }
    return err;   /* logged by the HAL */
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_gt_ctor, 0, 0, 1)
    ZEND_ARG_OBJ_INFO(0, bus, Baremetal\\I2c\\Bus, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, address, IS_LONG, 0, "0x5D")
    ZEND_ARG_TYPE_MASK(0, int, MAY_BE_LONG|MAY_BE_STRING, "-1")
    ZEND_ARG_TYPE_MASK(0, rst, MAY_BE_LONG|MAY_BE_STRING, "-1")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, width, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, height, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mirror_x, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, mirror_y, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, swap_xy, _IS_BOOL, 0, "false")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, poll_ms, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_gt_onboard, 0, 0, Baremetal\\I2c\\Driver\\Gt911, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, block, IS_STRING, 0, "'touch'")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gt_point, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gt_points, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_gt_max_points, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_gt_events, 0, 0, Baremetal\\Source, 0)
ZEND_END_ARG_INFO()

/* A pin argument (int or string) resolved through bm_pin.h; false after throwing. */
static bool gt_arg_pin(const char *what, zval *z, bm_pin_t *out)
{
    char num[24];
    const char *name;
    bm_err_t err;
    *out = BM_PIN_NONE;
    if (Z_TYPE_P(z) == IS_LONG) {
        snprintf(num, sizeof num, ZEND_LONG_FMT, Z_LVAL_P(z));
        name = num;
        err = Z_LVAL_P(z) == -1 ? BM_OK : Z_LVAL_P(z) < 0 ? BM_ERR_INVALID_ARG : bm_pin_resolve(name, out);
    } else {
        name = Z_STRVAL_P(z);
        err = strlen(name) != Z_STRLEN_P(z) ? BM_ERR_INVALID_ARG : bm_pin_resolve(name, out);
    }
    if (err != BM_OK) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, GT_CLASS ": %s '%s': %s", what, name,
                                bm_pin_err_name(err));
        return false;
    }
    return true;
}

/* Reset with the address chosen by INT: rst low, INT at the level of the address (only when INT is a GPIO: an
 * expander pin cannot be let go afterwards), 100 ms, rst high, 200 ms, INT back to an input. Without rst nothing is
 * done. */
static bool gt_reset(const gt_state *st, uint16_t addr)
{
    if (!bm_pin_valid(st->rst_pin)) {
        return true;
    }
    bool drive_int = bm_pin_is_gpio(st->int_pin);
    bm_err_t err = bm_pin_mode(st->rst_pin, BM_PIN_OUTPUT);
    if (err == BM_OK) {
        err = bm_pin_set(st->rst_pin, false);
    }
    if (err == BM_OK && drive_int) {
        bm_pin_mode(st->int_pin, BM_PIN_OUTPUT);
        bm_pin_set(st->int_pin, addr == GT_ADDR_ALT);
    }
    if (err == BM_OK) {
        bm_delay_ms(100);
        err = bm_pin_set(st->rst_pin, true);
    }
    if (err == BM_OK) {
        bm_delay_ms(200);
    }
    if (drive_int) {
        bm_pin_mode(st->int_pin, BM_PIN_INPUT);
    }
    if (err != BM_OK) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, GT_CLASS ": rst: %s", bm_pin_err_name(err));
        return false;
    }
    return true;
}

/* The product id at 0x8140 must start with "911"; false after throwing. */
static bool gt_check_id(i2c_dev_t *d, uint16_t addr)
{
    uint8_t id[4];
    if (gt_read(d, GT_REG_PRODUCT, id, sizeof id) != BM_OK) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, GT_CLASS ": no GT911 answers at 0x%02X (no answer)",
                                (unsigned) addr);
        return false;
    }
    if (memcmp(id, "911", 3) == 0) {
        return true;
    }
    char shown[4 * 4 + 1], *p = shown;
    for (size_t i = 0; i < sizeof id; i++) {
        if (id[i] >= 0x20 && id[i] < 0x7F && id[i] != '\\' && id[i] != '\'') {
            *p++ = (char) id[i];
        } else {
            p += snprintf(p, 5, "\\x%02X", id[i]);
        }
    }
    *p = '\0';
    zend_throw_exception_ex(spl_ce_RuntimeException, 0, GT_CLASS ": no GT911 answers at 0x%02X (product id '%s')",
                            (unsigned) addr, shown);
    return false;
}

PHP_METHOD(Gt911, __construct)
{
    zval *zbus, *zint = NULL, *zrst = NULL;
    zend_long addr = 0x5D, width = 0, height = 0, poll_ms = 0;
    bool mx = 0, my = 0, sw = 0;
    ZEND_PARSE_PARAMETERS_START(1, 10)
        Z_PARAM_OBJECT(zbus)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(addr)
        Z_PARAM_ZVAL(zint)
        Z_PARAM_ZVAL(zrst)
        Z_PARAM_LONG(width)
        Z_PARAM_LONG(height)
        Z_PARAM_BOOL(mx)
        Z_PARAM_BOOL(my)
        Z_PARAM_BOOL(sw)
        Z_PARAM_LONG(poll_ms)
    ZEND_PARSE_PARAMETERS_END();
    (void) zbus;   /* checked by the shared constructor */
    if (zint && Z_TYPE_P(zint) != IS_LONG && Z_TYPE_P(zint) != IS_STRING) {
        zend_argument_type_error(3, "must be of type string|int, %s given", zend_zval_type_name(zint));
        RETURN_THROWS();
    }
    if (zrst && Z_TYPE_P(zrst) != IS_LONG && Z_TYPE_P(zrst) != IS_STRING) {
        zend_argument_type_error(4, "must be of type string|int, %s given", zend_zval_type_name(zrst));
        RETURN_THROWS();
    }
    if (width < 0 || width > 4096) {
        zend_argument_value_error(5, "must be between 0 and 4096");
        RETURN_THROWS();
    }
    if (height < 0 || height > 4096) {
        zend_argument_value_error(6, "must be between 0 and 4096");
        RETURN_THROWS();
    }
    if (poll_ms != 0 && (poll_ms < GT_POLL_MS_MIN || poll_ms > GT_POLL_MS_MAX)) {
        zend_argument_value_error(10, "must be 0 or between %d and %d", GT_POLL_MS_MIN, GT_POLL_MS_MAX);
        RETURN_THROWS();
    }

    /* The shared constructor reads (bus, address, hz) from the first arguments of the call, so it is shown only
     * those two. */
    uint32_t nargs = ZEND_CALL_NUM_ARGS(execute_data);
    if (nargs > 2) {
        ZEND_CALL_NUM_ARGS(execute_data) = 2;
    }
    i2c_driver_ctor(INTERNAL_FUNCTION_PARAM_PASSTHRU, &gt911_driver_desc);
    ZEND_CALL_NUM_ARGS(execute_data) = nargs;
    if (EG(exception)) {
        RETURN_THROWS();
    }

    i2c_dev_t *d = bm_i2c_device_dev(Z_OBJ_P(ZEND_THIS));
    gt_state *st = d ? bm_i2c_dev_state(d) : NULL;
    if (!st) {
        zend_throw_exception(zend_ce_exception, GT_CLASS ": the device is not initialised", 0);
        RETURN_THROWS();
    }
    if (st->inited) {   /* a second handle on the same device keeps the first configuration */
        return;
    }
    bm_pin_t intp = BM_PIN_NONE, rst = BM_PIN_NONE;
    if ((zint && !gt_arg_pin("int", zint, &intp)) || (zrst && !gt_arg_pin("rst", zrst, &rst))) {
        RETURN_THROWS();
    }
    st->int_pin = intp;
    st->rst_pin = rst;
    if (!gt_reset(st, (uint16_t) addr) || !gt_check_id(d, (uint16_t) addr)) {
        RETURN_THROWS();
    }
    uint8_t cfg[7];
    bm_err_t err = gt_read(d, GT_REG_CONFIG, cfg, sizeof cfg);
    if (err != BM_OK) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, GT_CLASS ": cannot read the configuration: %s",
                                bm_i2c_hal_err_name(err));
        RETURN_THROWS();
    }
    int res_x = cfg[1] | (cfg[2] << 8), res_y = cfg[3] | (cfg[4] << 8);
    st->int_mode = cfg[6] & 0x03;
    st->polling = !bm_pin_is_gpio(intp);
    st->poll_ms = (uint16_t) gt_poll_period((int) poll_ms);
    if (width == 0) {
        width = res_x;
    }
    if (height == 0) {
        height = res_y;
    }
    if (width < 1 || width > 4096 || height < 1 || height > 4096) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
                                GT_CLASS ": the chip reports %d x %d; give the screen's width and height", res_x,
                                res_y);
        RETURN_THROWS();
    }
    st->track.res_x = res_x;
    st->track.res_y = res_y;
    st->track.w = (int) (sw ? height : width);
    st->track.h = (int) (sw ? width : height);
    st->track.mirror_x = mx;
    st->track.mirror_y = my;
    st->track.swap_xy = sw;
    st->inited = true;
}

/* The value of block.key for the constructor's int|string pin: an integer, a string, or -1 when absent. false after
 * throwing when it names no usable pin. */
static bool board_pin(const char *id, const char *block, const char *key, zval *out)
{
    bm_pin_t pin;
    bm_err_t err = bm_pin_board(block, key, &pin);
    if (err != BM_OK) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "board '%s': %s.%s: %s", id, block, key,
                                bm_pin_err_name(err));
        return false;
    }
    int v;
    const char *s;
    if (bm_board_int(block, key, &v)) {
        ZVAL_LONG(out, v);
    } else if ((s = bm_board_str(block, key)) != NULL) {
        ZVAL_STRING(out, s);
    } else {
        ZVAL_LONG(out, -1);
    }
    return true;
}

static int board_int_or(const char *block, const char *key, int def)
{
    int v;
    return bm_board_int(block, key, &v) ? v : def;
}

/* onboard(string $block = 'touch'): the controller described by a block of the board, on the bus of its 'i2c'
 * block. Keys: int, rst (optional pins: a GPIO number or an expander pin), width, height, address, mirror-x,
 * mirror-y, swap-xy, poll-ms (optional; width and height in the chip's axes, 0 or absent: the chip's resolution;
 * poll-ms only without an INT line on a GPIO). */
PHP_METHOD(Gt911, onboard)
{
    zend_string *zblock = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR(zblock)
    ZEND_PARSE_PARAMETERS_END();
    const char *block = zblock ? ZSTR_VAL(zblock) : "touch";
    const char *id = bm_board_id();
    const char *type = bm_board_type(block);
    if (!type) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "board '%s' has no '%s' block", id, block);
        RETURN_THROWS();
    }
    if (strcmp(type, "gt911") != 0) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "board '%s' has a %s of type '%s', not 'gt911'", id,
                                block, type);
        RETURN_THROWS();
    }
    zval args[10];
    ZVAL_UNDEF(&args[2]);
    ZVAL_UNDEF(&args[3]);
    if (!board_pin(id, block, "int", &args[2]) || !board_pin(id, block, "rst", &args[3])) {
        zval_ptr_dtor(&args[2]);
        RETURN_THROWS();
    }

    zend_string *bus_name = zend_string_init("Baremetal\\I2c\\Bus", sizeof("Baremetal\\I2c\\Bus") - 1, 0);
    zend_class_entry *bus_ce = zend_lookup_class(bus_name);
    zend_string_release(bus_name);
    if (!bus_ce) {
        zval_ptr_dtor(&args[2]);
        zval_ptr_dtor(&args[3]);
        zend_throw_exception(zend_ce_exception, GT_CLASS ": Baremetal\\I2c\\Bus is not available", 0);
        RETURN_THROWS();
    }
    zval bus;
    zend_call_method_with_0_params(NULL, bus_ce, NULL, "onboard", &bus);
    if (EG(exception)) {
        zval_ptr_dtor(&args[2]);
        zval_ptr_dtor(&args[3]);
        RETURN_THROWS();
    }

    zend_class_entry *ce = EX(func)->common.scope;
    object_init_ex(return_value, ce);
    ZVAL_COPY_VALUE(&args[0], &bus);
    ZVAL_LONG(&args[1], board_int_or(block, "address", 0x5D));
    ZVAL_LONG(&args[4], board_int_or(block, "width", 0));
    ZVAL_LONG(&args[5], board_int_or(block, "height", 0));
    ZVAL_BOOL(&args[6], board_int_or(block, "mirror-x", 0) != 0);
    ZVAL_BOOL(&args[7], board_int_or(block, "mirror-y", 0) != 0);
    ZVAL_BOOL(&args[8], board_int_or(block, "swap-xy", 0) != 0);
    ZVAL_LONG(&args[9], board_int_or(block, "poll-ms", 0));
    zend_call_known_instance_method(ce->constructor, Z_OBJ_P(return_value), NULL, 10, args);
    zval_ptr_dtor(&bus);
    zval_ptr_dtor(&args[2]);
    zval_ptr_dtor(&args[3]);
    if (EG(exception)) {
        zval_ptr_dtor(return_value);
        ZVAL_NULL(return_value);   /* the caller frees an unused result: not the object freed here */
        RETURN_THROWS();
    }
}

/* The fingers down now, ranked in press order: from a synchronous read before events(), from pub[] after. */
static int gt_current(i2c_dev_t *d, int id[GT_POINTS_MAX], int x[GT_POINTS_MAX], int y[GT_POINTS_MAX])
{
    gt_state *st = bm_i2c_dev_state(d);
    if (!atomic_load_explicit(&st->serviced, memory_order_acquire)) {
        gt_poll(d, st, NULL);
        return gt_track_points(&st->track, id, x, y);
    }
    int n = 0;
    int at[GT_POINTS_MAX];
    uint32_t v[GT_POINTS_MAX];
    for (int i = 0; i < GT_POINTS_MAX; i++) {
        uint32_t p = atomic_load_explicit(&st->pub[i], memory_order_acquire);
        if (!(p & GT_PUB_DOWN)) {
            continue;
        }
        /* insertion by rank */
        int k = n++;
        while (k > 0 && ((v[k - 1] >> 24) & 7) > ((p >> 24) & 7)) {
            v[k] = v[k - 1];
            at[k] = at[k - 1];
            k--;
        }
        v[k] = p;
        at[k] = i;
    }
    for (int k = 0; k < n; k++) {
        id[k] = at[k];
        x[k] = (int) ((v[k] >> 12) & 0xFFF);
        y[k] = (int) (v[k] & 0xFFF);
    }
    return n;
}

/* point(): the first finger still down, or null. */
PHP_METHOD(Gt911, point)
{
    ZEND_PARSE_PARAMETERS_NONE();
    i2c_dev_t *d = i2c_device_this(ZEND_THIS);
    if (!d) {
        RETURN_THROWS();
    }
    int id[GT_POINTS_MAX], x[GT_POINTS_MAX], y[GT_POINTS_MAX];
    if (gt_current(d, id, x, y) == 0) {
        RETURN_NULL();
    }
    array_init_size(return_value, 2);
    add_assoc_long(return_value, "x", x[0]);
    add_assoc_long(return_value, "y", y[0]);
}

/* points(): the fingers down, in the order they came down, with their id. */
PHP_METHOD(Gt911, points)
{
    ZEND_PARSE_PARAMETERS_NONE();
    i2c_dev_t *d = i2c_device_this(ZEND_THIS);
    if (!d) {
        RETURN_THROWS();
    }
    int id[GT_POINTS_MAX], x[GT_POINTS_MAX], y[GT_POINTS_MAX];
    int n = gt_current(d, id, x, y);
    if (n == 0) {
        RETURN_EMPTY_ARRAY();
    }
    array_init_size(return_value, (uint32_t) n);
    for (int i = 0; i < n; i++) {
        zval p;
        array_init_size(&p, 3);
        add_assoc_long(&p, "id", id[i]);
        add_assoc_long(&p, "x", x[i]);
        add_assoc_long(&p, "y", y[i]);
        add_next_index_zval(return_value, &p);
    }
}

/* maxPoints(): the GT911 reports five fingers. */
PHP_METHOD(Gt911, maxPoints)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(GT_POINTS_MAX);
}

/* events(): turns on the touch events: from the INT line when it is on a GPIO, otherwise by polling every poll_ms.
 * Idempotent: again returns a new Source handle on the same record. serviced and the record go into the state
 * BEFORE the service starts (starting it hands over through the executor queue, which synchronises): from then on
 * the PHP task leaves the track alone. The HAL handle goes in after, and the service copes with NULL. When the
 * service cannot start, both are taken back and point() / points() go on reading the chip. */
PHP_METHOD(Gt911, events)
{
    ZEND_PARSE_PARAMETERS_NONE();
    i2c_dev_t *d = i2c_device_this(ZEND_THIS);
    if (!d) {
        RETURN_THROWS();
    }
    gt_state *st = bm_i2c_dev_state(d);
    if (!st->polling && st->int_mode == GT_INT_HIGH) {
        zend_throw_exception(spl_ce_RuntimeException,
                             "Gt911::events(): the chip signals with INT high (0x804D bits 1:0 = 11); the events "
                             "need a falling edge", 0);
        RETURN_THROWS();
    }
    void *rec = NULL;
    zend_object *src = bm_input_touch_events(Z_OBJ_P(ZEND_THIS), &rec);
    if (!src) {
        RETURN_THROWS();
    }
    bool was_serviced = atomic_load_explicit(&st->serviced, memory_order_relaxed);
    if (!was_serviced) {
        gt_publish(st);   /* the points known so far, for points() until the first report */
        atomic_store_explicit(&st->serviced, true, memory_order_release);
        atomic_store_explicit(&st->rec, rec, memory_order_release);
    }
    void *irq = bm_i2c_device_irq(Z_OBJ_P(ZEND_THIS), st->polling ? -1 : st->int_pin.index);
    if (!irq) {
        /* No service started (a started one is never NULL again): the device goes back to the synchronous reads,
         * so point() and points() keep reading the chip. */
        if (!was_serviced) {
            atomic_store_explicit(&st->rec, NULL, memory_order_relaxed);
            atomic_store_explicit(&st->serviced, false, memory_order_relaxed);
        }
        OBJ_RELEASE(src);
        RETURN_THROWS();
    }
    atomic_store_explicit(&st->irq, irq, memory_order_release);
    if (st->polling) {
        bm_i2c_hal_irq_after(irq, 0);   /* the first look, now; each run asks for the next */
    }
    RETURN_OBJ(src);
}

static const zend_function_entry gt911_methods[] = {
    PHP_ME(Gt911, __construct, arginfo_gt_ctor,       ZEND_ACC_PUBLIC)
    PHP_ME(Gt911, onboard,     arginfo_gt_onboard,    ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_ME(Gt911, point,       arginfo_gt_point,      ZEND_ACC_PUBLIC)
    PHP_ME(Gt911, points,      arginfo_gt_points,     ZEND_ACC_PUBLIC)
    PHP_ME(Gt911, maxPoints,   arginfo_gt_max_points, ZEND_ACC_PUBLIC)
    PHP_ME(Gt911, events,      arginfo_gt_events,     ZEND_ACC_PUBLIC)
    PHP_FE_END
};

const i2c_driver_desc_t gt911_driver_desc = {
    .name = "gt911",
    .php_class = "Gt911",
    .capability = "Baremetal\\Input\\Touch",
    .default_addrs = { 0x5D, 0x14 },
    .n_default_addrs = 2,
    .default_hz = 400000,
    .state_size = sizeof(gt_state),
    .methods = gt911_methods,
    /* The INT line (falling edge) kicks the service on the executor; without it, the service asks for its next run. */
    .irq = { .supported = true, .service = gt911_service },
};
