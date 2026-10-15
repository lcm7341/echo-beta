// stepping backwards while recording
//
// gds checkpoint misses things: held buttons, the jump buffer, orb state, per player clocks, the camera. those ride along in BackstepExtra
// stepping back = load the newest save at or before the target frame, then replay the recorded inputs forward to the exact frame
// only backstep_percent of a seconds worth of saves is kept, the oldest are dropped
// a save is checked before it is loaded (state_check). a broken one is leaked on purpose bc gds destructor would crash on it
#include "Logic/perf.hpp"
#include "Logic/logic.hpp"
#include "GUI/gui.hpp"
#include "Physics/checkpoint.hpp"
#include "Logic/state_check.hpp"
#include "Logic/replay_algorithms.hpp"
#include "Hooks/shown_camera.hpp"
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <limits>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <unordered_map>

namespace echo::step {
    void trace_note(std::string const& line);
}

namespace chrono = std::chrono;

using namespace geode::prelude;

// 2.2 STILL doesnt save held buttons or orb shiet ofc
struct UsedMark {
    EnhancedGameObject* object;
    bool p1, p2;
};

std::vector<UsedMark> used_marks(PlayLayer* play) {
    std::vector<UsedMark> out;
    auto* p1 = play ? play->m_player1 : nullptr;
    if (!p1)
        return out;
    auto& grid = play->m_nonEffectObjects;
    auto& sizes = play->m_nonEffectObjectsSizes;
    float factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
    int center = static_cast<int>(std::floor(p1->getPositionX() * factor));
    int last = static_cast<int>(std::min(grid.size(), sizes.size())) - 1;
    for (int x = std::max(center - 10, 0); x <= std::min(center + 10, last); ++x) {
        auto* column = grid[x];
        auto* counts = sizes[x];
        if (!column || !counts)
            continue;
        for (std::size_t y = 0; y < std::min(column->size(), counts->size()); ++y) {
            auto* bucket = (*column)[y];
            if (!bucket)
                continue;
            int amt = std::min<int>((*counts)[y], static_cast<int>(bucket->size()));
            for (int i = 0; i < amt; ++i) {
                auto* obj = (*bucket)[i];
                if (!obj)
                    continue;
                switch (obj->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Slope:
                case GameObjectType::Decoration:
                    continue;
                default:
                    break;
                }
                if (auto* enhanced = typeinfo_cast<EnhancedGameObject*>(obj))
                    out.push_back({ enhanced, enhanced->m_activatedByPlayer1, enhanced->m_activatedByPlayer2 });
            }
        }
    }
    return out;
}

namespace {
    struct CameraState {
        struct Range {
            std::size_t begin, end;
        };

        static constexpr std::array RANGES{
            Range{ offsetof(GJGameState, m_cameraZoom), offsetof(GJGameState, m_spawnChannelRelated0) },
            Range{ offsetof(GJGameState, m_cameraPosition), offsetof(GJGameState, m_tweenActions) },
            Range{ offsetof(GJGameState, m_cameraEdgeValue0), offsetof(GJGameState, m_gameObjectPhysics) },
            Range{ offsetof(GJGameState, m_unkUint10), offsetof(GJGameState, m_activatedObjectIDs) },
            Range{ offsetof(GJGameState, m_unkUint13), offsetof(GJGameState, m_stateObjects) },
            Range{ offsetof(GJGameState, m_dynamicRotateActions) + sizeof(GJGameState::m_dynamicRotateActions), offsetof(GJGameState, m_unkUMap8) },
        };
#ifdef GEODE_IS_WINDOWS
        static_assert(offsetof(GJGameState, m_cameraPosition) == 0x268 && offsetof(GJGameState, m_cameraStepDiff) == 0x334 &&
                      offsetof(GJGameState, m_unkUint13) == 0x358 && offsetof(GJGameState, m_unkUint17) == 0x5bc);
#endif
        static constexpr std::size_t TOTAL_SIZE = [] {
            std::size_t total = 0;
            for (auto const& range : RANGES)
                total += range.end - range.begin;
            return total;
        }();
        std::array<std::byte, TOTAL_SIZE> state{};
        float flip = 0.f, width_offset = 0.f, height_offset = 0.f, unzoomed_height_offset = 0.f, target_height_offset = 0.f;
        bool calculate_target_height_offset = false;
        CCPoint layer_position;
        float layer_scale_x = 1.f, layer_scale_y = 1.f, layer_rotation = 0.f;
        bool present = false;

