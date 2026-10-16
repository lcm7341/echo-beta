#include "Logic/logic.hpp"
#include "Hooks/step.hpp"
#include "Hooks/rotation.hpp"
#include "GUI/gui.hpp"
#include <Geode/utils/async.hpp>
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>

using namespace geode::prelude;

FramePose Logic::capture_pose(PlayerObject* player) {
    FramePose pose;
    if (!player) return pose;
    pose.x = player->getPositionX();
    pose.y = player->getPositionY();
    pose.rotation = player->getRotation();
    pose.y_velocity = player->m_yVelocity;
    pose.x_velocity = (double)player->m_playerSpeed;
    return pose;
}

bool Logic::flip_controls() {
    auto* manager = GameManager::get();
    return manager && manager->getGameVariable("0010");
}

void Logic::set_replay_pos(PlayLayer* play) {
    int frame = play ? get_frame(play) : 0;
    auto later = inputs_in_order
                     ? std::lower_bound(inputs.begin(), inputs.end(), frame,
                           [](const Frame& input, int key) {
                               return input.number < key;
                           })
                     : std::find_if(inputs.begin(), inputs.end(), [frame](const Frame& input) {
                           return input.number >= frame;
                       });
    replay_pos = (size_t)std::distance(inputs.begin(), later);
    hold_sync = true;
}

void Logic::trim_recorded_inputs(int frame) {
    bool record1 = records_player(false), record2 = records_player(true);
    if (!record1 && !record2) return;
    compact_inputs(inputs, poses, input_ids, [&](const Frame& input) {
        bool matches = input.player2 ? record2 : record1;
        bool later = frame <= 0 ? input.number >= frame : input.number > frame;
        return !(matches && later);
    });
    mark_inputs_changed();
    refresh_live_hold();
}

void Logic::trim_fresh_attempt() {
    trim_recorded_inputs(0);
    if (!is_playing()) trim_rates(0);
    removed_frames = 0;
    replay_pos = 0;
}

