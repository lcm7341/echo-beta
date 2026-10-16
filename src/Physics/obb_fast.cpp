#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <cstring>
#include <vector>
#include "obb_math.hpp"

using namespace geode::prelude;

// every object that moved redoes its rotated hitbox every step, & gds function for it is ~22 calls into
// cocos's CCPoint operators. mine (obb_math.hpp) does the same math in the same order w/ gds own sin, cos, 0.5 & 1.0
// so it comes out EXACTLY the same, just w/out all the calls
static bool ready = false;
static OBB::Trig gd_cos = nullptr, gd_sin = nullptr;
static float half = 0.f, one = 0.f;
using Helper = void (*)(void*);
static Helper gd_order_corners = nullptr;

void my_calculate(OBB2D* self, const CCPoint* center, float width, float height, float rotationAngle) {
    OBB::calculate(reinterpret_cast<unsigned char*>(self), { center->x, center->y }, width, height, rotationAngle,
        gd_cos, gd_sin, half, one);
    gd_order_corners(self);
}

$execute {
    auto module = geode::base::get();
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get())
            continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && (hook->getAddress() == module + 0x6da80 || hook->getAddress() == module + 0x6dda0)) {
                return;
            }
    }
    // gds own sin & cos (what its thunks at 0x4d1884/0x4d1860 jump thru)
    gd_sin = *reinterpret_cast<OBB::Trig*>(module + 0x515348);
    gd_cos = *reinterpret_cast<OBB::Trig*>(module + 0x5153d0);
    half = *reinterpret_cast<const float*>(module + 0x622b08);         // 0.5f
    one = *reinterpret_cast<const float*>(module + 0x622c24);          // 1.0f
    gd_order_corners = reinterpret_cast<Helper>(module + 0x6dda0); // puts the corners in order after
    if (!gd_sin || !gd_cos || half != 0.5f || one != 1.f)
        return;
    static constexpr unsigned char prologue[16]{ 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
        0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20, 0x55 };
    if (std::memcmp(reinterpret_cast<const void*>(module + 0x6da80), prologue, sizeof(prologue)) != 0) {
        return;
    }
    // mov rax, <my function>; jmp rax. 12 bytes, fits in those 1st bytes
    std::vector<unsigned char> entry{ 0x48, 0xb8 };
    auto target = reinterpret_cast<unsigned long long>(&my_calculate);
    for (int i = 0; i < 8; ++i)
        entry.push_back((unsigned char)(target >> (8 * i)));
    entry.insert(entry.end(), { 0xff, 0xe0 });
    if (auto res = Mod::get()->patch(reinterpret_cast<void*>(module + 0x6da80), entry); !res) {
        return;
    }
    ready = true;
}
#endif
