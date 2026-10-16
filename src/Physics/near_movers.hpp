#pragma once

#include <Geode/Geode.hpp>

namespace NearMovers {
	gd::vector<GameObject*>* filter(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt);
}
