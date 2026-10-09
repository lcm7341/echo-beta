#include "Logic/logic.hpp"
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

FramePose Logic::capturePose(PlayerObject* player) {
	FramePose pose;
	if (!player) return pose;
	pose.x = player->getPositionX();
	pose.y = player->getPositionY();
	pose.rotation = player->getRotation();
	pose.yVelocity = player->m_yVelocity;
	pose.xVelocity = static_cast<double>(player->m_playerSpeed);
	return pose;
}
bool Logic::flipControls() {
	auto* manager = GameManager::get();
	return manager && manager->getGameVariable("0010");
}
void Logic::setReplayPos(PlayLayer* play) {
	auto& value = Logic::get();
	int frame = play ? Logic::get().getFrame(play) : 0;
	auto later = value.inputsInOrder
		? std::lower_bound(value.inputs.begin(), value.inputs.end(), frame,
			[](Frame const& input, int key) { return input.number < key; })
		: std::find_if(value.inputs.begin(), value.inputs.end(), [frame](Frame const& input) {
			return input.number >= frame;
		});
	value.replayPos = static_cast<size_t>(std::distance(value.inputs.begin(), later));
	value.holdSync = true;
}
void Logic::trimRecordedInputs(Session& value, int frame) {
	bool record1 = value.recordsPlayer(false);
	bool record2 = value.recordsPlayer(true);
	if (!record1 && !record2) return;
	replay::compact(value.inputs, value.poses, value.inputIds, [&](Frame const& input) {
		bool matches = input.player2 ? record2 : record1;
		bool later = frame <= 0 ? input.number >= frame : input.number > frame;
		return !(matches && later);
	});
	value.touchInputs();
	Logic::get().refreshLiveHold();
}
void Logic::trimFreshAttempt(PlayLayer* play, Session& value) {
	(void)play;
	Logic::get().trimRecordedInputs(value, 0);
	Logic::get().trimRates(0);
	value.removedFrames = 0;
	value.replayPos = 0;
}
PlayerSnapshot Logic::capturePlayer(PlayerObject* player) {
	PlayerSnapshot shot;
	if (!player) return shot;
	shot.present = true;
	shot.x = player->getPositionX();
	shot.y = player->getPositionY();
	shot.rotation = echo::rotation::gameplay(player);
	shot.visualRotation = echo::rotation::visual(player);
	shot.yVelocity = player->m_yVelocity;
	shot.platformerXVelocity = player->m_platformerXVelocity;
	shot.yVelocityRelated = player->m_yVelocityRelated;
	shot.fallSpeed = player->m_fallSpeed;
	shot.gravity = player->m_gravity;
	shot.speedMultiplier = player->m_speedMultiplier;
	shot.yStart = player->m_yStart;
	shot.maybeReverseSpeed = player->m_maybeReverseSpeed;
	shot.maybeReverseAcceleration = player->m_maybeReverseAcceleration;
	shot.accelerationOrSpeed = player->m_accelerationOrSpeed;
	shot.snapDistance = player->m_snapDistance;
	shot.lastFlipTime = player->m_lastFlipTime;
	shot.gameModeChangedTime = player->m_gameModeChangedTime;
	shot.lastLandTime = player->m_lastLandTime;
	shot.totalTime = player->m_totalTime;
	shot.playerSpeed = player->m_playerSpeed;
	shot.playerSpeedAC = player->m_playerSpeedAC;
	shot.vehicleSize = player->m_vehicleSize;
	shot.gravityMod = player->m_gravityMod;
	shot.xVelocityRelated = player->m_xVelocityRelated;
	shot.xVelocityRelated2 = player->m_xVelocityRelated2;
	shot.yVelocityRelated3 = player->m_yVelocityRelated3;
	shot.rotationSpeed = player->m_rotationSpeed;
	shot.rotateSpeed = player->m_rotateSpeed;
	shot.rotating = player->m_isRotating;
	shot.ballRotating = player->m_isBallRotating;
	shot.ballRotating2 = player->m_isBallRotating2;
	shot.slopeRotation = player->m_slopeRotation;
	shot.slopeVelocity = player->m_slopeVelocity;
	shot.shipRotation = player->m_shipRotation;
	shot.lastPortalPos = player->m_lastPortalPos;
	shot.ship = player->m_isShip;
	shot.bird = player->m_isBird;
	shot.ball = player->m_isBall;
	shot.dart = player->m_isDart;
	shot.robot = player->m_isRobot;
	shot.spider = player->m_isSpider;
	shot.swing = player->m_isSwing;
	shot.upsideDown = player->m_isUpsideDown;
	shot.onGround = player->m_isOnGround;
	shot.onGround2 = player->m_isOnGround2;
	shot.onGround3 = player->m_isOnGround3;
	shot.onGround4 = player->m_isOnGround4;
	shot.goingLeft = player->m_isGoingLeft;
	shot.sideways = player->m_isSideways;
	shot.hidden = player->m_isHidden;
	shot.dashing = player->m_isDashing;
	shot.dead = player->m_isDead;
	shot.jumpBuffered = player->m_jumpBuffered;
	shot.onSlope = player->m_isOnSlope;
	shot.wasOnSlope = player->m_wasOnSlope;
	shot.wasTeleported = player->m_wasTeleported;
	shot.fixGravityBug = player->m_fixGravityBug;
	shot.reverseSync = player->m_reverseSync;
	shot.locked = player->m_isLocked;
	shot.controlsDisabled = player->m_controlsDisabled;
	shot.holdingRight = player->m_holdingRight;
	shot.holdingLeft = player->m_holdingLeft;
	shot.reverseRelated = player->m_reverseRelated;
	shot.leftPressedFirst = player->m_leftPressedFirst;
	shot.movingLeft = player->m_platformerMovingLeft;
	shot.movingRight = player->m_platformerMovingRight;
	shot.platformerVelocityRelated = player->m_platformerVelocityRelated;
	shot.boosted = player->m_maybeIsBoosted;
	shot.scaleXRelatedTime = player->m_scaleXRelatedTime;
	shot.decreaseBoostSlide = player->m_decreaseBoostSlide;
	shot.scaleXRelated = player->m_scaleXRelated;
	shot.scaleXRelated2 = player->m_scaleXRelated2;
	shot.scaleXRelated3 = player->m_scaleXRelated3;
	shot.scaleXRelated4 = player->m_scaleXRelated4;
	shot.scaleXRelated5 = player->m_scaleXRelated5;
	shot.hasStopped = player->m_maybeHasStopped;
	shot.correctSlopeDirection = player->m_maybeGoingCorrectSlopeDirection;
	shot.sliding = player->m_isSliding;
	shot.slopeForce = player->m_maybeSlopeForce;
	shot.onIce = player->m_isOnIce;
	shot.physDeltaRelated = player->m_physDeltaRelated;
	shot.slidingTime = player->m_maybeSlidingTime;
	shot.slidingStartTime = player->m_maybeSlidingStartTime;
	shot.changedDirectionsTime = player->m_changedDirectionsTime;
	shot.slopeEndTime = player->m_slopeEndTime;
	shot.moving = player->m_isMoving;
	shot.slidingRight = player->m_isSlidingRight;
	shot.changedDirectionAngle = player->m_maybeChangedDirectionAngle;
	shot.groundYVelocity = player->m_groundYVelocity;
	shot.yVelocityBeforeSlope = player->m_yVelocityBeforeSlope;
	shot.slopeStartTime = player->m_slopeStartTime;
	shot.lastGroundedPos = player->m_lastGroundedPos;
	shot.accelerating = player->m_isAccelerating;
	shot.groundMaterial = player->m_groundObjectMaterial;
	shot.playerSpeedTime = player->m_somethingPlayerSpeedTime;
	shot.affectedByForces = player->m_affectedByForces;
	shot.stateForce = player->m_stateForce;
	shot.stateForceVector = player->m_stateForceVector;
	shot.stateBoostX = player->m_stateBoostX;
	shot.stateBoostY = player->m_stateBoostY;
	shot.stateFlipGravity = player->m_stateFlipGravity;
	shot.stateScale = player->m_stateScale;
	shot.stateDartSlide = player->m_stateDartSlide;
	shot.stateNoAutoJump = player->m_stateNoAutoJump;
	shot.stateRingJump = player->m_stateRingJump;
	shot.stateRingJump2 = player->m_stateRingJump2;
	shot.wasJumpBuffered = player->m_wasJumpBuffered;
	shot.stateJumpBuffered = player->m_stateJumpBuffered;
	shot.ringJumpRelated = player->m_ringJumpRelated;
	shot.touchedRing = player->m_touchedRing;
	shot.touchedCustomRing = player->m_touchedCustomRing;
	shot.touchedPad = player->m_touchedPad;
	shot.wasRobotJump = player->m_wasRobotJump;
	shot.blackOrbRelated = player->m_blackOrbRelated;
	shot.dashX = player->m_dashX;
	shot.dashY = player->m_dashY;
	shot.dashAngle = player->m_dashAngle;
	shot.dashStartTime = player->m_dashStartTime;
	shot.dashRing = player->m_dashRing;
	shot.spriteWidthScale = player->m_spriteWidthScale;
	shot.spriteHeightScale = player->m_spriteHeightScale;
	shot.scaleX = player->getScaleX();
	shot.scaleY = player->getScaleY();
	shot.internalPosition = player->m_position;
	shot.lastPosition = player->m_lastPosition;
	return shot;
}
void Logic::applyPlayer(PlayerObject* player, PlayerSnapshot const& shot) {
	if (!player || !shot.present) return;
	// Assigning mode/gravity flags alone leaves native sprites and orientation stale.
	// Mode setters may change velocities and rotation, so run them before exact state restoration.
	auto mode = [&](bool active, bool saved, auto toggle) {
		if (active && !saved) (player->*toggle)(false, true);
	};
	mode(player->m_isShip, shot.ship, &PlayerObject::toggleFlyMode);
	mode(player->m_isBird, shot.bird, &PlayerObject::toggleBirdMode);
	mode(player->m_isBall, shot.ball, &PlayerObject::toggleRollMode);
	mode(player->m_isDart, shot.dart, &PlayerObject::toggleDartMode);
	mode(player->m_isRobot, shot.robot, &PlayerObject::toggleRobotMode);
	mode(player->m_isSpider, shot.spider, &PlayerObject::toggleSpiderMode);
	mode(player->m_isSwing, shot.swing, &PlayerObject::toggleSwingMode);
	// Disable the previous vehicle before enabling the saved one, in either direction.
	if (shot.ship && !player->m_isShip) player->toggleFlyMode(true, true);
	if (shot.bird && !player->m_isBird) player->toggleBirdMode(true, true);
	if (shot.ball && !player->m_isBall) player->toggleRollMode(true, true);
	if (shot.dart && !player->m_isDart) player->toggleDartMode(true, true);
	if (shot.robot && !player->m_isRobot) player->toggleRobotMode(true, true);
	if (shot.spider && !player->m_isSpider) player->toggleSpiderMode(true, true);
	if (shot.swing && !player->m_isSwing) player->toggleSwingMode(true, true);
	if (player->m_isUpsideDown != shot.upsideDown) player->flipGravity(shot.upsideDown, true);
	// The checkpoint load can leave the size value right and the sprite at the other size (a mini
	// cube respawned drawn full size). Set the opposite size first so the toggle always rescales.
	{
		bool mini = shot.vehicleSize != 1.f;
		player->m_vehicleSize = mini ? 1.f : 0.6f;
		player->togglePlayerScale(mini, true);
	}
	player->setPosition({shot.x, shot.y});
	player->setRotation(shot.rotation);
	player->m_yVelocity = shot.yVelocity;
	player->m_platformerXVelocity = shot.platformerXVelocity;
	player->m_yVelocityRelated = shot.yVelocityRelated;
	player->m_fallSpeed = shot.fallSpeed;
	player->m_gravity = shot.gravity;
	player->m_speedMultiplier = shot.speedMultiplier;
	player->m_yStart = shot.yStart;
	player->m_maybeReverseSpeed = shot.maybeReverseSpeed;
	player->m_maybeReverseAcceleration = shot.maybeReverseAcceleration;
	player->m_accelerationOrSpeed = shot.accelerationOrSpeed;
	player->m_snapDistance = shot.snapDistance;
	player->m_lastFlipTime = shot.lastFlipTime;
	player->m_gameModeChangedTime = shot.gameModeChangedTime;
	player->m_lastLandTime = shot.lastLandTime;
	player->m_totalTime = shot.totalTime;
	player->m_playerSpeed = shot.playerSpeed;
	player->m_playerSpeedAC = shot.playerSpeedAC;
	player->m_vehicleSize = shot.vehicleSize;
	player->m_gravityMod = shot.gravityMod;
	player->m_xVelocityRelated = shot.xVelocityRelated;
	player->m_xVelocityRelated2 = shot.xVelocityRelated2;
	player->m_yVelocityRelated3 = shot.yVelocityRelated3;
	player->m_rotationSpeed = shot.rotationSpeed;
	player->m_rotateSpeed = shot.rotateSpeed;
	player->m_isRotating = shot.rotating;
	player->m_isBallRotating = shot.ballRotating;
	player->m_isBallRotating2 = shot.ballRotating2;
	player->m_slopeRotation = shot.slopeRotation;
	player->m_slopeVelocity = shot.slopeVelocity;
	player->m_shipRotation = shot.shipRotation;
	player->m_lastPortalPos = shot.lastPortalPos;
	player->m_isShip = shot.ship;
	player->m_isBird = shot.bird;
	player->m_isBall = shot.ball;
	player->m_isDart = shot.dart;
	player->m_isRobot = shot.robot;
	player->m_isSpider = shot.spider;
	player->m_isSwing = shot.swing;
	player->m_isUpsideDown = shot.upsideDown;
	player->m_jumpBuffered = shot.jumpBuffered;
	player->m_isOnGround = shot.onGround;
	player->m_isOnGround2 = shot.onGround2;
	player->m_isOnGround3 = shot.onGround3;
	player->m_isOnGround4 = shot.onGround4;
	player->m_isGoingLeft = shot.goingLeft;
	player->m_isSideways = shot.sideways;
	player->m_isHidden = shot.hidden;
	player->m_isDashing = shot.dashing;
	player->m_isDead = shot.dead;
	// The checkpoint was saved while a button was down. Putting that hold back ignores a release
	// that happened before the restart. The restart applies whatever button is down now.
	player->m_isOnSlope = shot.onSlope;
	player->m_wasOnSlope = shot.wasOnSlope;
	player->m_wasTeleported = shot.wasTeleported;
	player->m_fixGravityBug = shot.fixGravityBug;
	player->m_reverseSync = shot.reverseSync;
	player->m_isLocked = shot.locked;
	player->m_controlsDisabled = shot.controlsDisabled;
	player->m_reverseRelated = shot.reverseRelated;
	player->m_leftPressedFirst = shot.leftPressedFirst;
	player->m_platformerMovingLeft = shot.movingLeft;
	player->m_platformerMovingRight = shot.movingRight;
	player->m_platformerVelocityRelated = shot.platformerVelocityRelated;
	player->m_maybeIsBoosted = shot.boosted;
	player->m_scaleXRelatedTime = shot.scaleXRelatedTime;
	player->m_decreaseBoostSlide = shot.decreaseBoostSlide;
	player->m_scaleXRelated = shot.scaleXRelated;
	player->m_scaleXRelated2 = shot.scaleXRelated2;
	player->m_scaleXRelated3 = shot.scaleXRelated3;
	player->m_scaleXRelated4 = shot.scaleXRelated4;
	player->m_scaleXRelated5 = shot.scaleXRelated5;
	player->m_maybeHasStopped = shot.hasStopped;
	player->m_maybeGoingCorrectSlopeDirection = shot.correctSlopeDirection;
	player->m_isSliding = shot.sliding;
	player->m_maybeSlopeForce = shot.slopeForce;
	player->m_isOnIce = shot.onIce;
	player->m_physDeltaRelated = shot.physDeltaRelated;
	player->m_maybeSlidingTime = shot.slidingTime;
	player->m_maybeSlidingStartTime = shot.slidingStartTime;
	player->m_changedDirectionsTime = shot.changedDirectionsTime;
	player->m_slopeEndTime = shot.slopeEndTime;
	player->m_isMoving = shot.moving;
	player->m_isSlidingRight = shot.slidingRight;
	player->m_maybeChangedDirectionAngle = shot.changedDirectionAngle;
	player->m_groundYVelocity = shot.groundYVelocity;
	player->m_yVelocityBeforeSlope = shot.yVelocityBeforeSlope;
	player->m_slopeStartTime = shot.slopeStartTime;
	player->m_lastGroundedPos = shot.lastGroundedPos;
	player->m_isAccelerating = shot.accelerating;
	player->m_groundObjectMaterial = shot.groundMaterial;
	player->m_somethingPlayerSpeedTime = shot.playerSpeedTime;
	player->m_affectedByForces = shot.affectedByForces;
	player->m_stateForce = shot.stateForce;
	player->m_stateForceVector = shot.stateForceVector;
	player->m_stateBoostX = shot.stateBoostX;
	player->m_stateBoostY = shot.stateBoostY;
	player->m_stateFlipGravity = shot.stateFlipGravity;
	player->m_stateScale = shot.stateScale;
	player->m_stateDartSlide = shot.stateDartSlide;
	player->m_stateNoAutoJump = shot.stateNoAutoJump;
	player->m_stateRingJump = shot.stateRingJump;
	player->m_stateRingJump2 = shot.stateRingJump2;
	player->m_wasJumpBuffered = shot.wasJumpBuffered;
	player->m_stateJumpBuffered = shot.stateJumpBuffered;
	player->m_ringJumpRelated = shot.ringJumpRelated;
	player->m_touchedRing = shot.touchedRing;
	player->m_touchedCustomRing = shot.touchedCustomRing;
	player->m_touchedPad = shot.touchedPad;
	player->m_wasRobotJump = shot.wasRobotJump;
	player->m_blackOrbRelated = shot.blackOrbRelated;
	player->m_dashX = shot.dashX;
	player->m_dashY = shot.dashY;
	player->m_dashAngle = shot.dashAngle;
	player->m_dashStartTime = shot.dashStartTime;
	player->m_dashRing = shot.dashRing;
	player->m_spriteWidthScale = shot.spriteWidthScale;
	player->m_spriteHeightScale = shot.spriteHeightScale;
	player->setScaleX(shot.scaleX);
	player->setScaleY(shot.scaleY);
	// m_position is not physics state: GD stores the node's position there at the end of each update
	// (update+0x1003) and puts the node back on it at the start of the next (update+0x19c). A
	// checkpoint made mid-update holds the previous update's value, which trails the player, so
	// restoring it moved the player back on the first update after a respawn and replays of
	// practice-mode recordings drifted. The node's own position is what the next update must find.
	player->m_position = cocos2d::CCPoint(shot.x, shot.y);
	player->m_lastPosition = shot.lastPosition;
	echo::rotation::restore(player, shot.rotation, shot.visualRotation);
}
LayerSnapshot Logic::captureLayer(PlayLayer* play) {
	LayerSnapshot shot;
	auto const& state = play->m_gameState;
	shot.levelTime = state.m_levelTime;
	shot.totalTime = state.m_totalTime;
	shot.unkTime = state.m_unkDouble3;
	shot.attemptTime = play->m_attemptTime;
	shot.progress = state.m_currentProgress;
	shot.timeWarp = state.m_timeWarp;
	shot.queuedTimeWarp = state.m_queuedTimeWarp;
	shot.timeWarpRelated = state.m_timeWarpRelated;
	shot.timeMod = state.m_timeModRelated;
	shot.cameraAngle = state.m_cameraAngle;
	shot.levelFlipping = state.m_levelFlipping;
	shot.cameraPosition = state.m_cameraPosition;
	shot.dual = state.m_isDualMode;
	shot.streakBlend = state.m_playerStreakBlend;
	shot.gravityRelated = state.m_gravityRelated;
	shot.timeModRelated = state.m_timeModRelated2;
	return shot;
}
void Logic::applyLayer(PlayLayer* play, LayerSnapshot const& shot) {
	auto& state = play->m_gameState;
	state.m_levelTime = shot.levelTime;
	state.m_totalTime = shot.totalTime;
	state.m_unkDouble3 = shot.unkTime;
	play->m_attemptTime = shot.attemptTime;
	state.m_currentProgress = shot.progress;
	state.m_timeWarp = shot.timeWarp;
	state.m_queuedTimeWarp = shot.queuedTimeWarp;
	state.m_timeWarpRelated = shot.timeWarpRelated;
	state.m_timeModRelated = shot.timeMod;
	state.m_cameraAngle = shot.cameraAngle;
	state.m_levelFlipping = shot.levelFlipping;
	state.m_cameraPosition = shot.cameraPosition;
	state.m_isDualMode = shot.dual;
	state.m_playerStreakBlend = shot.streakBlend;
	state.m_gravityRelated = shot.gravityRelated;
	state.m_timeModRelated2 = shot.timeModRelated;
}
void Logic::trimFromCheckpoint(PlayLayer* play, Session& value, Checkpoint const& checkpoint) {
	value.stepFrame = checkpoint.frame;
	Logic::get().applyLayer(play, checkpoint.layer);
	Logic::get().applyPlayer(play->m_player1, checkpoint.player1);
	Logic::get().applyPlayer(play->m_player2, checkpoint.player2);
	if ((value).isRecording()) {
		int frame = Logic::get().getFrame(play);
		Logic::get().trimRecordedInputs(value, frame);
		Logic::get().trimRates(frame);
		Logic::get().sortInputs();
		value.replayPos = value.inputs.size();
	}
}
void Logic::clearPhysicalHolds() {
	auto& value = Logic::get();
	value.keyJumpP1 = value.keyJumpP2 = false;
	value.steeredKeys.clear();
	value.keyLeftP1 = value.keyRightP1 = false;
	value.keyLeftP2 = value.keyRightP2 = false;
	value.deathJumpP1 = value.deathJumpP2 = false;
	value.deathLeftP1 = value.deathRightP1 = false;
	value.deathLeftP2 = value.deathRightP2 = false;
	value.physicalJumpP1 = value.physicalJumpP2 = false;
	value.physicalLeftP1 = value.physicalRightP1 = false;
	value.physicalLeftP2 = value.physicalRightP2 = false;
}
namespace {
// One lane per player and button. A lookup is the last input in list order at or before a frame,
// so each lane keeps its frames ascending with the answer for every prefix.
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
	std::array<HeldLane, 6> lanes;
};

