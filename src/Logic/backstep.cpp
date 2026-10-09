#include "Logic/logic.hpp"
#include "GUI/gui.hpp"
#include "Physics/checkpoint.hpp"
#include "Logic/state_check.hpp"
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/CheckpointObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
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

using namespace geode::prelude;

// The native checkpoint is made for a practice respawn: it keeps the players' movement but not their
// held buttons, jump buffer and collision state, and Echo's respawn then presses held buttons again.
// A backstep has to continue as if nothing happened, so like Silicate it also keeps the whole player
// state and the effect manager's persistent items, and puts them back after the load.
// Whether each orb, pad and portal near the player had been used (GD lets each act once). GD's
// checkpoint did not bring these back: a dash orb or speed portal used just before a save came back
// unused after loading it, and the replay went another way (Backstep check, Carmine Bullet).
struct UsedMark {
	EnhancedGameObject* object;
	bool p1, p2;
};

std::vector<UsedMark> usedMarks(PlayLayer* play) {
	std::vector<UsedMark> out;
	auto* p1 = play ? play->m_player1 : nullptr;
	if (!p1) return out;
	auto& grid = play->m_nonEffectObjects;
	auto& sizes = play->m_nonEffectObjectsSizes;
	float factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
	int center = static_cast<int>(std::floor(p1->getPositionX() * factor));
	int last = static_cast<int>(std::min(grid.size(), sizes.size())) - 1;
	for (int x = std::max(center - 10, 0); x <= std::min(center + 10, last); ++x) {
		auto* column = grid[x];
		auto* counts = sizes[x];
		if (!column || !counts) continue;
		for (std::size_t y = 0; y < std::min(column->size(), counts->size()); ++y) {
			auto* bucket = (*column)[y];
			if (!bucket) continue;
			int count = std::min<int>((*counts)[y], static_cast<int>(bucket->size()));
			for (int i = 0; i < count; ++i) {
				auto* object = (*bucket)[i];
				if (!object) continue;
				switch (object->m_objectType) {
					case GameObjectType::Solid: case GameObjectType::Hazard: case GameObjectType::AnimatedHazard: case GameObjectType::Slope:
					case GameObjectType::Decoration: continue;
					default: break;
				}
				if (auto* enhanced = typeinfo_cast<EnhancedGameObject*>(object))
					out.push_back({enhanced, enhanced->m_activatedByPlayer1, enhanced->m_activatedByPlayer2});
			}
		}
	}
	return out;
}

namespace {
// GD's camera between steps. GD moves the camera once per physics step (updateCamera runs inside
// the step loop of GJBaseGameLayer::update), easing it toward the player, so its state is more
// than the position: the follow targets and eases, the static camera and edge state, the shake,
// and the height offsets on the layer that pan the camera up and down with the player. Loading a
// save goes through the practice respawn, which re-centres the camera (resetCamera); putting back
// the position alone left the rest where the respawn put it, and the next steps eased the camera
// from there: it jumped up and down wherever the camera follows the player vertically. Each save
// keeps all of it, taken at the start of a step like the save itself.
struct CameraState {
	// The camera fields of GJGameState that GD's camera update and the respawn's camera reset write
	// (read from both in GeometryDash.exe 2.2081); all plain values, no containers.
	struct Range {
		std::size_t begin, end;
	};
	static constexpr std::array kRanges{
		Range{offsetof(GJGameState, m_cameraZoom), offsetof(GJGameState, m_spawnChannelRelated0)},
		Range{offsetof(GJGameState, m_cameraPosition), offsetof(GJGameState, m_tweenActions)},
		Range{offsetof(GJGameState, m_cameraEdgeValue0), offsetof(GJGameState, m_gameObjectPhysics)},
		Range{offsetof(GJGameState, m_unkUint10), offsetof(GJGameState, m_activatedObjectIDs)},
		Range{offsetof(GJGameState, m_unkUint13), offsetof(GJGameState, m_stateObjects)},
		// The static camera's instant-exit flags (m_instantExitStaticCameraX/Y) and the float after them.
		Range{offsetof(GJGameState, m_dynamicRotateActions) + sizeof(GJGameState::m_dynamicRotateActions), offsetof(GJGameState, m_unkUMap8)},
	};
#ifdef GEODE_IS_WINDOWS
	// The offsets read from GD's code; a binding change that moves these needs the ranges checked again.
	static_assert(offsetof(GJGameState, m_cameraPosition) == 0x268 && offsetof(GJGameState, m_cameraStepDiff) == 0x334 &&
		offsetof(GJGameState, m_unkUint13) == 0x358 && offsetof(GJGameState, m_unkUint17) == 0x5bc);
#endif
	static constexpr std::size_t kSize = [] {
		std::size_t total = 0;
		for (auto const& range : kRanges) total += range.end - range.begin;
		return total;
	}();
	std::array<std::byte, kSize> state{};
	float flip = 0.f, widthOffset = 0.f, heightOffset = 0.f, unzoomedHeightOffset = 0.f, targetHeightOffset = 0.f;
	bool calculateTargetHeightOffset = false;
	bool present = false;

