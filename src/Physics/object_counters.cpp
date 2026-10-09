#include "object_counters.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cstring>

using namespace geode::prelude;

// GameObject::commonSetup (new objects) and GameObject::resetObject (every object, every level
// reset) give each object a variance index from their own LCG (GD 2.2081 Windows, base+0x6c2ee0;
// nothing else reads it). It is never reseeded, so the indexes depended on how many resets and
// objects came before: a practice restore gave objects different indexes than the start of the
// attempt had, and moving objects with random variance went elsewhere in a replay of the same
// inputs. commonSetup also hands out unique IDs from a global counter (base+0x6ba158).
//
// Echo starts that LCG from a fixed state before every level load and reset, and keeps checkpoint
// creation (practice checkpoints and backstep saves both build a checkpoint object) from using up
// an ID or a variance draw. Then a run with checkpoints matches a continuous one.
namespace {
#ifdef GEODE_IS_WINDOWS
// Resolves a rip-relative operand and checks it is the expected data address.
template <class T>
T* resolve(std::uintptr_t site, std::size_t operand, std::size_t length, std::initializer_list<std::uint8_t> prefix, std::uintptr_t expected) {
	auto const* code = reinterpret_cast<std::uint8_t const*>(geode::base::get() + site);
	std::size_t i = 0;
	for (auto byte : prefix) {
		if (code[i++] != byte) return nullptr;
	}
	std::int32_t rel = 0;
	std::memcpy(&rel, code + operand, 4);
	auto target = reinterpret_cast<std::uintptr_t>(code + length) + rel;
	if (target != geode::base::get() + expected) return nullptr;
	return reinterpret_cast<T*>(target);
}
#endif
constexpr std::uint64_t kVarianceSeed = 0;
}

namespace echo::objects {
int* uniqueIdCounter() {
#ifdef GEODE_IS_WINDOWS
	// commonSetup+0x16b: mov eax, [rip+rel32]
	static int* const counter = resolve<int>(0x18de5b, 2, 6, {0x8b, 0x05}, 0x6ba158);
	return counter;
#else
	return nullptr;
#endif
}

std::uint64_t* varianceGenerator() {
#ifdef GEODE_IS_WINDOWS
	// commonSetup+0xfc: imul rax, [rip+rel32], 0x343fd
	static std::uint64_t* const generator = resolve<std::uint64_t>(0x18ddec, 3, 11, {0x48, 0x69, 0x05}, 0x6c2ee0);
	return generator;
#else
	return nullptr;
#endif
}

void reseedVariance() {
	if (auto* generator = varianceGenerator()) *generator = kVarianceSeed;
}
}

class $modify(CheckpointCounters, PlayLayer) {
	CheckpointObject* createCheckpoint() {
		auto* ids = echo::objects::uniqueIdCounter();
		auto* variance = echo::objects::varianceGenerator();
		int id = ids ? *ids : 0;
		std::uint64_t draw = variance ? *variance : 0;
		auto* checkpoint = PlayLayer::createCheckpoint();
		if (ids) *ids = id;
		if (variance) *variance = draw;
		return checkpoint;
	}
};
