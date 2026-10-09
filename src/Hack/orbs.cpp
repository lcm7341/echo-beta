#include "Hack/orbs.hpp"
#include "Hack/trajectory.hpp"

#include "Logic/logic.hpp"

#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/RingObject.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

bool Orbs::clickOrbAssist(bool player2) {
	auto& value = Logic::get();
	if (!value.clickDashOrbs && !value.clickBlackOrbs) return false;
	// Playback of this icon must use the stored clicks only.
	if (value.playsPlayer(player2)) return false;
	if (value.isPlaying() && !value.isRecording()) return false;
	if (value.isRecording()) return value.recordsPlayer(player2);
	return true;
}

namespace {
constexpr int kGreenDashOrb = 1704;
constexpr int kBlackOrb = 1330;

std::unordered_set<int> usedP1;
std::unordered_set<int> usedP2;
bool jumpPressP1 = false;
bool jumpPressP2 = false;
bool holdAfterP1 = false;
bool holdAfterP2 = false;
bool toolPressP1 = false;
bool toolPressP2 = false;
bool toolHitP1 = false;
bool toolHitP2 = false;
bool pendingHitP1 = false;
bool pendingHitP2 = false;
bool toolArmed = false;
bool pushing = false;
bool heldBeforeP1 = false;
bool heldBeforeP2 = false;
bool swiftP1 = false;
bool swiftP2 = false;

// A section slot or a box pointer can be the tombstone -1. Calling through it reads that address.
bool live(void const* pointer) {
	auto bits = reinterpret_cast<uintptr_t>(pointer) & 0x00ffffffffffffffull;
	return bits >= 0x10000u && bits < 0x0000800000000000ull;
}

bool icon2Of(PlayerObject* player) {
	auto const& trajectory = Trajectory::get();
	if (player == trajectory.fake2) return true;
	if (player == trajectory.fake1) return false;
	auto* layer = GJBaseGameLayer::get();
	return layer && player == layer->m_player2;
}

enum class Kind { None, Dash, Black };

bool holdingJump(PlayerObject* player) {
	if (!player) return false;
	int jump = static_cast<int>(PlayerButton::Jump);
	auto found = player->m_holdingButtons.find(jump);
	return found != player->m_holdingButtons.end() && found->second;
}

Kind classify(GameObject* object) {
	if (!live(object) || object->m_isDisabled || object->m_isGroupDisabled) return Kind::None;
	auto& value = Logic::get();
	if (value.clickDashOrbs && (object->m_objectID == kGreenDashOrb || object->m_objectType == GameObjectType::DashRing || object->m_objectType == GameObjectType::GravityDashRing)) return Kind::Dash;
	if (value.clickBlackOrbs && (object->m_objectID == kBlackOrb || object->m_objectType == GameObjectType::DropRing)) return Kind::Black;
	return Kind::None;
}

bool taken(GameObject* object, bool player2, std::unordered_set<int> const& used) {
	if (used.contains(object->m_uniqueID)) return true;
	auto* ring = static_cast<EnhancedGameObject*>(object);
	if (static_cast<RingObject*>(object)->m_isSpawnOnly) return true;
	return player2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
}

void eachNear(GJBaseGameLayer* play, CCPoint pos, int span, auto&& visit) {
	float factorX = play->m_sectionXFactor > 1.f ? play->m_sectionXFactor : 100.f;
	float factorY = play->m_sectionYFactor > 1.f ? play->m_sectionYFactor : 100.f;
	int centerX = static_cast<int>(std::floor(pos.x / factorX));
	int centerY = static_cast<int>(std::floor(pos.y / factorY));
	auto const& columns = play->m_sections;
	for (int columnIndex = centerX - span; columnIndex <= centerX + span; ++columnIndex) {
		if (columnIndex < 0 || columnIndex >= static_cast<int>(columns.size())) continue;
		auto* column = columns[static_cast<size_t>(columnIndex)];
		if (!column) continue;
		for (int rowIndex = centerY - span; rowIndex <= centerY + span; ++rowIndex) {
			if (rowIndex < 0 || rowIndex >= static_cast<int>(column->size())) continue;
			auto* row = (*column)[static_cast<size_t>(rowIndex)];
			if (!row) continue;
			for (size_t i = 0; i < row->size(); ++i) {
				auto* object = (*row)[i];
				if (live(object)) visit(object);
			}
		}
	}
}

int travelSpan(GJBaseGameLayer* play, PlayerObject* player) {
	float factor = play->m_sectionXFactor > 1.f ? play->m_sectionXFactor : 100.f;
	float speed = std::abs(player->m_playerSpeed * static_cast<float>(player->m_speedMultiplier) * 5.77f);
	int span = static_cast<int>(std::ceil(speed * Logic::get().trajectorySeconds / factor)) + 2;
	return std::clamp(span, 2, 24);
}

bool overlapsNow(PlayerObject* player, GameObject* object) {
	if (!live(player) || !live(object)) return false;
	player->updateOrientedBox();
	object->updateOrientedBox();
	auto* playerBox = player->getOrientedBox();
	auto* objectBox = object->m_orientedBox;
	if (live(playerBox) && live(objectBox)) return playerBox->overlaps(objectBox);
	return player->getObjectRect().intersectsRect(object->getObjectRect());
}

CCPoint stepDelta(PlayerObject* player) {
	float speed = player->m_playerSpeed * static_cast<float>(player->m_speedMultiplier) * 5.77f;
	float dx = player->m_isGoingLeft ? -speed : speed;
	return {dx, static_cast<float>(player->m_yVelocity)};
}

bool overlapsSoon(PlayerObject* player, GameObject* object) {
	if (!live(player) || !live(object)) return false;
	if (overlapsNow(player, object)) return true;
	if (live(player->m_touchingRings) && player->m_touchingRings->containsObject(object)) return true;
	CCRect next = player->getObjectRect();
	next.origin.x += stepDelta(player).x;
	return next.intersectsRect(object->getObjectRect());
}

bool fitsMode(PlayerObject* player, Kind kind) {
	if (!player || kind == Kind::None) return false;
	if (kind == Kind::Dash) return true;
	return player->m_isBird;
}

GameObject* findOrb(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int> const& used, bool dashOk, bool blackOk) {
	if (!player || player->m_isDead || player->m_controlsDisabled) return nullptr;
	if (!dashOk && !blackOk) return nullptr;
	GameObject* found = nullptr;
	auto consider = [&](GameObject* object) {
		if (found || !object) return;
		Kind kind = classify(object);
		if (kind == Kind::Dash && !dashOk) return;
		if (kind == Kind::Black && !blackOk) return;
		if (kind == Kind::None || !fitsMode(player, kind) || taken(object, player2, used)) return;
		if (overlapsSoon(player, object)) found = object;
	};
	if (live(player->m_touchingRings)) {
		unsigned int count = player->m_touchingRings->count();
		for (unsigned int i = 0; i < count; ++i) {
			auto* object = static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i));
			if (live(object)) consider(object);
		}
	}
	if (!found) eachNear(play, player->getPosition(), 1, consider);
	return found;
}

