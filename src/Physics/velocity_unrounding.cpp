#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <array>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

// velocity unrounding, from chizz's mod (https://github.com/ch-zzzy/GDVelocityUnRounding)
// gd rounds the players y velocity to 3 decimals after a jump, a collision, a boost & postCollision. ts takes those out
// changes physics so the macro remembers if it was on (Logic::macro_unrounded)
#ifdef GEODE_IS_WINDOWS
// each block rounds xmm to 1/1000. nop it & the register keeps the real value
struct PatchSite {
    std::uintptr_t rva;
    std::size_t size;
};

const std::array SITES{
    PatchSite{ 0x38c329, 0x24 }, // updateJump
    PatchSite{ 0x213ea2, 0x32 }, // checkCollisions
    PatchSite{ 0x38dac7, 0x38 }, // postCollision
    PatchSite{ 0x39323b, 0x40 }, // collidedWithObjectInternal
    PatchSite{ 0x39ff18, 0x32 }, // boostPlayer
};
#endif
static std::vector<Patch*> patches;
static bool prepared = false;
static bool available = false;
static bool applied = false;

static void prepare() {
    if (prepared) return;
    prepared = true;
    if (Loader::get()->isModLoaded("chizz.velocity-unrounding")) {
        return;
    }
#ifdef GEODE_IS_WINDOWS
    auto base = geode::base::get();
    for (const auto& site : SITES) {
        auto res = Mod::get()->patch(reinterpret_cast<void*>(base + site.rva), ByteVector(site.size, 0x90));
        if (!res) {
            for (auto* patch : patches)
                (void)patch->disable();
            patches.clear();
            return;
        }
        auto* patch = res.unwrap();
        patch->setAutoEnable(false);
        (void)patch->disable();
        patches.push_back(patch);
    }
    available = true;
#endif
}

void Logic::sync_unrounding() {
    bool want = unrounded_physics();
    if (want == applied) return;
    prepare();
    if (!available) return;
    for (auto* patch : patches)
        (void)(want ? patch->enable() : patch->disable());
    applied = want;
}

bool Logic::unrounding_applied() {
    static bool other_mod = Loader::get()->isModLoaded("chizz.velocity-unrounding");
    return applied || other_mod;
}

class $modify(UnroundedPlayer, PlayerObject) {
    void setYVelocity(double velocity, int type) {
        if (applied) {
            m_yVelocity = velocity;
            return;
        }
        PlayerObject::setYVelocity(velocity, type);
    }
};
