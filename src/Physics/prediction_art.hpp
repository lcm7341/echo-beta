#pragma once
#include <Geode/Geode.hpp>

namespace phys {
// Updated once before a prediction, never scanned in the physics loop.
void preparePredictionArt();
bool fastPredictionPosition();
inline void positionWithoutArt(PlayerObject* player, cocos2d::CCPoint const& position) {
    // GD's PlayerObject setter does these two writes, then calls the GameObject
    // setter. Everything after that places visual children in world coordinates.
    player->m_isObjectRectDirty = true;
    player->m_isOrientedBoxDirty = true;
    player->GameObject::setPosition(position);
}
}
