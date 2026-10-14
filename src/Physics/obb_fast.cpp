#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <cstring>
#include <vector>
#include "obb_math.hpp"
#include "Rendering/circle_signatures.hpp"

using namespace geode::prelude;

// every object that moved redoes its rotated hitbox every step, & gds function for it is ~22 calls into
// cocos's CCPoint operators. mine (obb_math.hpp) does the same math in the same order w/ gds own sin, cos, 0.5 & 1.0
// so it comes out EXACTLY the same, just w/out all the calls
namespace {
    bool ready = false;
    echo::obb::Trig gd_cos = nullptr, gd_sin = nullptr;
    float half = 0.f, one = 0.f;
    using Helper = void (*)(void*);
    Helper gd_order_corners = nullptr;
}

// gds function jumps straight here. same arguments in the same places so nothing sits in between
// (this in rcx, center by ref in rdx, width/height in xmm2/xmm3, angle on the stack)
void my_calculate(OBB2D* self, CCPoint const* center, float width, float height, float rotationAngle) {
    echo::obb::calculate(reinterpret_cast<std::uint8_t*>(self), { center->x, center->y }, width, height, rotationAngle,
        gd_cos, gd_sin, half, one);
    gd_order_corners(self);
}

$execute {
    auto module = geode::base::get();
    // is ts the gd i wrote it for? (checked from after the 1st few bytes, a hook would have changed those)
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(module + 0x6da90), 0x30e) != 0xf20ffff8b9d467a8ull) {
        return;
    }
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get())
            continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && (hook->getAddress() == module + 0x6da80 || hook->getAddress() == module + 0x6dda0)) {
                return;
            }
    }
    // gds own sin & cos (what its thunks at 0x4d1884/0x4d1860 jump thru)
    gd_sin = *reinterpret_cast<echo::obb::Trig*>(module + 0x515348);
    gd_cos = *reinterpret_cast<echo::obb::Trig*>(module + 0x5153d0);
    half = *reinterpret_cast<float const*>(module + 0x622b08);         // 0.5f
    one = *reinterpret_cast<float const*>(module + 0x622c24);          // 1.0f
    gd_order_corners = reinterpret_cast<Helper>(module + 0x6dda0); // puts the corners in order after
    if (!gd_sin || !gd_cos || half != 0.5f || one != 1.f)
        return;
    // the 1st bytes are still robs = nobody else hooked it
    static constexpr std::uint8_t prologue[16]{ 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
        0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 };
    if (std::memcmp(reinterpret_cast<void const*>(module + 0x6da80), prologue, sizeof(prologue)) != 0) {
        return;
    }
    // mov rax, <my function>; jmp rax. 12 bytes, fits in those 1st bytes
    std::vector<std::uint8_t> entry{ 0x48, 0xb8 };
    auto target = reinterpret_cast<std::uint64_t>(&my_calculate);
    for (int i = 0; i < 8; ++i)
        entry.push_back(static_cast<std::uint8_t>(target >> (8 * i)));
    entry.insert(entry.end(), { 0xff, 0xe0 });
    if (auto res = Mod::get()->patch(reinterpret_cast<void*>(module + 0x6da80), entry); !res) {
        return;
    }
    ready = true;
}
#endif
