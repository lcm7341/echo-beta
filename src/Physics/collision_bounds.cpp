#include "collision_bounds.hpp"
#include "Logic/logic.hpp"
#ifdef GEODE_IS_WINDOWS
#include <Geode/Geode.hpp>

using namespace geode::prelude;

static Patch* bounds_patch = nullptr;

#define COCOS_GET_MIN_X 0xcc20
#define COCOS_GET_MAX_X 0xcbb0
#define COCOS_GET_MIN_Y 0xcc30
#define COCOS_GET_MAX_Y 0xcbc0
#define GD_IMPORT_MIN_X 0x516338
#define GD_IMPORT_MAX_X 0x516330
#define GD_IMPORT_MIN_Y 0x515e90
#define GD_IMPORT_MAX_Y 0x515e88
#define COLLISION_CHECK_OBJECTS 0x214960

// i paste cocos's getters into the game, ONLY while theyre the original bytes & the imports still point at them
static bool cocos_getters_unchanged() {
    auto game = geode::base::get(), cocos = geode::base::getCocos();
    auto unchanged = [&](std::uintptr_t getter, std::uintptr_t import, const auto& code) {
        auto* bytes = reinterpret_cast<const unsigned char*>(cocos + getter);
        return std::memcmp(bytes, code.data(), code.size()) == 0 && bytes[code.size()] == 0xc3 &&
               *reinterpret_cast<const std::uintptr_t*>(game + import) == cocos + getter;
    };
    using namespace CollisionBounds;
    return unchanged(COCOS_GET_MIN_X, GD_IMPORT_MIN_X, min_x_code) && unchanged(COCOS_GET_MAX_X, GD_IMPORT_MAX_X, max_x_code) &&
           unchanged(COCOS_GET_MIN_Y, GD_IMPORT_MIN_Y, min_y_code) && unchanged(COCOS_GET_MAX_Y, GD_IMPORT_MAX_Y, max_y_code);
}

static bool other_mod_hooks_collisions() {
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && hook->getAddress() == geode::base::get() + COLLISION_CHECK_OBJECTS) // tf
                return true;
    }
    return false;
}

namespace CollisionBounds {
    void prepare() {
        if (!bounds_patch) return;
        if (!cocos_getters_unchanged() || other_mod_hooks_collisions()) {
            if (auto disabled = bounds_patch->disable(); disabled) {
                bounds_patch = nullptr;
            }
            return;
        }
        if (!Logic::bot_disabled() && !bounds_patch->isEnabled()) (void)bounds_patch->enable();
    }
}

$execute {
    using namespace CollisionBounds;
    auto address = geode::base::get() + BOUNDS_CHECK_SITE;
    if (!cocos_getters_unchanged() || other_mod_hooks_collisions()) return;
    auto bytes = build_patch(address, geode::base::get() + NOT_TOUCHING);
    auto patched = Mod::get()->patch(reinterpret_cast<void*>(address), geode::ByteVector(bytes.begin(), bytes.end()));
    if (!patched) return;
    bounds_patch = patched.unwrap();
}

#else
namespace CollisionBounds {
    void prepare() {
    }
}
#endif
