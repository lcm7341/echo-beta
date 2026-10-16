// macros that came from silicate expect silicates variance numbering (seed + group + index), ts reproduces it for them
#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/AdvancedFollowInstance.hpp>
#include <Geode/binding/EnterEffectInstance.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <cmath>
#include <cstdint>

using namespace geode::prelude;

// nat GROW TF UP BRO
static short variance_index(unsigned long long seed, GameObject* object, int group, int index) {
    auto rotation = (int)((unsigned)((int)(std::fabs(object->m_startRotationX) * 6.7 + 1.0)) *
                                              (unsigned)(index + 67));
    unsigned long long value = seed;
    value *= (unsigned long long)((long long)((int)(object->m_objectID + 5541)));
    value *= (unsigned long long)((long long)((int)(std::fabs(object->m_startPosition.x) + 1.0)));
    value *= (unsigned long long)((long long)((int)(std::fabs(object->m_startPosition.y) + 1.0)));
    value *= (unsigned long long)((long long)((int)object->m_zLayer + 2137));
    value *= (unsigned long long)((long long)(object->m_groupCount + 6969));
    value *= (unsigned long long)((long long)((int)object->m_isObjectBlack * 420 + 14));
    value *= (unsigned long long)((long long)rotation);
    value *= (unsigned long long)((long long)(group + 2137));
    return (short)(value % 1777);
}

class $modify(SilicateVarianceLayer, GJBaseGameLayer) {
    void processMoveActionsStep(float dt, bool visibleFrame) {
        const auto& logic = Logic::get();
        if (logic.macro_silicate_variance && (logic.is_playing() || logic.is_recording()) && !logic.predicting) {
            unsigned long long seed = logic.lvl_seed_known ? logic.lvl_seed : 0;
            auto stamp = [&](int group) {
                auto* objects = getGroup(group);
                if (!objects)
                    return;
                for (int i = 0; i < (int)objects->count(); ++i) {
                    if (auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i)))
                        obj->m_varianceIndex = variance_index(seed, obj, group, i);
                }
            };
            for (auto& instance : m_gameState.m_advanceFollowInstances)
                stamp(instance.m_group);
            for (auto& instance : m_gameState.m_rotateEffectInstances)
                stamp(instance.m_targetID);
            for (auto& instance : m_gameState.m_scaleEffectInstances)
                stamp(instance.m_targetID);
            for (auto& instance : m_gameState.m_moveEffectInstances)
                stamp(instance.m_targetID);
        }
        GJBaseGameLayer::processMoveActionsStep(dt, visibleFrame);
    }
};
