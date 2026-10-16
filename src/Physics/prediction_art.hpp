#pragma once
#include <Geode/Geode.hpp>

using namespace cocos2d;

namespace phys {
	// once before a prediction NOT every step
	void prepare_prediction_art();
	bool can_skip_prediction_art();
	bool can_defer_player_art();

	inline void pos_without_art(PlayerObject* player, const CCPoint& position) {
		player->m_isObjectRectDirty = true;
		player->m_isOrientedBoxDirty = true;
		player->GameObject::setPosition(position);
	}
}
