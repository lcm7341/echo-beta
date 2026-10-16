// a batch of prediction steps doesnt need the trail & particles placed on every step. they are placed once at the end
#include "prediction_art.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Hook.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>

using namespace geode::prelude;

#define PLAYER_SET_POSITION 0x39c640

namespace phys {
    static bool skip_prediction_art = false, skip_live_art = false;

    bool can_skip_prediction_art() {
        return skip_prediction_art;
    }

    bool can_defer_player_art() {
        return skip_live_art;
    }

    void prepare_prediction_art() {
#ifdef GEODE_IS_WINDOWS
        skip_prediction_art = skip_live_art = true;
        const std::string_view VISUAL_ONLY_MODS[] = { "cvolton.misc_bugfixes" };
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get()) continue;
            bool visual_only = std::ranges::find(VISUAL_ONLY_MODS, mod->getID()) != std::end(VISUAL_ONLY_MODS);
            for (auto* hook : mod->getHooks()) {
                if (hook->isEnabled() && hook->getAddress() == geode::base::get() + PLAYER_SET_POSITION) {
                    skip_live_art = false;
                    if (!visual_only) skip_prediction_art = false;
                }
            }
        }
#else
        skip_prediction_art = skip_live_art = false;
#endif
    }
}