void swiftHold(PlayerObject* player, bool keepHold) {
	auto key = PlayerButton::Jump;
	int jump = static_cast<int>(key);
	// Press, then release, on this same step. The release is what makes the orb a click.
	// A hold after that is the player's own hold, and it must not take the orb again.
	Orbs::armTool(true);
	player->m_stateRingJump2 = true;
	player->pushButton(key);
	player->releaseButton(key);
	Orbs::armTool(false);
	if (!keepHold) return;
	player->pushButton(key);
	player->m_holdingButtons[jump] = true;
	player->m_jumpBuffered = true;
	player->m_wasJumpBuffered = true;
	player->m_stateJumpBuffered = 1;
}

void queueJump(PlayLayer* play, bool player2, bool down) {
	play->queueButton(static_cast<int>(PlayerButton::Jump), down, player2, Logic::get().clickStamp());
}

// One slot per icon. The collision that hits the pad runs after this step reads its clicks, so the
// swift waits here until the next step. liveBlack means the hold-click already pressed a black orb.
// A blue orb flips gravity, so a second swift in the same step flips it back. That one waits for the next pad.
struct PadStep {
	bool pending = false;
	bool liveBlack = false;
	int black = 0;
	bool anon = false;
	std::vector<GameObject*> blue;
	std::vector<int> pads;
};
PadStep padP1;
PadStep padP2;
std::unordered_set<int> spentPadP1;
std::unordered_set<int> spentPadP2;
std::unordered_set<int> spentBlueP1;
std::unordered_set<int> spentBlueP2;

