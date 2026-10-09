#pragma once
#ifdef GEODE_IS_WINDOWS
namespace cocos2d {
	class CCNode;
}

using namespace cocos2d;

namespace echo::bursts {
	void prepare();
	void note_start(CCNode* layer, unsigned count_before, int orb);
	void merge_fresh();
	void note_removed(CCNode* child);
}
#endif
