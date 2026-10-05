#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace echo::nodes {
	struct ChildKey {
		int z;
		unsigned arrival;
		void* node;
	};

	inline bool before(ChildKey const& a, ChildKey const& b) {
		return a.z < b.z || (a.z == b.z && a.arrival < b.arrival);
	}

	template <class ZOrder, class Arrival>
	bool stable_sort_children(void** nodes, unsigned count, ZOrder zOrder, Arrival arrival) {
		if (count < 2)
			return false;
		static std::vector<ChildKey> keys;
		keys.resize(count);
		for (unsigned i = 0; i < count; ++i)
			keys[i] = { zOrder(nodes[i]), arrival(nodes[i]), nodes[i] };
		unsigned sorted = 1;
		while (sorted < count && !before(keys[sorted], keys[sorted - 1]))
			++sorted;
		if (sorted == count)
			return false;
		std::stable_sort(keys.begin() + sorted, keys.end(), before);
		std::inplace_merge(keys.begin(), keys.begin() + sorted, keys.end(), before);
		for (unsigned i = 0; i < count; ++i)
			nodes[i] = keys[i].node;
		return true;
	}
}
