#ifndef TPKBD_POINTER_MOTION_H
#define TPKBD_POINTER_MOTION_H

#include <math.h>
#include <stdint.h>

// Velocity changes gain, never delays or predicts pointer movement. Pixel
// remainders preserve subpixel travel across reports, independently per axis.
typedef struct {
    int x, y;
    uint16_t scan;
    uint64_t ns;
    double speed, remainder_x, remainder_y;
    int have_speed;
} PointerMotion;

static inline void pointer_motion_anchor(PointerMotion *p, int x, int y,
                                         uint16_t scan, uint64_t ns) {
    *p = (PointerMotion){.x = x, .y = y, .scan = scan, .ns = ns};
}

static inline void pointer_motion_observe(PointerMotion *p, int x, int y,
                                          uint16_t scan, uint64_t received_ns) {
    uint16_t ticks = (uint16_t)(scan - p->scan);
    uint64_t ns = p->ns + (uint64_t)ticks * 100000;
    uint64_t error = ns > received_ns ? ns - received_ns : received_ns - ns;
    if (!ticks || ticks > 1000 || error > 20000000) ns = received_ns;
    if (ns <= p->ns) return; // Preserve movement until a valid time interval arrives.
    double dt = (double)(ns - p->ns) / 1e9;
    double speed = hypot((double)x - p->x, (double)y - p->y) / dt;
    if (dt > 0.1) {
        p->speed = 0;
        p->have_speed = 0;
        p->remainder_x = p->remainder_y = 0;
    }
    if (!p->have_speed) { p->speed = speed; p->have_speed = 1; }
    else {
        // Reduce gain promptly when slowing to acquire a small target; smooth
        // acceleration more gently so individual sensor reports cannot jerk it.
        double tau = speed < p->speed ? 0.006 : 0.012;
        p->speed += -expm1(-dt / tau) * (speed - p->speed);
    }
    p->x = x; p->y = y; p->scan = scan; p->ns = ns;
}

static inline double pointer_motion_gain(double speed, double base, int acceleration) {
    if (!acceleration) return base;
    // About 14..136 mm/s for this pad (~11 counts/mm). Smooth endpoints make
    // crossing either threshold continuous. This is a custom curve, not Apple's.
    double t = fmax(0, fmin(1, (speed - 150.0) / 1350.0));
    double smooth = t * t * (3.0 - 2.0 * t);
    return base * (0.5 + 1.5 * smooth);
}

static inline void pointer_motion_apply(PointerMotion *p, int dx, int dy,
                                        double base, int acceleration, int *px, int *py) {
    *px = *py = 0;
    if (!dx && !dy) return; // A stationary finger must never release old fractions.
    double gain = pointer_motion_gain(p->speed, base, acceleration);
    if (dx) {
        double x = p->remainder_x + dx * gain;
        *px = (int)lround(x);
        p->remainder_x = x - *px;
    }
    if (dy) {
        double y = p->remainder_y + dy * gain;
        *py = (int)lround(y);
        p->remainder_y = y - *py;
    }
}

#endif
