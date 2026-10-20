#ifndef TPKBD_TWO_FINGER_GESTURE_H
#define TPKBD_TWO_FINGER_GESTURE_H
#include <math.h>
#include <stdint.h>

enum { TWO_NONE, TWO_PENDING, TWO_SCROLL, TWO_PINCH };
typedef struct {
    int mode, id1, id2, radial_run;
    double cx, cy, distance, previous_distance, previous_radial;
    uint64_t began_ns;
} TwoFingerGesture;

static inline void two_finger_begin(TwoFingerGesture *g, int id1, int id2,
                                    double cx, double cy, double distance, uint64_t ns) {
    if (id1 > id2) { int tmp = id1; id1 = id2; id2 = tmp; }
    *g = (TwoFingerGesture){.mode = TWO_PENDING, .id1 = id1, .id2 = id2,
                           .cx = cx, .cy = cy, .distance = distance,
                           .previous_distance = distance, .began_ns = ns};
}

static inline int two_finger_same_contacts(const TwoFingerGesture *g, int id1, int id2) {
    if (id1 > id2) { int tmp = id1; id1 = id2; id2 = tmp; }
    return g->mode != TWO_NONE && g->id1 == id1 && g->id2 == id2;
}

static inline int two_finger_classify(TwoFingerGesture *g, double cx, double cy,
                                      double distance, int pinch_enabled) {
    if (g->mode != TWO_PENDING) return g->mode;
    double radial = distance - g->previous_distance;
    if (fabs(radial) >= 0.5) {
        // A large isolated landing jump after tiny jitter is not a continuous pinch.
        int continuous = radial * g->previous_radial > 0 &&
                         fabs(radial) <= fmax(8, fabs(g->previous_radial) * 4);
        g->radial_run = continuous ? g->radial_run + 1 : 1;
        g->previous_radial = radial;
    } else g->radial_run = 0;
    g->previous_distance = distance;
    double translation = hypot(cx - g->cx, cy - g->cy);
    double separation = fabs(distance - g->distance);
    int can_pinch = pinch_enabled && g->distance >= 60 && distance >= 40;
    if (can_pinch && separation >= fmax(8, g->distance * 0.03) &&
        separation > translation * 1.5 && g->radial_run >= 2)
        g->mode = TWO_PINCH;
    else if (!pinch_enabled || (translation >= 2 &&
             (!can_pinch || separation <= translation * 1.5)))
        g->mode = TWO_SCROLL;
    return g->mode;
}
#endif
