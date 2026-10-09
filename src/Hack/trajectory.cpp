#include "Hack/trajectory.hpp"
#include "Hack/level_forecast.hpp"
#include "Hack/trajectory_path.hpp"
#include "Hack/orbs.hpp"
#include "Logic/logic.hpp"
#include "Physics/checkpoint.hpp"
#include "Physics/collisions.hpp"
#include "Physics/object.hpp"
#include "Physics/prediction_art.hpp"
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
}

void Trajectory::park(PlayerObject* player) {
	if (!player) return;
	player->setVisible(false);
	player->m_isHidden = true;
	player->stopAllActions();
	player->unscheduleUpdate();
	// A fake left on the live player stays in the collision grid and the next real step hits it.
	player->setPosition({-100000.f, -100000.f});
	player->m_position = CCPoint{-100000.f, -100000.f};
	if (auto* play = PlayLayer::get()) play->removeObjectFromSection(player);
}

PlayerObject* Trajectory::spawnFake(PlayLayer* play, char const* id) {
	auto* player = PlayerObject::create(1, 1, play, play, true);
	player->retain();
	player->setID(id);
	player->setVisible(false);
	player->m_isHidden = true;
	player->setOpacity(0);
	player->stopAllActions();
	player->unscheduleUpdate();
	player->setPosition({-100000.f, -100000.f});
	if (player->m_iconSprite) player->m_iconSprite->setVisible(false);
	if (player->m_iconSpriteSecondary) player->m_iconSpriteSecondary->setVisible(false);
	if (player->m_iconSpriteWhitener) player->m_iconSpriteWhitener->setVisible(false);
	player->removeFromParentAndCleanup(false);
	park(player);
	return player;
}

void Trajectory::attach(PlayLayer* play) {
	detach();
	if (!play || !play->m_objectLayer) return;
	node = CCDrawNode::create();
	node->retain();
	node->m_bUseArea = false;
	node->setID("trajectory"_spr);
	node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
	Logic::get().seatOverlay(node, play);
	fake1 = spawnFake(play, "trajectory-player-1"_spr);
	fake2 = spawnFake(play, "trajectory-player-2"_spr);
}

void Trajectory::syncSeat(PlayLayer* play) {
	if (!node || !play) return;
	Logic::get().seatOverlay(node, play);
}

void Trajectory::detach() {
	auto drop = [](auto*& object) {
		if (!object) return;
		object->stopAllActions();
		object->unscheduleAllSelectors();
		object->removeFromParentAndCleanup(true);
		object->release();
		object = nullptr;
	};
	drop(fake1);
	drop(fake2);
	drop(node);
	usedP1.clear();
	usedP2.clear();
	busy = false;
	cached = false;
	delta = 0.f;
	Logic::get().predicting = false;
	Logic::get().syncPredictHooks();
}

void Trajectory::noteButton(bool player1, bool down) {
	(player1 ? heldP1 : heldP2) = down;
}

bool Trajectory::markDead(PlayerObject* player) {
	if (!player) return false;
	if (player == fake1) deadP1 = true;
	else if (player == fake2) deadP2 = true;
	else return false;
	return true;
}

bool Trajectory::markPredictDeath(PlayerObject* player) {
	if (markDead(player)) return true;
	// Solids/slopes often call destroyPlayer on the live body while colliding a fake.
	if (collidingFake && markDead(collidingFake)) return true;
	auto* play = PlayLayer::get();
	if (!play || !player) return false;
	if (player == play->m_player1) {
		deadP1 = true;
		return true;
	}
	if (player == play->m_player2) {
		deadP2 = true;
		return true;
	}
	return false;
}

bool Trajectory::fakeActivated(PlayerObject* player, EnhancedGameObject* object) const {
	if (!object || object->m_isMultiActivate) return false;
	if (player == fake1) return usedP1.contains(object);
	if (player == fake2) return usedP2.contains(object);
	return false;
}

bool Trajectory::realActivated(PlayerObject* player, EnhancedGameObject* object) const {
	if (!object) return false;
	auto* layer = GJBaseGameLayer::get();
	PlayerObject* real = player;
	if (layer && player == fake1) real = layer->m_player1;
	else if (layer && player == fake2) real = layer->m_player2;
	return phys::hasBeenActivatedByPlayer(real, object);
}

void Trajectory::rememberActivation(EnhancedGameObject* object, PlayerObject* player) {
	if (player == fake1) usedP1.insert(object);
	else if (player == fake2) usedP2.insert(object);
}

ccColor4F Trajectory::colorFor(PlayerObject* fake, Click click) const {
	auto const& value = Logic::get();
	float const* c = value.trajectoryColor;
	switch (click) {
		case Click::Swift: c = value.trajectorySwiftColor; break;
		case Click::Release: c = value.trajectoryReleaseColor; break;
		case Click::Hold:
		case Click::Replay: break;
	}
	if (fake == fake2) return {1.f - c[0], 1.f - c[1], 1.f - c[2], c[3]};
	return {c[0], c[1], c[2], c[3]};
}

float Trajectory::lineWidth(PlayLayer* play) const {
	return Logic::get().trajectoryWidth / std::max(play->m_gameState.m_cameraZoom, 0.05f);
}

