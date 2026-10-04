#pragma once
#include <Geode/cocos/include/cocos2d.h>
using namespace cocos2d;

class CCCircleWave;

namespace echo::waves {
	void merge_fresh();
	bool count_duplicate(CCNode* parent, CCPoint position, ccColor3B color, float from, float to,
		float duration, bool fadeIn, bool easeOut, void* delegate);
	float count_of(CCCircleWave* wave);
}