        static CameraState of(PlayLayer* play) {
            CameraState out;
            auto const* base = reinterpret_cast<std::byte const*>(&play->m_gameState);
            std::size_t at = 0;
            for (auto const& range : RANGES) {
                std::memcpy(out.state.data() + at, base + range.begin, range.end - range.begin);
                at += range.end - range.begin;
            }
            out.flip = play->m_cameraFlip;
            out.width_offset = play->m_cameraWidthOffset;
            out.height_offset = play->m_cameraHeightOffset;
            out.unzoomed_height_offset = play->m_cameraUnzoomedHeightOffset;
            out.target_height_offset = play->m_targetCameraHeightOffset;
            out.calculate_target_height_offset = play->m_calculateTargetHeightOffset;
            if (auto* layer = play->m_objectLayer) {
                out.layer_position = layer->getPosition();
                out.layer_scale_x = layer->getScaleX();
                out.layer_scale_y = layer->getScaleY();
                out.layer_rotation = layer->getRotation();
            }
            out.present = true;
            return out;
        }

        void apply(PlayLayer* play) const {
            if (!present)
                return;
            auto* base = reinterpret_cast<std::byte*>(&play->m_gameState);
            std::size_t at = 0;
            for (auto const& range : RANGES) {
                std::memcpy(base + range.begin, state.data() + at, range.end - range.begin);
                at += range.end - range.begin;
            }
            play->m_cameraFlip = flip;
            play->m_cameraWidthOffset = width_offset;
            play->m_cameraHeightOffset = height_offset;
            play->m_cameraUnzoomedHeightOffset = unzoomed_height_offset;
            play->m_targetCameraHeightOffset = target_height_offset;
            play->m_calculateTargetHeightOffset = calculate_target_height_offset;
        }
    };

    std::optional<CameraState>& pinned_camera() {
        static std::optional<CameraState> pin;
        return pin;
    }

    bool pinned_this_frame = false;

    void show_camera(PlayLayer* play, CameraState const& state) {
        state.apply(play);
        play->updateCamera(0.f);
        state.apply(play);
        if (auto* layer = play->m_objectLayer) {
            layer->setPosition(state.layer_position);
            layer->setScaleX(state.layer_scale_x);
            layer->setScaleY(state.layer_scale_y);
            layer->setRotation(state.layer_rotation);
        }
    }

    void pin_camera(PlayLayer* play) {
        pinned_camera() = CameraState::of(play);
        pinned_this_frame = true;
    }
}

void Logic::note_shown_camera(PlayLayer* play, bool stepped) {
    if (!play || !Logic::get().backstep)
        return;
    if (std::exchange(pinned_this_frame, false)) {
        if (auto& pin = pinned_camera())
            show_camera(play, *pin);
        return;
    }
    if (stepped) {
        pinned_camera().reset();
        return;
    }
    if (auto& pin = pinned_camera())
        show_camera(play, *pin);
}

struct BackstepExtra {
    std::vector<UsedMark> used;
    SavedPlayerCheckpoint player1, player2;
    bool has_player2 = false;
    gd::unordered_map<int, int> persistent_items;
    gd::vector<GameObject*> calc_non_effect;
    int calc_non_effect_size = 0;
    CameraState camera;
    std::shared_ptr<ShownCamera> shown_camera;
};

namespace {
    struct Probe {
        CCPoint node1, node2;
        double yvel1 = 0.0, yvel2 = 0.0;
        bool has_player2 = false;
        bool operator==(Probe const&) const = default;
    };

    constexpr size_t RING = 1 << 17;
    std::vector<std::pair<int, Probe>> history;
    bool reported = false;
    bool check_restore = false;
    unsigned attempt_number = 1;

    Probe probe(PlayLayer* play) {
        Probe value;
        if (auto* body = play->m_player1) {
            value.node1 = body->getPosition();
            value.yvel1 = body->m_yVelocity;
        }
        if (auto* body = play->m_player2) {
            value.node2 = body->getPosition();
            value.yvel2 = body->m_yVelocity;
            value.has_player2 = true;
        }
        return value;
    }

