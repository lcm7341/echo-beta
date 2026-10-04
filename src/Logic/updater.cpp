// decides how many physics steps each drawn frame runs, then runs them
//
// steps owed = frame time / step length. the leftover is carried to the next frame (logic.carry) so no time is lost
// the count reaches gd thru steps_this_tick, which the step patch makes update() read instead of doing its own math
// frame advance: exactly 1 step per press. speedhack & frame time dont matter
// pause before kill / trajectory autoclicker: 1 step at a time, so there is a chance to stop BEFORE the death
// real time: a frame may run up to a seconds worth of steps so game time stays right when physics cant keep up
// max_steps_for_frame times real steps & the rest of the frame, then caps the frame to what fits. heavy levels slow down, they dont freeze
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/prediction_art.hpp"
#include "Rendering/dash_end.hpp"
#include "Rendering/dash_bursts.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace chrono = std::chrono;

using namespace geode::prelude;

PlayLayer* Logic::as_play(GJBaseGameLayer* layer) {
    if (!layer)
        return nullptr;
    if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer)
        return play;
    if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer)
        return nullptr;
    return typeinfo_cast<PlayLayer*>(layer);
}

LevelEditorLayer* Logic::as_editor(GJBaseGameLayer* layer) {
    if (!layer)
        return nullptr;
    if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer)
        return editor;
    if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer)
        return nullptr;
    return typeinfo_cast<LevelEditorLayer*>(layer);
}

bool Logic::advance_paused(GJBaseGameLayer* play) {
    return play && Logic::get().frame_advance && !Logic::get().render_capturing();
}

bool Logic::lock_delta_active() const {
    if (frame_advance && !Logic::get().render_capturing())
        return true;
    return lock_delta;
}

int Logic::unlocked_step_cap() const {
    auto* director = CCDirector::sharedDirector();
    double interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
    if (!std::isfinite(interval) || interval <= 0.0)
        interval = 1.0 / 60.0;
    if (real_time_mode)
        interval = std::max(interval, 0.1);
    double per_frame = std::ceil(sample_rate() * std::max(interval, 1.0 / 1000.0));
    return static_cast<int>(std::clamp(per_frame, 1.0, 100000.0)) * 10;
}

namespace {
    using Clock = chrono::steady_clock;

    double seconds_since(Clock::time_point start, Clock::time_point end) {
        return chrono::duration<double>(end - start).count();
    }

    struct StepClock {
        Clock::time_point first, last;
        int steps = 0;
        int sampled_step = 0;
        bool active = false;
    };

    struct FrameSample {
        double spans = 0.0;
        double gaps = 0.0;
        double updates = 0.0;
        double update_time = 0.0;
        double steps = 0.0;
    };

    struct StepCost {
        double per_step = 0.0, per_update = 0.0;
        double last_physics = 0.0;
        double other_work = 0.0;
    };

    StepClock& step_clock() {
        static StepClock clock;
        return clock;
    }

    FrameSample& frame_sample() {
        static FrameSample sample;
        return sample;
    }

    StepCost& step_cost() {
        static StepCost cost;
        return cost;
    }

    template <class Update>
    void time_update(Update&& update) {
        step_clock() = {};
        step_clock().active = true;
        auto started = Clock::now();
        update();
        step_clock().active = false;
        auto const& clock = step_clock();
        auto& sample = frame_sample();
        sample.updates += 1.0;
        sample.update_time += seconds_since(started, Clock::now());
        sample.steps += clock.steps;
        if (clock.sampled_step < 2)
            return;
        sample.spans += seconds_since(clock.first, clock.last);
        sample.gaps += clock.sampled_step - 1;
    }

    void settle_cost() {
        auto sample = std::exchange(frame_sample(), FrameSample{});
        auto& cost = step_cost();
        cost.last_physics = sample.update_time;
        if (sample.gaps < 4.0)
            return;
        double per_step = sample.spans / sample.gaps;
        if (!(per_step > 0.0) || !std::isfinite(per_step))
            return;
        double per_update = std::max(0.0, (sample.update_time - sample.steps * per_step) / std::max(sample.updates, 1.0));
        bool fresh = !(cost.per_step > 0.0);
        cost.per_step = fresh ? per_step : cost.per_step + (per_step - cost.per_step) * 0.3;
        cost.per_update = fresh ? per_update : cost.per_update + (per_update - cost.per_update) * 0.3;
    }

