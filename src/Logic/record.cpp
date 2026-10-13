// recording
//
// record_input is called from gds button handler. the input is stored for step_frame + 1 bc gd counts a step BEFORE it reads the button
// pressing a button thats already held is not a new input. the latest hold per player & button is kept so that check is instant
// in a 1 player dual 1 button moves both icons, thats stored as p1 even when the 2nd icon is the one being recorded
// recording over part of a macro trims everything after that point for the recorded player & leaves the other players inputs
// capture_player / apply_player copy a players whole state. setting the flags alone leaves the sprite in the wrong mode, so it goes thru gds setters
// recompute_cps marks where the click rules break (16 a second, 3 on a frame, 5+ faster than 48/s)
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
    if (!player)
        return pose;
    pose.x = player->getPositionX();
    pose.y = player->getPositionY();
    pose.rotation = player->getRotation();
    pose.yVelocity = player->m_yVelocity;
    pose.x_velocity = static_cast<double>(player->m_playerSpeed);
    return pose;
}

bool Logic::flip_controls() {
    auto* manager = GameManager::get();
    return manager && manager->getGameVariable("0010");
}

void Logic::set_replay_pos(PlayLayer* play) {
    auto& logic = Logic::get();
    int frame = play ? Logic::get().get_frame(play) : 0;
    auto later = logic.inputs_in_order
                     ? std::lower_bound(logic.inputs.begin(), logic.inputs.end(), frame,
                           [](Frame const& input, int key) {
                               return input.number < key;
                           })
                     : std::find_if(logic.inputs.begin(), logic.inputs.end(), [frame](Frame const& input) {
                           return input.number >= frame;
                       });
    logic.replay_pos = static_cast<size_t>(std::distance(logic.inputs.begin(), later));
    logic.hold_sync = true;
}

void Logic::trim_recorded_inputs(Session& value, int frame) {
    bool record1 = value.records_player(false), record2 = value.records_player(true);
    if (!record1 && !record2)
        return;
    replay::compact(value.inputs, value.poses, value.input_ids, [&](Frame const& input) {
        bool matches = input.player2 ? record2 : record1;
        bool later = frame <= 0 ? input.number >= frame : input.number > frame;
        return !(matches && later);
    });
    value.touch_inputs();
    Logic::get().refresh_live_hold();
}

void Logic::trim_fresh_attempt(PlayLayer* play, Session& value) {
    (void)play;
    Logic::get().trim_recorded_inputs(value, 0);
    if (!value.is_playing())
        Logic::get().trim_rates(0);
    value.removed_frames = 0;
    value.replay_pos = 0;
}

PlayerSnapshot Logic::capture_player(PlayerObject* player) {
    PlayerSnapshot shot;
    if (!player)
        return shot;
    shot.present = true;
    shot.x = player->getPositionX();
    shot.y = player->getPositionY();
    shot.rotation = echo::rotation::gameplay(player);
    shot.visual_rotation = echo::rotation::visual(player);
    shot.yVelocity = player->m_yVelocity;
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
    shot.lastLandTime = player->m_lastLandTime;
    shot.totalTime = player->m_totalTime;
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
    shot.movingLeft = player->m_platformerMovingLeft;
    shot.movingRight = player->m_platformerMovingRight;
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
    shot.scaleX = player->getScaleX();
    shot.scaleY = player->getScaleY();
    shot.internal_pos = player->m_position;
    shot.lastPosition = player->m_lastPosition;
    return shot;
}

