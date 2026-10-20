// Run via tests/run.sh. Events are inspected in memory; nothing is posted to macOS.
#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>
#include <assert.h>
#include <stdio.h>

static double test_time_ms = 1000;
static uint64_t test_mach_time(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (uint64_t)(test_time_ms * 1e6 * tb.denom / tb.numer);
}
typedef struct { int phase, momentum; int64_t x, y; double fixed_x, fixed_y; } Sample;
static Sample samples[4096];
static int sample_count;
typedef struct { CGEventType type; CGPoint point; int count; } MouseSample;
static MouseSample mouse_samples[4096];
static int mouse_count;
static CGPoint test_cursor = {500, 500};
static CGPoint test_event_location(CGEventRef event) {
    return CGEventGetType(event) == kCGEventNull ? test_cursor : CGEventGetLocation(event);
}
static void capture_event(CGEventTapLocation tap, CGEventRef event) {
    (void)tap;
    CGEventType type = CGEventGetType(event);
    if (type == kCGEventMouseMoved || type == kCGEventLeftMouseDown ||
        type == kCGEventLeftMouseUp || type == kCGEventLeftMouseDragged ||
        type == kCGEventRightMouseDown || type == kCGEventRightMouseUp ||
        type == kCGEventRightMouseDragged) {
        assert(mouse_count < 4096);
        test_cursor = CGEventGetLocation(event);
        mouse_samples[mouse_count++] = (MouseSample){type, test_cursor,
            (int)CGEventGetIntegerValueField(event, kCGMouseEventClickState)};
    }
    if (type != kCGEventScrollWheel) return;
    assert(sample_count < 4096);
    samples[sample_count++] = (Sample){
        (int)CGEventGetIntegerValueField(event, kCGScrollWheelEventScrollPhase),
        (int)CGEventGetIntegerValueField(event, kCGScrollWheelEventMomentumPhase),
        CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis2),
        CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis1),
        CGEventGetDoubleValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2),
        CGEventGetDoubleValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1)
    };
}
#define CGEventPost capture_event
#define CGEventGetLocation test_event_location
#define mach_absolute_time test_mach_time
#define main driver_main
#ifndef TPKBD_SOURCE
#define TPKBD_SOURCE "../tpkbd_touchpad.c"
#endif
#include TPKBD_SOURCE
#undef main

static void reset(void) {
    touch_resampling = 0;
    scroll_smooth_ms = 0;
    mom_stop();
    sample_count = mouse_count = 0;
    test_cursor = CGPointMake(500, 500);
    screen_bounds = CGRectMake(0, 0, 2000, 1500);
    g_display_count = 1; g_display_bounds[0] = screen_bounds;
    move_gain = 1.8f; move_acceleration = 1; move_slop = 14; drag_slop = 12;
    pointer_motion = (PointerMotion){0}; pointer_contact_id = -1;
    click_count = 0; last_down_ms = 0; release_streak = 0; drag_armed = 0;
    touch_now = type_lock_on = type_grandf = 0; last_key_ms = 0;
    test_time_ms += 1000;
    gesture_state = scroll_active = have_last = btn_prev = 0;
    scroll_lift_ms = 0;
    tap_active = tap_nfid = tap_moved = 0;
    memset(tip_state, 0, sizeof(tip_state));
    memset(down_seq, 0, sizeof(down_seq));
    disable_rel = 1; tap_on = 0; typelock_ms = 0; swipe3_on = 0;
    natural = 1; scroll_gain = 2.0f; momentum_on = 1;
}

static void report(int fingers, int y0, int y1) {
    uint8_t r[30] = {3};
    for (int i = 0; i < fingers; i++) {
        int x = 200 + i * 100, y = i ? y1 : y0;
        r[1 + i * 5] = 3 | (i << 2);
        r[2 + i * 5] = x & 255; r[3 + i * 5] = x >> 8;
        r[4 + i * 5] = y & 255; r[5 + i * 5] = y >> 8;
    }
    r[28] = fingers;
    test_time_ms += 10;
    handle_report(3, r, sizeof(r));
}

static void slow_scroll(void) {
    reset();
    report(2, 200, 200);
    for (int i = 1; i <= 20; i++) report(2, 200 + i, 200);
    // Half-count centroid steps * gain 2 = one pixel EACH report, not 0/2 jumps.
    assert(sample_count == 21);
    for (int i = 1; i < sample_count; i++) {
        assert(samples[i].y == 1);
        assert(fabs(samples[i].fixed_y - 0.1) < 1.0 / 65536);
        assert(samples[i].phase == (i == 1 ? PH_BEGAN : PH_CHANGED));
    }
    momentum_on = 0;
    report(0, 0, 0);
    assert(samples[sample_count - 2].phase == PH_ENDED);
    assert(samples[sample_count - 1].momentum == MPH_END);
}