struct PredictionPad {
	bool pending = false;
	bool liveBlack = false;
	bool anon = false;
	std::vector<GameObject*> black;
	std::vector<GameObject*> blue;
	std::vector<int> pads;
};
PredictionPad predPad1;
PredictionPad predPad2;
std::unordered_set<int> predSpentPad1;
std::unordered_set<int> predSpentPad2;
std::unordered_set<int> predSpentBlue1;
std::unordered_set<int> predSpentBlue2;
std::unordered_set<int> predSpentBlack1;
std::unordered_set<int> predSpentBlack2;

bool jumpOrGravityPad(int type) {
	return type == static_cast<int>(GameObjectType::YellowJumpPad) || type == static_cast<int>(GameObjectType::PinkJumpPad) || type == static_cast<int>(GameObjectType::RedJumpPad) || type == static_cast<int>(GameObjectType::GravityPad) || type == static_cast<int>(GameObjectType::SpiderPad);
}

bool blackOrb(GameObject* object) {
	auto& value = Logic::get();
	return value.padBlackOrb && live(object) && !object->m_isDisabled && !object->m_isGroupDisabled && (object->m_objectID == kBlackOrb || object->m_objectType == GameObjectType::DropRing) && !static_cast<RingObject*>(object)->m_isSpawnOnly;
}

bool blueOrb(GameObject* object) {
	auto& value = Logic::get();
	return value.padBlueOrb && live(object) && !object->m_isDisabled && !object->m_isGroupDisabled && object->m_objectID != 1022 && (object->m_objectID == 84 || object->m_objectType == GameObjectType::GravityRing) && !static_cast<RingObject*>(object)->m_isSpawnOnly;
}

bool orbTriggered(GameObject* object, bool player2) {
	auto* ring = static_cast<EnhancedGameObject*>(object);
	return player2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
}

void rememberBlue(PadStep& step, GameObject* object, bool player2, std::unordered_set<int> const& spent) {
	if (!blueOrb(object) || orbTriggered(object, player2) || spent.contains(object->m_uniqueID)) return;
	if (std::ranges::find(step.blue, object) == step.blue.end()) step.blue.push_back(object);
}

// Rings the collision already collected, plus any stacked on the same spot that the collision listed once.
void collectPadOrbs(PlayLayer* play, PlayerObject* player, bool player2, PadStep& step) {
	std::unordered_set<GameObject*> seen;
	int black = 0;
	auto take = [&](GameObject* object) {
		if (!object || !seen.insert(object).second) return;
		if (blackOrb(object)) ++black;
		rememberBlue(step, object, player2, player2 ? spentBlueP2 : spentBlueP1);
	};
	if (player->m_touchingRings) {
		unsigned int count = player->m_touchingRings->count();
		for (unsigned int i = 0; i < count; ++i) take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
	}
	eachNear(play, player->getPosition(), 1, [&](GameObject* object) {
		if (!blackOrb(object) && !blueOrb(object)) return;
		if (overlapsNow(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect())) take(object);
	});
	if (black > step.black) step.black = black;
}

// The click can only land on one blue orb this step. Leave that one in the touching list.
void armBlue(PlayerObject* player, GameObject* orb) {
	auto* rings = player->m_touchingRings;
	if (!rings) return;
	std::vector<GameObject*> extra;
	unsigned int count = rings->count();
	for (unsigned int i = 0; i < count; ++i) {
		auto* object = static_cast<GameObject*>(rings->objectAtIndex(i));
		if (object != orb && blueOrb(object)) extra.push_back(object);
	}
	for (auto* object : extra) rings->removeObject(object, false);
	if (!rings->containsObject(orb)) rings->addObject(orb);
}

// Press then release, once per orb. A hold-click that already landed this step left the button down, so this only lets go.
void queuePadSwift(PlayLayer* play, bool player2, int times, bool alreadyHeld) {
	int extra = alreadyHeld ? 0 : std::max(times, 1);
	for (int i = 0; i < extra; ++i) {
		queueJump(play, player2, true);
		queueJump(play, player2, false);
	}
	if (alreadyHeld) queueJump(play, player2, false);
}
}

