#include "Logic/perf.hpp"
#include "Hooks/step.hpp"
#include <Geode/Geode.hpp>
#include <algorithm>
#include <array>
#include <deque>
#include <vector>

using Clock = std::chrono::steady_clock;

static double ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

struct PerfFrame {
    Clock::time_point at;
    double frame_ms = 0.0, draw_ms = 0.0;
    std::array<double, PERF_SLOT_COUNT> slot{};
    int steps = 0, saves = 0, traj_steps = 0;
    double level_time = 0.0;
};

static PerfScope* g_top = nullptr;
static PerfFrame g_now;
static bool g_started = false;
static Clock::time_point g_frame_from, g_logic_done;
static bool g_logic_noted = false;
static std::deque<PerfFrame> g_window;
static PerfSummary g_summary;
static Clock::time_point g_summed;
static int g_last_step = -1;
static double g_last_level_time = -1.0;
static double g_gd_dt = 0.0, g_used_dt = 0.0;
static Clock::time_point g_hitch_at;
static double g_save_parts[4] = {};
static int g_hitch_lines = 0;

static void recompute(Clock::time_point now) {
    PerfSummary out;
    out.gd_dt_ms = g_gd_dt * 1000.0;
    out.used_dt_ms = g_used_dt * 1000.0;
    out.save_gd_ms = g_save_parts[0];
    out.save_extra_ms = g_save_parts[1];
    out.save_objects_ms = g_save_parts[2];
    out.save_trim_ms = g_save_parts[3];
    out.last_hitch = ms_between(g_hitch_at, now) < 4000.0 ? g_summary.last_hitch : std::string{};
    size_t n = g_window.size();
    if (n == 0) {
        g_summary = out;
        return;
    }
    std::vector<double> times;
    times.reserve(n);
    double span = std::max(ms_between(g_window.front().at, now), 1.0);
    double total = 0.0, level = 0.0;
    int steps = 0;
    for (const auto& frame : g_window) {
        times.push_back(frame.frame_ms);
        total += frame.frame_ms;
        out.max_ms = std::max(out.max_ms, frame.frame_ms);
        out.draw_ms += frame.draw_ms;
        for (int s = 0; s < PERF_SLOT_COUNT; ++s) {
            out.slot_ms[s] += frame.slot[s];
            out.slot_max_ms[s] = std::max(out.slot_max_ms[s], frame.slot[s]);
        }
        steps += frame.steps;
        out.saves_per_sec += frame.saves;
        out.traj_steps_per_frame += frame.traj_steps;
        level += frame.level_time;
    }
    std::sort(times.begin(), times.end());
    out.median_ms = times[n / 2];
    out.low1_ms = times[std::min(n - 1, (size_t)((double)n * 0.99))];
    out.avg_ms = total / (double)n;
    out.fps = (double)n * 1000.0 / std::max(total, span * 0.5);
    for (double time : times)
        if (out.median_ms > 0.0 && time > out.median_ms * 2.0 && time - out.median_ms > 2.0)
            ++out.hitches;
    for (int s = 0; s < PERF_SLOT_COUNT; ++s)
        out.slot_ms[s] /= (double)n;
    out.draw_ms /= (double)n;
    out.traj_steps_per_frame /= (double)n;
    out.steps_per_sec = steps * 1000.0 / std::max(total, 1.0);
    out.steps_per_frame = (double)steps / (double)n;
    out.level_speed = total > 0.0 ? level * 1000.0 / total : 0.0;
    g_summary = out;
}