PlayerSnapshot Logic::capture_player(PlayerObject* player) {
    PlayerSnapshot shot;
    if (!player) return shot;
    shot.present = true;
    shot.x = player->getPositionX();
    shot.y = player->getPositionY();
    shot.rotation = gameplay_rotation(player);
    shot.visual_rotation = visual_rotation(player);
    shot.y_velocity = player->m_yVelocity;
    shot.platformer_x_velocity = player->m_platformerXVelocity;
    shot.y_velocity_related = player->m_yVelocityRelated;
    shot.fall_speed = player->m_fallSpeed;
    shot.gravity = player->m_gravity;
    shot.speed_multiplier = player->m_speedMultiplier;
    shot.y_start = player->m_yStart;
    shot.maybe_reverse_speed = player->m_maybeReverseSpeed;
    shot.maybe_reverse_acceleration = player->m_maybeReverseAcceleration;
    shot.acceleration_or_speed = player->m_accelerationOrSpeed;
    shot.snap_distance = player->m_snapDistance;
    shot.last_flip_time = player->m_lastFlipTime;
    shot.game_mode_changed_time = player->m_gameModeChangedTime;
    shot.last_land_time = player->m_lastLandTime;
    shot.total_time = player->m_totalTime;
    shot.player_speed = player->m_playerSpeed;
    shot.player_speed_ac = player->m_playerSpeedAC;
    shot.vehicle_size = player->m_vehicleSize;
    shot.gravity_mod = player->m_gravityMod;
    shot.x_velocity_related = player->m_xVelocityRelated;
    shot.x_velocity_related2 = player->m_xVelocityRelated2;
    shot.y_velocity_related3 = player->m_yVelocityRelated3;
    shot.rotation_speed = player->m_rotationSpeed;
    shot.rotate_speed = player->m_rotateSpeed;
    shot.rotating = player->m_isRotating;
    shot.ball_rotating = player->m_isBallRotating;
    shot.ball_rotating2 = player->m_isBallRotating2;
    shot.slope_rotation = player->m_slopeRotation;
    shot.slope_velocity = player->m_slopeVelocity;
    shot.ship_rotation = player->m_shipRotation;
    shot.last_portal_pos = player->m_lastPortalPos;
    shot.ship = player->m_isShip;
    shot.bird = player->m_isBird;
    shot.ball = player->m_isBall;
    shot.dart = player->m_isDart;
    shot.robot = player->m_isRobot;
    shot.spider = player->m_isSpider;
    shot.swing = player->m_isSwing;
    shot.upside_down = player->m_isUpsideDown;
    shot.on_ground = player->m_isOnGround;
    shot.on_ground2 = player->m_isOnGround2;
    shot.on_ground3 = player->m_isOnGround3;
    shot.on_ground4 = player->m_isOnGround4;
    shot.going_left = player->m_isGoingLeft;
    shot.sideways = player->m_isSideways;
    shot.hidden = player->m_isHidden;
    shot.dashing = player->m_isDashing;
    shot.dead = player->m_isDead;
    shot.jump_buffered = player->m_jumpBuffered;
    shot.on_slope = player->m_isOnSlope;
    shot.was_on_slope = player->m_wasOnSlope;
    shot.was_teleported = player->m_wasTeleported;
    shot.fix_gravity_bug = player->m_fixGravityBug;
    shot.reverse_sync = player->m_reverseSync;
    shot.locked = player->m_isLocked;
    shot.controls_disabled = player->m_controlsDisabled;
    shot.holding_right = player->m_holdingRight;
    shot.holding_left = player->m_holdingLeft;
    shot.reverse_related = player->m_reverseRelated;
    shot.left_pressed_first = player->m_leftPressedFirst;
    shot.moving_left = player->m_platformerMovingLeft;
    shot.moving_right = player->m_platformerMovingRight;
    shot.platformer_velocity_related = player->m_platformerVelocityRelated;
    shot.boosted = player->m_maybeIsBoosted;
    shot.scale_x_related_time = player->m_scaleXRelatedTime;
    shot.decrease_boost_slide = player->m_decreaseBoostSlide;
    shot.scale_x_related = player->m_scaleXRelated;
    shot.scale_x_related2 = player->m_scaleXRelated2;
    shot.scale_x_related3 = player->m_scaleXRelated3;
    shot.scale_x_related4 = player->m_scaleXRelated4;
    shot.scale_x_related5 = player->m_scaleXRelated5;
    shot.has_stopped = player->m_maybeHasStopped;
    shot.correct_slope_direction = player->m_maybeGoingCorrectSlopeDirection;
    shot.sliding = player->m_isSliding;
    shot.slope_force = player->m_maybeSlopeForce;
    shot.on_ice = player->m_isOnIce;
    shot.phys_delta_related = player->m_physDeltaRelated;
    shot.sliding_time = player->m_maybeSlidingTime;
    shot.sliding_start_time = player->m_maybeSlidingStartTime;
    shot.changed_directions_time = player->m_changedDirectionsTime;
    shot.slope_end_time = player->m_slopeEndTime;
    shot.moving = player->m_isMoving;
    shot.sliding_right = player->m_isSlidingRight;
    shot.changed_direction_angle = player->m_maybeChangedDirectionAngle;
    shot.ground_y_velocity = player->m_groundYVelocity;
    shot.y_velocity_before_slope = player->m_yVelocityBeforeSlope;
    shot.slope_start_time = player->m_slopeStartTime;
    shot.last_grounded_pos = player->m_lastGroundedPos;
    shot.accelerating = player->m_isAccelerating;
    shot.ground_material = player->m_groundObjectMaterial;
    shot.player_speed_time = player->m_somethingPlayerSpeedTime;
    shot.affected_by_forces = player->m_affectedByForces;
    shot.state_force = player->m_stateForce;
    shot.state_force_vector = player->m_stateForceVector;
    shot.state_boost_x = player->m_stateBoostX;
    shot.state_boost_y = player->m_stateBoostY;
    shot.state_flip_gravity = player->m_stateFlipGravity;
    shot.state_scale = player->m_stateScale;
    shot.state_dart_slide = player->m_stateDartSlide;
    shot.state_no_auto_jump = player->m_stateNoAutoJump;
    shot.state_ring_jump = player->m_stateRingJump;
    shot.state_ring_jump2 = player->m_stateRingJump2;
    shot.was_jump_buffered = player->m_wasJumpBuffered;
    shot.state_jump_buffered = player->m_stateJumpBuffered;
    shot.ring_jump_related = player->m_ringJumpRelated;
    shot.touched_ring = player->m_touchedRing;
    shot.touched_custom_ring = player->m_touchedCustomRing;
    shot.touched_pad = player->m_touchedPad;
    shot.was_robot_jump = player->m_wasRobotJump;
    shot.black_orb_related = player->m_blackOrbRelated;
    shot.dash_x = player->m_dashX;
    shot.dash_y = player->m_dashY;
    shot.dash_angle = player->m_dashAngle;
    shot.dash_start_time = player->m_dashStartTime;
    shot.dash_ring = player->m_dashRing;
    shot.sprite_width_scale = player->m_spriteWidthScale;
    shot.sprite_height_scale = player->m_spriteHeightScale;
    shot.scale_x = player->getScaleX();
    shot.scale_y = player->getScaleY();
    shot.internal_pos = player->m_position;
    shot.last_position = player->m_lastPosition;
    return shot;
}

