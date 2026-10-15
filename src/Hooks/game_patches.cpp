// small speedups patched into gd on windows & android. the bytes, offsets & how to find them again are in game_patches.hpp
#include <Geode/Geode.hpp>
#include "game_patches.hpp"
#include "Rendering/circle_signatures.hpp"
#include <cstring>

using namespace geode::prelude;

namespace {
    template <class T, size_t N>
    bool matches(uintptr_t offset, std::array<T, N> const& expected) {
        return std::memcmp(reinterpret_cast<void*>(geode::base::get() + offset),
                   expected.data(), sizeof(expected)) == 0;
    }

    template <class T, size_t N>
    auto install(uintptr_t offset, std::array<T, N> const& replacement) {
        auto* bytes = reinterpret_cast<uint8_t const*>(replacement.data());
        return Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset),
            std::vector<uint8_t>(bytes, bytes + sizeof(replacement)));
    }
}

$execute {
    using namespace echo::patches;
#ifdef GEODE_IS_ANDROID64
    // fuck it we ball
    if (matches(point_address, point_original)) {
        (void)install(point_address, point_replacement);
    }
    if (matches(rings_address, rings_original)) {
        // +4 because only the cbz changes
        (void)install(rings_address + 4, rings_replacement);
    }
#endif
#ifdef GEODE_IS_WINDOWS
    if (matches(event_lookup_signature_address, event_lookup_original) &&
        echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(geode::base::get() + event_map_address),
            event_map_size) == event_map_hash) {
        (void)install(event_lookup_address, event_lookup_replacement);
    }
    if (matches(queue_address, queue_original) && matches(queue_cave_address, queue_cave_original)) {
        // cave goes in first or the call would land on int3s
        auto cave = install(queue_cave_address, queue_cave_replacement);
        if (cave && !install(queue_address, queue_replacement)) {
            // the call didnt go in, so take the cave back out
            (void)cave.unwrap()->disable();
        }
    }
#endif
}