static void note_hitch(const PerfFrame& frame) {
    double median = g_summary.median_ms;
    if (g_window.size() < 30 || !(median > 0.0) || frame.frame_ms < median * 2.0 || frame.frame_ms - median < 2.0)
        return;
    std::string parts;
    int worst = -1;
    for (int s = 0; s < PERF_SLOT_COUNT; ++s) {
        if (frame.slot[s] < 0.05)
            continue;
        if (worst < 0 || frame.slot[s] > frame.slot[worst])
            worst = s;
        parts += fmt::format(" {}={:.2f}", perf_slot_name((PerfSlot)s), frame.slot[s]);
    }
    double counted = frame.draw_ms;
    for (double value : frame.slot)
        counted += value;
    std::string blame = worst >= 0 && frame.slot[worst] > frame.draw_ms ? perf_slot_name((PerfSlot)worst) : "draw/other";
    auto now = Clock::now();
    bool newer = ms_between(g_hitch_at, now) > 4000.0;
    static double shown_ms = 0.0;
    if (newer || frame.frame_ms >= shown_ms) {
        shown_ms = frame.frame_ms;
        g_hitch_at = now;
        g_summary.last_hitch = fmt::format("{:.1f}ms ({} {:.1f}ms{})", frame.frame_ms, blame,
            blame == "draw/other" ? frame.draw_ms : frame.slot[worst], frame.saves ? fmt::format(", {} save", frame.saves) : std::string{});
    }
    if (g_hitch_lines < 5000) {
        ++g_hitch_lines;
        trace_note(fmt::format("HITCH ms={:.2f} median={:.2f} draw={:.2f}{} unaccounted={:.2f} steps={} saves={} traj_steps={}\n",
            frame.frame_ms, median, frame.draw_ms, parts, std::max(0.0, frame.frame_ms - counted), frame.steps, frame.saves, frame.traj_steps));
    }
}

const char* perf_slot_name(PerfSlot slot) {
    switch (slot) {
    case PERF_PHYSICS: return "physics";
    case PERF_BACKSTEP: return "backstep";
    case PERF_TRAJECTORY: return "trajectory";
    case PERF_AUTOCLICK: return "autoclick";
    case PERF_HITBOXES: return "hitboxes";
    case PERF_LABELS: return "labels";
    default: return "?";
    }
}

PerfScope::PerfScope(PerfSlot slot) : slot(slot), from(Clock::now()), parent(g_top) {
    g_top = this;
}

PerfScope::~PerfScope() {
    double took = ms_between(from, Clock::now());
    g_now.slot[slot] += std::max(0.0, took - nested);
    if (parent)
        parent->nested += took;
    g_top = parent;
}

void perf_frame_start() {
    auto now = Clock::now();
    if (g_started) {
        g_now.at = now;
        g_now.frame_ms = ms_between(g_frame_from, now);
        g_now.draw_ms = g_logic_noted ? ms_between(g_logic_done, now) : 0.0;
        if (g_now.frame_ms < 1000.0) {
            note_hitch(g_now);
            g_window.push_back(g_now);
        }
        while (!g_window.empty() && ms_between(g_window.front().at, now) > 1000.0)
            g_window.pop_front();
        if (ms_between(g_summed, now) >= 250.0) {
            g_summed = now;
            recompute(now);
        }
    }
    g_started = true;
    g_frame_from = now;
    g_logic_noted = false;
    g_now = PerfFrame{};
}

void perf_logic_done() {
    g_logic_done = Clock::now();
    g_logic_noted = true;
}

void perf_count_backstep_save() {
    ++g_now.saves;
}

void perf_count_traj_steps(int steps) {
    g_now.traj_steps += std::max(steps, 0);
}

void perf_note_frame_dt(double gd_dt, double used_dt) {
    g_gd_dt = gd_dt;
    g_used_dt = used_dt;
}

void perf_note_steps(int step, double level_time) {
    if (g_last_step >= 0 && step >= g_last_step)
        g_now.steps += step - g_last_step;
    if (g_last_level_time >= 0.0 && level_time >= g_last_level_time)
        g_now.level_time += level_time - g_last_level_time;
    g_last_step = step;
    g_last_level_time = level_time;
}

void perf_note_save_parts(double gd_ms, double extra_ms, double objects_ms, double trim_ms) {
    g_save_parts[0] = gd_ms;
    g_save_parts[1] = extra_ms;
    g_save_parts[2] = objects_ms;
    g_save_parts[3] = trim_ms;
}

const PerfSummary& perf_summary() {
    return g_summary;
}
