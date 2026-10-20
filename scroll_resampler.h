#ifndef TPKBD_SCROLL_RESAMPLER_H
#define TPKBD_SCROLL_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

// Cumulative positions allow interpolation without losing fractional distance.
// Callers sample slightly behind input time, never extrapolate past received data.
#define SCROLL_SAMPLE_CAPACITY 128
typedef struct { uint64_t ns; double x, y; } ScrollSample;
typedef struct {
    ScrollSample samples[SCROLL_SAMPLE_CAPACITY];
    size_t first, count;
    double x, y, sent_x, sent_y;
} ScrollResampler;

static inline ScrollSample *scroll_sample_at(ScrollResampler *s, size_t i) {
    return &s->samples[(s->first + i) % SCROLL_SAMPLE_CAPACITY];
}

static inline void scroll_resampler_begin(ScrollResampler *s, uint64_t ns) {
    *s = (ScrollResampler){0};
    s->samples[0] = (ScrollSample){ns, 0, 0};
    s->count = 1;
}

static inline void scroll_resampler_push(ScrollResampler *s, uint64_t ns, double dx, double dy) {
    s->x += dx; s->y += dy;
    ScrollSample *last = scroll_sample_at(s, s->count - 1);
    if (ns <= last->ns) { // Batched/duplicate timestamps: merge, don't divide by zero.
        last->x = s->x; last->y = s->y;
        return;
    }
    if (s->count == SCROLL_SAMPLE_CAPACITY) {
        s->first = (s->first + 1) % SCROLL_SAMPLE_CAPACITY;
        s->count--;
    }
    *scroll_sample_at(s, s->count++) = (ScrollSample){ns, s->x, s->y};
}

static inline void scroll_resampler_read(ScrollResampler *s, uint64_t ns, double *dx, double *dy) {
    while (s->count > 1 && scroll_sample_at(s, 1)->ns <= ns) {
        s->first = (s->first + 1) % SCROLL_SAMPLE_CAPACITY;
        s->count--;
    }
    ScrollSample *a = scroll_sample_at(s, 0);
    double x = a->x, y = a->y;
    if (s->count > 1 && ns > a->ns) {
        ScrollSample *b = scroll_sample_at(s, 1);
        double f = (double)(ns - a->ns) / (double)(b->ns - a->ns);
        x += f * (b->x - a->x); y += f * (b->y - a->y);
    }
    *dx = x - s->sent_x; *dy = y - s->sent_y;
    s->sent_x = x; s->sent_y = y;
}

#endif