Trajectory::Key Trajectory::makeKey(PlayLayer* play) const {
	auto const& value = Logic::get();
	Key key;
	key.frame = Logic::get().getFrame(play);
	auto fill = [](PlayerObject* player, float* out) {
		if (!player) return;
		out[0] = player->m_position.x;
		out[1] = player->m_position.y;
		out[2] = player->getRotation();
		out[3] = static_cast<float>(player->m_yVelocity);
		out[4] = static_cast<float>(player->m_platformerXVelocity);
		out[5] = static_cast<float>((player->m_holdingLeft ? 1 : 0) | (player->m_holdingRight ? 2 : 0) | (player->m_leftPressedFirst ? 4 : 0));
	};
	fill(play->m_player1, key.p1);
	fill(play->m_player2, key.p2);
	key.dual = play->m_gameState.m_isDualMode;
	auto jumpHeld = [](PlayerObject* body) {
		if (!body) return false;
		int jump = static_cast<int>(PlayerButton::Jump);
		auto found = body->m_holdingButtons.find(jump);
		return found != body->m_holdingButtons.end() && found->second;
	};
	// Maintain Gravity on wave releases/represses jump to steer gravity. The real hold is the key/UI.
	if (value.maintainGravity) {
		key.heldP1 = Logic::get().physicalHold(play, false);
		key.heldP2 = Logic::get().physicalHold(play, true);
	} else {
		key.heldP1 = jumpHeld(play->m_player1);
		key.heldP2 = jumpHeld(play->m_player2);
	}
	key.dead = play->m_playerDied;
	key.playing = value.isPlaying();
	key.recording = value.isRecording();
	key.hold = value.trajectoryHold;
	key.swift = value.trajectorySwift;
	key.release = value.trajectoryRelease;
	key.dashOrbs = Orbs::clickOrbAssist(false) || Orbs::clickOrbAssist(true);
	key.blackOrbs = key.dashOrbs && value.clickBlackOrbs;
	key.padBlack = value.padBlackOrb;
	key.padBlue = value.padBlueOrb;
	key.pauseKill = value.pauseBeforeKill && value.isRecording() && value.showTrajectory;
	key.pauseLength = value.pauseAtTrajectoryLength;
	key.pauseMs = value.pauseTrajectoryMs;
	key.hideP1 = Logic::get().trajectoryPlayerHidden(false);
	key.hideP2 = Logic::get().trajectoryPlayerHidden(true);
	key.orbs = Orbs::poseHash(play, play->m_player1);
	if (play->m_player2) key.orbs ^= Orbs::poseHash(play, play->m_player2) + 0x9e3779b9u;
	key.seconds = value.trajectorySeconds;
	key.width = value.trajectoryWidth;
	key.zoom = play->m_gameState.m_cameraZoom;
	std::copy(std::begin(value.trajectoryColor), std::end(value.trajectoryColor), key.color);
	std::copy(std::begin(value.trajectorySwiftColor), std::end(value.trajectorySwiftColor), key.swiftColor);
	std::copy(std::begin(value.trajectoryReleaseColor), std::end(value.trajectoryReleaseColor), key.releaseColor);
	return key;
}

// Taken directly from Silicate (GPL-3.0): the calls that copy a real player's state onto a fake.
void Trajectory::seed(PlayerObject* fake, PlayerObject* real) {
	fake->copyAttributes(real);
	SavedPlayerCheckpoint checkpoint = SavedPlayerCheckpoint::create(real);
	checkpoint.apply(fake);
	// The copy includes the real player's pending practice checkpoint. A fake that switches mode
	// (toggleBirdMode and the rest call removePendingCheckpoint) would free the real player's
	// checkpoint and leave it pointing at freed memory, crashing later.
	fake->m_pendingCheckpoint = nullptr;
	// Checkpoint application copies the live visual flags too. Restore the
	// prediction-only presentation flags AFTER applying the physics state.
	// m_maybeReducedEffects stays as the real player has it: it is not a presentation flag.
	// GD sets it on its dummy players, and PlayerObject::flipGravity then skips halving the
	// y velocity (the ball's click, every gravity orb and pad), spiderTestJumpInternal returns
	// before teleporting, and ringJump, stopDashing, toggleDartMode, togglePlayerScale and
	// updateTimeMod take their dummy paths. The effects it used to hide stay hidden by the
	// predicting hooks and by low-detail mode during predictions (simulate).
	fake->m_maybeReducedEffects = real->m_maybeReducedEffects;
	fake->m_isHidden = true;
	fake->m_playEffects = false;
	fake->setPosition(real->m_position);
	fake->setRotation(real->getRotation());
	fake->m_isObjectRectDirty = true;
	fake->updateOrientedBox();
}

// GJBaseGameLayer::toggleDualMode (GD 2.2081, win 0x2168d0), its gameplay half on the fakes. The
// layer call itself cannot run here: it adds the second player to the first one's parent (a fake
// has none), and touches the UI layer, particles and streaks.
void Trajectory::dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on) {
	auto& state = layer->m_gameState;
	if (state.m_isDualMode == on || !fake1 || !fake2) return;
	bool twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
	// playerWillSwitchMode and updateDualGround read the layer's player slots.
	auto withFakes = [&](auto&& action) {
		PlayerObject* one = layer->m_player1;
		PlayerObject* two = layer->m_player2;
		layer->m_player1 = fake1;
		layer->m_player2 = fake2;
		action();
		layer->m_player1 = one;
		layer->m_player2 = two;
	};
	if (on) {
		// Only the first player can be alone; the second follows it in.
		if (player != fake1 || pairing) return;
		state.m_isDualMode = true;
		fake1->m_unkA99 = true;
		if (joinReal) seed(fake2, joinReal);
		fake2->m_unkA99 = true;
		fake2->resetObject();
		fake2->copyAttributes(fake1);
		bool unlinked = state.m_unlinkDualGravity;
		fake2->flipGravity(unlinked ? fake1->m_isUpsideDown : !fake1->m_isUpsideDown, true);
		fake2->setYVelocity(unlinked ? fake1->m_yVelocity : -fake1->m_yVelocity, 0);
		fake2->m_isOnGround = false;
		fake2->m_isOnGround2 = false;
		if (twoPlayer) {
			if (!fake2->m_inputsLocked) fake2->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = false;
			if (!fake2->m_controlsDisabled) {
				fake2->m_jumpBuffered = false;
				fake2->m_stateRingJump = false;
				fake2->m_touchedPad = true;
				if (fake2->m_isDashing) fake2->stopDashing();
			}
		}
		// resetObject shows the icon again.
		fake2->setVisible(false);
		fake2->m_isHidden = true;
		fake2->m_playEffects = false;
		fake2->m_isObjectRectDirty = true;
		state.m_lastActivatedPortal2 = portal;
		withFakes([&] { layer->playerWillSwitchMode(player, portal); });
		joined = joinable;
		return;
	}
	state.m_isDualMode = false;
	fake1->m_unkA99 = false;
	fake2->m_unkA99 = false;
	// The first player carries on from wherever the one that touched the portal is.
	if (player == fake2) fake1->copyAttributes(fake2);
	fake2->releaseAllButtons();
	// (The portal's free-mode camera settings are camera only.)
	state.m_lastActivatedPortal2 = nullptr;
	int mode = 6;
	if (fake1->m_isShip || fake1->m_isBird || fake1->m_isDart || fake1->m_isSwing) mode = 5;
	else if (fake1->m_isBall) mode = 0x10;
	else if (fake1->m_isSpider) mode = 0x21;
	withFakes([&] { layer->updateDualGround(player, mode, false, 0.f); });
	parted = true;
}

