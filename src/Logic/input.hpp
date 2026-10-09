#pragma once

#include <Geode/Geode.hpp>

// Physical key and mouse state outside the game's own queue. Windows asks the OS. Other platforms
// only see what reaches the level's keypress hook, and touches already live on the UI layer.
namespace Input {
bool jumpKeyDown();
bool leftKeyDown();
bool rightKeyDown();
bool mouseDown();
void noteKey(cocos2d::enumKeyCodes key, bool down);
void clearKeys();
}
