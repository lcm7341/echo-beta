// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
// Silicate's reimplementation of GD internals, adapted only to build inside Echo.

#pragma once

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace phys {
// linkPartner mirrors a portal onto the other icon in a 1-player dual. An orb flips only the icon that clicked it.
void flipGravity(PlayerObject* player, bool gravity, bool linkPartner = false);
void propellPlayer(PlayerObject* player, float force, bool playBumpEffect,
                   int p2);
void bumpPlayer(PlayerObject* player, float force, int p2, bool playBumpEffect,
                GameObject* object);
}  // namespace phys
