// decides how many physics steps each drawn frame runs, then runs them
//
// steps owed = frame time / step length. the leftover is carried to the next frame (logic.carry) so no time is lost
// the count reaches gd thru tick_steps, which the step patch makes update() read instead of doing its own math
// frame advance: exactly 1 step per press. speedhack & frame time dont matter
// pause before kill / trajectory autoclicker: 1 step at a time, so there is a chance to stop BEFORE the death
// real time: a frame may run up to a seconds worth of steps so game time stays right when physics cant keep up
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"
#include "Hack/trajectory.hpp"
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
    if (!layer) {
        return nullptr;
    }
    if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer) {
        return play;
    }
    if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer) {
        return nullptr;
    }
    return typeinfo_cast<PlayLayer*>(layer);
}

LevelEditorLayer* Logic::as_editor(GJBaseGameLayer* layer) {
    log::debug("as editor");
    if (!layer) {
        return nullptr;
    }
    if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer) {
        return editor;
    }
    if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer) {
        return nullptr;
    }
    return typeinfo_cast<LevelEditorLayer*>(layer);
}

bool Logic::advance_paused(GJBaseGameLayer* play) {
    return play && Logic::get().frame_advance && !Logic::get().render_capturing();
}

bool Logic::lock_delta_active() const {
    return lock_delta;
}

int Logic::unlocked_step_cap() const {
    auto* director = CCDirector::sharedDirector();
    double interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
    if (!std::isfinite(interval) || interval <= 0.0) {
        interval = 1.0 / 60.0;
    }
    if (real_time_mode) {
        interval = std::max(interval, 0.1);
    }
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
    if (clock.steps == 1) {
        clock.first = now;
    }
    clock.last = now;
    clock.sampled_step = clock.steps;
}

// everything a frame does that ISNT physics
void Logic::note_frame_work(double seconds) {
    if (std::isfinite(seconds)) {
        frame_work = frame_work > 0.0 ? frame_work + (seconds - frame_work) * 0.05 : seconds;
    }
    auto& cost = step_cost();
    double other = std::max(0.0, seconds - std::exchange(cost.last_physics, 0.0));
    if (!std::isfinite(other))
        return;
    cost.other_work = cost.other_work > 0.0 ? cost.other_work + (other - cost.other_work) * 0.2 : other;
}

// for the trace. what the pacing thinks a step & a frame cost rn
std::string echo::step::pace() {
    log::debug("pace");
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
    if (!(cost.per_step > 0.0)) {
        return std::numeric_limits<int>::max();
    }
    auto* director = CCDirector::sharedDirector();
    double interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
    if (!std::isfinite(interval) || interval <= 0.0) {
        interval = 1.0 / 60.0;
    }
    if (real_time_mode) {
        constexpr double REAL_TIME_FRAME = 0.25;
        double fixed = cost.per_update + cost.other_work;
        double room = std::max({ REAL_TIME_FRAME - fixed, REAL_TIME_FRAME * 0.5, std::min(fixed, 1.0) });
        double steps = std::floor(room / cost.per_step);
        if (!std::isfinite(steps) || steps >= static_cast<double>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return std::max(static_cast<int>(steps), 8);
    }
    double budget = std::clamp(interval * 1.5, 0.004, 0.05);
    double fixed = cost.per_update + cost.other_work;
    double floor = std::max(cost.per_update, std::min(fixed * 4.0, 0.05 - fixed));
    floor = std::max(floor, std::min(fixed, 0.25));
    double room = std::max(budget - cost.per_update, floor);
    double steps = std::floor(room / cost.per_step);
    if (!std::isfinite(steps) || steps >= static_cast<double>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return std::max(static_cast<int>(steps), 8);
}

int Logic::count_unlocked_steps(GJBaseGameLayer* play, double dt) {
    auto& logic = Logic::get();
    if (!play) {
        return 0;
    }
    Logic::get().apply_rate(logic.step_frame);
    float warp = std::min(play->m_gameState.m_timeWarp, 1.f);
    if (!std::isfinite(warp) || warp < 0.f) {
        warp = 1.f;
    }
    float wanted = logic.physics_step() * warp;
    if (!std::isfinite(wanted) || !(wanted > 0.f)) {
        return 0;
    }
    if (!std::isfinite(dt) || dt < 0.f) {
        dt = 0.f;
    }
    if (logic.respawn_frames > 0) {
        logic.carry = 0.f;
        play->m_extraDelta = 0.0;
    }
    if (!std::isfinite(logic.carry) || logic.carry < 0.f) {
        logic.carry = 0.f;
    }
    double pending = logic.carry + dt;
    if (!std::isfinite(pending) || pending < 0.f) {
        pending = 0.f;
    }
    double requested = std::floor(pending / wanted);
    int steps = static_cast<int>(std::min(requested, double(std::numeric_limits<int>::max())));
    if (steps < 0) {
        steps = 0;
    }
    bool capped = false;
    logic.carry = capped && !logic.real_time_mode ? 0.f : pending - static_cast<double>(steps) * wanted;
    if (!std::isfinite(logic.carry) || logic.carry < 0.f) {
        logic.carry = 0.f;
    }
    if (logic.respawn_frames > 0) {
        --logic.respawn_frames;
        steps = std::min(steps, 1);
    }
    return steps;
}

void Logic::run_frame(GJBaseGameLayer* play, float dt, std::function<void(float)> const& tick) {
    // port this
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
