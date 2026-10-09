#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <cstring>
#include <vector>
#include "obb_math.hpp"
#include "Rendering/circle_signatures.hpp"

using namespace geode::prelude;

// Moved objects recompute their oriented box every step they are collision-checked. The native
// routine spends most of its time in ~22 calls into libcocos2d's CCPoint operators. obb_math.hpp
// does the same float operations in the same order and operand order, with the game's own
// sinf/cosf, its 0.5f/1.0f constants and its corner-ordering helper, so every stored bit (and
// MXCSR flag) matches; tests/obb_math.py runs the installed routine against it.
namespace {
bool ready = false;
echo::obb::Trig nativeCos = nullptr, nativeSin = nullptr;
float half = 0.f, one = 0.f;
using Helper = void (*)(void*);
Helper nativeOrderCorners = nullptr;
}

// Replaces the routine at its entry, with its own calling convention (this in rcx, the center by
// reference in rdx, width/height in xmm2/xmm3, angle on the stack), so calls skip a hook dispatch.
void fastCalculateWithCenter(OBB2D* self, CCPoint const* center, float width, float height, float rotationAngle) {
    echo::obb::calculate(reinterpret_cast<std::uint8_t*>(self), {center->x, center->y}, width, height, rotationAngle,
                         nativeCos, nativeSin, half, one);
    nativeOrderCorners(self);
}

$execute {
    auto module = geode::base::get();
    // The routine past the prologue a hook rewrites.
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(module + 0x6da90), 0x30e) != 0xf20ffff8b9d467a8ull) {
        log::info("Echo fast oriented boxes skipped: native code differs");
        return;
    }
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && (hook->getAddress() == module + 0x6da80 || hook->getAddress() == module + 0x6dda0)) {
                log::info("Echo fast oriented boxes skipped: {} hooks OBB2D", mod->getID());
                return;
            }
    }
    // GD's sinf/cosf imports, as the routine's thunks at 0x4d1884/0x4d1860 jump through them.
    nativeSin = *reinterpret_cast<echo::obb::Trig*>(module + 0x515348);
    nativeCos = *reinterpret_cast<echo::obb::Trig*>(module + 0x5153d0);
    half = *reinterpret_cast<float const*>(module + 0x622b08);
    one = *reinterpret_cast<float const*>(module + 0x622c24);
    nativeOrderCorners = reinterpret_cast<Helper>(module + 0x6dda0);
    if (!nativeSin || !nativeCos || half != 0.5f || one != 1.f) return;
    // The entry must still be GD's own prologue: no hook has been placed on it.
    static constexpr std::uint8_t prologue[16]{0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                               0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55};
    if (std::memcmp(reinterpret_cast<void const*>(module + 0x6da80), prologue, sizeof(prologue)) != 0) {
        log::info("Echo fast oriented boxes skipped: the routine's entry is already patched");
        return;
    }
    // mov rax, imm64; jmp rax over the prologue (12 bytes, inside the unhooked entry).
    std::vector<std::uint8_t> entry{0x48, 0xb8};
    auto target = reinterpret_cast<std::uint64_t>(&fastCalculateWithCenter);
    for (int i = 0; i < 8; ++i) entry.push_back(static_cast<std::uint8_t>(target >> (8 * i)));
    entry.insert(entry.end(), {0xff, 0xe0});
    if (auto result = Mod::get()->patch(reinterpret_cast<void*>(module + 0x6da80), entry); !result) {
        log::warn("Echo fast oriented boxes patch failed: {}", result.unwrapErr());
        return;
    }
    ready = true;
    log::info("Echo fast oriented boxes enabled");
}
#endif
