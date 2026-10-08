// stepping backwards while recording
//
// every few steps the whole level is saved w/ gds own checkpoint (every step at 240 tps, less often above that, a save can cost 1ms)
// gds checkpoint misses things: held buttons, the jump buffer, orb state, per player clocks, the camera. those ride along in BackstepInfo
#include "Logic/logic.hpp"
#include "GUI/gui.hpp"
#include "Physics/checkpoint.hpp"
#include "Logic/state_check.hpp"
#include "Logic/replay_algorithms.hpp"
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
struct UsedMark {
    EnhancedGameObject* object;
    bool p1, p2;
};

std::vector<UsedMark> used_marks(PlayLayer* play) {
    std::vector<UsedMark> out;
    auto* p1 = play ? play->m_player1 : nullptr;
    if (!p1) {
        return out;
    }
    auto& grid = play->m_nonEffectObjects;
    auto& sizes = play->m_nonEffectObjectsSizes;
    float factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
    int center = static_cast<int>(std::floor(p1->getPositionX() * factor));
    int last = static_cast<int>(std::min(grid.size(), sizes.size())) - 1;
    for (int x = std::max(center - 10, 0); x <= std::min(center + 10, last); ++x) {
        auto* column = grid[x];
        auto* counts = sizes[x];
        if (!column || !counts) {
            continue;
        }
        for (std::size_t y = 0; y < std::min(column->size(), counts->size()); ++y) {
            auto* bucket = (*column)[y];
            if (!bucket) {
                continue;
            }
            int amt = std::min<int>((*counts)[y], static_cast<int>(bucket->size()));
            for (int i = 0; i < amt; ++i) {
                auto* obj = (*bucket)[i];
                if (!obj) {
                    continue;
                }
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
                if (auto* enhanced = typeinfo_cast<EnhancedGameObject*>(obj)) {
                    out.push_back({ enhanced, enhanced->m_activatedByPlayer1, enhanced->m_activatedByPlayer2 });
                }
            }
        }
    }
    return out;
}

namespace {
    struct CamState {
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
        static_assert(offsetof(GJGameState, m_cameraPosition) == 0x268 && offsetof(GJGameState, m_cameraStepDiff) == 0x334 &&
                      offsetof(GJGameState, m_unkUint13) == 0x358 && offsetof(GJGameState, m_unkUint17) == 0x5bc);
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

        static CamState of(PlayLayer* play) {
            CamState out;
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

    std::optional<CamState>& pinned_camera() {
        static std::optional<CamState> pin;
        return pin;
    }

    bool pinned_this_frame = false;

    void show_camera(PlayLayer* play, CamState const& state) {
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
        pinned_camera() = CamState::of(play);
        pinned_this_frame = true;
    }
}

void Logic::note_shown_camera(PlayLayer* play, bool stepped) {
    if (!play || !Logic::get().backstep)
        return;
    if (std::exchange(pinned_this_frame, false)) {
        if (auto& pin = pinned_camera()) {
            show_camera(play, *pin);
        }
        return;
    }
    if (stepped) {
        pinned_camera().reset();
        return;
    }
    if (auto& pin = pinned_camera()) {
        show_camera(play, *pin);
    }
}

struct BackstepInfo {
    std::vector<UsedMark> used;
    SavedPlayerCheckpoint player1, player2;
    bool has_player2 = false;
    gd::unordered_map<int, int> persistent_items;
    gd::vector<GameObject*> calc_non_effect;
    int calc_non_effect_size = 0;
    CamState camera;
};

namespace {
    struct Buttons {
        bool jump_buffered = false, state_ring_jump = false, was_jump_buffered = false, was_robot_jump = false;
        unsigned char state_jump_buffered = 0;
        bool state_ring_jump2 = false, touched_ring = false, touched_custom_ring = false, touched_gravity_portal = false, touched_pad = false;
        bool ring_jump_related = false, holding_left = false, holding_right = false;
        // NOT in gds checkpoint either
        double fall_speed = 0;
        int reverse_related = 0;
        geode::SeedValueRSV jump_related{};
        std::array<std::int8_t, 4> holding{ -1, -1, -1, -1 };
        std::vector<int> touched_rings, ring_related;
        bool present = false;

