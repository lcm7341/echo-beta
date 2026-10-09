#include "Logic/logic.hpp"
#include "GUI/gui.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/orbs.hpp"
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJRobotSprite.hpp>
#include <Geode/binding/GJSpiderSprite.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <vector>
#include "Logic/input.hpp"
#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>


using namespace geode::prelude;

int Logic::getFrame(PlayLayer* play) {
	if (!play) return 0;
	return Logic::get().stepFrame;
}
void Logic::toggleRecording() {
	auto& value = Logic::get();
	if (value.isPlaying() && !value.isRecording()) {
		bool record1 = value.recordP1 && !value.playP1;
		bool record2 = value.recordP2 && !value.playP2;
		if (!record1 && !record2) {
			value.message = "That player is already playing";
			Logic::get().refreshPopup();
			return;
		}
	}
	bool wasRecording = value.isRecording();
	// A new recording takes the Velocity Unrounding setting. Recording over a playing macro
	// keeps the macro's.
	if (value.state == RunState::Idle) {
		value.macroUnrounded = value.velocityUnrounding;
		value.macroSilicateVariance = false;
	}
	switch (value.state) {
		case RunState::Idle: value.state = RunState::Recording; break;
		case RunState::Recording: value.state = RunState::Idle; break;
		case RunState::Playing: value.state = RunState::RecordingAndPlaying; break;
		case RunState::RecordingAndPlaying: value.state = RunState::Playing; break;
	}
	value.message = Logic::get().stateName(value.state);
	// A small warm buffer avoids early reallocations without reserving 8 MiB for every recording.
	if (value.isRecording() && !wasRecording && value.inputs.capacity() < 4096) value.inputs.reserve(4096);
	if (wasRecording && !(value).isRecording()) Logic::get().autosaveReplay();
	Logic::get().refreshPopup();
}
void Logic::togglePlaying() {
	auto& value = Logic::get();
	switch (value.state) {
		case RunState::Idle: value.state = RunState::Playing; break;
		case RunState::Playing: value.state = RunState::Idle; break;
		case RunState::Recording: value.state = RunState::Playing; break;
		case RunState::RecordingAndPlaying: value.state = RunState::Recording; break;
	}
	if ((value).isPlaying()) Logic::get().setReplayPos(PlayLayer::get());
	else value.holdSync = false;
	value.message = Logic::get().stateName(value.state);
	Logic::get().refreshPopup();
}
bool Logic::steerPlayer(bool& isPlayer1) {
	bool record1 = recordsPlayer(false);
	bool record2 = recordsPlayer(true);
	if (record1 == record2) return false;
	isPlayer1 = !record2;
	return true;
}
PlayerObject* Logic::iconBody(GJBaseGameLayer* play, bool icon2) {
	if (!play) return nullptr;
	return icon2 ? play->m_player2 : play->m_player1;
}
bool Logic::pressIcon(GJBaseGameLayer* play, bool icon2, bool down, int button) {
	PlayerObject* body = iconBody(play, icon2);
	if (!body || body->m_isDead || body->m_controlsDisabled) return false;
	if (button == 0) button = static_cast<int>(PlayerButton::Jump);
	auto key = static_cast<PlayerButton>(button);
	if (down) body->pushButton(key);
	else body->releaseButton(key);
	return true;
}
bool Logic::physicalHold(GJBaseGameLayer* play, bool icon2) {
	if (!play || !play->m_uiLayer) return false;
	if (suppressAutoHold) return false;
	auto* ui = play->m_uiLayer;
	bool p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1 || keyJumpP1 || (restartHoldCaptured && deathJumpP1);
	bool p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1 || keyJumpP2 || (restartHoldCaptured && deathJumpP2);
	// A captured press can follow the UI once the UI shows it. A captured release has to survive
	// the restart: the checkpoint puts the old hold back, and reading the UI again would keep it.
	if (restartHoldCaptured && ((deathJumpP1 && (ui->m_p1Jumping || ui->m_p1TouchId != -1)) || (deathJumpP2 && (ui->m_p2Jumping || ui->m_p2TouchId != -1)))) {
		restartHoldCaptured = false;
	}
	bool record1 = recordsPlayer(false);
	bool record2 = recordsPlayer(true);
	bool onePlayerDual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
	if (record1 != record2 && !onePlayerDual) {
		if (icon2 != record2) return icon2 ? p2 : p1;
		return p1 || p2;
	}
	if (flipControls()) std::swap(p1, p2);
	// One click drives both icons, so Maintain Gravity has to see the same hold on each.
	if (onePlayerDual || clickBoth) return p1 || p2;
	return icon2 ? p2 : p1;
}
bool Logic::steerKey(GJBaseGameLayer* play, cocos2d::enumKeyCodes key, bool down, double timestamp) {
	if (!play || resetting || refreshOnly) return false;
	int code = static_cast<int>(key);
	auto held = steeredKeys.find(code);
	if (!down && held != steeredKeys.end()) {
		int button = held->second & 0xF;
		bool icon2 = (held->second >> 4) != 0;
		steeredKeys.erase(held);
		bool still = std::ranges::any_of(steeredKeys, [&](auto const& entry) { return entry.second == ((icon2 ? 1 : 0) << 4 | button); });
		if (button == static_cast<int>(PlayerButton::Jump)) (icon2 ? keyJumpP2 : keyJumpP1) = still;
		if (!still) play->queueButton(button, false, icon2, timestamp);
		if (button == static_cast<int>(PlayerButton::Jump) && !still) {
			restartHoldCaptured = false;
			deathJumpP1 = deathJumpP2 = false;
		}
		return true;
	}
	if (!down) return false;
	// After a full-attempt reset we cleared steeredKeys while the key was still down.
	// The next down looks like a fresh press — ignore jump until a real release clears the latch.
	if (suppressAutoHold) {
		switch (key) {
			case cocos2d::KEY_Space:
			case cocos2d::KEY_W:
			case cocos2d::KEY_Up:
				return true;
			default:
				break;
		}
	}
	bool target1 = true;
	if (!steerPlayer(target1)) return false;
	bool icon2 = !target1;
	int button = 0;
	switch (key) {
		case cocos2d::KEY_Space:
		case cocos2d::KEY_W:
		case cocos2d::KEY_Up:
			button = static_cast<int>(PlayerButton::Jump);
			break;
		case cocos2d::KEY_A:
		case cocos2d::KEY_Left:
			if (play->m_isPlatformer) button = static_cast<int>(PlayerButton::Left);
			break;
		case cocos2d::KEY_D:
		case cocos2d::KEY_Right:
			if (play->m_isPlatformer) button = static_cast<int>(PlayerButton::Right);
			break;
		default:
			break;
	}
	if (button == 0) return false;
	if (held != steeredKeys.end()) return true;
	int tag = (icon2 ? 1 : 0) << 4 | button;
	bool already = std::ranges::any_of(steeredKeys, [&](auto const& entry) { return entry.second == tag; });
	steeredKeys[code] = tag;
	if (button == static_cast<int>(PlayerButton::Jump)) {
		(icon2 ? keyJumpP2 : keyJumpP1) = true;
	}
	if (!already) play->queueButton(button, true, icon2, timestamp);
	return true;
}
void Logic::setRoutedHold(bool icon2, int button, bool down) {
	if (button == 0) button = static_cast<int>(PlayerButton::Jump);
	auto& value = Logic::get();
	auto set = [&](bool& target) {
		target = down;
	};
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	if (button == jump) {
		if (icon2) set(value.physicalJumpP2);
		else set(value.physicalJumpP1);
	} else if (button == left) {
		if (icon2) set(value.physicalLeftP2);
		else set(value.physicalLeftP1);
	} else if (button == right) {
		if (icon2) set(value.physicalRightP2);
		else set(value.physicalRightP1);
	}
}
void Logic::reinforceRoutedHold(PlayerObject* body) {
	auto* layer = GJBaseGameLayer::get();
	if (!layer || !body || maintainGravity || suppressAutoHold) return;
	bool icon2 = body == iconBody(layer, true);
	if (!icon2 && body != iconBody(layer, false)) return;
	if (!recordsPlayer(icon2)) return;
	body->m_controlsDisabled = false;
	auto hold = [&](bool want, PlayerButton key) {
		if (!want || Logic::buttonHeld(body, static_cast<int>(key))) return;
		body->pushButton(key);
	};
	if (icon2) {
		hold(physicalJumpP2, PlayerButton::Jump);
		hold(physicalLeftP2, PlayerButton::Left);
		hold(physicalRightP2, PlayerButton::Right);
	} else {
		hold(physicalJumpP1, PlayerButton::Jump);
		hold(physicalLeftP1, PlayerButton::Left);
		hold(physicalRightP1, PlayerButton::Right);
	}
}
void Logic::captureRestartHolds(PlayLayer* play) {
	restartHoldCaptured = false;
	if (!play) return;
	// The reset drops the queue, and a release made while dead is still waiting in it.
	std::ranges::for_each(play->m_queuedButtons, [](PlayerButtonCommand const& cmd) { Logic::get().rememberMove(cmd); });
	// Practice restart from a checkpoint should keep a manual hold. Full attempt reset should not.
	bool fromCheckpoint = Logic::get().savedCheckpoint(play);
	if (fromCheckpoint) {
		suppressAutoHold = false;
		return;
	}
	// Holding through death must not leave the next attempt held (especially 1-player dual ship).
	// Clear routed/key/UI holds and latch until the player fully releases, so a still-held key
	// or click cannot re-arm the respawn. Applies whether or not Echo is recording.
	Logic::get().clearPhysicalHolds();
	if (auto* ui = play->m_uiLayer) {
		ui->m_p1Jumping = false;
		ui->m_p2Jumping = false;
		ui->m_p1TouchId = -1;
		ui->m_p2TouchId = -1;
	}
	suppressAutoHold = true;
	// resetting is true here, so releaseAllButtons is not skipped for the recorded player.
	if (play->m_player1) play->m_player1->releaseAllButtons();
	if (play->m_player2) play->m_player2->releaseAllButtons();
}
void Logic::applyMaintainGravity(GJBaseGameLayer* play) {
	if (!play || !play->m_uiLayer || play->m_levelEndAnimationStarted || applyingPlayback || restoringHolds) return;
	if (!maintainGravity) return;
	int jumpKey = static_cast<int>(PlayerButton::Jump);
	bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode || two;
	auto eligible = [&](PlayerObject* body, bool icon2) {
		// Only the wave steers by gravity. Any other mode pressed here gets a click it never had.
		if (!body || !body->m_isDart || playsPlayer(icon2) || (icon2 && !dual) || body->m_controlsDisabled) return false;
		if (recordsPlayer(icon2)) return true;
		// A 1-player dual is recorded on player 1 even when only player 2 is marked.
		bool oneDual = play->m_gameState.m_isDualMode && !two;
		return oneDual && !icon2 && recordsPlayer(true);
	};
	auto mismatch = [&](PlayerObject* body, bool icon2, bool& push) {
		push = false;
		if (!eligible(body, icon2)) return false;
		bool physical = physicalHold(play, icon2);
		if (body->m_isUpsideDown != (physical == body->m_jumpBuffered)) return false;
		push = physical != body->m_isUpsideDown;
		return true;
	};
	auto apply = [&](PlayerObject* body, bool icon2, bool push, bool record) {
		if (!body) return;
		if (record) {
			Frame input;
			input.number = stepFrame + 1;
			input.holding = push;
			input.player2 = icon2;
			input.button = static_cast<uint8_t>(jumpKey);
			size_t at = Logic::get().placeRecorded(*this, input);
			if (at != static_cast<size_t>(-1) && saveDebug) setPoseAt(at, Logic::get().capturePose(body));
		}
		body->m_isLocked = false;
		if (!record) mirrorClick = true;
		Logic::get().pressIcon(play, icon2, push, jumpKey);
		mirrorClick = false;
	};
	auto correct = [&](PlayerObject* body, bool icon2) {
		maintainingGravity = true;
		// Same Gravity settles the flip itself. One hold correction is enough; a loop of clicks toggles gravity and the playback never lands on the same line.
		int limit = sameGravity ? 1 : 8;
		for (int step = 0; step < limit; ++step) {
			bool push = false;
			if (!mismatch(body, icon2, push)) break;
			bool beforeFlip = body->m_isUpsideDown;
			bool beforeBuffer = body->m_jumpBuffered;
			apply(body, icon2, push, true);
			if (body->m_isUpsideDown == beforeFlip && body->m_jumpBuffered == beforeBuffer) break;
		}
		maintainingGravity = false;
	};
	if (!isRecording()) return;
	if (clickBoth && dual) {
		bool held = physicalHold(play, false);
		Trajectory::get().noteButton(true, held);
		Trajectory::get().noteButton(false, held);
	}
	// A 1-player dual has one hold. It has to click both icons, and the macro stores that as player 1.
	// Playback already gives player 2 the same button, so a player 2 input would click them again.
	bool oneDual = play->m_gameState.m_isDualMode && !two;
	if (oneDual) {
		maintainingGravity = true;
		int limit = sameGravity ? 1 : 8;
		for (int step = 0; step < limit; ++step) {
			bool push = false;
			if (!mismatch(play->m_player1, false, push)) break;
			bool beforeFlip = play->m_player1->m_isUpsideDown;
			bool beforeBuffer = play->m_player1->m_jumpBuffered;
			apply(play->m_player1, false, push, true);
			if (play->m_player1->m_isUpsideDown == beforeFlip && play->m_player1->m_jumpBuffered == beforeBuffer) break;
			apply(play->m_player2, true, push, false);
		}
		maintainingGravity = false;
		return;
	}
	// Click Both decides both icons before either orb flip can cancel the other.
	bool paired = clickBoth && dual && !two;
	if (!paired) {
		correct(play->m_player1, false);
		correct(play->m_player2, true);
		return;
	}
	maintainingGravity = true;
	int limit = sameGravity ? 1 : 8;
	for (int step = 0; step < limit; ++step) {
		bool push1 = false;
		bool push2 = false;
		bool need1 = mismatch(play->m_player1, false, push1);
		bool need2 = mismatch(play->m_player2, true, push2);
		if (!need1 && !need2) break;
		bool flip1 = play->m_player1 && play->m_player1->m_isUpsideDown;
		bool buffer1 = play->m_player1 && play->m_player1->m_jumpBuffered;
		bool flip2 = play->m_player2 && play->m_player2->m_isUpsideDown;
		bool buffer2 = play->m_player2 && play->m_player2->m_jumpBuffered;
		if (need1) apply(play->m_player1, false, push1, true);
		if (need2) apply(play->m_player2, true, push2, true);
		bool changed1 = need1 && play->m_player1 && (play->m_player1->m_isUpsideDown != flip1 || play->m_player1->m_jumpBuffered != buffer1);
		bool changed2 = need2 && play->m_player2 && (play->m_player2->m_isUpsideDown != flip2 || play->m_player2->m_jumpBuffered != buffer2);
		if (!changed1 && !changed2) break;
	}
	maintainingGravity = false;
}
void Logic::syncSameGravity(GJBaseGameLayer* play) {
	if (!play || !sameGravity || !maintainGravity || applyingPlayback || restoringHolds) return;
	bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode && two;
	if (!dual || !play->m_player1 || !play->m_player2) return;
	auto* first = play->m_player1;
	auto* second = play->m_player2;
	bool now1 = first->m_isUpsideDown;
	bool now2 = second->m_isUpsideDown;
	// Playback plays the stored clicks. Forcing a match here changes the replay.
	if (!recordsPlayer(false) && !recordsPlayer(true)) {
		sameGravityWasP1 = now1;
		sameGravityWasP2 = now2;
		sameGravitySeen = true;
		return;
	}
	bool target = now1;
	if (sameGravitySeen && now2 != sameGravityWasP2 && now1 == sameGravityWasP1) target = now2;
	auto match = [&](PlayerObject* body, bool icon2) {
		if (!body || !recordsPlayer(icon2) || body->m_controlsDisabled || body->m_isUpsideDown == target) return;
		body->flipGravity(target, true);
	};
	match(first, false);
	match(second, true);
	sameGravityWasP1 = first->m_isUpsideDown;
	sameGravityWasP2 = second->m_isUpsideDown;
	sameGravitySeen = true;
}
void Logic::toggleFrameAdvance() {
	auto& value = Logic::get();
	value.frameAdvance = !value.frameAdvance;
	value.advanceQueued = false;
	value.holdingAdvance = false;
	if (value.frameAdvance) Logic::get().disarmLengthPauseIfHolding();
	value.message = value.frameAdvance ? "Frame advance on" : "Frame advance off";
	Logic::get().refreshPopup();
}
void Logic::disarmLengthPauseIfHolding() {
	auto& value = Logic::get();
	if (!value.pauseAtTrajectoryLength) return;
	auto* play = PlayLayer::get();
	if (!play || !play->m_player1) return;
	if (Logic::get().physicalHold(play, false) || Logic::get().physicalHold(play, true)) {
		value.pauseLengthArmed = false;
	}
}
void Logic::requestStep() {
	auto& value = Logic::get();
	bool wasOff = !value.frameAdvance;
	if (!value.frameAdvance) value.frameAdvance = true;
	if (wasOff) Logic::get().disarmLengthPauseIfHolding();
	value.advanceSteps = 1;
	value.advanceQueued = true;
}
void Logic::pollAdvanceHold() {
	auto& value = Logic::get();
	if (!value.frameAdvance || !value.holdingAdvance || !PlayLayer::get()) return;
	if (GUI::get().typing()) {
		value.holdingAdvance = false;
		return;
	}
	auto now = std::chrono::steady_clock::now();
	// Start repeat credit after the hold delay, and retain fractional ticks. A
	// manual press always queues one tick; held repeat is an explicit tick budget.
	auto start = value.advanceHoldStart + std::chrono::milliseconds(value.advanceHoldMs);
	if (now < start) return;
	auto from = std::max(start, value.lastAdvanceStep);
	double rate = value.sampleRate() * std::clamp(value.advanceSpeed, 1.f, 200.f) / 100.0 * value.speedhack;
	if (!std::isfinite(rate) || !(rate > 0.0)) return;
	double elapsed = std::chrono::duration<double>(now - from).count();
	int cap = std::min(value.unlockedStepCap(), value.affordableSteps(false));
	double wanted = std::floor(elapsed * rate);
	if (wanted < 1.0) return;
	int steps = static_cast<int>(std::min(wanted, static_cast<double>(cap)));
	value.lastAdvanceStep = wanted > cap ? now : from + std::chrono::duration_cast<decltype(now - from)>(std::chrono::duration<double>(steps / rate));
	value.advanceSteps = value.advanceQueued ? std::min(cap, value.advanceSteps + steps) : steps;
	value.advanceQueued = true;
}
void Logic::checkAdvancePause(PlayLayer* play) {
	if (!play || play->m_playerDied || Logic::get().renderCapturing()) return;
	auto& value = Logic::get();
	if (!value.pauseBeforeKill || !value.isRecording()) return;
	auto watch = [&](bool player2) {
		if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2)) return false;
		if (value.isRecording()) return value.recordsPlayer(player2);
		return true;
	};
	bool deathNext = false;
	if (watch(false) && value.deathNextP1) deathNext = true;
	if (watch(true) && value.deathNextP2) deathNext = true;
	if (!deathNext) {
		value.pauseKillArmed = true;
		return;
	}
	bool beforeKill = value.pauseKillArmed;
	if (!beforeKill) return;
	bool wasHolding = value.holdingAdvance;
	bool wasOff = !value.frameAdvance;
	if (!wasOff && !wasHolding) return;
	value.frameAdvance = true;
	value.holdingAdvance = false;
	value.advanceQueued = false;
	value.pauseKillArmed = false;
	if (wasOff || wasHolding) {
		value.message = "Paused before kill";
		Logic::get().refreshPopup();
	}
}
void Logic::onKeybind(std::string_view key, bool down) {
	if (down && GUI::get().typing()) return;
	if (key == "open-menu") {
		if (down) GUI::get().openPopup();
		return;
	}
	if (!down && key == "step-back") {
		Logic::get().holdingBack = false;
		return;
	}
	if (!down && key == "step-frame") {
		Logic::get().holdingAdvance = false;
		Logic::get().disarmLengthPauseIfHolding();
		return;
	}
	if (!down) return;
	if (key == "step-back") {
		auto now = std::chrono::steady_clock::now();
		Logic::get().holdingBack = true;
		Logic::get().backHoldStart = now;
		Logic::get().lastBackStep = now;
		Logic::get().stepBack(PlayLayer::get(), 1);
		return;
	}
	if (key == "step-frame") {
		auto now = std::chrono::steady_clock::now();
		Logic::get().holdingAdvance = true;
		Logic::get().advanceHoldStart = now;
		Logic::get().lastAdvanceStep = now;
		Logic::get().requestStep();
		return;
	}
	Logic::get().dispatchSettingKey(key, true);
}
void Logic::syncTimeScale() {
	auto* scheduler = CCDirector::sharedDirector()->getScheduler();
	if (!scheduler) return;
	auto const& value = Logic::get();
	float speed = std::max(value.speedhack, 0.01f);
	if (!std::isfinite(speed) || speed < 0.01f) speed = 1.f;
	if (Logic::get().renderActive()) {
		scheduler->setTimeScale(1.f);
		return;
	}
	// Death only runs through the scheduler. Speedhack has to live on timeScale there or
	// the retry timer and music snap back to 1x. Lock FPS already folds speed into the
	// animation interval, so timeScale still scales the death timer with the chosen rate.
	if (auto* play = PlayLayer::get(); play && play->m_playerDied) {
		scheduler->setTimeScale(speed);
		return;
	}
	// Inside a live attempt the updater applies speedhack to the step count.
	if (PlayLayer::get() || value.editorTesting) scheduler->setTimeScale(1.f);
	else scheduler->setTimeScale(speed);
}
// The renderer sets its own pace while it runs, so the visual rate is left alone until it stops.
void Logic::syncPlaybackFps() {
	auto& value = Logic::get();
	auto* director = CCDirector::sharedDirector();
	if (!director || Logic::get().renderActive()) return;
	if (value.playbackFps >= 1.f) {
		if (!value.playbackFpsHeld) value.heldInterval = director->getAnimationInterval();
		value.playbackFpsHeld = true;
		float rate = value.playbackFps;
		// Lock FPS keeps one physics quantum per visual frame. Speedhack then changes how
		// often those frames arrive, so the keybind restores the last speed instead of
		// packing more steps into a capped frame.
		if (value.lockFps) {
			float speed = std::max(value.speedhack, 0.01f);
			if (!std::isfinite(speed) || speed < 0.01f) speed = 1.f;
			rate *= speed;
		}
		director->setAnimationInterval(1.f / rate);
		return;
	}
	if (!value.playbackFpsHeld) return;
	value.playbackFpsHeld = false;
	director->setAnimationInterval(value.heldInterval > 0.f ? value.heldInterval : 1.f / 60.f);
}
void Logic::beginEditorPlaytest() {
	auto& value = Logic::get();
	value.editorTesting = true;
	value.editorPaused = false;
	value.trailP1.clear();
	value.trailP2.clear();
	value.stepFrame = 0;
	value.progressBaseSet = false;
	value.carry = 0.f;
	value.respawnFrames = 0;
	value.followLog.clear();
	value.buttonsPrepared = false;
	value.replayClicksLeft = 0;
	Logic::get().clearClickHitbox();
	if (value.isRecording() && !value.isPlaying()) Logic::get().trimFreshAttempt(nullptr, value);
	if (value.isPlaying()) Logic::get().setReplayPos(nullptr);
	Logic::get().syncTimeScale();
}
void Logic::endEditorPlaytest(GJBaseGameLayer* editor) {
	(void)editor;
	auto& value = Logic::get();
	value.editorTesting = false;
	value.editorPaused = false;
	value.carry = 0.f;
	value.stepsLeft = 0;
	Logic::get().syncTimeScale();
}
void Logic::resumeAttempt(PlayLayer* play) {
	if (!play) return;
	auto& value = Logic::get();
	value.resumePending = true;
	value.carry = 0.f;
	value.present = true;
	play->m_extraDelta = 0.0;
	play->m_resumeTimer = 0;
	bool menu = Logic::get().pauseMenuOpen();
	if (!menu) play->m_isPaused = false;
	if (auto* director = CCDirector::sharedDirector()) {
		if (!menu && director->isPaused()) director->resume();
		director->setNextDeltaTimeZero(false);
	}
	Logic::get().syncTimeScale();
}
void Logic::retainPlaybackHold(PlayLayer* play) {
	auto& value = Logic::get();
	if (!play || !value.isPlaying()) return;
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	auto hold = [&](bool icon2) {
		if (!value.playsPlayer(icon2)) return;
		auto* body = Logic::get().iconBody(play, icon2);
		if (!body) return;
		auto keep = [&](int button, bool want) {
			if (!want) return;
			body->m_holdingButtons[button] = true;
			if (button == jump) body->m_jumpBuffered = true;
			if (button == left) body->m_holdingLeft = true;
			if (button == right) body->m_holdingRight = true;
		};
		int frame = value.stepFrame;
		keep(jump, Logic::get().heldAt(value, icon2, frame, jump));
		if (!play->m_isPlatformer) return;
		keep(left, Logic::get().heldAt(value, icon2, frame, left));
		keep(right, Logic::get().heldAt(value, icon2, frame, right));
	};
	hold(false);
	hold(true);
}
void Logic::onFocusChange() {
	auto& value = Logic::get();
	value.carry = 0.f;
	value.present = true;
	if (auto* director = CCDirector::sharedDirector()) director->setNextDeltaTimeZero(true);
	if (auto* play = PlayLayer::get()) {
		play->m_extraDelta = 0.0;
		if (!play->m_playerDied) {
			if (play->m_player1) play->m_player1->m_isDead = false;
			if (play->m_player2) play->m_player2->m_isDead = false;
		}
	}
	Logic::get().syncTimeScale();
}






