// a batch of prediction steps doesnt need the trail & particles placed on every step. they are placed once at the end
#include "prediction_art.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Hook.hpp>
#include <cstdint>

using namespace geode::prelude;

namespace phys {
    namespace {
        bool fast_pos = false;
    }

    bool fast_prediction_pos() {
        return fast_pos;
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
        fast_pos = supported;
        static bool reported = false;
        if (!fast_pos) {
            reported = true;
            return;
        }
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get())
                continue;
            for (auto* hook : mod->getHooks()) {
                if (hook->isEnabled() && hook->getAddress() == geode::base::get() + 0x39c640) {
                    fast_pos = false;
                    reported = true;
                    return;
                }
            }
        }
        reported = true;
#else
        fast_pos = false;
#endif
    }
}