int laneOf(bool player2, int button) {
	int const jump = static_cast<int>(PlayerButton::Jump);
	int const left = static_cast<int>(PlayerButton::Left);
	int const right = static_cast<int>(PlayerButton::Right);
	int slot = button == 0 || button == jump ? 0 : button == left ? 1 : button == right ? 2 : -1;
	if (slot < 0) return -1;
	return (player2 ? 3 : 0) + slot;
}

bool sameInput(Frame const& a, Frame const& b) {
	return a.number == b.number && a.holding == b.holding && a.player2 == b.player2 && a.button == b.button;
}

bool appendInput(InputIndex& index, Frame const& input) {
	int lane = laneOf(input.player2, input.button);
	if (lane >= 0) {
		auto& at = index.lanes[static_cast<size_t>(lane)];
		if (!at.frames.empty() && input.number < at.frames.back()) return false;
		at.frames.push_back(input.number);
		at.held.push_back(input.holding ? 1 : 0);
	}
	if (index.count > 0 && input.number < index.last.number) index.sorted = false;
	(input.holding ? index.holds : index.releases) += 1;
	index.last = input;
	++index.count;
	return true;
}

void rebuildIndex(InputIndex& index, std::vector<Frame> const& inputs) {
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
	bool inOrder = std::ranges::all_of(inputs, [&](Frame const& input) { return appendInput(index, input); });
	if (inOrder) return;

	reset();
	std::array<std::vector<size_t>, 6> members;
	for (size_t i = 0; i < inputs.size(); ++i) {
		auto const& input = inputs[i];
		int lane = laneOf(input.player2, input.button);
		if (lane >= 0) members[static_cast<size_t>(lane)].push_back(i);
		if (i > 0 && input.number < inputs[i - 1].number) index.sorted = false;
		(input.holding ? index.holds : index.releases) += 1;
	}
	for (size_t lane = 0; lane < members.size(); ++lane) {
		auto& order = members[lane];
		std::ranges::stable_sort(order, {}, [&](size_t i) { return inputs[i].number; });
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
	if (!inputs.empty()) index.last = inputs.back();
}

InputIndex& heldIndex() {
	thread_local InputIndex index;
	return index;
}

InputIndex& syncedIndex(Session const& value) {
	auto& index = heldIndex();
	auto const& inputs = value.inputs;
	bool kept = index.owner == &inputs && index.revision == value.inputsRevision && index.count <= inputs.size();
	if (kept && index.count > 0) kept = sameInput(inputs[index.count - 1], index.last);
	if (kept && index.count == inputs.size()) kept = index.data == inputs.data();
	if (!kept) rebuildIndex(index, inputs);
	for (size_t i = index.count; i < inputs.size(); ++i) {
		if (appendInput(index, inputs[i])) continue;
		rebuildIndex(index, inputs);
		break;
	}
	index.owner = &inputs;
	index.data = inputs.data();
	index.revision = value.inputsRevision;
	return index;
}
}

