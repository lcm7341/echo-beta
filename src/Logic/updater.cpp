#include "Logic/perf.hpp"
#include "Logic/logic.hpp"
#include "GUI/gui.hpp"
#include "Hooks/step.hpp"
#include "Hooks/opt.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/moving_objects.hpp"
#include "Physics/collision_bounds.hpp"
#include "Physics/prediction_art.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <algorithm>
#include <map>
#include <fstream>
#include <deque>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_set>
#include <utility>
#ifdef GEODE_IS_MACOS
#include "Logic/mac_refresh_clock.hpp"
#endif
#ifdef GEODE_IS_WINDOWS
#include "Logic/win_refresh_clock.hpp"
#endif

namespace chrono = std::chrono;

using namespace geode::prelude;

PlayLayer* Logic::as_play(GJBaseGameLayer* layer) {
    if (!layer) return nullptr;
    if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer)
        return play;
    if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer)
        return nullptr;
    return typeinfo_cast<PlayLayer*>(layer);
}

LevelEditorLayer* Logic::as_editor(GJBaseGameLayer* layer) {
    if (!layer) return nullptr;
    if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer)
        return editor;
    if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer)
        return nullptr;
    return typeinfo_cast<LevelEditorLayer*>(layer);
}

bool Logic::advance_paused(GJBaseGameLayer* play) {
    return play && frame_advance && !render_capturing();
}

bool Logic::real_time_now() {
    return real_time_mode && (!render_capturing() || render_real_time);
}

bool Logic::lock_delta_active() {
    if (frame_advance && !render_capturing()) return true;
    return lock_delta;
}

static float g_frame_speed = 1.f;

int Logic::unlocked_step_cap() {
    double interval = CCDirector::sharedDirector()->getAnimationInterval();
    if (real_time_now()) interval = std::max(interval, 0.1);
    // 10 frames worth of steps is the most 1 frame may run. that is counted at the CURRENT speed:
    // w/out the speed in here a 99x speedhack topped out at 10-20x no matter how fast the pc is
    double speed = std::max(g_frame_speed, 1.f);
    double per_frame = std::ceil(sample_rate() * std::max(interval, 1.0 / 1000.0) * speed);
    return (int)std::clamp(per_frame * 10.0, 1.0, 10000000.0);
}

using Clock = chrono::steady_clock;

static double seconds_since(Clock::time_point start, Clock::time_point end) {
    return chrono::duration<double>(end - start).count();
}