    void place_deferred_player_art(GJBaseGameLayer* play) {
        auto& logic = Logic::get();
        if (!std::exchange(logic.player_art_deferred, false))
            return;
        for (auto* player : { play->m_player1, play->m_player2 }) {
            if (player)
                player->setPosition(player->getPosition());
        }
    }
}

void Logic::note_step() {
    auto& clock = step_clock();
    if (!clock.active)
        return;
    ++clock.steps;
    if (clock.steps > 8 && (clock.steps & 15) != 0)
        return;
    auto now = Clock::now();
    if (clock.steps == 1)
        clock.first = now;
    clock.last = now;
    clock.sampled_step = clock.steps;
}

// everything a frame does that ISNT physics
void Logic::note_frame_work(double seconds) {
    if (std::isfinite(seconds))
        frame_work = frame_work > 0.0 ? frame_work + (seconds - frame_work) * 0.05 : seconds;
    auto& cost = step_cost();
    double other = std::max(0.0, seconds - std::exchange(cost.last_physics, 0.0));
    if (!std::isfinite(other))
        return;
    cost.other_work = cost.other_work > 0.0 ? cost.other_work + (other - cost.other_work) * 0.2 : other;
}

// for the trace. what the pacing thinks a step & a frame cost rn
std::string echo::step::pace() {
    auto& logic = Logic::get();
    auto const& cost = step_cost();
    auto* director = CCDirector::sharedDirector();
    return fmt::format("tps={:.2f} fps={:.2f} interval_ms={:.3f} real_time={} lock_tps={} speed={:.3f} step_us={:.1f} update_us={:.1f} other_us={:.1f} afford={} cap={}",
        logic.sample_rate(), logic.playback_fps, director ? director->getAnimationInterval() * 1000.0 : 0.0,
        logic.real_time_mode, logic.lock_delta, logic.speedhack, cost.per_step * 1e6, cost.per_update * 1e6, cost.other_work * 1e6,
        logic.max_steps_for_frame(), logic.unlocked_step_cap());
}

