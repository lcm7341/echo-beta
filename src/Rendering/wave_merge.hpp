#pragma once
#include <Geode/cocos/include/cocos2d.h>
class CCCircleWave;
namespace echo::waves {
// Merges identical rings made since the last action update (start of each action update).
void mergeFresh();
// ringJump's ring: true when an identical ring from this frame now counts it (nothing to build).
bool countDuplicate(cocos2d::CCNode* parent, cocos2d::CCPoint position, cocos2d::ccColor3B color, float from, float to,
                    float duration, bool fadeIn, bool easeOut, void* delegate);
// How many identical rings this ring is drawn as; 1 for an unmerged ring.
float countOf(CCCircleWave* wave);
}
