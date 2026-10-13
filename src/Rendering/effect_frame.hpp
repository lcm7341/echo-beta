#pragma once
#include <array>
#include <cstddef>

// the same effect asked for again in 1 drawn frame (stacked orbs do ts a LOT)
// the 1st one plays, the rest get skipped
namespace echo::effects {
	// what makes 2 effects the same one: who its on, which effect & where
	struct Key {
		void const* owner;
		int effect, kind;
		float x = 0.f, y = 0.f;
		bool operator==(Key const&) const = default;
	};

	struct Frame {
		// 64 is plenty. past that they all just play
		std::array<Key, 64> seen{};
		std::size_t amt = 0;
		bool on = false;

		void clear() { amt = 0; }
		void begin() { clear(); on = true; }
		void end() { on = false; clear(); }

		// true = ts one already played ts frame
		bool repeated(Key const& key) {
			if (!on)
				return false;
			for (std::size_t i = 0; i < amt; ++i)
				if (seen[i] == key)
					return true;
			if (amt < seen.size())
				seen[amt++] = key;
			return false;
		}
	};
}
