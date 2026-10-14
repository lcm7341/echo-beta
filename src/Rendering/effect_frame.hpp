#pragma once
#include <array>
#include <cstddef>

// the same effect again in 1 shown frame: the 1st plays & the rest are counted on its circle waves (EchoCircleWave)
namespace echo::effects {
	// what makes 2 effects the same one: who its on, which effect & where
	struct Key {
		void const* owner;
		int effect, kind;
		float x = 0.f, y = 0.f;
		bool operator==(Key const&) const = default;
	};

	struct Seen {
		Key key{};
		// the circle waves the 1st one made (CCCircleWave, held til the frame ends)
		std::array<void*, 4> waves{};
		int wave_amt = 0;
	};

	struct Frame {
		// 64 is plenty. past that they all just play
		std::array<Seen, 64> seen{};
		std::size_t amt = 0;
		bool on = false;
		// the one thats playing rn. a wave made while ts is set belongs to it
		Seen* open = nullptr;
		// the step the frame began on
		int began = 0;

		// nullptr = not on, or full. fresh = ts is the 1st time ts frame
		Seen* find(Key const& key, bool& fresh) {
			fresh = false;
			if (!on)
				return nullptr;
			for (std::size_t i = 0; i < amt; ++i)
				if (seen[i].key == key)
					return &seen[i];
			if (amt >= seen.size())
				return nullptr;
			fresh = true;
			seen[amt] = { key };
			return &seen[amt++];
		}
	};
}
