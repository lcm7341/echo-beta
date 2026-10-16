#pragma once
#include <array>
#include <cstddef>

// the same effect again in 1 shown frame: the 1st plays & the rest are counted on its circle waves (EchoCircleWave)
struct EffectKey {
	const void* owner;
	int effect, type;
	float x = 0.f, y = 0.f;
	bool operator==(const EffectKey&) const = default;
};

struct EffectRecord {
	EffectKey key{};
	std::array<void*, 4> waves{};
	int wave_count = 0;
	float first_x = 0.f, first_y = 0.f;
	bool has_first = false;
	int last_step = 0, play_count = 0;
};

struct EffectFrame {
	std::array<EffectRecord, 64> seen{};
	std::size_t count = 0;
	bool active = false;
	EffectRecord* current = nullptr;
	int started_step = 0;

	EffectRecord* find(const EffectKey& key, bool& fresh) {
		fresh = false;
		if (!active) return nullptr;
		EffectRecord* free = nullptr;
		for (std::size_t i = 0; i < count; ++i) {
			if (seen[i].key == key) return &seen[i];
			if (!free && !seen[i].key.owner) free = &seen[i];
		}
		if (free) {
			fresh = true;
			*free = { key };
			return free;
		}
		if (count >= seen.size()) return nullptr;
		fresh = true;
		seen[count] = { key };
		return &seen[count++];
	}
};