void Logic::apply_player(PlayerObject* player, const PlayerSnapshot& shot) {
    if (!player || !shot.present) return;
    if (player->m_isShip && !shot.ship) player->toggleFlyMode(false, true);
    if (player->m_isBird && !shot.bird) player->toggleBirdMode(false, true);
    if (player->m_isBall && !shot.ball) player->toggleRollMode(false, true);
    if (player->m_isDart && !shot.dart) player->toggleDartMode(false, true);
    if (player->m_isRobot && !shot.robot) player->toggleRobotMode(false, true);
    if (player->m_isSpider && !shot.spider) player->toggleSpiderMode(false, true);
    if (player->m_isSwing && !shot.swing) player->toggleSwingMode(false, true);
    if (shot.ship && !player->m_isShip) player->toggleFlyMode(true, true);
    if (shot.bird && !player->m_isBird) player->toggleBirdMode(true, true);
    if (shot.ball && !player->m_isBall) player->toggleRollMode(true, true);
    if (shot.dart && !player->m_isDart) player->toggleDartMode(true, true);
    if (shot.robot && !player->m_isRobot) player->toggleRobotMode(true, true);
    if (shot.spider && !player->m_isSpider) player->toggleSpiderMode(true, true);
    if (shot.swing && !player->m_isSwing) player->toggleSwingMode(true, true);
    if (player->m_isUpsideDown != shot.upside_down) player->flipGravity(shot.upside_down, true);
    // mini cube respawned full size once :/
    bool mini = shot.vehicle_size != 1.f;
    player->m_vehicleSize = mini ? 1.f : 0.6f;
    player->togglePlayerScale(mini, true);
    player->setPosition({ shot.x, shot.y });
    player->setRotation(shot.rotation);
    player->m_yVelocity = shot.y_velocity;
    player->m_platformerXVelocity = shot.platformer_x_velocity;
    player->m_yVelocityRelated = shot.y_velocity_related;
    player->m_fallSpeed = shot.fall_speed;
    player->m_gravity = shot.gravity;
    player->m_speedMultiplier = shot.speed_multiplier;
    player->m_yStart = shot.y_start;
    player->m_maybeReverseSpeed = shot.maybe_reverse_speed;
    player->m_maybeReverseAcceleration = shot.maybe_reverse_acceleration;
    player->m_accelerationOrSpeed = shot.acceleration_or_speed;
    player->m_snapDistance = shot.snap_distance;
    player->m_lastFlipTime = shot.last_flip_time;
    player->m_gameModeChangedTime = shot.game_mode_changed_time;
    player->m_lastLandTime = shot.last_land_time;
    player->m_totalTime = shot.total_time;
    player->m_playerSpeed = shot.player_speed;
    player->m_playerSpeedAC = shot.player_speed_ac;
    player->m_vehicleSize = shot.vehicle_size;
    player->m_gravityMod = shot.gravity_mod;
    player->m_xVelocityRelated = shot.x_velocity_related;
    player->m_xVelocityRelated2 = shot.x_velocity_related2;
    player->m_yVelocityRelated3 = shot.y_velocity_related3;
    player->m_rotationSpeed = shot.rotation_speed;
    player->m_rotateSpeed = shot.rotate_speed;
    player->m_isRotating = shot.rotating;
    player->m_isBallRotating = shot.ball_rotating;
    player->m_isBallRotating2 = shot.ball_rotating2;
    player->m_slopeRotation = shot.slope_rotation;
    player->m_slopeVelocity = shot.slope_velocity;
    player->m_shipRotation = shot.ship_rotation;
    player->m_lastPortalPos = shot.last_portal_pos;
    player->m_isShip = shot.ship;
    player->m_isBird = shot.bird;
    player->m_isBall = shot.ball;
    player->m_isDart = shot.dart;
    player->m_isRobot = shot.robot;
    player->m_isSpider = shot.spider;
    player->m_isSwing = shot.swing;
    player->m_isUpsideDown = shot.upside_down;
    player->m_jumpBuffered = shot.jump_buffered;
    player->m_isOnGround = shot.on_ground;
    player->m_isOnGround2 = shot.on_ground2;
    player->m_isOnGround3 = shot.on_ground3;
    player->m_isOnGround4 = shot.on_ground4;
    player->m_isGoingLeft = shot.going_left;
    player->m_isSideways = shot.sideways;
    player->m_isHidden = shot.hidden;
    player->m_isDashing = shot.dashing;
    player->m_isDead = shot.dead;
    player->m_isOnSlope = shot.on_slope;
    player->m_wasOnSlope = shot.was_on_slope;
    player->m_wasTeleported = shot.was_teleported;
    player->m_fixGravityBug = shot.fix_gravity_bug;
    player->m_reverseSync = shot.reverse_sync;
    player->m_isLocked = shot.locked;
    player->m_controlsDisabled = shot.controls_disabled;
    player->m_reverseRelated = shot.reverse_related;
    player->m_leftPressedFirst = shot.left_pressed_first;
    player->m_platformerMovingLeft = shot.moving_left;
    player->m_platformerMovingRight = shot.moving_right;
    player->m_platformerVelocityRelated = shot.platformer_velocity_related;
    player->m_maybeIsBoosted = shot.boosted;
    player->m_scaleXRelatedTime = shot.scale_x_related_time;
    player->m_decreaseBoostSlide = shot.decrease_boost_slide;
    player->m_scaleXRelated = shot.scale_x_related;
    player->m_scaleXRelated2 = shot.scale_x_related2;
    player->m_scaleXRelated3 = shot.scale_x_related3;
    player->m_scaleXRelated4 = shot.scale_x_related4;
    player->m_scaleXRelated5 = shot.scale_x_related5;
    player->m_maybeHasStopped = shot.has_stopped;
    player->m_maybeGoingCorrectSlopeDirection = shot.correct_slope_direction;
    player->m_isSliding = shot.sliding;
    player->m_maybeSlopeForce = shot.slope_force;
    player->m_isOnIce = shot.on_ice;
    player->m_physDeltaRelated = shot.phys_delta_related;
    player->m_maybeSlidingTime = shot.sliding_time;
    player->m_maybeSlidingStartTime = shot.sliding_start_time;
    player->m_changedDirectionsTime = shot.changed_directions_time;
    player->m_slopeEndTime = shot.slope_end_time;
    player->m_isMoving = shot.moving;
    player->m_isSlidingRight = shot.sliding_right;
    player->m_maybeChangedDirectionAngle = shot.changed_direction_angle;
    player->m_groundYVelocity = shot.ground_y_velocity;
    player->m_yVelocityBeforeSlope = shot.y_velocity_before_slope;
    player->m_slopeStartTime = shot.slope_start_time;
    player->m_lastGroundedPos = shot.last_grounded_pos;
    player->m_isAccelerating = shot.accelerating;
    player->m_groundObjectMaterial = shot.ground_material;
    player->m_somethingPlayerSpeedTime = shot.player_speed_time;
    player->m_affectedByForces = shot.affected_by_forces;
    player->m_stateForce = shot.state_force;
    player->m_stateForceVector = shot.state_force_vector;
    player->m_stateBoostX = shot.state_boost_x;
    player->m_stateBoostY = shot.state_boost_y;
    player->m_stateFlipGravity = shot.state_flip_gravity;
    player->m_stateScale = shot.state_scale;
    player->m_stateDartSlide = shot.state_dart_slide;
    player->m_stateNoAutoJump = shot.state_no_auto_jump;
    player->m_stateRingJump = shot.state_ring_jump;
    player->m_stateRingJump2 = shot.state_ring_jump2;
    player->m_wasJumpBuffered = shot.was_jump_buffered;
    player->m_stateJumpBuffered = shot.state_jump_buffered;
    player->m_ringJumpRelated = shot.ring_jump_related;
    player->m_touchedRing = shot.touched_ring;
    player->m_touchedCustomRing = shot.touched_custom_ring;
    player->m_touchedPad = shot.touched_pad;
    player->m_wasRobotJump = shot.was_robot_jump;
    player->m_blackOrbRelated = shot.black_orb_related;
    player->m_dashX = shot.dash_x;
    player->m_dashY = shot.dash_y;
    player->m_dashAngle = shot.dash_angle;
    player->m_dashStartTime = shot.dash_start_time;
    player->m_dashRing = shot.dash_ring;
    player->m_spriteWidthScale = shot.sprite_width_scale;
    player->m_spriteHeightScale = shot.sprite_height_scale;
    player->setScaleX(shot.scale_x);
    player->setScaleY(shot.scale_y);
    // m_position is NOT physics. gd just copies the node pos there
    player->m_position = CCPoint(shot.x, shot.y);
    player->m_lastPosition = shot.last_position;
    restore_rotation(player, shot.rotation, shot.visual_rotation);
}

