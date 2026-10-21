#include "Logic/perf.hpp"
#include "Logic/logic.hpp"
#include <Geode/binding/HardStreak.hpp>
#include "GUI/gui.hpp"
#include "Physics/checkpoint.hpp"
#include "Logic/state_check.hpp"
#include "Hooks/step.hpp"
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

namespace chrono = std::chrono;

using namespace geode::prelude;

// 2.2 STILL doesnt save held buttons or orb shiet ofc
struct UsedObject {
    EnhancedGameObject* object;
    bool p1, p2;
};

std::vector<UsedObject> used_objects(PlayLayer* play) {
    std::vector<UsedObject> out;
    auto* p1 = play ? play->m_player1 : nullptr;
    if (!p1) return out;
    auto& grid = play->m_nonEffectObjects;
    auto& sizes = play->m_nonEffectObjectsSizes;
    float factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
    int center = (int)std::floor(p1->getPositionX() * factor);
    int last = (int)std::min(grid.size(), sizes.size()) - 1;
    for (int x = std::max(center - 10, 0); x <= std::min(center + 10, last); ++x) {
        auto* column = grid[x];
        auto* counts = sizes[x];
        if (!column || !counts) continue;
        for (std::size_t y = 0; y < std::min(column->size(), counts->size()); ++y) {
            auto* bucket = (*column)[y];
            if (!bucket) continue;
            int amt = std::min<int>((*counts)[y], (int)bucket->size());
            for (int i = 0; i < amt; ++i) {
                auto* obj = (*bucket)[i];
                if (!obj) continue;
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
        for (const auto& range : RANGES)
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
        const auto* base = reinterpret_cast<const std::byte*>(&play->m_gameState);
        std::size_t at = 0;
        for (const auto& range : RANGES) {
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
        if (!present) return;
        auto* base = reinterpret_cast<std::byte*>(&play->m_gameState);
        std::size_t at = 0;
        for (const auto& range : RANGES) {
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

static std::optional<CameraState> g_pinned_camera;

static bool pinned_this_frame = false;

static void show_camera(PlayLayer* play, const CameraState& state) {
    state.apply(play);
    update_camera_keeping_nodes(play);
    state.apply(play);
    if (auto* layer = play->m_objectLayer) {
        layer->setPosition(state.layer_position);
        layer->setScaleX(state.layer_scale_x);
        layer->setScaleY(state.layer_scale_y);
        layer->setRotation(state.layer_rotation);
        play->updateExtraGameLayers();
    }
}

static void pin_camera(PlayLayer* play) {
    g_pinned_camera = CameraState::of(play);
    pinned_this_frame = true;
}

void Logic::note_shown_camera(PlayLayer* play, bool stepped) {
    if (!play || !backstep) return;
    if (std::exchange(pinned_this_frame, false)) {
        if (auto& pin = g_pinned_camera) show_camera(play, *pin);
        return;
    }
    if (stepped) {
        g_pinned_camera.reset();
        return;
    }
    if (auto& pin = g_pinned_camera) show_camera(play, *pin);
}

struct BackstepGdData {
    std::vector<UsedObject> used;
    SavedPlayerCheckpoint player1, player2;
    bool has_player2 = false;
    gd::unordered_map<int, int> persistent_items;
    gd::vector<GameObject*> calc_non_effect;
    int calc_non_effect_size = 0;
    CameraState camera;
    std::shared_ptr<ShownCamera> shown_camera;
};

struct DriftProbe {
    CCPoint player1_position, player2_position;
    double player1_y_velocity = 0.0, player2_y_velocity = 0.0;
    bool has_player2 = false;
    bool operator==(const DriftProbe&) const = default;
};

const size_t RING = 1 << 17;
static std::vector<std::pair<int, DriftProbe>> g_drift_history;
static bool g_drift_reported = false;
static bool g_check_drift = false;
static unsigned attempt_number = 1;

static DriftProbe take_drift_probe(PlayLayer* play) {
    DriftProbe value;
    if (auto* body = play->m_player1) {
        value.player1_position = body->getPosition();
        value.player1_y_velocity = body->m_yVelocity;
    }
    if (auto* body = play->m_player2) {
        value.player2_position = body->getPosition();
        value.player2_y_velocity = body->m_yVelocity;
        value.has_player2 = true;
    }
    return value;
}

static void check_drift(PlayLayer* play, int frame, bool replaying) {
    if (frame < 0) return;
    replaying = replaying || g_check_drift;
    g_check_drift = false;
    if (g_drift_history.size() != RING)
        g_drift_history.assign(RING, { -1, DriftProbe{} });
    auto& slot = g_drift_history[(size_t)frame % RING];
    auto now = take_drift_probe(play);
    if (!replaying || slot.first != frame) {
        slot = { frame, now };
        return;
    }
    if (now == slot.second || g_drift_reported) return;
    g_drift_reported = true;
    const auto& was = slot.second;
    Logic::get().message = fmt::format("Step back replay drifted at frame {}, this part may not play back the same", frame);
    trace_note(fmt::format("DRIFT frame={} p1 was={:.4f},{:.4f} yvel={:.4f} now={:.4f},{:.4f} yvel={:.4f}\n", frame, was.player1_position.x,
        was.player1_position.y, was.player1_y_velocity, now.player1_position.x, now.player1_position.y, now.player1_y_velocity));
}

int Logic::backstep_interval() const {
    double rate = sample_rate();
    if (rate <= 60.0) return 1;
    double speed = frame_advance ? 1.0 : std::clamp((double)speedhack, 1.0, 1000.0);
    double per_second = speed > 1.0 ? 10.0 : 60.0;
    double gap = rate / per_second * speed;
    const double SAVE_BUDGET = 0.05;
    if (backstep_save_sec > 0.0)
        gap = std::max(gap, std::min(backstep_save_sec * rate * speed / SAVE_BUDGET, rate / 2.0 * speed));
    return std::max(1, (int)std::lround(gap));
}

void Logic::release_backstep(BackstepSave& save) {
    if (!save.object) return;
    // its broken so i leak it. gds destructor would crash
    if (!save.broken && game_state_intact(save.object->m_gameState)) save.object->release();
    save.object = nullptr;
}

void Logic::clear_backsteps() {
    ++attempt_number;
    if (!backstep) g_drift_history = std::vector<std::pair<int, DriftProbe>>();
    for (auto& slot : g_drift_history)
        slot.first = -1;
    g_drift_reported = false;
    g_check_drift = false;
    for (auto& save : backsteps)
        release_backstep(save);
    g_pinned_camera.reset();
    pinned_this_frame = false;
    backsteps.clear();
    clear_backstep_objects();
    if (!backstep_swapped) backstep_inputs.clear();
}

struct UntimedScope {
    chrono::steady_clock::time_point from;
    ~UntimedScope() { Logic::get().note_untimed(chrono::duration<double>(chrono::steady_clock::now() - from).count()); }
};

static std::shared_ptr<BackstepGdData> capture_gd_data(PlayLayer* play) {
    auto extra = std::make_shared<BackstepGdData>();
    if (play->m_player1) extra->player1 = SavedPlayerCheckpoint::create(play->m_player1);
    if (play->m_player2) {
        extra->player2 = SavedPlayerCheckpoint::create(play->m_player2);
        extra->has_player2 = true;
    }
    if (play->m_effectManager)
        extra->persistent_items = play->m_effectManager->m_persistentItemCountMap;
    extra->calc_non_effect = play->m_calcNonEffectObjects;
    extra->calc_non_effect_size = play->m_calcNonEffectObjectsSize;
    extra->used = used_objects(play);
    extra->camera = CameraState::of(play);
    extra->shown_camera = save_shown_camera(play);
    return extra;
}

static bool on_held_ladder(int ahead, int step) {
    for (int distance = step; distance <= ahead; distance *= 2)
        if (distance == ahead) return true;
    return false;
}

void Logic::save_backstep(PlayLayer* play) {
    if (!play || predicting || backstepping) return;
    bool resim = backstep_target >= 0;
    if (!backstep || render_capturing()) {
        if (!backsteps.empty()) clear_backsteps();
        return;
    }
    RunState mode = resim ? backstep_state : state;
    if (mode != backstep_saved_state) {
        clear_backsteps();
        backstep_saved_state = mode;
    }
    int frame = step_frame;
    check_drift(play, frame, resim);
    bool shown = save_shown_step;
    save_shown_step = false;
    int close_by = std::max(1, (int)std::lround(sample_rate() / 60.0));
    bool repeating = resim && holding_back && chrono::steady_clock::now() >= back_hold_start + chrono::milliseconds(advance_hold_ms);
    if (resim) close_by = repeating ? 0 : std::min(close_by, 60);
    bool close = resim && close_by > 0 && backstep_target - frame <= close_by;
    bool dense = close || shown;
    int gap = backstep_interval();
    if (resim && !close) gap = std::max(close_by, (int)std::lround(sample_rate()));
    if (repeating && !shown && !on_held_ladder(backstep_target - frame, held_step)) return;
    if (!dense && !repeating && !backsteps.empty() && frame - backsteps.back().checkpoint.frame < gap) return;
    if (!backsteps.empty() && backsteps.back().checkpoint.frame >= frame) return;
    auto saving = chrono::steady_clock::now();
    PerfScope save_time(PERF_BACKSTEP);
    perf_count_backstep_save();
    UntimedScope untimed{ saving };
    // createCheckpoint must NOT change the object counters
    auto* obj = play->createCheckpoint();
    auto gd_done = chrono::steady_clock::now();
    if (!obj) return;
    obj->retain();
    auto extra = capture_gd_data(play);
    backsteps.push_back({ obj, capture_checkpoint(play, obj, frame), std::move(extra), {} });
    auto extra_done = chrono::steady_clock::now();
    note_backstep_objects(play);
    auto objects_done = chrono::steady_clock::now();
    double rate = sample_rate();
    size_t keep = (size_t)std::clamp(std::llround(rate * backstep_percent / 100.0), 1ll, 100000ll);
    while (!resim && backsteps.size() > keep) {
        release_backstep(backsteps.front());
        backsteps.pop_front();
    }
    auto trim_done = chrono::steady_clock::now();
    auto ms = [](auto from, auto to) {
        return chrono::duration<double, std::milli>(to - from).count();
    };
    perf_note_save_parts(ms(saving, gd_done), ms(gd_done, extra_done), ms(extra_done, objects_done), ms(objects_done, trim_done));
    double took = chrono::duration<double>(chrono::steady_clock::now() - saving).count();
    if (took > 0.0 && backsteps.size() > 1)
        backstep_save_sec = backstep_save_sec > 0.0 ? backstep_save_sec + (took - backstep_save_sec) * 0.2 : took;
}

void Logic::note_idle_input(bool down, bool player1, int button) {
    if (!backstep || state != RunState::Idle || backstepping || resetting || refreshing_visuals || render_capturing())
        return;
    if (button != 0 && button != JUMP && button != LEFT && button != RIGHT) return;
    backstep_inputs.push_back({ step_frame + 1, (unsigned char)(button == 0 ? JUMP : button), down, !player1 });
}

void Logic::trim_backsteps(PlayLayer* play) {
    ++attempt_number;
    g_check_drift = false;
    g_pinned_camera.reset();
    pinned_this_frame = false;
    if (backsteps.empty()) return;
    if (!play || !get_latest_checkpoint(play)) {
        clear_backsteps();
        return;
    }
    while (!backsteps.empty() && backsteps.back().checkpoint.frame > step_frame)
        pop_backstep();
    std::erase_if(backstep_inputs, [&](const Frame& input) {
        return input.number > step_frame;
    });
}

bool Logic::step_back(PlayLayer* play, int steps) {
    if (steps <= 0) return false;
    return step_to(play, std::max(0, step_frame - steps), std::numeric_limits<int>::max());
}

bool Logic::step_to(PlayLayer* play, int target, int keep_up_to) {
    if (!play || !play->m_checkpointArray || target < 0) {
        return false;
    }
    if (!backstep || backstep_target >= 0 || backstepping || render_capturing() ||
        play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
        if (!backstep) message = "Turn on Backstep first";
        return false;
    }
    if (target <= best_frame_clicked_at) best_hold_owned = false;
    int keep = std::min(target, keep_up_to);
    while (!backsteps.empty() && backsteps.back().checkpoint.frame > keep)
        pop_backstep();
    if (backsteps.empty()) {
        message = "Nothing saved to step back to";
        return false;
    }
    // DONT load a broken save
    while (!backsteps.empty() && (backsteps.back().broken || !game_state_intact(backsteps.back().object->m_gameState))) {
        backsteps.back().broken = true;
        pop_backstep();
    }
    if (backsteps.empty()) {
        message = "Nothing saved to step back to";
        return false;
    }
    auto save = backsteps.back();
    bool resim = save.checkpoint.frame < target;
    held_step = std::max(1, step_frame - target);
    trace_note(fmt::format("BACK from={} target={} save={} resim={} state={} inputs={} queued={}\n", step_frame, target, save.checkpoint.frame, resim,
        (int)state, inputs.size(), play->m_queuedButtons.size()));
    if (state == RunState::Idle && !backstep_swapped) {
        compact_inputs(backstep_inputs, backstep_poses, backstep_ids, [target](const Frame& input) {
            return input.number <= target;
        });
    }
    g_drift_reported = false;
    g_check_drift = true;
    backstep_state = state;
    backstep_play_p1 = play_p1;
    backstep_play_p2 = play_p2;
    if (resim) {
        state = RunState::Playing;
        play_p1 = play_p2 = true;
        if (backstep_state == RunState::Idle) {
            backstep_replay_pos = replay_pos;
            std::swap(inputs, backstep_inputs);
            std::swap(poses, backstep_poses);
            std::swap(input_ids, backstep_ids);
            backstep_swapped = true;
            mark_inputs_changed();
        }
    }
    backstepping = true;
    play->m_checkpointArray->addObject(save.object);
    checkpoints.push_back(save.checkpoint);
    bool music_sync = play->m_practiceMusicSync;
    bool held = holding_back && chrono::steady_clock::now() >= back_hold_start + chrono::milliseconds(advance_hold_ms);
    play->m_practiceMusicSync = music_sync && !held;
    play->resetLevel();
    play->m_practiceMusicSync = music_sync;
    play->m_checkpointArray->removeObject(save.object, true);
    sync_checkpoints(play);
    while (auto* last = play->getLastCheckpoint()) {
        auto it = std::ranges::find(checkpoints, last, &Checkpoint::object);
        if (it == checkpoints.end() || it->frame <= keep) break;
        auto amt = play->m_checkpointArray->count();
        play->removeCheckpoint(false);
        if (play->m_checkpointArray->count() >= amt) break;
    }
    object_capture = nullptr;
    if (auto* extra = save.extra.get()) {
        auto put = [](PlayerObject* body, SavedPlayerCheckpoint& shot) {
            if (!body) return;
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
        if (extra->has_player2) put(play->m_player2, extra->player2);
        apply_player(play->m_player1, save.checkpoint.player1);
        apply_player(play->m_player2, save.checkpoint.player2);
        if (play->m_effectManager)
            play->m_effectManager->m_persistentItemCountMap = extra->persistent_items;
        play->m_calcNonEffectObjects = extra->calc_non_effect;
        play->m_calcNonEffectObjectsSize = extra->calc_non_effect_size;
        for (const auto& mark : extra->used) {
            if (trace_logging && (mark.object->m_activatedByPlayer1 != mark.p1 || mark.object->m_activatedByPlayer2 != mark.p2))
                trace_note(fmt::format("UNUSE save={} uid={} id={} now={}{} saved={}{}\n", save.checkpoint.frame, mark.object->m_uniqueID, mark.object->m_objectID,
                    mark.object->m_activatedByPlayer1 ? 1 : 0, mark.object->m_activatedByPlayer2 ? 1 : 0, mark.p1 ? 1 : 0, mark.p2 ? 1 : 0));
            mark.object->m_activatedByPlayer1 = mark.p1;
            mark.object->m_activatedByPlayer2 = mark.p2;
        }
    }
    play->m_queuedButtons.clear();
    load_backstep_objects(save.checkpoint.frame);
    g_pinned_camera.reset();
    pinned_this_frame = false;
    if (auto* extra = save.extra.get(); extra && extra->camera.present) {
        auto& tweens = play->m_gameState.m_tweenActions;
        const auto& saved = save.object->m_gameState.m_tweenActions;
        bool same = tweens.size() == saved.size();
        for (const auto& entry : saved)
            same = same && tweens.find(entry.first) != tweens.end();
        if (!same) tweens = saved;
        else
            for (const auto& [key, tween] : saved)
                tweens.find(key)->second = tween;
        show_camera(play, extra->camera);
        if (!resim) pin_camera(play);
    }
    load_shown_camera(play, save.extra ? save.extra->shown_camera.get() : nullptr);
    backstepping = false;
    frame_advance = true;
    holding_advance = false;
    if (resim) {
        backstep_target = target;
        advance_steps = target - save.checkpoint.frame;
        advance_queued = true;
    }
    else {
        advance_queued = false;
        match_live_holds(play);
        draw_overlay(play);
    }
    return true;
}

void Logic::finish_backstep(PlayLayer* play) {
    if (backstep_target < 0) return;
    if (play) {
        for (auto* body : { play->m_player1, play->m_player2 })
            if (body && body->m_waveTrail) body->m_waveTrail->reset();
    }
    if (play && step_frame == backstep_target && !play->m_playerDied) pin_camera(play);
    backstep_target = -1;
    state = backstep_state;
    play_p1 = backstep_play_p1;
    play_p2 = backstep_play_p2;
    advance_queued = false;
    advance_steps = 1;
    int frame = step_frame;
    trace_note(fmt::format("BACKDONE frame={} state={} inputs={} dead={}\n", frame, (int)state, inputs.size(), play && play->m_playerDied));
    if (backstep_swapped) {
        std::erase_if(inputs, [frame](const Frame& input) {
            return input.number > frame;
        });
        std::swap(inputs, backstep_inputs);
        std::swap(poses, backstep_poses);
        std::swap(input_ids, backstep_ids);
        backstep_swapped = false;
        mark_inputs_changed();
        replay_pos = backstep_replay_pos;
        match_live_holds(play);
        return;
    }
    if (!play) return;
    if (is_recording()) {
        trim_recorded_inputs(frame);
        if (!is_playing()) trim_rates(frame);
        sort_inputs();
    }
    if (is_playing()) set_replay_pos(play);
    else replay_pos = inputs.size();
    match_live_holds(play);
}

void Logic::poll_back_hold() {
    auto* play = PlayLayer::get();
    if (back_tap_queued) {
        if (!play) back_tap_queued = false;
        else if (backstep_target < 0) {
            back_tap_queued = false;
            step_back(play, 1);
            return;
        }
    }
    if (!holding_back || !play) return;
    if (GUI::get().typing()) {
        holding_back = false;
        return;
    }
    if (backstep_target >= 0) return;
    auto now = chrono::steady_clock::now();
    auto start = back_hold_start + chrono::milliseconds(advance_hold_ms);
    if (now < start) {
        back_steps_owed = 0.0;
        last_back_step = start;
        return;
    }
    int steps = owed_hold_steps(back_steps_owed, last_back_step, 1e6);
    if (steps == 0) return;
    if (!step_back(play, steps)) holding_back = false;
}

#ifdef GEODE_IS_WINDOWS
static_assert(offsetof(PlayLayer, m_checkpointArray) == 0x3888);
#endif

class $modify(BackstepPlayLayer, PlayLayer) {
    // stepping back is NOT a new attempt
    void updateAttempts() {
        if (Logic::get().backstepping) return;
        PlayLayer::updateAttempts();
    }
};
