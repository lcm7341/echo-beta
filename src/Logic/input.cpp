#include "Logic/input.hpp"

#include <algorithm>
#include <array>

#include <windows.h>

using namespace cocos2d;

namespace {

    constexpr std::array JUMP_KEYS{ KEY_Space, KEY_W, KEY_Up };
    constexpr std::array LEFT_KEYS{ KEY_A, KEY_Left };
    constexpr std::array RIGHT_KEYS{ KEY_D, KEY_Right };

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
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    }

    void note_key(enumKeyCodes key, bool down) {
        (void)key;
        (void)down;
    }

    void clear_keys() {
    }
}