void Logic::apply_player(PlayerObject* player, PlayerSnapshot const& shot) {
    if (!player || !shot.present)
        return;
    auto mode = [&](bool active, bool saved, auto toggle) {
        if (active && !saved)
            (player->*toggle)(false, true);
    };
    mode(player->m_isShip, shot.ship, &PlayerObject::toggleFlyMode);
    mode(player->m_isBird, shot.bird, &PlayerObject::toggleBirdMode);
    mode(player->m_isBall, shot.ball, &PlayerObject::toggleRollMode);
    mode(player->m_isDart, shot.dart, &PlayerObject::toggleDartMode);
    mode(player->m_isRobot, shot.robot, &PlayerObject::toggleRobotMode);
    mode(player->m_isSpider, shot.spider, &PlayerObject::toggleSpiderMode);
    mode(player->m_isSwing, shot.swing, &PlayerObject::toggleSwingMode);
    if (shot.ship && !player->m_isShip)
        player->toggleFlyMode(true, true);
    if (shot.bird && !player->m_isBird)
        player->toggleBirdMode(true, true);
    if (shot.ball && !player->m_isBall)
        player->toggleRollMode(true, true);
    if (shot.dart && !player->m_isDart)
        player->toggleDartMode(true, true);
    if (shot.robot && !player->m_isRobot)
        player->toggleRobotMode(true, true);
    if (shot.spider && !player->m_isSpider)
        player->toggleSpiderMode(true, true);
    if (shot.swing && !player->m_isSwing)
        player->toggleSwingMode(true, true);
    if (player->m_isUpsideDown != shot.upside_down)
        player->flipGravity(shot.upside_down, true);
    // mini cube respawned full size once :/
    {
        bool mini = shot.vehicle_size != 1.f;
        player->m_vehicleSize = mini ? 1.f : 0.6f;
        player->togglePlayerScale(mini, true);
    }
    player->setPosition({ shot.x, shot.y });
    player->setRotation(shot.rotation);
    player->m_yVelocity = shot.yVelocity;
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
    player->m_lastLandTime = shot.lastLandTime;
    player->m_totalTime = shot.totalTime;
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
    player->m_platformerMovingLeft = shot.movingLeft;
    player->m_platformerMovingRight = shot.movingRight;
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
    player->setScaleX(shot.scaleX);
    player->setScaleY(shot.scaleY);
    // m_position is NOT physics. gd just copies the node pos there
    player->m_position = CCPoint(shot.x, shot.y);
    player->m_lastPosition = shot.lastPosition;
    echo::rotation::restore(player, shot.rotation, shot.visual_rotation);
}

LayerSnapshot Logic::capture_layer(PlayLayer* play) {
    LayerSnapshot shot;
    auto const& state = play->m_gameState;
    shot.levelTime = state.m_levelTime;
    shot.totalTime = state.m_totalTime;
    shot.unk_time = state.m_unkDouble3;
    shot.attemptTime = play->m_attemptTime;
    shot.progress = state.m_currentProgress;
    shot.timeWarp = state.m_timeWarp;
    shot.queued_time_warp = state.m_queuedTimeWarp;
    shot.time_warp_related = state.m_timeWarpRelated;
    shot.timeMod = state.m_timeModRelated;
    shot.camera_angle = state.m_cameraAngle;
    shot.levelFlipping = state.m_levelFlipping;
    shot.camera_pos = state.m_cameraPosition;
    shot.dual = state.m_isDualMode;
    shot.streak_blend = state.m_playerStreakBlend;
    shot.gravity_related = state.m_gravityRelated;
    shot.time_mod_related = state.m_timeModRelated2;
    return shot;
}

void Logic::apply_layer(PlayLayer* play, LayerSnapshot const& shot) {
    auto& state = play->m_gameState;
    state.m_levelTime = shot.levelTime;
    state.m_totalTime = shot.totalTime;
    state.m_unkDouble3 = shot.unk_time;
    play->m_attemptTime = shot.attemptTime;
    state.m_currentProgress = shot.progress;
    state.m_timeWarp = shot.timeWarp;
    state.m_queuedTimeWarp = shot.queued_time_warp;
    state.m_timeWarpRelated = shot.time_warp_related;
    state.m_timeModRelated = shot.timeMod;
    state.m_cameraAngle = shot.camera_angle;
    state.m_levelFlipping = shot.levelFlipping;
    state.m_cameraPosition = shot.camera_pos;
    state.m_isDualMode = shot.dual;
    state.m_playerStreakBlend = shot.streak_blend;
    state.m_gravityRelated = shot.gravity_related;
    state.m_timeModRelated2 = shot.time_mod_related;
}

