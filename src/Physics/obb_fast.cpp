#include <Geode/Geode.hpp>
#include <cstring>
#include <vector>
#include "obb_math.hpp"
#include "Rendering/circle_signatures.hpp"

using namespace geode::prelude;

// moved objects redo their oriented box every step & gds one is ~22 calls into cocos's CCPoint operators
// obb_math.hpp does the same float ops in the same order w/ gds own sinf/cosf & constants
// so every bit comes out the same
namespace {
    bool ready = false;
    echo::obb::Trig native_cos = nullptr, native_sin = nullptr;
    float half = 0.f, one = 0.f;
    using Helper = void (*)(void*);
    Helper native_order_corners = nullptr;
}

// goes in at the entry w/ the same calling convention so theres no hook in the way
// (this in rcx, center by ref in rdx, width/height in xmm2/xmm3, angle on the stack)
void fast_calculate_with_center(OBB2D* self, CCPoint const* center, float width, float height, float rotationAngle) {
    echo::obb::calculate(reinterpret_cast<std::uint8_t*>(self), { center->x, center->y }, width, height, rotationAngle,
        native_cos, native_sin, half, one);
    native_order_corners(self);
}

$execute {
    auto module = geode::base::get();
    // hash starts after the prologue (a hook would rewrite that)
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
    // gds own sinf/cosf imports (what its thunks at 0x4d1884/0x4d1860 jump thru)
    native_sin = *reinterpret_cast<echo::obb::Trig*>(module + 0x515348);
    native_cos = *reinterpret_cast<echo::obb::Trig*>(module + 0x5153d0);
    half = *reinterpret_cast<float const*>(module + 0x622b08);         // 0.5f
    one = *reinterpret_cast<float const*>(module + 0x622c24);          // 1.0f
    native_order_corners = reinterpret_cast<Helper>(module + 0x6dda0); // the corner ordering one
    if (!native_sin || !native_cos || half != 0.5f || one != 1.f)
        return;
    // entry still robs prologue = nobody hooked it
    static constexpr std::uint8_t prologue[16]{ 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
        0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 };
    if (std::memcmp(reinterpret_cast<void const*>(module + 0x6da80), prologue, sizeof(prologue)) != 0) {
        return;
    }
    // mov rax, <fn>; jmp rax (12 bytes, fits inside the prologue)
    std::vector<std::uint8_t> entry{ 0x48, 0xb8 };
    auto target = reinterpret_cast<std::uint64_t>(&fast_calculate_with_center);
    for (int i = 0; i < 8; ++i)
        entry.push_back(static_cast<std::uint8_t>(target >> (8 * i)));
    entry.insert(entry.end(), { 0xff, 0xe0 });
    if (auto res = Mod::get()->patch(reinterpret_cast<void*>(module + 0x6da80), entry); !res) {
        return;
    }
    ready = true;
}
