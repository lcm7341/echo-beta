#include "prediction_art.hpp"
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Hook.hpp>
#include <cstdint>

using namespace geode::prelude;
namespace phys {
namespace {
bool fastPosition = false;
}
bool fastPredictionPosition() { return fastPosition; }
void preparePredictionArt() {
#ifdef GEODE_IS_WINDOWS
    // Verified GD 2.2081 PlayerObject::setPosition body, excluding the prologue
    // rewritten by Geode's own hook. An unknown body retains the native setter.
    static bool const supported = [] {
        auto* code = reinterpret_cast<uint8_t const*>(geode::base::get() + 0x39c660);
        uint64_t hash = 14695981039346656037ull;
        for (size_t i = 0; i < 0xcab; ++i) hash = (hash ^ code[i]) * 1099511628211ull;
        return hash == 0x4382ee25ecaa5ff1ull;
    }();
    fastPosition = supported;
    static bool reported = false;
    if (!fastPosition) {
        if (!reported) log::warn("Echo trajectory position fast path disabled: native setter differs");
        reported = true;
        return;
    }
    // Other mods may attach gameplay work to this setter. Retain their entire
    // hook chain instead of assuming that work is visual too.
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks()) {
            if (hook->isEnabled() && hook->getAddress() == geode::base::get() + 0x39c640) {
                fastPosition = false;
                if (!reported) log::info("Echo trajectory retains native position setter for {}", mod->getID());
                reported = true;
                return;
            }
        }
    }
    if (!reported) log::info("Echo trajectory physics-only position setter enabled");
    reported = true;
#else
    fastPosition = false;
#endif
}
}