LayerSnapshot Logic::capture_layer(PlayLayer* play) {
    LayerSnapshot shot;
    const auto& state = play->m_gameState;
    shot.level_time = state.m_levelTime;
    shot.total_time = state.m_totalTime;
    shot.unk_time = state.m_unkDouble3;
    shot.attempt_time = play->m_attemptTime;
    shot.progress = state.m_currentProgress;
    shot.time_warp = state.m_timeWarp;
    shot.queued_time_warp = state.m_queuedTimeWarp;
    shot.time_warp_related = state.m_timeWarpRelated;
    shot.time_mod = state.m_timeModRelated;
    shot.camera_angle = state.m_cameraAngle;
    shot.level_flipping = state.m_levelFlipping;
    shot.camera_pos = state.m_cameraPosition;
    shot.dual = state.m_isDualMode;
    shot.streak_blend = state.m_playerStreakBlend;
    shot.gravity_related = state.m_gravityRelated;
    shot.time_mod_related = state.m_timeModRelated2;
    return shot;
}

void Logic::apply_layer(PlayLayer* play, const LayerSnapshot& shot) {
    auto& state = play->m_gameState;
    state.m_levelTime = shot.level_time;
    state.m_totalTime = shot.total_time;
    state.m_unkDouble3 = shot.unk_time;
    play->m_attemptTime = shot.attempt_time;
    state.m_currentProgress = shot.progress;
    state.m_timeWarp = shot.time_warp;
    state.m_queuedTimeWarp = shot.queued_time_warp;
    state.m_timeWarpRelated = shot.time_warp_related;
    state.m_timeModRelated = shot.time_mod;
    state.m_cameraAngle = shot.camera_angle;
    state.m_levelFlipping = shot.level_flipping;
    state.m_cameraPosition = shot.camera_pos;
    state.m_isDualMode = shot.dual;
    state.m_playerStreakBlend = shot.streak_blend;
    state.m_gravityRelated = shot.gravity_related;
    state.m_timeModRelated2 = shot.time_mod_related;
}