	static CameraState of(PlayLayer* play) {
		CameraState out;
		auto const* base = reinterpret_cast<std::byte const*>(&play->m_gameState);
		std::size_t at = 0;
		for (auto const& range : kRanges) {
			std::memcpy(out.state.data() + at, base + range.begin, range.end - range.begin);
			at += range.end - range.begin;
		}
		out.flip = play->m_cameraFlip;
		out.widthOffset = play->m_cameraWidthOffset;
		out.heightOffset = play->m_cameraHeightOffset;
		out.unzoomedHeightOffset = play->m_cameraUnzoomedHeightOffset;
		out.targetHeightOffset = play->m_targetCameraHeightOffset;
		out.calculateTargetHeightOffset = play->m_calculateTargetHeightOffset;
		out.present = true;
		return out;
	}

	void apply(PlayLayer* play) const {
		if (!present) return;
		auto* base = reinterpret_cast<std::byte*>(&play->m_gameState);
		std::size_t at = 0;
		for (auto const& range : kRanges) {
			std::memcpy(base + range.begin, state.data() + at, range.end - range.begin);
			at += range.end - range.begin;
		}
		play->m_cameraFlip = flip;
		play->m_cameraWidthOffset = widthOffset;
		play->m_cameraHeightOffset = heightOffset;
		play->m_cameraUnzoomedHeightOffset = unzoomedHeightOffset;
		play->m_targetCameraHeightOffset = targetHeightOffset;
		play->m_calculateTargetHeightOffset = calculateTargetHeightOffset;
	}
};

// The camera a step back put on screen, with the object layer as it was drawn. Frames that step
// nothing put it back, so nothing moves it until a step runs.
struct CameraPin {
	CameraState state;
	cocos2d::CCPoint layerPosition;
	float layerScaleX = 1.f, layerScaleY = 1.f, layerRotation = 0.f;
};
std::optional<CameraPin>& pinnedCamera() {
	static std::optional<CameraPin> pin;
	return pin;
}
// Set when a step back pinned the camera during this frame's steps (its replay ran them).
bool pinnedThisFrame = false;

// Puts the camera state back and brings the object layer, background and ground to it. The update
// runs with dt 0 for the art, then the state is put back again in case it moved anything.
void showCamera(PlayLayer* play, CameraState const& state) {
	state.apply(play);
	play->updateCamera(0.f);
	state.apply(play);
}

void putPin(PlayLayer* play, CameraPin const& pin) {
	showCamera(play, pin.state);
	if (auto* layer = play->m_objectLayer) {
		layer->setPosition(pin.layerPosition);
		layer->setScaleX(pin.layerScaleX);
		layer->setScaleY(pin.layerScaleY);
		layer->setRotation(pin.layerRotation);
	}
}

void pinCamera(PlayLayer* play) {
	CameraPin pin;
	pin.state = CameraState::of(play);
	if (auto* layer = play->m_objectLayer) {
		pin.layerPosition = layer->getPosition();
		pin.layerScaleX = layer->getScaleX();
		pin.layerScaleY = layer->getScaleY();
		pin.layerRotation = layer->getRotation();
	}
	pinnedCamera() = pin;
	pinnedThisFrame = true;
}
}

