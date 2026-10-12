// trackpads send precise scroll deltas, mouse wheels send whole lines. the menu has to scroll them differently
#import <AppKit/AppKit.h>

bool echo_scroll_is_precise() {
    NSEvent* event = [NSApp currentEvent];
    return event && event.type == NSEventTypeScrollWheel && event.hasPreciseScrollingDeltas;
}