namespace {
struct HeldLook {
	bool held = false;
	bool wave = false;
	bool regular = false;
	bool ship = false;
	bool ghost = false;
	bool fireMid = false;
	bool fireBot = false;
	bool fireTop = false;
	bool dash = false;
};

HeldLook* heldLooks() {
	static HeldLook players[2]{};
	return players;
}

HeldLook& heldLook(PlayerObject* player) {
	auto* play = PlayLayer::get();
	bool second = play && player == play->m_player2;
	return heldLooks()[second ? 1 : 0];
}

void takeNode(CCNode* node, bool& saved, bool already) {
	if (!node) return;
	if (!already) saved = node->isVisible();
	node->setVisible(false);
}

void giveNode(CCNode* node, bool saved) {
	if (node) node->setVisible(saved);
}

void stopParticle(CCParticleSystemQuad* system) {
	if (!system) return;
	system->setVisible(false);
	system->stopSystem();
}

void showParticle(CCParticleSystemQuad* system) {
	if (system) system->setVisible(true);
}

void restorePlayer(PlayerObject* player, HeldLook const& held) {
	player->m_playEffects = true;
	if (!player->m_isDead) {
		player->setVisible(true);
		player->updatePlayerArt();
		player->updatePlayerGlow();
		if (player->m_hasGlow && player->m_isRobot && player->m_robotSprite) player->m_robotSprite->showGlow();
		if (player->m_hasGlow && player->m_isSpider && player->m_spiderSprite) player->m_spiderSprite->showGlow();
	}
	giveNode(player->m_waveTrail, held.wave);
	giveNode(player->m_regularTrail, held.regular);
	giveNode(player->m_shipStreak, held.ship);
	giveNode(player->m_ghostTrail, held.ghost);
	giveNode(player->m_swingFireMiddle, held.fireMid);
	giveNode(player->m_swingFireBottom, held.fireBot);
	giveNode(player->m_swingFireTop, held.fireTop);
	giveNode(player->m_dashSpritesContainer, held.dash);
	showParticle(player->m_playerGroundParticles);
	showParticle(player->m_trailingParticles);
	showParticle(player->m_shipClickParticles);
	showParticle(player->m_vehicleGroundParticles);
	showParticle(player->m_ufoClickParticles);
	showParticle(player->m_robotBurstParticles);
	showParticle(player->m_dashParticles);
	showParticle(player->m_swingBurstParticles1);
	showParticle(player->m_swingBurstParticles2);
	showParticle(player->m_landParticles0);
	showParticle(player->m_landParticles1);
}
}