void Logic::trim_from_checkpoint(PlayLayer* play, const Checkpoint& checkpoint) {
    step_frame = checkpoint.frame;
    apply_layer(play, checkpoint.layer);
    apply_player(play->m_player1, checkpoint.player1);
    apply_player(play->m_player2, checkpoint.player2);
    if (is_recording()) {
        int frame = get_frame(play);
        trim_recorded_inputs(frame);
        if (!is_playing()) trim_rates(frame);
        sort_inputs();
        replay_pos = inputs.size();
    }
}

void Logic::clear_physical_holds() {
    key_jump_p1 = key_jump_p2 = false;
    routed_keys.clear();
    key_left_p1 = key_right_p1 = false;
    key_left_p2 = key_right_p2 = false;
    death_jump_p1 = death_jump_p2 = false;
    death_left_p1 = death_right_p1 = false;
    death_left_p2 = death_right_p2 = false;
    physical_jump_p1 = physical_jump_p2 = false;
    physical_left_p1 = physical_right_p1 = false;
    physical_left_p2 = physical_right_p2 = false;
}

static int channel_index(bool player2, int button) {
    int slot = Logic::button_slot(button);
    if (slot < 0) return -1;
    return (player2 ? 3 : 0) + slot;
}

static bool same_input(const Frame& a, const Frame& b) {
    return a.number == b.number && a.holding == b.holding && a.player2 == b.player2 && a.button == b.button;
}

struct ChannelHistory {
    std::vector<int> frames;
    std::vector<char> held;
};

struct HoldHistory {
    const std::vector<Frame>* inputs = nullptr;
    const Frame* data = nullptr;
    size_t count = 0;
    unsigned long long version = 0;
    Frame last;
    std::array<ChannelHistory, 9> channels;
};

static thread_local HoldHistory g_hold_history;

static void rebuild_hold_history(HoldHistory& history, const std::vector<Frame>& inputs) {
    std::array<std::vector<size_t>, 9> members;
    for (size_t i = 0; i < inputs.size(); ++i) {
        int channel = channel_index(inputs[i].player2, inputs[i].button);
        if (channel < 0) continue;
        members[(size_t)channel].push_back(i);
        members[(size_t)(6 + channel % 3)].push_back(i);
    }
    for (size_t channel = 0; channel < members.size(); ++channel) {
        auto& order = members[channel];
        std::ranges::stable_sort(order, {}, [&](size_t i) {
            return inputs[i].number;
        });
        auto& out = history.channels[channel];
        out.frames.clear();
        out.held.clear();
        out.frames.reserve(order.size());
        out.held.reserve(order.size());
        size_t newest = 0;
        for (size_t k = 0; k < order.size(); ++k) {
            newest = k == 0 ? order[k] : std::max(newest, order[k]);
            out.frames.push_back(inputs[order[k]].number);
            out.held.push_back(inputs[newest].holding ? 1 : 0);
        }
    }
    history.inputs = &inputs;
    history.data = inputs.data();
    history.count = inputs.size();
    history.last = inputs.empty() ? Frame{} : inputs.back();
}

