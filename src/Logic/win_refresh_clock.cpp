#include "Logic/win_refresh_clock.hpp"
#include <Windows.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdint>

// same thing as mac_refresh_clock.cpp but for windows.
//
// the screen ticks at a steady rate (eg 240fps) & shows 1 frame per tick, but gd's dt is not steady
// the lvl ends up moving uneven amounts while the screen stays even, which i'm pretty
// sure is most of the stutter. so this counts the screen ticks since last frame & use (ticks * tick length) as the dt instead.
// if thats more than 2 ticks off from gd's dt something weird probably happened (lag spike, or window hidden or idk)
// & just default to regular dt on that frame

static bool windows_said_no = false;
static uint64_t ticks_last_frame = 0;
static bool is_first_frame = true;
// kept within 1 refresh of real time
static LARGE_INTEGER qpc_last{};
static double real_total = 0.0, used_total = 0.0;

static bool ask_windows_for_ticks(uint64_t& total_ticks, double& seconds_per_tick) {
    if (windows_said_no)
        return false;

    DWM_TIMING_INFO info = {};
    info.cbSize = sizeof(info);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info))) {
        windows_said_no = true;
        return false;
    }

    if (info.rateRefresh.uiNumerator == 0 || info.rateRefresh.uiDenominator == 0)
        return false;
    seconds_per_tick = (double)info.rateRefresh.uiDenominator / info.rateRefresh.uiNumerator;
    total_ticks = info.cRefresh;
    // wine leaves the count at 0
    if (total_ticks == 0) {
        windows_said_no = true;
        return false;
    }
    return true;
}

namespace echo::win {
    double refresh_frame_time(double gd_time) {
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        double real = qpc_last.QuadPart ? static_cast<double>(now.QuadPart - qpc_last.QuadPart) / static_cast<double>(frequency.QuadPart) : -1.0;
        qpc_last = now;
        // not called for a while: start over
        if (!(real >= 0.0) || real > 0.25)
            is_first_frame = true;

        uint64_t total_ticks;
        double seconds_per_tick;
        if (!ask_windows_for_ticks(total_ticks, seconds_per_tick))
            return gd_time;

        uint64_t ticks_this_frame = total_ticks - ticks_last_frame;
        ticks_last_frame = total_ticks;

        if (is_first_frame) {
            is_first_frame = false;
            real_total = used_total = 0.0;
            return gd_time;
        }
        real_total += real;

        double screen_time = ticks_this_frame * seconds_per_tick;
        if (std::abs(screen_time - gd_time) > seconds_per_tick * 2)
            screen_time = gd_time;
        if (std::abs(used_total + screen_time - real_total) > seconds_per_tick)
            screen_time = std::max(real_total - used_total, 0.0);
        used_total += screen_time;
        double settled = std::min(used_total, real_total);
        used_total -= settled;
        real_total -= settled;
        return screen_time;
    }
}
