#include "moving_objects.hpp"
#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

static bool ready = false;

namespace MovingObjects {
    void prepare() {
#ifdef GEODE_IS_WINDOWS
        ready = true;
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
        ++MovingObjects::group_epoch;
        PlayLayer::addToGroup(object, groupID, triggerGroup);
    }
};

class $modify(EchoMovingObjects, GJBaseGameLayer) {
    void addToGroup(GameObject* object, int groupID, bool triggerGroup) {
        ++MovingObjects::group_epoch;
        GJBaseGameLayer::addToGroup(object, groupID, triggerGroup);
    }

    void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
#ifdef GEODE_IS_WINDOWS
        if (ready && objects && Logic::as_play(this) && !Logic::get().predicting) {
            MovingObjects::move(this, objects, dx, dy, [](void* p) {
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
