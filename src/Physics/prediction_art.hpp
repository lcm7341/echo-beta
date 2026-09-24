#pragma once
#include <Geode/Geode.hpp>

using namespace cocos2d;

namespace phys {
	// once before a prediction NOT every step
	void prepare_prediction_art();
	bool fast_prediction_pos();

	inline void pos_without_art(PlayerObject* player, CCPoint const& position) {
		player->m_isObjectRectDirty = true;
		player->m_isOrientedBoxDirty = true;
		player->GameObject::setPosition(position);
	}
}
