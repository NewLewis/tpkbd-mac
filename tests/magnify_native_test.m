#import <AppKit/AppKit.h>
#include <assert.h>
#include <math.h>
#include "../magnify_event.h"

int main(void) { @autoreleasepool {
    const int phases[] = {1, 2, 4, 8};
    const NSEventPhase native[] = {NSEventPhaseBegan, NSEventPhaseChanged,
                                   NSEventPhaseEnded, NSEventPhaseCancelled};
    for (int i = 0; i < 4; i++) for (int sign = -1; sign <= 1; sign += 2) {
        double delta = phases[i] == 1 || phases[i] == 2 ? sign * 0.125 : 0;
        CGEventRef cg = magnify_event_create(CGPointMake(500, 500), delta, phases[i], 1000000000);
        assert(cg);
        NSEvent *event = [NSEvent eventWithCGEvent:cg];
        assert(event && event.type == NSEventTypeMagnify);
        assert(event.phase == native[i]);
        assert(fabs(event.magnification - delta) < 1e-7);
        CFRelease(cg);
    }
    puts("PASS: AppKit decodes native magnify type, amount, Began/Changed/Ended/Cancelled");
} return 0; }
