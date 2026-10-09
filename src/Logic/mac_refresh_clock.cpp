// frame timing on mac
// the dt gd hands over jitters against the display (same refresh rate, uneven frames) so movement stutters
// a CVDisplayLink ticks once per real screen refresh. each frame: refreshes since the last frame * the refresh period = the dt that gets used
// if that is more than 2 refreshes away from gds dt (lag spike, window was hidden) gds dt wins for that frame
// the link belongs to 1 display, so once a second it checks which screen the window is on & moves the link over
// if the link cant be started it is never retried, frames just keep gds dt
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX
#include "Logic/mac_refresh_clock.hpp"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace chrono = std::chrono;

namespace {
    struct RefreshClock {
        CVDisplayLinkRef link = nullptr;
        std::atomic<uint64_t> ticks{ 0 };
        bool failed = false;
        uint64_t seen = 0;
        bool primed = false;
        uint32_t display = 0;
        chrono::steady_clock::time_point checked{};

        static CVReturn onRefresh(CVDisplayLinkRef, CVTimeStamp const*, CVTimeStamp const*, CVOptionFlags, CVOptionFlags*, void* context) {
            static_cast<RefreshClock*>(context)->ticks.fetch_add(1, std::memory_order_relaxed);
            return kCVReturnSuccess;
        }

        bool running() {
            if (link)
                return true;
            if (failed)
                return false;
            display = echo::mac::window_display();
            if (!display)
                display = CGMainDisplayID();
            if (CVDisplayLinkCreateWithCGDisplay(display, &link) != kCVReturnSuccess || !link) {
                link = nullptr;
                failed = true;
                return false;
            }
            CVDisplayLinkSetOutputCallback(link, &RefreshClock::onRefresh, this);
            if (CVDisplayLinkStart(link) != kCVReturnSuccess) {
                CVDisplayLinkRelease(link);
                link = nullptr;
                failed = true;
                return false;
            }
            return true;
        }

        double period() const {
            double actual = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link);
            if (actual > 0.0 && actual < 0.1)
                return actual;
            auto nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
            if (nominal.timeValue > 0 && nominal.timeScale > 0)
                return static_cast<double>(nominal.timeValue) / static_cast<double>(nominal.timeScale);
            return 0.0;
        }

        void follow_window() {
            auto now = chrono::steady_clock::now();
            if (now - checked < chrono::seconds(1))
                return;
            checked = now;
            uint32_t cur = echo::mac::window_display();
            if (!cur || cur == display)
                return;
            if (CVDisplayLinkSetCurrentCGDisplay(link, cur) == kCVReturnSuccess) {
                display = cur;
                primed = false;
            }
        }

        double frameTime(double dt) {
            if (!running())
                return dt;
            follow_window();
            double refresh = period();
            uint64_t now = ticks.load(std::memory_order_relaxed);
            uint64_t elapsed = now - seen;
            seen = now;
            if (!primed || !(refresh > 0.0)) {
                primed = true;
                return dt;
            }
            double counted = static_cast<double>(elapsed) * refresh;
            if (std::abs(counted - dt) > refresh * 2.0)
                return dt;
            return counted;
        }
    };

    RefreshClock s_clock;
}

namespace echo::mac {
    double refresh_frame_time(double dt) {
        return s_clock.frameTime(dt);
    }
}

#pragma clang diagnostic pop
#endif
