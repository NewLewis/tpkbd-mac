#ifndef TPKBD_TWO_FINGER_TAP_H
#define TPKBD_TWO_FINGER_TAP_H

#include <stdint.h>
#include <stdlib.h>

typedef struct { int id, x, y; } TapContact;
typedef struct {
    int active, valid, seen, paired, pair_reports, released, previous_mask;
    uint64_t began_ms, last_ms, overlap_ms, released_ms;
    TapContact fingers[2];
} TwoFingerTap;

static inline void two_finger_tap_cancel(TwoFingerTap *t) {
    // Invalidity is sticky until all contacts have lifted.
    t->valid = 0;
}

static inline int two_finger_tap_update(TwoFingerTap *t, const TapContact *c,
                                       int count, uint64_t ms, int allowed,
                                       int slop, int max_ms) {
    if (!t->active) {
        if (!count) return 0;
        *t = (TwoFingerTap){.active = 1, .valid = 1, .began_ms = ms, .last_ms = ms};
    }
    if (!allowed || count > 2 || max_ms <= 0 || ms < t->last_ms ||
        ms - t->began_ms >= (uint64_t)max_ms) t->valid = 0;
    if (t->previous_mask == 3 && ms >= t->last_ms) t->overlap_ms += ms - t->last_ms;
    if (t->released && ms - t->released_ms > 80) t->valid = 0;

    int mask = 0;
    for (int i = 0; i < count; i++) {
        int index = -1;
        for (int j = 0; j < t->seen; j++) if (t->fingers[j].id == c[i].id) { index = j; break; }
        if (index < 0) {
            if (t->seen == 2) { t->valid = 0; continue; }
            index = t->seen++;
            t->fingers[index] = c[i];
        }
        if (mask & (1 << index)) t->valid = 0; // Duplicate contact identities.
        mask |= 1 << index;
        if (abs(c[i].x - t->fingers[index].x) > slop ||
            abs(c[i].y - t->fingers[index].y) > slop) t->valid = 0;
    }
    if (t->paired && (mask & ~t->previous_mask)) t->valid = 0; // Re-touch/replacement.
    if (count && (t->previous_mask & ~mask)) {
        if (!t->paired) t->valid = 0;
        if (!t->released) { t->released = 1; t->released_ms = ms; }
    }
    if (count == 2 && mask == 3) {
        if (!t->paired && ms - t->began_ms > 80) t->valid = 0;
        t->paired = 1;
        if (t->pair_reports < 2) t->pair_reports++;
    }
    if (!count) {
        int result = t->valid && t->paired && t->pair_reports == 2 && t->overlap_ms >= 10;
        *t = (TwoFingerTap){0};
        return result;
    }
    t->last_ms = ms;
    t->previous_mask = mask;
    return 0;
}

#endif
