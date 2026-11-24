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

/* Pure tracking logic of the GT911: the status byte (0x814E) and the point records (0x814F, 8 bytes each: track id,
   x low, x high, y low, y high, size low, size high, reserved) to touch events with stable ids. No PHP, no I2C. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "bm_input.h"

#define GT_POINTS_MAX 5
#define GT_EVENTS_MAX (2 * GT_POINTS_MAX)
#define GT_STATUS_READY 0x80
#define GT_STATUS_COUNT 0x0F
#define GT_RECORD_LEN 8
#define GT_FIRST_LEN (1 + GT_RECORD_LEN)   /* the status and the first record, read together */

typedef struct {
    bool down;
    uint8_t track;     /* the chip's track id of the finger */
    int x, y;          /* screen coordinates */
    uint32_t seq;      /* when it came down (for the order of points()) */
} gt_slot_t;

typedef struct {
    int w, h;                          /* screen size, after swap_xy */
    int res_x, res_y;                  /* the chip's resolution (0x8048, 0x804A); 0: as the screen */
    bool mirror_x, mirror_y, swap_xy;  /* applied as the display does: scale, swap, then mirror */
    gt_slot_t slot[GT_POINTS_MAX];     /* index = the id given to the events */
    uint32_t seq;
} gt_track_t;

typedef struct { bm_input_kind_t kind; int id, x, y; } gt_event_t;

/* One report: status and min(count, 5) records. Without GT_STATUS_READY nothing changes and 0 is returned. Events:
   the Released of the fingers gone first, then the Moved of those that moved, then the Pressed of the new ones
   (each gets the lowest free id). Returns how many. */
int gt_track_report(gt_track_t *t, uint8_t status, const uint8_t *records, gt_event_t out[GT_EVENTS_MAX]);
/* The read plan of a report: the status is read with the first record (GT_FIRST_LEN bytes at the status register);
   returns how many bytes of the other records follow, read from the status register + GT_FIRST_LEN: 0 when the
   status is not ready or with no finger or one, (min(count, 5) - 1) * GT_RECORD_LEN otherwise. */
int gt_track_more(uint8_t status);
/* A failed read: every finger down is Released at its last point. */
int gt_track_lost(gt_track_t *t, gt_event_t out[GT_POINTS_MAX]);
/* The chip's (rx, ry) to screen coordinates: scaled from res to the size before the swap, clamped, swapped,
   mirrored. */
void gt_track_map(const gt_track_t *t, int rx, int ry, int *x, int *y);
/* The fingers down in the order they came down: ids and coordinates into the arrays; returns how many. */
int gt_track_points(const gt_track_t *t, int id[GT_POINTS_MAX], int x[GT_POINTS_MAX], int y[GT_POINTS_MAX]);

#define GT_POLL_MS_MIN 5          /* the chip reports at most every 5 ms */
#define GT_POLL_MS_MAX 100
#define GT_POLL_MS_DEFAULT 10     /* under a frame of a 55-60 Hz display (16-18 ms) */
#define GT_POLL_ERR_MS 500        /* the next look after a failed read */
/* The polling period (ms) without an INT line: asked_ms when it is not 0 (the caller checked GT_POLL_MS_MIN ..
   GT_POLL_MS_MAX), otherwise GT_POLL_MS_DEFAULT. */
int gt_poll_period(int asked_ms);
/* After one look while polling: the delay before the next one (period_ms, or GT_POLL_ERR_MS after a failed read),
   and whether a failure is reported (true for the first failed look of a run only: *failing remembers the run and a
   good look ends it, so a chip that stops answering is reported once, and again after it came back and failed
   anew). */
int gt_poll_next(bool *failing, bool ok, int period_ms, bool *report);
