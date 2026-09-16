// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#pragma once

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace phys {
void flipGravity(PlayerObject* player, bool gravity, bool linkPartner = false);
void propellPlayer(PlayerObject* player, float force, bool playBumpEffect,
                   int p2);
void bumpPlayer(PlayerObject* player, float force, int p2, bool playBumpEffect,
                GameObject* object);
}
