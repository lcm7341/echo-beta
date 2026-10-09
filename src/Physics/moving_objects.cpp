#include "moving_objects.hpp"
#include "Rendering/circle_signatures.hpp"
#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

namespace {
    bool ready = false;
}

namespace echo::movement {
    void prepare() {
#ifdef GEODE_IS_WINDOWS
        static bool const supported = [] {
            auto base = geode::base::get();
            // gd code hash
            return echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(base + 0x22dd70), 0x160) == 0x98f38c9054902d57ull && // tf
                   echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(base + 0x227f70), 0xf7) == 0x36c19a0a704c9a34ull;    // tf
        }();
        ready = supported;
        if (!ready)
            return;
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get())
                continue;
            for (auto* hook : mod->getHooks()) {
                auto address = hook->getAddress();
                if (hook->isEnabled() && (address == geode::base::get() + 0x22dd50 || address == geode::base::get() + 0x227f50)) {
                    ready = false;
                    return;
                }
            }
        }
#endif
    }
}

class $modify(EchoGroupEpochPlay, PlayLayer) {
    void addToGroup(GameObject* object, int groupID, bool triggerGroup) {
        ++echo::movement::group_epoch;
        PlayLayer::addToGroup(object, groupID, triggerGroup);
    }
};

class $modify(EchoMovingObjects, GJBaseGameLayer) {
    void addToGroup(GameObject* object, int groupID, bool triggerGroup) {
        ++echo::movement::group_epoch;
        GJBaseGameLayer::addToGroup(object, groupID, triggerGroup);
    }

    void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
#ifdef GEODE_IS_WINDOWS
        if (ready && objects && Logic::as_play(this) && !Logic::get().predicting) {
            echo::movement::move(this, objects, dx, dy, [](void* p) {
                return static_cast<CCArray*>(p)->count();
            },
                [](void* p) {
                    auto* data = static_cast<CCArray*>(p)->data;
                    return std::pair{ data->arr, data->num };
                },
                [this](void* object) {
                    this->updateObjectSection(static_cast<GameObject*>(object));
                },
                [this](void* array) {
                    this->updateDisabledObjectsLastPos(static_cast<CCArray*>(array));
                });
            return;
        }
#endif
        GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
    }
};