bool Logic::playerHidden(PlayerObject* player) const {
	auto* play = PlayLayer::get();
	if (!play || !player) return false;
	if (player == play->m_player1) return hideP1;
	if (player == play->m_player2) return hideP2;
	return false;
}
bool Logic::hitboxPlayerHidden(bool player2) const {
	if (hitboxesFollowHidePlayer) return player2 ? hideP2 : hideP1;
	return player2 ? hideHitboxP2 : hideHitboxP1;
}
bool Logic::trajectoryPlayerHidden(bool player2) const {
	if (trajectoryFollowHidePlayer) return player2 ? hideP2 : hideP1;
	return player2 ? hideTrajectoryP2 : hideTrajectoryP1;
}
void Logic::concealPlayer(PlayerObject* player, bool hide) {
	if (!player) return;
	auto& held = heldLook(player);
	if (!hide) {
		if (!held.held) return;
		HeldLook saved = held;
		held = {};
		restorePlayer(player, saved);
		return;
	}
	bool already = held.held;
	held.held = true;
	player->setVisible(false);
	player->m_playEffects = false;
	takeNode(player->m_waveTrail, held.wave, already);
	takeNode(player->m_regularTrail, held.regular, already);
	takeNode(player->m_shipStreak, held.ship, already);
	takeNode(player->m_ghostTrail, held.ghost, already);
	takeNode(player->m_swingFireMiddle, held.fireMid, already);
	takeNode(player->m_swingFireBottom, held.fireBot, already);
	takeNode(player->m_swingFireTop, held.fireTop, already);
	takeNode(player->m_dashSpritesContainer, held.dash, already);
	if (player->m_robotSprite) player->m_robotSprite->hideGlow();
	if (player->m_spiderSprite) player->m_spiderSprite->hideGlow();
	stopParticle(player->m_playerGroundParticles);
	stopParticle(player->m_trailingParticles);
	stopParticle(player->m_shipClickParticles);
	stopParticle(player->m_vehicleGroundParticles);
	stopParticle(player->m_ufoClickParticles);
	stopParticle(player->m_robotBurstParticles);
	stopParticle(player->m_dashParticles);
	stopParticle(player->m_swingBurstParticles1);
	stopParticle(player->m_swingBurstParticles2);
	stopParticle(player->m_landParticles0);
	stopParticle(player->m_landParticles1);
}
void Logic::afterFrame(PlayLayer* play, float dt) {
	auto& value = Logic::get();
	if (!play || value.resetting) return;
	Logic::get().flushClickHitboxes(play);
	Logic::get().concealPlayer(play->m_player1, value.hideP1);
	Logic::get().concealPlayer(play->m_player2, value.hideP2);
	if ((value).isRecording()) value.recordingSeconds += static_cast<double>(dt);
	if (play->m_hasCompletedLevel && (value).isRecording()) {
		value.state = (value).isPlaying() ? RunState::Playing : RunState::Idle;
	}
	if (play->m_endPortal) value.endX = play->m_endPortal->getPositionX();
	if (value.armImmediateRestart && play->m_uiLayer && play->m_attemptLabel && !Logic::get().pauseMenuOpen()) {
		value.armImmediateRestart = false;
		Loader::get()->queueInMainThread([] {
			auto* layer = PlayLayer::get();
			if (!layer || Logic::get().pauseMenuOpen() || !layer->m_uiLayer || !layer->m_attemptLabel) return;
			layer->resetLevel();
		});
	}
	if (!Logic::get().renderCapturing()) Logic::get().updateCps(play);
	if (!value.present) return;
	Logic::get().applyLayout(play);
	// Inside the scheduler, hitboxes are drawn once after the frame (every step's redraw was the
	// lag of frame advance and backstep with hitboxes on).
	if (!value.inScheduler) Logic::get().drawHitboxes(play);
	Logic::get().drawOverlay(play);
}
void Logic::onPlayInit(PlayLayer* play) {
	heldLooks()[0] = {};
	heldLooks()[1] = {};
	auto& value = Logic::get();
	value.replayPos = 0;
	value.replayIndex = 0;
	value.sequenceOrder.clear();
	value.stepFrame = 0;
	value.progressBaseSet = false;
	value.removedFrames = 0;
	value.carry = 0.f;
	value.respawnFrames = 0;
	value.ignoreRecordFrames = 0;
	value.refreshOnly = false;
	value.refreshQueued = false;
	value.inScheduler = false;
	value.buttonsPrepared = false;
	value.replayClicksLeft = 0;
	Logic::get().cancelSteps();
	Logic::get().clearClickHitbox();
	Logic::get().applyRate(0);
	value.present = true;
	value.layoutStyled = false;
	Logic::get().clearStuckDeath(play);
	Logic::get().clearPhysicalHolds();
	value.checkpoints.clear();
	value.restoredCheckpoint = nullptr;
	value.clicksP1.clear();
	value.clicksP2.clear();
	++value.clicksRevision;
	value.attempts = 1;
	value.trailP1.clear();
	value.trailP2.clear();
	value.hiddenDecorations.clear();
	value.lastClickP1 = 0.0;
	value.lastClickP2 = 0.0;
	if (play->m_level) {
		value.macroName = play->m_level->m_levelName;
		if (value.macroName.empty()) value.macroName = "Replay";
	}
	if (value.lockDisplayFps) {
		CCDirector::sharedDirector()->setAnimationInterval(1.f / std::max(value.fps, 1.f));
		value.displayFpsLocked = true;
	}
	Logic::get().syncTimeScale();
	Logic::get().resetOpeningMusicHold();
	if (value.seedOverride) {
		value.levelSeed = value.seedOverrideValue;
		value.levelSeedKnown = true;
		value.levelSeedFromFile = false;
	} else if (!value.isPlaying() && !value.levelSeedFromFile) {
		value.levelSeed = 0;
		value.levelSeedKnown = false;
	}
	value.armImmediateRestart = value.immediateRestart;
}
namespace {
struct LiveRand {
	int uniqueId = 0;
	uint64_t* state = nullptr;
};
std::vector<LiveRand>& liveRand() {
	static std::vector<LiveRand> triggers;
	return triggers;
}
struct VarianceLock {
	bool held = false;
	std::array<float, 2000> values{};
};
VarianceLock& varianceLock() {
	static VarianceLock lock;
	return lock;
}
void fillVariance(PlayLayer* play, uint64_t seed) {
	uint64_t state = seed;
	for (float& slot : play->m_varianceValues) {
		slot = static_cast<float>(state & 0xFFFF) / 32768.f - 1.f;
		state = 214013ull * state + 2531011ull;
	}
}
}
uint64_t Logic::gameRandomState() {
	return GameToolbox::getfast_srand();
}
void Logic::setGameRandomState(uint64_t state) {
	GameToolbox::fast_srand(state);
}
void Logic::noteRandTrigger(int uniqueId, uint64_t* state) {
	if (!state) return;
	for (auto const& trigger : liveRand()) {
		if (trigger.state == state) return;
	}
	liveRand().push_back({uniqueId, state});
}
void Logic::clearRandTriggers() {
	liveRand().clear();
}
void Logic::reseedRandTriggers(uint64_t seed) {
	for (auto& trigger : liveRand()) {
		if (!trigger.state) continue;
		*trigger.state = seed ^ (static_cast<uint64_t>(trigger.uniqueId) * 2137ull);
	}
}
void Logic::rollLiveSeed(PlayLayer* play) {
	static std::mt19937_64 rng{std::random_device{}()};
	auto& value = Logic::get();
	uint64_t rolled = rng();
	if (rolled == 0) rolled = 1;
	varianceLock().held = false;
	if (!value.levelSeedFromFile || value.isRecording()) {
		value.levelSeed = rolled;
		value.levelSeedKnown = true;
		if (value.isRecording()) value.levelSeedFromFile = false;
	}
	if (!play) return;
	play->m_randomSeed = rolled;
	play->m_replayRandSeed = rolled;
	play->m_unk32e0 = rolled;
	Logic::get().setGameRandomState(rolled);
	value.shakeRandom = rolled & 0x7FFF;
	value.teleportRandom = rolled & 0x7FFF;
	fillVariance(play, rolled);
	Logic::get().reseedRandTriggers(rolled);
}
void Logic::applyAttemptVariance(PlayLayer* play, uint64_t seed) {
	if (!play) return;
	fillVariance(play, seed);
	auto& lock = varianceLock();
	lock.held = true;
	std::copy(play->m_varianceValues.begin(), play->m_varianceValues.end(), lock.values.begin());
}
void Logic::keepVariance(PlayLayer* play) {
	auto& lock = varianceLock();
	auto const& value = Logic::get();
	if (!play || !lock.held) return;
	if (!value.seedOverride && !value.isPlaying()) return;
	replay::restoreVariance(play->m_varianceValues, lock.values);
}
void Logic::plantLevelSeed(PlayLayer* play) {
	auto& value = Logic::get();
	if (!play || value.keepLevelSeed) return;
	if (value.seedOverride) {
		value.levelSeed = value.seedOverrideValue;
		value.levelSeedKnown = true;
	}
	if (!value.levelSeedKnown) return;
	play->m_randomSeed = value.levelSeed;
	play->m_replayRandSeed = value.levelSeed;
	play->m_unk32e0 = value.levelSeed;
	Logic::get().setGameRandomState(value.levelSeed);
	value.shakeRandom = value.levelSeed & 0x7FFF;
	value.teleportRandom = value.levelSeed & 0x7FFF;
	Logic::get().applyAttemptVariance(play, value.levelSeed);
	Logic::get().reseedRandTriggers(value.levelSeed);
}
void Logic::restoreCheckpointRandom(PlayLayer* play) {
	auto& value = Logic::get();
	auto* saved = Logic::get().savedCheckpoint(play);
	if (!saved) return;
	auto const& checkpoint = *saved;
	Logic::get().setGameRandomState(checkpoint.randomState);
	value.shakeRandom = checkpoint.shakeRandom;
	value.teleportRandom = checkpoint.teleportRandom;
	if (checkpoint.variance.size() == play->m_varianceValues.size()) {
		std::copy(checkpoint.variance.begin(), checkpoint.variance.end(), play->m_varianceValues.begin());
		auto& lock = varianceLock();
		lock.held = true;
		std::copy(play->m_varianceValues.begin(), play->m_varianceValues.end(), lock.values.begin());
	}
	for (auto const& saved : checkpoint.randStates) {
		for (auto& trigger : liveRand()) {
			if (trigger.uniqueId == saved.first && trigger.state) *trigger.state = saved.second;
		}
	}
}
void reshuffleSequence(Session& value) {
	value.sequenceOrder.resize(value.sequence.size());
	std::iota(value.sequenceOrder.begin(), value.sequenceOrder.end(), size_t{0});
	std::mt19937 rng{std::random_device{}()};
	std::shuffle(value.sequenceOrder.begin(), value.sequenceOrder.end(), rng);
	value.replayIndex = 0;
}
void Logic::onReset(PlayLayer* play) {
	auto& value = Logic::get();
	Logic::get().clearClickHitbox();
	value.trailP1.clear();
	value.trailP2.clear();
	Logic::get().resetOpeningMusicHold();
	if (!Logic::get().layoutActive()) {
		for (auto* object : value.hiddenDecorations) {
			if (object) object->m_isHide = false;
		}
		value.hiddenDecorations.clear();
		value.layoutStyled = false;
	}
	value.currentlyPressing = false;
	value.deathNextP1 = false;
	value.deathNextP2 = false;
	value.noclipDeadP1 = false;
	value.noclipDeadP2 = false;
	Orbs::clear();
	value.showedNewBest = false;
	value.overMaxCps = false;
	value.cpsBreaking = false;
	if (play->m_endPortal) value.endX = play->m_endPortal->getPositionX();
	if ((value).isRecording() && !value.backstepping) ++value.attempts;

	if (value.sequenceEnabled && !(value).isRecording() && !value.backstepping) {
		if (value.sequenceRandom && !value.sequence.empty()) {
			if (value.sequenceOrder.size() != value.sequence.size() || value.replayIndex >= value.sequenceOrder.size()) reshuffleSequence(value);
			size_t pick = value.sequenceOrder[value.replayIndex];
			value.inputs = value.sequence[pick].actions;
			value.clearSides();
			value.macroName = value.sequence[pick].name;
			++value.replayIndex;
			Logic::get().sortInputs();
		} else if (value.replayIndex < value.sequence.size()) {
			value.inputs = value.sequence[value.replayIndex].actions;
			value.clearSides();
			value.macroName = value.sequence[value.replayIndex].name;
			++value.replayIndex;
			Logic::get().sortInputs();
		} else {
			value.replayIndex = 0;
		}
	}

	bool const practice = play->m_isPracticeMode || value.backstepping;
	auto* saved = Logic::get().savedCheckpoint(play);
	bool const fromCheckpoint = practice && saved;
	if (fromCheckpoint) {
		value.stepFrame = saved->frame;
	} else {
		value.stepFrame = 0;
		value.progressBaseSet = false;
		value.removedFrames = 0;
		value.sameGravitySeen = false;
	}
	Logic::get().applyRate(value.stepFrame);
	// Clicks from the frame being returned to onward never happened on this attempt.
	auto replayed = [&value](int frame) { return frame >= value.stepFrame; };
	std::erase_if(value.clicksP1, replayed);
	std::erase_if(value.clicksP2, replayed);
	Logic::get().rewindFollow(play);
	++value.clicksRevision;
	play->m_extraDelta = 0.0;
	play->m_resumeTimer = 0;
	if (auto* director = CCDirector::sharedDirector()) director->setNextDeltaTimeZero(false);
	Logic::get().clearStuckDeath(play);
	value.present = true;

	if (value.sortAutomatically && (value).isRecording() && (value).isPlaying()) Logic::get().sortInputs();
	if (fromCheckpoint) {
		if (!value.backstepping) value.objectRestore = saved->object;
		Logic::get().trimFromCheckpoint(play, value, *saved);
		play->m_queuedButtons.clear();
	} else if ((value).isRecording()) {
		Logic::get().trimFreshAttempt(play, value);
	}
	if (fromCheckpoint) {
		play->m_extraDelta = 0.0;
		value.refreshQueued = true;
		if (!value.inScheduler) Logic::get().flushVisualRefresh();
	}
	if (!practice) value.checkpoints.clear();
	if ((value).isPlaying()) Logic::get().setReplayPos(play);
	Trajectory::get().invalidate();
	Trajectory::get().refresh(play);
	Logic::get().restoreHolds(play);
	if (fromCheckpoint) {
		value.resumePending = false;
		play->m_extraDelta = 0.0;
		play->m_resumeTimer = 0;
		if (!Logic::get().pauseMenuOpen()) play->m_isPaused = false;
		Logic::get().clearStuckDeath(play);
		Logic::get().syncTimeScale();
		// The respawn is not a gravity flip. Same Gravity would otherwise match the pre-death gravity and realign both players.
		value.sameGravityWasP1 = play->m_player1 && play->m_player1->m_isUpsideDown;
		value.sameGravityWasP2 = play->m_player2 && play->m_player2->m_isUpsideDown;
		value.sameGravitySeen = true;
	} else {
		Logic::get().resumeAttempt(play);
	}
}
void Logic::restoreHolds(PlayLayer* play) {
	auto& value = Logic::get();
	if (!play) return;
	if (!value.isPlaying() && !value.isRecording()) {
		value.restartHoldCaptured = false;
		value.deathJumpP1 = value.deathJumpP2 = false;
		value.deathLeftP1 = value.deathRightP1 = false;
		value.deathLeftP2 = value.deathRightP2 = false;
		value.keyJumpP1 = value.keyJumpP2 = false;
		value.physicalJumpP1 = value.physicalJumpP2 = false;
		value.steeredKeys.clear();
		// Native/UI holds can outlive a release during death. Check the physical
		// sources before restoring an idle checkpoint's hold.
		bool p1 = false;
		bool p2 = false;
		if (auto* ui = play->m_uiLayer) {
			p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1;
			p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1;
		}
#ifdef GEODE_IS_WINDOWS
		if (!Input::jumpKeyDown() && !Input::mouseDown()) p1 = p2 = false;
#endif
		play->m_queuedButtons.clear();
		if (play->m_player1) play->m_player1->releaseAllButtons();
		if (play->m_player2) play->m_player2->releaseAllButtons();
		bool checkpoint = Logic::get().savedCheckpoint(play);
		bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
		bool shared = play->m_gameState.m_isDualMode && !two;
		bool allow = checkpoint && !value.suppressAutoHold;
		// Restore a hold silently. queueButton/pushButton would fire a new jump
		// or black orb at the restored position and overwrite checkpoint velocity.
		auto restore = [allow](PlayerObject* body, bool held) {
			if (!body) return;
			held = allow && held;
			body->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = held;
			body->m_jumpBuffered = held;
			if (!held) {
				body->m_wasJumpBuffered = false;
				body->m_stateJumpBuffered = 0;
			}
		};
		restore(play->m_player1, p1);
		restore(play->m_player2, shared ? p1 : two && p2);
		if (auto* ui = play->m_uiLayer) {
			ui->m_p1Jumping = allow && p1;
			ui->m_p2Jumping = allow && (shared ? p1 : two && p2);
			if (!ui->m_p1Jumping) ui->m_p1TouchId = -1;
			if (!ui->m_p2Jumping) ui->m_p2TouchId = -1;
		}
		return;
	}
	for (auto const& cmd : play->m_queuedButtons) Logic::get().rememberMove(cmd);
	play->m_queuedButtons.clear();

	bool playing = (value).isPlaying();
	bool recording = (value).isRecording();
	bool checkpoint = Logic::get().savedCheckpoint(play);
	if (checkpoint && !playing && !recording) return;
	if (playing && !recording) {
		if (value.checkpoints.empty()) {
			if (play->m_player1) play->m_player1->releaseAllButtons();
			if (play->m_player2) play->m_player2->releaseAllButtons();
		}
	}
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	auto putHold = [&](bool icon2, int button, bool want) {
		auto* body = Logic::get().iconBody(play, icon2);
		if (!body) return;
		bool have = Logic::buttonHeld(body, button);
		bool wave = button == jump && body->m_isDart && value.maintainGravity && value.recordsPlayer(icon2);
		if (!want && button == jump) body->m_jumpBuffered = false;
		if (!want && button == left) body->m_holdingLeft = false;
		if (!want && button == right) body->m_holdingRight = false;
		if (have == want && !(wave && body->m_jumpBuffered != want)) return;
		// The macro already has this hold. Pushing it again jumps and flips gravity, and playback never repeats that click.
		body->m_holdingButtons[button] = want;
		if (button == jump) body->m_jumpBuffered = want;
		if (button == left) body->m_holdingLeft = want;
		if (button == right) body->m_holdingRight = want;
	};
	std::vector<PlayerButtonCommand> deferred;
	bool oneDual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
	if (recording) {
		if (value.suppressAutoHold) {
			if (play->m_player1) play->m_player1->releaseAllButtons();
			if (play->m_player2) play->m_player2->releaseAllButtons();
			if (auto* ui = play->m_uiLayer) {
				ui->m_p1Jumping = false;
				ui->m_p2Jumping = false;
				ui->m_p1TouchId = -1;
				ui->m_p2TouchId = -1;
			}
		}
		auto jumpKeyDown = &Input::jumpKeyDown;
		auto leftKeyDown = &Input::leftKeyDown;
		auto rightKeyDown = &Input::rightKeyDown;
		auto mouseDown = &Input::mouseDown;
		auto liveJump = [&](bool icon2) {
			bool uiDown = false;
			if (auto* ui = play->m_uiLayer) {
				if (icon2) uiDown = ui->m_p2Jumping || ui->m_p2TouchId != -1;
				else uiDown = ui->m_p1Jumping || ui->m_p1TouchId != -1;
			}
			bool down = uiDown || (icon2 ? value.keyJumpP2 : value.keyJumpP1) || (icon2 ? value.physicalJumpP2 : value.physicalJumpP1) || jumpKeyDown();
			if (value.suppressAutoHold) {
				// Ignore still-held hardware/UI until a real release; do not re-arm from GetAsyncKeyState.
				if (!jumpKeyDown() && !uiDown) {
					value.keyJumpP1 = value.keyJumpP2 = false;
					value.physicalJumpP1 = value.physicalJumpP2 = false;
					value.suppressAutoHold = false;
				}
				return false;
			}
			if (uiDown) return true;
			// A routed hold re-presses every step, so one whose release was lost would hold the whole attempt.
			if (!jumpKeyDown() && !mouseDown()) {
				(icon2 ? value.keyJumpP2 : value.keyJumpP1) = false;
				(icon2 ? value.physicalJumpP2 : value.physicalJumpP1) = false;
				return false;
			}
			if (icon2 ? value.keyJumpP2 : value.keyJumpP1) return true;
			if (icon2 ? value.physicalJumpP2 : value.physicalJumpP1) return true;
			bool sticky = value.restartHoldCaptured && (icon2 ? value.deathJumpP2 : value.deathJumpP1);
			return sticky && jumpKeyDown();
		};
		auto liveSide = [&](bool icon2, bool sticky, bool& key, bool& physical, bool down) {
			if (value.suppressAutoHold) return false;
#ifdef GEODE_IS_MOBILE
			down = down || physical;
#endif
			if (!down) {
				key = false;
				physical = false;
				return false;
			}
			if (key || physical) return true;
			return value.restartHoldCaptured && sticky;
		};
		auto live = [&](bool icon2) {
			// Full attempt reset with suppress: drop holds on every icon, including the one the
			// macro owns and the dual partner that recordsPlayer skips.
			if (value.suppressAutoHold) {
				value.restoringHolds = true;
				putHold(icon2, jump, false);
				value.restoringHolds = false;
				if (auto* body = Logic::get().iconBody(play, icon2)) body->releaseAllButtons();
				return;
			}
			if (!value.recordsPlayer(icon2)) {
				// A 1-player dual has one hold, stored on player 1. The icon left unrecorded follows it,
				// or it keeps the hold it died with and nothing live ever releases it.
				if (!oneDual || value.playsPlayer(icon2) || !value.recordsPlayer(!icon2)) return;
				auto follow = [&](int button) { putHold(icon2, button, Logic::get().heldAt(value, false, value.stepFrame, button)); };
				value.restoringHolds = true;
				follow(jump);
				if (play->m_isPlatformer) {
					follow(left);
					follow(right);
				}
				value.restoringHolds = false;
				return;
			}
			// Record-while-playing: the macro owns this icon. A sticky death hold was inventing a
			// press on respawn and leaving the player held.
			if (value.playsPlayer(icon2)) return;
			bool jumpHeld = liveJump(icon2);
			auto* body = Logic::get().iconBody(play, icon2);
			if (value.maintainGravity && body && body->m_isDart) jumpHeld = jumpHeld != body->m_isUpsideDown;
			bool leftHeld = liveSide(
				icon2,
				icon2 ? value.deathLeftP2 : value.deathLeftP1,
				icon2 ? value.keyLeftP2 : value.keyLeftP1,
				icon2 ? value.physicalLeftP2 : value.physicalLeftP1,
				leftKeyDown()
			);
			bool rightHeld = liveSide(
				icon2,
				icon2 ? value.deathRightP2 : value.deathRightP1,
				icon2 ? value.keyRightP2 : value.keyRightP1,
				icon2 ? value.physicalRightP2 : value.physicalRightP1,
				rightKeyDown()
			);
			// The player starts from the hold the macro has at this frame. A change goes through the
			// game's queue in the next step like a live click, so it is recorded and counted by the level
			// at the same point playback sends it.
			auto note = [&](int button, bool held) {
				bool stored = Logic::get().heldAt(value, icon2 && !oneDual, value.stepFrame, button);
				value.restoringHolds = true;
				putHold(icon2, button, stored);
				value.restoringHolds = false;
				if (held == stored) return;
				PlayerButtonCommand cmd;
				cmd.m_button = static_cast<PlayerButton>(button);
				cmd.m_isPush = held;
				cmd.m_isPlayer2 = icon2;
				cmd.m_step = value.stepFrame + 1;
				cmd.m_timestamp = 0.0;
				deferred.push_back(cmd);
			};
			note(jump, jumpHeld);
			if (!jumpHeld && play->m_uiLayer) {
				if (icon2) {
					play->m_uiLayer->m_p2Jumping = false;
					play->m_uiLayer->m_p2TouchId = -1;
				} else {
					play->m_uiLayer->m_p1Jumping = false;
					play->m_uiLayer->m_p1TouchId = -1;
				}
			}
			if (!play->m_isPlatformer) return;
			note(left, leftHeld);
			note(right, rightHeld);
		};
		live(false);
		live(true);
	}
	if (checkpoint && playing) {
		value.restoringHolds = true;
		int frame = value.stepFrame;
		auto ensure = [&](bool stored2, int button) {
			if (!value.playsPlayer(stored2)) return;
			bool want = Logic::get().heldAt(value, stored2, frame, button);
			if (value.recordsPlayer(stored2)) value.applyingPlayback = false;
			else value.applyingPlayback = true;
			putHold(stored2, button, want);
			value.applyingPlayback = false;
		};
		ensure(false, jump);
		if (play->m_isPlatformer) {
			ensure(false, left);
			ensure(false, right);
		}
		if (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode) {
			ensure(true, jump);
			if (play->m_isPlatformer) {
				ensure(true, left);
				ensure(true, right);
			}
		}
	}
	// Not while a step back loads a save: pumping sends the macro's input for the next frame now, and
	// stepBack then puts the saved player back over it and clears the queue, so that press was lost.
	// The first step after the load (Backstep check: an orb press missing there flipped gravity in one
	// run and not the next). That step sends it, as the first run did.
	if ((playing || recording) && !value.backstepping) Logic::get().pumpButtons(play, play, 0.f, true);
	std::ranges::for_each(deferred, [play](PlayerButtonCommand const& cmd) { play->m_queuedButtons.push_back(cmd); });
	if (recording && checkpoint) Logic::get().sortInputs();
	value.restoringHolds = false;
	value.restartHoldCaptured = false;
	value.deathJumpP1 = value.deathJumpP2 = false;
	value.deathLeftP1 = value.deathRightP1 = false;
	value.deathLeftP2 = value.deathRightP2 = false;
}
void Logic::onCheckpointCreated(PlayLayer* play, CheckpointObject* object) {
	auto& value = Logic::get();
	if (!play || !object || value.predicting) return;
	if (std::ranges::any_of(value.checkpoints, [object](Checkpoint const& saved) { return saved.object == object; })) return;
	// Placed inside a physics step, the checkpoint holds that step's movement but the step is
	// only counted once processCommands returns.
	int frame = Logic::get().getFrame(play) + (value.inStep ? 1 : 0);
	value.checkpoints.push_back(Logic::get().captureCheckpoint(play, object, frame));
	value.objectCapture = object;
}
Checkpoint Logic::captureCheckpoint(PlayLayer* play, CheckpointObject* object, int frame) {
	auto& value = Logic::get();
	Checkpoint checkpoint;
	checkpoint.object = object;
	checkpoint.frame = frame;
	checkpoint.layer = Logic::get().captureLayer(play);
	checkpoint.player1 = Logic::get().capturePlayer(play->m_player1);
	checkpoint.player2 = Logic::get().capturePlayer(play->m_player2);
	checkpoint.replayPos = value.replayPos;
	checkpoint.removedFrames = value.removedFrames;
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	checkpoint.heldP1 = Logic::get().heldAt(value, false, checkpoint.frame, jump);
	checkpoint.heldP2 = Logic::get().heldAt(value, true, checkpoint.frame, jump);
	checkpoint.heldLeftP1 = Logic::get().heldAt(value, false, checkpoint.frame, left);
	checkpoint.heldLeftP2 = Logic::get().heldAt(value, true, checkpoint.frame, left);
	checkpoint.heldRightP1 = Logic::get().heldAt(value, false, checkpoint.frame, right);
	checkpoint.heldRightP2 = Logic::get().heldAt(value, true, checkpoint.frame, right);
	checkpoint.randomState = Logic::get().gameRandomState();
	checkpoint.shakeRandom = value.shakeRandom;
	checkpoint.teleportRandom = value.teleportRandom;
	checkpoint.variance.assign(play->m_varianceValues.begin(), play->m_varianceValues.end());
	for (auto const& trigger : liveRand()) {
		if (trigger.state) checkpoint.randStates.emplace_back(trigger.uniqueId, *trigger.state);
	}
	return checkpoint;
}
void Logic::syncCheckpoints(PlayLayer* play) {
	auto& value = Logic::get();
	// Eviction removes the FIRST native checkpoint; undo removes the LAST.
	// Reconcile by identity instead of assuming either operation always pops back.
	std::erase_if(value.checkpoints, [play](Checkpoint const& saved) {
		return !play || !play->m_checkpointArray || !play->m_checkpointArray->containsObject(saved.object);
	});
	if (value.restoredCheckpoint && (!play || !play->m_checkpointArray ||
		!play->m_checkpointArray->containsObject(value.restoredCheckpoint))) value.restoredCheckpoint = nullptr;
}
Checkpoint const* Logic::savedCheckpoint(PlayLayer* play) {
	auto& value = Logic::get();
	// A backstep loads its save through the practice path, practice mode or not.
	if (!play || !(play->m_isPracticeMode || value.backstepping)) return nullptr;
	auto* object = value.restoredCheckpoint ? value.restoredCheckpoint : play->getLastCheckpoint();
	if (!object || !play->m_checkpointArray || !play->m_checkpointArray->containsObject(object)) return nullptr;
	auto found = std::ranges::find_if(value.checkpoints, [object](Checkpoint const& saved) { return saved.object == object; });
	return found == value.checkpoints.end() ? nullptr : &*found;
}
void Logic::onComplete(PlayLayer* play) {
	auto& value = Logic::get();
	if ((value).isRecording()) value.state = (value).isPlaying() ? RunState::Playing : RunState::Idle;
	if (play->m_player1) value.endX = play->m_player1->getPositionX();
	value.message = "Level complete";
}
void Logic::onQuit(PlayLayer*) {
	auto& value = Logic::get();
	Logic::get().finishBackstep(nullptr);
	Logic::get().clearBacksteps();
	value.holdingBack = false;
	value.frameAdvance = false;
	value.advanceQueued = false;
	value.holdingAdvance = false;
	for (auto* object : value.hiddenDecorations) {
		if (object) object->m_isHide = false;
	}
	value.hiddenDecorations.clear();
	Logic::get().clearPhysicalHolds();
	Input::clearKeys();
	if (value.displayFpsLocked) {
		CCDirector::sharedDirector()->setAnimationInterval(1.f / 60.f);
		value.displayFpsLocked = false;
	}
	if (auto* scheduler = CCDirector::sharedDirector()->getScheduler()) scheduler->setTimeScale(1.f);
	Logic::get().stopRender();
	Logic::get().saveSettingsFile();
}
void Logic::clearStuckDeath(PlayLayer* play) {
	if (!play) return;
	play->m_playerDied = false;
	Logic::get().noclipDeadP1 = false;
	Logic::get().noclipDeadP2 = false;
	if (play->m_player1) play->m_player1->m_isDead = false;
	if (play->m_player2) play->m_player2->m_isDead = false;
	Logic::get().present = true;
}
bool Logic::shouldNoclip(PlayLayer* play, PlayerObject* player) {
	auto const& value = Logic::get();
	if (player == play->m_player1) return value.noclipP1;
	if (player == play->m_player2) return value.noclipP2;
	return false;
}






