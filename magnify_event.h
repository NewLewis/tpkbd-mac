#ifndef TPKBD_MAGNIFY_EVENT_H
#define TPKBD_MAGNIFY_EVENT_H
#include <CoreGraphics/CoreGraphics.h>
#include <stdint.h>

// Gesture layout follows the magnification path in Mac Mouse Fix TouchSimulator.
// AppKit decodes this CG type-29/subtype-8 event as NSEventTypeMagnify.
static inline CGEventRef magnify_event_create(CGPoint location, double delta,
                                              int phase, uint64_t ns) {
    CGEventRef event = CGEventCreate(NULL);
    if (!event) return NULL;
    CGEventSetType(event, 29);
    CGEventSetIntegerValueField(event, 110, 8);
    CGEventSetIntegerValueField(event, 132, phase);
    CGEventSetDoubleValueField(event, 113, delta);
    CGEventSetLocation(event, location);
    CGEventSetTimestamp(event, ns);
    CGEventSetIntegerValueField(event, kCGEventSourceUserData, 0x54504b42);
    return event;
}
#endif
