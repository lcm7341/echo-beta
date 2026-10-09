#pragma once
#ifdef GEODE_IS_WINDOWS
namespace cocos2d { class CCSprite; }
extern volatile bool echoDashBatchingEnabled;
namespace echo::dash {
void trackDraw(cocos2d::CCSprite* root, cocos2d::CCSprite* outline);
void untrackDraw(cocos2d::CCSprite* root);
}
#endif