std::vector<Orbs::Pose> Orbs::capture(GJBaseGameLayer* play, PlayerObject* player) {
	std::vector<Pose> poses;
	if (!play || !player) return poses;
	auto& value = Logic::get();
	if (!value.clickDashOrbs && !value.clickBlackOrbs && !value.padBlueOrb && !value.padBlackOrb) return poses;
	eachNear(play, player->getPosition(), travelSpan(play, player), [&](GameObject* object) {
		bool listed = classify(object) != Kind::None || blueOrb(object) || blackOrb(object);
		if (!listed) return;
		if (std::ranges::any_of(poses, [&](Pose const& pose) { return pose.object == object; })) return;
		poses.push_back({object, object->getPosition(), object->getRotation(), object->getRotationX(), object->getRotationY()});
	});
	return poses;
}

void Orbs::restore(std::vector<Pose> const& poses) {
	for (auto const& pose : poses) {
		if (!pose.object) continue;
		// Native setters dirty transforms and bounds even when the value is unchanged.
		// Almost all nearby orbs are untouched by a prediction step.
		bool changed = false;
		if (pose.object->getPosition() != pose.pos) { pose.object->setPosition(pose.pos); changed = true; }
		if (pose.object->getRotation() != pose.rotation) { pose.object->setRotation(pose.rotation); changed = true; }
		if (pose.object->getRotationX() != pose.rotationX) { pose.object->setRotationX(pose.rotationX); changed = true; }
		if (pose.object->getRotationY() != pose.rotationY) { pose.object->setRotationY(pose.rotationY); changed = true; }
		if (changed || pose.object->m_isOrientedBoxDirty) pose.object->updateOrientedBox();
	}
}

uint32_t Orbs::poseHash(GJBaseGameLayer* play, PlayerObject* player) {
	uint32_t hash = 2166136261u;
	auto mix = [&](uint32_t value) { hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2); };
	for (auto const& pose : capture(play, player)) {
		mix(static_cast<uint32_t>(pose.object->m_uniqueID));
		mix(std::bit_cast<uint32_t>(pose.pos.x));
		mix(std::bit_cast<uint32_t>(pose.pos.y));
		mix(std::bit_cast<uint32_t>(pose.rotation));
	}
	return hash;
}

void Orbs::clear() {
	usedP1.clear();
	usedP2.clear();
	jumpPressP1 = false;
	jumpPressP2 = false;
	holdAfterP1 = false;
	holdAfterP2 = false;
	toolPressP1 = false;
	toolPressP2 = false;
	toolHitP1 = false;
	toolHitP2 = false;
	pendingHitP1 = false;
	pendingHitP2 = false;
	toolArmed = false;
	pushing = false;
	heldBeforeP1 = false;
	heldBeforeP2 = false;
	swiftP1 = false;
	swiftP2 = false;
	spentPadP1.clear();
	spentPadP2.clear();
	spentBlueP1.clear();
	spentBlueP2.clear();
	padP1 = {};
	padP2 = {};
	Orbs::resetPredictionPads();
}

void Orbs::clickPads(PlayLayer* play, PlayerObject* player, int objectType, GameObject* pad) {
	if (!play || !player || Logic::get().predicting || !jumpOrGravityPad(objectType)) return;
	auto& value = Logic::get();
	if (!value.padBlackOrb && !value.padBlueOrb) return;
	bool player2 = player == play->m_player2;
	if (value.playsPlayer(player2)) return;
	if (pad && !jumpOrGravityPad(static_cast<int>(pad->m_objectType))) pad = nullptr;
	auto& step = player2 ? padP2 : padP1;
	auto& spentPads = player2 ? spentPadP2 : spentPadP1;
	step.pending = true;
	collectPadOrbs(play, player, player2, step);
	if (!pad) {
		step.anon = true;
		return;
	}
	if (spentPads.contains(pad->m_uniqueID)) return;
	if (std::ranges::find(step.pads, pad->m_uniqueID) == step.pads.end()) step.pads.push_back(pad->m_uniqueID);
}

bool Orbs::hasPendingPads() {
	return padP1.pending || padP2.pending;
}