bool Logic::cbfEnabled() const {
	char const* ids[] = {"syzzi.click_between_frames", "zmx.cbf-lite"};
	for (char const* id : ids) {
		auto* mod = Loader::get()->getLoadedMod(id);
		if (!mod) continue;
		if (mod->hasSetting("soft-toggle") && mod->getSettingValue<bool>("soft-toggle")) continue;
		return true;
	}
	return false;
}

bool Logic::vanillaClickPhysics() const {
	if (auto* play = PlayLayer::get()) {
		if (play->m_clickBetweenSteps || play->m_clickOnSteps) return true;
	} else if (auto* gm = GameManager::get()) {
		if (gm->getGameVariable("0177") || gm->getGameVariable("0176")) return true;
	}
	return Logic::get().cbfEnabled();
}

bool Logic::deferPhysics() const {
	auto const& value = Logic::get();
	if (value.refreshOnly) return false;
	// runFrame lets the scheduler finish level entry without a step budget.
	// Recording/playback may already be armed here: native update and delta
	// must still run until GD starts the attempt, or entry stalls at zero ticks.
	if (auto* play = PlayLayer::get(); play && !play->m_started && !Logic::get().renderCapturing()) return true;
	if (value.physicsLocked()) return false;
	if (Logic::get().renderCapturing()) return false;
	return Logic::get().vanillaClickPhysics();
}

