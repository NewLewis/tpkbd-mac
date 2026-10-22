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
typedef struct { int phase; double delta; CGPoint point; } PinchSample;
static PinchSample pinch_samples[4096];
static int pinch_count;
static CGPoint test_cursor = {500, 500};
static int stale_cursor_reads;
static CGPoint test_event_location(CGEventRef event) {
    return CGEventGetType(event) == kCGEventNull ?
        (stale_cursor_reads ? CGPointMake(500, 500) : test_cursor) : CGEventGetLocation(event);
}
static void capture_event(CGEventTapLocation tap, CGEventRef event) {
    (void)tap;
    CGEventType type = CGEventGetType(event);
    if (type == 29 && CGEventGetIntegerValueField(event, 110) == 8) {
        assert(tap == kCGHIDEventTap);
        assert(pinch_count < 4096);
        pinch_samples[pinch_count++] = (PinchSample){
            (int)CGEventGetIntegerValueField(event, 132),
            CGEventGetDoubleValueField(event, 113), CGEventGetLocation(event)};
    }
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
    pinch_active = pinch_sent = pinch_tail = 0;
    pinch_on = 0; pinch_gain = 1;
    drag3_on = 0; three_drag = (ThreeFingerDrag){0};
    three_drag_cursor = CGPointZero; stale_cursor_reads = 0;
    system_swipe_tail = 0;
    swipe3_active = swipe3_sent = swipe3_axis = 0;
    swipe3_resend_cancel();
    two_finger = (TwoFingerGesture){0};
    scroll_smooth_ms = 0;
    mom_stop();
    sample_count = mouse_count = pinch_count = 0;
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
    tap_right_on = 1; two_finger_tap = (TwoFingerTap){0};
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

static void pair_report(int x1, int y1, int x2, int y2, int mask, int button, int id2) {
    uint8_t r[30] = {3};
    int xs[] = {x1, x2}, ys[] = {y1, y2};
    for (int i = 0; i < 2; i++) {
        r[1 + i * 5] = ((mask & (1 << i)) ? 3 : 0) | ((i ? id2 : 0) << 2);
        r[2 + i * 5] = xs[i] & 255; r[3 + i * 5] = xs[i] >> 8;
        r[4 + i * 5] = ys[i] & 255; r[5 + i * 5] = ys[i] >> 8;
    }
    r[28] = !!(mask & 1) + !!(mask & 2); r[29] = button;
    test_time_ms += 7;
    handle_report(3, r, sizeof(r));
}

static double pinch_scale(void) {
    double scale = 1;
    assert(pinch_count >= 3 && pinch_samples[0].phase == PH_BEGAN);
    for (int i = 0; i < pinch_count; i++) {
        assert(isfinite(pinch_samples[i].delta));
        assert(pinch_samples[i].point.x == 500 && pinch_samples[i].point.y == 500);
        scale *= 1 + pinch_samples[i].delta;
        if (i > 0 && i < pinch_count - 1) assert(pinch_samples[i].phase == PH_CHANGED);
    }
    assert(pinch_samples[pinch_count - 1].phase == PH_ENDED);
    return scale;
}

static void pinch_gestures(void) {
    // Outward, inward and reversed gestures preserve the distance ratio.
    for (int reverse = 0; reverse < 3; reverse++) {
        reset(); pinch_on = 1; natural = 0;
        pair_report(400, 400, 600, 400, 3, 0, 1);
        for (int i = 1; i <= 10; i++) {
            int d = reverse == 1 ? -i * 5 : i * 5;
            pair_report(400 - d, 400, 600 + d, 400, 3, 0, 1);
        }
        if (reverse == 2) for (int i = 9; i >= 0; i--)
            pair_report(400 - i * 5, 400, 600 + i * 5, 400, 3, 0, 1);
        pair_report(400, 400, 600, 400, 0, 0, 1);
        assert(fabs(pinch_scale() - (reverse == 0 ? 1.5 : reverse == 1 ? 0.5 : 1)) < 1e-5);
        assert(reverse == 1 ? pinch_samples[0].delta < 0 : pinch_samples[0].delta > 0);
        assert(!mom_active && mouse_count == 0 && !pinch_active);
        for (int i = 0; i < sample_count; i++)
            assert(samples[i].x == 0 && samples[i].y == 0); // No accidental scrolling.
    }
    // One finger can stay still; common translation must not steal that pinch.
    reset(); pinch_on = 1;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    for (int i = 1; i <= 10; i++) pair_report(400, 400, 600 + i * 4, 400, 3, 0, 1);
    pair_report(400, 400, 640, 400, 0, 0, 1);
    assert(fabs(pinch_scale() - 1.2) < 1e-5);

    // Both fingers translating remains the existing scroll path, including
    // after later distance changes. The choice is locked until contact changes.
    reset(); pinch_on = 1; momentum_on = 0;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    for (int i = 1; i <= 10; i++) pair_report(400, 400 + i * 5, 600, 400 + i * 5, 3, 0, 1);
    pair_report(380, 450, 620, 450, 3, 0, 1);
    pair_report(380, 450, 620, 450, 0, 0, 1);
    int distance = 0;
    for (int i = 0; i < sample_count; i++) distance += samples[i].y;
    assert(pinch_count == 0 && distance == 100);

    // The display-paced scroll path still preserves full distance with pinch on.
    reset(); pinch_on = 1; momentum_on = 0; scroll_smooth_ms = 12;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    for (int i = 1; i <= 10; i++) {
        pair_report(400, 400 + i * 5, 600, 400 + i * 5, 3, 0, 1);
        touch_scroll_tick(mach_ns());
    }
    pair_report(400, 450, 600, 450, 0, 0, 1);
    distance = 0;
    for (int i = 0; i < sample_count; i++) distance += samples[i].y;
    assert(pinch_count == 0 && distance == 100 && !touch_resampling);

    // Tiny relative jitter and an isolated landing jump must never magnify.
    reset(); pinch_on = 1;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    for (int i = 0; i < 10; i++) pair_report(400 - i % 2, 400, 600 + i % 2, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 3, 0, 1);
    for (int i = 0; i < 5; i++) pair_report(390, 400, 610, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 0, 0, 1);
    assert(pinch_count == 0);

    // Release one finger: complete zoom once and swallow its residual movement.
    reset(); pinch_on = 1; scroll_smooth_ms = 12;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    for (int i = 1; i <= 10; i++) {
        pair_report(400 - i * 5, 400, 600 + i * 5, 400, 3, 0, 1);
        pinch_tick(mach_ns());
    }
    pair_report(350, 400, 650, 400, 1, 0, 1);
    assert(fabs(pinch_scale() - 1.5) < 1e-5);
    int count = pinch_count;
    pair_report(350, 420, 650, 400, 1, 0, 1);
    pinch_tick(mach_ns() + 100000000);
    assert(mouse_count == 0 && pinch_count == count);
    pair_report(350, 420, 650, 400, 0, 0, 1);
    assert(!pinch_tail);

    // A short buffered pinch can end before its first display tick. Its first
    // event still carries the actual direction, and flushing preserves scale.
    for (int inward = 0; inward < 2; inward++) {
        reset(); pinch_on = 1; scroll_smooth_ms = 12;
        int sign = inward ? -1 : 1;
        pair_report(400, 400, 600, 400, 3, 0, 1);
        pair_report(400 - sign * 5, 400, 600 + sign * 5, 400, 3, 0, 1);
        pair_report(400 - sign * 10, 400, 600 + sign * 10, 400, 3, 0, 1);
        assert(pinch_active && !pinch_sent && pinch_count == 0);
        pair_report(400 - sign * 10, 400, 600 + sign * 10, 400, 0, 0, 1);
        assert(pinch_count == 2 && pinch_samples[0].phase == PH_BEGAN);
        assert(pinch_samples[1].phase == PH_ENDED);
        assert(fabs(pinch_samples[0].delta - sign * 0.1) < 1e-5);
        assert(!pinch_active && !pinch_sent);
    }

    // A replacement contact closes the old gesture rather than zooming by the
    // unrelated distance. Pressing during zoom also sends a proper cancellation.
    reset(); pinch_on = 1;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    pair_report(395, 400, 605, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 3, 0, 1);
    assert(pinch_active);
    pair_report(200, 400, 1000, 400, 3, 0, 2);
    assert(!pinch_active && pinch_samples[pinch_count - 1].phase == PH_CANCELLED);

    reset(); pinch_on = 1;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    pair_report(395, 400, 605, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 3, 1, 1);
    assert(!pinch_active && pinch_samples[pinch_count - 1].phase == PH_CANCELLED);

    reset(); pinch_on = 1;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    pair_report(395, 400, 605, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 3, 0, 1);
    report(3, 400, 400);
    assert(!pinch_active && pinch_samples[pinch_count - 1].phase == PH_CANCELLED);

    // User's posture: index holds left button, middle finger moves to drag.
    reset(); pinch_on = 1;
    pointer_report(1, 400, 400, 0, 7);
    pointer_report(1, 400, 400, 1, 7);
    pair_report(400, 400, 600, 400, 3, 1, 1);
    pair_report(400, 400, 650, 420, 3, 1, 1);
    assert(pinch_count == 0 && mouse_samples[0].type == kCGEventLeftMouseDown);
    assert(mouse_samples[mouse_count - 1].type == kCGEventLeftMouseDragged);

    reset(); pinch_on = 1;
    pair_report(400, 400, 600, 400, 3, 0, 1);
    pair_report(395, 400, 605, 400, 3, 0, 1);
    pair_report(390, 400, 610, 400, 3, 0, 1);
    device_detached(NULL, 0, NULL, NULL);
    assert(!pinch_active && pinch_samples[pinch_count - 1].phase == PH_CANCELLED);
}

static void contacts_report(const DragContact *contacts, int count, int click) {
    uint8_t r[30] = {3};
    for (int i = 0; i < count; i++) {
        r[1 + i * 5] = 3 | (contacts[i].id << 2);
        r[2 + i * 5] = contacts[i].x & 255; r[3 + i * 5] = contacts[i].x >> 8;
        r[4 + i * 5] = contacts[i].y & 255; r[5 + i * 5] = contacts[i].y >> 8;
    }
    test_time_ms += 10;
    uint16_t scan = (uint16_t)(uint64_t)(test_time_ms * 10);
    r[26] = scan & 255; r[27] = scan >> 8;
    r[28] = count; r[29] = click;
    handle_report(3, r, sizeof(r));
}

static void group_report(int count, int dx, int dy, int click) {
    DragContact contacts[NFINGER];
    for (int i = 0; i < count; i++) contacts[i] = (DragContact){i, 300 + i * 100 + dx, 400 + dy};
    contacts_report(contacts, count, click);
}

static void start_drag(void) {
    group_report(3, 0, 0, 0);
    for (int i = 1; i <= 6; i++) group_report(3, i * 4, 0, 0);
    assert(three_drag.phase == DRAG3_ACTIVE && !btn_prev && !swipe3_active);
    assert(mouse_samples[0].type == kCGEventLeftMouseDown && mouse_samples[0].count == 1);
    for (int i = 1; i < mouse_count; i++) assert(mouse_samples[i].type == kCGEventLeftMouseDragged);
}

static void three_finger_dragging(void) {
    // The same Down -> Dragged -> Up stream selects text and drags files or
    // windows. No physical button is needed; a preceding click is not doubled.
    reset(); drag3_on = swipe3_on = 1; move_gain = 1; move_acceleration = 0;
    click_count = 3; last_down_ms = now_ms();
    start_drag();
    assert(test_cursor.x == 524 && test_cursor.y == 500);
    group_report(3, 30, 12, 0);
    assert(test_cursor.x == 530 && test_cursor.y == 512);
    group_report(2, 90, 60, 0);
    assert(mouse_samples[mouse_count - 1].type == kCGEventLeftMouseUp);
    assert(test_cursor.x == 530 && test_cursor.y == 512);
    int count = mouse_count;
    group_report(1, 150, 80, 0);
    group_report(2, 150, 80, 0);
    group_report(3, 150, 80, 0);
    group_report(0, 0, 0, 0);
    assert(mouse_count == count && three_drag.phase == DRAG3_IDLE);
    assert(pinch_count == 0 && !scroll_active && !mom_active);
    for (int i = 0; i < sample_count; i++) assert(!samples[i].x && !samples[i].y);

    reset(); drag3_on = 1; move_gain = 1; move_acceleration = 0; stale_cursor_reads = 1;
    start_drag();
    assert(test_cursor.x == 524);
    group_report(0, 0, 0, 0);
    assert(mouse_samples[mouse_count - 1].point.x == 524);

    // A stationary three-finger touch, including staggered lift, cannot click.
    reset(); drag3_on = swipe3_on = tap_on = 1;
    for (int i = 0; i < 20; i++) group_report(3, i % 2, i % 2, 0);
    group_report(2, 10, 0, 0); group_report(1, 30, 0, 0); group_report(0, 0, 0, 0);
    assert(mouse_count == 0 && pinch_count == 0 && !swipe3_sent);

    // A settling jump followed by rest cannot press the mouse; after settling,
    // real travel starts from the new contact positions without replaying it.
    reset(); drag3_on = 1; move_gain = 1; move_acceleration = 0;
    group_report(3, 0, 0, 0); group_report(3, 18, 12, 0);
    for (int i = 0; i < 8; i++) group_report(3, 18, 12, 0);
    assert(mouse_count == 0);
    for (int i = 1; i <= 6; i++) group_report(3, 18 + i * 4, 12, 0);
    assert(test_cursor.x == 524 && test_cursor.y == 500);
    group_report(0, 0, 0, 0);
    reset(); drag3_on = 1; move_gain = 1; move_acceleration = 0;
    group_report(3, 0, 0, 0);
    for (int i = 1; i <= 50; i++) group_report(3, i / 3, 0, 0);
    assert(three_drag.phase == DRAG3_ACTIVE && test_cursor.x == 516);
    group_report(0, 0, 0, 0);

    // Reordering report slots preserves contact identities. A replaced contact
    // reanchors without jumping, while subsequent motion remains a drag.
    reset(); drag3_on = swipe3_on = 1; move_gain = 1; move_acceleration = 0;
    start_drag(); count = mouse_count;
    DragContact reordered[] = {{2,524,400},{0,324,400},{1,424,400}};
    contacts_report(reordered, 3, 0);
    assert(mouse_count == count && test_cursor.x == 524);
    reordered[0] = (DragContact){6,1000,650};
    contacts_report(reordered, 3, 0);
    assert(mouse_count == count && test_cursor.x == 524);
    for (int i = 0; i < 3; i++) { reordered[i].x += 2; reordered[i].y -= 1; }
    contacts_report(reordered, 3, 0);
    assert(test_cursor.x == 526 && test_cursor.y == 499);
    group_report(0, 0, 0, 0);

    // Sub-count centroid motion accumulates instead of being truncated, and
    // an added fourth finger cannot steal a drag for a desktop gesture.
    reset(); drag3_on = swipe3_on = 1; move_acceleration = 0;
    start_drag(); double before = test_cursor.x;
    DragContact precise[] = {{0,324,400},{1,424,400},{2,524,400}};
    for (int i = 0; i < 30; i++) { precise[0].x++; contacts_report(precise, 3, 0); }
    assert(test_cursor.x - before == 18);
    group_report(0, 0, 0, 0);
    reset(); drag3_on = swipe3_on = 1; move_gain = 1; move_acceleration = 0;
    start_drag();
    group_report(4, 30, 0, 0); group_report(4, 40, 0, 0);
    assert(test_cursor.x == 540 && !swipe3_active);
    group_report(3, 45, 0, 1); // Accidental pressure still belongs to the drag.
    assert(!btn_prev && test_cursor.x == 545);
    group_report(0, 0, 0, 0);
    int downs = 0, ups = 0;
    for (int i = 0; i < mouse_count; i++) {
        downs += mouse_samples[i].type == kCGEventLeftMouseDown;
        ups += mouse_samples[i].type == kCGEventLeftMouseUp;
        assert(mouse_samples[i].type != kCGEventRightMouseDown);
    }
    assert(downs == 1 && ups == 1);

    // Three-finger sums must not make the pointer speed look three times faster.
    for (int fast = 0; fast < 2; fast++) {
        reset(); drag3_on = 1;
        screen_bounds.size.width = g_display_bounds[0].size.width = 10000;
        group_report(3, 0, 0, 0);
        for (int i = 1; i <= 50; i++) group_report(3, i * (fast ? 15 : 1), 0, 0);
        group_report(0, 0, 0, 0);
        assert(test_cursor.x == 500 + (fast ? 2700 : 45));
    }

    // New touches during typing are ignored; an already active drag survives
    // typing. Losing the device or stopping the daemon releases its button once.
    reset(); drag3_on = 1; typelock_ms = 500; last_key_ms = now_ms();
    for (int i = 0; i < 10; i++) group_report(3, i * 10, 0, 0);
    assert(mouse_count == 0 && three_drag.phase == DRAG3_IDLE);
    reset(); drag3_on = 1;
    start_drag(); typelock_ms = 500; last_key_ms = now_ms(); type_grandf = touch_now;
    group_report(3, 40, 0, 0);
    assert(three_drag.phase == DRAG3_ACTIVE && mouse_samples[mouse_count - 1].type == kCGEventLeftMouseDragged);
    device_detached(NULL, 0, NULL, NULL);
    assert(three_drag.phase == DRAG3_IDLE && mouse_samples[mouse_count - 1].type == kCGEventLeftMouseUp);
    count = mouse_count; release_pointer_buttons(); assert(mouse_count == count);
    reset(); drag3_on = 1; start_drag();
    release_pointer_buttons();
    assert(three_drag.phase == DRAG3_IDLE && mouse_samples[mouse_count - 1].type == kCGEventLeftMouseUp);

    // Physical index-button + middle-finger dragging has priority, even if a
    // third/fourth finger lands. Release must not be swallowed by gesture code.
    for (int fingers = 3; fingers <= 4; fingers++) {
        reset(); drag3_on = swipe3_on = 1;
        pointer_report(1, 300, 400, 0, 0); pointer_report(1, 300, 400, 1, 0);
        for (int i = 0; i < 8; i++) group_report(fingers, i * 10, 0, 1);
        assert(btn_prev && three_drag.phase == DRAG3_IDLE && !swipe3_active);
        for (int i = 0; i < 8; i++) group_report(fingers, 70, 0, 0);
        assert(!btn_prev && three_drag.phase == DRAG3_TAIL);
        assert(mouse_samples[mouse_count - 1].type == kCGEventLeftMouseUp);
        count = mouse_count; group_report(0, 0, 0, 0); assert(mouse_count == count);
    }
}

static void four_finger_swiping(void) {
    for (int vertical = 0; vertical < 2; vertical++) {
        reset(); drag3_on = swipe3_on = 1;
        group_report(1, 0, 0, 0); group_report(2, 0, 0, 0);
        group_report(3, 0, 0, 0);
        group_report(3, vertical ? 0 : 20, vertical ? 20 : 0, 0);
        assert(three_drag.phase == DRAG3_PENDING && mouse_count == 0);
        group_report(4, 20, 20, 0);
        assert(swipe3_active && three_drag.phase == DRAG3_IDLE);
        group_report(4, vertical ? 20 : 40, vertical ? 40 : 20, 0);
        group_report(4, vertical ? 20 : 60, vertical ? 60 : 20, 0);
        assert(swipe3_sent && swipe3_axis == (vertical ? 2 : 1));
        group_report(3, 100, 100, 0);
        assert(!swipe3_active && system_swipe_tail);
        for (int i = 0; i < 10; i++) group_report(3, 150 + i * 10, 100, 0);
        group_report(2, 250, 100, 0); group_report(1, 300, 100, 0); group_report(0, 0, 0, 0);
        assert(mouse_count == 0 && pinch_count == 0 && !system_swipe_tail);
        assert(three_drag.phase == DRAG3_IDLE && test_cursor.x == 500 && test_cursor.y == 500);
    }
    reset(); drag3_on = 0; swipe3_on = 1;
    group_report(3, 0, 0, 0); group_report(3, 20, 0, 0); group_report(3, 40, 0, 0);
    assert(swipe3_active && swipe3_sent && three_drag.phase == DRAG3_IDLE);
    group_report(0, 0, 0, 0);
    assert(!swipe3_active && mouse_count == 0);

    reset(); drag3_on = 1; swipe3_on = 0;
    group_report(4, 0, 0, 0); group_report(4, 100, 100, 0);
    for (int i = 0; i < 10; i++) group_report(3, 100 + i * 10, 100, 0);
    group_report(0, 0, 0, 0);
    assert(mouse_count == 0 && sample_count == 0 && !system_swipe_tail);
}

static void assert_right_tap(void) {
    assert(mouse_count == 2);
    assert(mouse_samples[0].type == kCGEventRightMouseDown);
    assert(mouse_samples[1].type == kCGEventRightMouseUp);
    assert(test_cursor.x == 500 && test_cursor.y == 500);
}

static int right_clicks(void) {
    int result = 0;
    for (int i = 0; i < mouse_count; i++) result += mouse_samples[i].type == kCGEventRightMouseDown;
    return result;
}

static void begin_two_tap(void) {
    for (int i = 0; i < 3; i++) pair_report(400, 400, 600, 400, 3, 0, 1);
}

static void two_finger_tapping(void) {
    for (int tail = 0; tail <= 2; tail++) {
        reset(); tap_on = tap_right_on = pinch_on = drag3_on = swipe3_on = 1;
        pair_report(400, 400, 600, 400, 3, 0, 1);
        pair_report(400, 400, 600, 400, 3, 0, 1);
        pair_report(400, 400, 600, 400, 3, 0, 1);
        if (tail) pair_report(400, 400, 600, 400, tail, 0, 1);
        pair_report(400, 400, 600, 400, 0, 0, 1);
        assert_right_tap();
    }

    // Fingers may land in either order or change report slots; each identity
    // keeps its own anchor instead of inheriting another finger's position.
    for (int first = 1; first <= 2; first++) {
        reset(); tap_on = tap_right_on = pinch_on = 1;
        pair_report(400, 400, 600, 400, first, 0, 1);
        begin_two_tap();
        pair_report(401, 401, 599, 399, 3, 0, 1); // Small opposing jitter, stationary centroid.
        pair_report(400, 400, 600, 400, 0, 0, 1);
        assert_right_tap();
    }
    reset(); tap_on = tap_right_on = pinch_on = 1;
    begin_two_tap();
    DragContact swapped[] = {{1,600,400},{0,400,400}};
    contacts_report(swapped, 2, 0); contacts_report(NULL, 0, 0);
    assert_right_tap();

    // Movement of the second finger alone invalidates the tap, even when an
    // isolated jump has not been classified as pinch or scroll.
    reset(); tap_on = tap_right_on = pinch_on = 1;
    begin_two_tap();
    pair_report(400, 400, 630, 400, 3, 0, 1);
    pair_report(400, 400, 630, 400, 3, 0, 1);
    pair_report(400, 400, 630, 400, 0, 0, 1);
    assert(right_clicks() == 0);

    // Scroll recognition remains sticky across delayed output and a one-finger
    // tail that has already reset gesture_state before the final lift.
    for (int buffered = 0; buffered < 2; buffered++) {
        reset(); tap_on = tap_right_on = pinch_on = 1;
        scroll_smooth_ms = buffered ? 12 : 0;
        begin_two_tap();
        pair_report(400, 402, 600, 402, 3, 0, 1);
        if (!buffered) for (int i = 0; i < 10; i++) pair_report(400, 402, 600, 402, 1, 0, 1);
        pair_report(400, 402, 600, 402, 0, 0, 1);
        assert(right_clicks() == 0);
    }
    reset(); tap_on = tap_right_on = pinch_on = 1; scroll_smooth_ms = 12;
    begin_two_tap();
    for (int i = 1; i <= 3; i++) pair_report(400 + i * 2, 400, 600 - i * 2, 400, 3, 0, 1);
    pair_report(406, 400, 594, 400, 0, 0, 1);
    assert(pinch_count > 0 && right_clicks() == 0);

    // A physical right click is exactly one click, even if the button releases
    // and finishes its debounce before the two stationary contacts lift.
    reset(); tap_on = tap_right_on = pinch_on = 1;
    begin_two_tap();
    pair_report(400, 400, 600, 400, 3, 1, 1);
    for (int i = 0; i < 6; i++) pair_report(400, 400, 600, 400, 3, 0, 1);
    pair_report(400, 400, 600, 400, 0, 0, 1);
    assert_right_tap();

    // Neither partial lift from a three/four-finger gesture nor a brief phantom
    // second contact, replaced identity or re-touch may become a secondary tap.
    for (int fingers = 3; fingers <= 4; fingers++) {
        reset(); tap_on = tap_right_on = pinch_on = drag3_on = swipe3_on = 1;
        begin_two_tap(); group_report(fingers, 0, 0, 0);
        begin_two_tap(); pair_report(400, 400, 600, 400, 0, 0, 1);
        assert(right_clicks() == 0);
    }
    for (int invalid = 0; invalid < 7; invalid++) {
        reset(); tap_on = tap_right_on = pinch_on = 1;
        if (invalid == 0) {
            pair_report(400, 400, 600, 400, 1, 0, 1);
            pair_report(400, 400, 600, 400, 3, 0, 1); // Only one paired report.
        } else {
            if (invalid == 1) {
                pair_report(400, 400, 600, 400, 1, 0, 1);
                test_time_ms += 90; // Second finger arrives too late.
            }
            begin_two_tap();
            if (invalid == 2) {
                pair_report(400, 400, 600, 400, 1, 0, 1);
                test_time_ms += 90;
                pair_report(400, 400, 600, 400, 1, 0, 1); // Final release too late.
            }
            if (invalid == 3) {
                pair_report(400, 400, 600, 400, 1, 0, 1);
                pair_report(400, 400, 600, 400, 3, 0, 1); // A lifted finger returns.
            }
            if (invalid == 4) pair_report(400, 400, 600, 400, 3, 0, 2);
            if (invalid == 5) for (int i = 0; i < 40; i++) pair_report(400, 400, 600, 400, 3, 0, 1);
            if (invalid == 6) device_detached(NULL, 0, NULL, NULL);
        }
        pair_report(400, 400, 600, 400, 0, 0, 1);
        assert(right_clicks() == 0);
    }

    // The optional right-tap switch, master tap switch, typing lock and the
    // old scroll-only mode all remain compatible with the new recognizer.
    for (int disabled = 0; disabled < 3; disabled++) {
        reset(); tap_on = tap_right_on = pinch_on = 1;
        if (disabled == 0) tap_right_on = 0;
        if (disabled == 1) tap_on = 0;
        if (disabled == 2) { typelock_ms = 20; last_key_ms = now_ms(); }
        begin_two_tap();
        pair_report(400, 400, 600, 400, 0, 0, 1);
        assert(right_clicks() == 0);
    }
    reset(); tap_on = tap_right_on = 1; pinch_on = 0;
    begin_two_tap(); pair_report(400, 400, 600, 400, 0, 0, 1);
    assert_right_tap();
}

int main(void) {
    two_finger_tapping();
    three_finger_dragging();
    four_finger_swiping();
    pinch_gestures();
    pointer_precision_and_acceleration();
    tap_no_drift();
    slow_scroll();
    distance_and_direction();
    velocity();
    momentum_phases();
    staggered_lift();
    resampler_cadence();
    resampled_gesture();
    puts("PASS: two-finger tap/independent identities/overlap/timing/gesture exclusion/physical-click exclusion, three-finger drag/selection/button ownership/identity/fractions/typing/release, four-finger swipes/arbitration/tails/legacy mode, pinch ratios/phases/arbitration/jitter/contact changes/drag priority, pointer fractions/velocity curve/cadence/reversal/edges, tap boundaries/no drift, multi-click, movement cancellation/drag, slow scroll precision, fractional fields, distance/direction, time-based velocity, pause/reversal, momentum phases/distance, staggered lift, 60/75/120Hz resampling, USB jitter, scan clock wrap, flush/no drift");
    return 0;
}
