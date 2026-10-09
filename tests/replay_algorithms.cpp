#include "Logic/replay_algorithms.hpp"

#include <cassert>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>

struct Input {
    int frame;
    bool player2;
    bool down;
    bool operator==(Input const&) const = default;
};
struct Rate { int frame; float fps; };

void regressions() {
    std::mt19937 rng(7341);
    for (int test = 0; test < 3000; ++test) {
        std::array<float, 32> live, saved;
        for (std::size_t i = 0; i < live.size(); ++i) {
            saved[i] = std::bit_cast<float>(static_cast<std::uint32_t>(rng()));
            live[i] = test % 2 ? saved[i] : std::bit_cast<float>(static_cast<std::uint32_t>(rng()));
        }
        live[0] = -0.f;
        saved[0] = 0.f;
        auto referenceVariance = live;
        if (referenceVariance != saved) referenceVariance = saved;
        replay::restoreVariance(live, saved);
        assert(std::memcmp(live.data(), referenceVariance.data(), sizeof(live)) == 0);
        std::vector<Input> inputs;
        std::vector<int> poses, ids;
        int n = static_cast<int>(rng() % 300);
        for (int i = 0; i < n; ++i) {
            inputs.push_back({static_cast<int>(rng() % 100) - 5, (rng() & 1) != 0, (rng() & 1) != 0});
            poses.push_back(i * 3);
            ids.push_back(i + 1);
        }
        if (test % 3 == 0) poses.clear();
        if (test % 7 == 0 && !ids.empty()) ids.pop_back();
        auto before = inputs;
        bool havePoses = poses.size() == inputs.size();
        bool haveIds = ids.size() == inputs.size();
        auto posesBefore = poses, idsBefore = ids;
        int frame = static_cast<int>(rng() % 100) - 5;
        bool player2 = (rng() & 1) != 0;
        bool both = (test % 5 == 0);
        auto keep = [&](Input const& input) {
            return (!both && input.player2 != player2) || (frame <= 0 ? input.frame < frame : input.frame <= frame);
        };
        std::vector<Input> expected;
        std::vector<int> expectedPoses, expectedIds;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (!keep(before[i])) continue;
            expected.push_back(before[i]);
            if (havePoses) expectedPoses.push_back(posesBefore[i]);
            if (haveIds) expectedIds.push_back(idsBefore[i]);
        }
        auto* data = inputs.data();
        auto capacity = inputs.capacity();
        replay::compact(inputs, poses, ids, keep);
        assert(inputs == expected && poses == expectedPoses && ids == expectedIds);
        assert(inputs.data() == data && inputs.capacity() == capacity);

        std::vector<int> frames;
        for (auto const& input : before) frames.push_back(input.frame);
        std::sort(frames.begin(), frames.end());
        replay::PrefixCursor cursor;
        for (int query = -10; query < 110; ++query) {
            auto reference = std::upper_bound(frames.begin(), frames.end(), query) - frames.begin();
            assert(cursor.seek(frames, query) == static_cast<std::size_t>(reference));
        }
        for (int q = 0; q < 100; ++q) {
            int query = static_cast<int>(rng() % 150) - 20;
            auto reference = std::upper_bound(frames.begin(), frames.end(), query) - frames.begin();
            assert(cursor.seek(frames, query) == static_cast<std::size_t>(reference));
        }
        // Appending at the cached frame must include every new same-frame event.
        cursor.seek(frames, 200);
        frames.insert(frames.end(), 3, 200);
        assert(cursor.seek(frames, 200) == frames.size());
        frames.clear();
        cursor = {};
        assert(cursor.seek(frames, 0) == 0);

        replay::SmallSnapshot<Input, 8> snapshot(before);
        snapshot.removeIf([&](Input const& input) { return !keep(input); });
        assert(snapshot.size() == expected.size());
        assert(std::equal(snapshot.begin(), snapshot.end(), expected.begin(), expected.end()));
        assert(before.size() == static_cast<std::size_t>(n));

        std::vector<Rate> rates;
        for (int i = 0; i < n; ++i) rates.push_back({i / 3, (i % 11 == 0) ? std::numeric_limits<float>::quiet_NaN() : float(i + 1)});
        for (int q : {-1, 0, n / 6, n, std::numeric_limits<int>::max()}) {
            float reference = 240;
            for (auto const& rate : rates) {
                if (rate.frame > q) break;
                if (std::isfinite(rate.fps) && rate.fps >= 1) reference = rate.fps;
            }
            float actual = 240;
            auto after = replay::rateAfter(rates, q);
            while (after != rates.begin()) {
                auto const& rate = *--after;
                if (std::isfinite(rate.fps) && rate.fps >= 1) { actual = rate.fps; break; }
            }
            assert(actual == reference);
        }
    }
    std::cout << "3000 randomized replay cases passed (variance bits, compaction, metadata, seeks, rewinds, appends, snapshots, TPS boundaries)\n";
}