double Logic::clickStamp() const {
	return Logic::get().cbfEnabled() ? std::numeric_limits<double>::min() : 0.0;
}

void Logic::prepareButtons(GJBaseGameLayer* play) {
	auto& value = Logic::get();
	bool paused = Logic::get().editorPaused;
	if (auto* live = Logic::asPlay(play)) paused = paused || live->m_isPaused;
	if (!play || value.buttonsPrepared || value.resetting || value.refreshOnly || paused || !(value).physicsLocked()) return;
	value.buttonsPrepared = true;

	auto& user = play->m_queuedButtons;
	for (auto const& cmd : user) Logic::get().rememberMove(cmd);

	bool playing = (value).isPlaying();
	bool recording = (value).isRecording();
	if (recording && !value.restoringHolds) {
		auto steer = [&](PlayerButtonCommand& cmd) {
			bool isPlayer1 = !cmd.m_isPlayer2;
			if (Logic::get().steerPlayer(isPlayer1)) cmd.m_isPlayer2 = !isPlayer1;
		};
		for (auto& cmd : user) steer(cmd);
	}
	if (!value.restoringHolds && playing && value.ignoreInputs) {
		user.erase(std::remove_if(user.begin(), user.end(), [&](PlayerButtonCommand const& cmd) {
			return value.playsPlayer(cmd.m_isPlayer2);
		}), user.end());
		if (!recording) user.clear();
	} else if (!value.restoringHolds && recording && value.ignoreRecordFrames > 0) {
		user.clear();
	}

	value.replayClicksLeft = 0;
	if (!playing) return;
	// Same frame recordInput stores: the game has already counted this step, stepFrame has not.
	int frame = Logic::get().stepFrame + 1;
	// The game applies the queue inside this step, after it advances its own frame count. Live
	// clicks are recorded there, so replayed clicks have to go through the same queue.
	std::array<char, 6> queued{};
	auto queue = [&](bool holding, int button, bool player2) {
		int jump = static_cast<int>(PlayerButton::Jump);
		int left = static_cast<int>(PlayerButton::Left);
		int right = static_cast<int>(PlayerButton::Right);
		int slot = button == 0 || button == jump ? 0 : button == left ? 1 : button == right ? 2 : -1;
		if (slot >= 0) queued[static_cast<size_t>((player2 ? 3 : 0) + slot)] = 1;
		PlayerButtonCommand cmd;
		cmd.m_button = static_cast<PlayerButton>(button == 0 ? jump : button);
		cmd.m_isPush = holding;
		cmd.m_isPlayer2 = player2;
		cmd.m_step = frame;
		cmd.m_timestamp = 0.0;
		play->m_queuedButtons.push_back(cmd);
		++value.replayClicksLeft;
	};
	while (value.replayPos < value.inputs.size() && value.inputs[value.replayPos].number == frame) {
		Frame input = value.inputs[value.replayPos];
		++value.replayPos;
		int button = input.button == 0 ? static_cast<int>(PlayerButton::Jump) : input.button;
		if (!value.playsPlayer(input.player2)) continue;
		queue(input.holding, button, input.player2);
	}
	// With Ignore Actions off, a player takes over once the macro has no inputs left for them.
	// Holding the macro's last state there released every live press on the next step: a swift
	// click. Each player's last input frame is cached until the inputs change.
	// With Ignore Actions off, a player takes over once the macro has no inputs left for them.
	// Holding the macro's last state there released every live press on the next step: a swift
	// click. Each player's last input frame is cached until the inputs change.
	static struct {
		uint64_t revision = ~0ull;
		Frame const* data = nullptr;
		size_t size = 0;
		int first[2] = {-1, -1};
		int last[2] = {-1, -1};
	} ends;
	if (ends.revision != value.inputsRevision || ends.data != value.inputs.data() || ends.size != value.inputs.size()) {
		ends.revision = value.inputsRevision;
		ends.data = value.inputs.data();
		ends.size = value.inputs.size();
		ends.first[0] = ends.first[1] = std::numeric_limits<int>::max();
		ends.last[0] = ends.last[1] = -1;
		for (auto const& input : value.inputs) {
			int side = input.player2 ? 1 : 0;
			ends.first[side] = std::min(ends.first[side], input.number);
			ends.last[side] = std::max(ends.last[side], input.number);
		}
	}
	auto ensure = [&](bool stored2, int button) {
		int side = stored2 ? 1 : 0;
		if (!value.ignoreInputs && frame > ends.last[side]) return;
		int jump = static_cast<int>(PlayerButton::Jump);
		int left = static_cast<int>(PlayerButton::Left);
		int right = static_cast<int>(PlayerButton::Right);
		int slot = button == jump ? 0 : button == left ? 1 : button == right ? 2 : -1;
		if (slot >= 0 && queued[static_cast<size_t>((stored2 ? 3 : 0) + slot)]) return;
		auto* body = Logic::get().iconBody(play, stored2);
		if (!body || !value.playsPlayer(stored2)) return;
		// A direction pressed while controls are off only reaches the map. Re-sending it every step changes nothing until they return.
		if (slot > 0 && body->m_controlsDisabled) return;
		bool want = Logic::get().heldAt(value, stored2, frame, button);
		if (Logic::buttonHeld(body, button) == want) return;
		queue(want, button, stored2);
	};
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	ensure(false, jump);
	if (play->m_isPlatformer) {
		ensure(false, left);
		ensure(false, right);
	}
	// A normal dual follows player 1. Re-applying player 2 every step never sticks, so it clicks forever.
	if (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode) {
		ensure(true, jump);
		if (play->m_isPlatformer) {
			ensure(true, left);
			ensure(true, right);
		}
	}
}

