#ifndef TPKBD_THREE_FINGER_DRAG_H
#define TPKBD_THREE_FINGER_DRAG_H

#include "pointer_motion.h"
#include <stdlib.h>

typedef struct { int id, x, y; } DragContact;
enum { DRAG3_IDLE, DRAG3_PENDING, DRAG3_ACTIVE, DRAG3_TAIL };
typedef struct {
    int phase, ids[3], anchor_x, anchor_y, last_x, last_y;
    int previous_dx, previous_dy, motion_run;
    uint64_t began_ns, last_motion_ns;
    PointerMotion motion;
} ThreeFingerDrag;
typedef struct { int consume, begin, end, dx, dy; } Drag3Output;

static inline void drag3_anchor(ThreeFingerDrag *d, const DragContact *c,
                                uint16_t scan, uint64_t ns) {
    int x = 0, y = 0;
    for (int i = 0; i < 3; i++) { d->ids[i] = c[i].id; x += c[i].x; y += c[i].y; }
    d->anchor_x = d->last_x = x;
    d->anchor_y = d->last_y = y;
    d->began_ns = ns;
    d->last_motion_ns = ns;
    d->previous_dx = d->previous_dy = d->motion_run = 0;
    pointer_motion_anchor(&d->motion, x, y, scan, ns);
}

static inline Drag3Output drag3_update(ThreeFingerDrag *d, const DragContact *c,
                                      int count, uint16_t scan, uint64_t ns,
                                      int enabled, int blocked, int click,
                                      double gain, int acceleration) {
    Drag3Output out = {0};
    if (d->phase == DRAG3_TAIL) {
        out.consume = 1;
        if (!count && !click) *d = (ThreeFingerDrag){0};
        return out;
    }
    if (!enabled || blocked) {
        if (d->phase != DRAG3_IDLE) {
            out.consume = 1;
            out.end = d->phase == DRAG3_ACTIVE;
            *d = (ThreeFingerDrag){.phase = count || click ? DRAG3_TAIL : DRAG3_IDLE};
        }
        return out;
    }
    if (d->phase == DRAG3_IDLE) {
        if (count != 3) return out;
        out.consume = 1;
        d->phase = click ? DRAG3_TAIL : DRAG3_PENDING;
        if (!click) drag3_anchor(d, c, scan, ns);
        return out;
    }
    if (d->phase == DRAG3_PENDING && count >= 4) {
        // Four fingers often arrive as 1 -> 2 -> 3 -> 4. A pending drag must
        // yield without generating a transient click or selection.
        *d = (ThreeFingerDrag){0};
        return out;
    }
    out.consume = 1;
    if (count < 3 || (click && d->phase == DRAG3_PENDING)) {
        out.end = d->phase == DRAG3_ACTIVE;
        *d = (ThreeFingerDrag){.phase = count || click ? DRAG3_TAIL : DRAG3_IDLE};
        return out;
    }

    // Match identities across report slots. An extra finger cannot turn an
    // established drag into a desktop swipe or change its centroid.
    int x = 0, y = 0, matched = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < count; j++) {
        if (d->ids[i] != c[j].id) continue;
        x += c[j].x; y += c[j].y; matched++;
        break;
    }
    if (matched != 3) { drag3_anchor(d, c, scan, ns); return out; }
    int frame_dx = x - d->motion.x, frame_dy = y - d->motion.y;
    pointer_motion_observe(&d->motion, x, y, scan, ns);
    if (d->phase == DRAG3_PENDING) {
        if (!frame_dx && !frame_dy) {
            if (ns - d->last_motion_ns >= 60000000ULL) {
                d->anchor_x = d->last_x = x;
                d->anchor_y = d->last_y = y;
                d->previous_dx = d->previous_dy = d->motion_run = 0;
            }
            return out;
        }
        int continuous = (double)frame_dx * d->previous_dx +
                         (double)frame_dy * d->previous_dy > 0;
        d->motion_run = continuous ? d->motion_run + 1 : 1;
        d->previous_dx = frame_dx; d->previous_dy = frame_dy;
        d->last_motion_ns = ns;
        // Briefly allow the fourth finger to land. Stationary three-finger
        // touches, isolated landing jumps and sub-0.6mm settling cannot click.
        if (ns - d->began_ns < 35000000ULL || d->motion_run < 2 ||
            (abs(x - d->anchor_x) <= 18 && abs(y - d->anchor_y) <= 18)) return out;
        d->phase = DRAG3_ACTIVE;
        out.begin = 1;
    }
    // Coordinates are sums rather than rounded centroids, preserving thirds
    // of a sensor count. Normalize speed and gain back to one-finger units.
    double scaled_gain = pointer_motion_gain(d->motion.speed / 3, gain, acceleration) / 3;
    pointer_motion_apply(&d->motion, x - d->last_x, y - d->last_y,
                         scaled_gain, 0, &out.dx, &out.dy);
    d->last_x = x; d->last_y = y;
    return out;
}

#endif
