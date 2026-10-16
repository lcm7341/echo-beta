#pragma once

#include <Geode/Geode.hpp>

using namespace cocos2d;

// real key & mouse state NOT gds queue
namespace Input {
	bool jump_key_down();
	bool left_key_down();
	bool right_key_down();
	bool mouse_down();
	void note_key(enumKeyCodes key, bool down);
	void clear_keys();
}
