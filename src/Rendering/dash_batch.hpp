#pragma once
#ifdef GEODE_IS_WINDOWS
namespace cocos2d {
	class CCSprite;
}

using namespace cocos2d;

extern volatile bool echo_dash_batching_enabled;

namespace echo::dash {
	void track_draw(CCSprite* root, CCSprite* outline);
	void untrack_draw(CCSprite* root);
}
#endif
