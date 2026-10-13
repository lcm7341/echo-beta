#pragma once

// switches for the bigger speedups, so a trace can show what each one is actually worth
// they are saved w/ the other settings & the step trace writes the settings at the start of every attempt,
// so 2 traces of the same macro w/ 1 switch flipped can be compared line by line
namespace echo::opt {
	inline bool prefetch = true; // the collision loops ask for the next objects memory early (collision_prefetch.cpp)
	inline bool getters = true;  // the 4 rect getters pasted into the collision check (collision_bounds.cpp)
	inline bool effects = true;  // stacked orbs: only the 1st ring, dash start & dash end on a step show their effect (hooks.cpp)
}

namespace echo::prefetch {
	void prepare();
}