void Logic::rememberMove(PlayerButtonCommand const& cmd) {
	auto& value = Logic::get();
	bool player2 = cmd.m_isPlayer2;
	if (cmd.m_button == PlayerButton::Left) {
		(player2 ? value.physicalLeftP2 : value.physicalLeftP1) = cmd.m_isPush;
	} else if (cmd.m_button == PlayerButton::Right) {
		(player2 ? value.physicalRightP2 : value.physicalRightP1) = cmd.m_isPush;
	}
}
void runOrbClicks(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt) {
	auto* live = Logic::asPlay(play);
	if (!live || !layer || Logic::get().predicting) return;
	if (!Orbs::hasPendingPads()) {
		Orbs::commitToolHits();
		Logic::get().suppressClickHitbox = false;
		return;
	}
	// Preserve the caller's queue by ownership transfer rather than copying it twice.
	auto saved = std::move(play->m_queuedButtons);
	play->m_queuedButtons.clear();
	Orbs::flushPads(live);
	auto& value = Logic::get();
	value.suppressClickHitbox = true;
	if (!play->m_queuedButtons.empty()) {
		// Injected pad swifts skip handleButton recording — store them here so playback can replay them.
		if (value.isRecording() && !value.restoringHolds) {
			for (auto const& cmd : play->m_queuedButtons) {
				Logic::get().recordInput(play, cmd.m_isPush, !cmd.m_isPlayer2, static_cast<int>(cmd.m_button));
			}
		}
		value.injecting = true;
		layer->GJBaseGameLayer::processQueuedButtons(dt, true);
		value.injecting = false;
	}
	Orbs::commitToolHits();
	value.suppressClickHitbox = false;
	play->m_queuedButtons = std::move(saved);
}
void Logic::pumpButtons(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt, bool clearInputQueue) {
	auto& value = Logic::get();
	bool paused = Logic::get().editorPaused;
	if (auto* live = Logic::asPlay(play)) paused = paused || live->m_isPaused;
	if (!play || !layer || value.resetting || value.refreshOnly || paused || !(value).physicsLocked()) {
		if (layer) {
			gd::vector<PlayerButtonCommand> orbs;
			if (auto* live = Logic::asPlay(play); live && !value.predicting) {
				Orbs::noteJumpPresses(live);
				size_t before = play->m_queuedButtons.size();
				Orbs::flushPads(live);
				for (size_t i = before; i < play->m_queuedButtons.size(); ++i) orbs.push_back(play->m_queuedButtons[i]);
				if (play->m_queuedButtons.size() > before) play->m_queuedButtons.resize(before);
			}
			layer->GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
			if (!orbs.empty()) {
				value.suppressClickHitbox = true;
				if (value.isRecording() && !value.restoringHolds) {
					for (auto const& cmd : orbs) {
						Logic::get().recordInput(play, cmd.m_isPush, !cmd.m_isPlayer2, static_cast<int>(cmd.m_button));
					}
				}
				play->m_queuedButtons = orbs;
				value.injecting = true;
				layer->GJBaseGameLayer::processQueuedButtons(dt, true);
				value.injecting = false;
				value.suppressClickHitbox = false;
			}
			Orbs::commitToolHits();
			if (value.isRecording() && value.maintainGravity && value.sameGravity && !value.restoringHolds && !value.resetting) Logic::get().syncSameGravity(play);
		}
		return;
	}
	Logic::get().prepareButtons(play);
	if (auto* live = Logic::asPlay(play)) Orbs::noteJumpPresses(live);
	int replay = value.replayClicksLeft;
	// The common empty-input step needs no snapshot or queue reconstruction.
	// Still flush tools and run gravity correction in their original order.
	if (play->m_queuedButtons.empty()) {
		value.applyingPlayback = false;
		if (value.isRecording() && value.maintainGravity && !value.restoringHolds) Logic::get().applyMaintainGravity(play);
		runOrbClicks(play, layer, dt);
		value.applyingPlayback = false;
		if (value.isRecording() && value.maintainGravity && value.sameGravity && !value.restoringHolds) Logic::get().syncSameGravity(play);
		if (!clearInputQueue && replay == 0) play->m_queuedButtons.clear();
		value.buttonsPrepared = false;
		return;
	}
#ifdef GEODE_IS_WINDOWS
	// Alternating press/release playback normally has exactly one command per
	// tick. Keep it in its original queue instead of snapshotting, clearing,
	// copying it back and then having the native processor remove it again.
	// Mixed queues, dual routing, retained queues and gravity/pad tools keep
	// the general path below, including its callback ordering.
	bool correctGravity = value.isRecording() && value.maintainGravity && !value.restoringHolds;
	bool singleDual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
	if (clearInputQueue && play->m_queuedButtons.size() == 1 && (replay == 0 || replay == 1) &&
		!correctGravity && (replay == 0 || !singleDual) && !Orbs::hasPendingPads()) {
		value.applyingPlayback = false;
		if (replay == 1) {
			// The general replay path commits tools before applying its commands.
			Orbs::commitToolHits();
			value.suppressClickHitbox = false;
			value.applyingPlayback = true;
		}
		layer->GJBaseGameLayer::processQueuedButtons(dt, true);
		value.applyingPlayback = false;
		if (replay == 0) {
			if (value.isRecording() && value.maintainGravity && !value.restoringHolds)
				Logic::get().applyMaintainGravity(play);
			runOrbClicks(play, layer, dt);
		}
		if (value.isRecording() && value.maintainGravity && value.sameGravity && !value.restoringHolds)
			Logic::get().syncSameGravity(play);
		value.buttonsPrepared = false;
		return;
	}
#endif
	replay::SmallSnapshot<PlayerButtonCommand, 8> queued(play->m_queuedButtons);
	if (value.isRecording() && value.maintainGravity && !value.restoringHolds) {
		int jump = static_cast<int>(PlayerButton::Jump);
		queued.removeIf([&](PlayerButtonCommand const& cmd) {
			int button = static_cast<int>(cmd.m_button);
			bool jumpButton = button == 0 || button == jump;
			return jumpButton && value.recordsPlayer(cmd.m_isPlayer2) && !value.playsPlayer(cmd.m_isPlayer2);
		});
	}
	int userCount = std::max(0, static_cast<int>(queued.size()) - replay);
	auto runQueue = [&](int from, int to, bool playback) {
		play->m_queuedButtons.clear();
		for (int i = from; i < to; ++i) play->m_queuedButtons.push_back(queued[i]);
		value.applyingPlayback = playback;
		if (!play->m_queuedButtons.empty()) layer->GJBaseGameLayer::processQueuedButtons(dt, true);
	};
	runQueue(0, userCount, false);
	if (value.isRecording() && value.maintainGravity && !value.restoringHolds) Logic::get().applyMaintainGravity(play);
	// Maintain Gravity drops every jump in the queue above, which used to delete the dash and pad swifts.
	// They run after the hold has been corrected, so a dash still clicks and the trajectory can follow it.
	runOrbClicks(play, layer, dt);
	// One command at a time so click/release/click still lands. Vanilla handleButton only —
	// pressIcon on the other dual icon was forcing controls on mid-effect and crashing.
	bool oneDual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
	value.applyingPlayback = true;
	for (int i = userCount; i < static_cast<int>(queued.size()); ++i) {
		auto const& cmd = queued[static_cast<size_t>(i)];
		auto apply = [&](bool player2) {
			PlayerButtonCommand next = cmd;
			next.m_isPlayer2 = player2;
			play->m_queuedButtons.clear();
			play->m_queuedButtons.push_back(next);
			layer->GJBaseGameLayer::processQueuedButtons(dt, true);
		};
		uint32_t partnerBefore = value.partnerPresses;
		apply(cmd.m_isPlayer2);
		// Vanilla already clicks both icons of a 1-player dual. Only a press routed to one icon needs the copy.
		bool partnerClicked = value.partnerPresses != partnerBefore;
		if (oneDual && !cmd.m_isPlayer2 && !partnerClicked && play->m_player2 && !play->m_player2->m_isDead && !play->m_player2->m_controlsDisabled) {
			apply(true);
		}
	}
	value.applyingPlayback = false;
	// Recording only. A played player keeps the gravity the macro stored.
	if (value.isRecording() && value.maintainGravity && value.sameGravity && !value.restoringHolds) Logic::get().syncSameGravity(play);
	if (!clearInputQueue && replay == 0) {
		play->m_queuedButtons.assign(queued.begin(), queued.end());
	}
	value.buttonsPrepared = false;
}
void Logic::offsetInputs(int delta, int first, int last) {
	auto& value = Logic::get();
	int n = static_cast<int>(value.inputs.size());
	bool all = first < 0 || last < 0;
	if (all) {
		first = 0;
		last = n - 1;
	}
	if (first > last) std::swap(first, last);
	first = std::max(first, 0);
	last = std::min(last, n - 1);
	for (int i = first; i <= last; ++i) {
		if (value.inputs[static_cast<size_t>(i)].number + delta >= 0) value.inputs[static_cast<size_t>(i)].number += delta;
	}
	if (all) {
		for (auto& change : value.rates) change.frame = std::max(change.frame + delta, 0);
		Logic::get().normalizeRates();
	}
	value.touchInputs();
	Logic::get().sortInputs();
	Logic::get().message = fmt::format("Shifted inputs by {}", delta);
	Logic::get().refreshPopup();
	Logic::get().autosaveReplay();
}
void Logic::randomOffsetInputs(int magnitude, int first, int last) {
	if (magnitude < 0) magnitude = -magnitude;
	auto& value = Logic::get();
	int n = static_cast<int>(value.inputs.size());
	bool all = first < 0 || last < 0;
	if (all) {
		first = 0;
		last = n - 1;
	}
	if (first > last) std::swap(first, last);
	first = std::max(first, 0);
	last = std::min(last, n - 1);
	std::mt19937 rng{std::random_device{}()};
	std::uniform_int_distribution<int> dist(-magnitude, magnitude);
	for (int i = first; i <= last; ++i) {
		int next = value.inputs[static_cast<size_t>(i)].number + dist(rng);
		value.inputs[static_cast<size_t>(i)].number = std::max(next, 0);
	}
	value.touchInputs();
	Logic::get().sortInputs();
	Logic::get().message = fmt::format("Randomly offset inputs by +/- {}", magnitude);
	Logic::get().refreshPopup();
	Logic::get().autosaveReplay();
}
void Logic::removePlayerInputs(bool player2) {
	auto& value = Logic::get();
	replay::compact(value.inputs, value.poses, value.inputIds,
		[player2](Frame const& input) { return input.player2 != player2; });
	// An explicit editor deletion can return unused storage; attempt trimming
	// deliberately keeps its buffers to avoid reallocating while recording.
	value.inputs.shrink_to_fit();
	value.poses.shrink_to_fit();
	value.inputIds.shrink_to_fit();
	value.touchInputs();
	Logic::get().refreshLiveHold();
	Logic::get().recomputeCps(Logic::get());
	Logic::get().message = player2 ? "Removed player 2 inputs" : "Removed player 1 inputs";
	Logic::get().refreshPopup();
	Logic::get().autosaveReplay();
}
void Logic::flipInputHolds() {
	for (auto& input : Logic::get().inputs) input.holding = !input.holding;
	Logic::get().refreshLiveHold();
	Logic::get().recomputeCps(Logic::get());
	Logic::get().message = "Flipped presses and releases";
	Logic::get().refreshPopup();
	Logic::get().autosaveReplay();
}
void Logic::flipInputPlayers() {
	for (auto& input : Logic::get().inputs) input.player2 = !input.player2;
	Logic::get().refreshLiveHold();
	Logic::get().recomputeCps(Logic::get());
	Logic::get().message = "Flipped player 1 and 2";
	Logic::get().refreshPopup();
	Logic::get().autosaveReplay();
}
void Logic::sortInputs() {
	auto& value = Logic::get();
	auto& inputs = value.inputs;
	// Most attempt resets already have ordered inputs. Do not allocate and sort
	// a full permutation (and its debug/id copies) just to discover that nothing moved.
	if (std::is_sorted(inputs.begin(), inputs.end(), [](Frame const& a, Frame const& b) {
		return a.number != b.number ? a.number < b.number : a.player2 < b.player2;
	})) {
		// Sequence replacement may leave the allocation and final input unchanged.
		// Refresh its metadata even when the replacement needs no permutation.
		value.touchInputs();
		Logic::get().refreshLiveHold();
		Logic::get().recomputeCps(value);
		return;
	}
	std::vector<size_t> order(inputs.size());
	std::iota(order.begin(), order.end(), 0);
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		if (inputs[a].number == inputs[b].number) return inputs[a].player2 < inputs[b].player2;
		return inputs[a].number < inputs[b].number;
	});
	bool moved = false;
	for (size_t i = 0; i < order.size(); ++i) if (order[i] != i) moved = true;
	if (moved) {
		auto permute = [&](auto& side) {
			if (side.size() != order.size()) return;
			using Value = typename std::decay_t<decltype(side)>::value_type;
			std::vector<Value> next;
			next.reserve(order.size());
			for (size_t i : order) next.push_back(side[i]);
			side.swap(next);
		};
		permute(inputs);
		permute(value.poses);
		permute(value.inputIds);
		value.touchInputs();
		Logic::get().refreshLiveHold();
	}
	Logic::get().recomputeCps(Logic::get());
}
uint64_t Logic::mintInputId() {
	return ++Logic::get().nextInputId;
}
// The editor calls this every frame. Inputs already given an id are skipped until the list is
// edited, so only newly recorded ones are looked at.
void Logic::identifyInputs(bool force) {
	auto& value = Logic::get();
	if (!force && value.isRecording() && value.inputIds.empty()) return;
	struct Seen {
		std::vector<Frame> const* owner = nullptr;
		Frame const* data = nullptr;
		size_t count = 0;
		uint64_t revision = 0;
		uint64_t next = 0;
		uint64_t lastId = 0;
	};
	thread_local Seen seen;
	auto& inputs = value.inputs;
	bool kept = seen.owner == &inputs && seen.revision == value.inputsRevision && seen.count <= inputs.size() && value.nextInputId >= seen.next;
	if (kept && seen.count > 0) kept = value.inputId(seen.count - 1) == seen.lastId;
	if (kept && seen.count == inputs.size()) kept = seen.data == inputs.data();
	size_t from = kept ? seen.count : 0;
	uint64_t highest = value.nextInputId;
	for (size_t i = from; i < inputs.size(); ++i) {
		uint64_t id = value.inputId(i);
		if (id > highest) highest = id;
	}
	value.nextInputId = highest;
	for (size_t i = from; i < inputs.size(); ++i) {
		if (value.inputId(i) != 0) continue;
		value.setInputId(i, ++value.nextInputId);
	}
	seen = {&inputs, inputs.data(), inputs.size(), value.inputsRevision, value.nextInputId, inputs.empty() ? 0 : value.inputId(inputs.size() - 1)};
}
void Logic::clearInputs() {
	auto& value = Logic::get();
	value.inputs.clear();
	value.clearSides();
	value.macroUnrounded = value.velocityUnrounding;
	value.macroSilicateVariance = false;
	// Explicit Clear releases storage; practice trimming retains it for the next attempt.
	std::vector<Frame>().swap(value.inputs);
	std::vector<FramePose>().swap(value.poses);
	std::vector<uint64_t>().swap(value.inputIds);
	Logic::get().releaseReplayIndex();
	value.touchInputs();
	value.rates.clear();
	value.replayPos = 0;
	value.levelSeedFromFile = false;
	if (value.seedOverride) {
		value.levelSeed = value.seedOverrideValue;
		value.levelSeedKnown = true;
	} else {
		value.levelSeed = 0;
		value.levelSeedKnown = false;
	}
	value.message = "Cleared inputs";
	Logic::get().refreshPopup();
}











