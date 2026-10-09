#pragma once

#include <Geode/Geode.hpp>

class PlayLayer;

// Level motion for predictions. The trajectory used to move only its stand-in players, so every
// moving object stayed where it was and a spike moving in front of the player was invisible to it.
//
// build() runs GD's own move pipeline (prepareMoveActions, processMoveActionsStep, postMoveActions)
// forward on a shadow copy of the effect manager and records, per step, the state of every object
// that moved; the live objects, sections and effect manager are put back exactly. A prediction then
// calls apply(k) before its k-th step so collisions see each object where GD would have it, and
// rewind() afterwards.
namespace forecast {
// Makes the next `steps` steps of motion available. Reuses and extends the previous forecast when
// the level has only advanced since; otherwise builds it again.
void prepare(PlayLayer* play, int steps, float dt);
// Puts step `step` (1-based) of the forecast onto the objects that are inside `area` (around the
// predicted players) or come into it.
void apply(PlayLayer* play, int step, cocos2d::CCRect const& area);
// Back to the live state after a prediction that applied steps.
void rewind(PlayLayer* play);
// Whether a forecast pass is running: its triggers are meant to fire, on the shadow effect manager.
bool firing();
// Something the forecast cannot see changed the level (a reset, a backstep, a new command).
void invalidate();
// One live physics step happened.
void noteLiveStep();
// The trajectory's prediction of player 1 from now: path[j] after j more steps (path[0] is now),
// and where it was the step before. Objects locked to the player follow it.
void notePlayerPath(std::vector<cocos2d::CCPoint> path, cocos2d::CCPoint before);
}
