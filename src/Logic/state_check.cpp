#include "Logic/state_check.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/EnterEffectInstance.hpp>
#include <Geode/binding/GJGameState.hpp>

#ifdef GEODE_IS_WINDOWS
#include "Logic/state_check_walk.hpp"

template <class T>
static bool vector_ok(const std::vector<T>& items) {
    const auto* raw = reinterpret_cast<const std::uintptr_t*>(&items);
    auto first = raw[0], last = raw[1], end = raw[2];
    if (!first) return !last && !end;
    return (first & 7) == 0 && first <= last && last <= end && (last - first) % sizeof(T) == 0;
}

static bool effects_ok(const std::vector<EnterEffectInstance>& items) {
    if (!vector_ok(items)) return false;
    for (const auto& item : items) {
        if (!tree_ok(item.m_enterEffectAnimMap) || !vector_ok(item.m_easeIndices)) return false;
    }
    return true;
}

template <class Table>
static bool effect_lists_ok(const Table& table) {
    if (!list_ok(table)) return false;
    for (const auto& [key, items] : table) {
        if (!effects_ok(items)) return false;
    }
    return true;
}

bool game_state_intact(const GJGameState& state) {
    if (!list_ok(state.m_spawnChannelRelated0)) return false;
    if (!list_ok(state.m_spawnChannelRelated1)) return false;
    if (!list_ok(state.m_tweenActions)) return false;
    if (!list_ok(state.m_gameObjectPhysics)) return false;
    if (!tree_ok(state.m_activatedObjectIDs)) return false;
    if (!list_ok(state.m_stateObjects)) return false;
    if (!tree_ok(state.m_unkMapPairGJGameEventIntVectorEventTriggerInstance)) return false;
    if (!tree_ok(state.m_unkMapPairGJGameEventIntInt)) return false;
    if (!effect_lists_ok(state.m_enterEffectInstanceVectors)) return false;
    if (!effect_lists_ok(state.m_exitEffectInstanceVectors)) return false;
    if (!effects_ok(state.m_moveEffectInstances)) return false;
    if (!effects_ok(state.m_rotateEffectInstances)) return false;
    if (!effects_ok(state.m_scaleEffectInstances)) return false;
    if (!effects_ok(state.m_fadeEffectInstances)) return false;
    if (!effects_ok(state.m_tintEffectInstances)) return false;
    if (!list_ok(state.m_unsortedAreaEffects)) return false;
    if (!list_ok(state.m_unkUMap8)) return false;
    if (!tree_ok(state.m_proximityVolumeRelated)) return false;
    if (!list_ok(state.m_songChannelStates)) return false;
    if (!list_ok(state.m_songTriggerStateVectors)) return false;
    return true;
}

#else
bool game_state_intact(const GJGameState&) {
    return true;
}
#endif