void Orbs::flushPads(PlayLayer* play) {
	if (!play || Logic::get().predicting) return;
	if (!hasPendingPads()) return;
	auto saved1 = std::move(padP1);
	auto saved2 = std::move(padP2);
	padP1 = {};
	padP2 = {};
	auto flush = [&](PlayerObject* player, bool player2, PadStep const& step) {
		if (!step.pending || !player) return;
		auto& spentPads = player2 ? spentPadP2 : spentPadP1;
		auto& spentBlue = player2 ? spentBlueP2 : spentBlueP1;
		if (step.black > 0) queuePadSwift(play, player2, step.black, step.liveBlack);
		if (step.blue.empty() || (step.pads.empty() && !step.anon)) return;
		// The collision already flipped one. Another swift this step would flip it back, so the next orb waits for the next pad.
		bool alreadyFlipped = std::ranges::any_of(step.blue, [&](GameObject* orb) { return orbTriggered(orb, player2); });
		if (alreadyFlipped) {
			if (!step.pads.empty()) spentPads.insert(step.pads.front());
			return;
		}
		auto waiting = std::ranges::find_if(step.blue, [&](GameObject* orb) {
			return orb && !orbTriggered(orb, player2) && !spentBlue.contains(orb->m_uniqueID);
		});
		if (waiting == step.blue.end()) return;
		armBlue(player, *waiting);
		spentBlue.insert((*waiting)->m_uniqueID);
		if (!step.pads.empty()) spentPads.insert(step.pads.front());
		// Need a real press edge for the orb and for the macro. If already held, release first.
		if (holdingJump(player)) queueJump(play, player2, false);
		queueJump(play, player2, true);
		queueJump(play, player2, false);
	};
	flush(play->m_player1, false, saved1);
	if (play->m_gameState.m_isDualMode) flush(play->m_player2, true, saved2);
}

namespace {
// The orb found and every unused orb of the same kind on the same spot.
std::vector<GameObject*> orbStack(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int> const& used, GameObject* orb) {
	Kind kind = classify(orb);
	std::vector<GameObject*> stack;
	auto take = [&](GameObject* object) {
		if (!live(object) || classify(object) != kind || taken(object, player2, used)) return;
		if (std::ranges::find(stack, object) != stack.end()) return;
		stack.push_back(object);
	};
	if (live(player->m_touchingRings)) {
		unsigned int count = player->m_touchingRings->count();
		for (unsigned int i = 0; i < count; ++i) take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
	}
	eachNear(play, player->getPosition(), 1, [&](GameObject* object) {
		if (!live(object) || classify(object) != kind) return;
		if (overlapsSoon(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect())) take(object);
	});
	if (stack.empty()) stack.push_back(orb);
	return stack;
}
}

void Orbs::armTool(bool on) {
	toolArmed = on;
}

void Orbs::beginStep() {
	usedP1.clear();
	usedP2.clear();
	jumpPressP1 = false;
	jumpPressP2 = false;
	toolHitP1 = false;
	toolHitP2 = false;
	pendingHitP1 = false;
	pendingHitP2 = false;
	toolArmed = false;
	swiftP1 = false;
	swiftP2 = false;
}

void Orbs::commitToolHits() {
	if (pendingHitP1) toolHitP1 = true;
	if (pendingHitP2) toolHitP2 = true;
	pendingHitP1 = false;
	pendingHitP2 = false;
}

bool Orbs::blockHoldRing(PlayerObject* player, GameObject* object) {
	if (!player || !object) return false;
	bool player2 = icon2Of(player);
	// Only the Click Dash/Black Orbs feature blocks hold-through, and only while assisting.
	if (!Orbs::clickOrbAssist(player2)) return false;
	Kind kind = classify(object);
	if (kind == Kind::None || !fitsMode(player, kind)) return false;
	if (toolArmed) return false;
	if (player2 ? swiftP2 : swiftP1) return true;
	bool fresh = pushing && !(player2 ? heldBeforeP2 : heldBeforeP1);
	return !fresh;
}

void Orbs::beginPush(PlayerObject* player) {
	if (!player) return;
	bool player2 = icon2Of(player);
	pushing = true;
	(player2 ? heldBeforeP2 : heldBeforeP1) = holdingJump(player);
}

void Orbs::endPush(PlayerObject* player) {
	if (!player) return;
	bool player2 = icon2Of(player);
	if (!(player2 ? heldBeforeP2 : heldBeforeP1)) (player2 ? swiftP2 : swiftP1) = true;
	pushing = false;
}

