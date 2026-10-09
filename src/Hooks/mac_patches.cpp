// puts the mac byte patches in. the bytes & offsets are in mac_patches.hpp
// arm & intel are separate slices of the same app, so every patch exists twice w/ its own offsets
// before anything is written the original bytes are compared (or hashed). any mismatch & that patch is skipped
#include <Geode/Geode.hpp>
#ifdef GEODE_IS_MACOS
#include "mac_patches.hpp"
#include "Logic/logic.hpp"
#include "Rendering/circle_signatures.hpp"
#include <cstddef>
#include <cstring>
#include <utility>

using namespace geode::prelude;

#if defined(__aarch64__)
namespace {
    // what the patched update() calls for its delta
    // the inlined code already ran the resume timer so hold it at 0 or it runs twice
    double modified_delta(GJBaseGameLayer* self, float dt) {
        static_assert(offsetof(GJBaseGameLayer, m_resumeTimer) == 0x310c);
        int timer = std::exchange(self->m_resumeTimer, 0);
        double delta = self->getModifiedDelta(dt);
        self->m_resumeTimer = timer;
        return delta;
    }
}
#endif

$execute {
    namespace native = echo::patches::mac;
    auto base = geode::base::get();
    auto* site = reinterpret_cast<void*>(base + native::step_address);
    // step count patch. not robs bytes = leave it alone
    if (std::memcmp(site, native::step_original.data(), native::step_original.size()) == 0) {
        auto bytes = native::step_replacement;
        auto* amt = &Logic::get().steps_this_tick;
        static_assert(sizeof(amt) == 8);
        // the address of our count goes into the patch itself
        std::memcpy(bytes.data() + native::step_pointer_offset, &amt, sizeof(amt));
        auto res = Mod::get()->patch(site, std::vector<uint8_t>(bytes.begin(), bytes.end()));
        if (res) {
            step_patched = true;
        }
    }

#if defined(__aarch64__)
    // without ts gd rounds every update to whole 240ths & splits that over my steps
    // so every step was the wrong length & macros at a weird tps died on the first click
    auto* delta = reinterpret_cast<void*>(base + native::delta_address);
    if (step_patched && std::memcmp(delta, native::delta_original.data(), native::delta_original.size()) == 0) {
        auto bytes = native::delta_replacement;
        double (*target)(GJBaseGameLayer*, float) = &modified_delta;
        static_assert(sizeof(target) == 8);
        std::memcpy(bytes.data() + native::delta_pointer_offset, &target, sizeof(target));
        auto res = Mod::get()->patch(delta, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    }
#endif

    // libc++ inlined both lookups so theres no function to check, hash the whole thing instead
    auto* event = reinterpret_cast<uint8_t const*>(base + native::event_address);
    if (echo::circles::fingerprint(event, native::event_size) == native::event_hash) {
        auto res = Mod::get()->patch(reinterpret_cast<void*>(base + native::event_lookup_address),
            std::vector<uint8_t>(native::event_lookup_replacement.begin(), native::event_lookup_replacement.end()));
    }
}
#endif