void Logic::releaseReplayIndex() {
	heldIndex() = {};
}

bool Logic::heldAt(Session const& value, bool player2, int frame, int button) {
	int const jump = static_cast<int>(PlayerButton::Jump);
	int want = button == 0 ? jump : button;
	int lane = laneOf(player2, want);
	if (lane < 0) {
		bool held = false;
		for (auto const& input : value.inputs) {
			int stored = input.button == 0 ? jump : input.button;
			if (input.player2 != player2 || stored != want || input.number > frame) continue;
			held = input.holding;
		}
		return held;
	}
	size_t slot = static_cast<size_t>(lane);
	if (value.liveSeen[slot] && frame >= value.liveFrame[slot]) return value.liveHeld[slot] != 0;
	if (!value.isPlaying()) {
		for (size_t i = value.inputs.size(); i-- > 0;) {
			auto const& input = value.inputs[i];
			int stored = input.button == 0 ? jump : input.button;
			if (input.player2 != player2 || stored != want || input.number > frame) continue;
			return input.holding;
		}
		return false;
	}
	auto const& at = syncedIndex(value).lanes[slot];
	size_t after = at.cursor.seek(at.frames, frame);
	return after > 0 && at.held[after - 1] != 0;
}
std::pair<int, int> Logic::countHolds(Session const& value) {
	return {value.holdCount, value.releaseCount};
}
bool Logic::inputsSorted(Session const& value) {
	return value.inputsInOrder;
}
void noteLive(Session& value, Frame const& frame) {
	value.newestInput = std::max(value.newestInput, frame.number);
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	int button = frame.button == 0 ? jump : frame.button;
	int slot = button == jump ? 0 : button == left ? 1 : button == right ? 2 : -1;
	if (slot < 0) return;
	slot += frame.player2 ? 3 : 0;
	size_t lane = static_cast<size_t>(slot);
	if (value.liveSeen[lane] && frame.number < value.liveFrame[lane]) return;
	value.liveFrame[lane] = frame.number;
	value.liveHeld[lane] = frame.holding ? 1 : 0;
	value.liveSeen[lane] = 1;
}
void growSides(Session& value, size_t index, bool appended) {
	auto grow = [&](auto& side, auto empty) {
		if (side.empty()) return;
		if (side.size() + 1 != value.inputs.size()) {
			side.clear();
			return;
		}
		if (appended) side.push_back(empty);
		else side.insert(side.begin() + static_cast<std::ptrdiff_t>(index), empty);
	};
	grow(value.poses, FramePose{});
	grow(value.inputIds, uint64_t{0});
	if (value.saveDebug && value.poses.size() != value.inputs.size()) value.poses.resize(value.inputs.size());
}
size_t Logic::placeRecorded(Session& value, Frame const& frame) {
	(frame.holding ? value.holdCount : value.releaseCount) += 1;
	if (!value.sortAutomatically || !(value).isPlaying()) {
		if (!value.inputs.empty() && frame.number < value.inputs.back().number) value.inputsInOrder = false;
		value.inputs.push_back(frame);
		growSides(value, value.inputs.size() - 1, true);
		noteLive(value, frame);
		return value.inputs.size() - 1;
	}
	auto later = std::upper_bound(value.inputs.begin(), value.inputs.end(), frame, [](Frame const& key, Frame const& element) {
		if (key.number != element.number) return key.number < element.number;
		if (key.player2 != element.player2) return key.player2 < element.player2;
		return key.button < element.button;
	});
	size_t index = static_cast<size_t>(std::distance(value.inputs.begin(), later));
	value.inputs.insert(later, frame);
	growSides(value, index, false);
	if (index + 1 != value.inputs.size()) value.touchInputs();
	if (index <= value.replayPos) ++value.replayPos;
	noteLive(value, frame);
	return index;
}
void Logic::refreshLiveHold() {
	auto& value = Logic::get();
	value.liveFrame.fill(-1);
	value.liveHeld.fill(0);
	value.liveSeen.fill(0);
	value.newestInput = -1;
	value.holdCount = 0;
	value.releaseCount = 0;
	value.inputsInOrder = true;
	int previous = -1;
	for (auto const& input : value.inputs) {
		noteLive(value, input);
		(input.holding ? value.holdCount : value.releaseCount) += 1;
		if (previous >= 0 && input.number < previous) value.inputsInOrder = false;
		previous = input.number;
	}
}
void Logic::recordInput(GJBaseGameLayer* play, bool down, bool player1, int button, bool force) {
	auto& value = Logic::get();
	if (!(value).isRecording() || !play) return;
	if (value.resetting || value.refreshOnly || value.ignoreRecordFrames > 0) return;
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	if (button != 0 && button != jump && button != left && button != right) return;
	if (button == 0) button = jump;
	if (value.mirrorClick) return;
	bool player2 = !player1;
	// Outside two-player levels every press drives player 1: GD plays the second player's keys
	// (up arrow, the right side) as player 1, in a dual section both icons click from one button,
	// and Android may deliver a dual exit's touch-up to the second icon. Recorded as player 2, the
	// press did nothing on playback in a one-player level.
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool folded = !twoPlayer && player2;
	if (folded) {
		player1 = true;
		player2 = false;
	}
	if (!value.recordsPlayer(player2) && !(folded && value.recordsPlayer(true))) return;
	// The game counts this step before it reads the button, and stepFrame advances after.
	// Storing the current stepFrame puts the click one frame before the game sees it.
	int now = Logic::get().stepFrame + 1;
	if (!(value).isPlaying() && !value.inputs.empty() && value.inputs.back().number > now) return;
	// Pad/tool swifts must land even when the hold state already matches (e.g. press while held).
	if (!force && down == Logic::get().heldAt(value, player2, now, button)) return;

	Frame frame;
	frame.number = now;
	frame.holding = down;
	frame.player2 = player2;
	frame.button = static_cast<uint8_t>(button);
	size_t at = Logic::get().placeRecorded(value, frame);
	if (at != static_cast<size_t>(-1) && value.saveDebug) {
		PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
		value.setPoseAt(at, Logic::get().capturePose(body));
	}
	if (!value.clickBoth || value.routingInput || value.maintainingGravity || !value.recordsPlayer(!player2)) return;
	value.routingInput = true;
	Logic::get().recordInput(play, down, !player1, button, force);
	value.routingInput = false;
}
void Logic::injectButton(PlayLayer* play, bool down, int button, bool player1) {
	auto& value = Logic::get();
	value.injecting = true;
	play->handleButton(down, button == 0 ? static_cast<int>(PlayerButton::Jump) : button, player1);
	value.injecting = false;
}
void Logic::noteRule(std::vector<CpsNote>& notes, int first, int last, std::string text) {
	if (last < first) std::swap(first, last);
	// Overlapping or touching breaks of the same rule are one stretch, so the editor can open the whole run.
	if (!notes.empty() && notes.back().text == text && notes.back().last + 1 >= first) {
		notes.back().first = std::min(notes.back().first, first);
		notes.back().last = std::max(notes.back().last, last);
		return;
	}
	notes.push_back({first, last, std::move(text)});
}
void Logic::evalPlayerCps(std::vector<int> const& presses, bool player2, Session& value) {
	auto& notes = player2 ? value.cpsNotesP2 : value.cpsNotesP1;
	char const* who = player2 ? "P2" : "P1";
	if (presses.empty()) return;
	std::vector<double> times(presses.size());
	std::ranges::transform(presses, times.begin(), [&](int frame) { return value.secondsAt(frame); });

	int index = 0;
	while (index < static_cast<int>(presses.size())) {
		int frame = presses[index];
		int end = index;
		while (end < static_cast<int>(presses.size()) && presses[end] == frame) ++end;
		if (end - index > Logic::kMaxPerFrame) Logic::get().noteRule(notes, frame, frame, "Rule 2");
		index = end;
	}

	int best = player2 ? 0 : 0;
	std::string bestText = fmt::format("{}: 0", who);
	int left = 0;
	for (int right = 0; right < static_cast<int>(presses.size()); ++right) {
		double end = times[static_cast<size_t>(right)];
		while (left <= right && end - times[static_cast<size_t>(left)] > 1.0) ++left;
		int count = right - left + 1;
		if (count > best) {
			best = count;
			bestText = fmt::format(
				"{}: {} ({} to {})",
				who,
				count,
				presses[left],
				presses[right]
			);
		}
		if (count > value.maxCps) Logic::get().noteRule(notes, presses[left], presses[right], "Rule 1");
	}

	// Any run of 5 or more clicks faster than 48/s breaks the rule. A violating run is noticed
	// when its last click arrives, so every window that ends at a click is enough.
	// Clicks start..end break it when 48 * time(start) - start > 48 * time(end) - end - 1, so the
	// earliest such start is found from a running maximum. Starts within rounding distance of the
	// line get the exact check.
	int n = static_cast<int>(presses.size());
	double rate = static_cast<double>(Logic::kMaxBurstRate);
	std::vector<double> lead(presses.size());
	std::vector<double> peak(presses.size());
	for (size_t s = 0; s < presses.size(); ++s) {
		lead[s] = rate * times[s] - static_cast<double>(s);
		peak[s] = s == 0 ? lead[s] : std::max(peak[s - 1], lead[s]);
	}
	for (int end = Logic::kMinBurstClicks - 1; end < n; ++end) {
		int lastStart = end - (Logic::kMinBurstClicks - 1);
		double need = rate * times[static_cast<size_t>(end)] - static_cast<double>(end) - 1.0;
		double slack = 1e-6 + 1e-12 * std::abs(need);
		auto first = std::upper_bound(peak.begin(), peak.begin() + lastStart + 1, need - slack);
		for (int start = static_cast<int>(std::distance(peak.begin(), first)); start <= lastStart; ++start) {
			double margin = lead[static_cast<size_t>(start)] - need;
			if (margin <= -slack) continue;
			int count = end - start + 1;
			if (margin > slack || Logic::get().burstTooFast(value, count, presses[start], presses[end])) {
				Logic::get().noteRule(notes, presses[start], presses[end], "Rule 3");
				break;
			}
		}
	}

	if (player2) {
		if (best > 0 && (value.highestCps.rfind("P2:", 0) == 0 || bestText > value.highestCps)) {
			// replaced below by comparing counts
		}
	}
	int currentBest = 0;
	auto colon = value.highestCps.find(": ");
	if (colon != std::string::npos) currentBest = std::atoi(value.highestCps.c_str() + colon + 2);
	if (best >= currentBest) value.highestCps = bestText;
}
namespace {
struct CpsResult {
	std::vector<CpsNote> p1;
	std::vector<CpsNote> p2;
	std::string highest;
};
CpsResult scanCps(std::vector<int> const& p1, std::vector<int> const& p2, float macroFps, std::vector<RateChange> rates, int maxCps) {
	Session scan;
	scan.macroFps = macroFps;
	scan.fps = macroFps;
	scan.maxCps = std::max(maxCps, 1);
	scan.rates = std::move(rates);
	scan.highestCps = "P1: 0";
	Logic::live().evalPlayerCps(p1, false, scan);
	Logic::live().evalPlayerCps(p2, true, scan);
	return {std::move(scan.cpsNotesP1), std::move(scan.cpsNotesP2), std::move(scan.highestCps)};
}
std::pair<std::vector<int>, std::vector<int>> splitPresses(std::vector<Frame> const& inputs) {
	std::pair<std::vector<int>, std::vector<int>> presses;
	for (auto const& input : inputs) {
		if (!input.holding) continue;
		(input.player2 ? presses.second : presses.first).push_back(input.number);
	}
	return presses;
}
void takeCps(Session& value, CpsResult result) {
	value.cpsNotesP1 = std::move(result.p1);
	value.cpsNotesP2 = std::move(result.p2);
	value.highestCps = std::move(result.highest);
}
}
// The game's own session checks on a worker so loading and editing a large macro never wait on it.
// Only the newest check is kept, since an older one describes inputs that have since changed.
void Logic::recomputeCps(Session& value) {
	auto [p1, p2] = splitPresses(value.inputs);
	if (&value != &Logic::live()) {
		takeCps(value, scanCps(p1, p2, value.macroFps, value.rates, value.maxCps));
		return;
	}
	static std::atomic<uint64_t> ticket{0};
	uint64_t mine = ticket.fetch_add(1) + 1;
	value.cpsNotesP1.clear();
	value.cpsNotesP2.clear();
	value.highestCps = "Checking CPS...";
	try {
		std::thread([p1 = std::move(p1), p2 = std::move(p2), fps = value.macroFps, rates = value.rates, maxCps = value.maxCps, mine] {
			std::optional<CpsResult> result;
			try {
				result = scanCps(p1, p2, fps, rates, maxCps);
			} catch (...) {
				result.reset();
			}
			if (!result) return;
			geode::queueInMainThread([result = std::move(*result), mine]() mutable {
				if (mine != ticket.load()) return;
				takeCps(Logic::live(), std::move(result));
				Logic::live().refreshPopup();
			});
		}).detach();
	} catch (...) {
		auto presses = splitPresses(value.inputs);
		takeCps(value, scanCps(presses.first, presses.second, value.macroFps, value.rates, value.maxCps));
	}
}