void Orbs::noteJumpPresses(PlayLayer* play) {
	jumpPressP1 = false;
	jumpPressP2 = false;
	holdAfterP1 = play && holdingJump(play->m_player1);
	holdAfterP2 = play && play->m_player2 && holdingJump(play->m_player2);
	if (!play) return;
	int jump = static_cast<int>(PlayerButton::Jump);
	for (auto const& cmd : play->m_queuedButtons) {
		int button = static_cast<int>(cmd.m_button);
		if (button != 0 && button != jump) continue;
		if (cmd.m_isPush) (cmd.m_isPlayer2 ? jumpPressP2 : jumpPressP1) = true;
		(cmd.m_isPlayer2 ? holdAfterP2 : holdAfterP1) = cmd.m_isPush;
	}
}

int Orbs::claimClick(GJBaseGameLayer* layer, PlayerObject* player, bool player2) {
	if (!layer || !player || Logic::get().predicting || Logic::get().editorPaused) return 0;
	if (auto* live = Logic::asPlay(layer); live && live->m_isPaused) return 0;
	if (!Orbs::clickOrbAssist(player2)) return 0;
	auto& value = Logic::get();
	if (holdingJump(player) || !player->m_touchingRings) return 0;
	if (player2 ? swiftP2 : swiftP1) return 0;
	auto& used = player2 ? usedP2 : usedP1;
	GameObject* orb = findOrb(layer, player, player2, used, value.clickDashOrbs, value.clickBlackOrbs);
	if (!orb) return 0;
	auto stack = orbStack(layer, player, player2, used, orb);
	for (auto* object : stack) {
		if (object && !player->m_touchingRings->containsObject(object)) player->m_touchingRings->addObject(object);
	}
	player->m_stateRingJump2 = true;
	player->m_stateJumpBuffered = 1;
	if (classify(orb) == Kind::Black) (player2 ? padP2 : padP1).liveBlack = true;
	for (auto* object : stack) {
		if (object) used.insert(object->m_uniqueID);
	}
	// One release+press per stacked orb after the player's press (single = CRC, double = two swifts).
	return std::max(1, static_cast<int>(stack.size()));
}

namespace {
void gatherPredictionOrbs(GJBaseGameLayer* play, PlayerObject* player, bool player2, PredictionPad& step) {
	auto& spentBlue = player2 ? predSpentBlue2 : predSpentBlue1;
	auto& spentBlack = player2 ? predSpentBlack2 : predSpentBlack1;
	auto& liveBlue = player2 ? spentBlueP2 : spentBlueP1;
	auto take = [&](GameObject* object) {
		if (!live(object)) return;
		if (player->m_ringRelatedSet.count(object->m_uniqueID) || orbTriggered(object, player2)) return;
		if (blackOrb(object) && !spentBlack.contains(object->m_uniqueID)) {
			if (std::ranges::find(step.black, object) == step.black.end()) step.black.push_back(object);
		}
		if (blueOrb(object) && !spentBlue.contains(object->m_uniqueID) && !liveBlue.contains(object->m_uniqueID)) {
			if (std::ranges::find(step.blue, object) == step.blue.end()) step.blue.push_back(object);
		}
	};
	if (live(player->m_touchingRings)) {
		unsigned int count = player->m_touchingRings->count();
		for (unsigned int i = 0; i < count; ++i) take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
	}
	eachNear(play, player->getPosition(), 1, [&](GameObject* object) {
		if (!blackOrb(object) && !blueOrb(object)) return;
		if (overlapsNow(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect())) take(object);
	});
}
}

void Orbs::resetPredictionPads() {
	predPad1 = {};
	predPad2 = {};
	predSpentPad1.clear();
	predSpentPad2.clear();
	predSpentBlue1.clear();
	predSpentBlue2.clear();
	predSpentBlack1.clear();
	predSpentBlack2.clear();
}

void Orbs::notePredictionPad(GJBaseGameLayer* play, PlayerObject* player, GameObject* pad) {
	if (!Logic::get().predicting || !play || !player) return;
	auto& value = Logic::get();
	if (!value.padBlackOrb && !value.padBlueOrb) return;
	bool player2 = icon2Of(player);
	if (value.playsPlayer(player2)) return;
	if (pad && !jumpOrGravityPad(static_cast<int>(pad->m_objectType))) return;
	auto& step = player2 ? predPad2 : predPad1;
	auto& spentPads = player2 ? predSpentPad2 : predSpentPad1;
	if (pad && spentPads.contains(pad->m_uniqueID)) return;
	step.pending = true;
	if (!pad) step.anon = true;
	else if (std::ranges::find(step.pads, pad->m_uniqueID) == step.pads.end()) step.pads.push_back(pad->m_uniqueID);
	gatherPredictionOrbs(play, player, player2, step);
}