void Logic::trim_from_checkpoint(PlayLayer* play, Session& value, Checkpoint const& checkpoint) {
    value.step_frame = checkpoint.frame;
    Logic::get().apply_layer(play, checkpoint.layer);
    Logic::get().apply_player(play->m_player1, checkpoint.player1);
    Logic::get().apply_player(play->m_player2, checkpoint.player2);
    if ((value).is_recording()) {
        int frame = Logic::get().get_frame(play);
        Logic::get().trim_recorded_inputs(value, frame);
        if (!value.is_playing())
            Logic::get().trim_rates(frame);
        Logic::get().sort_inputs();
        value.replay_pos = value.inputs.size();
    }
}

void Logic::clear_physical_holds() {
    auto& logic = Logic::get();
    logic.key_jump_p1 = logic.key_jump_p2 = false;
    logic.steered_keys.clear();
    logic.key_left_p1 = logic.key_right_p1 = false;
    logic.key_left_p2 = logic.key_right_p2 = false;
    logic.death_jump_p1 = logic.death_jump_p2 = false;
    logic.death_left_p1 = logic.death_right_p1 = false;
    logic.death_left_p2 = logic.death_right_p2 = false;
    logic.physical_jump_p1 = logic.physical_jump_p2 = false;
    logic.physical_left_p1 = logic.physical_right_p1 = false;
    logic.physical_left_p2 = logic.physical_right_p2 = false;
}

namespace {
    struct HeldLane {
        std::vector<int> frames;
        std::vector<char> held;
        mutable replay::PrefixCursor cursor;
    };

    struct InputIndex {
        std::vector<Frame> const* owner = nullptr;
        Frame const* data = nullptr;
        size_t count = 0;
        uint64_t revision = 0;
        Frame last;
        bool sorted = true;
        int holds = 0;
        int releases = 0;
        // 0-5 = p1 & p2 jump, left, right. 6-8 = the same 3 buttons with BOTH players together
        // (outside 2 player levels gd gives every input to the same icon)
        std::array<HeldLane, 9> lanes;
    };

    int lane_of(bool player2, int button) {
        int const jump = static_cast<int>(PlayerButton::Jump);
        int const left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
        int slot = button == 0 || button == jump ? 0 : button == left ? 1
                                                   : button == right  ? 2
                                                                      : -1;
        if (slot < 0)
            return -1;
        return (player2 ? 3 : 0) + slot;
    }

    bool same_input(Frame const& a, Frame const& b) {
        return a.number == b.number && a.holding == b.holding && a.player2 == b.player2 && a.button == b.button;
    }

    bool append_input(InputIndex& index, Frame const& input) {
        int lane = lane_of(input.player2, input.button);
        if (lane >= 0) {
            auto& at = index.lanes[static_cast<size_t>(lane)];
            auto& both = index.lanes[static_cast<size_t>(6 + lane % 3)];
            if (!at.frames.empty() && input.number < at.frames.back())
                return false;
            if (!both.frames.empty() && input.number < both.frames.back())
                return false;
            at.frames.push_back(input.number);
            at.held.push_back(input.holding ? 1 : 0);
            both.frames.push_back(input.number);
            both.held.push_back(input.holding ? 1 : 0);
        }
        if (index.count > 0 && input.number < index.last.number)
            index.sorted = false;
        (input.holding ? index.holds : index.releases) += 1;
        index.last = input;
        ++index.count;
        return true;
    }

