#pragma once

#include <Geode/Geode.hpp>

#include <unordered_set>

namespace Orbs {
struct Pose {
	GameObject* object = nullptr;
	cocos2d::CCPoint pos;
	float rotation = 0.f;
	float rotationX = 0.f;
	float rotationY = 0.f;
};

// True only when Click Dash/Black Orbs should assist this icon. Never during playback of that icon.
bool clickOrbAssist(bool player2);
// A new jump press is over dash/black orbs. Returns how many release+press swifts to run and
// record (one per stacked orb). 0 means no assist.
int claimClick(GJBaseGameLayer* layer, PlayerObject* player, bool player2);
// pushButton sets the hold before it touches rings, so this remembers whether the hold was already down.
void beginPush(PlayerObject* player);
void endPush(PlayerObject* player);
// A jump press in this step's queue. Dash and black orbs click on that press, not a hold or release.
void noteJumpPresses(PlayLayer* play);
// Once per physics step, before the queue is read. Last step's tool hit is done.
void beginStep();
// The tool's own swift may take a dash or black orb. A hold or release must not.
void armTool(bool on);
void commitToolHits();
bool blockHoldRing(PlayerObject* player, GameObject* object);
// A jump or gravity pad was hit during collision, which runs after the step reads its clicks.
void clickPads(PlayLayer* play, PlayerObject* player, int objectType, GameObject* pad);
// Queues that pad's swift before this step reads the queue. The rings from the pad step are still touching.
void flushPads(PlayLayer* play);
bool hasPendingPads();
// The trajectory's copy of a live orb click, pressed straight on the fake player.
// freshClick is a press this step. A hold or release over the orb does not click it.
void clickFake(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int>& used, bool freshClick);
// Pad blue and black orbs for one prediction. The swift lands on the step after the pad, same as the live player.
void resetPredictionPads();
void notePredictionPad(GJBaseGameLayer* play, PlayerObject* player, GameObject* pad);
void applyPredictionPads(GJBaseGameLayer* play, PlayerObject* player, bool player2);
void clear();
// The orbs as they are this frame. The trajectory keeps them here so a fast spinner
// is the path from now, not the path after the orbs have turned.
std::vector<Pose> capture(GJBaseGameLayer* play, PlayerObject* player);
void restore(std::vector<Pose> const& poses);
uint32_t poseHash(GJBaseGameLayer* play, PlayerObject* player);
}
