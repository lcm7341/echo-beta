#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cstdint>
#include <utility>
#include <vector>

using namespace geode::prelude;

// No Mirror Portals has to stay visual. Skipping GJBaseGameLayer::toggleFlipped kept the flip
// progress at 0, and PlayerObject::levelFlipping (read by PlayerObject::update, updateJump and
// hitGround) and the camera read it, so a macro recorded with the option on died without it and the
// other way round. GD flips as usual; only what it draws changes. On Windows 2.2081:
// - updateCamera mirrors the camera x, then sets m_objectLayer's scale X to direction * zoom
//   (direction at layer+0x3204, -1.0 while mirrored) and copies the transform to the extra game
//   layers (updateExtraGameLayers). Gameplay keeps its own camera (m_gameState.m_cameraPosition);
//   the transform and rotate triggers set the object layer to identity while they work. Mirroring
//   reflects the screen about its centre, so after updateCamera the object layer gets its scale X
//   sign back and its x from winWidth - x.
// - updateCamera hands the finished mirror to PlayLayer::flipArt, which only scales the attempt
//   label; hooks.cpp passes "not flipped".
// - updateCameraBGArt scrolls the background, middleground and ground by a factor from the flip
//   progress (1 - 2p, then -2(p - 0.5)), so it runs with the progress at 0.
// The flip itself also moves nodes that gameplay reads: landing snaps against object node
// positions, so a macro split by 2 units when nodes were put back during the update. Those are
// only changed while visit draws and are put back straight after:
// - updateVisibility slides each visible object's node toward its mirrored spot
//   (screenFlipObject); the slid ones are recorded and drawn where they were,
// - update ends by moving each player's node to an in-between spot (update+0x17b5), drawn on
//   m_position instead, and
// - visit flips both players (flipObjects) once past half way and back after drawing; flipping
//   them once more around it cancels that while every setter still runs as GD calls it.
namespace {
struct ScreenFlip {
	Ref<GameObject> object;
	CCPoint before;
	CCPoint after;
};
std::vector<ScreenFlip>& screenFlips() {
	static std::vector<ScreenFlip> flips;
	return flips;
}

#ifdef GEODE_IS_WINDOWS
float& mirrorDirection(GJBaseGameLayer* layer) {
	return *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(layer) + 0x3204);
}

// Same players and condition as the update's own draw offset (player byte 0xa2a clear; player 2
// only while the layer byte 0x422 is set).
bool offsetsPlayer(GJBaseGameLayer* layer, PlayerObject* body) {
	if (!body || reinterpret_cast<std::uint8_t*>(body)[0xa2a] != 0) return false;
	return body == layer->m_player1 || reinterpret_cast<std::uint8_t*>(layer)[0x422] != 0;
}
#endif
}

class $modify(NoMirrorLayer, GJBaseGameLayer) {
	void visit() {
#ifdef GEODE_IS_WINDOWS
		float progress = m_gameState.m_flipProgress;
		if (!Logic::get().noMirrorPortals || !Logic::asPlay(this) || !(progress > 0.f && progress < 1.f)) {
			screenFlips().clear();
			GJBaseGameLayer::visit();
			return;
		}
		// visit's own test (GJBaseGameLayer::visit+0x27): it draws through CCNode::visit when
		// layer+0x31f4 is below 1, and otherwise flips the players once past half way.
		bool plain = *reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(this) + 0x31f4) < 1.f;
		bool flipped = !plain && m_player1 && (mirrorDirection(this) == 1.f ? progress >= .5f : progress <= .5f);
		std::vector<std::pair<PlayerObject*, CCPoint>> players;
		for (auto* body : {m_player1, m_player2}) {
			if (!offsetsPlayer(this, body)) continue;
			players.emplace_back(body, body->getPosition());
			body->setPosition(body->m_position);
		}
		auto& flips = screenFlips();
		for (auto& flip : flips) {
			if (flip.object->getPosition() == flip.after) flip.object->setPosition(flip.before);
		}
		if (flipped) flipObjects();
		GJBaseGameLayer::visit();
		if (flipped) flipObjects();
		for (auto& flip : flips) {
			if (flip.object->getPosition() == flip.before) flip.object->setPosition(flip.after);
		}
		for (auto& [body, position] : players) body->setPosition(position);
#else
		GJBaseGameLayer::visit();
#endif
	}

	void updateCamera(float dt) {
		GJBaseGameLayer::updateCamera(dt);
		if (!Logic::get().noMirrorPortals || !Logic::asPlay(this) || !m_objectLayer) return;
#ifdef GEODE_IS_WINDOWS
		if (mirrorDirection(this) != -1.f) return;
#else
		if (!(m_objectLayer->getScaleX() < 0.f)) return;
#endif
		float scaleX = m_objectLayer->getScaleX();
		auto position = m_objectLayer->getPosition();
		m_objectLayer->setScaleX(-scaleX);
		m_objectLayer->setPosition({CCDirector::get()->getWinSize().width - position.x, position.y});
		updateExtraGameLayers();
	}

	void updateCameraBGArt(CCPoint position, float zoom) {
		if (!Logic::get().noMirrorPortals || !Logic::asPlay(this)) {
			GJBaseGameLayer::updateCameraBGArt(position, zoom);
			return;
		}
		float progress = m_gameState.m_flipProgress;
		m_gameState.m_flipProgress = 0.f;
		GJBaseGameLayer::updateCameraBGArt(position, zoom);
		m_gameState.m_flipProgress = progress;
	}
};

class $modify(NoMirrorObjects, PlayLayer) {
	void updateVisibility(float dt) {
		screenFlips().clear();
		PlayLayer::updateVisibility(dt);
	}

	void screenFlipObject(GameObject* object) {
		if (!Logic::get().noMirrorPortals || !object) {
			PlayLayer::screenFlipObject(object);
			return;
		}
		auto before = object->getPosition();
		PlayLayer::screenFlipObject(object);
		auto after = object->getPosition();
		if (after != before) screenFlips().push_back({object, before, after});
	}
};