    void rebuild_index(InputIndex& index, std::vector<Frame> const& inputs) {
        auto reset = [&] {
            for (auto& lane : index.lanes) {
                lane.frames.clear();
                lane.held.clear();
                lane.cursor = {};
            }
            index.count = 0;
            index.sorted = true;
            index.holds = 0;
            index.releases = 0;
            index.last = Frame{};
        };
        reset();
        bool in_order = std::ranges::all_of(inputs, [&](Frame const& input) {
            return append_input(index, input);
        });
        if (in_order)
            return;

        reset();
        std::array<std::vector<size_t>, 9> members;
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto const& input = inputs[i];
            int lane = lane_of(input.player2, input.button);
            if (lane >= 0) {
                members[static_cast<size_t>(lane)].push_back(i);
                members[static_cast<size_t>(6 + lane % 3)].push_back(i);
            }
            if (i > 0 && input.number < inputs[i - 1].number)
                index.sorted = false;
            (input.holding ? index.holds : index.releases) += 1;
        }
        for (size_t lane = 0; lane < members.size(); ++lane) {
            auto& order = members[lane];
            std::ranges::stable_sort(order, {}, [&](size_t i) {
                return inputs[i].number;
            });
            auto& at = index.lanes[lane];
            at.frames.reserve(order.size());
            at.held.reserve(order.size());
            size_t newest = 0;
            for (size_t k = 0; k < order.size(); ++k) {
                newest = k == 0 ? order[k] : std::max(newest, order[k]);
                at.frames.push_back(inputs[order[k]].number);
                at.held.push_back(inputs[newest].holding ? 1 : 0);
            }
        }
        index.count = inputs.size();
        if (!inputs.empty())
            index.last = inputs.back();
    }

    InputIndex& held_index() {
        thread_local InputIndex idx;
        return idx;
    }

    InputIndex& synced_index(Session const& value) {
        auto& idx = held_index();
        auto const& inputs = value.inputs;
        bool kept = idx.owner == &inputs && idx.revision == value.inputs_revision && idx.count <= inputs.size();
        if (kept && idx.count > 0)
            kept = same_input(inputs[idx.count - 1], idx.last);
        if (kept && idx.count == inputs.size())
            kept = idx.data == inputs.data();
        if (!kept)
            rebuild_index(idx, inputs);
        for (size_t i = idx.count; i < inputs.size(); ++i) {
            if (append_input(idx, inputs[i]))
                continue;
            rebuild_index(idx, inputs);
            break;
        }
        idx.owner = &inputs;
        idx.data = inputs.data();
        idx.revision = value.inputs_revision;
        return idx;
    }
}

void Logic::release_replay_index() {
    held_index() = {};
}

bool Logic::held_at(Session const& value, bool player2, int frame, int button) {
    int const jump = static_cast<int>(PlayerButton::Jump);
    int want = button == 0 ? jump : button;
    int lane = lane_of(player2, want);
    if (lane < 0) {
        bool held = false;
        for (auto const& input : value.inputs) {
            int stored = input.button == 0 ? jump : input.button;
            if (input.player2 != player2 || stored != want || input.number > frame)
                continue;
            held = input.holding;
        }
        return held;
    }
    size_t slot = static_cast<size_t>(lane);
    if (value.live_seen[slot] && frame >= value.live_frame[slot])
        return value.live_held[slot] != 0;
    // playing a macro outside a 2 player level: a p2 input moves the same icon as a p1 input, so count them as 1
    if (value.is_playing() && !value.is_recording()) {
        auto* play = PlayLayer::get();
        if (play && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode)) {
            auto const& both = synced_index(value).lanes[6 + slot % 3];
            size_t after = both.cursor.seek(both.frames, frame);
            return after > 0 && both.held[after - 1] != 0;
        }
    }
    if (!value.is_playing()) {
        for (size_t i = value.inputs.size(); i-- > 0;) {
            auto const& input = value.inputs[i];
            int stored = input.button == 0 ? jump : input.button;
            if (input.player2 != player2 || stored != want || input.number > frame)
                continue;
            return input.holding;
        }
        return false;
    }
    auto const& at = synced_index(value).lanes[slot];
    size_t after = at.cursor.seek(at.frames, frame);
    return after > 0 && at.held[after - 1] != 0;
}

std::pair<int, int> Logic::count_holds(Session const& value) {
    return { value.hold_count, value.release_count };
}

