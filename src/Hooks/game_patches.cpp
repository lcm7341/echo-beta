// small speedups patched into gd on windows & android. the bytes, offsets & how to find them again are in game_patches.hpp
#include <Geode/Geode.hpp>
#include "game_patches.hpp"
#include <cstring>

using namespace geode::prelude;

template <class T, size_t N>
static bool matches(uintptr_t offset, const std::array<T, N>& expected) {
    return std::memcmp(reinterpret_cast<void*>(geode::base::get() + offset),
               expected.data(), sizeof(expected)) == 0;
}

template <class T, size_t N>
static auto install(uintptr_t offset, const std::array<T, N>& replacement) {
    auto* bytes = reinterpret_cast<const unsigned char*>(replacement.data());
    return Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset),
        std::vector<unsigned char>(bytes, bytes + sizeof(replacement)));
}

$execute {
    using namespace GamePatches;
#ifdef GEODE_IS_ANDROID64
    // fuck it we ball
    if (matches(point_copy_address, point_copy_original)) {
        (void)install(point_copy_address, point_copy_replacement);
    }
    if (matches(ring_clear_address, ring_clear_original)) {
        // +4 because only the cbz changes
        (void)install(ring_clear_address + 4, ring_clear_replacement);
    }
#endif
#ifdef GEODE_IS_WINDOWS
    if (matches(event_lookup_signature_address, event_lookup_original)) {
        (void)install(event_lookup_address, event_lookup_replacement);
    }
    if (matches(queue_move_address, queue_move_original) && matches(queue_move_cave_address, queue_move_cave_original)) {
        // cave goes in first or the call would land on int3s
        auto cave = install(queue_move_cave_address, queue_move_cave_replacement);
        if (cave && !install(queue_move_address, queue_move_replacement)) {
            // the call didnt go in, so take the cave back out
            (void)cave.unwrap()->disable();
        }
    }
#endif
}
