#include "Logic/logic.hpp"

using namespace geode::prelude;

$execute {
    Logic::get().boot();
    listenForAllKeybindSettingPresses([](std::string_view key, Keybind const&, bool down, bool repeat, double) {
        if (repeat)
            return;
        Logic::get().on_keybind(key, down);
    });
}