bool Logic::inputs_sorted(Session const& value) {
    return value.inputs_in_order;
}

void note_live(Session& value, Frame const& frame) {
    value.newest_input = std::max(value.newest_input, frame.number);
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    int button = frame.button == 0 ? jump : frame.button;
    int slot = button == jump ? 0 : button == left ? 1
                                : button == right  ? 2
                                                   : -1;
    if (slot < 0)
        return;
    slot += frame.player2 ? 3 : 0;
    size_t lane = static_cast<size_t>(slot);
    if (value.live_seen[lane] && frame.number < value.live_frame[lane])
        return;
    value.live_frame[lane] = frame.number;
    value.live_held[lane] = frame.holding ? 1 : 0;
    value.live_seen[lane] = 1;
}

void grow_sides(Session& value, size_t index, bool appended) {
    auto grow = [&](auto& side, auto empty) {
        if (side.empty())
            return;
        if (side.size() + 1 != value.inputs.size()) {
            side.clear();
            return;
        }
        if (appended)
            side.push_back(empty);
        else
            side.insert(side.begin() + static_cast<std::ptrdiff_t>(index), empty);
    };
    grow(value.poses, FramePose{});
    grow(value.input_ids, uint64_t{ 0 });
    if (value.save_debug && value.poses.size() != value.inputs.size())
        value.poses.resize(value.inputs.size());
}

size_t Logic::add_input(Session& value, Frame const& frame) {
    (frame.holding ? value.hold_count : value.release_count) += 1;
    if (!value.sort_automatically || !(value).is_playing()) {
        if (!value.inputs.empty() && frame.number < value.inputs.back().number)
            value.inputs_in_order = false;
        value.inputs.push_back(frame);
        grow_sides(value, value.inputs.size() - 1, true);
        note_live(value, frame);
        return value.inputs.size() - 1;
    }
    auto later = std::upper_bound(value.inputs.begin(), value.inputs.end(), frame, [](Frame const& key, Frame const& element) {
        if (key.number != element.number)
            return key.number < element.number;
        if (key.player2 != element.player2)
            return key.player2 < element.player2;
        return key.button < element.button;
    });
    size_t idx = static_cast<size_t>(std::distance(value.inputs.begin(), later));
    value.inputs.insert(later, frame);
    grow_sides(value, idx, false);
    if (idx + 1 != value.inputs.size())
        value.touch_inputs();
    if (idx <= value.replay_pos)
        ++value.replay_pos;
    note_live(value, frame);
    return idx;
}

void Logic::refresh_live_hold() {
    auto& logic = Logic::get();
    logic.live_frame.fill(-1);
    logic.live_held.fill(0);
    logic.live_seen.fill(0);
    logic.newest_input = -1;
    logic.hold_count = 0;
    logic.release_count = 0;
    logic.inputs_in_order = true;
    int prev = -1;
    for (auto const& input : logic.inputs) {
        note_live(logic, input);
        (input.holding ? logic.hold_count : logic.release_count) += 1;
        if (prev >= 0 && input.number < prev)
            logic.inputs_in_order = false;
        prev = input.number;
    }
}

