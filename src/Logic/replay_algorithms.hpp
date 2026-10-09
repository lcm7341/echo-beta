#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace replay {
// Seeded variance is normally bit-identical. Use the library's vectorized byte
// comparison first, retaining float equality for signed zeros and differing NaNs.
template <std::size_t Size>
void restoreVariance(std::array<float, Size>& live, std::array<float, Size> const& saved) {
    if (std::memcmp(live.data(), saved.data(), sizeof(live)) != 0 && live != saved) live = saved;
}

// Most physics steps carry only a press/release pair. Keep those snapshots on
// the stack while preserving arbitrary command counts for spam and stacked orbs.
template <class T, std::size_t InlineSize>
class SmallSnapshot {
    std::array<T, InlineSize> local_{};
    std::vector<T> overflow_;
    std::size_t size_;
    bool overflowed_;
public:
    template <class Range>
    explicit SmallSnapshot(Range const& source)
        : size_(source.size()), overflowed_(size_ > InlineSize) {
        if (overflowed_) overflow_.assign(source.begin(), source.end());
        else std::copy(source.begin(), source.end(), local_.begin());
    }
    T* begin() { return overflowed_ ? overflow_.data() : local_.data(); }
    T* end() { return begin() + size_; }
    std::size_t size() const { return size_; }
    T& operator[](std::size_t index) { return begin()[index]; }
    template <class Predicate>
    void removeIf(Predicate predicate) {
        auto last = std::remove_if(begin(), end(), predicate);
        size_ = static_cast<std::size_t>(last - begin());
        if (overflowed_) overflow_.resize(size_);
    }
};

// Compact aligned input metadata together, retaining allocation for the next attempt.
// Empty or incomplete side arrays follow the existing rule: discard them.
template <class Inputs, class Poses, class Ids, class Keep>
void compact(Inputs& inputs, Poses& poses, Ids& ids, Keep keep) {
    const auto size = inputs.size();
    const bool havePoses = poses.size() == size;
    const bool haveIds = ids.size() == size;
    if (!havePoses) poses.clear();
    if (!haveIds) ids.clear();
    std::size_t out = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (!keep(inputs[i])) continue;
        if (out != i) {
            inputs[out] = inputs[i];
            if (havePoses) poses[out] = poses[i];
            if (haveIds) ids[out] = ids[i];
        }
        ++out;
    }
    inputs.resize(out);
    if (havePoses) poses.resize(out);
    if (haveIds) ids.resize(out);
}

// Playback normally advances through a lane. Seek only on a rewind or initial
// lookup; incremental queries visit each event once, including same-frame events.
struct PrefixCursor {
    std::size_t after = 0;
    int frame = 0;
    bool known = false;

    template <class Frames>
    std::size_t seek(Frames const& frames, int next) {
        if (!known || next < frame || after > frames.size()) {
            after = static_cast<std::size_t>(
                std::upper_bound(frames.begin(), frames.end(), next) - frames.begin());
        } else if (frames.size() - after > 8 && frames[after + 8] <= next) {
            after = static_cast<std::size_t>(std::upper_bound(
                frames.begin() + static_cast<std::ptrdiff_t>(after), frames.end(), next) - frames.begin());
        } else {
            while (after < frames.size() && frames[after] <= next) ++after;
        }
        frame = next;
        known = true;
        return after;
    }
};

template <class Rates>
auto rateAfter(Rates const& rates, int frame) {
    return std::upper_bound(rates.begin(), rates.end(), frame,
        [](int key, auto const& change) { return key < change.frame; });
}
}