struct StepClock {
    Clock::time_point first, last;
    int steps = 0;
    int sampled_step = 0;
    bool active = false;
    double untimed = 0.0;
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

static StepClock g_step_clock;

static FrameSample g_frame_sample;

static StepCost g_step_cost;

template <class Update>
static void time_update(Update&& update) {
    g_step_clock = {};
    g_step_clock.active = true;
    auto started = Clock::now();
    update();
    g_step_clock.active = false;
    const auto& clock = g_step_clock;
    auto& sample = g_frame_sample;
    sample.updates += 1.0;
    double took = seconds_since(started, Clock::now());
    sample.update_time += took;
    if (Logic::get().render_capturing()) Logic::get().render_physics += took;
    sample.steps += clock.steps;
    if (clock.sampled_step < 2) return;
    sample.spans += std::max(0.0, seconds_since(clock.first, clock.last) - clock.untimed);
    sample.gaps += clock.sampled_step - 1;
}

static void settle_cost() {
    auto sample = std::exchange(g_frame_sample, FrameSample{});
    auto& cost = g_step_cost;
    cost.last_physics = sample.update_time;
    if (sample.gaps < 4.0) return;
    double per_step = sample.spans / sample.gaps;
    if (per_step <= 0.0) return;
    double per_update = std::max(0.0, (sample.update_time - sample.steps * per_step) / std::max(sample.updates, 1.0));
    bool fresh = !(cost.per_step > 0.0);
    cost.per_step = fresh ? per_step : cost.per_step + (per_step - cost.per_step) * 0.3;
    cost.per_update = fresh ? per_update : cost.per_update + (per_update - cost.per_update) * 0.3;
}

static void place_deferred_player_art(GJBaseGameLayer* play) {
    auto& logic = Logic::get();
    if (!std::exchange(logic.player_art_deferred, false)) return;
    for (auto* player : { play->m_player1, play->m_player2 }) {
        if (player) player->setPosition(player->getPosition());
    }
}

static bool g_single_path = false;
static double g_single_cost = 0.0;

static int single_affordable() {
    if (!(g_single_cost > 0.0)) return std::numeric_limits<int>::max();
    double interval = CCDirector::sharedDirector()->getAnimationInterval();
    double budget = std::clamp(interval * 1.5, 0.004, 0.05);
    double room = std::max(budget - g_step_cost.other_work, budget * 0.5);
    double steps = std::floor(room / g_single_cost);
    if (steps >= (double)(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    return std::max((int)steps, 1);
}

void Logic::note_untimed(double seconds) {
    auto& clock = g_step_clock;
    if (clock.active) clock.untimed += seconds;
}

void Logic::note_step() {
    auto& clock = g_step_clock;
    if (!clock.active) return;
    ++clock.steps;
    if (clock.steps > 8 && (clock.steps & 15) != 0) return;
    auto now = Clock::now();
    if (clock.steps == 1) clock.first = now;
    clock.last = now;
    clock.sampled_step = clock.steps;
}

void Logic::note_frame_work(double seconds) {
    frame_work = frame_work > 0.0 ? frame_work + (seconds - frame_work) * 0.05 : seconds;
    auto& cost = g_step_cost;
    double other = std::max(0.0, seconds - std::exchange(cost.last_physics, 0.0));
    cost.other_work = cost.other_work > 0.0 ? cost.other_work + (other - cost.other_work) * 0.2 : other;
}

std::string step_pace() {
    auto& logic = Logic::get();
    const auto& cost = g_step_cost;
    return fmt::format("tps={:.2f} fps={:.2f} interval_ms={:.3f} real_time={} lock_tps={} speed={:.3f} step_us={:.1f} update_us={:.1f} other_us={:.1f} afford={} cap={}",
        logic.sample_rate(), logic.playback_fps, CCDirector::sharedDirector()->getAnimationInterval() * 1000.0,
        logic.real_time_mode, logic.lock_delta, logic.speedhack, cost.per_step * 1e6, cost.per_update * 1e6, cost.other_work * 1e6,
        logic.affordable_steps(), logic.unlocked_step_cap());
}

int Logic::affordable_steps() const {
    const auto& cost = g_step_cost;
    if (!(cost.per_step > 0.0)) return std::numeric_limits<int>::max();
    double interval = CCDirector::sharedDirector()->getAnimationInterval();
    double budget = std::clamp(interval * 1.5, 0.004, 0.05);
    double fixed = cost.per_update + cost.other_work;
    double floor = std::max(cost.per_update, std::min(fixed * 4.0, 0.05 - fixed));
    floor = std::max(floor, std::min(fixed, 0.25));
    double room = std::max(budget - cost.per_update, floor);
    double steps = std::floor(room / cost.per_step);
    if (steps >= (double)(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
    return std::max((int)steps, 8);
}

int Logic::count_unlocked_steps(GJBaseGameLayer* play, double dt) {
    apply_rate(step_frame);
    float wanted = physics_step();
    if (respawn_frames > 0) {
        carry = 0.f;
        play->m_extraDelta = 0.0;
    }
    double pending = carry + dt;
    double requested = std::floor(pending / wanted);
    int steps = (int)std::min(requested, double(std::numeric_limits<int>::max()));
    bool capped = false;
    bool real_time = real_time_now();
    bool uncapped = real_time && true_real_time;
    bool render_cap = false;
    if (render_capturing() && render_real_time) {
        auto* live = Logic::as_play(play);
        render_cap = live && (live->m_hasCompletedLevel || live->m_levelEndAnimationStarted);
    }
    if ((!render_capturing() || render_cap) && !lock_delta && !uncapped) {
        int affordable = g_single_path ? single_affordable() : affordable_steps();
        int cap = std::max(std::min(unlocked_step_cap(), affordable) - frame_steps, 0);
        capped = steps > cap;
        steps = std::min(steps, cap);
    }
    const double REAL_TIME_BACKLOG = 0.1;
    carry = capped && !real_time ? 0.f : pending - (double)steps * wanted;
    if (capped && real_time) carry = std::min((double)carry, REAL_TIME_BACKLOG);
    if (carry < 0.f) carry = 0.f;
    if (respawn_frames > 0) {
        --respawn_frames;
        steps = std::min(steps, 1);
    }
    return steps;
}

void Logic::run_frame(GJBaseGameLayer* play, float dt, const std::function<void(float)>& tick) {
    if (render_paused()) return;
    // a lvl that is on its way out (finished it, then edit level) is still what PlayLayer::get() hands back for a bit.
    // its not on screen anymore & stepping it myself crashed in updateVisibility (android). gd runs whatever is left of it
    if (!play || !play->isRunning() || (Logic::as_play(play) && play->getParent() != CCDirector::sharedDirector()->getRunningScene())) {
        tick(dt);
        return;
    }
    flatten_click_times(play);
    MovingObjects::prepare();
#ifdef GEODE_IS_WINDOWS
    CollisionBounds::prepare();
#endif
    frame_steps = 0;
    bool paused = editor_paused;
    if (auto* live = Logic::as_play(play)) paused = paused || live->m_isPaused;
    if (auto* live = Logic::as_play(play); live && !live->m_started && !render_capturing()) {
        carry = 0.f;
        steps_left = 0;
        tick(dt);
        return;
    }
    if (paused) {
        carry = 0.f;
        tick(dt);
        return;
    }
    if (auto* live = Logic::as_play(play); live && live->m_playerDied) {
        carry = 0.f;
        steps_left = 0;
        steps_this_tick = 0;
        if (advance_paused(play)) {
            advance_queued = false;
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        sync_time_scale();
        tick(dt);
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }
    if (defer_physics()) {
        carry = 0.f;
        tick(dt);
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }

    bool force_step = false;
    if (advance_paused(play)) {
        carry = 0.f;
        respawn_frames = 0;
        play->m_extraDelta = 0.0;
        if (find_best_queued) {
            find_best_queued = false;
            find_best_frame(Logic::as_play(play), tick);
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        if (!advance_queued) {
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        force_step = true;
    }

    float speed = render_active() ? 1.f : speedhack;
    float frame_dt = dt;
    bool rendering = render_capturing();
#ifdef GEODE_IS_MACOS
    suggest_sdl_pacing();
    if (!rendering) frame_dt = (float)refresh_frame_time(frame_dt);
#elif defined(GEODE_IS_WINDOWS)
    if (!rendering) frame_dt = (float)refresh_frame_time(frame_dt);
#endif
    perf_note_frame_dt(dt, frame_dt);
    if (!rendering && !real_time_mode && frame_dt > 0.25f) frame_dt = 0.25f;
    if (!rendering && lock_delta) {
        frame_dt = (float)CCDirector::sharedDirector()->getAnimationInterval();
    }
    frame_dt *= speed;
    if (float warp = play->m_gameState.m_timeWarp; warp > 1.f) frame_dt *= std::min(warp, 10.f);
    g_frame_speed = speed;

    auto died = [&] {
        auto* live = Logic::as_play(play);
        return live && live->m_playerDied;
    };
    auto tick_length = [&] {
        return physics_step() * std::min(play->m_gameState.m_timeWarp, 1.f);
    };
    // pause before kill. uses the last trajectory NOT a new predict
    auto pause_kill_safe_steps = [&]() -> int {
        if (!pause_before_kill || !is_recording() || rendering)
            return std::numeric_limits<int>::max();
        auto watch = [&](bool player2) {
            if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2)) return false;
            return records_player(player2);
        };
        if ((watch(false) && death_next_p1) || (watch(true) && death_next_p2)) return 0;
        bool have_traj = traj_dies_p1 || traj_dies_p2 || traj_alive_p1 > 0.f || traj_alive_p2 > 0.f;
        if (!have_traj) return std::numeric_limits<int>::max();
        float alive = std::numeric_limits<float>::infinity();
        if (watch(false)) alive = std::min(alive, traj_alive_p1);
        if (watch(true)) alive = std::min(alive, traj_alive_p2);
        float step_sec = tick_length();
        if (!std::isfinite(alive)) return std::numeric_limits<int>::max();
        int safe = (int)std::floor((double)(alive / step_sec) + 1e-9);
        return std::max(0, safe);
    };
    bool pause_kill = pause_before_kill && is_recording() && !rendering;
    // NOT while a macro plays. it made every step a whole update (75k tps = 1250 updates a frame)
    bool traj_click = traj_autoclicker && !rendering && !(is_playing() && !is_recording());
    auto lookahead = [&] {
        if (pause_kill && !show_traj) Trajectory::get().lookahead(PlayLayer::get());
    };
    auto autoclick = [&] {
        if (traj_click) Trajectory::get().autoclick(PlayLayer::get());
    };
    if (auto& traj = Trajectory::get(); traj.job_pending()) {
        bool plain = !force_step && !(frame_advance && !rendering) && !pause_kill && !traj_click;
        if (plain) {
            traj.refresh(Logic::as_play(play));
            present = true;
            return;
        }
        traj.cancel_job();
    }
    auto frame_began = Clock::now();
    if (!force_step) autoclick();
    if (!force_step && pause_kill) {
        auto* live = PlayLayer::get();
        lookahead();
        check_advance_pause(live);
        if (advance_paused(play) && !advance_queued) {
            Trajectory::get().refresh(live);
            return;
        }
    }
    bool can_batch = step_patched || sample_rate() == VANILLA_TPS;
    bool single_steps = frame_advance && !rendering;
    if (!force_step && !single_steps && !pause_kill && !traj_click && can_batch) {
        int steps = count_unlocked_steps(play, frame_dt);
        steps = std::min(steps, pause_kill_safe_steps());
        if (steps <= 0) {
            check_advance_pause(PlayLayer::get());
            present = true;
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        while (steps > 0) {
            if (died()) break;
            int batch = std::min(steps, steps_until_rate_change());
            float left = (float)(steps - batch) * physics_step();
            float step_dt = physics_step();
            steps_left = batch;
            steps_this_tick = (unsigned)batch;
            frame_steps += batch;
            defer_player_art = batch > 1 && phys::fast_live_pos();
            time_update([&] {
                play->update(step_dt * (float)batch);
            });
            defer_player_art = false;
            place_deferred_player_art(play);
            steps_left = 0;
            skip_physics = true;
            tick(step_dt * (float)batch);
            skip_physics = false;
            if (died() || batch == steps) break;
            double leftover = carry;
            carry = 0.f;
            steps = count_unlocked_steps(play, left + leftover);
            steps = std::min(steps, pause_kill_safe_steps());
        }
        settle_cost();
        frame_steps = 0;
        steps_left = 0;
        steps_this_tick = 0;
        present = true;
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }

    if (!force_step) {
        g_single_path = true;
        int steps = count_unlocked_steps(play, frame_dt);
        steps = std::min(steps, pause_kill_safe_steps());
        int ran = 0;
        if (steps <= 0) {
            check_advance_pause(PlayLayer::get());
            present = true;
            Trajectory::get().refresh(PlayLayer::get());
            return;
        }
        while (steps > 0) {
            if (died()) break;
            int batch = std::min(steps, steps_until_rate_change());
            float left = (float)(steps - batch) * physics_step();
            bool finished = false;
            for (int i = 0; i < batch; ++i) {
                if (died()) {
                    finished = true;
                    break;
                }
                // DONT resim mid frame
                if (traj_click && i > 0) autoclick();
                if (pause_kill && i > 0 && !traj_click) lookahead();
                if (pause_kill && pause_kill_safe_steps() <= 0) {
                    check_advance_pause(PlayLayer::get());
                    finished = true;
                    break;
                }
                int chunk = 1;
                float step_dt = physics_step();
                steps_left = chunk;
                steps_this_tick = (unsigned)chunk;
                frame_steps += chunk;
                defer_player_art = chunk > 1 && phys::fast_live_pos();
                time_update([&] {
                    play->update(step_dt * (float)chunk);
                });
                defer_player_art = false;
                place_deferred_player_art(play);
                steps_left = 0;
                skip_physics = true;
                tick(step_dt * (float)chunk);
                skip_physics = false;
                ran += chunk;
                i += chunk - 1;
                if (died()) {
                    finished = true;
                    break;
                }
                if (pause_kill) {
                    float step_sec = tick_length();
                    if (traj_alive_p1 > 0.f)
                        traj_alive_p1 = std::max(0.f, traj_alive_p1 - step_sec);
                    if (traj_alive_p2 > 0.f)
                        traj_alive_p2 = std::max(0.f, traj_alive_p2 - step_sec);
                    if (traj_alive_p1 <= 1e-6f && traj_dies_p1) death_next_p1 = true;
                    if (traj_alive_p2 <= 1e-6f && traj_dies_p2) death_next_p2 = true;
                    if (pause_kill_safe_steps() <= 0) {
                        check_advance_pause(PlayLayer::get());
                        finished = true;
                        break;
                    }
                }
            }
            if (finished || batch == steps) break;
            double leftover = carry;
            carry = 0.f;
            steps = count_unlocked_steps(play, left + leftover);
            steps = std::min(steps, pause_kill_safe_steps());
        }
        g_single_path = false;
        if (ran > 0) {
            double each = seconds_since(frame_began, Clock::now()) / ran;
            if (each > 0.0)
                g_single_cost = g_single_cost > 0.0 ? g_single_cost + (each - g_single_cost) * 0.3 : each;
        }
        settle_cost();
        frame_steps = 0;
        steps_left = 0;
        steps_this_tick = 0;
        present = true;
        Trajectory::get().refresh(PlayLayer::get());
        return;
    }

    int limit = std::max(advance_steps, 1);
    advance_queued = false;
    advance_steps = 1;
    carry = 0.f;
    play->m_extraDelta = 0.0;
    steps_left = 0;
    // held w/ nothing to check between steps it goes in batches like playing. 1 whole update a step never got near the speedhack
    bool batched = can_batch && !pause_kill && !traj_click && limit > 1;
    for (int taken = 0; taken < limit; ++taken) {
        if (died()) break;
        if (batched) {
            apply_rate(step_frame);
            float step_dt = physics_step();
            int batch = std::clamp(steps_until_rate_change(), 1, limit - taken);
            steps_left = batch;
            steps_this_tick = (unsigned)batch;
            frame_steps += batch;
            defer_player_art = batch > 1 && phys::fast_live_pos();
            time_update([&] {
                play->update(step_dt * (float)batch);
            });
            defer_player_art = false;
            place_deferred_player_art(play);
            steps_left = 0;
            skip_physics = true;
            tick(step_dt * (float)batch);
            skip_physics = false;
            steps_this_tick = 0;
            taken += batch - 1;
            continue;
        }
        // autoclicker must NOT add to a backstep replay
        if (backstep_target < 0) autoclick();
        if (pause_kill && holding_advance && backstep_target < 0) {
            if (!traj_click) Trajectory::get().lookahead(PlayLayer::get());
            check_advance_pause(PlayLayer::get());
            if (!holding_advance) break;
        }
        apply_rate(step_frame);
        float tick_dt = physics_step();
        steps_left = 1;
        steps_this_tick = 1;
        tick(tick_dt);
        steps_left = 0;
        steps_this_tick = 0;
    }
    frame_steps = 0;
    if (backstep_target >= 0) {
        finish_backstep(Logic::as_play(play));
        click_best_frame(Logic::as_play(play));
    }
    present = true;

    Trajectory::get().refresh(PlayLayer::get());
}

void Logic::stop_best_frame(const char* why) {
    bool was = best_frame_running;
    best_frame_running = false;
    best_frame_step_first = false;
    find_best_queued = false;
    best_frame_click_at = -1;
    if (was && why) {
        message = why;
    }
}

static bool lvl_moves_objects(PlayLayer* play) {
    if (!play->m_objects) return false;
    for (unsigned i = 0; i < play->m_objects->count(); ++i) {
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        if (!obj) continue;
        switch (obj->m_objectID) {
        case 901:
        case 1346:
        case 1347:
        case 1814:
        case 2067:
            return true;
        default:
            break;
        }
    }
    return false;
}

void Logic::request_best_frame() {
    if (best_frame_running) {
        stop_best_frame("Find Best Frame stopped");
        return;
    }
    auto* play = PlayLayer::get();
    if (!play || !play->m_started || play->m_playerDied || backstep_target >= 0 || render_capturing())
        return;
    if (!backstep) {
        message = "Turn on Backstep first";
        return;
    }
    if (!traj_moving_objects && play->m_level) {
        static std::unordered_set<int> asked;
        int id = play->m_level->m_levelID.value();
        if (asked.insert(id).second && lvl_moves_objects(play)) {
            ask_moving_objects = true;
            GUI::get().reveal_menu();
            return;
        }
    }
#ifdef GEODE_IS_MOBILE
    best_frame_click = false;
    best_frame_auto = false;
#endif
    frame_advance = true;
    holding_advance = false;
    advance_queued = false;
    find_best_queued = true;
    // keep going = keep clicking. w/ click that frame off there is nothing to keep going with, it just stops on the frame
    best_frame_running = best_frame_auto && best_frame_click;
    best_frame_step_first = best_frame_click && best_frame_clicked_at == step_frame && !play->m_queuedButtons.empty();
}

void Logic::find_best_frame(PlayLayer* play, const std::function<void(float)>& tick) {
    auto& trajectory = Trajectory::get();
    if (!play || play->m_playerDied || !trajectory.attached()) {
        stop_best_frame();
        return;
    }
    auto step = [&] {
        apply_rate(step_frame);
        float tick_dt = physics_step();
        steps_left = 1;
        steps_this_tick = 1;
        tick(tick_dt);
        steps_left = 0;
        steps_this_tick = 0;
        return !play->m_playerDied && !play->m_hasCompletedLevel;
    };
    if (std::exchange(best_frame_step_first, false) && !step()) {
        present = true;
        stop_best_frame(play->m_hasCompletedLevel ? "Level complete" : "Find Best Frame stopped");
        return;
    }
    bool player1 = !(is_recording() && !records_player(false) && records_player(true));
    if (plays_player(!player1)) {
        stop_best_frame();
        message = "The macro plays this player";
        return;
    }
    bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    int keep = (int)std::clamp(std::llround(sample_rate() * backstep_percent / 100.0), 1ll, 100000ll);
    int range = std::clamp(best_frame_range, 1, keep);
    int start = step_frame;

    struct Window {
        int from = -1, to = -1;
        int score = -1;
    };

    Window best;
    Window cur;
    int full = 0;
    for (int i = 0; i <= range; ++i) {
        int len = 0;
        int score = trajectory.flip_survival(play, player1, len);
        if (len <= 0) break;
        full = std::max(full, len);
        if (score == cur.score && cur.to == i - 1) cur.to = i;
        else
            cur = { i, i, score };
        if (cur.score > best.score || cur.from == best.from) best = cur;
        if (best.score >= full && cur.from != best.from) break;
        if (i == range) break;
        trajectory.lookahead(play);
        bool dies = player1 ? death_next_p1 || (!two_player && death_next_p2) : death_next_p2;
        if (dies || !step()) break;
    }
    present = true;
    if (best.score <= 0) {
        stop_best_frame();
        message = "No frame found";
        if (step_frame != start) step_back(play, step_frame - start);
        return;
    }
    int pick = start + best.from + (best.to - best.from) / 2;
    bool running = best_frame_running;
    best_frame_click_at = best_frame_click || running ? pick : -1;
    best_frame_click_p1 = player1;
    int back = step_frame - pick;
    if (back > 0 && !step_back(play, back)) {
        stop_best_frame();
        return;
    }
    message = best.score >= full
                        ? fmt::format("Best frame: +{} (window {} steps)", pick - start, best.to - best.from + 1)
                        : fmt::format("Best frame: +{} (lives {}/{} steps)", pick - start, best.score, full);
    if (running && best.score < full) {
        best_frame_running = false;
        message += ". Auto stopped: nothing survives the full trajectory";
    }
    if (backstep_target < 0) click_best_frame(play);
}

void Logic::click_best_frame(PlayLayer* play) {
    int at = std::exchange(best_frame_click_at, -1);
    if (!play || at < 0 || step_frame != at || play->m_playerDied) {
        if (at >= 0) stop_best_frame("Find Best Frame stopped");
        return;
    }
    bool player1 = best_frame_click_p1;
    bool held = Trajectory::get().held_next(play, player1);
    play->queueButton(JUMP, !held, !player1, click_stamp());
    best_frame_clicked_at = step_frame;
    best_hold_owned = true;
    best_hold = !held;
    best_hold_raw = raw_jump(play, !player1);
    reset_traj();
    if (best_frame_running && !best_frame_auto) best_frame_running = false;
    if (best_frame_running) {
        best_frame_step_first = true;
        find_best_queued = true;
    }
}

void Logic::cancel_steps() {
    steps_left = 0;
    carry = 0.f;
}

void Logic::flush_visual_refresh() {
    if (!refresh_queued) return;
    refresh_queued = false;
    refresh_only = true;
    CCDirector::sharedDirector()->getScheduler()->update(0.f);
    refresh_only = false;
}
