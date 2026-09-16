// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#pragma once

#include <Geode/Geode.hpp>

#include "Physics/gravity.hpp"

namespace phys {
cocos2d::CCArray* getGroup(GJBaseGameLayer* pl, int groupID);
float redirectPlayerForce(PlayerObject* player, float force, float forceMod,
                          float forceMin, float forceMax);

void teleportPlayer(GJBaseGameLayer* pl, TeleportPortalObject* object,
                    PlayerObject* player);
}
