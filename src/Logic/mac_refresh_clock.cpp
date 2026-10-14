#include "Logic/mac_refresh_clock.hpp"
#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

// manual because no dwm on mac

static CVDisplayLinkRef display_link = nullptr;
static bool mac_said_no = false;
static uint32_t screen_being_counted = 0;
static std::chrono::steady_clock::time_point last_screen_check;
static std::atomic<uint64_t> tick_counter{ 0 };
static uint64_t ticks_last_frame = 0;
static bool is_first_frame = true;

static CVReturn on_screen_tick(CVDisplayLinkRef, CVTimeStamp const*, CVTimeStamp const*, CVOptionFlags, CVOptionFlags*, void*) {
    tick_counter++;
    return kCVReturnSuccess;
}

static bool start_counting() {
    screen_being_counted = echo::mac::window_display();
    if (screen_being_counted == 0)
        screen_being_counted = CGMainDisplayID();

    if (CVDisplayLinkCreateWithCGDisplay(screen_being_counted, &display_link) != kCVReturnSuccess) {
        display_link = nullptr;
        return false;
    }
    CVDisplayLinkSetOutputCallback(display_link, on_screen_tick, nullptr);
    if (CVDisplayLinkStart(display_link) != kCVReturnSuccess) {
        CVDisplayLinkRelease(display_link);
        display_link = nullptr;
        return false;
    }
    return true;
}

static void switch_screens_if_moved() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_screen_check < std::chrono::seconds(1))
        return;
    last_screen_check = now;

    uint32_t screen_now = echo::mac::window_display();
    if (screen_now == 0 || screen_now == screen_being_counted)
        return;
    if (CVDisplayLinkSetCurrentCGDisplay(display_link, screen_now) == kCVReturnSuccess) {
        screen_being_counted = screen_now;
        is_first_frame = true;
    }
}

static bool ask_mac_for_ticks(uint64_t& total_ticks, double& seconds_per_tick) {
    if (mac_said_no)
        return false;
    if (display_link == nullptr && !start_counting()) {
        mac_said_no = true;
        return false;
    }
    switch_screens_if_moved();

    seconds_per_tick = CVDisplayLinkGetActualOutputVideoRefreshPeriod(display_link);
    if (seconds_per_tick <= 0 || seconds_per_tick >= 0.1)
        return false;
    total_ticks = tick_counter.load();
    return true;
}

namespace echo::mac {
    double refresh_frame_time(double gd_time) {
        uint64_t total_ticks;
        double seconds_per_tick;
        if (!ask_mac_for_ticks(total_ticks, seconds_per_tick))
            return gd_time;

        uint64_t ticks_this_frame = total_ticks - ticks_last_frame;
        ticks_last_frame = total_ticks;

        if (is_first_frame) {
            is_first_frame = false;
            return gd_time;
        }

        double screen_time = ticks_this_frame * seconds_per_tick;
        if (std::abs(screen_time - gd_time) > seconds_per_tick * 2)
            return gd_time;
        return screen_time;
    }
}