static void distance_and_direction(void) {
    reset();
    natural = 0;
    gesture_send_maybegin();
    for (int i = 0; i < 100; i++) post_gesture_scroll(0.25, -0.125);
    int64_t x = 0, y = 0;
    double fx = 0, fy = 0;
    for (int i = 0; i < sample_count; i++) {
        x += samples[i].x; y += samples[i].y;
        fx += samples[i].fixed_x; fy += samples[i].fixed_y;
    }
    assert(x == -25 && y == 12);
    assert(fabs(fx - x * 0.1) < 0.001);
    assert(fabs(fy - y * 0.1) < 0.001);
}

static double velocity_at_interval(double dt) {
    scroll_velocity_reset();
    for (double t = 0; t < 140 - 0.001; t += dt) {
        test_time_ms += dt;
        scroll_velocity_update(0, dt * 0.5, test_time_ms);
    }
    return scrl_ema_y;
}

static void velocity(void) {
    reset();
    double a = velocity_at_interval(5), b = velocity_at_interval(10);
    double c = velocity_at_interval(20);
    assert(fabs(a - b) < 1e-6 && fabs(b - c) < 1e-6);
    assert(a > 0.49 && a < 0.5);
    // A brief reversal must not launch inertia in the previous direction.
    test_time_ms += 10;
    scroll_velocity_update(0, -5, test_time_ms);
    assert(scrl_ema_y < 0);
    for (int i = 0; i < 20; i++) {
        test_time_ms += 10;
        scroll_velocity_update(0, 0, test_time_ms);
    }
    assert(fabs(scrl_ema_y) < 0.001);
    // A pause without stationary reports must also suppress stale momentum.
    gesture_state = 2;
    scrl_ema_y = 0.5;
    last_scrl_ms = test_time_ms;
    test_time_ms += 120;
    end_scroll_gesture();
    assert(!mom_active);
}

static void momentum_phases(void) {
    reset();
    gesture_send_maybegin();
    post_gesture_scroll(0, 1);
    send_scroll_event(0, 0, 0, 0, PH_ENDED, MPH_NONE, 1);
    mom_start(0, 1, 61);
    int before = sample_count;
    test_time_ms += 1;
    mom_timer_cb(NULL, NULL); // Too little displacement for a whole pixel.
    assert(sample_count == before && !mom_begun);
    test_time_ms += 20;
    mom_timer_cb(NULL, NULL);
    assert(sample_count > before);
    assert(samples[before].momentum == MPH_BEGIN);
    for (int i = 0; i < 200 && mom_active; i++) {
        test_time_ms += 10;
        mom_timer_cb(NULL, NULL);
    }
    assert(!mom_active);
    assert(samples[sample_count - 1].momentum == MPH_END);
    double total = 0;
    for (int i = before; i < sample_count; i++) {
        total += samples[i].y;
        assert(samples[i].phase == PH_UNDEFINED);
        if (i > before && i < sample_count - 1)
            assert(samples[i].momentum == MPH_CONTINUE);
    }
    assert(fabs(total - mom_total_d) < 1.0);
}

static void staggered_lift(void) {
    reset();
    report(2, 200, 200);
    for (int i = 1; i <= 10; i++) report(2, 200 + i * 5, 200 + i * 5);
    report(1, 250, 0);
    assert(!mom_active && scroll_active);
    report(1, 250, 0);
    assert(!mom_active && scroll_active);
    report(0, 0, 0);
    assert(mom_active && !scroll_active);
    mom_stop();

    // Keeping one finger down is a switch to pointer movement, not a flick.
    reset();
    report(2, 200, 200);
    for (int i = 1; i <= 10; i++) report(2, 200 + i * 5, 200 + i * 5);
    for (int i = 0; i < 8; i++) report(1, 250, 0);
    assert(!mom_active && !scroll_active);
    report(0, 0, 0);
    assert(!mom_active);

    // The same applies if the firmware sends no stationary reports.
    reset();
    report(2, 200, 200);
    for (int i = 1; i <= 10; i++) report(2, 200 + i * 5, 200 + i * 5);
    report(1, 250, 0);
    test_time_ms += 80;
    report(0, 0, 0);
    assert(!mom_active && !scroll_active);
}