        static Buttons of(PlayerObject* body) {
            Buttons out;
            if (!body)
                return out;
            out.present = true;
            out.jump_buffered = body->m_jumpBuffered;
            out.state_ring_jump = body->m_stateRingJump;
            out.was_jump_buffered = body->m_wasJumpBuffered;
            out.was_robot_jump = body->m_wasRobotJump;
            out.state_jump_buffered = body->m_stateJumpBuffered;
            out.state_ring_jump2 = body->m_stateRingJump2;
            out.touched_ring = body->m_touchedRing;
            out.touched_custom_ring = body->m_touchedCustomRing;
            out.touched_gravity_portal = body->m_touchedGravityPortal;
            out.touched_pad = body->m_touchedPad;
            out.ring_jump_related = body->m_ringJumpRelated;
            out.holding_left = body->m_holdingLeft;
            out.holding_right = body->m_holdingRight;
            out.fall_speed = body->m_fallSpeed;
            out.reverse_related = body->m_reverseRelated;
            out.jump_related = body->m_jumpRelatedAC2;
            for (auto const& [button, held] : body->m_holdingButtons)
                if (button >= 0 && button < 4)
                    out.holding[static_cast<std::size_t>(button)] = held ? 1 : 0;
            for (int ring : body->m_ringRelatedSet)
                out.ring_related.push_back(ring);
            return out;
        }

        void apply(PlayerObject* body) const {
            if (!body || !present)
                return;
            body->m_jumpBuffered = jump_buffered;
            body->m_stateRingJump = state_ring_jump;
            body->m_wasJumpBuffered = was_jump_buffered;
            body->m_wasRobotJump = was_robot_jump;
            body->m_stateJumpBuffered = state_jump_buffered;
            body->m_stateRingJump2 = state_ring_jump2;
            body->m_touchedRing = touched_ring;
            body->m_touchedCustomRing = touched_custom_ring;
            body->m_touchedGravityPortal = touched_gravity_portal;
            body->m_touchedPad = touched_pad;
            body->m_ringJumpRelated = ring_jump_related;
            body->m_holdingLeft = holding_left;
            body->m_holdingRight = holding_right;
            body->m_fallSpeed = fall_speed;
            body->m_reverseRelated = reverse_related;
            body->m_jumpRelatedAC2 = jump_related;
            body->m_holdingButtons.clear();
            for (int button = 0; button < 4; ++button)
                if (holding[static_cast<std::size_t>(button)] >= 0)
                    body->m_holdingButtons[button] = holding[static_cast<std::size_t>(button)] != 0;
            body->m_touchedRings.clear();
            for (int ring : touched_rings)
                body->m_touchedRings.insert(ring);
            body->m_ringRelatedSet.clear();
            for (int ring : ring_related)
                body->m_ringRelatedSet.insert(ring);
        }
    };

    struct Probe {
        double x = 0, y = 0, yVelocity = 0, levelTime = 0, rotation = 0, node_x = 0, node_y = 0, last_x = 0, last_y = 0;
        CCPoint position1, position2, node1, node2;
        double time1 = 0, time2 = 0;
        Buttons buttons1, buttons2;
        std::uint64_t inputs = 0;
        unsigned attempt = 0;
        bool has_player2 = false;
        unsigned progress = 0;
    };

    constexpr size_t RING = 1 << 17;
    std::vector<std::pair<int, Probe>> history;
    bool reported = false;
    bool check_restore = false;
    unsigned attempt_number = 1;

