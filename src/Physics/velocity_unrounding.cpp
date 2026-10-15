#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <array>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

// velocity unrounding, from chizz's mod (https://github.com/ch-zzzy/GDVelocityUnRounding)
// gd rounds the players y velocity to 3 decimals after a jump, a collision, a boost & postCollision. ts takes those out
// changes physics so the macro remembers if it was on (Session::macro_unrounded)
namespace {
#ifdef GEODE_IS_WINDOWS
    // each block rounds xmm to 1/1000. nop it & the register keeps the real value
    // bytes are hashed first
    struct Site {
        std::uintptr_t rva;
        std::size_t size;
        std::uint64_t fingerprint;
    };

    constexpr std::array SITES{
        Site{ 0x38c329, 0x24, 0xc4f7983894ccfe0bull }, // updateJump
        Site{ 0x213ea2, 0x32, 0xec65c38c7b82b7a9ull }, // checkCollisions
        Site{ 0x38dac7, 0x38, 0x7a9ae823fdb4a085ull }, // postCollision
        Site{ 0x39323b, 0x40, 0xbf27b9f4033918feull }, // collidedWithObjectInternal
        Site{ 0x39ff18, 0x32, 0x71836103b10abd48ull }, // boostPlayer
    };

    std::uint64_t fingerprint(std::uint8_t const* bytes, std::size_t size) {
        std::uint64_t hash = 14695981039346656037ull;
        for (std::size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        return hash;
    }
#endif
    std::vector<Patch*> patches;
    bool prepared = false;
    bool available = false;
    bool applied = false;

    void prepare() {
        if (prepared)
            return;
        prepared = true;
        if (Loader::get()->isModLoaded("chizz.velocity-unrounding")) {
            return;
        }
#ifdef GEODE_IS_WINDOWS
        auto base = geode::base::get();
        for (auto const& site : SITES) {
            if (fingerprint(reinterpret_cast<std::uint8_t const*>(base + site.rva), site.size) != site.fingerprint) {
                return;
            }
        }
        for (auto const& site : SITES) {
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
}

void Session::sync_unrounding() {
    bool want = unrounded_physics();
    if (want == applied)
        return;
    prepare();
    if (!available)
        return;
    for (auto* patch : patches)
        (void)(want ? patch->enable() : patch->disable());
    applied = want;
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