static void resampler_cadence(void) {
    // A constant-speed 140Hz device arriving with USB jitter must yield uniform
    // motion at 60/75/120Hz, rather than alternating 1/2/3 input deltas per frame.
    const int rates[] = {60, 75, 120};
    for (int k = 0; k < 3; k++) {
        ScrollResampler r;
        scroll_resampler_begin(&r, 1000000000);
        int input = 1;
        double previous_output = 0;
        for (int frame = 1; frame <= rates[k] / 2; frame++) {
            double t = frame * 1000.0 / rates[k];
            while (input <= 70) {
                double sample = input * (1000.0 / 140);
                double arrival = sample + (input % 3) * 1.5;
                if (arrival > t) break;
                scroll_resampler_push(&r, 1000000000 + (uint64_t)(sample * 1e6),
                                      0, 1200.0 / 140);
                input++;
            }
            double dx, dy;
            scroll_resampler_read(&r, (uint64_t)((1000 + t - 12) * 1e6), &dx, &dy);
            if (t > 35 && t < 450) assert(fabs(dy - 1200.0 / rates[k]) < 0.00001);
            assert(dx == 0 && dy >= 0);
            previous_output += dy;
        }
        double dx, dy;
        scroll_resampler_read(&r, UINT64_MAX, &dx, &dy);
        assert(fabs(previous_output + dy - (input - 1) * 1200.0 / 140) < 0.00001);
        scroll_resampler_read(&r, UINT64_MAX, &dx, &dy);
        assert(dx == 0 && dy == 0); // A stationary finger cannot drift.
    }

    ScrollResampler r;
    scroll_resampler_begin(&r, 0);
    // Duplicate timestamps, direction changes, and a stalled consumer/overflow.
    scroll_resampler_push(&r, 10, 3, 4);
    scroll_resampler_push(&r, 10, -1, -2);
    for (uint64_t i = 11; i < 1000; i++) scroll_resampler_push(&r, i, -0.5, 0.25);
    double dx, dy;
    scroll_resampler_read(&r, UINT64_MAX, &dx, &dy);
    assert(fabs(dx - (2 - 989 * 0.5)) < 1e-8);
    assert(fabs(dy - (2 + 989 * 0.25)) < 1e-8);
}

static void resampled_gesture(void) {
    reset();
    scroll_smooth_ms = 12;
    report(2, 200, 200);
    for (int i = 1; i <= 10; i++) {
        report(2, 200 + i * 5, 200 + i * 5);
        touch_scroll_tick(mach_ns());
    }
    assert(touch_resampling && gesture_state == 2);
    momentum_on = 0;
    report(0, 0, 0);
    assert(!touch_resampling && !mom_active);
    int64_t total = 0;
    for (int i = 0; i < sample_count; i++) total += samples[i].y;
    assert(total == 100); // Includes the buffered final movement exactly once.
    assert(samples[sample_count - 2].phase == PH_ENDED);
    int count = sample_count;
    touch_scroll_tick(mach_ns() + 100000000);
    assert(sample_count == count); // No Changed can escape after Ended.

    // Hardware scan time wraps independently of host callback jitter.
    touch_scroll_begin(1000000000, 65500);
    assert(touch_scroll_time(1008000000, 35) == 1007100000);
    assert(touch_scroll_time(1012000000, 107) == 1014300000);
    // Missing/reset scan clock recovers to receipt time rather than jumping far.
    assert(touch_scroll_time(2000000000, 107) == 2000000000);
    touch_scroll_finish();
    reset();
}

static void pointer_report(int tips, int x, int y, int button, int dt) {
    uint8_t r[30] = {3};
    r[1] = tips ? 3 : 0;
    r[2] = x & 255; r[3] = x >> 8;
    r[4] = y & 255; r[5] = y >> 8;
    r[28] = tips; r[29] = button;
    test_time_ms += dt;
    handle_report(3, r, sizeof(r));
}

static void assert_clean_tap(int expected_count) {
    assert(test_cursor.x == 500 && test_cursor.y == 500);
    assert(mouse_count >= 2);
    for (int i = mouse_count - 2; i < mouse_count; i++) {
        assert(mouse_samples[i].point.x == 500 && mouse_samples[i].point.y == 500);
        assert(mouse_samples[i].count == expected_count);
    }
    assert(mouse_samples[mouse_count - 2].type == kCGEventLeftMouseDown);
    assert(mouse_samples[mouse_count - 1].type == kCGEventLeftMouseUp);
}