    Probe probe(PlayLayer* play) {
        Probe value;
        std::uint64_t inputs = 0xcbf29ce484222325ull; // fnv-1a start value
        for (auto const& cmd : play->m_queuedButtons) {
            std::uint64_t word = static_cast<std::uint64_t>(static_cast<int>(cmd.m_button)) | (static_cast<std::uint64_t>(cmd.m_isPush) << 8) |
                                 (static_cast<std::uint64_t>(cmd.m_isPlayer2) << 9);
            inputs = (inputs ^ word) * 0x100000001b3ull; // fnv-1a prime
        }
        value.inputs = inputs;
        value.attempt = attempt_number;
        if (auto* body = play->m_player1) {
            value.x = body->m_position.x;
            value.y = body->m_position.y;
            value.yVelocity = body->m_yVelocity;
            value.rotation = body->getRotation();
            value.node_x = body->getPositionX();
            value.node_y = body->getPositionY();
            value.last_x = body->m_lastPosition.x;
            value.last_y = body->m_lastPosition.y;
            value.position1 = body->m_position;
            value.node1 = CCPoint(body->getPositionX(), body->getPositionY());
            value.time1 = body->m_totalTime;
            value.buttons1 = Buttons::of(body);
        }
        if (auto* body = play->m_player2) {
            value.position2 = body->m_position;
            value.node2 = CCPoint(body->getPositionX(), body->getPositionY());
            value.time2 = body->m_totalTime;
            value.buttons2 = Buttons::of(body);
            value.has_player2 = true;
        }
        value.levelTime = play->m_gameState.m_levelTime;
        value.progress = play->m_gameState.m_currentProgress;
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
        auto put = [](PlayerObject* body, CCPoint node, CCPoint position, double time, Buttons const& buttons) {
            body->setPosition(node);
            body->m_position = position;
            body->m_totalTime = time;
            buttons.apply(body);
        };
        if (play->m_player1)
            put(play->m_player1, slot.second.node1, slot.second.position1, slot.second.time1, slot.second.buttons1);
        if (play->m_player2 && slot.second.has_player2)
            put(play->m_player2, slot.second.node2, slot.second.position2, slot.second.time2, slot.second.buttons2);
    }
}

int Logic::backstep_interval() const {
    double rate = sample_rate();
    if (!std::isfinite(rate) || rate <= 60.0) {
        return 1;
    }
    return std::max(1, static_cast<int>(std::lround(rate / 60.0)));
}

void Logic::release_backstep(BackstepSave& save, char const* why) {
    if (!save.object)
        return;
    auto broken = save.broken ? std::string{ "flagged earlier" } : echo::state_check::broken(save.object->m_gameState);
    if (broken.empty()) {
        save.object->release();
    }
    else {
    }
    save.object = nullptr;
}

void Logic::clear_backsteps() {
    ++attempt_number;
    auto& logic = Logic::get();
    history.clear();
    reported = false;
    for (auto& save : logic.backsteps)
        Logic::get().release_backstep(save, "clear");
    pinned_camera().reset();
    pinned_this_frame = false;
    logic.backsteps.clear();
    Logic::get().clear_backstep_objects();
    if (!logic.backstep_swapped) {
        logic.backstep_inputs.clear();
    }
}

// runs BEFORE the step so the save is the state at step_frame
void Logic::save_backstep(PlayLayer* play) {
    log::debug("save backstep");
    auto& logic = Logic::get();
    if (!play || logic.predicting || logic.backstepping)
        return;
    bool resim = logic.backstep_target >= 0;
    if (!logic.backstep || Logic::get().render_capturing()) {
        if (!logic.backsteps.empty()) {
            Logic::get().clear_backsteps();
        }
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
    bool dense = resim || shown;
    if (!dense && !logic.backsteps.empty() && frame - logic.backsteps.back().echo.frame < Logic::get().backstep_interval())
        return;
    if (!logic.backsteps.empty() && logic.backsteps.back().echo.frame >= frame)
        return;
    auto* obj = play->createCheckpoint();
    if (!obj)
        return;
    obj->retain();
    auto extra = std::make_shared<BackstepInfo>();
    if (play->m_player1) {
        extra->player1 = SavedPlayerCheckpoint::create(play->m_player1);
    }
    if (play->m_player2) {
        extra->player2 = SavedPlayerCheckpoint::create(play->m_player2);
        extra->has_player2 = true;
    }
    if (play->m_effectManager) {
        extra->persistent_items = play->m_effectManager->m_persistentItemCountMap;
    }
    extra->calc_non_effect = play->m_calcNonEffectObjects;
    extra->calc_non_effect_size = play->m_calcNonEffectObjectsSize;
    extra->used = used_marks(play);
    extra->camera = CamState::of(play);
    logic.backsteps.push_back({ obj, Logic::get().capture_checkpoint(play, obj, frame), std::move(extra), {} });
    Logic::get().note_backstep_objects(play);
    double rate = logic.sample_rate();
    if (!std::isfinite(rate) || rate <= 0.0) {
        rate = 240.0;
    }
    size_t keep = static_cast<size_t>(std::clamp(std::llround(rate * logic.backstep_percent / 100.0), 1ll, 100000ll));
    while (!resim && logic.backsteps.size() > keep) {
        Logic::get().release_backstep(logic.backsteps.front(), "trim");
        logic.backsteps.pop_front();
    }
}

void Logic::trim_backsteps(PlayLayer* play) {
    // todo
}

bool Logic::step_back(PlayLayer* play, int steps) {
    if (steps <= 0)
        return false;
    return Logic::get().step_to(play, std::max(0, Logic::get().step_frame - steps), std::numeric_limits<int>::max());
}

bool Logic::step_to(PlayLayer* play, int target, int keep_up_to) {
    // TODO
    return {};
}

void Logic::finish_backstep(PlayLayer* play) {
    // port this
}

void Logic::poll_back_hold() {
    // TODO
}

static_assert(offsetof(PlayLayer, m_checkpointArray) == 0x3888);
