#pragma once
#ifdef GEODE_IS_WINDOWS
namespace cocos2d { class CCNode; }
namespace echo::bursts {
void prepare();
// After startDashing: the object layer and its child count before the call, and the orb's id.
void noteStart(cocos2d::CCNode* layer, unsigned countBefore, int orb);
// Start of the director's action update: merges identical bursts made since the last one.
void mergeFresh();
// A child left its parent through removeChild.
void noteRemoved(cocos2d::CCNode* child);
}
#endif
