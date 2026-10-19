#include <Geode/Geode.hpp>
#ifdef GEODE_IS_IOS
#include "Logic/logic.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace geode::prelude;

// ios 2.2081, offsets from the loaded game image
// step count: same 8 instructions as arm mac (mac_patches.hpp) at 0x1fe730, count is in w21 not w20
// no jit (Loader::isPatchless) = you CANNOT write game code at runtime, ios kills the whole page & every level crashed
// so there its a GEODE_MOD_STATIC_PATCH, the launcher writes it into the binary before launch
// static = same bytes every launch so it cant hold an address of mine
// the count sits in the empty space after the last __DATA section instead (__common ends at 0x8b1778, the segment at 0x8b4000)
// nothing else uses that slot. it got blamed for a crash once & it wasnt it
const std::uintptr_t STEP_SITE = 0x1fe730, STEP_SLOT = 0x8b3ff0;
const std::array<unsigned, 8> STEP_ORIGINAL = {
    0x1e22c000, // fcvt d0, s0
    0x1e601920, // fdiv d0, d9, d0
    0x1e621002, // fmov d2, #4.0
    0x1e620800, // fmul d0, d0, d2
    0x1e624000, // fcvt s0, d0
    0x1e264000, // frinta s0, s0
    0x1e216800, // fmaxnm s0, s0, s1
    0x1e380015, // fcvtzs w21, s0
};
const std::array<unsigned, 8> STEP_REPLACEMENT = {
    0xb00035a8, // adrp x8, 0x8b3000
    0xb94ff115, // ldr w21, [x8, #0xff0]
    0x710006bf, // cmp w21, #1
    0x1a9fa6b5, // csinc w21, w21, wzr, ge
    0xd503201f,
    0xd503201f,
    0xd503201f,
    0xd503201f, // nop
};
static bool g_step_patched = false;
extern Hook* step_count_hook;
int vanilla_physics_steps(float dt, float time_warp);

static int* step_slot() {
    return reinterpret_cast<int*>(geode::base::get() + STEP_SLOT);
}

$execute {
    auto* site = reinterpret_cast<void*>(geode::base::get() + STEP_SITE);
    auto is = [site](const std::array<unsigned, 8>& words) {
        return std::memcmp(site, words.data(), sizeof(words)) == 0;
    };
    if (Loader::get()->isPatchless()) {
        GEODE_MOD_STATIC_PATCH(0x1fe730, {
                                             0xa8,
                                             0x35,
                                             0x00,
                                             0xb0,
                                             0x15,
                                             0xf1,
                                             0x4f,
                                             0xb9,
                                             0xbf,
                                             0x06,
                                             0x00,
                                             0x71,
                                             0xb5,
                                             0xa6,
                                             0x9f,
                                             0x1a,
                                             0x1f,
                                             0x20,
                                             0x03,
                                             0xd5,
                                             0x1f,
                                             0x20,
                                             0x03,
                                             0xd5,
                                             0x1f,
                                             0x20,
                                             0x03,
                                             0xd5,
                                             0x1f,
                                             0x20,
                                             0x03,
                                             0xd5,
                                         });
    }
    else if (is(STEP_ORIGINAL)) {
        std::vector<unsigned char> bytes(sizeof(STEP_REPLACEMENT));
        std::memcpy(bytes.data(), STEP_REPLACEMENT.data(), bytes.size());
        (void)Mod::get()->patch(site, bytes);
    }
    if (is(STEP_REPLACEMENT)) {
        g_step_patched = true;
        step_patched = true;
    }
    else if (is(STEP_ORIGINAL)) {
    }
    else {
    }
}

class $modify(EchoIosStepCount, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::Last);
        if (auto hook = self.getHook("GJBaseGameLayer::update")) step_count_hook = hook.unwrap();
    }

    void update(float dt) {
        if (g_step_patched)
            *step_slot() = Logic::bot_disabled() ? vanilla_physics_steps(dt, m_gameState.m_timeWarp) : Logic::get().steps_this_tick;
        GJBaseGameLayer::update(dt);
    }
};
#endif
