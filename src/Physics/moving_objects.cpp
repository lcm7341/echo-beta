#include "moving_objects.hpp"
#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

static bool no_other_hooks = false;
#define MOVE_OBJECTS_ADDRESS 0x22dd50
#define UPDATE_OBJECT_SECTION_ADDRESS 0x227f50

namespace MovingObjects {
    void prepare() {
#ifdef GEODE_IS_WINDOWS
        no_other_hooks = true;
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get()) continue;
            for (auto* hook : mod->getHooks()) {
                auto address = hook->getAddress();
                if (hook->isEnabled() && (address == geode::base::get() + MOVE_OBJECTS_ADDRESS || address == geode::base::get() + UPDATE_OBJECT_SECTION_ADDRESS)) {
                    no_other_hooks = false;
                    return;
                }
            }
        }
#endif
    }
}

class $modify(EchoGroupVersionPlay, PlayLayer) {
    void addToGroup(GameObject* object, int groupID, bool triggerGroup) {
        ++MovingObjects::group_version;
        PlayLayer::addToGroup(object, groupID, triggerGroup);
    }
};

class $modify(EchoMovingObjects, GJBaseGameLayer) {
    void addToGroup(GameObject* object, int groupID, bool triggerGroup) {
        ++MovingObjects::group_version;
        GJBaseGameLayer::addToGroup(object, groupID, triggerGroup);
    }

    void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
#ifdef GEODE_IS_WINDOWS
        if (no_other_hooks && objects && Logic::as_play(this)) {
            MovingObjects::move(this, objects, dx, dy, [](void* array) {
                return static_cast<CCArray*>(array)->count();
            },
                [](void* array) {
                    auto* data = static_cast<CCArray*>(array)->data;
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