// After each drawn frame: a frame that stepped nothing keeps a step back's camera.
void Logic::noteShownCamera(PlayLayer* play, bool stepped) {
	if (!play || !Logic::get().backstep) return;
	if (std::exchange(pinnedThisFrame, false)) {
		if (auto& pin = pinnedCamera()) putPin(play, *pin);
		return;
	}
	if (stepped) {
		pinnedCamera().reset();
		return;
	}
	if (auto& pin = pinnedCamera()) putPin(play, *pin);
}

struct BackstepExtra {
	std::vector<UsedMark> used;
	SavedPlayerCheckpoint player1;
	SavedPlayerCheckpoint player2;
	bool hasPlayer2 = false;
	gd::unordered_map<int, int> persistentItems;
	gd::vector<GameObject*> calcNonEffect;
	int calcNonEffectSize = 0;
	CameraState camera;
};

// Backstep, after Silicate's backwards stepping: the game's own checkpoint saves the whole level, and
// stepping back loads one through the practice respawn path. Silicate saves one every step, which at
// high TPS costs more than the step. Echo saves one every backstepInterval() steps (every step at
// 240 TPS) and replays the recorded inputs from the save to the frame stepped back to.

// The players' state at every step, by frame. A backstep's replay passing a frame again must arrive
// at the same state: the first difference is logged, and the positions are put back (compare).
namespace {
// A player's button and orb state at the start of a step: whether a jump is buffered, which rings it
// touches and has jumped from, which buttons it holds. GD's checkpoint keeps none of it, and the
// hold restore only sets it when the hold changes, so after a load it was whatever the run before
// had left: an orb under the player was taken on the first step in one run and not the next
// (Backstep check, Carmine Bullet).
struct Buttons {
	bool jumpBuffered = false, stateRingJump = false, wasJumpBuffered = false, wasRobotJump = false;
	unsigned char stateJumpBuffered = 0;
	bool stateRingJump2 = false, touchedRing = false, touchedCustomRing = false, touchedGravityPortal = false, touchedPad = false;
	bool ringJumpRelated = false, holdingLeft = false, holdingRight = false;
	// Carried from step to step and not in GD's checkpoint either: the first replayed step after a load
	// started from whatever the run before left (Backstep check: hidden drift one frame after every
	// load, then an orb taken in one run and not the next).
	double fallSpeed = 0;
	int reverseRelated = 0;
	geode::SeedValueRSV jumpRelated{};
	// Compact copies: the frame ring holds 131072 of these, and GD's map and set allocate even empty.
	std::array<std::int8_t, 4> holding{-1, -1, -1, -1}; // Buttons 0-3: -1 absent, else held.
	std::vector<int> touchedRings, ringRelated;
	bool present = false;