template <class Work>
double measure(Work work) {
    auto start = std::chrono::steady_clock::now();
    auto result = work();
    auto end = std::chrono::steady_clock::now();
    // Consume the result outside the measured region so the search cannot disappear.
    std::cout << "checksum=" << result << " ";
    return std::chrono::duration<double, std::milli>(end - start).count();
}
void benchmark() {
    std::vector<int> frames(1000000);
    for (std::size_t i = 0; i < frames.size(); ++i) frames[i] = static_cast<int>(i * 2);
    auto old = measure([&] {
        std::uint64_t sum = 0;
        for (int frame = 0; frame < 2000000; ++frame)
            sum += std::upper_bound(frames.begin(), frames.end(), frame) - frames.begin();
        return sum;
    });
    auto next = measure([&] {
        std::uint64_t sum = 0;
        replay::PrefixCursor cursor;
        for (int frame = 0; frame < 2000000; ++frame) sum += cursor.seek(frames, frame);
        return sum;
    });
    std::cout << "hold lookup: " << old << " -> " << next << " ms\n";
    std::vector<Rate> rates;
    for (int i = 0; i < 10000; ++i) rates.push_back({i * 2, float(i + 1)});
    old = measure([&] {
        std::uint64_t sum = 0;
        for (int q = 0; q < 20000; ++q) {
            int last = 0;
            for (auto const& rate : rates) { if (rate.frame > q) break; last = rate.frame; }
            sum += last;
        }
        return sum;
    });
    next = measure([&] {
        std::uint64_t sum = 0;
        for (int q = 0; q < 20000; ++q) sum += (replay::rateAfter(rates, q) - 1)->frame;
        return sum;
    });
    std::cout << "TPS lookup: " << old << " -> " << next << " ms\n";
    // Match the production input/debug/id sizes when measuring checkpoint trimming.
    using Pose = std::array<double, 4>;
    std::vector<Input> inputs(1000000);
    std::vector<Pose> poses(inputs.size());
    std::vector<std::uint64_t> ids(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i] = {static_cast<int>(i), false, true};
    auto kept = inputs.size() / 2;
    old = measure([&] {
        std::vector<char> keep(inputs.size(), 1);
        for (std::size_t i = kept; i < keep.size(); ++i) keep[i] = 0;
        auto pack = [&](auto& data) {
            using Value = typename std::decay_t<decltype(data)>::value_type;
            std::vector<Value> next;
            next.reserve(kept);
            for (std::size_t i = 0; i < keep.size(); ++i) if (keep[i]) next.push_back(data[i]);
            data.swap(next);
        };
        pack(poses); pack(ids); pack(inputs);
        return inputs.size();
    });
    inputs.resize(1000000); poses.resize(1000000); ids.resize(1000000);
    for (std::size_t i = 0; i < inputs.size(); ++i) inputs[i] = {static_cast<int>(i), false, true};
    next = measure([&] {
        replay::compact(inputs, poses, ids, [kept](Input const& input) { return input.frame < static_cast<int>(kept); });
        return inputs.size();
    });
    std::cout << "checkpoint trim: " << old << " -> " << next << " ms; temporary buffers: "
              << (1000000 + kept * (sizeof(Input) + sizeof(Pose) + sizeof(std::uint64_t))) << " -> 0 bytes\n";
    std::cout << "initial input reserve: " << (1048576 * sizeof(Input)) << " -> " << (4096 * sizeof(Input)) << " bytes\n";
}
int main(int argc, char** argv) {
    regressions();
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark") benchmark();
}