int Logic::max_steps_for_frame() const {
    auto const& cost = step_cost();
    if (!(cost.per_step > 0.0))
        return std::numeric_limits<int>::max();
    auto* director = CCDirector::sharedDirector();
    double interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
    if (!std::isfinite(interval) || interval <= 0.0)
        interval = 1.0 / 60.0;
    if (real_time_mode) {
        constexpr double REAL_TIME_FRAME = 0.25;
        double fixed = cost.per_update + cost.other_work;
        double room = std::max({ REAL_TIME_FRAME - fixed, REAL_TIME_FRAME * 0.5, std::min(fixed, 1.0) });
        double steps = std::floor(room / cost.per_step);
        if (!std::isfinite(steps) || steps >= static_cast<double>(std::numeric_limits<int>::max()))
            return std::numeric_limits<int>::max();
        return std::max(static_cast<int>(steps), 8);
    }
    double budget = std::clamp(interval * 1.5, 0.004, 0.05);
    double fixed = cost.per_update + cost.other_work;
    double floor = std::max(cost.per_update, std::min(fixed * 4.0, 0.05 - fixed));
    floor = std::max(floor, std::min(fixed, 0.25));
    double room = std::max(budget - cost.per_update, floor);
    double steps = std::floor(room / cost.per_step);
    if (!std::isfinite(steps) || steps >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return std::max(static_cast<int>(steps), 8);
}

int Logic::count_unlocked_steps(GJBaseGameLayer* play, double dt) {
    auto& logic = Logic::get();
    if (!play)
        return 0;
    Logic::get().apply_rate(logic.step_frame);
    float warp = std::min(play->m_gameState.m_timeWarp, 1.f);
    if (!std::isfinite(warp) || warp < 0.f)
        warp = 1.f;
    // a time warp under 1x makes each step SHORTER in level time, it does not add steps (gd runs the same step count)
    // dividing by the warp here ran 2x the steps at 0.5x so the level never slowed down
    (void)warp;
    float wanted = logic.physics_step();
    if (!std::isfinite(wanted) || !(wanted > 0.f))
        return 0;
    if (!std::isfinite(dt) || dt < 0.f)
        dt = 0.f;
    if (logic.respawn_frames > 0) {
        logic.carry = 0.f;
        play->m_extraDelta = 0.0;
    }
    if (!std::isfinite(logic.carry) || logic.carry < 0.f)
        logic.carry = 0.f;
    double pending = logic.carry + dt;
    if (!std::isfinite(pending) || pending < 0.f)
        pending = 0.f;
    double requested = std::floor(pending / wanted);
    int steps = static_cast<int>(std::min(requested, double(std::numeric_limits<int>::max())));
    if (steps < 0)
        steps = 0;
    bool capped = false;
    if (!Logic::get().render_capturing()) {
        int affordable = Logic::get().max_steps_for_frame();
        int cap = std::max(std::min(Logic::get().unlocked_step_cap(), affordable) - logic.frame_steps, 0);
        capped = steps > cap;
        steps = std::min(steps, cap);
    }
    logic.carry = capped && !logic.real_time_mode ? 0.f : pending - static_cast<double>(steps) * wanted;
    if (!std::isfinite(logic.carry) || logic.carry < 0.f)
        logic.carry = 0.f;
    if (logic.respawn_frames > 0) {
        --logic.respawn_frames;
        steps = std::min(steps, 1);
    }
    return steps;
}

void Logic::run_frame(GJBaseGameLayer* play, float dt, std::function<void(float)> const& tick) {
    if (Logic::get().render_paused())
        return;
    auto& logic = Logic::get();
    if (!play) {
        tick(dt);
        return;
    }
    echo::dash::prepare();
    echo::bursts::prepare();
    logic.frame_steps = 0;
    bool paused = Logic::get().editor_paused;
    if (auto* live = Logic::as_play(play))
        paused = paused || live->m_isPaused;
    if (auto* live = Logic::as_play(play); live && !live->m_started && !Logic::get().render_capturing()) {
        logic.carry = 0.f;
        logic.steps_left = 0;
        tick(dt);
        return;
    }
    if (paused) {
        logic.carry = 0.f;
        tick(dt);
        return;
    }
    // death timer & auto retry are in here. skipping it freezes the attempt
    if (auto* live = Logic::as_play(play); live && live->m_playerDied) {
        logic.carry = 0.f;
        logic.steps_left = 0;
        logic.steps_this_tick = 0;
        if (Logic::get().advance_paused(play)) {
            logic.advance_queued = false;
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        Logic::get().sync_time_scale();
        tick(dt);
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }
    if (Logic::get().defer_physics()) {
        logic.carry = 0.f;
        tick(dt);
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }

    bool force_step = false;
    if (Logic::get().advance_paused(play)) {
        logic.carry = 0.f;
        logic.respawn_frames = 0;
        play->m_extraDelta = 0.0;
        if (logic.find_best_queued) {
            logic.find_best_queued = false;
            Logic::get().find_best_frame(Logic::as_play(play), tick);
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        if (!logic.advance_queued) {
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        force_step = true;
    }

    float speed = Logic::get().render_active() ? 1.f : std::max(logic.speedhack, 0.01f);
    if (!std::isfinite(speed) || speed < 0.01f)
        speed = 1.f;
    float frame_dt = dt;
    if (!std::isfinite(frame_dt) || frame_dt < 0.f)
        frame_dt = 0.f;
    bool rendering = Logic::get().render_capturing();
    if (!rendering && !logic.real_time_mode && frame_dt > 0.25f)
        frame_dt = 0.25f;
    frame_dt *= speed;

    auto died = [&] {
        auto* live = Logic::as_play(play);
        return live && live->m_playerDied;
    };
    auto tick_length = [&] {
        return logic.physics_step() * std::min(play->m_gameState.m_timeWarp, 1.f);
    };
    // pause before kill. uses the last trajectory NOT a new predict
    auto pause_kill_safe_steps = [&]() -> int {
        if (!logic.pause_before_kill || !logic.is_recording() || rendering)
            return std::numeric_limits<int>::max();
        auto watch = [&](bool player2) {
            if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2))
                return false;
            if (logic.is_recording())
                return logic.records_player(player2);
            return true;
        };
        if ((watch(false) && logic.death_next_p1) || (watch(true) && logic.death_next_p2))
            return 0;
        bool have_traj = logic.traj_dies_p1 || logic.traj_dies_p2 || logic.traj_alive_p1 > 0.f || logic.traj_alive_p2 > 0.f;
        if (!have_traj)
            return std::numeric_limits<int>::max();
        float alive = std::numeric_limits<float>::infinity();
        if (watch(false))
            alive = std::min(alive, logic.traj_alive_p1);
        if (watch(true))
            alive = std::min(alive, logic.traj_alive_p2);
        float step_sec = tick_length();
        if (!(step_sec > 0.f) || !std::isfinite(alive))
            return std::numeric_limits<int>::max();
        int safe = static_cast<int>(std::floor(static_cast<double>(alive / step_sec) + 1e-9));
        return std::max(0, safe);
    };
    bool pause_kill = logic.pause_before_kill && logic.is_recording() && !rendering;
    // NOT while a macro plays. it made every step a whole update (75k tps = 1250 updates a frame)
    bool traj_click = logic.traj_autoclicker && !rendering && !(logic.is_playing() && !logic.is_recording());
    auto lookahead = [&] {
        if (pause_kill && !logic.show_traj)
            Trajectory::get().lookahead(PlayLayer::get());
    };
    auto autoclick = [&] {
        if (traj_click)
            Trajectory::get().autoclick(PlayLayer::get());
    };
    if (!force_step)
        autoclick();
    if (!force_step && pause_kill) {
        auto* live = PlayLayer::get();
        lookahead();
        Logic::get().check_advance_pause(live);
        if (Logic::get().advance_paused(play) && !logic.advance_queued) {
            Trajectory::get().refresh(live);
            return;
        }
    }
    bool can_batch = step_patched || logic.sample_rate() == VANILLA_TPS;
    // lock tps used to force 1 whole update per step (his trace: 21657 steps, 21657 updates). only frame advance needs that
    bool single_steps = logic.frame_advance && !rendering;
    if (!force_step && !single_steps && !pause_kill && !traj_click && can_batch) {
        int steps = Logic::get().count_unlocked_steps(play, frame_dt);
        steps = std::min(steps, pause_kill_safe_steps());
        if (steps <= 0) {
            Logic::get().check_advance_pause(PlayLayer::get());
            logic.present = true;
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        while (steps > 0) {
            if (died())
                break;
            int batch = std::min(steps, logic.steps_until_rate_change());
            float left = static_cast<float>(steps - batch) * logic.physics_step();
            float step_dt = logic.physics_step();
            logic.steps_left = batch;
            logic.steps_this_tick = static_cast<uint32_t>(batch);
            logic.frame_steps += batch;
            logic.defer_player_art = batch > 1 && phys::fast_prediction_pos();
            time_update([&] {
                play->update(step_dt * static_cast<float>(batch));
            });
            logic.defer_player_art = false;
            place_deferred_player_art(play);
            logic.steps_left = 0;
            logic.skip_physics = true;
            tick(step_dt * static_cast<float>(batch));
            logic.skip_physics = false;
            if (died() || batch == steps)
                break;
            double carry = logic.carry;
            logic.carry = 0.f;
            steps = Logic::get().count_unlocked_steps(play, left + carry);
            steps = std::min(steps, pause_kill_safe_steps());
        }
        settle_cost();
        logic.frame_steps = 0;
        logic.steps_left = 0;
        logic.steps_this_tick = 0;
        logic.present = true;
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }

    if (!force_step) {
        int steps = Logic::get().count_unlocked_steps(play, frame_dt);
        steps = std::min(steps, pause_kill_safe_steps());
        if (steps <= 0) {
            Logic::get().check_advance_pause(PlayLayer::get());
            logic.present = true;
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        while (steps > 0) {
            if (died())
                break;
            int batch = std::min(steps, logic.steps_until_rate_change());
            float left = static_cast<float>(steps - batch) * logic.physics_step();
            bool finished = false;
            for (int i = 0; i < batch; ++i) {
                if (died()) {
                    finished = true;
                    break;
                }
                // DONT resim mid frame
                if (traj_click && i > 0)
                    autoclick();
                if (pause_kill && i > 0 && !traj_click)
                    lookahead();
                if (pause_kill && pause_kill_safe_steps() <= 0) {
                    Logic::get().check_advance_pause(PlayLayer::get());
                    finished = true;
                    break;
                }
                float step_dt = logic.physics_step();
                logic.steps_left = 1;
                logic.steps_this_tick = 1;
                ++logic.frame_steps;
                time_update([&] {
                    play->update(step_dt);
                });
                logic.steps_left = 0;
                logic.skip_physics = true;
                tick(step_dt);
                logic.skip_physics = false;
                if (died()) {
                    finished = true;
                    break;
                }
                if (pause_kill) {
                    float step_sec = tick_length();
                    if (logic.traj_alive_p1 > 0.f)
                        logic.traj_alive_p1 = std::max(0.f, logic.traj_alive_p1 - step_sec);
                    if (logic.traj_alive_p2 > 0.f)
                        logic.traj_alive_p2 = std::max(0.f, logic.traj_alive_p2 - step_sec);
                    if (logic.traj_alive_p1 <= 1e-6f && logic.traj_dies_p1)
                        logic.death_next_p1 = true;
                    if (logic.traj_alive_p2 <= 1e-6f && logic.traj_dies_p2)
                        logic.death_next_p2 = true;
                    if (pause_kill_safe_steps() <= 0) {
                        Logic::get().check_advance_pause(PlayLayer::get());
                        finished = true;
                        break;
                    }
                }
            }
            if (finished || batch == steps)
                break;
            double carry = logic.carry;
            logic.carry = 0.f;
            steps = Logic::get().count_unlocked_steps(play, left + carry);
            steps = std::min(steps, pause_kill_safe_steps());
        }
        settle_cost();
        logic.frame_steps = 0;
        logic.steps_left = 0;
        logic.steps_this_tick = 0;
        logic.present = true;
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }

    int limit = std::max(logic.advance_steps, 1);
    logic.advance_queued = false;
    logic.advance_steps = 1;
    logic.carry = 0.f;
    play->m_extraDelta = 0.0;
    logic.steps_left = 0;
    for (int taken = 0; taken < limit; ++taken) {
        if (died())
            break;
        if (pause_kill && logic.holding_advance && logic.backstep_target < 0) {
            if (!traj_click)
                Trajectory::get().lookahead(PlayLayer::get());
            Logic::get().check_advance_pause(PlayLayer::get());
            if (!logic.holding_advance)
                break;
        }
        Logic::get().apply_rate(logic.step_frame);
        float tick_dt = logic.physics_step();
        if (!std::isfinite(tick_dt) || !(tick_dt > 0.f) || tick_dt > 1.f)
            break;
        logic.steps_left = 1;
        logic.steps_this_tick = 1;
        tick(tick_dt);
        logic.steps_left = 0;
        logic.steps_this_tick = 0;
    }
    if (logic.backstep_target >= 0) {
        Logic::get().click_best_frame(Logic::as_play(play));
    }
    logic.present = true;

    Trajectory::get().refresh(PlayLayer::get());
}

void Logic::stop_best_frame(char const* why) {
    // later
}

void Logic::request_best_frame() {
    // TODO
}

void Logic::find_best_frame(PlayLayer* play, std::function<void(float)> const& tick) {
    // port this
}

void Logic::click_best_frame(PlayLayer* play) {
    // TODO
}

void Logic::cancel_steps() {
    auto& logic = Logic::get();
    logic.steps_left = 0;
    logic.carry = 0.f;
}

void Logic::flush_visual_refresh() {
    auto& logic = Logic::get();
    if (!logic.refresh_queued)
        return;
    auto* director = CCDirector::sharedDirector();
    if (!director || !director->getScheduler())
        return;
    logic.refresh_queued = false;
    logic.refresh_only = true;
    director->getScheduler()->update(0.f);
    logic.refresh_only = false;
}