	static Buttons of(PlayerObject* body) {
		Buttons out;
		if (!body) return out;
		out.present = true;
		out.jumpBuffered = body->m_jumpBuffered;
		out.stateRingJump = body->m_stateRingJump;
		out.wasJumpBuffered = body->m_wasJumpBuffered;
		out.wasRobotJump = body->m_wasRobotJump;
		out.stateJumpBuffered = body->m_stateJumpBuffered;
		out.stateRingJump2 = body->m_stateRingJump2;
		out.touchedRing = body->m_touchedRing;
		out.touchedCustomRing = body->m_touchedCustomRing;
		out.touchedGravityPortal = body->m_touchedGravityPortal;
		out.touchedPad = body->m_touchedPad;
		out.ringJumpRelated = body->m_ringJumpRelated;
		out.holdingLeft = body->m_holdingLeft;
		out.holdingRight = body->m_holdingRight;
		out.fallSpeed = body->m_fallSpeed;
		out.reverseRelated = body->m_reverseRelated;
		out.jumpRelated = body->m_jumpRelatedAC2;
		for (auto const& [button, held] : body->m_holdingButtons)
			if (button >= 0 && button < 4) out.holding[static_cast<std::size_t>(button)] = held ? 1 : 0;
		out.touchedRings.assign(body->m_touchedRings.begin(), body->m_touchedRings.end());
		out.ringRelated.assign(body->m_ringRelatedSet.begin(), body->m_ringRelatedSet.end());
		return out;
	}

	void apply(PlayerObject* body) const {
		if (!body || !present) return;
		body->m_jumpBuffered = jumpBuffered;
		body->m_stateRingJump = stateRingJump;
		body->m_wasJumpBuffered = wasJumpBuffered;
		body->m_wasRobotJump = wasRobotJump;
		body->m_stateJumpBuffered = stateJumpBuffered;
		body->m_stateRingJump2 = stateRingJump2;
		body->m_touchedRing = touchedRing;
		body->m_touchedCustomRing = touchedCustomRing;
		body->m_touchedGravityPortal = touchedGravityPortal;
		body->m_touchedPad = touchedPad;
		body->m_ringJumpRelated = ringJumpRelated;
		body->m_holdingLeft = holdingLeft;
		body->m_holdingRight = holdingRight;
		body->m_fallSpeed = fallSpeed;
		body->m_reverseRelated = reverseRelated;
		body->m_jumpRelatedAC2 = jumpRelated;
		body->m_holdingButtons.clear();
		for (int button = 0; button < 4; ++button)
			if (holding[static_cast<std::size_t>(button)] >= 0) body->m_holdingButtons[button] = holding[static_cast<std::size_t>(button)] != 0;
		body->m_touchedRings.clear();
		body->m_touchedRings.insert(touchedRings.begin(), touchedRings.end());
		body->m_ringRelatedSet.clear();
		body->m_ringRelatedSet.insert(ringRelated.begin(), ringRelated.end());
	}
};

struct Probe {
	double x = 0, y = 0, yVelocity = 0, levelTime = 0, rotation = 0, nodeX = 0, nodeY = 0, lastX = 0, lastY = 0;
	cocos2d::CCPoint position1, position2, node1, node2;
	// Each player's own clock (PlayerObject::m_totalTime). A load left it a step ahead (the respawn's
	// steps advance it without counting a frame), and GD times jump buffers and orbs against it.
	double time1 = 0, time2 = 0;
	Buttons buttons1, buttons2;
	// The presses and releases the step was about to run (GD's queue at its start).
	std::uint64_t inputs = 0;
	unsigned attempt = 0; // Records from another attempt (or level) are never followed.
	bool hasPlayer2 = false;
	unsigned progress = 0;
};
// Every step's probe in a ring indexed by frame.
constexpr size_t kRing = 1 << 17; // Past the 240 saves Backstep keeps at any TPS.
std::vector<std::pair<int, Probe>> history;
bool reported = false;
bool checkRestore = false; // The first step after any backstep is still the saved state.
unsigned attemptNumber = 1;
Probe probe(PlayLayer* play) {
	Probe value;
	std::uint64_t inputs = 0xcbf29ce484222325ull;
	for (auto const& cmd : play->m_queuedButtons) {
		std::uint64_t word = static_cast<std::uint64_t>(static_cast<int>(cmd.m_button)) | (static_cast<std::uint64_t>(cmd.m_isPush) << 8) |
			(static_cast<std::uint64_t>(cmd.m_isPlayer2) << 9);
		inputs = (inputs ^ word) * 0x100000001b3ull;
	}
	value.inputs = inputs;
	value.attempt = attemptNumber;
	if (auto* body = play->m_player1) {
		value.x = body->m_position.x;
		value.y = body->m_position.y;
		value.yVelocity = body->m_yVelocity;
		value.rotation = body->getRotation();
		value.nodeX = body->getPositionX();
		value.nodeY = body->getPositionY();
		value.lastX = body->m_lastPosition.x;
		value.lastY = body->m_lastPosition.y;
		value.position1 = body->m_position;
		value.node1 = cocos2d::CCPoint(body->getPositionX(), body->getPositionY());
		value.time1 = body->m_totalTime;
		value.buttons1 = Buttons::of(body);
	}
	if (auto* body = play->m_player2) {
		value.position2 = body->m_position;
		value.node2 = cocos2d::CCPoint(body->getPositionX(), body->getPositionY());
		value.time2 = body->m_totalTime;
		value.buttons2 = Buttons::of(body);
		value.hasPlayer2 = true;
	}
	value.levelTime = play->m_gameState.m_levelTime;
	value.progress = play->m_gameState.m_currentProgress;
	return value;
}
// Only a backstep's replay repeats the same inputs; new play after it may differ on purpose.
void compare(PlayLayer* play, int frame, bool replaying) {
	if (frame < 0) return;
	replaying = replaying || checkRestore;
	checkRestore = false;
	if (history.size() != kRing) history.assign(kRing, {-1, Probe{}});
	auto& slot = history[static_cast<size_t>(frame) % kRing];
	auto now = probe(play);
	if (!replaying || slot.first != frame) {
		slot = {frame, now};
		return;
	}
	// Each game update starts by moving the player's node back to m_position, and inside an update
	// m_position trails the node by a step. So the original run's state at a step depended on
	// where that step fell in its update. The replay runs each step as its own update, which
	// pulled the player back a step every step: it fell behind and reached things frames late.
	// Put back both positions exactly as the original run had them at this step.
	auto put = [](PlayerObject* body, cocos2d::CCPoint node, cocos2d::CCPoint position, double time, Buttons const& buttons) {
		body->setPosition(node);
		body->m_position = position;
		body->m_totalTime = time;
		buttons.apply(body);
	};
	if (play->m_player1) put(play->m_player1, slot.second.node1, slot.second.position1, slot.second.time1, slot.second.buttons1);
	if (play->m_player2 && slot.second.hasPlayer2) put(play->m_player2, slot.second.node2, slot.second.position2, slot.second.time2, slot.second.buttons2);
}
}

