#include "Logic/logic.hpp"

using namespace geode::prelude;

$execute {
    Logic::get().boot();
#ifndef GEODE_IS_MOBILE
    listenForAllKeybindSettingPresses([](std::string_view key, Keybind const&, bool down, bool repeat, double) {
        if (repeat)
            return;
        Logic::get().on_keybind(key, down);
    });
#endif
}