void Trajectory::press(PlayLayer* play, PlayerObject* fake, Click click) {
	// When Click Dash/Black Orbs is on (recording only), stash rings during the press so hold-through
	// does not take them — clickFake applies the same swift the live click would.
	// When those settings are off, a normal press must be able to take overlapping orbs.
	auto& value = Logic::get();
	bool stashOrbs = Orbs::clickOrbAssist(fake == fake2);
	if (stashOrbs) value.suppressRings = true;
	switch (click) {
		case Click::Replay: break;
		case Click::Hold:
			fake->pushButton(PlayerButton::Jump);
			break;
		case Click::Swift:
			// A tap while holding is a release and a fresh press, or the press is ignored.
			fake->releaseButton(PlayerButton::Jump);
			fake->m_jumpBuffered = false;
			fake->pushButton(PlayerButton::Jump);
			fake->releaseButton(PlayerButton::Jump);
			break;
		case Click::Release:
			fake->releaseButton(PlayerButton::Jump);
			fake->m_jumpBuffered = false;
			break;
	}
	if (stashOrbs) value.suppressRings = false;
}

bool Trajectory::advance(PlayLayer* play, PlayerObject* fake, float stepDelta, bool draw, Click click) {
	CCPoint from = fake->getPosition();

	// Taken directly from Silicate (GPL-3.0): the order GD advances a player within one physics step.
	fake->m_totalTime += Logic::get().physicsStep();
	auto clearLog = [&](CCDictionary* log) {
		if (!log) return;
		auto shared = [&](PlayerObject* body) {
			if (!body) return false;
			return log == body->m_collisionLogTop || log == body->m_collisionLogBottom || log == body->m_collisionLogLeft || log == body->m_collisionLogRight;
		};
		if (shared(play->m_player1) || shared(play->m_player2)) return;
		log->removeAllObjects();
	};
	clearLog(fake->m_collisionLogTop);
	clearLog(fake->m_collisionLogBottom);
	clearLog(fake->m_collisionLogLeft);
	clearLog(fake->m_collisionLogRight);
	fake->m_playEffects = false;
	bool liveWasDead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
	fake->update(stepDelta);
	fake->m_unkUnused3 = fake->getRotation();
	fake->updateRotation(stepDelta);
	fake->m_shipRotation = fake->getPosition();
	// Solids/slopes resolve against the layer's player slots. Park the fake in the matching
	// slot so block and slope kills register on the prediction path.
	PlayerObject*& slot = (fake == fake2) ? play->m_player2 : play->m_player1;
	PlayerObject* savedSlot = slot;
	if (slot) slot = fake;
	collidingFake = fake;
	int hit = play->checkCollisions(fake, stepDelta, false);
	collidingFake = nullptr;
	if (slot) slot = savedSlot;
	// checkCollisions returns 1 after any destroyPlayer call, and the anticheat spike at the level
	// start calls it without killing (destroyPlayer ignores it). Real kills reach the predicting
	// destroyPlayer hook, which marks the fake dead, so the return value is not a death by itself.
	(void)hit;
	// Some kills only set m_isDead / destroy the live body without going through destroyPlayer.
	if (fake->m_isDead) markDead(fake);
	bool liveNowDead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
	if (!liveWasDead && liveNowDead) markDead(fake);
	phys::checkSpawnObjects(play, fake);
	// End of Silicate code.

	bool dead = (fake == fake1 ? deadP1 : deadP2) || fake->m_isDead;
	if (dead) {
		if (fake == fake1) deadP1 = true;
		else if (fake == fake2) deadP2 = true;
		// The fatal step itself is not drawn. Keep the marker on the last living pose.
		if (draw) {
			flushPath(play, fake, click);
			CCPoint saved = fake->getPosition();
			fake->setPosition(from);
			drawDeath(fake);
			fake->setPosition(saved);
		}
		return true;
	}
	if (draw) {
		auto& path = paths[fake == fake2];
		if (path.empty()) path.push_back(from);
		path.push_back(fake->getPosition());
	}
	return false;
}

void Trajectory::flushPath(PlayLayer* play, PlayerObject* fake, Click click) {
	auto& path = paths[fake == fake2];
	float width = lineWidth(play);
	auto color = colorFor(fake, click);
	echo::trajectory::drawPath(path, [&](CCPoint a, CCPoint b) {
		node->drawSegment(a, b, width, color);
	});
	path.clear();
}

void Trajectory::drawDeath(PlayerObject* fake) {
	auto* play = PlayLayer::get();
	if (!play) return;
	auto const& value = Logic::get();
	float width = lineWidth(play);
	auto color = [](float const* c) { return ccColor4F{c[0], c[1], c[2], c[3]}; };
	node->drawRect(fake->getObjectRect(), {0.f, 0.f, 0.f, 0.f}, width, color(value.hitboxOuterColor), BorderAlignment::Inside);
	node->drawRect(fake->getObjectRect(0.3f, 0.3f), {0.f, 0.f, 0.f, 0.f}, width, color(value.hitboxInnerColor), BorderAlignment::Inside);
}

