#pragma once
#include <chrono>
#include <cstdint>
#include <string>

enum PerfSlot : int {
    PERF_PHYSICS,
    PERF_BACKSTEP,
    PERF_TRAJECTORY,
    PERF_AUTOCLICK,
    PERF_HITBOXES,
    PERF_LABELS,
    PERF_SLOT_COUNT,
};

const char* perf_slot_name(PerfSlot slot);

struct PerfScope {
    explicit PerfScope(PerfSlot slot);
    ~PerfScope();
    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;
    PerfSlot slot;
    std::chrono::steady_clock::time_point from;
    double nested = 0.0;
    PerfScope* parent = nullptr;
};

void perf_frame_start();
void perf_logic_done();

void perf_count_backstep_save();
void perf_count_traj_steps(int steps);
void perf_note_frame_dt(double gd_dt, double used_dt);
void perf_note_steps(int steps, double level_time);
void perf_note_save_parts(double gd_ms, double extra_ms, double objects_ms, double trim_ms);

struct PerfSummary {
    double fps = 0.0;
    double avg_ms = 0.0, low1_ms = 0.0, max_ms = 0.0, median_ms = 0.0;
    int hitches = 0;
    double slot_ms[PERF_SLOT_COUNT] = {};
    double slot_max_ms[PERF_SLOT_COUNT] = {};
    double draw_ms = 0.0;
    double steps_per_sec = 0.0, steps_per_frame = 0.0;
    double level_speed = 0.0;
    int saves_per_sec = 0;
    double traj_steps_per_frame = 0.0;
    double gd_dt_ms = 0.0, used_dt_ms = 0.0;
    std::string last_hitch;
    double save_gd_ms = 0.0, save_extra_ms = 0.0, save_objects_ms = 0.0, save_trim_ms = 0.0;
};

const PerfSummary& perf_summary();
