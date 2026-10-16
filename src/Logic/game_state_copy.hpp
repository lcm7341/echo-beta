#pragma once
#include <Geode/binding/GJGameState.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

// bindings have CCPoint unions in here so i cant just copy it :/
#define ECHO_GAME_STATE_CONTAINERS(X)                                                                            \
X(m_spawnChannelRelated0)                                                                                    \
X(m_spawnChannelRelated1)                                                                                    \
X(m_tweenActions)                                                                                            \
X(m_gameObjectPhysics)                                                                                       \
X(m_unkVecFloat1)                                                                                            \
X(m_activatedObjectIDs)                                                                                      \
X(m_unkMapPairGJGameEventIntVectorEventTriggerInstance)                                                      \
X(m_unkMapPairGJGameEventIntInt)                                                                             \
X(m_enterEffectInstanceVectors) X(m_exitEffectInstanceVectors)                                               \
	X(m_enterChannelMap) X(m_exitChannelMap) X(m_moveEffectInstances) X(m_rotateEffectInstances)             \
		X(m_scaleEffectInstances) X(m_fadeEffectInstances) X(m_tintEffectInstances) X(m_unsortedAreaEffects) \
			X(m_advanceFollowInstances) X(m_dynamicMoveActions) X(m_dynamicRotateActions) X(m_unkUMap8)      \
				X(m_proximityVolumeRelated) X(m_songChannelStates) X(m_songTriggerStateVectors) X(m_sfxTriggerStates)

template <class T>
using Member = std::remove_cvref_t<T>;

// copies everything thats NOT a container
inline void copy_plain_bytes(void* dst, const GJGameState& src) {
	auto* base = reinterpret_cast<const std::byte*>(&src);
	std::array<std::pair<std::size_t, std::size_t>, 26> skip{};
	std::size_t n = 0;
#define X(name) skip[n++] = { (std::size_t)(reinterpret_cast<const std::byte*>(&src.name) - base), sizeof(src.name) };
	ECHO_GAME_STATE_CONTAINERS(X)
#undef X
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
#define X(name) new (&state().name) Member<decltype(src.name)>(src.name);
		ECHO_GAME_STATE_CONTAINERS(X)
#undef X
	}

	~SavedGameState(){
#define X(name)                                           \
{                                                     \
	using T = Member<decltype(state().name)>; \
	state().name.~T();                                \
}
		ECHO_GAME_STATE_CONTAINERS(X)
#undef X
	} SavedGameState(const SavedGameState&) = delete;
	SavedGameState& operator=(const SavedGameState&) = delete;

	void restore(GJGameState& dst) const {
#define X(name) dst.name = state().name;
		ECHO_GAME_STATE_CONTAINERS(X)
#undef X
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

#undef ECHO_GAME_STATE_CONTAINERS
