#include "collision_bounds.hpp"
#include <Geode/Geode.hpp>
#include "Rendering/circle_signatures.hpp"

using namespace geode::prelude;

namespace {
    Patch* bounds_patch = nullptr;

    // i paste cocos's getters into the game, ONLY while theyre the original bytes & the imports still point at them
    bool getters_native() {
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

    // another mod hooking the function we patched into
    bool foreign_hook() {
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get())
                continue;
            for (auto* hook : mod->getHooks())
                if (hook->isEnabled() && hook->getAddress() == geode::base::get() + 0x214960) // tf
                    return true;
        }
        return false;
    }
}

namespace echo::collision_bounds {
    // another mod hooked it after me = take mine out & let theirs run
    void prepare() {
        if (!bounds_patch || (getters_native() && !foreign_hook()))
            return;
        if (auto res = bounds_patch->disable(); res) {
            bounds_patch = nullptr;
        }
    }
}

$execute {
    using namespace echo::collision_bounds;
    auto address = geode::base::get() + site;
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(address), size) != fingerprint ||
        !getters_native() || foreign_hook()) {
        return;
    }
    auto bytes = code(address, geode::base::get() + miss);
    auto res = Mod::get()->patch(reinterpret_cast<void*>(address), geode::ByteVector(bytes.begin(), bytes.end()));
    if (!res) {
        return;
    }
    bounds_patch = res.unwrap();
}
