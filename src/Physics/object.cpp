// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
// Silicate's reimplementation of GD internals, adapted only to build inside Echo.

#include "Physics/object.hpp"

#include <Geode/Geode.hpp>

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer) {
    if (!isPlatformer) {
        if (object->m_isMultiActivate) {
            return true;
        }
    } else if (object->m_isNoMultiActivate) {
        return true;
    }

    return false;
}

// this doesn't work in trajectory for some odd reason
bool hasBeenActivatedByPlayer(PlayerObject* player,
                              EnhancedGameObject* object) {
    bool activatedPlatformer =
        phys::activatedPlatformer(object, player->m_isPlatformer);
    if (activatedPlatformer) {
        return false;
    }

    if (!player->m_isSecondPlayer) {
        return object->m_activatedByPlayer1;
    }

    return object->m_activatedByPlayer2;
}
}  // namespace phys
