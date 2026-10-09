#include "Logic/input.hpp"

#include <algorithm>
#include <array>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

namespace {
using cocos2d::enumKeyCodes;

constexpr std::array kJumpKeys{cocos2d::KEY_Space, cocos2d::KEY_W, cocos2d::KEY_Up};
constexpr std::array kLeftKeys{cocos2d::KEY_A, cocos2d::KEY_Left};
constexpr std::array kRightKeys{cocos2d::KEY_D, cocos2d::KEY_Right};

#ifdef GEODE_IS_WINDOWS
int virtualKey(enumKeyCodes key) {
	switch (key) {
		case cocos2d::KEY_Space: return VK_SPACE;
		case cocos2d::KEY_Up: return VK_UP;
		case cocos2d::KEY_Left: return VK_LEFT;
		case cocos2d::KEY_Right: return VK_RIGHT;
		default: return static_cast<int>(key);
	}
}
bool anyDown(auto const& keys) {
	return std::ranges::any_of(keys, [](enumKeyCodes key) { return (GetAsyncKeyState(virtualKey(key)) & 0x8000) != 0; });
}
#else
std::array<bool, 0x400>& heldKeys() {
	static std::array<bool, 0x400> held{};
	return held;
}
bool anyDown(auto const& keys) {
	auto const& held = heldKeys();
	return std::ranges::any_of(keys, [&](enumKeyCodes key) {
		auto index = static_cast<size_t>(key);
		return index < held.size() && held[index];
	});
}
#endif
}

namespace Input {
bool jumpKeyDown() {
	return anyDown(kJumpKeys);
}
bool leftKeyDown() {
	return anyDown(kLeftKeys);
}
bool rightKeyDown() {
	return anyDown(kRightKeys);
}
bool mouseDown() {
#ifdef GEODE_IS_WINDOWS
	return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
	return false;
#endif
}
void noteKey(enumKeyCodes key, bool down) {
#ifndef GEODE_IS_WINDOWS
	auto index = static_cast<size_t>(key);
	if (index < heldKeys().size()) heldKeys()[index] = down;
#else
	(void)key;
	(void)down;
#endif
}
void clearKeys() {
#ifndef GEODE_IS_WINDOWS
	heldKeys().fill(false);
#endif
}
}
