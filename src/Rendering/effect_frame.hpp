#pragma once
#include <array>
#include <cstddef>

// the same effect again in 1 shown frame: the 1st plays & the rest are counted on its circle waves (EchoCircleWave)
struct EffectKey {
	const void* owner;
	int effect, kind;
	float x = 0.f, y = 0.f;
	bool operator==(const EffectKey&) const = default;
};

struct EffectSeen {
	EffectKey key{};
	std::array<void*, 4> waves{};
	int wave_amt = 0;
	float first_x = 0.f, first_y = 0.f;
	bool has_first = false;
	int last_step = 0, plays = 0;
};

struct EffectFrame {
	std::array<EffectSeen, 64> seen{};
	std::size_t amt = 0;
	bool on = false;
	EffectSeen* open = nullptr;
	int began = 0;

	EffectSeen* find(const EffectKey& key, bool& fresh) {
		fresh = false;
		if (!on) return nullptr;
		EffectSeen* free = nullptr;
		for (std::size_t i = 0; i < amt; ++i) {
			if (seen[i].key == key) return &seen[i];
			if (!free && !seen[i].key.owner) free = &seen[i];
		}
		if (free) {
			fresh = true;
			*free = { key };
			return free;
		}
		if (amt >= seen.size()) return nullptr;
		fresh = true;
		seen[amt] = { key };
		return &seen[amt++];
	}
};