// A save is a whole checkpoint (0.5-1 ms on a busy level); saving every step at 240 TPS took about
// a quarter of each frame. Sixty saves a second, and a backstep replays the few steps between.
int Logic::backstepInterval() const {
	double rate = sampleRate();
	if (!std::isfinite(rate) || rate <= 60.0) return 1;
	return std::max(1, static_cast<int>(std::lround(rate / 60.0)));
}

void Logic::releaseBackstep(BackstepSave& save, char const* why) {
	if (!save.object) return;
	auto broken = save.broken ? std::string{"flagged earlier"} : echo::state_check::broken(save.object->m_gameState);
	if (broken.empty()) {
		save.object->release();
	} else {
		// Its game state is broken (GD's destructor would crash on it): leaked instead of released.
		log::warn("Echo backstep save at frame {} has a broken game state ({}) on {}; leaked instead of released", save.echo.frame, broken, why);
	}
	save.object = nullptr;
}

void Logic::clearBacksteps() {
	++attemptNumber;
	auto& value = Logic::get();
	history.clear();
	reported = false;
	for (auto& save : value.backsteps) Logic::get().releaseBackstep(save, "clear");
	pinnedCamera().reset();
	pinnedThisFrame = false;
	value.backsteps.clear();
	Logic::get().clearBackstepObjects();
	if (!value.backstepSwapped) value.backstepInputs.clear();
}