void Logic::record_input(GJBaseGameLayer* play, bool down, bool player1, int button, bool force) {
    auto& logic = Logic::get();
    if (!(logic).is_recording() || !play)
        return;
    if (logic.resetting || logic.refresh_only || logic.ignore_record_frames > 0)
        return;
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    if (button != 0 && button != jump && button != left && button != right)
        return;
    if (button == 0)
        button = jump;
    if (logic.mirror_click)
        return;
    bool player2 = !player1;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool folded = !twoPlayer && player2;
    if (folded) {
        player1 = true;
        player2 = false;
    }
    if (!logic.records_player(player2) && !(folded && logic.records_player(true)))
        return;
    int now = Logic::get().step_frame + 1;
    if (!(logic).is_playing()) {
        auto last = std::find_if(logic.inputs.rbegin(), logic.inputs.rend(), [player2](Frame const& input) {
            return input.player2 == player2;
        });
        if (last != logic.inputs.rend() && last->number > now)
            return;
    }
    if (!force && down == Logic::get().held_at(logic, player2, now, button)) {
        if (echo::step::tracing())
            echo::step::trace_event(fmt::format("SKIP frame={} down={} p2={} button={} (macro already has it {})\n", now, down, player2, button, down ? "held" : "released"));
        return;
    }
    if (echo::step::tracing())
        echo::step::trace_event(fmt::format("REC frame={} down={} p2={} button={} force={}\n", now, down, player2, button, force));

    Frame frame;
    frame.number = now;
    frame.holding = down;
    frame.player2 = player2;
    frame.button = static_cast<uint8_t>(button);
    size_t at = Logic::get().add_input(logic, frame);
    if (at != static_cast<size_t>(-1) && logic.save_debug) {
        PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
        logic.set_pose_at(at, Logic::get().capture_pose(body));
    }
    if (!logic.click_both || logic.routing_input || logic.maintaining_gravity || !logic.records_player(!player2))
        return;
    logic.routing_input = true;
    Logic::get().record_input(play, down, !player1, button, force);
    logic.routing_input = false;
}

void Logic::inject_button(PlayLayer* play, bool down, int button, bool player1) {
    auto& logic = Logic::get();
    logic.injecting = true;
    play->handleButton(down, button == 0 ? static_cast<int>(PlayerButton::Jump) : button, player1);
    logic.injecting = false;
}

void Logic::note_rule(std::vector<CpsNote>& notes, int first, int last, std::string text) {
    if (last < first)
        std::swap(first, last);
    if (!notes.empty() && notes.back().text == text && notes.back().last + 1 >= first) {
        notes.back().first = std::min(notes.back().first, first);
        notes.back().last = std::max(notes.back().last, last);
        return;
    }
    notes.push_back({ first, last, std::move(text) });
}

void Logic::eval_player_cps(std::vector<int> const& presses, bool player2, Session& value) {
    auto& notes = player2 ? value.cps_notes_p2 : value.cps_notes_p1;
    char const* who = player2 ? "P2" : "P1";
    if (presses.empty())
        return;
    std::vector<double> times(presses.size());
    std::ranges::transform(presses, times.begin(), [&](int frame) {
        return value.seconds_at(frame);
    });

    int idx = 0;
    while (idx < static_cast<int>(presses.size())) {
        int frame = presses[idx];
        int end = idx;
        while (end < static_cast<int>(presses.size()) && presses[end] == frame)
            ++end;
        if (end - idx > Logic::MAX_PER_FRAME)
            Logic::get().note_rule(notes, frame, frame, "Rule 2");
        idx = end;
    }

    int best = player2 ? 0 : 0;
    std::string best_text = fmt::format("{}: 0", who);
    int left = 0;
    for (int right = 0; right < static_cast<int>(presses.size()); ++right) {
        double end = times[static_cast<size_t>(right)];
        while (left <= right && end - times[static_cast<size_t>(left)] > 1.0)
            ++left;
        int amt = right - left + 1;
        if (amt > best) {
            best = amt;
            best_text = fmt::format(
                "{}: {} ({} to {})",
                who,
                amt,
                presses[left],
                presses[right]);
        }
        if (amt > value.max_cps)
            Logic::get().note_rule(notes, presses[left], presses[right], "Rule 1");
    }

    // 5 or more clicks faster than 48/s breaks the rule
    int n = static_cast<int>(presses.size());
    double rate = static_cast<double>(Logic::MAX_BURST_RATE);
    std::vector<double> lead(presses.size());
    std::vector<double> peak(presses.size());
    for (size_t s = 0; s < presses.size(); ++s) {
        lead[s] = rate * times[s] - static_cast<double>(s);
        peak[s] = s == 0 ? lead[s] : std::max(peak[s - 1], lead[s]);
    }
    for (int end = Logic::MIN_BURST_CLICKS - 1; end < n; ++end) {
        int last_start = end - (Logic::MIN_BURST_CLICKS - 1);
        double need = rate * times[static_cast<size_t>(end)] - static_cast<double>(end) - 1.0;
        double slack = 1e-6 + 1e-12 * std::abs(need);
        auto first = std::upper_bound(peak.begin(), peak.begin() + last_start + 1, need - slack);
        for (int start = static_cast<int>(std::distance(peak.begin(), first)); start <= last_start; ++start) {
            double margin = lead[static_cast<size_t>(start)] - need;
            if (margin <= -slack)
                continue;
            int amt = end - start + 1;
            if (margin > slack || Logic::get().burst_too_fast(value, amt, presses[start], presses[end])) {
                Logic::get().note_rule(notes, presses[start], presses[end], "Rule 3");
                break;
            }
        }
    }

    if (player2) {
        if (best > 0 && (value.highest_cps.rfind("P2:", 0) == 0 || best_text > value.highest_cps)) {
        }
    }
    int current_best = 0;
    auto colon = value.highest_cps.find(": ");
    if (colon != std::string::npos)
        current_best = std::atoi(value.highest_cps.c_str() + colon + 2);
    if (best >= current_best)
        value.highest_cps = best_text;
}

