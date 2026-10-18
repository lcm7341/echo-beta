// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
#pragma once

#include <Geode/Geode.hpp>
#include <cmath>

#include "Geode/cocos/support/CCPointExtension.h"
#include "Physics/gjbasegamelayer.hpp"
#include "Physics/gravity.hpp"

using namespace geode::prelude;

namespace phys {
void startDashing(PlayerObject* player, DashRingObject* ring);

void stopDashing(PlayerObject* player);

void ringJump(PlayerObject* player, RingObject* ring);
void activateStreakState(PlayerObject* player);
void placeStreakPointState(PlayerObject* player);
void gameEvent(PlayerObject* player, int event);

void togglePlayerScale(PlayerObject* player, bool smallSize);

void toggleSpiderMode(PlayerObject* player, bool isSpider);
void toggleDartMode(PlayerObject* player, bool isDart);
void toggleRollMode(PlayerObject* player, bool isRoll);
void toggleRobotMode(PlayerObject* player, bool isRobot);
void toggleFlyMode(PlayerObject* player, bool isFly);
void toggleBirdMode(PlayerObject* player, bool isBird);
void toggleSwingMode(PlayerObject* player, bool isSwing);
}
