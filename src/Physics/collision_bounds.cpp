#include "collision_bounds.hpp"
#ifdef GEODE_IS_WINDOWS
#include <Geode/Geode.hpp>
#include "Rendering/circle_signatures.hpp"

using namespace geode::prelude;
namespace {
Patch* boundsPatch = nullptr;
bool gettersNative() {
    auto game = geode::base::get(), cocos = geode::base::getCocos();
    auto matches = [&](std::uintptr_t offset, std::uintptr_t import, auto const& body) {
        auto* bytes = reinterpret_cast<std::uint8_t const*>(cocos + offset);
        return std::memcmp(bytes, body.data(), body.size()) == 0 && bytes[body.size()] == 0xc3 &&
            *reinterpret_cast<std::uintptr_t const*>(game + import) == cocos + offset;
    };
    using namespace echo::collision_bounds;
    return matches(0xcc20, 0x516338, minX) && matches(0xcbb0, 0x516330, maxX) &&
        matches(0xcc30, 0x515e90, minY) && matches(0xcbc0, 0x515e88, maxY);
}
bool foreignHook() {
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && hook->getAddress() == geode::base::get() + 0x214960) return true;
    }
    return false;
}
}
namespace echo::collision_bounds {
void prepare() {
    if (!boundsPatch || (gettersNative() && !foreignHook())) return;
    if (auto result = boundsPatch->disable(); result) {
        boundsPatch = nullptr;
        log::info("Echo collision bounds inline stands aside for another hook");
    } else log::error("Echo could not restore native collision bounds: {}", result.unwrapErr());
}
}
$execute {
    using namespace echo::collision_bounds;
    auto address = geode::base::get() + site;
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(address), size) != fingerprint ||
        !gettersNative() || foreignHook()) {
        log::info("Echo collision bounds inline skipped: native code differs or another mod hooks it");
        return;
    }
    auto bytes = code(address, geode::base::get() + miss);
    auto result = Mod::get()->patch(reinterpret_cast<void*>(address), geode::ByteVector(bytes.begin(), bytes.end()));
    if (!result) {
        log::warn("Echo collision bounds inline failed: {}", result.unwrapErr());
        return;
    }
    boundsPatch = result.unwrap();
    log::info("Echo collision bounds inline installed");
}
#else
namespace echo::collision_bounds { void prepare() {} }
#endif
