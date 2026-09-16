// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#pragma once

#include <Geode/Geode.hpp>

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer);

bool hasBeenActivatedByPlayer(PlayerObject* player, EnhancedGameObject* object);
}
