// recording
//
// record_input is called from gds button handler. the input is stored for step_frame + 1 bc gd counts a step BEFORE it reads the button
// pressing a button thats already held is not a new input. the latest hold per player & button is kept so that check is instant
// in a 1 player dual 1 button moves both icons, thats stored as p1 even when the 2nd icon is the one being recorded
// recording over part of a macro trims everything after that point for the recorded player & leaves the other players inputs
// capture_player / apply_player copy a players whole state. setting the flags alone leaves the sprite in the wrong mode, so it goes thru gds setters
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
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

PlayerSave Logic::capture_player(PlayerObject* player) {
    PlayerSave shot;
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

void Logic::apply_player(PlayerObject* player, PlayerSave const& shot) {
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
    // TODO mini cube respawns full size
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
