#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace echo::perf {
    enum Slot : int {
        Physics,
        Backstep,
        Trajectory,
        Autoclick,
        Hitboxes,
        Labels,
        SlotCount,
    };

    char const* slot_name(Slot slot);

    struct Scope {
        explicit Scope(Slot slot);
        ~Scope();
        Scope(Scope const&) = delete;
        Scope& operator=(Scope const&) = delete;
        Slot slot;
        std::chrono::steady_clock::time_point from;
        double nested = 0.0;
        Scope* parent = nullptr;
    };

    void frame_start();
    void logic_done();

    void count_backstep_save();
    void count_traj_steps(int steps);
    void note_frame_dt(double gd_dt, double used_dt);
    void note_steps(int steps, double level_time);
    void note_save_parts(double gd_ms, double extra_ms, double objects_ms, double trim_ms);

    struct Summary {
        double fps = 0.0;
        double avg_ms = 0.0, low1_ms = 0.0, max_ms = 0.0, median_ms = 0.0;
        int hitches = 0;
        double slot_ms[SlotCount] = {};
        double slot_max_ms[SlotCount] = {};
        double draw_ms = 0.0;
        double steps_per_sec = 0.0, steps_per_frame = 0.0;
        double level_speed = 0.0;
        int saves_per_sec = 0;
        double traj_steps_per_frame = 0.0;
        double gd_dt_ms = 0.0, used_dt_ms = 0.0;
        std::string last_hitch;
        double save_gd_ms = 0.0, save_extra_ms = 0.0, save_objects_ms = 0.0, save_trim_ms = 0.0;
    };

    Summary const& summary();
}
