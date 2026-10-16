#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

template <std::size_t Size>
void restore_variance(std::array<float, Size>& live, const std::array<float, Size>& saved) {
	if (std::memcmp(live.data(), saved.data(), sizeof(live)) != 0 && live != saved)
		live = saved;
}

template <class T, std::size_t InlineSize>
class SmallSnapshot {
	std::array<T, InlineSize> local_{};
	std::vector<T> overflow_;
	std::size_t size_;
	bool overflowed_;

public:
	template <class Range>
	explicit SmallSnapshot(const Range& source)
		: size_(source.size()), overflowed_(size_ > InlineSize) {
		if (overflowed_)
			overflow_.assign(source.begin(), source.end());
		else
			std::copy(source.begin(), source.end(), local_.begin());
	}

	T* begin() {
		return overflowed_ ? overflow_.data() : local_.data();
	}

	T* end() {
		return begin() + size_;
	}

	std::size_t size() const {
		return size_;
	}

	T& operator[](std::size_t index) {
		return begin()[index];
	}

	template <class Predicate>
	void removeIf(Predicate predicate) {
		auto last = std::remove_if(begin(), end(), predicate);
		size_ = (std::size_t)(last - begin());
		if (overflowed_)
			overflow_.resize(size_);
	}
};

template <class Inputs, class Poses, class Ids, class Keep>
void compact_inputs(Inputs& inputs, Poses& poses, Ids& ids, Keep keep) {
	const auto size = inputs.size();
	const bool have_poses = poses.size() == size, have_ids = ids.size() == size;
	if (!have_poses)
		poses.clear();
	if (!have_ids)
		ids.clear();
	std::size_t out = 0;
	for (std::size_t i = 0; i < size; ++i) {
		if (!keep(inputs[i]))
			continue;
		if (out != i) {
			inputs[out] = inputs[i];
			if (have_poses)
				poses[out] = poses[i];
			if (have_ids)
				ids[out] = ids[i];
		}
		++out;
	}
	inputs.resize(out);
	if (have_poses)
		poses.resize(out);
	if (have_ids)
		ids.resize(out);
}

struct PrefixCursor {
	std::size_t after = 0;
	int frame = 0;
	bool known = false;

	template <class Frames>
	std::size_t seek(const Frames& frames, int next) {
		if (!known || next < frame || after > frames.size()) {
			after = (std::size_t)(std::upper_bound(frames.begin(), frames.end(), next) - frames.begin());
		}
		else if (frames.size() - after > 8 && frames[after + 8] <= next) {
			after = (std::size_t)(std::upper_bound(
												 frames.begin() + (std::ptrdiff_t)after, frames.end(), next) -
											 frames.begin());
		}
		else {
			while (after < frames.size() && frames[after] <= next)
				++after;
		}
		frame = next;
		known = true;
		return after;
	}
};

template <class Rates>
auto rate_after(const Rates& rates, int frame) {
	return std::upper_bound(rates.begin(), rates.end(), frame,
		[](int key, const auto& change) {
			return key < change.frame;
		});
}