namespace {
    struct CpsResult {
        std::vector<CpsNote> p1, p2;
        std::string highest;
    };

    CpsResult scan_cps(std::vector<int> const& p1, std::vector<int> const& p2, float macro_fps, std::vector<RateChange> rates, int max_cps) {
        Session scan;
        scan.macro_fps = macro_fps;
        scan.fps = macro_fps;
        scan.max_cps = std::max(max_cps, 1);
        scan.rates = std::move(rates);
        scan.highest_cps = "P1: 0";
        Logic::live().eval_player_cps(p1, false, scan);
        Logic::live().eval_player_cps(p2, true, scan);
        return { std::move(scan.cps_notes_p1), std::move(scan.cps_notes_p2), std::move(scan.highest_cps) };
    }

    std::pair<std::vector<int>, std::vector<int>> split_presses(std::vector<Frame> const& inputs) {
        std::pair<std::vector<int>, std::vector<int>> presses;
        for (auto const& input : inputs) {
            if (!input.holding)
                continue;
            (input.player2 ? presses.second : presses.first).push_back(input.number);
        }
        return presses;
    }

    void take_cps(Session& value, CpsResult result) {
        value.cps_notes_p1 = std::move(result.p1);
        value.cps_notes_p2 = std::move(result.p2);
        value.highest_cps = std::move(result.highest);
    }
}

// runs on a worker so big macros dont freeze the game
void Logic::recompute_cps(Session& value) {
    auto [p1, p2] = split_presses(value.inputs);
    if (&value != &Logic::live()) {
        take_cps(value, scan_cps(p1, p2, value.macro_fps, value.rates, value.max_cps));
        return;
    }
    static std::atomic<uint64_t> ticket{ 0 };
    uint64_t mine = ticket.fetch_add(1) + 1;
    value.cps_notes_p1.clear();
    value.cps_notes_p2.clear();
    value.highest_cps = "Checking CPS...";
    try {
        std::thread([p1 = std::move(p1), p2 = std::move(p2), fps = value.macro_fps, rates = value.rates, max_cps = value.max_cps, mine] {
            std::optional<CpsResult> res;
            try {
                res = scan_cps(p1, p2, fps, rates, max_cps);
            }
            catch (...) {
                res.reset();
            }
            if (!res)
                return;
            geode::queueInMainThread([res = std::move(*res), mine]() mutable {
                if (mine != ticket.load())
                    return;
                take_cps(Logic::live(), std::move(res));
            });
        }).detach();
    }
    catch (...) {
        auto presses = split_presses(value.inputs);
        take_cps(value, scan_cps(presses.first, presses.second, value.macro_fps, value.rates, value.max_cps));
    }
}