void Logic::refreshPopup() {
	auto* scene = CCDirector::sharedDirector()->getRunningScene();
	if (!scene) return;
	auto* popup = scene->getChildByIDRecursive("echo-popup");
	if (!popup) return;
	auto const& value = Logic::get();
	if (auto* primary = typeinfo_cast<CCLabelBMFont*>(popup->getChildByIDRecursive("echo-status-primary"))) {
		int frame = PlayLayer::get() ? Logic::get().getFrame(PlayLayer::get()) : 0;
		primary->setString(fmt::format(
			"{}  frame {}  inputs {}  {:.2f}x  {:.0f} fps",
			Logic::get().stateName(value.state),
			frame,
			value.inputs.size(),
			value.speedhack,
			value.fps
		).c_str());
	}
	if (auto* secondary = typeinfo_cast<CCLabelBMFont*>(popup->getChildByIDRecursive("echo-status-secondary"))) {
		secondary->setString(value.message.empty() ? value.highestCps.c_str() : value.message.c_str());
	}
}
void Logic::notify(std::string const& text, bool success) {
	Notification::create(text, success ? NotificationIcon::Success : NotificationIcon::Error)->show();
	Logic::get().message = text;
	Logic::get().refreshPopup();
}
// Jump, left, and right presses all count. Releases do not. Trajectory never reaches handleButton.
void Logic::noteClick(PlayLayer* play, bool down, bool player1, int button) {
	auto& value = Logic::get();
	if (!down || !play) return;
	if (value.resetting || value.refreshOnly || value.ignoreRecordFrames > 0) return;
	int jump = static_cast<int>(PlayerButton::Jump);
	int left = static_cast<int>(PlayerButton::Left);
	int right = static_cast<int>(PlayerButton::Right);
	if (button != 0 && button != jump && button != left && button != right) return;
	if (button == 0) button = jump;
	PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
	if (body) {
		if (Logic::buttonHeld(body, button) || (button == jump && Logic::buttonHeld(body, 0))) return;
	}
	bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode;
	bool oneDual = dual && !two;
	// A 1-player dual is one button. Both icons can click, but CPS and the macro stay on player 1.
	if (oneDual) player1 = true;
	bool player2 = !player1;
	// Separate P2 CPS only while a 2-player level is actually in dual. Solo sections stay on P1.
	if (player2 && !(two && dual)) return;
	auto& clicks = player2 ? value.clicksP2 : value.clicksP1;
	// Same frame the macro stores. stepFrame has not advanced yet for this step.
	int now = Logic::get().stepFrame + 1;
	clicks.push_back(now);
	double nowT = value.secondsAt(now);
	size_t before = clicks.size();
	while (clicks.size() > 1 && nowT - value.secondsAt(clicks.front()) > 2.0) clicks.pop_front();
	while (clicks.size() > 4096) clicks.pop_front();
	if (clicks.size() != before) ++value.clicksRevision;
}
namespace {
// Clicks are stored in the order they happen, so their frames only go up and the last second of
// them is found by binary search.
struct RecentClicks {
	int inSecond = 0;
	int onFrame = 0;
};
RecentClicks recentClicks(Session const& value, std::deque<int> const& clicks, int now) {
	double nowT = value.secondsAt(now);
	auto end = std::upper_bound(clicks.begin(), clicks.end(), now);
	auto start = std::partition_point(clicks.begin(), end, [&](int frame) { return nowT - value.secondsAt(frame) > 1.0; });
	auto same = std::lower_bound(start, end, now);
	return {static_cast<int>(std::distance(start, end)), static_cast<int>(std::distance(same, end))};
}

// For each click i, the best of 48 * time(click) - index over clicks 0..i. A burst ending at the
// newest click breaks the 48/s rule exactly when that best clears 48 * time(newest) - count.
bool sameRates(std::vector<RateChange> const& a, std::vector<RateChange> const& b) {
	return std::ranges::equal(a, b, [](RateChange const& x, RateChange const& y) { return x.frame == y.frame && x.fps == y.fps; });
}
double burstPeak(Session const& value, std::deque<int> const& clicks) {
	struct State {
		std::deque<int> const* owner = nullptr;
		uint64_t revision = 0;
		float macroFps = 0.f;
		std::vector<RateChange> rates;
		size_t covered = 0;
		double best = 0.0;
	};
	thread_local std::array<State, 2> caches;
	auto& state = &clicks == &value.clicksP2 ? caches[1] : caches[0];
	bool kept = state.owner == &clicks && state.revision == value.clicksRevision && state.covered <= clicks.size() &&
		state.macroFps == value.macroFps && sameRates(state.rates, value.rates);
	if (!kept) {
		state = {};
		state.owner = &clicks;
		state.revision = value.clicksRevision;
		state.macroFps = value.macroFps;
		state.rates = value.rates;
	}
	int count = static_cast<int>(clicks.size());
	int target = count - Logic::kMinBurstClicks;
	if (target < 0) return 0.0;
	double rate = static_cast<double>(Logic::kMaxBurstRate);
	while (state.covered <= static_cast<size_t>(target)) {
		double lead = rate * value.secondsAt(clicks[state.covered]) - static_cast<double>(state.covered);
		state.best = state.covered == 0 ? lead : std::max(state.best, lead);
		++state.covered;
	}
	return state.best;
}
}
unsigned Logic::countPressesInLastSecond(bool player2) {
	auto const& value = Logic::get();
	int now = PlayLayer::get() ? Logic::get().getFrame(PlayLayer::get()) : 0;
	auto const& clicks = player2 ? value.clicksP2 : value.clicksP1;
	return static_cast<unsigned>(recentClicks(value, clicks, now).inSecond);
}
bool Logic::burstTooFast(Session const& value, int clicks, int first, int last) const {
	if (clicks < Logic::kMinBurstClicks) return false;
	double span = value.secondsAt(last) - value.secondsAt(first);
	if (!(span > 0.0)) return true;
	return static_cast<double>(clicks) / span > Logic::kMaxBurstRate;
}
bool Logic::playerCpsBroken(std::deque<int> const& clicks, int now, float maxPerSecond) const {
	if (clicks.empty()) return false;
	auto const& value = Logic::get();
	auto recent = recentClicks(value, clicks, now);
	if (static_cast<float>(recent.inSecond) > maxPerSecond) return true;
	if (recent.onFrame > Logic::kMaxPerFrame) return true;
	int n = static_cast<int>(clicks.size());
	double backT = value.secondsAt(clicks.back());
	if (n < Logic::kMinBurstClicks || value.secondsAt(now) - backT > 1.0) return false;
	double need = static_cast<double>(Logic::kMaxBurstRate) * backT - static_cast<double>(n);
	double slack = 1e-6 + 1e-12 * std::abs(need);
	double top = burstPeak(value, clicks);
	if (top > need + slack) return true;
	if (top <= need - slack) return false;
	for (int start = 0; start <= n - Logic::kMinBurstClicks; ++start) {
		if (Logic::get().burstTooFast(value, n - start, clicks[static_cast<size_t>(start)], clicks.back())) return true;
	}
	return false;
}
void Logic::setTps(float rate) {
	auto& value = Logic::get();
	rate = std::clamp(rate, 1.f, FLT_MAX);
	if (!std::isfinite(rate)) return;
	if (!kStepPatch && rate < kVanillaTps) {
		value.message = "TPS below 240 requires the physics step patch";
		rate = kVanillaTps;
	}
	if ((value).isRecording()) {
		int frame = PlayLayer::get() ? Logic::get().getFrame(PlayLayer::get()) : 0;
		Logic::get().placeRate(frame, rate);
		value.message = fmt::format("TPS {:.2f} at frame {}", rate, frame);
		Logic::get().refreshPopup();
	} else if (!(value).isPlaying()) {
		value.macroFps = rate;
		value.fps = rate;
		Logic::get().pushRuntimeSettings();
	}
	if (value.playbackFps < 1.f) {
		if (auto* director = CCDirector::sharedDirector()) director->setAnimationInterval(1.f / rate);
	}
}
void Logic::placeRate(int frame, float rate) {
	auto& value = Logic::get();
	rate = std::clamp(rate, 1.f, FLT_MAX);
	if (!std::isfinite(rate)) return;
	if (frame <= 0) {
		value.macroFps = rate;
		value.fps = rate;
		std::erase_if(value.rates, [](RateChange const& change) { return change.frame <= 0; });
		return;
	}
	float previous = value.rateAt(frame - 1);
	auto at = std::find_if(value.rates.begin(), value.rates.end(), [frame](RateChange const& change) {
		return change.frame == frame;
	});
	if (rate == previous) {
		if (at != value.rates.end()) value.rates.erase(at);
		value.fps = rate;
		return;
	}
	if (at != value.rates.end()) at->fps = rate;
	else {
		value.rates.push_back({frame, rate});
		std::stable_sort(value.rates.begin(), value.rates.end(), [](RateChange const& a, RateChange const& b) {
			return a.frame < b.frame;
		});
	}
	value.fps = rate;
}
void Logic::applyRate(int frame) {
	auto& value = Logic::get();
	float next = value.rateAt(frame);
	if (!std::isfinite(next) || next < 1.f) next = 240.f;
	if (next == value.fps) return;
	value.fps = next;
	if (value.displayFpsLocked && value.playbackFps < 1.f && !Logic::get().renderCapturing()) {
		if (auto* director = CCDirector::sharedDirector()) director->setAnimationInterval(1.f / next);
	}
}
void Logic::normalizeRates() {
	auto& value = Logic::get();
	std::erase_if(value.rates, [](RateChange const& change) {
		return change.frame < 0 || !std::isfinite(change.fps) || change.fps < 1.f;
	});
	std::stable_sort(value.rates.begin(), value.rates.end(), [](RateChange const& a, RateChange const& b) {
		return a.frame < b.frame;
	});
	std::vector<RateChange> kept;
	float rate = value.macroFps;
	for (auto const& change : value.rates) {
		if (!kept.empty() && kept.back().frame == change.frame) kept.back() = change;
		else kept.push_back(change);
	}
	value.rates.clear();
	for (auto const& change : kept) {
		if (change.fps == rate) continue;
		value.rates.push_back(change);
		rate = change.fps;
	}
}
void Logic::trimRates(int frame) {
	std::erase_if(Logic::get().rates, [frame](RateChange const& change) { return change.frame >= frame; });
	Logic::get().applyRate(std::max(frame, 0));
}
void Logic::updateCps(PlayLayer* play) {
	auto& value = Logic::get();
	auto holding = [](PlayerObject* player) {
		if (!player) return false;
		static constexpr int buttons[] = {
			static_cast<int>(PlayerButton::Jump),
			static_cast<int>(PlayerButton::Left),
			static_cast<int>(PlayerButton::Right),
		};
		return std::ranges::any_of(buttons, [player](int button) { return Logic::buttonHeld(player, button); });
	};
	value.currentlyPressing = holding(play->m_player1) || (play->m_gameState.m_isDualMode && holding(play->m_player2));
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool twoDual = twoPlayer && play->m_gameState.m_isDualMode;
	if (!twoDual && !value.clicksP2.empty()) {
		value.clicksP2.clear();
		++value.clicksRevision;
	}
	unsigned p1 = Logic::get().countPressesInLastSecond(false);
	unsigned p2 = twoDual ? Logic::get().countPressesInLastSecond(true) : 0;
	value.currentCps = static_cast<float>(twoDual ? std::max(p1, p2) : p1);
	int now = Logic::get().getFrame(play);
	float limit = std::max(static_cast<float>(value.maxCps), 1.f);
	value.cpsBreaking = Logic::get().playerCpsBroken(value.clicksP1, now, limit);
	if (twoDual) value.cpsBreaking = value.cpsBreaking || Logic::get().playerCpsBroken(value.clicksP2, now, limit);
	if (value.cpsBreaking) value.overMaxCps = true;
}





