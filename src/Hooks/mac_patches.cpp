#include <Geode/Geode.hpp>
#ifdef GEODE_IS_MACOS
#include "mac_patches.hpp"
#include "Logic/logic.hpp"
#include <cstddef>
#include <cstring>
#include <utility>

using namespace geode::prelude;

#if defined(__aarch64__)
static double modified_delta(GJBaseGameLayer* self, float dt) {
    static_assert(offsetof(GJBaseGameLayer, m_resumeTimer) == 0x310c);
    int timer = std::exchange(self->m_resumeTimer, 0);
    double delta = self->getModifiedDelta(dt);
    self->m_resumeTimer = timer;
    return delta;
}
#endif

$execute {
    auto base = geode::base::get();
    auto* site = reinterpret_cast<void*>(base + MacPatches::step_address);
    if (std::memcmp(site, MacPatches::step_original.data(), MacPatches::step_original.size()) == 0) {
        auto bytes = MacPatches::step_replacement;
        auto* amt = &Logic::get().steps_this_tick;
        static_assert(sizeof(amt) == 8);
        std::memcpy(bytes.data() + MacPatches::step_pointer_offset, &amt, sizeof(amt));
        auto res = Mod::get()->patch(site, std::vector<unsigned char>(bytes.begin(), bytes.end()));
        if (res) {
            step_patched = true;
        }
    }

#if defined(__aarch64__)
    auto* delta = reinterpret_cast<void*>(base + MacPatches::delta_address);
    if (step_patched && std::memcmp(delta, MacPatches::delta_original.data(), MacPatches::delta_original.size()) == 0) {
        auto bytes = MacPatches::delta_replacement;
        double (*target)(GJBaseGameLayer*, float) = &modified_delta;
        static_assert(sizeof(target) == 8);
        std::memcpy(bytes.data() + MacPatches::delta_pointer_offset, &target, sizeof(target));
        auto res = Mod::get()->patch(delta, std::vector<unsigned char>(bytes.begin(), bytes.end()));
    }
#endif

    auto res = Mod::get()->patch(reinterpret_cast<void*>(base + MacPatches::event_lookup_address),
        std::vector<unsigned char>(MacPatches::event_lookup_replacement.begin(), MacPatches::event_lookup_replacement.end()));
}
#endif
