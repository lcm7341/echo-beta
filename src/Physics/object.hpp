// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
// Silicate's reimplementation of GD internals, adapted only to build inside Echo.

#pragma once

#include <Geode/Geode.hpp>

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer);

// this doesn't work in trajectory for some odd reason
bool hasBeenActivatedByPlayer(PlayerObject* player, EnhancedGameObject* object);
}  // namespace phys
