#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "Logic/mac_refresh_clock.hpp"

static NSScreen* gameScreen() {
    NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow ?
                                                           : NSApp.windows.firstObject;
    return window.screen ?: NSScreen.mainScreen;
}

unsigned window_display() {
    NSNumber* number = gameScreen().deviceDescription[@"NSScreenNumber"];
    return number ? number.unsignedIntValue : 0;
}

double window_max_refresh() {
    if (@available(macOS 12.0, *)) return (double)gameScreen().maximumFramesPerSecond;
    return 0.0;
}