    void compare(PlayLayer* play, int frame, bool replaying) {
        if (frame < 0)
            return;
        replaying = replaying || check_restore;
        check_restore = false;
        if (history.size() != RING)
            history.assign(RING, { -1, Probe{} });
        auto& slot = history[static_cast<size_t>(frame) % RING];
        auto now = probe(play);
        if (!replaying || slot.first != frame) {
            slot = { frame, now };
            return;
        }
        if (now == slot.second || reported)
            return;
        reported = true;
        auto const& was = slot.second;
        Logic::get().message = fmt::format("Step back replay drifted at frame {}, this part may not play back the same", frame);
        echo::step::trace_note(fmt::format("DRIFT frame={} p1 was={:.4f},{:.4f} yvel={:.4f} now={:.4f},{:.4f} yvel={:.4f}\n", frame, was.node1.x,
            was.node1.y, was.yvel1, now.node1.x, now.node1.y, now.yvel1));
    }
}

int Logic::backstep_interval() const {
    double rate = sample_rate();
    if (!std::isfinite(rate) || rate <= 60.0)
        return 1;
    double speed = frame_advance ? 1.0 : std::clamp(static_cast<double>(speedhack), 1.0, 1000.0);
    double per_second = speed > 1.0 ? 10.0 : 60.0;
    double gap = rate / per_second * speed;
    constexpr double SAVE_BUDGET = 0.05;
    if (std::isfinite(backstep_save_sec) && backstep_save_sec > 0.0)
        gap = std::max(gap, std::min(backstep_save_sec * rate * speed / SAVE_BUDGET, rate / 2.0 * speed));
    return std::max(1, static_cast<int>(std::lround(gap)));
}

void Logic::release_backstep(BackstepSave& save, char const* why) {
    if (!save.object)
        return;
    auto broken = save.broken ? std::string{ "flagged earlier" } : echo::state_check::broken(save.object->m_gameState);
    if (broken.empty()) {
        save.object->release();
    }
    else {
        // its broken so i leak it. gds destructor would crash
    }
    save.object = nullptr;
}

void Logic::clear_backsteps() {
    ++attempt_number;
    auto& logic = Logic::get();
    for (auto& slot : history)
        slot.first = -1;
    reported = false;
    for (auto& save : logic.backsteps)
        Logic::get().release_backstep(save, "clear");
    pinned_camera().reset();
    pinned_this_frame = false;
    logic.backsteps.clear();
    Logic::get().clear_backstep_objects();
    if (!logic.backstep_swapped)
        logic.backstep_inputs.clear();
}

void Logic::save_backstep(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!play || logic.predicting || logic.backstepping)
        return;
    bool resim = logic.backstep_target >= 0;
    if (!logic.backstep || Logic::get().render_capturing()) {
        if (!logic.backsteps.empty())
            Logic::get().clear_backsteps();
        return;
    }
    RunState mode = resim ? logic.backstep_state : logic.state;
    if (mode != logic.backstep_saved_state) {
        Logic::get().clear_backsteps();
        logic.backstep_saved_state = mode;
    }
    int frame = logic.step_frame;
    if (mode == RunState::Idle && !resim) {
        for (auto const& cmd : play->m_queuedButtons)
            logic.backstep_inputs.push_back({ frame + 1, static_cast<uint8_t>(cmd.m_button), cmd.m_isPush, cmd.m_isPlayer2 });
    }
    compare(play, frame, resim);
    bool shown = logic.save_shown_step;
    logic.save_shown_step = false;
    int close_by = std::max(1, static_cast<int>(std::lround(logic.sample_rate() / 60.0)));
    if (resim) {
        bool repeating = logic.holding_back && chrono::steady_clock::now() >= logic.back_hold_start + chrono::milliseconds(logic.advance_hold_ms);
        close_by = repeating ? 0 : std::min(close_by, 60);
    }
    bool close = resim && close_by > 0 && logic.backstep_target - frame <= close_by;
    bool dense = close || shown;
    int gap = Logic::get().backstep_interval();
    if (resim && !close)
        gap = std::max(close_by, static_cast<int>(std::lround(logic.sample_rate())));
    if (!dense && !logic.backsteps.empty() && frame - logic.backsteps.back().echo.frame < gap)
        return;
    if (!logic.backsteps.empty() && logic.backsteps.back().echo.frame >= frame)
        return;
    auto saving = chrono::steady_clock::now();
    echo::perf::Scope save_time(echo::perf::Backstep);
    echo::perf::count_backstep_save();
    struct Untimed {
        chrono::steady_clock::time_point from;
        ~Untimed() { Logic::get().note_untimed(chrono::duration<double>(chrono::steady_clock::now() - from).count()); }
    } untimed{ saving };
    // createCheckpoint must NOT change the object counters
    auto* obj = play->createCheckpoint();
    auto gd_done = chrono::steady_clock::now();
    if (!obj)
        return;
    obj->retain();
    auto extra = std::make_shared<BackstepExtra>();
    if (play->m_player1)
        extra->player1 = SavedPlayerCheckpoint::create(play->m_player1);
    if (play->m_player2) {
        extra->player2 = SavedPlayerCheckpoint::create(play->m_player2);
        extra->has_player2 = true;
    }
    if (play->m_effectManager)
        extra->persistent_items = play->m_effectManager->m_persistentItemCountMap;
    extra->calc_non_effect = play->m_calcNonEffectObjects;
    extra->calc_non_effect_size = play->m_calcNonEffectObjectsSize;
    extra->used = used_marks(play);
    extra->camera = CameraState::of(play);
    extra->shown_camera = echo_save_shown_camera(play);
    logic.backsteps.push_back({ obj, Logic::get().capture_checkpoint(play, obj, frame), std::move(extra), {} });
    auto extra_done = chrono::steady_clock::now();
    Logic::get().note_backstep_objects(play);
    auto objects_done = chrono::steady_clock::now();
    double rate = logic.sample_rate();
    if (!std::isfinite(rate) || rate <= 0.0)
        rate = 240.0;
    size_t keep = static_cast<size_t>(std::clamp(std::llround(rate * logic.backstep_percent / 100.0), 1ll, 100000ll));
    while (!resim && logic.backsteps.size() > keep) {
        Logic::get().release_backstep(logic.backsteps.front(), "trim");
        logic.backsteps.pop_front();
    }
    auto trim_done = chrono::steady_clock::now();
    auto ms = [](auto from, auto to) {
        return chrono::duration<double, std::milli>(to - from).count();
    };
    echo::perf::note_save_parts(ms(saving, gd_done), ms(gd_done, extra_done), ms(extra_done, objects_done), ms(objects_done, trim_done));
    double took = chrono::duration<double>(chrono::steady_clock::now() - saving).count();
    if (std::isfinite(took) && took > 0.0)
        logic.backstep_save_sec = logic.backstep_save_sec > 0.0 ? logic.backstep_save_sec + (took - logic.backstep_save_sec) * 0.2 : took;
}

