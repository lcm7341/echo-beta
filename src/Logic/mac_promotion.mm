// which screen the game window is on & how fast that screen refreshes
// the window is mainWindow, else keyWindow, else the first window there is
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "Logic/mac_refresh_clock.hpp"

namespace {
    NSScreen* gameScreen() {
        NSWindow* window = NSApp.mainWindow ?: NSApp.keyWindow ?
                                                               : NSApp.windows.firstObject;
        return window.screen ?: NSScreen.mainScreen;
    }
}

namespace echo::mac {
    uint32_t window_display() {
        NSNumber* number = gameScreen().deviceDescription[@"NSScreenNumber"];
        return number ? number.unsignedIntValue : 0;
    }

    double window_max_refresh() {
        if (@available(macOS 12.0, *))
            return static_cast<double>(gameScreen().maximumFramesPerSecond);
        return 0.0;
    }
}