static bool append_to_hold_history(HoldHistory& history, const Frame& input) {
    int channel = channel_index(input.player2, input.button);
    if (channel >= 0) {
        auto& own = history.channels[(size_t)channel];
        auto& both = history.channels[(size_t)(6 + channel % 3)];
        if (!own.frames.empty() && input.number < own.frames.back()) return false;
        if (!both.frames.empty() && input.number < both.frames.back()) return false;
        own.frames.push_back(input.number);
        own.held.push_back(input.holding ? 1 : 0);
        both.frames.push_back(input.number);
        both.held.push_back(input.holding ? 1 : 0);
    }
    history.last = input;
    ++history.count;
    return true;
}

static const HoldHistory& current_hold_history(const Logic& logic) {
    auto& history = g_hold_history;
    const auto& inputs = logic.inputs;
    bool same = history.inputs == &inputs && history.version == logic.inputs_version && history.count <= inputs.size();
    if (same && history.count > 0) same = same_input(inputs[history.count - 1], history.last);
    if (same && history.count == inputs.size()) same = history.data == inputs.data();
    if (!same) rebuild_hold_history(history, inputs);
    for (size_t i = history.count; i < inputs.size(); ++i) {
        if (append_to_hold_history(history, inputs[i])) continue;
        rebuild_hold_history(history, inputs);
        break;
    }
    history.data = inputs.data();
    history.version = logic.inputs_version;
    return history;
}

static bool held_in_channel(const ChannelHistory& channel, int frame) {
    size_t after = (size_t)(std::upper_bound(channel.frames.begin(), channel.frames.end(), frame) - channel.frames.begin());
    return after > 0 && channel.held[after - 1] != 0;
}

void Logic::release_replay_index() {
    g_hold_history = {};
}

bool Logic::held_at(bool player2, int frame, int button) {
    int want = button == 0 ? JUMP : button;
    int lane = channel_index(player2, want);
    if (lane < 0) {
        bool held = false;
        for (const auto& input : inputs) {
            int stored = input.button == 0 ? JUMP : input.button;
            if (input.player2 != player2 || stored != want || input.number > frame) continue;
            held = input.holding;
        }
        return held;
    }
    size_t slot = (size_t)lane;
    if (has_last_input[slot] && frame >= last_input_frame[slot]) return last_input_held[slot] != 0;
    if (is_playing() && !is_recording()) {
        auto* play = PlayLayer::get();
        if (play && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode)) {
            return held_in_channel(current_hold_history(*this).channels[6 + slot % 3], frame);
        }
    }
    if (!is_playing()) {
        for (size_t i = inputs.size(); i-- > 0;) {
            const auto& input = inputs[i];
            int stored = input.button == 0 ? JUMP : input.button;
            if (input.player2 != player2 || stored != want || input.number > frame) continue;
            return input.holding;
        }
        return false;
    }
    return held_in_channel(current_hold_history(*this).channels[slot], frame);
}

std::pair<int, int> Logic::count_holds() {
    return { hold_count, release_count };
}

bool Logic::inputs_sorted() {
    return inputs_in_order;
}

static void note_last_input(Logic& logic, const Frame& frame) {
    logic.newest_input_frame = std::max(logic.newest_input_frame, frame.number);
    int slot = Logic::button_slot(frame.button);
    if (slot < 0) return;
    slot += frame.player2 ? 3 : 0;
    size_t lane = (size_t)slot;
    if (logic.has_last_input[lane] && frame.number < logic.last_input_frame[lane]) return;
    logic.last_input_frame[lane] = frame.number;
    logic.last_input_held[lane] = frame.holding ? 1 : 0;
    logic.has_last_input[lane] = 1;
}

static void grow_input_extras(Logic& logic, size_t index, bool appended) {
    auto grow = [&](auto& side, auto empty) {
        if (side.empty()) return;
        if (side.size() + 1 != logic.inputs.size()) {
            side.clear();
            return;
        }
        if (appended) side.push_back(empty);
        else side.insert(side.begin() + (std::ptrdiff_t)index, empty);
    };
    grow(logic.poses, FramePose{});
    grow(logic.input_ids, 0ull);
    if (logic.save_debug && logic.poses.size() != logic.inputs.size())
        logic.poses.resize(logic.inputs.size());
}

