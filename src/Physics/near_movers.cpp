#include "Physics/near_movers.hpp"
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

#define SWEEP_MARGIN 8.f

namespace NearMovers {
	static gd::vector<GameObject*> g_near;

	gd::vector<GameObject*>* filter(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
		const CCRect& box = player->getObjectRect();
		float margin = SWEEP_MARGIN + std::fabs(dt) * 5.f;
		float half_w = box.size.width * 0.5f + margin, half_h = box.size.height * 0.5f + margin;
		float px = box.origin.x + box.size.width * 0.5f, py = box.origin.y + box.size.height * 0.5f;
		g_near.clear();
		int used = std::min(count, (int)objects->size());
		for (int i = 0; i < used; ++i) {
			auto* object = (*objects)[(std::size_t)i];
			if (!object) continue;
			float scale = std::max(std::fabs(object->m_scaleX), std::fabs(object->m_scaleY));
			float reach = (object->m_width + object->m_height + object->m_objectRadius) * scale + std::fabs(object->m_customBoxOffset.x) +
				std::fabs(object->m_customBoxOffset.y) + std::fabs(object->m_boxOffset.x) + std::fabs(object->m_boxOffset.y);
			float reach_w = half_w + reach, reach_h = half_h + reach;
			bool close = std::fabs((float)object->m_positionX - px) <= reach_w && std::fabs((float)object->m_positionY - py) <= reach_h;
			if (!close) close = std::fabs(object->m_lastPosition.x - px) <= reach_w && std::fabs(object->m_lastPosition.y - py) <= reach_h;
			if (close) g_near.push_back(object);
		}
		return &g_near;
	}
}