void Logic::tickAutoclicker(PlayLayer* play) {
	auto& value = Logic::get();
	if (!value.autoclicker || !play || play->m_isPaused) return;
	auto& click = value.autoClick;
	if (click.pressed) ++click.sincePress;
	else ++click.sinceRelease;

	int jump = static_cast<int>(PlayerButton::Jump);
	auto pressPlayer = [&](bool player1) {
		bool twoPlayer = play->m_level && play->m_level->m_twoPlayerMode;
		if (!player1 && !twoPlayer) return;
		play->queueButton(jump, true, !player1, Logic::get().clickStamp());
	};
	auto releasePlayer = [&](bool player1) {
		bool twoPlayer = play->m_level && play->m_level->m_twoPlayerMode;
		if (!player1 && !twoPlayer) return;
		play->queueButton(jump, false, !player1, Logic::get().clickStamp());
	};

	if (!click.pressed && click.sinceRelease >= click.releaseInterval) {
		click.pressed = true;
		click.sinceRelease = 0;
		click.sincePress = 0;
		if (value.autoclickP1) pressPlayer(true);
		if (value.autoclickP2) pressPlayer(false);
		if (click.pressInterval <= 0) {
			click.pressed = false;
			if (value.autoclickP1) releasePlayer(true);
			if (value.autoclickP2) releasePlayer(false);
		}
	} else if (click.pressed && click.sincePress >= click.pressInterval) {
		click.pressed = false;
		click.sincePress = 0;
		if (value.autoclickP1) releasePlayer(true);
		if (value.autoclickP2) releasePlayer(false);
	}

	if (value.autoclickAutoDisable) {
		--click.disableIn;
		if (click.disableIn <= 0) {
			value.autoclicker = false;
			value.autoclickAutoDisable = false;
			click.disableIn = 1;
			Logic::writeBool("autoclicker", false);
			value.message = "Autoclicker disabled";
		}
	}
}

int Logic::followSteps() const {
	double steps = followDelayMs ? followDelay / 1000.0 * sampleRate() : followDelay;
	if (!std::isfinite(steps)) return 0;
	return std::max(0, static_cast<int>(std::lround(steps)));
}
bool Logic::followActive(GJBaseGameLayer* play) const {
	// Click Both already moves both at once, and a macro being played drives the players itself.
	// A backstep's replay plays the recording back, but the attempt is still the recording.
	bool macro = backstepTarget >= 0 ? (backstepState == RunState::Playing || backstepState == RunState::RecordingAndPlaying) : isPlaying();
	if (followMode == 0 || clickBoth || macro || !play) return false;
	return play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
}
void Logic::noteFollowPress(GJBaseGameLayer* play, bool down, int button) {
	followLog.push_back({stepFrame + 1, play->m_gameState.m_levelTime, down, button});
	while (followLog.size() > 65536) followLog.pop_front();
}
// A delay in ms is measured on the level's own clock, not converted to steps through Echo's TPS:
// the TPS can come from elsewhere (another mod's TPS bypass), and the step count per second with it.
bool Logic::followDue(FollowPress const& press, int frame, double time) const {
	if (followDelayMs) return press.time + std::max(followDelay, 0.f) / 1000.0 <= time + 1e-7;
	return press.frame + followSteps() <= frame;
}
// After a respawn or a backstep: the copies due from the step about to run on come again.
void Logic::rewindFollow(GJBaseGameLayer* play) {
	std::erase_if(followLog, [this](auto const& press) { return press.frame > stepFrame; });
	if (!play) return;
	double time = play->m_gameState.m_levelTime;
	// Sent: its copy was due at a step before the one about to run (which starts at `time`).
	double delay = std::max(followDelay, 0.f) / 1000.0;
	for (auto& press : followLog) press.sent = followDelayMs ? press.time + delay < time - 1e-7 : press.frame + followSteps() <= stepFrame;
}
// At the start of each live step: the follower gets the presses the leader made the delay ago.
void Logic::runFollow(GJBaseGameLayer* play) {
	if (!followActive(play) || predicting) return;
	bool follower2 = followMode == 1;
	int frame = stepFrame + 1;
	double time = play->m_gameState.m_levelTime;
	// During a backstep's replay the copies are already in the recording being replayed.
	bool replaying = backstepTarget >= 0;
	std::vector<std::pair<bool, int>> presses;
	for (auto& press : followLog) {
		if (press.sent) continue;
		if (!followDue(press, frame, time)) break;
		press.sent = true;
		if (!replaying) presses.push_back({press.down, press.button});
	}
	for (auto [down, button] : presses) {
		followInjecting = true;
		routingInput = true;
		play->handleButton(down, button == 0 ? static_cast<int>(PlayerButton::Jump) : button, !follower2);
		routingInput = false;
		followInjecting = false;
	}
}
