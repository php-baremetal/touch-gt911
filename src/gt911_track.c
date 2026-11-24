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

#include "gt911_track.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* A chip coordinate scaled from res to size; res 0 (unknown) keeps it as it is. */
static int scale(int v, int res, int size)
{
    return res > 0 ? (int) ((long long) v * size / res) : v;
}

void gt_track_map(const gt_track_t *t, int rx, int ry, int *px, int *py)
{
    /* the screen before the swap: the chip's axes */
    int bw = t->swap_xy ? t->h : t->w, bh = t->swap_xy ? t->w : t->h;
    int x = clampi(scale(rx, t->res_x, bw), 0, bw > 0 ? bw - 1 : 0);
    int y = clampi(scale(ry, t->res_y, bh), 0, bh > 0 ? bh - 1 : 0);
    if (t->swap_xy) {
        int s = x; x = y; y = s;
    }
    int maxx = t->w > 0 ? t->w - 1 : 0, maxy = t->h > 0 ? t->h - 1 : 0;
    *px = t->mirror_x ? maxx - x : x;
    *py = t->mirror_y ? maxy - y : y;
}

static void emit(gt_event_t *out, int *n, bm_input_kind_t kind, int id, int x, int y)
{
    out[*n].kind = kind;
    out[*n].id = id;
    out[*n].x = x;
    out[*n].y = y;
    (*n)++;
}

int gt_track_report(gt_track_t *t, uint8_t status, const uint8_t *records, gt_event_t out[GT_EVENTS_MAX])
{
    if (!(status & GT_STATUS_READY)) {
        return 0;
    }
    int count = status & GT_STATUS_COUNT;
    if (count > GT_POINTS_MAX) {
        count = GT_POINTS_MAX;
    }
    /* For each record, the slot of its track id (-1: a new finger; -2: a repeated track id, ignored). */
    int match[GT_POINTS_MAX];
    bool kept[GT_POINTS_MAX] = { false };
    for (int r = 0; r < count; r++) {
        uint8_t track = records[GT_RECORD_LEN * r];
        match[r] = -1;
        for (int p = 0; p < r; p++) {
            if (records[GT_RECORD_LEN * p] == track) {
                match[r] = -2;
                break;
            }
        }
        for (int s = 0; match[r] == -1 && s < GT_POINTS_MAX; s++) {
            if (t->slot[s].down && t->slot[s].track == track) {
                match[r] = s;
                kept[s] = true;
            }
        }
    }
    int n = 0;
    for (int s = 0; s < GT_POINTS_MAX; s++) {
        if (t->slot[s].down && !kept[s]) {
            t->slot[s].down = false;
            emit(out, &n, BM_INPUT_RELEASED, s, t->slot[s].x, t->slot[s].y);
        }
    }
    int x[GT_POINTS_MAX], y[GT_POINTS_MAX];
    for (int r = 0; r < count; r++) {
        const uint8_t *b = records + GT_RECORD_LEN * r;
        gt_track_map(t, b[1] | (b[2] << 8), b[3] | (b[4] << 8), &x[r], &y[r]);
    }
    for (int s = 0; s < GT_POINTS_MAX; s++) {
        for (int r = 0; kept[s] && r < count; r++) {
            gt_slot_t *sl = &t->slot[s];
            if (match[r] == s && (sl->x != x[r] || sl->y != y[r])) {
                sl->x = x[r];
                sl->y = y[r];
                emit(out, &n, BM_INPUT_MOVED, s, x[r], y[r]);
            }
        }
    }
    for (int r = 0; r < count; r++) {
        if (match[r] != -1) {
            continue;
        }
        for (int s = 0; s < GT_POINTS_MAX; s++) {
            gt_slot_t *sl = &t->slot[s];
            if (!sl->down) {
                sl->down = true;
                sl->track = records[GT_RECORD_LEN * r];
                sl->x = x[r];
                sl->y = y[r];
                sl->seq = ++t->seq;
                emit(out, &n, BM_INPUT_PRESSED, s, x[r], y[r]);
                break;
            }
        }
    }
    return n;
}

int gt_track_more(uint8_t status)
{
    if (!(status & GT_STATUS_READY)) {
        return 0;
    }
    int count = status & GT_STATUS_COUNT;
    if (count > GT_POINTS_MAX) {
        count = GT_POINTS_MAX;   /* as gt_track_report: a count above five is cut */
    }
    return count > 1 ? (count - 1) * GT_RECORD_LEN : 0;
}

int gt_track_lost(gt_track_t *t, gt_event_t out[GT_POINTS_MAX])
{
    int n = 0;
    for (int s = 0; s < GT_POINTS_MAX; s++) {
        if (t->slot[s].down) {
            t->slot[s].down = false;
            emit(out, &n, BM_INPUT_RELEASED, s, t->slot[s].x, t->slot[s].y);
        }
    }
    return n;
}

int gt_track_points(const gt_track_t *t, int id[GT_POINTS_MAX], int x[GT_POINTS_MAX], int y[GT_POINTS_MAX])
{
    int n = 0;
    for (int s = 0; s < GT_POINTS_MAX; s++) {
        if (!t->slot[s].down) {
            continue;
        }
        /* insertion by seq: at most five */
        int i = n++;
        while (i > 0 && t->slot[id[i - 1]].seq > t->slot[s].seq) {
            id[i] = id[i - 1];
            x[i] = x[i - 1];
            y[i] = y[i - 1];
            i--;
        }
        id[i] = s;
        x[i] = t->slot[s].x;
        y[i] = t->slot[s].y;
    }
    return n;
}

int gt_poll_period(int asked_ms)
{
    return asked_ms != 0 ? asked_ms : GT_POLL_MS_DEFAULT;
}

int gt_poll_next(bool *failing, bool ok, int period_ms, bool *report)
{
    *report = !ok && !*failing;
    *failing = !ok;
    return ok ? period_ms : GT_POLL_ERR_MS;
}