// Called at the start of each live physics step, before it runs: the save holds the state at stepFrame.
void Logic::saveBackstep(PlayLayer* play) {
	auto& value = Logic::get();
	if (!play || value.predicting || value.backstepping) return;
	bool resim = value.backstepTarget >= 0;
	if (!value.backstep || Logic::get().renderCapturing()) {
		if (!value.backsteps.empty()) Logic::get().clearBacksteps();
		return;
	}
	// Saves replay with the inputs of the state they were made in. Starting or stopping
	// recording or playback leaves nothing to replay them with.
	RunState mode = resim ? value.backstepState : value.state;
	if (mode != value.backstepSavedState) {
		Logic::get().clearBacksteps();
		value.backstepSavedState = mode;
	}
	int frame = value.stepFrame;
	if (mode == RunState::Idle && !resim) {
		for (auto const& cmd : play->m_queuedButtons)
			value.backstepInputs.push_back({frame + 1, static_cast<uint8_t>(cmd.m_button), cmd.m_isPush, cmd.m_isPlayer2});
	}
	compare(play, frame, resim);
	// Saves are TPS / 60 steps apart in play, so at a high TPS a step back replays dozens of steps.
	// The replay after a step back saves every step, so the next steps back are a load each with no
	// replay. Frame advance saves the state shown before each advance, for a step back to land on.
	bool shown = value.saveShownStep;
	value.saveShownStep = false;
	bool dense = resim || shown;
	if (!dense && !value.backsteps.empty() && frame - value.backsteps.back().echo.frame < Logic::get().backstepInterval()) return;
	if (!value.backsteps.empty() && value.backsteps.back().echo.frame >= frame) return;
	// createCheckpoint leaves the object ID counter and variance generator as they were (object_counters.cpp).
	auto* object = play->createCheckpoint();
	if (!object) return;
	object->retain();
	auto extra = std::make_shared<BackstepExtra>();
	if (play->m_player1) extra->player1 = SavedPlayerCheckpoint::create(play->m_player1);
	if (play->m_player2) {
		extra->player2 = SavedPlayerCheckpoint::create(play->m_player2);
		extra->hasPlayer2 = true;
	}
	if (play->m_effectManager) extra->persistentItems = play->m_effectManager->m_persistentItemCountMap;
	extra->calcNonEffect = play->m_calcNonEffectObjects;
	extra->calcNonEffectSize = play->m_calcNonEffectObjectsSize;
	extra->used = usedMarks(play);
	extra->camera = CameraState::of(play);
	value.backsteps.push_back({object, Logic::get().captureCheckpoint(play, object, frame), std::move(extra), {}});
	Logic::get().noteBackstepObjects(play);
	// The replay's saves come on top of the limit: they get used up as the step back goes on, and
	// trimming for them would drop the oldest saves still ahead of it.
	size_t keep = static_cast<size_t>(std::max(value.backstepSaves, 1));
	while (!resim && value.backsteps.size() > keep) {
		Logic::get().releaseBackstep(value.backsteps.front(), "trim");
		value.backsteps.pop_front();
	}
}

// After a reset that was not a backstep: saves past the frame respawned at belong to a gone future.
void Logic::trimBacksteps(PlayLayer* play) {
	++attemptNumber;
	pinnedCamera().reset();
	pinnedThisFrame = false;
	auto& value = Logic::get();
	if (value.backsteps.empty()) return;
	// A full restart rolls a new attempt; nothing saved before it applies.
	if (!play || !Logic::get().savedCheckpoint(play)) {
		Logic::get().clearBacksteps();
		return;
	}
	while (!value.backsteps.empty() && value.backsteps.back().echo.frame > value.stepFrame) Logic::get().popBackstep();
	std::erase_if(value.backstepInputs, [&](Frame const& input) { return input.number > value.stepFrame; });
}