void Logic::trim_backsteps(PlayLayer* play) {
    ++attempt_number;
    pinned_camera().reset();
    pinned_this_frame = false;
    auto& logic = Logic::get();
    if (logic.backsteps.empty())
        return;
    if (!play || !Logic::get().get_latest_checkpoint(play)) {
        Logic::get().clear_backsteps();
        return;
    }
    while (!logic.backsteps.empty() && logic.backsteps.back().echo.frame > logic.step_frame)
        Logic::get().pop_backstep();
    std::erase_if(logic.backstep_inputs, [&](Frame const& input) {
        return input.number > logic.step_frame;
    });
}

bool Logic::step_back(PlayLayer* play, int steps) {
    if (steps <= 0)
        return false;
    return Logic::get().step_to(play, std::max(0, Logic::get().step_frame - steps), std::numeric_limits<int>::max());
}

bool Logic::step_to(PlayLayer* play, int target, int keep_up_to) {
    auto& logic = Logic::get();
    if (!play || !play->m_checkpointArray || target < 0) {
        return false;
    }
    if (!logic.backstep || logic.backstep_target >= 0 || logic.backstepping || Logic::get().render_capturing() ||
        play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
        if (!logic.backstep)
            logic.message = "Turn on Backstep first";
        return false;
    }
    if (target <= logic.best_frame_clicked_at)
        logic.best_hold_owned = false;
    int keep = std::min(target, keep_up_to);
    while (!logic.backsteps.empty() && logic.backsteps.back().echo.frame > keep)
        Logic::get().pop_backstep();
    if (logic.backsteps.empty()) {
        logic.message = "Nothing saved to step back to";
        return false;
    }
    // DONT load a broken save
    while (!logic.backsteps.empty() && (logic.backsteps.back().broken || !echo::state_check::broken(logic.backsteps.back().object->m_gameState).empty())) {
        logic.backsteps.back().broken = true;
        Logic::get().pop_backstep();
    }
    if (logic.backsteps.empty()) {
        logic.message = "Nothing saved to step back to";
        return false;
    }
    auto save = logic.backsteps.back();
    bool resim = save.echo.frame < target;
    echo::step::trace_note(fmt::format("BACK from={} target={} save={} resim={} state={} inputs={} queued={}\n", logic.step_frame, target, save.echo.frame, resim,
        static_cast<int>(logic.state), logic.inputs.size(), play->m_queuedButtons.size()));
    if (logic.state == RunState::Idle && !logic.backstep_swapped) {
        replay::compact(logic.backstep_inputs, logic.backstep_poses, logic.backstep_ids, [target](Frame const& input) {
            return input.number <= target;
        });
    }
    reported = false;
    check_restore = true;
    logic.backstep_state = logic.state;
    logic.backstep_play_p1 = logic.play_p1;
    logic.backstep_play_p2 = logic.play_p2;
    if (resim) {
        logic.state = RunState::Playing;
        logic.play_p1 = logic.play_p2 = true;
        if (logic.backstep_state == RunState::Idle) {
            logic.backstep_replay_pos = logic.replay_pos;
            std::swap(logic.inputs, logic.backstep_inputs);
            std::swap(logic.poses, logic.backstep_poses);
            std::swap(logic.input_ids, logic.backstep_ids);
            logic.backstep_swapped = true;
            logic.touch_inputs();
        }
    }
    logic.backstepping = true;
    play->m_checkpointArray->addObject(save.object);
    logic.checkpoints.push_back(save.echo);
    bool music_sync = play->m_practiceMusicSync;
    play->m_practiceMusicSync = true;
    play->resetLevel();
    play->m_practiceMusicSync = music_sync;
    play->m_checkpointArray->removeObject(save.object, true);
    Logic::get().sync_checkpoints(play);
    while (auto* last = play->getLastCheckpoint()) {
        auto it = std::ranges::find(logic.checkpoints, last, &Checkpoint::object);
        if (it == logic.checkpoints.end() || it->frame <= keep)
            break;
        auto amt = play->m_checkpointArray->count();
        play->removeCheckpoint(false);
        if (play->m_checkpointArray->count() >= amt)
            break;
    }
    logic.object_capture = nullptr;
    if (auto* extra = save.extra.get()) {
        auto put = [](PlayerObject* body, SavedPlayerCheckpoint& shot) {
            if (!body)
                return;
            auto logs = std::array{ body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight };
            auto* wave = body->m_waveTrail;
            auto* pending = body->m_pendingCheckpoint;
            auto* regular = body->m_regularTrail;
            auto* ghost = body->m_ghostTrail;
            shot.apply(body);
            body->m_collisionLogTop = logs[0];
            body->m_collisionLogBottom = logs[1];
            body->m_collisionLogLeft = logs[2];
            body->m_collisionLogRight = logs[3];
            body->m_waveTrail = wave;
            body->m_regularTrail = regular;
            body->m_ghostTrail = ghost;
            body->m_pendingCheckpoint = pending;
        };
        put(play->m_player1, extra->player1);
        if (extra->has_player2)
            put(play->m_player2, extra->player2);
        Logic::get().apply_player(play->m_player1, save.echo.player1);
        Logic::get().apply_player(play->m_player2, save.echo.player2);
        if (play->m_effectManager)
            play->m_effectManager->m_persistentItemCountMap = extra->persistent_items;
        play->m_calcNonEffectObjects = extra->calc_non_effect;
        play->m_calcNonEffectObjectsSize = extra->calc_non_effect_size;
        for (auto const& mark : extra->used) {
            if (logic.trace_logging && (mark.object->m_activatedByPlayer1 != mark.p1 || mark.object->m_activatedByPlayer2 != mark.p2))
                echo::step::trace_note(fmt::format("UNUSE save={} uid={} id={} now={}{} saved={}{}\n", save.echo.frame, mark.object->m_uniqueID, mark.object->m_objectID,
                    mark.object->m_activatedByPlayer1 ? 1 : 0, mark.object->m_activatedByPlayer2 ? 1 : 0, mark.p1 ? 1 : 0, mark.p2 ? 1 : 0));
            mark.object->m_activatedByPlayer1 = mark.p1;
            mark.object->m_activatedByPlayer2 = mark.p2;
        }
    }
    play->m_queuedButtons.clear();
    Logic::get().load_backstep_objects(save.echo.frame);
    pinned_camera().reset();
    pinned_this_frame = false;
    if (auto* extra = save.extra.get(); extra && extra->camera.present) {
        auto& tweens = play->m_gameState.m_tweenActions;
        auto const& saved = save.object->m_gameState.m_tweenActions;
        bool same = tweens.size() == saved.size();
        for (auto const& entry : saved)
            same = same && tweens.find(entry.first) != tweens.end();
        if (!same)
            tweens = saved;
        else
            for (auto const& [key, tween] : saved)
                tweens.find(key)->second = tween;
        show_camera(play, extra->camera);
        if (!resim)
            pin_camera(play);
    }
    echo_load_shown_camera(play, save.extra ? save.extra->shown_camera.get() : nullptr);
    logic.backstepping = false;
    logic.frame_advance = true;
    logic.holding_advance = false;
    if (resim) {
        logic.backstep_target = target;
        logic.advance_steps = target - save.echo.frame;
        logic.advance_queued = true;
    }
    else {
        logic.advance_queued = false;
        Logic::get().match_live_holds(play);
        Logic::get().draw_overlay(play);
    }
    return true;
}