size_t Logic::add_input(const Frame& frame) {
    (frame.holding ? hold_count : release_count) += 1;
    if (!sort_automatically || !is_playing()) {
        if (!inputs.empty() && frame.number < inputs.back().number) inputs_in_order = false;
        inputs.push_back(frame);
        grow_input_extras(*this, inputs.size() - 1, true);
        note_last_input(*this, frame);
        return inputs.size() - 1;
    }
    auto later = std::upper_bound(inputs.begin(), inputs.end(), frame, [](const Frame& key, const Frame& element) {
        if (key.number != element.number) return key.number < element.number;
        if (key.player2 != element.player2) return key.player2 < element.player2;
        return key.button < element.button;
    });
    size_t idx = (size_t)std::distance(inputs.begin(), later);
    inputs.insert(later, frame);
    grow_input_extras(*this, idx, false);
    if (idx + 1 != inputs.size()) mark_inputs_changed();
    if (idx <= replay_pos) ++replay_pos;
    note_last_input(*this, frame);
    return idx;
}

void Logic::refresh_live_hold() {
    last_input_frame.fill(-1);
    last_input_held.fill(0);
    has_last_input.fill(0);
    newest_input_frame = -1;
    hold_count = 0;
    release_count = 0;
    inputs_in_order = true;
    int prev = -1;
    for (const auto& input : inputs) {
        note_last_input(*this, input);
        (input.holding ? hold_count : release_count) += 1;
        if (prev >= 0 && input.number < prev) inputs_in_order = false;
        prev = input.number;
    }
}

void Logic::record_input(GJBaseGameLayer* play, bool down, bool player1, int button, bool force) {
    if (!is_recording() || !play) return;
    if (resetting || refreshing_visuals || skip_record_frames > 0) return;
    if (button != 0 && button != JUMP && button != LEFT && button != RIGHT) return;
    if (button == 0) button = JUMP;
    if (mirror_click) return;
    bool player2 = !player1;
    bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool folded = !two_player && player2;
    if (folded) {
        player1 = true;
        player2 = false;
    }
    if (!records_player(player2) && !(folded && records_player(true))) return;
    int now = step_frame + 1;
    if (!is_playing()) {
        auto last = std::find_if(inputs.rbegin(), inputs.rend(), [player2](const Frame& input) {
            return input.player2 == player2;
        });
        if (last != inputs.rend() && last->number > now) return;
    }
    if (!force && down == held_at(player2, now, button)) {
        if (tracing())
            trace_event(fmt::format("SKIP frame={} down={} p2={} button={} (macro already has it {})\n", now, down, player2, button, down ? "held" : "released"));
        return;
    }
    if (tracing())
        trace_event(fmt::format("REC frame={} down={} p2={} button={} force={}\n", now, down, player2, button, force));

    Frame frame;
    frame.number = now;
    frame.holding = down;
    frame.player2 = player2;
    frame.button = (unsigned char)button;
    size_t at = add_input(frame);
    if (save_debug) {
        PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
        set_pose_at(at, capture_pose(body));
    }
    if (!click_both || routing_input || maintaining_gravity || assist_input || !records_player(!player2))
        return;
    routing_input = true;
    record_input(play, down, !player1, button, force);
    routing_input = false;
}

void Logic::inject_button(PlayLayer* play, bool down, int button, bool player1) {
    injecting = true;
    play->handleButton(down, button == 0 ? JUMP : button, player1);
    injecting = false;
}

struct CpsScan {
    float macro_fps = 240.f;
    std::vector<RateChange> rates;
    int max_cps = 16;
    std::vector<CpsBreak> notes_p1, notes_p2;
    std::string highest = "P1: 0";

    double seconds_at(int frame) const {
        return seconds_at_frame(rates, macro_fps, frame);
    }
};

static void add_cps_break(std::vector<CpsBreak>& notes, int first, int last, std::string text) {
    if (last < first) std::swap(first, last);
    if (!notes.empty() && notes.back().text == text && notes.back().last + 1 >= first) {
        notes.back().first = std::min(notes.back().first, first);
        notes.back().last = std::max(notes.back().last, last);
        return;
    }
    notes.push_back({ first, last, std::move(text) });
}

static void check_rule_2(std::vector<CpsBreak>& notes, const std::vector<int>& presses) {
    int index = 0;
    while (index < (int)presses.size()) {
        int frame = presses[index];
        int end = index;
        while (end < (int)presses.size() && presses[end] == frame)
            ++end;
        if (end - index > Logic::MAX_PER_FRAME) add_cps_break(notes, frame, frame, "Rule 2");
        index = end;
    }
}

static std::pair<int, std::string> check_rule_1(CpsScan& scan, std::vector<CpsBreak>& notes, const std::vector<int>& presses,
    const std::vector<double>& press_seconds, const char* who) {
    int best = 0;
    std::string best_text = fmt::format("{}: 0", who);
    int left = 0;
    for (int right = 0; right < (int)presses.size(); ++right) {
        double end = press_seconds[(size_t)right];
        while (left <= right && end - press_seconds[(size_t)left] > 1.0)
            ++left;
        int count = right - left + 1;
        if (count > best) {
            best = count;
            best_text = fmt::format("{}: {} ({} to {})", who, count, presses[left], presses[right]);
        }
        if (count > scan.max_cps) add_cps_break(notes, presses[left], presses[right], "Rule 1");
    }
    return { best, best_text };
}

