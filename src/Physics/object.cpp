// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#include "Physics/object.hpp"

#include <Geode/Geode.hpp>

#include "Hack/trajectory.hpp"

namespace phys {
bool activatedPlatformer(EnhancedGameObject* object, bool isPlatformer) {
    if (!isPlatformer) {
        if (object->m_isMultiActivate) {
            return true;
        }
    } else if (!object->m_isNoMultiActivate) {
        return true;
    }

    return false;
}

static bool is_second_player(PlayerObject* player) {
    auto& trajectory = Trajectory::get();
    if (trajectory.isFake(player)) return trajectory.is_second_fake(player);
    return player->m_uniqueID != 1;
}

bool hasBeenActivatedByPlayer(PlayerObject* player,
                              EnhancedGameObject* object) {
    bool activatedPlatformer =
        phys::activatedPlatformer(object, player->m_isPlatformer);
    if (activatedPlatformer) {
        return false;
    }

    if (!is_second_player(player)) {
        return object->m_activatedByPlayer1;
    }

    return object->m_activatedByPlayer2;
}

bool canBeActivatedByPlayer(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* object) {
    auto& state = layer->m_gameState;
    auto& ids = state.m_activatedObjectIDs;
    std::pair<int, int> key{ object->m_uniqueID, player->m_uniqueID };
    if (activatedPlatformer(object, player->m_isPlatformer) && ids.find(key) != ids.end()) {
        ids[key] = (int)state.m_commandIndex;
        return false;
    }
    ids[key] = (int)state.m_commandIndex;
    return !hasBeenActivatedByPlayer(player, object);
}
}