bool Logic::stepBack(PlayLayer* play, int steps) {
	auto& value = Logic::get();
	if (!play || !play->m_checkpointArray || steps <= 0) {
		return false;
	}
	if (!value.backstep || value.backstepTarget >= 0 || value.backstepping || Logic::get().renderCapturing() ||
		play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
		if (!value.backstep) value.message = "Turn on Backstep first";
		Logic::get().refreshPopup();
		return false;
	}
	int target = std::max(0, value.stepFrame - steps);
	while (!value.backsteps.empty() && value.backsteps.back().echo.frame > target) Logic::get().popBackstep();
	if (value.backsteps.empty()) {
		value.message = "Nothing saved to step back to";
		Logic::get().refreshPopup();
		return false;
	}
	// Loading a broken save would copy its broken game state (Logic/state_check.hpp).
	while (!value.backsteps.empty() && (value.backsteps.back().broken || !echo::state_check::broken(value.backsteps.back().object->m_gameState).empty())) {
		value.backsteps.back().broken = true;
		Logic::get().popBackstep();
	}
	if (value.backsteps.empty()) {
		value.message = "Nothing saved to step back to";
		Logic::get().refreshPopup();
		return false;
	}
	auto save = value.backsteps.back();
	bool resim = save.echo.frame < target;
	reported = false;
	checkRestore = true;
	// The frames between the save and the target come back by playing the recorded inputs.
	// Playback restores holds from the inputs and leaves them untrimmed until the target.
	value.backstepState = value.state;
	value.backstepPlayP1 = value.playP1;
	value.backstepPlayP2 = value.playP2;
	if (resim) {
		value.state = RunState::Playing;
		value.playP1 = value.playP2 = true;
		if (value.backstepState == RunState::Idle) {
			value.backstepReplayPos = value.replayPos;
			std::swap(value.inputs, value.backstepInputs);
			std::swap(value.poses, value.backstepPoses);
			std::swap(value.inputIds, value.backstepIds);
			value.backstepSwapped = true;
			value.touchInputs();
		}
	}
	// resetLevel loads the last checkpoint in the array, in practice mode or not.
	value.backstepping = true;
	play->m_checkpointArray->addObject(save.object);
	value.checkpoints.push_back(save.echo);
	bool musicSync = play->m_practiceMusicSync;
	play->m_practiceMusicSync = true;
	play->resetLevel();
	play->m_practiceMusicSync = musicSync;
	play->m_checkpointArray->removeObject(save.object, true);
	Logic::get().syncCheckpoints(play);
	if (auto* extra = save.extra.get()) {
		auto put = [](PlayerObject* body, SavedPlayerCheckpoint& shot) {
			if (!body) return;
			// The collision logs and trails are live nodes the player owns now; keep those.
			auto logs = std::array{body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight};
			auto* wave = body->m_waveTrail;
			// A pending practice checkpoint from save time may be freed since; keep the current one.
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
		if (extra->hasPlayer2) put(play->m_player2, extra->player2);
		// Silicate's copy sets the node to m_position, which inside a step is the previous step's
		// position. Echo's own snapshot keeps the node, m_position and m_lastPosition apart.
		Logic::get().applyPlayer(play->m_player1, save.echo.player1);
		Logic::get().applyPlayer(play->m_player2, save.echo.player2);
		if (play->m_effectManager) play->m_effectManager->m_persistentItemCountMap = extra->persistentItems;
		play->m_calcNonEffectObjects = extra->calcNonEffect;
		play->m_calcNonEffectObjectsSize = extra->calcNonEffectSize;
		for (auto const& mark : extra->used) {
			mark.object->m_activatedByPlayer1 = mark.p1;
			mark.object->m_activatedByPlayer2 = mark.p2;
		}
	}
	// The respawn re-pressed held buttons. The saved state already holds them.
	play->m_queuedButtons.clear();
	Logic::get().loadBackstepObjects(save.echo.frame);
	// The camera as it was at the save. A replay to the target moves it from there step by step as
	// the live run did; a load that is already the target shows it as it is.
	pinnedCamera().reset();
	pinnedThisFrame = false;
	if (auto* extra = save.extra.get(); extra && extra->camera.present) {
		// The respawn's camera reset also stops the camera's tweens (zoom, offset, rotation).
		auto& tweens = play->m_gameState.m_tweenActions;
		auto const& saved = save.object->m_gameState.m_tweenActions;
		bool same = tweens.size() == saved.size() && std::ranges::all_of(saved, [&](auto const& entry) { return tweens.contains(entry.first); });
		if (!same) tweens = saved;
		else
			for (auto const& [key, tween] : saved) tweens.find(key)->second = tween;
		showCamera(play, extra->camera);
		if (!resim) pinCamera(play);
	}
	value.backstepping = false;
	value.frameAdvance = true;
	value.holdingAdvance = false;
	if (resim) {
		value.backstepTarget = target;
		value.advanceSteps = target - save.echo.frame;
		value.advanceQueued = true;
	} else {
		value.advanceQueued = false;
	}
	return true;
}

// Runs after the frame-advance steps that replay to the target, or before any reset that cuts them short.
void Logic::finishBackstep(PlayLayer* play) {
	auto& value = Logic::get();
	if (value.backstepTarget < 0) return;
	// The replay reached the frame stepped back to: keep the camera it arrived at on screen.
	if (play && value.stepFrame == value.backstepTarget && !play->m_playerDied) pinCamera(play);
	value.backstepTarget = -1;
	value.state = value.backstepState;
	value.playP1 = value.backstepPlayP1;
	value.playP2 = value.backstepPlayP2;
	value.advanceQueued = false;
	value.advanceSteps = 1;
	int frame = value.stepFrame;
	if (value.backstepSwapped) {
		std::erase_if(value.inputs, [frame](Frame const& input) { return input.number > frame; });
		std::swap(value.inputs, value.backstepInputs);
		std::swap(value.poses, value.backstepPoses);
		std::swap(value.inputIds, value.backstepIds);
		value.backstepSwapped = false;
		value.touchInputs();
		value.replayPos = value.backstepReplayPos;
		return;
	}
	if (!play) return;
	if (value.isRecording()) {
		Logic::get().trimRecordedInputs(value, frame);
		Logic::get().trimRates(frame);
		Logic::get().sortInputs();
	}
	if (value.isPlaying()) Logic::get().setReplayPos(play);
	else value.replayPos = value.inputs.size();
}

void Logic::pollBackHold() {
	auto& value = Logic::get();
	auto* play = PlayLayer::get();
	if (!value.holdingBack || !play) return;
	if (GUI::get().typing()) {
		value.holdingBack = false;
		return;
	}
	if (value.backstepTarget >= 0) return;
	auto now = std::chrono::steady_clock::now();
	auto start = value.backHoldStart + std::chrono::milliseconds(value.advanceHoldMs);
	if (now < start) return;
	auto from = std::max(start, value.lastBackStep);
	double rate = value.sampleRate() * std::clamp(value.advanceSpeed, 1.f, 200.f) / 100.0 * value.speedhack;
	if (!std::isfinite(rate) || !(rate > 0.0)) return;
	double wanted = std::floor(std::chrono::duration<double>(now - from).count() * rate);
	if (wanted < 1.0) return;
	int steps = static_cast<int>(std::min(wanted, 1e6));
	value.lastBackStep = from + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(steps / rate));
	if (!Logic::get().stepBack(play, steps)) value.holdingBack = false;
}

#ifdef GEODE_IS_WINDOWS
static_assert(offsetof(PlayLayer, m_checkpointArray) == 0x3888);
#endif

class $modify(BackstepPlayLayer, PlayLayer) {
	// Stepping back is not a new attempt.
	void updateAttempts() {
		if (Logic::get().backstepping) return;
		PlayLayer::updateAttempts();
	}
};