void Logic::finish_backstep(PlayLayer* play) {
    auto& logic = Logic::get();
    if (logic.backstep_target < 0)
        return;
    if (play && logic.step_frame == logic.backstep_target && !play->m_playerDied)
        pin_camera(play);
    logic.backstep_target = -1;
    logic.state = logic.backstep_state;
    logic.play_p1 = logic.backstep_play_p1;
    logic.play_p2 = logic.backstep_play_p2;
    logic.advance_queued = false;
    logic.advance_steps = 1;
    int frame = logic.step_frame;
    echo::step::trace_note(fmt::format("BACKDONE frame={} state={} inputs={} dead={}\n", frame, static_cast<int>(logic.state), logic.inputs.size(), play && play->m_playerDied));
    if (logic.backstep_swapped) {
        std::erase_if(logic.inputs, [frame](Frame const& input) {
            return input.number > frame;
        });
        std::swap(logic.inputs, logic.backstep_inputs);
        std::swap(logic.poses, logic.backstep_poses);
        std::swap(logic.input_ids, logic.backstep_ids);
        logic.backstep_swapped = false;
        logic.touch_inputs();
        logic.replay_pos = logic.backstep_replay_pos;
        Logic::get().match_live_holds(play);
        return;
    }
    if (!play)
        return;
    if (logic.is_recording()) {
        Logic::get().trim_recorded_inputs(logic, frame);
        if (!logic.is_playing())
            Logic::get().trim_rates(frame);
        Logic::get().sort_inputs();
    }
    if (logic.is_playing())
        Logic::get().set_replay_pos(play);
    else
        logic.replay_pos = logic.inputs.size();
    Logic::get().match_live_holds(play);
}

