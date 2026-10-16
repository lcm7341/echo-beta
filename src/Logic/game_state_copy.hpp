#pragma once
#include <Geode/binding/GJGameState.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

// bindings have CCPoint unions in here so i cant just copy it :/
// calls visit(a.member, b.member) for every container in GJGameState
template <class A, class B, class Visit>
void each_container(A& a, B& b, Visit visit) {
	visit(a.m_spawnChannelRelated0, b.m_spawnChannelRelated0);
	visit(a.m_spawnChannelRelated1, b.m_spawnChannelRelated1);
	visit(a.m_tweenActions, b.m_tweenActions);
	visit(a.m_gameObjectPhysics, b.m_gameObjectPhysics);
	visit(a.m_unkVecFloat1, b.m_unkVecFloat1);
	visit(a.m_activatedObjectIDs, b.m_activatedObjectIDs);
	visit(a.m_unkMapPairGJGameEventIntVectorEventTriggerInstance, b.m_unkMapPairGJGameEventIntVectorEventTriggerInstance);
	visit(a.m_unkMapPairGJGameEventIntInt, b.m_unkMapPairGJGameEventIntInt);
	visit(a.m_enterEffectInstanceVectors, b.m_enterEffectInstanceVectors);
	visit(a.m_exitEffectInstanceVectors, b.m_exitEffectInstanceVectors);
	visit(a.m_enterChannelMap, b.m_enterChannelMap);
	visit(a.m_exitChannelMap, b.m_exitChannelMap);
	visit(a.m_moveEffectInstances, b.m_moveEffectInstances);
	visit(a.m_rotateEffectInstances, b.m_rotateEffectInstances);
	visit(a.m_scaleEffectInstances, b.m_scaleEffectInstances);
	visit(a.m_fadeEffectInstances, b.m_fadeEffectInstances);
	visit(a.m_tintEffectInstances, b.m_tintEffectInstances);
	visit(a.m_unsortedAreaEffects, b.m_unsortedAreaEffects);
	visit(a.m_advanceFollowInstances, b.m_advanceFollowInstances);
	visit(a.m_dynamicMoveActions, b.m_dynamicMoveActions);
	visit(a.m_dynamicRotateActions, b.m_dynamicRotateActions);
	visit(a.m_unkUMap8, b.m_unkUMap8);
	visit(a.m_proximityVolumeRelated, b.m_proximityVolumeRelated);
	visit(a.m_songChannelStates, b.m_songChannelStates);
	visit(a.m_songTriggerStateVectors, b.m_songTriggerStateVectors);
	visit(a.m_sfxTriggerStates, b.m_sfxTriggerStates);
}

// copies everything thats NOT a container
inline void copy_plain_bytes(void* dst, const GJGameState& src) {
	auto* base = reinterpret_cast<const std::byte*>(&src);
	std::array<std::pair<std::size_t, std::size_t>, 26> skip{};
	std::size_t n = 0;
	each_container(src, src, [&](const auto& member, const auto&) {
		skip[n++] = { (std::size_t)(reinterpret_cast<const std::byte*>(&member) - base), sizeof(member) };
	});
	std::sort(skip.begin(), skip.begin() + n);
	std::size_t at = 0;
	for (std::size_t i = 0; i < n; ++i) {
		std::memcpy(static_cast<std::byte*>(dst) + at, base + at, skip[i].first - at);
		at = skip[i].first + skip[i].second;
	}
	std::memcpy(static_cast<std::byte*>(dst) + at, base + at, sizeof(GJGameState) - at);
}

class SavedGameState {
public:
	explicit SavedGameState(const GJGameState& src) {
		copy_plain_bytes(m_storage, src);
		each_container(state(), src, [](auto& saved, const auto& from) {
			std::construct_at(&saved, from);
		});
	}

	~SavedGameState() {
		each_container(state(), state(), [](auto& saved, auto&) {
			std::destroy_at(&saved);
		});
	}

	SavedGameState(const SavedGameState&) = delete;
	SavedGameState& operator=(const SavedGameState&) = delete;

	void restore(GJGameState& dst) const {
		each_container(dst, state(), [](auto& to, const auto& saved) {
			to = saved;
		});
		copy_plain_bytes(&dst, state());
	}

private:
	GJGameState& state() {
		return *std::launder(reinterpret_cast<GJGameState*>(m_storage));
	}

	const GJGameState& state() const {
		return *std::launder(reinterpret_cast<const GJGameState*>(m_storage));
	}

	alignas(GJGameState) std::byte m_storage[sizeof(GJGameState)];
};