static void check_rule_3(CpsScan& scan, std::vector<CpsBreak>& notes, const std::vector<int>& presses, const std::vector<double>& press_seconds) {
    int n = (int)presses.size();
    double rate = (double)Logic::MAX_BURST_RATE;
    std::vector<double> burst_lead(presses.size());
    std::vector<double> highest_lead(presses.size());
    for (size_t s = 0; s < presses.size(); ++s) {
        burst_lead[s] = rate * press_seconds[s] - (double)s;
        highest_lead[s] = s == 0 ? burst_lead[s] : std::max(highest_lead[s - 1], burst_lead[s]);
    }
    for (int end = Logic::MIN_BURST_CLICKS - 1; end < n; ++end) {
        int latest_start = end - (Logic::MIN_BURST_CLICKS - 1);
        double lead_needed = rate * press_seconds[(size_t)end] - (double)end - 1.0;
        double rounding = Logic::burst_rounding(lead_needed);
        auto first_possible = std::upper_bound(highest_lead.begin(), highest_lead.begin() + latest_start + 1, lead_needed - rounding);
        for (int start = (int)std::distance(highest_lead.begin(), first_possible); start <= latest_start; ++start) {
            double margin = burst_lead[(size_t)start] - lead_needed;
            if (margin <= -rounding) continue;
            int count = end - start + 1;
            if (margin > rounding || Logic::burst_too_fast(count, scan.seconds_at(presses[end]) - scan.seconds_at(presses[start]))) {
                add_cps_break(notes, presses[start], presses[end], "Rule 3");
                break;
            }
        }
    }
}

static void check_player_cps(CpsScan& scan, const std::vector<int>& presses, bool player2) {
    auto& notes = player2 ? scan.notes_p2 : scan.notes_p1;
    const char* who = player2 ? "P2" : "P1";
    if (presses.empty()) return;
    std::vector<double> press_seconds(presses.size());
    std::ranges::transform(presses, press_seconds.begin(), [&](int frame) {
        return scan.seconds_at(frame);
    });
    check_rule_2(notes, presses);
    auto [best, best_text] = check_rule_1(scan, notes, presses, press_seconds, who);
    check_rule_3(scan, notes, presses, press_seconds);
    int current_best = 0;
    auto colon = scan.highest.find(": ");
    if (colon != std::string::npos) current_best = std::atoi(scan.highest.c_str() + colon + 2);
    if (best >= current_best) scan.highest = best_text;
}

static void scan_cps(CpsScan& scan, const std::vector<int>& p1, const std::vector<int>& p2) {
    check_player_cps(scan, p1, false);
    check_player_cps(scan, p2, true);
}

static std::pair<std::vector<int>, std::vector<int>> split_presses(const std::vector<Frame>& inputs) {
    std::pair<std::vector<int>, std::vector<int>> presses;
    for (const auto& input : inputs) {
        if (!input.holding) continue;
        (input.player2 ? presses.second : presses.first).push_back(input.number);
    }
    return presses;
}

static void apply_cps_scan(Logic& logic, CpsScan& scan) {
    logic.cps_notes_p1 = std::move(scan.notes_p1);
    logic.cps_notes_p2 = std::move(scan.notes_p2);
    logic.highest_cps = std::move(scan.highest);
}

void Logic::recompute_cps() {
    auto [p1, p2] = split_presses(inputs);
    CpsScan scan;
    scan.macro_fps = macro_fps;
    scan.rates = rates;
    scan.max_cps = std::max(max_cps, 1);
    static std::atomic<unsigned long long> latest_scan{ 0 };
    unsigned long long this_scan = latest_scan.fetch_add(1) + 1;
    cps_notes_p1.clear();
    cps_notes_p2.clear();
    highest_cps = "Checking CPS...";
    try {
        std::thread([scan, p1 = std::move(p1), p2 = std::move(p2), this_scan]() mutable {
            try {
                scan_cps(scan, p1, p2);
            }
            catch (...) {
                return;
            }
            geode::queueInMainThread([scan = std::move(scan), this_scan]() mutable {
                if (this_scan != latest_scan.load()) return;
                apply_cps_scan(Logic::get(), scan);
            });
        }).detach();
    }
    catch (...) {
        auto presses = split_presses(inputs);
        scan_cps(scan, presses.first, presses.second);
        apply_cps_scan(*this, scan);
    }
}
