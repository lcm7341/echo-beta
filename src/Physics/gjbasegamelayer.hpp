// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
// Silicate's reimplementation of GD internals, adapted only to build inside Echo.

#pragma once

#include <Geode/Geode.hpp>

#include "Physics/gravity.hpp"

namespace phys {
cocos2d::CCArray* getGroup(GJBaseGameLayer* pl, int groupID);
float redirectPlayerForce(PlayerObject* player, float force, float forceMod,
                          float forceMin, float forceMax);

// this function is a nightmare
void teleportPlayer(GJBaseGameLayer* pl, TeleportPortalObject* object,
                    PlayerObject* player);
}  // namespace phys