void Trajectory::simulate(PlayLayer* play, bool player1, Click click, bool both, bool draw, int limit, int measure) {
	auto& value = Logic::get();
	constexpr int kMeasureLive = 1;
	constexpr int kMeasureHold = 2;
	constexpr int kMeasureRelease = 4;
	bool watch = measure != 0;
	PlayerObject* fake = player1 ? fake1 : fake2;
	PlayerObject* real = player1 ? play->m_player1 : play->m_player2;
	PlayerObject* partner = otherFake(fake);
	PlayerObject* partnerReal = player1 ? play->m_player2 : play->m_player1;
	if (!fake || !real) return;
	bool pair = both && play->m_gameState.m_isDualMode && partner && partnerReal;
	// Low-detail mode for the prediction: the spider jump makes its CCCircleWave unless it is on,
	// and it is visual only.
	auto* manager = GameManager::get();
	bool performance = manager && manager->m_performanceMode;
	if (manager) manager->m_performanceMode = true;

	// checkCollisions writes the layer's live players even when the body passed in is the fake.
	auto first = SavedPlayerCheckpoint::create(play->m_player1);
	auto second = play->m_player2 ? SavedPlayerCheckpoint::create(play->m_player2) : SavedPlayerCheckpoint();
	bool hasSecond = play->m_player2 != nullptr;
	bool died = play->m_playerDied;
	auto queued = play->m_queuedButtons;
	auto recorded = play->m_queuedRecordedButtons;
	int recordedSize = play->m_queuedRecordedButtonsSize;
	// Predicting from a corpse needs a living layer. The real death flag is restored below.
	play->m_playerDied = false;
	auto logsOf = [](PlayerObject* body) {
		return body ? std::array{body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight} : std::array<CCDictionary*, 4>{};
	};
	struct Trails {
		HardStreak* wave = nullptr;
		cocos2d::CCMotionStreak* regular = nullptr;
		GhostTrailEffect* ghost = nullptr;
	};
	auto trailsOf = [](PlayerObject* body) {
		if (!body) return Trails{};
		return Trails{body->m_waveTrail, body->m_regularTrail, body->m_ghostTrail};
	};
	auto logs1 = logsOf(play->m_player1);
	auto logs2 = logsOf(play->m_player2);
	auto trails1 = trailsOf(play->m_player1);
	auto trails2 = trailsOf(play->m_player2);
	auto* layer = play->m_objectLayer;
	CCPoint layerPos = layer ? layer->getPosition() : CCPoint{};
	float layerScaleX = layer ? layer->getScaleX() : 1.f;
	float layerScaleY = layer ? layer->getScaleY() : 1.f;
	float layerRotation = layer ? layer->getRotation() : 0.f;
	auto restorePlayers = [&] {
		auto putBody = [](PlayerObject* body, SavedPlayerCheckpoint& shot, std::array<CCDictionary*, 4> const& logs, Trails const& trails) {
			if (!body) return;
			shot.apply(body);
			body->m_collisionLogTop = logs[0];
			body->m_collisionLogBottom = logs[1];
			body->m_collisionLogLeft = logs[2];
			body->m_collisionLogRight = logs[3];
			body->m_waveTrail = trails.wave;
			body->m_regularTrail = trails.regular;
			body->m_ghostTrail = trails.ghost;
		};
		putBody(play->m_player1, first, logs1, trails1);
		if (hasSecond) putBody(play->m_player2, second, logs2, trails2);
		play->m_playerDied = died;
		play->m_queuedButtons = queued;
		play->m_queuedRecordedButtons = recorded;
		play->m_queuedRecordedButtonsSize = recordedSize;
		if (layer) {
			layer->setPosition(layerPos);
			layer->setScaleX(layerScaleX);
			layer->setScaleY(layerScaleY);
			layer->setRotation(layerRotation);
		}
	};

	GJGameState saved = play->m_gameState;
	Orbs::resetPredictionPads();
	seed(fake, real);
	if (pair) seed(partner, partnerReal);
	fake->m_controlsDisabled = false;
	if (pair && partner) partner->m_controlsDisabled = false;
	deadP1 = false;
	deadP2 = false;

	std::array<PlayerObject*, 2> bodies = {fake, pair ? partner : nullptr};
	auto eachBody = [&](auto&& action) {
		for (auto* body : bodies) {
			if (body) action(body);
		}
	};

	bool played = value.playsPlayer(!player1);
	bool clicker = !played && value.autoclicker && value.recordsPlayer(!player1) && (player1 ? value.autoclickP1 : value.autoclickP2);
	auto jumpHeld = [](PlayerObject* body) {
		if (!body) return false;
		int jump = static_cast<int>(PlayerButton::Jump);
		auto found = body->m_holdingButtons.find(jump);
		return found != body->m_holdingButtons.end() && found->second;
	};
	// Key/UI hold — Maintain Gravity can clear m_holdingButtons while the player is still holding.
	// Treat the physical hold as already-held so we do not turn a continuous hold into a fresh press
	// (that breaks death-next / Pause Before Kill, and with Click Dash Orbs can take orbs early).
	bool keyHeld = Logic::get().physicalHold(play, !player1);
	bool startedHeld = jumpHeld(real) || (value.maintainGravity && keyHeld && real->m_isDart);
	AutoClick clock = value.autoClick;
	if (clicker) {
		bool stashOrbs = Orbs::clickOrbAssist(!player1) || (pair && Orbs::clickOrbAssist(player1));
		if (stashOrbs) value.suppressRings = true;
		eachBody([&](PlayerObject* body) {
			if (clock.pressed) body->pushButton(PlayerButton::Jump);
			else body->releaseButton(PlayerButton::Jump);
		});
		if (stashOrbs) value.suppressRings = false;
	} else {
		eachBody([&](PlayerObject* body) {
			body->m_controlsDisabled = false;
			body->m_isLocked = false;
			bool alreadyHeld = jumpHeld(body);
			if (!alreadyHeld && value.maintainGravity && keyHeld && body->m_isDart && click == Click::Hold) {
				alreadyHeld = true;
			}
			// Holding through must keep the live buffer and hold. Clearing them and
			// pushButton again turns a continuous hold into a fresh press, which makes
			// death-next miss while the button is down — and with Click Dash Orbs + Maintain
			// Gravity that false fresh press takes the orb in the predict when live does not.
			if (click == Click::Hold && alreadyHeld) return;
			if (click != Click::Release && click != Click::Replay) body->m_jumpBuffered = false;
			press(play, body, click);
		});
	}

	float warp = play->m_gameState.m_timeWarp;
	float stepDelta = delta > 0.f ? delta : value.physicsStep() * 60.f * std::min(warp, 1.f);
	int steps = echo::trajectory::predictionSteps(value.trajectorySeconds, value.sampleRate(), warp);
	if (limit > 0) steps = std::min(steps, limit);
	// The level's moving objects follow the same steps (Hack/level_forecast.cpp). The move pipeline
	// takes the step in seconds (GJBaseGameLayer::update keeps both: the 60-based delta the players
	// get, and the seconds per step it hands prepareMoveActions and processMoveActionsStep).
	if (value.trajectoryMovingObjects) forecast::prepare(play, steps, stepDelta / 60.f);
	else forecast::invalidate();
	if (draw) {
		paths[fake == fake2].reserve(steps + 1);
		if (partner) paths[partner == fake2].reserve(steps + 1);
	}
	int frame = value.getFrame(play);
	size_t cursor = value.replayPos;
	// Playback re-sends the macro's hold each step when GD has dropped it (Logic::playbackInputs'
	// ensure). The prediction keeps the macro's hold per player and button the same way.
	constexpr int kHoldButtons[] = {static_cast<int>(PlayerButton::Jump), static_cast<int>(PlayerButton::Left), static_cast<int>(PlayerButton::Right)};
	std::array<std::array<bool, 3>, 2> macroHeld{};
	std::array<int, 2> lastInput = {-1, -1};
	if (value.isPlaying()) {
		for (int side = 0; side < 2; ++side) {
			if (!value.playsPlayer(side == 1)) continue;
			for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b) macroHeld[side][b] = Logic::get().heldAt(value, side == 1, frame, kHoldButtons[b]);
		}
		for (size_t i = value.inputs.size(); i-- > 0 && (lastInput[0] < 0 || lastInput[1] < 0);) {
			int& last = lastInput[value.inputs[i].player2 ? 1 : 0];
			if (last < 0) last = value.inputs[i].number;
		}
	}
	bool done1 = false;
	bool done2 = !pair;
	int liveSteps1 = 0;
	int liveSteps2 = 0;
	auto poses = Orbs::capture(play, real);
	if (pair) {
		auto extra = Orbs::capture(play, partnerReal);
		poses.insert(poses.end(), extra.begin(), extra.end());
	}
	auto restoreOrbs = [&] { Orbs::restore(poses); };
	// A dual portal ahead brings the second player in (dualPortal). In a two-player level it takes
	// no clicks from this prediction: it only moves.
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool ended1 = false;
	bool ended2 = false;
	pairing = pair;
	joinReal = partnerReal;
	joinable = player1 && !pair && partner == fake2 && partnerReal && !Logic::get().trajectoryPlayerHidden(true);
	joined = false;
	parted = false;
	auto portals = [&] {
		if (std::exchange(joined, false)) {
			pair = true;
			pairing = true;
			done2 = false;
			partner->m_controlsDisabled = false;
			partner->m_isLocked = false;
			if (!twoPlayer) bodies[1] = partner;
			if (draw) paths[partner == fake2].reserve(steps + 1);
		}
		if (std::exchange(parted, false)) {
			pairing = false;
			if (fake == fake2) {
				ended1 = !done1;
				done1 = true;
			} else if (pair) {
				ended2 = !done2;
				done2 = true;
				bodies[1] = nullptr;
			}
		}
	};
	std::array<std::unordered_set<int>, 2> orbClicks;
	struct SavedFlag { EnhancedGameObject* object; bool p1; bool p2; };
	std::vector<SavedFlag> flags;
	for (auto const& pose : poses) {
		auto* ring = static_cast<EnhancedGameObject*>(pose.object);
		if (ring) flags.push_back({ring, ring->m_activatedByPlayer1, ring->m_activatedByPlayer2});
	}
	auto snapRings = [](PlayerObject* player) {
		std::vector<CCObject*> rings;
		if (!player || !player->m_touchingRings) return rings;
		unsigned int count = player->m_touchingRings->count();
		rings.reserve(count);
		for (unsigned int i = 0; i < count; ++i) rings.push_back(player->m_touchingRings->objectAtIndex(i));
		return rings;
	};
	auto putRings = [](PlayerObject* player, std::vector<CCObject*> const& rings) {
		if (!player || !player->m_touchingRings) return;
		player->m_touchingRings->removeAllObjects();
		for (auto* ring : rings) {
			if (ring) player->m_touchingRings->addObject(ring);
		}
	};
	auto rings1 = snapRings(play->m_player1);
	auto rings2 = snapRings(play->m_player2);
	auto restoreLive = [&] {
		restoreOrbs();
		for (auto const& flag : flags) {
			if (!flag.object) continue;
			flag.object->m_activatedByPlayer1 = flag.p1;
			flag.object->m_activatedByPlayer2 = flag.p2;
		}
		putRings(play->m_player1, rings1);
		putRings(play->m_player2, rings2);
	};

	// The live prediction of player 1 also moves the objects locked to the player in the next forecast.
	std::vector<CCPoint> playerPath;
	bool notePath = (measure & kMeasureLive) && fake == fake1 && steps >= 30 && !died;
	if (notePath) {
		playerPath.reserve(static_cast<size_t>(steps) + 1);
		playerPath.push_back(fake->getPosition());
	}
	for (int i = 0; i < steps && !(done1 && done2); ++i) {
		{
			// Moving objects only matter near the predicted players.
			constexpr float kReach = 360.f;
			auto at = fake->getPosition();
			float left = at.x, right = at.x, bottom = at.y, top = at.y;
			if (pair && partner) {
				auto other = partner->getPosition();
				left = std::min(left, other.x);
				right = std::max(right, other.x);
				bottom = std::min(bottom, other.y);
				top = std::max(top, other.y);
			}
			if (value.trajectoryMovingObjects) forecast::apply(play, i + 1, {left - kReach, bottom - kReach, right - left + 2.f * kReach, top - bottom + 2.f * kReach});
		}
		std::array<bool, 2> orbPress{};
		// stepFrame has already counted the step that just ran. The next step plays inputs one past it.
		if (played || (pair && value.isPlaying())) {
			int at = frame + i + 1;
			bool shared = pair && value.recordsPlayer(false) == value.recordsPlayer(true) && (!value.isPlaying() || value.playP1 == value.playP2);
			auto send = [&](PlayerObject* target, bool player2, PlayerButton button, bool holding) {
				auto apply = [&](PlayerObject* body, bool icon2) {
					if (!body) return;
					if (holding) {
						body->pushButton(button);
						if (button == PlayerButton::Jump) orbPress[icon2 ? 1 : 0] = true;
					} else body->releaseButton(button);
				};
				apply(target, player2);
				if (shared) apply(otherFake(target), !player2);
			};
			std::array<std::array<bool, 3>, 2> sent{};
			while (cursor < value.inputs.size() && value.inputs[cursor].number <= at) {
				auto const& input = value.inputs[cursor++];
				int raw = input.button == 0 ? 1 : input.button;
				send(input.player2 ? fake2 : fake1, input.player2, static_cast<PlayerButton>(raw), input.holding);
				int side = input.player2 ? 1 : 0;
				for (int b = 0; b < 3; ++b) {
					if (kHoldButtons[b] != raw) continue;
					macroHeld[side][b] = input.holding;
					sent[side][b] = true;
				}
			}
			// Player 2 is re-sent only in a two-player level; a normal dual follows player 1.
			bool twoPlayerLevel = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
			for (int side = 0; side < (twoPlayerLevel ? 2 : 1); ++side) {
				if (!value.playsPlayer(side == 1)) continue;
				if (!value.ignoreInputs && at > lastInput[side]) continue;
				PlayerObject* body = side == 1 ? fake2 : fake1;
				if (body != fake && !(pair && body == partner)) continue;
				for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b) {
					if (sent[side][b]) continue;
					if (b > 0 && body->m_controlsDisabled) continue;
					if (Logic::buttonHeld(body, kHoldButtons[b]) == macroHeld[side][b]) continue;
					send(body, side == 1, static_cast<PlayerButton>(kHoldButtons[b]), macroHeld[side][b]);
				}
			}
		}

		bool pressedNow = i == 0 && (click == Click::Swift || (click == Click::Hold && !startedHeld));
		if (clicker) {
			if (clock.pressed) ++clock.sincePress;
			else ++clock.sinceRelease;
			bool flip = clock.pressed ? clock.sincePress >= clock.pressInterval : clock.sinceRelease >= clock.releaseInterval;
			if (flip) {
				clock.pressed = !clock.pressed;
				clock.sincePress = 0;
				clock.sinceRelease = 0;
				pressedNow = clock.pressed;
				bool stashOrbs = Orbs::clickOrbAssist(!player1) || (pair && Orbs::clickOrbAssist(player1));
				if (stashOrbs) value.suppressRings = true;
				eachBody([&](PlayerObject* body) {
					if (clock.pressed) body->pushButton(PlayerButton::Jump);
					else body->releaseButton(PlayerButton::Jump);
				});
				if (stashOrbs) value.suppressRings = false;
				if (clock.pressed && clock.pressInterval <= 0) {
					clock.pressed = false;
					pressedNow = false;
					eachBody([&](PlayerObject* body) {
						body->releaseButton(PlayerButton::Jump);
					});
				}
			}
		}

		// Maintain Gravity only corrects the hold. Both icons click before either orb flips the other,
		// and the flip is corrected in this step. Doing it on the next step puts one wave a frame late.
		auto maintain = [&](PlayerObject* body) {
			bool icon2 = body == fake2;
			if (!body->m_isDart || value.playsPlayer(icon2) || !value.maintainGravity || !value.recordsPlayer(icon2)) return;
			bool physical = clicker ? clock.pressed : click == Click::Hold;
			bool stashOrbs = Orbs::clickOrbAssist(icon2);
			if (stashOrbs) value.suppressRings = true;
			for (int step = 0; step < 8 && !body->m_controlsDisabled; ++step) {
				if (body->m_isUpsideDown != (physical == body->m_jumpBuffered)) break;
				bool beforeFlip = body->m_isUpsideDown;
				bool beforeBuffer = body->m_jumpBuffered;
				if (physical != body->m_isUpsideDown) {
					body->pushButton(PlayerButton::Jump);
				} else body->releaseButton(PlayerButton::Jump);
				if (body->m_isUpsideDown == beforeFlip && body->m_jumpBuffered == beforeBuffer) break;
			}
			if (stashOrbs) value.suppressRings = false;
		};
		eachBody(maintain);
		eachBody([&](PlayerObject* body) {
			bool icon2 = body == fake2;
			// Mirror live claimClick: only a fresh press with Click Dash/Black Orbs on.
			bool fresh = orbPress[icon2 ? 1 : 0] || (!value.playsPlayer(icon2) && pressedNow);
			Orbs::clickFake(play, body, icon2, orbClicks[icon2 ? 1 : 0], fresh);
			// Mirror live pad flush: only after a jump/gravity pad was hit this prediction.
			Orbs::applyPredictionPads(play, body, icon2);
		});
		eachBody(maintain);

		// Taken directly from Silicate (GPL-3.0): the game state GD advances once per physics step.
		auto& state = play->m_gameState;
		float dt = value.physicsStep();
		state.m_totalTime += dt;
		state.m_unkDouble3 += dt / state.m_timeWarp;
		state.m_currentProgress++;
		state.m_unkUint5 += static_cast<int>(roundf(state.m_timeWarp * 1000.0f));
		float timeMod = *reinterpret_cast<float*>(&state.m_timeModRelated);
		if (timeMod != 0.0f) {
			state.m_timeModRelated = 0;
			state.m_timeModRelated2 = 0;
			eachBody([&](PlayerObject* body) { body->updateTimeMod(timeMod, true); });
		}
		// End of Silicate code.

		// GJBaseGameLayer::update, each step after the queued buttons: an orb the player did not
		// touch on the step before is no longer one it is touching.
		if (!done1) fake->resetTouchedRings(false);
		if (!done2 && partner) partner->resetTouchedRings(false);
		bool was1 = done1;
		bool was2 = done2;
		if (!done1) done1 = advance(play, fake, stepDelta, draw, click);
		if (notePath && !done1) playerPath.push_back(fake->getPosition());
		portals();
		if (!done2) done2 = advance(play, partner, stepDelta, draw, click);
		portals();
		restoreOrbs();
		if ((measure & kMeasureLive) && i == 0 && !died) {
			if (player1) {
				if (!was1 && done1 && !ended1) value.deathNextP1 = true;
				if (pair && !was2 && done2 && !ended2) value.deathNextP2 = true;
			} else if (!was1 && done1 && !ended1) {
				value.deathNextP2 = true;
			}
		}
		if (watch) {
			if (!was1 && !done1) ++liveSteps1;
			if (pair && !was2 && !done2) ++liveSteps2;
		}
	}

	if (watch) {
		float scale = std::clamp(warp, 0.01f, 1.f) / static_cast<float>(value.sampleRate());
		auto store = [&](float& alive1, bool& dies1, float& alive2, bool& dies2) {
			if (player1) {
				alive1 = static_cast<float>(liveSteps1) * scale;
				dies1 = done1 && !ended1;
				if (pair) {
					alive2 = static_cast<float>(liveSteps2) * scale;
					dies2 = done2 && !ended2;
				}
			} else {
				alive2 = static_cast<float>(liveSteps1) * scale;
				dies2 = done1 && !ended1;
			}
		};
		if (measure & kMeasureLive) store(value.trajAliveP1, value.trajDiesP1, value.trajAliveP2, value.trajDiesP2);
		if (measure & kMeasureHold) {
			if (player1) {
				value.trajHoldAliveP1 = static_cast<float>(liveSteps1) * scale;
				if (pair) value.trajHoldAliveP2 = static_cast<float>(liveSteps2) * scale;
			} else {
				value.trajHoldAliveP2 = static_cast<float>(liveSteps1) * scale;
			}
		}
		if (measure & kMeasureRelease) {
			if (player1) {
				value.trajReleaseAliveP1 = static_cast<float>(liveSteps1) * scale;
				if (pair) value.trajReleaseAliveP2 = static_cast<float>(liveSteps2) * scale;
			} else {
				value.trajReleaseAliveP2 = static_cast<float>(liveSteps1) * scale;
			}
		}
	}

	if (notePath) forecast::notePlayerPath(std::move(playerPath), real->m_lastPosition);
	forecast::rewind(play);
	pairing = false;
	joinable = false;
	joinReal = nullptr;
	if (manager) manager->m_performanceMode = performance;
	if (draw) {
		flushPath(play, fake, click);
		if (partner) flushPath(play, partner, click);
	}
	restoreLive();
	park(fake);
	if (partner) park(partner);
	usedP1.clear();
	usedP2.clear();
	play->m_gameState = saved;
	restorePlayers();
}