void Orbs::applyPredictionPads(GJBaseGameLayer* play, PlayerObject* player, bool player2) {
	if (!Logic::get().predicting || !play || !player) return;
	auto& step = player2 ? predPad2 : predPad1;
	if (!step.pending) return;
	PredictionPad saved = step;
	step = {};
	auto& spentPads = player2 ? predSpentPad2 : predSpentPad1;
	auto& spentBlue = player2 ? predSpentBlue2 : predSpentBlue1;
	auto& spentBlack = player2 ? predSpentBlack2 : predSpentBlack1;
	if (!saved.black.empty() && !saved.liveBlack) {
		if (auto* rings = player->m_touchingRings) {
			for (auto* object : saved.black) {
				if (object && !rings->containsObject(object)) rings->addObject(object);
			}
		}
		for (auto* object : saved.black) {
			if (!object || player->m_ringRelatedSet.count(object->m_uniqueID)) continue;
			player->m_stateRingJump2 = true;
			player->m_stateJumpBuffered = 1;
			swiftHold(player, false);
			spentBlack.insert(object->m_uniqueID);
		}
	} else if (saved.liveBlack) {
		player->releaseButton(PlayerButton::Jump);
	}
	if (saved.blue.empty() || (saved.pads.empty() && !saved.anon)) return;
	bool already = std::ranges::any_of(saved.blue, [&](GameObject* orb) {
		return orb && (orbTriggered(orb, player2) || spentBlue.contains(orb->m_uniqueID) || player->m_ringRelatedSet.count(orb->m_uniqueID));
	});
	if (already) {
		if (!saved.pads.empty()) spentPads.insert(saved.pads.front());
		return;
	}
	auto waiting = std::ranges::find_if(saved.blue, [&](GameObject* orb) {
		return orb && !orbTriggered(orb, player2) && !spentBlue.contains(orb->m_uniqueID) && !player->m_ringRelatedSet.count(orb->m_uniqueID);
	});
	if (waiting == saved.blue.end()) return;
	armBlue(player, *waiting);
	spentBlue.insert((*waiting)->m_uniqueID);
	if (!saved.pads.empty()) spentPads.insert(saved.pads.front());
	player->m_stateRingJump2 = true;
	player->m_stateJumpBuffered = 1;
	// Same edges as live flushPads: release first if held, then press+release.
	if (holdingJump(player)) player->releaseButton(PlayerButton::Jump);
	swiftHold(player, false);
}

void Orbs::clickFake(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int>& used, bool freshClick) {
	if (!play || !player) return;
	auto& value = Logic::get();
	if (!Orbs::clickOrbAssist(player2)) {
		if (!freshClick && !holdingJump(player)) used.clear();
		return;
	}
	bool held = holdingJump(player);
	if (!freshClick) {
		if (!held) used.clear();
		return;
	}
	auto key = PlayerButton::Jump;
	GameObject* orb = findOrb(play, player, player2, used, value.clickDashOrbs, value.clickBlackOrbs);
	if (!orb) return;
	auto stack = orbStack(play, player, player2, used, orb);
	if (auto* rings = player->m_touchingRings) {
		for (auto* object : stack) {
			if (object && !rings->containsObject(object)) rings->addObject(object);
		}
	}
	if (classify(orb) == Kind::Black) (player2 ? predPad2 : predPad1).liveBlack = true;
	// Same as live claimClick: one press per stacked orb, ending held if the player is holding.
	int swifts = std::max(1, static_cast<int>(stack.size()));
	if (held) player->releaseButton(key);
	for (int i = 0; i < swifts; ++i) {
		player->m_stateRingJump2 = true;
		player->m_stateJumpBuffered = 1;
		Orbs::armTool(true);
		player->pushButton(key);
		player->releaseButton(key);
		Orbs::armTool(false);
	}
	if (held) {
		player->pushButton(key);
		int jump = static_cast<int>(key);
		player->m_holdingButtons[jump] = true;
		player->m_jumpBuffered = true;
		player->m_wasJumpBuffered = true;
		player->m_stateJumpBuffered = 1;
	}
	for (auto* object : stack) {
		if (object) used.insert(object->m_uniqueID);
	}
}