void Logic::poll_back_hold() {
    auto& logic = Logic::get();
    auto* play = PlayLayer::get();
    if (logic.back_tap_queued) {
        if (!play)
            logic.back_tap_queued = false;
        else if (logic.backstep_target < 0) {
            logic.back_tap_queued = false;
            Logic::get().step_back(play, 1);
            return;
        }
    }
    if (!logic.holding_back || !play)
        return;
    if (GUI::get().typing()) {
        logic.holding_back = false;
        return;
    }
    if (logic.backstep_target >= 0)
        return;
    auto now = chrono::steady_clock::now();
    auto start = logic.back_hold_start + chrono::milliseconds(logic.advance_hold_ms);
    if (now < start) {
        logic.back_owed = 0.0;
        return;
    }
    double rate = logic.sample_rate() * std::clamp(logic.advance_speed, 1.f, 200.f) / 100.0 * logic.speedhack;
    if (!std::isfinite(rate) || !(rate > 0.0))
        return;
    double interval = CCDirector::sharedDirector() ? CCDirector::sharedDirector()->getAnimationInterval() : 1.0 / 60.0;
    if (!std::isfinite(interval) || interval <= 0.0)
        interval = 1.0 / 60.0;
    logic.back_owed += rate * interval;
    double wanted = std::floor(logic.back_owed);
    if (wanted < 1.0)
        return;
    int steps = static_cast<int>(std::min(wanted, 1e6));
    logic.back_owed = std::min(logic.back_owed - steps, 1.0);
    logic.last_back_step = now;
    if (!Logic::get().step_back(play, steps))
        logic.holding_back = false;
}

#ifdef GEODE_IS_WINDOWS
static_assert(offsetof(PlayLayer, m_checkpointArray) == 0x3888);
#endif

class $modify(BackstepPlayLayer, PlayLayer) {
    // stepping back is NOT a new attempt
    void updateAttempts() {
        if (Logic::get().backstepping)
            return;
        PlayLayer::updateAttempts();
    }
};