void Trajectory::refresh(PlayLayer* play) {
	if (!play || !play->m_started || !play->m_player1 || !node || !fake1 || !fake2) return;
	auto& value = Logic::get();
	// Once the level is complete the end animation flies the player into the end screen. Predicting
	// there (seeding fakes, restoring the live players each frame) kept the player from doing that.
	if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
		if (node->isVisible()) node->setVisible(false);
		park(fake1);
		park(fake2);
		if (cached || value.deathNextP1 || value.deathNextP2) {
			cached = false;
			value.deathNextP1 = false;
			value.deathNextP2 = false;
			Logic::get().syncDeathLabel(play);
		}
		return;
	}
	bool rendering = Logic::get().renderCapturing();
	bool draw = value.showTrajectory;
	// Pause At Trajectory Length is temporarily disabled — never pay for a hidden predict.
	bool needPredict = draw;
	if (!needPredict) {
		if (node->isVisible()) node->setVisible(false);
		// Without a drawn trajectory, Death Next Frame and Pause Before Kill still need to know
		// whether the next step kills: a one-step lookahead says so.
		bool pauseKill = value.pauseBeforeKill && value.isRecording();
		if (!rendering && (value.showDeathNext || pauseKill)) {
			lookahead(play);
			Logic::get().checkAdvancePause(play);
			Logic::get().syncDeathLabel(play);
			return;
		}
		if (cached || value.deathNextP1 || value.deathNextP2 || value.trajDiesP1 || value.trajDiesP2) {
			cached = false;
			value.deathNextP1 = false;
			value.deathNextP2 = false;
			value.trajAliveP1 = 0.f;
			value.trajAliveP2 = 0.f;
			value.trajDiesP1 = false;
			value.trajDiesP2 = false;
			value.trajHoldAliveP1 = 0.f;
			value.trajHoldAliveP2 = 0.f;
			value.trajReleaseAliveP1 = 0.f;
			value.trajReleaseAliveP2 = 0.f;
			Logic::get().syncDeathLabel(play);
		}
		return;
	}
	Logic::get().seatOverlay(node, play);
	if (play->m_objectLayer) {
		if (!fake1->getParent()) play->m_objectLayer->addChild(fake1);
		if (!fake2->getParent()) play->m_objectLayer->addChild(fake2);
	}
	if (rendering && !value.showTrajectory) {
		node->setVisible(false);
		park(fake1);
		park(fake2);
		Logic::get().deathNextP1 = false;
		Logic::get().deathNextP2 = false;
		value.trajAliveP1 = 0.f;
		value.trajAliveP2 = 0.f;
		value.trajDiesP1 = false;
		value.trajDiesP2 = false;
		value.trajHoldAliveP1 = 0.f;
		value.trajHoldAliveP2 = 0.f;
		value.trajReleaseAliveP1 = 0.f;
		value.trajReleaseAliveP2 = 0.f;
		Logic::get().syncDeathLabel(play);
		return;
	}
	park(fake1);
	park(fake2);

	node->setVisible(draw);

	float savedSeconds = value.trajectorySeconds;
	int stepLimit = 0;

	Key key = makeKey(play);
	key.hold = draw && value.trajectoryHold;
	key.swift = draw && value.trajectorySwift;
	key.release = draw && value.trajectoryRelease;
	if (cached && key == lastKey) {
		value.trajectorySeconds = savedSeconds;
		Logic::get().checkAdvancePause(play);
		return;
	}

	busy = true;
	phys::preparePredictionArt();
	value.predicting = true;
	Logic::get().syncPredictHooks();
	node->clear();
	auto snap = [](PlayerObject* body) {
		return body ? std::pair{body->m_jumpBuffered, body->m_holdingButtons} : std::pair{false, gd::map<int, bool>{}};
	};
	auto p1Hold = snap(play->m_player1);
	auto p2Hold = snap(play->m_player2);
	bool ui1 = false;
	bool ui2 = false;
	int touch1 = -1;
	int touch2 = -1;
	if (auto* ui = play->m_uiLayer) {
		ui1 = ui->m_p1Jumping;
		ui2 = ui->m_p2Jumping;
		touch1 = ui->m_p1TouchId;
		touch2 = ui->m_p2TouchId;
	}
	auto restore = [&] {
		auto put = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
			if (!body) return;
			body->m_jumpBuffered = hold.first;
			body->m_holdingButtons = hold.second;
		};
		put(play->m_player1, p1Hold);
		put(play->m_player2, p2Hold);
		if (auto* ui = play->m_uiLayer) {
			ui->m_p1Jumping = ui1;
			ui->m_p2Jumping = ui2;
			ui->m_p1TouchId = touch1;
			ui->m_p2TouchId = touch2;
		}
	};

	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode && play->m_player2;
	auto live = [&](bool player2) {
		if (value.playsPlayer(player2)) return false;
		return true;
	};
	value.deathNextP1 = false;
	value.deathNextP2 = false;
	value.trajAliveP1 = 0.f;
	value.trajAliveP2 = 0.f;
	value.trajDiesP1 = false;
	value.trajDiesP2 = false;
	value.trajHoldAliveP1 = 0.f;
	value.trajHoldAliveP2 = 0.f;
	value.trajReleaseAliveP1 = 0.f;
	value.trajReleaseAliveP2 = 0.f;
	auto jumpHeld = [](PlayerObject* body) {
		if (!body) return false;
		int jump = static_cast<int>(PlayerButton::Jump);
		auto found = body->m_holdingButtons.find(jump);
		return found != body->m_holdingButtons.end() && found->second;
	};
	auto actual = [&](bool player1) {
		if (value.playsPlayer(!player1)) return Click::Replay;
		// Maintain Gravity steers by tapping jump; m_holdingButtons is not the real hold.
		PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
		if (value.maintainGravity && value.recordsPlayer(!player1) && body && body->m_isDart) {
			return Logic::get().physicalHold(play, !player1) ? Click::Hold : Click::Release;
		}
		return jumpHeld(body) ? Click::Hold : Click::Release;
	};
	constexpr int kMeasureLive = 1;
	constexpr int kMeasureHold = 2;
	constexpr int kMeasureRelease = 4;
	auto run = [&](bool player1, Click click, bool both, bool show, int measure) {
		simulate(play, player1, click, both, show, stepLimit, measure);
	};
	auto modes = [&](bool player1, bool both) {
		auto act = actual(player1);
		auto fire = [&](Click click, bool show) {
			int measure = 0;
			if (click == act) measure |= kMeasureLive;
			if (value.pauseAtTrajectoryLength && click == Click::Hold) measure |= kMeasureHold;
			if (value.pauseAtTrajectoryLength && click == Click::Release) measure |= kMeasureRelease;
			if (!show && measure == 0) return;
			run(player1, click, both, show, measure);
		};
		if (play->m_playerDied || rendering) {
			fire(act, draw);
			return;
		}
		bool showHold = draw && value.trajectoryHold;
		bool showSwift = draw && value.trajectorySwift;
		bool showRelease = draw && value.trajectoryRelease;
		bool needHold = showHold || value.pauseAtTrajectoryLength;
		bool needRelease = showRelease || value.pauseAtTrajectoryLength;
		if (needHold) fire(Click::Hold, showHold);
		if (showSwift) fire(Click::Swift, true);
		if (needRelease) fire(Click::Release, showRelease);
		bool covered = (act == Click::Hold && needHold) || (act == Click::Swift && showSwift) || (act == Click::Release && needRelease);
		if (!covered) fire(act, draw);
	};
	bool hide1 = Logic::get().trajectoryPlayerHidden(false);
	bool hide2 = Logic::get().trajectoryPlayerHidden(true);
	// A normal dual uses one click for both icons. The top and bottom paths have to be simulated together.
	bool linked = dual && !twoPlayer && !hide1 && !hide2;
	if (!hide1) {
		if (!rendering && !play->m_playerDied && value.playsPlayer(false)) run(true, Click::Replay, linked, draw, kMeasureLive);
		if (live(false) || rendering || play->m_playerDied) modes(true, linked);
	}
	bool second = dual && !hide2 && !linked && (twoPlayer || live(true) || value.playsPlayer(true) || play->m_playerDied);
	if (second) {
		if (!rendering && !play->m_playerDied && value.playsPlayer(true)) run(false, Click::Replay, false, draw, kMeasureLive);
		if (live(true) || rendering || play->m_playerDied) modes(false, false);
	}
	restore();
	Logic::get().syncDeathLabel(play);
	Logic::get().checkAdvancePause(play);

	busy = false;
	value.predicting = false;
	Logic::get().syncPredictHooks();
	cached = true;
	lastKey = key;
	value.trajectorySeconds = savedSeconds;
}

