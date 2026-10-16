#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

template <std::size_t Size>
void restore_variance(std::array<float, Size>& live, const std::array<float, Size>& saved) {
	if (std::memcmp(live.data(), saved.data(), sizeof(live)) != 0 && live != saved) live = saved;
}

template <class Inputs, class Poses, class Ids, class Keep>
void compact_inputs(Inputs& inputs, Poses& poses, Ids& ids, Keep keep) {
	const auto size = inputs.size();
	const bool have_poses = poses.size() == size, have_ids = ids.size() == size;
	if (!have_poses) poses.clear();
	if (!have_ids) ids.clear();
	std::size_t out = 0;
	for (std::size_t i = 0; i < size; ++i) {
		if (!keep(inputs[i])) continue;
		if (out != i) {
			inputs[out] = inputs[i];
			if (have_poses) poses[out] = poses[i];
			if (have_ids) ids[out] = ids[i];
		}
		++out;
	}
	inputs.resize(out);
	if (have_poses) poses.resize(out);
	if (have_ids) ids.resize(out);
}

template <class Rates>
auto rate_after(const Rates& rates, int frame) {
	return std::upper_bound(rates.begin(), rates.end(), frame,
		[](int key, const auto& change) {
			return key < change.frame;
		});
}
