#include "Logic/input.hpp"

#include <algorithm>
#include <array>

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace cocos2d;

namespace {

    constexpr std::array JUMP_KEYS{ KEY_Space, KEY_W, KEY_Up };
    constexpr std::array LEFT_KEYS{ KEY_A, KEY_Left };
    constexpr std::array RIGHT_KEYS{ KEY_D, KEY_Right };

#ifdef GEODE_IS_WINDOWS
    int virtual_key(enumKeyCodes key) {
        switch (key) {
        case KEY_Space:
            return VK_SPACE;
        case KEY_Up:
            return VK_UP;
        case KEY_Left:
            return VK_LEFT;
        case KEY_Right:
            return VK_RIGHT;
        default:
            return static_cast<int>(key);
        }
    }

    bool any_down(auto const& keys) {
        return std::ranges::any_of(keys, [](enumKeyCodes key) {
            return (GetAsyncKeyState(virtual_key(key)) & 0x8000) != 0;
        });
    }

#else
    std::array<bool, 0x400>& held_keys() {
        static std::array<bool, 0x400> held{};
        return held;
    }

    bool any_down(auto const& keys) {
        auto const& held = held_keys();
        return std::ranges::any_of(keys, [&](enumKeyCodes key) {
            auto idx = static_cast<size_t>(key);
            return idx < held.size() && held[idx];
        });
    }
#endif
}

namespace Input {
    bool jump_key_down() {
        return any_down(JUMP_KEYS);
    }

    bool left_key_down() {
        return any_down(LEFT_KEYS);
    }

    bool rightKeyDown() {
        return any_down(RIGHT_KEYS);
    }

    bool mouseDown() {
#ifdef GEODE_IS_WINDOWS
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
        return false;
#endif
    }

    void note_key(enumKeyCodes key, bool down) {
#ifndef GEODE_IS_WINDOWS
        auto idx = static_cast<size_t>(key);
        if (idx < held_keys().size())
            held_keys()[idx] = down;
#else
        (void)key;
        (void)down;
#endif
    }

    void clear_keys() {
#ifndef GEODE_IS_WINDOWS
        held_keys().fill(false);
#endif
    }
}
