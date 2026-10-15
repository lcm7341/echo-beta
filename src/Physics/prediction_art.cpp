// a batch of prediction steps doesnt need the trail & particles placed on every step. they are placed once at the end
#include "prediction_art.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Hook.hpp>
#include <algorithm>
#include <cstdint>
#include <string_view>

using namespace geode::prelude;

namespace phys {
    namespace {
        // the real player still needs other mods setPosition hooks (misc bugfixes fixes the wave trail there)
        bool fast_pos = false, fast_live = false;
    }

    bool fast_prediction_pos() {
        return fast_pos;
    }

    bool fast_live_pos() {
        return fast_live;
    }

    void prepare_prediction_art() {
#ifdef GEODE_IS_WINDOWS
        static bool const supported = [] {
            auto* code = reinterpret_cast<uint8_t const*>(geode::base::get() + 0x39c660); // PlayerObject::setPosition
            uint64_t hash = 14695981039346656037ull;                                      // fnv-1a start value
            for (size_t i = 0; i < 0xcab; ++i)
                hash = (hash ^ code[i]) * 1099511628211ull; // fnv-1a prime
            return hash == 0x4382ee25ecaa5ff1ull;           // gd code hash
        }();
        fast_pos = fast_live = supported;
        static bool reported = false;
        if (!fast_pos) {
            reported = true;
            return;
        }
        // visual only setPosition hooks, fake players can skip them
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
        reported = true;
#else
        fast_pos = fast_live = false;
#endif
    }
}
