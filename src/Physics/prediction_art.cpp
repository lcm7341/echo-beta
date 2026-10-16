// a batch of prediction steps doesnt need the trail & particles placed on every step. they are placed once at the end
#include "prediction_art.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Hook.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>

using namespace geode::prelude;

namespace phys {
    static bool fast_pos = false, fast_live = false;

    bool fast_prediction_pos() {
        return fast_pos;
    }

    bool fast_live_pos() {
        return fast_live;
    }

    void prepare_prediction_art() {
#ifdef GEODE_IS_WINDOWS
        fast_pos = fast_live = true;
        constexpr std::string_view VISUAL_ONLY[] = { "cvolton.misc_bugfixes" };
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get())
                continue;
            bool visual = std::ranges::find(VISUAL_ONLY, mod->getID()) != std::end(VISUAL_ONLY);
            for (auto* hook : mod->getHooks()) {
                if (hook->isEnabled() && hook->getAddress() == geode::base::get() + 0x39c640) {
                    fast_live = false;
                    if (!visual)
                        fast_pos = false;
                }
            }
        }
#else
        fast_pos = fast_live = false;
#endif
    }
}