void Trajectory::lookahead(PlayLayer* play) {
	auto& value = Logic::get();
	value.deathNextP1 = false;
	value.deathNextP2 = false;
	if (!play || !play->m_started || play->m_playerDied || !play->m_player1 || !node || !fake1 || !fake2) return;
	if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) return;
	// Same setup as refresh(): the fakes live in the object layer while they predict.
	if (play->m_objectLayer) {
		if (!fake1->getParent()) play->m_objectLayer->addChild(fake1);
		if (!fake2->getParent()) play->m_objectLayer->addChild(fake2);
	}
	park(fake1);
	park(fake2);
	busy = true;
	phys::preparePredictionArt();
	value.predicting = true;
	Logic::get().syncPredictHooks();
	auto snap = [](PlayerObject* body) {
		return body ? std::pair{body->m_jumpBuffered, body->m_holdingButtons} : std::pair{false, gd::map<int, bool>{}};
	};
	auto p1Hold = snap(play->m_player1);
	auto p2Hold = snap(play->m_player2);
	auto jumpHeld = [](PlayerObject* body) {
		if (!body) return false;
		auto found = body->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
		return found != body->m_holdingButtons.end() && found->second;
	};
	// The click the live player is giving right now, as refresh() measures it. A press or release
	// made this frame is still in the game's queue (it is applied inside the next step), so the
	// newest queued jump for that player is what the next step will see.
	auto actual = [&](bool player1) {
		PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
		if (value.maintainGravity && value.recordsPlayer(!player1) && body && body->m_isDart)
			return Logic::get().physicalHold(play, !player1) ? Click::Hold : Click::Release;
		int jump = static_cast<int>(PlayerButton::Jump);
		for (auto it = play->m_queuedButtons.rbegin(); it != play->m_queuedButtons.rend(); ++it) {
			int button = static_cast<int>(it->m_button);
			if ((button == 0 || button == jump) && it->m_isPlayer2 != player1) return it->m_isPush ? Click::Hold : Click::Release;
		}
		return jumpHeld(body) ? Click::Hold : Click::Release;
	};
	// While recording, the recorded players; otherwise every player the macro is not playing.
	auto watched = [&](bool player2) {
		return value.isRecording() ? value.recordsPlayer(player2) : !value.playsPlayer(player2);
	};
	constexpr int kMeasureLive = 1;
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode && play->m_player2;
	bool linked = dual && !twoPlayer;
	if (watched(false) && !value.playsPlayer(false)) simulate(play, true, actual(true), linked, false, 1, kMeasureLive);
	if (dual && !linked && watched(true) && !value.playsPlayer(true)) simulate(play, false, actual(false), false, false, 1, kMeasureLive);
	auto put = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
		if (!body) return;
		body->m_jumpBuffered = hold.first;
		body->m_holdingButtons = hold.second;
	};
	put(play->m_player1, p1Hold);
	put(play->m_player2, p2Hold);
	busy = false;
	value.predicting = false;
	Logic::get().syncPredictHooks();
	// Only deathNext matters here; a one-step alive length must not feed the step budget.
	value.trajAliveP1 = value.trajAliveP2 = 0.f;
	value.trajDiesP1 = value.trajDiesP2 = false;
	// The drawn trajectory, if any, predicted from an older state.
	cached = false;
}

bool Logic::isTrajectoryPlayer(cocos2d::CCObject* object) {
	auto const& trajectory = Trajectory::get();
	if (!object) return false;
	return (trajectory.fake1 && object == static_cast<cocos2d::CCObject*>(trajectory.fake1)) ||
		(trajectory.fake2 && object == static_cast<cocos2d::CCObject*>(trajectory.fake2));
}

void Logic::resetTrajectory() {
	Trajectory::get().invalidate();
}