static void tap_no_drift(void) {
    // Boundary and diagonal drift, both within and beyond the early snap window.
    const int offsets[][2] = {{7,7}, {8,8}, {0,14}, {14,0}, {-14,-14}, {4,14}};
    for (int late = 0; late < 2; late++) for (int k = 0; k < 6; k++) {
        reset(); tap_on = 1;
        pointer_report(1, 200, 200, 0, 7);
        if (late) for (int i = 0; i < 12; i++) pointer_report(1, 200, 200, 0, 7);
        pointer_report(1, 200 + offsets[k][0], 200 + offsets[k][1], 0, 7);
        pointer_report(0, 200, 200, 0, 7);
        assert_clean_tap(1);
        assert(mouse_count == 2);
    }
    // A measured-size landing shift (18 counts) is still swallowed once.
    reset(); tap_on = 1;
    pointer_report(1, 200, 200, 0, 7);
    pointer_report(1, 200, 218, 0, 7);
    pointer_report(1, 202, 229, 0, 7);
    pointer_report(0, 202, 229, 0, 7);
    assert_clean_tap(1); assert(mouse_count == 2);

    // Double/triple taps keep click count and never shift their location.
    reset(); tap_on = 1;
    for (int i = 1; i <= 3; i++) {
        pointer_report(1, 200, 200, 0, 70);
        pointer_report(1, 204, 214, 0, 7);
        pointer_report(0, 204, 214, 0, 7);
        assert_clean_tap(i);
        assert(mouse_count == i * 2);
    }

    // Actual pointer motion must cancel tapping, and later landing filtering
    // must not clear that decision. Move slop 0 also disables the snap filter.
    reset(); tap_on = 1; move_slop = 0;
    pointer_report(1, 200, 200, 0, 7);
    pointer_report(1, 200, 210, 0, 7);
    pointer_report(0, 200, 210, 0, 7);
    assert(test_cursor.y > 500);
    assert(mouse_count == 1 && mouse_samples[0].type == kCGEventMouseMoved);

    reset(); tap_on = 1;
    pointer_report(1, 200, 200, 0, 7);
    for (int i = 1; i <= 8; i++) pointer_report(1, 200, 200 + i * 10, 0, 7);
    pointer_report(0, 200, 280, 0, 7);
    assert(test_cursor.y > 500);
    for (int i = 0; i < mouse_count; i++) assert(mouse_samples[i].type == kCGEventMouseMoved);

    // Press-and-drag still emits Dragged, not Moved, after the press dead zone.
    reset(); tap_on = 1;
    pointer_report(1, 200, 200, 0, 7);
    pointer_report(1, 200, 200, 1, 7);
    pointer_report(1, 200, 205, 1, 7);
    pointer_report(1, 200, 225, 1, 7);
    assert(mouse_samples[0].type == kCGEventLeftMouseDown);
    assert(mouse_samples[mouse_count - 1].type == kCGEventLeftMouseDragged);
    assert(test_cursor.y > 500);
}

static void pointer_precision_and_acceleration(void) {
    // Fixed-gain comparison: ten one-count reports must move 18 pixels, not ten.
    reset(); move_slop = 0; move_acceleration = 0;
    pointer_report(1, 200, 200, 0, 7);
    for (int i = 1; i <= 10; i++) pointer_report(1, 200 + i, 200, 0, 7);
    assert(test_cursor.x == 518 && test_cursor.y == 500);
    for (int i = 9; i >= 0; i--) pointer_report(1, 200 + i, 200, 0, 7);
    assert(test_cursor.x == 500 && test_cursor.y == 500);

    // Same physical travel: slow reports give precision; fast ones travel farther.
    double travel[2];
    for (int fast = 0; fast < 2; fast++) {
        reset(); move_slop = 0;
        pointer_report(1, 200, 200, 0, 7);
        for (int i = 1; i <= 10; i++) pointer_report(1, 200 + i * 10, 200, 0, fast ? 5 : 100);
        travel[fast] = test_cursor.x - 500;
    }
    assert(travel[0] == 90 && travel[1] == 360);

    PointerMotion p;
    pointer_motion_anchor(&p, 0, 0, 100, 1000000000);
    int px, py, total = 0;
    for (int i = 0; i < 100; i++) {
        pointer_motion_apply(&p, 1, 0, 0.25, 0, &px, &py);
        total += px;
        assert(py == 0);
        assert(fabs(p.remainder_x) <= 0.5);
    }
    assert(total == 25);
    for (int i = 0; i < 100; i++) {
        pointer_motion_apply(&p, -1, 0, 0.25, 0, &px, &py);
        total += px;
    }
    assert(total == 0);
    // A half-pixel remainder must not move a stationary axis, including when
    // the OTHER axis is moving. Neither may stationary reports creep the cursor.
    pointer_motion_apply(&p, 1, 0, 0.5, 0, &px, &py);
    assert(p.remainder_x == -0.5);
    for (int i = 0; i < 10; i++) {
        pointer_motion_apply(&p, 0, 1, 0.5, 0, &px, &py);
        assert(px == 0);
        pointer_motion_apply(&p, 0, 0, 0.5, 0, &px, &py);
        assert(px == 0 && py == 0);
    }

    // Continuous, monotonic, bounded gain without a threshold jump.
    double previous = 0;
    for (int speed = 0; speed <= 5000; speed++) {
        double gain = pointer_motion_gain(speed, 1.8, 1);
        assert(gain >= previous && gain >= 0.9 && gain <= 3.6);
        if (speed) assert(gain - previous < 0.004);
        previous = gain;
    }
    assert(fabs(pointer_motion_gain(825, 1.8, 1) - 2.25) < 1e-10);

    // The same velocity must have the same gain at different report frequencies.
    double speeds[2];
    for (int k = 0; k < 2; k++) {
        pointer_motion_anchor(&p, 0, 0, 0, 1000000000);
        int dt = k ? 10 : 5, step = k ? 8 : 4;
        for (int i = 1; i <= 20; i++)
            pointer_motion_observe(&p, step * i, 0, 0, 1000000000 + (uint64_t)i * dt * 1000000);
        speeds[k] = p.speed;
    }
    assert(fabs(speeds[0] - speeds[1]) < 1e-8 && fabs(speeds[0] - 800) < 1e-8);

    // Scan clock wrap removes USB arrival jitter; slowing promptly lowers gain.
    pointer_motion_anchor(&p, 0, 0, 65500, 1000000000);
    pointer_motion_observe(&p, 14, 0, 35, 1009000000);
    assert(fabs(p.speed - 14 / 0.0071) < 1e-8);
    assert(p.ns == 1007100000);
    for (int i = 1; i <= 3; i++)
        pointer_motion_observe(&p, 14 + i, 0, 35 + i * 71, 1007100000 + (uint64_t)i * 7100000);
    assert(pointer_motion_gain(p.speed, 1.8, 1) < 1.0);
    pointer_motion_observe(&p, 17, 0, 0, 1500000000);
    assert(p.speed == 0 && p.remainder_x == 0 && p.remainder_y == 0);

    // No fraction/acceleration leaks through a new contact, scrolling, or a screen edge.
    reset(); move_slop = 0; move_acceleration = 0; move_gain = 0.5f;
    pointer_report(1, 200, 200, 0, 7);
    pointer_report(1, 201, 200, 0, 7);
    assert(pointer_motion.remainder_x == -0.5);
    pointer_report(0, 201, 200, 0, 7);
    pointer_report(1, 300, 300, 0, 7);
    assert(pointer_motion.remainder_x == 0 && !pointer_motion.have_speed);
    assert(test_cursor.x == 501);
    report(2, 300, 300);
    pointer_report(1, 300, 300, 0, 70);
    pointer_report(1, 300, 300, 0, 70);
    assert(pointer_motion.remainder_x == 0 && !pointer_motion.have_speed);

    reset(); move_slop = 0; move_acceleration = 0;
    test_cursor = CGPointMake(1999, 500);
    pointer_report(1, 200, 200, 0, 7);
    pointer_report(1, 201, 200, 0, 7);
    assert(test_cursor.x == 1999 && pointer_motion.remainder_x == 0);
    pointer_report(1, 200, 200, 0, 7);
    assert(test_cursor.x == 1997);
}

int main(void) {
    pointer_precision_and_acceleration();
    tap_no_drift();
    slow_scroll();
    distance_and_direction();
    velocity();
    momentum_phases();
    staggered_lift();
    resampler_cadence();
    resampled_gesture();
    puts("PASS: pointer fractions/velocity curve/cadence/reversal/edges, tap boundaries/no drift, multi-click, movement cancellation/drag, slow scroll precision, fractional fields, distance/direction, time-based velocity, pause/reversal, momentum phases/distance, staggered lift, 60/75/120Hz resampling, USB jitter, scan clock wrap, flush/no drift");
    return 0;
}
