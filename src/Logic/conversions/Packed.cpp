// the .echo format
//
// a click is mostly predictable from the clicks before it, so the file stores how SURPRISING each one was, not the click itself
// range coder: every bit is coded against odds that adapt as bits go by. reader & writer have to update them identically or it desyncs
// an event = which channel (player + button), hold or release, & the gap in frames since the last one
//   repeats are found by hashing the last few events (hash_tail). if the same thing followed last time, that costs almost nothing
// versions: 1 = bit packed, 2 = range coder w/ simple odds, 3 = ts, 4 = 3 + tps changes. old ones still load, only 3 & 4 get written
// the flags byte says what else is in the file
#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <vector>

namespace fs = std::filesystem;

using namespace geode::prelude;

struct BitReader {
    uint8_t const* data = nullptr;
    size_t size = 0;
    size_t index = 0;
    int left = 0;
    uint8_t acc = 0;

    int bit() {
        if (left == 0) {
            if (index >= size)
                return 0;
            acc = data[index++];
            left = 8;
        }
        int value = (acc >> 7) & 1;
        acc <<= 1;
        --left;
        return value;
    }

    uint32_t gamma() {
        uint32_t zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31 || index > size)
                return 0;
        }
        uint32_t n = 1;
        for (uint32_t i = 0; i < zeros; ++i)
            n = (n << 1) | static_cast<uint32_t>(bit());
        return n;
    }
};

struct Odds {
    uint16_t value = 1 << 15;
    uint8_t seen = 0;

    uint32_t chance() const {
        return std::clamp<uint32_t>(value, 64, 65535 - 64);
    }

    void learn(int bit) {
        int target = bit ? 0 : 65535;
        value = static_cast<uint16_t>(value + (target - static_cast<int>(value)) / (static_cast<int>(seen) + 2));
        if (seen < 10)
            ++seen;
    }
};

struct Tally {
    uint32_t one = 1u << 31;
    uint16_t seen = 0;

    uint32_t zero16() const {
        return 65536 - std::clamp<uint32_t>(one >> 16, 1, 65535);
    }

    int one12() const {
        return static_cast<int>(one >> 20);
    }

    void learn(int bit, int cap) {
        int64_t target = bit ? 0xFFFFFFFFll : 0;
        one = static_cast<uint32_t>(one + (target - static_cast<int64_t>(one)) / (seen + 2));
        if (seen < cap)
            ++seen;
    }
};

constexpr int QUICK = 30;
constexpr int STEADY = 4000;

struct RangeWriter {
    std::vector<uint8_t> bytes;
    uint64_t low = 0;
    uint32_t range = 0xFFFFFFFF; // full range
    uint8_t cache = 0;
    uint64_t pending = 1;
    bool first = true;
    bool failed = false;

    void shift_low() {
        if (static_cast<uint32_t>(low) < 0xFF000000u || (low >> 32) != 0) {
            uint8_t carry = static_cast<uint8_t>(low >> 32);
            uint8_t out = cache;
            do {
                if (!first)
                    bytes.push_back(static_cast<uint8_t>(out + carry));
                first = false;
                out = 0xFF;
            } while (--pending != 0);
            cache = static_cast<uint8_t>(low >> 24);
        }
        ++pending;
        low = (low & 0x00FFFFFF) << 8;
    }

    int bit(uint32_t zero, int value) {
        uint32_t bound = (range >> 16) * zero;
        if (value) {
            low += bound;
            range -= bound;
        }
        else {
            range = bound;
        }
        while (range < (1u << 24)) {
            range <<= 8;
            shift_low();
        }
        return value;
    }

    int code(Tally& tally, int value, int cap = QUICK) {
        bit(tally.zero16(), value);
        tally.learn(value, cap);
        return value;
    }

    void finish() {
        for (int zeros = 32; zeros > 0; --zeros) {
            uint64_t mask = (uint64_t{ 1 } << zeros) - 1;
            uint64_t rounded = (low + mask) & ~mask;
            if (rounded < low + range) {
                low = rounded;
                break;
            }
        }
        for (int i = 0; i < 5; ++i)
            shift_low();
        while (!bytes.empty() && bytes.back() == 0)
            bytes.pop_back();
    }
};

struct RangeReader {
    uint8_t const* data = nullptr;
    size_t size = 0;
    size_t index = 0;
    uint32_t range = 0xFFFFFFFF; // full range
    uint32_t value = 0;
    bool failed = false;

    uint8_t next() {
        if (index >= size) {
            if (++index > size + 64)
                failed = true;
            return 0;
        }
        return data[index++];
    }

    void start() {
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | next();
    }

    int bit(uint32_t zero, int) {
        uint32_t bound = (range >> 16) * zero;
        int res = value >= bound ? 1 : 0;
        if (res) {
            value -= bound;
            range -= bound;
        }
        else {
            range = bound;
        }
        while (range < (1u << 24)) {
            range <<= 8;
            value = (value << 8) | next();
        }
        return res;
    }

    int code(Odds& odds, int) {
        int res = bit(odds.chance(), 0);
        odds.learn(res);
        return res;
    }

    int code(Tally& tally, int, int cap = QUICK) {
        int res = bit(tally.zero16(), 0);
        tally.learn(res, cap);
        return res;
    }
};

template <int SlotBits, int MaxSlot, class Cell = Odds>
struct NumberOdds {
    std::array<Cell, 1 << SlotBits> slot{};
    std::array<std::array<Cell, 16>, MaxSlot + 1> high{};
    std::array<std::array<Cell, MaxSlot>, MaxSlot + 1> low{};
};

template <class Coder, int SlotBits, int MaxSlot, class Cell>
uint64_t code_number(Coder& coder, NumberOdds<SlotBits, MaxSlot, Cell>& odds, uint64_t number) {
    int len = std::bit_width(number);
    int node = 1;
    for (int i = SlotBits - 1; i >= 0; --i)
        node = (node << 1) | coder.code(odds.slot[node], (len >> i) & 1);
    len = node - (1 << SlotBits);
    if (len > MaxSlot) {
        coder.failed = true;
        return 0;
    }
    if (len <= 1)
        return static_cast<uint64_t>(len);
    uint64_t res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int wanted = static_cast<int>((number >> i) & 1);
        int bit = depth < 4 ? coder.code(odds.high[len][tree], wanted) : coder.code(odds.low[len][i], wanted);
        if (depth < 4)
            tree = (tree << 1) | bit;
        res = (res << 1) | static_cast<uint64_t>(bit);
    }
    return res;
}

uint64_t zigzag(int64_t value) {
    return (static_cast<uint64_t>(value) << 1) ^ static_cast<uint64_t>(value >> 63);
}

int64_t unzigzag(uint64_t value) {
    return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1);
}

template <class Coder, class Numbers>
void code_samples(Coder& coder, std::vector<Frame> const& inputs, std::vector<FramePose>& poses, Numbers& sample) {
    constexpr bool reading = std::is_same_v<Coder, RangeReader>;
    if constexpr (reading)
        poses.resize(inputs.size());
    std::array<std::array<uint64_t, 5>, 2> prev{};
    for (size_t i = 0; i < inputs.size() && !coder.failed; ++i) {
        FramePose pose = i < poses.size() ? poses[i] : FramePose{};
        int player = inputs[i].player2 ? 1 : 0;
        std::array<uint64_t, 5> raw{
            std::bit_cast<uint32_t>(pose.x),
            std::bit_cast<uint32_t>(pose.y),
            std::bit_cast<uint32_t>(pose.rotation),
            std::bit_cast<uint64_t>(pose.yVelocity),
            std::bit_cast<uint64_t>(pose.x_velocity)
        };
        for (int field = 0; field < 5; ++field) {
            bool wide = field >= 3;
            int64_t change = wide
                                 ? static_cast<int64_t>(raw[field] - prev[player][field])
                                 : static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(raw[field]) - static_cast<uint32_t>(prev[player][field])));
            int64_t coded = unzigzag(code_number(coder, sample[player * 5 + field], zigzag(change)));
            raw[field] = wide
                             ? prev[player][field] + static_cast<uint64_t>(coded)
                             : static_cast<uint32_t>(static_cast<uint32_t>(prev[player][field]) + static_cast<uint32_t>(coded));
            prev[player][field] = raw[field];
        }
        if constexpr (reading) {
            poses[i].x = std::bit_cast<float>(static_cast<uint32_t>(raw[0]));
            poses[i].y = std::bit_cast<float>(static_cast<uint32_t>(raw[1]));
            poses[i].rotation = std::bit_cast<float>(static_cast<uint32_t>(raw[2]));
            poses[i].yVelocity = std::bit_cast<double>(raw[3]);
            poses[i].x_velocity = std::bit_cast<double>(raw[4]);
        }
    }
}

struct InputOdds {
    std::array<Odds, 12> player{};
    std::array<std::array<Odds, 4>, 24> button{};
    std::array<Odds, 6> hold{};
    std::array<NumberOdds<6, 32>, 4> delta{};
    std::array<NumberOdds<7, 64>, 10> sample{};
};

void read_version2(RangeReader& coder, std::vector<Frame>& inputs, std::vector<FramePose>& poses, size_t count, bool samples) {
    auto odds = std::make_unique<InputOdds>();
    std::array<int, 6> state{};
    int last_channel = 0, last_hold = 0, last_frame = 0;
    for (size_t i = 0; i < count && !coder.failed; ++i) {
        Frame input;
        int context = last_channel * 2 + last_hold;
        int player2 = coder.code(odds->player[context], 0);
        auto& button = odds->button[context * 2 + player2];
        int high = coder.code(button[1], 0), low = coder.code(button[2 + high], 0);
        int code = (high << 1) | low;
        if (code == 3) {
            coder.failed = true;
            break;
        }
        int channel = player2 * 3 + code;
        int hold = coder.code(odds->hold[state[channel] * 2 + last_hold], 0);
        auto& gap = odds->delta[hold * 2 + (channel == last_channel ? 1 : 0)];
        uint64_t step = code_number(coder, gap, 0);
        if (step > static_cast<uint64_t>(INT32_MAX - last_frame)) {
            coder.failed = true;
            break;
        }
        int frame = last_frame + static_cast<int>(step);
        input.number = frame;
        input.holding = hold != 0;
        input.player2 = player2 != 0;
        input.button = ReplayIO::unpack_button(code);
        inputs.push_back(input);
        state[channel] = hold ? 1 : 2;
        last_channel = channel;
        last_hold = hold;
        last_frame = frame;
    }
    if (samples && !coder.failed)
        code_samples(coder, inputs, poses, odds->sample);
}

struct Logistic {
    static int squash(int d) {
        static constexpr int curve[33] = { 1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
            2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
        if (d > 2047)
            return 4095;
        if (d < -2047)
            return 1;
        int w = d & 127;
        int i = (d >> 7) + 16;
        return (curve[i] * (128 - w) + curve[i + 1] * w + 64) >> 7;
    }

    static int stretch(int p) {
        static auto const table = [] {
            std::array<int16_t, 4096> res{};
            int at = 0;
            for (int x = -2047; x <= 2047; ++x) {
                int v = squash(x);
                for (int i = at; i <= v; ++i)
                    res[i] = static_cast<int16_t>(x);
                at = v + 1;
            }
            for (int i = at; i < 4096; ++i)
                res[i] = 2047;
            return res;
        }();
        return table[p];
    }
};

template <size_t N>
struct Blend {
    std::array<int32_t, N + 1> weight;

    Blend() {
        weight.fill((1 << 16) / static_cast<int32_t>(N));
        weight[N] = 0;
    }
};

template <class Coder, size_t N>
int code_blend(Coder& coder, std::array<Tally*, N> const& cells, Blend<N>& blend, int bit, int rate) {
    std::array<int, N + 1> st;
    int64_t dot = 0;
    for (size_t i = 0; i < N; ++i) {
        st[i] = Logistic::stretch(cells[i]->one12());
        dot += static_cast<int64_t>(blend.weight[i]) * st[i];
    }
    st[N] = 256;
    dot += static_cast<int64_t>(blend.weight[N]) * st[N];
    int one = std::clamp(Logistic::squash(static_cast<int>(dot >> 16)), 1, 4095);
    bit = coder.bit(65536 - (static_cast<uint32_t>(one) << 4), bit);
    int error = ((bit << 12) - one) * rate;
    for (size_t i = 0; i <= N; ++i)
        blend.weight[i] += (st[i] * error) >> 10;
    for (auto* cell : cells)
        cell->learn(bit, QUICK);
    return bit;
}

int length_bucket(uint64_t length) {
    if (length < 4)
        return static_cast<int>(length);
    int width = std::bit_width(length);
    return std::min(4 + (width - 3) * 2 + static_cast<int>((length >> (width - 2)) & 1), 39);
}

constexpr int SLOT_NODES = 64, HIGH_NODES = 33 * 16;
using GapTree = std::array<Tally, SLOT_NODES + HIGH_NODES + 33 * 32>;

struct EventOdds {
    std::array<std::array<Tally, 6>, 40> match{};
    std::array<std::array<Tally, 5>, 144> channel_pair{};
    std::array<std::array<Tally, 5>, 12> channel_last{};
    std::array<std::array<Tally, 5>, 81 * 12> channel_held{};
    std::array<Blend<3>, 5> channel_blend{};
    std::array<Tally, 72> hold_short{};
    std::array<Tally, 12> hold_state{};
    std::array<Tally, 80> hold_gap{};
    std::array<Blend<3>, 2> hold_blend{};
    std::array<GapTree, 4> gap_kind{};
    std::array<GapTree, 4 * 40> gap_same{}, gap_last{};
    std::array<Blend<3>, 4 * 3> gap_blend{};
    std::array<NumberOdds<7, 64, Tally>, 10> sample{};
    std::vector<int32_t> recent = std::vector<int32_t>(1 << 18, -1);
};

template <class Coder>
uint64_t code_gap(Coder& coder, EventOdds& odds, int base, int same_bucket, int last_bucket, uint64_t number) {
    std::array<GapTree*, 3> trees{ &odds.gap_kind[base], &odds.gap_same[base * 40 + same_bucket], &odds.gap_last[base * 40 + last_bucket] };
    auto blended = [&](int node, int wanted, int stage) {
        std::array<Tally*, 3> cells{ &(*trees[0])[node], &(*trees[1])[node], &(*trees[2])[node] };
        return code_blend(coder, cells, odds.gap_blend[base * 3 + stage], wanted, 1);
    };
    int len = std::bit_width(number);
    int node = 1;
    for (int i = 5; i >= 0; --i)
        node = (node << 1) | blended(node, (len >> i) & 1, 0);
    len = node - SLOT_NODES;
    if (len > 32) {
        coder.failed = true;
        return 0;
    }
    if (len <= 1)
        return static_cast<uint64_t>(len);
    uint64_t res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int wanted = static_cast<int>((number >> i) & 1);
        int bit = depth < 4
                      ? blended(SLOT_NODES + len * 16 + tree, wanted, 1)
                      : blended(SLOT_NODES + HIGH_NODES + len * 32 + i, wanted, 2);
        if (depth < 4)
            tree = (tree << 1) | bit;
        res = (res << 1) | static_cast<uint64_t>(bit);
    }
    return res;
}

template <class Coder>
uint64_t code_plain(Coder& coder, uint64_t value, int width) {
    uint64_t res = 0;
    for (int i = width - 1; i >= 0; --i)
        res |= static_cast<uint64_t>(coder.bit(32768, static_cast<int>((value >> i) & 1))) << i;
    return res;
}

template <class Coder>
uint64_t code_count(Coder& coder, uint64_t value) {
    int len = static_cast<int>(code_plain(coder, static_cast<uint64_t>(std::bit_width(value)), 7));
    if (len > 64) {
        coder.failed = true;
        return 0;
    }
    if (len <= 1)
        return static_cast<uint64_t>(len);
    return (uint64_t{ 1 } << (len - 1)) | code_plain(coder, value, len - 1);
}

template <class Coder>
uint64_t code_bits(Coder& coder, uint64_t bits, int width) {
    int zeros = static_cast<int>(code_plain(coder, bits == 0 ? static_cast<uint64_t>(width) : static_cast<uint64_t>(std::countr_zero(bits)), width == 64 ? 7 : 6));
    if (zeros >= width) {
        if (zeros > width)
            coder.failed = true;
        return 0;
    }
    uint64_t rest = code_plain(coder, zeros + 1 < 64 ? bits >> (zeros + 1) : 0, width - zeros - 1);
    return ((rest << 1) | 1) << zeros;
}

template <class Coder>
void code_rates(Coder& coder, std::vector<RateChange>& rates, size_t count) {
    constexpr bool writing = std::is_same_v<Coder, RangeWriter>;
    int last = 0;
    uint32_t prev = 0;
    if constexpr (!writing)
        rates.clear();
    for (size_t i = 0; i < count && !coder.failed; ++i) {
        uint64_t gap = 0;
        uint32_t bits = 0;
        if constexpr (writing) {
            gap = static_cast<uint64_t>(std::max(rates[i].frame - last, 0));
            bits = std::bit_cast<uint32_t>(rates[i].fps) ^ prev;
        }
        gap = code_count(coder, gap);
        bits = static_cast<uint32_t>(code_bits(coder, bits, 32));
        if constexpr (!writing) {
            if (gap > static_cast<uint64_t>(INT32_MAX - last)) {
                coder.failed = true;
                break;
            }
            last += static_cast<int>(gap);
            bits ^= prev;
            float fps = std::bit_cast<float>(bits);
            if (!(fps >= 1.f) || !std::isfinite(fps)) {
                coder.failed = true;
                break;
            }
            rates.push_back({ last, fps });
            prev = bits;
        }
        else {
            last = rates[i].frame;
            prev = std::bit_cast<uint32_t>(rates[i].fps);
        }
    }
}

constexpr int MIN_MATCH = 4;
constexpr uint64_t VERIFY = 64;
constexpr uint64_t MAX_INPUTS = uint64_t{ 1 } << 24;
// random big numbers to scramble the hash. MIX is the golden ratio one
constexpr uint64_t HASH_SEED = 0x2545F4914F6CDD1Dull, HASH_MIX = 0x9E3779B97F4A7C15ull;
constexpr uint64_t HASH_ADDEND = 0x632BE59BD9B4E019ull;

uint32_t hash_tail(std::vector<uint64_t> const& history) {
    uint64_t hash = static_cast<uint64_t>(MIN_MATCH) * HASH_SEED;
    size_t start = history.size() - MIN_MATCH;
    for (size_t k = start; k < history.size(); ++k)
        hash = (hash ^ history[k]) * HASH_MIX + HASH_ADDEND;
    return static_cast<uint32_t>(hash >> 46);
}

constexpr int SYMBOL_SHIFT = 4;
constexpr int CHANNEL_MASK = 7;

struct Symbol {
    uint64_t gap = 0;
    int channel = 0;
    int hold = 0;

    uint64_t bits() const {
        return (gap << SYMBOL_SHIFT) | static_cast<uint64_t>((channel << 1) | hold);
    }

    static Symbol unpack(uint64_t raw) {
        return Symbol{
            raw >> SYMBOL_SHIFT,
            static_cast<int>((raw >> 1) & CHANNEL_MASK),
            static_cast<int>(raw & 1),
        };
    }

    static Symbol from_input(Frame const& input, int last_frame) {
        int channel = (input.player2 ? 3 : 0) + ReplayIO::pack_button(input.button);
        int hold = input.holding ? 1 : 0;
        return Symbol{ static_cast<uint64_t>(input.number - last_frame), channel, hold };
    }

    Frame to_input(int frame) const {
        Frame input;
        input.number = frame;
        input.holding = hold != 0;
        input.player2 = channel >= 3;
        input.button = ReplayIO::unpack_button(channel % 3);
        return input;
    }

    bool overflows(int last_frame) const {
        return gap > static_cast<uint64_t>(INT32_MAX - last_frame) || channel > 5;
    }
};

template <class Coder>
void code_events(Coder& coder, EventOdds& odds, std::vector<Frame>& inputs, size_t count) {
    constexpr bool writing = std::is_same_v<Coder, RangeWriter>;
    std::vector<uint64_t> history;
    history.reserve(std::min<size_t>(count, 1u << 20));
    size_t ahead = 0;
    uint64_t matched = 0;
    int misses = 0;
    std::array<int, 6> state{};
    std::array<uint64_t, 12> kind_gap{};
    int last_frame = 0;
    auto symbol_at = [&](size_t back) {
        return history.size() > back ? static_cast<int>(history[history.size() - 1 - back] & 15) : 0;
    };
    for (size_t i = 0; i < count && !coder.failed; ++i) {
        uint64_t want = 0;
        if constexpr (writing)
            want = Symbol::from_input(inputs[i], last_frame).bits();

        uint64_t got = 0;
        bool guessed = false;
        if (matched > 0) {
            uint64_t guess = history[ahead], guess_gap = guess >> SYMBOL_SHIFT;
            int gap_shape = guess_gap == 0 ? 0 : guess_gap == 1 ? 1
                                                                : 2;
            int kind = gap_shape * 2 + std::min(misses, 1);
            guessed = coder.code(odds.match[length_bucket(matched)][kind], want == guess ? 1 : 0, STEADY) != 0;
            if (guessed)
                got = guess;
            misses = guessed ? 0 : misses + 1;
        }
        if (!guessed) {
            int last = symbol_at(0);
            int prev = symbol_at(1);
            uint64_t last_gap = history.empty() ? 0 : history.back() >> SYMBOL_SHIFT;
            int want_channel = static_cast<int>((want >> 1) & CHANNEL_MASK);
            int held = state[0] + state[1] * 3 + state[3] * 9 + state[4] * 27;
            auto channel_bit = [&](int node, int wanted) {
                std::array<Tally*, 3> cells{
                    &odds.channel_pair[last * 12 + prev][node],
                    &odds.channel_last[last][node],
                    &odds.channel_held[held * 12 + last][node],
                };
                return code_blend(coder, cells, odds.channel_blend[node], wanted, 2);
            };
            int player2 = channel_bit(0, want_channel >= 3 ? 1 : 0);
            int high = channel_bit(1 + player2, (want_channel % 3) >> 1);
            int low = high ? 0 : channel_bit(3 + player2, (want_channel % 3) & 1);
            int channel = player2 * 3 + ((high << 1) | low);
            int same = channel == (last >> 1) ? 1 : 0;
            int base = (state[channel] * 2 + (last & 1)) * 2 + same;
            int short_gap = static_cast<int>(std::min<uint64_t>(last_gap, 5));
            int gap_band = (state[channel] == 1 ? 40 : 0) + length_bucket(last_gap);
            std::array<Tally*, 3> hold_cells{
                &odds.hold_short[base * 6 + short_gap],
                &odds.hold_state[base],
                &odds.hold_gap[gap_band],
            };
            int hold = code_blend(coder, hold_cells, odds.hold_blend[same], static_cast<int>(want & 1), 2);
            uint64_t gap = code_gap(
                coder,
                odds,
                hold * 2 + same,
                length_bucket(kind_gap[channel * 2 + hold]),
                length_bucket(last_gap),
                want >> SYMBOL_SHIFT);
            got = Symbol{ gap, channel, hold }.bits();
        }
        Symbol symbol = Symbol::unpack(got);
        if (symbol.overflows(last_frame)) {
            coder.failed = true;
            break;
        }
        int frame = last_frame + static_cast<int>(symbol.gap);
        if constexpr (!writing)
            inputs.push_back(symbol.to_input(frame));
        int channel = symbol.channel;
        int hold = symbol.hold;
        uint64_t gap = symbol.gap;

        if (matched > 0 && history[ahead] == got) {
            ++matched;
            ++ahead;
        }
        else {
            matched = 0;
        }
        history.push_back(got);
        if (history.size() >= MIN_MATCH) {
            uint32_t slot = hash_tail(history);
            int32_t at = odds.recent[slot];
            if (matched == 0 && at >= 0) {
                uint64_t len = 0;
                while (len < static_cast<uint64_t>(at) && len < VERIFY) {
                    size_t here = history.size() - 1 - len;
                    size_t there = static_cast<size_t>(at) - 1 - len;
                    if (history[there] != history[here])
                        break;
                    ++len;
                }
                if (len >= MIN_MATCH) {
                    matched = len;
                    ahead = static_cast<size_t>(at);
                }
            }
            odds.recent[slot] = static_cast<int32_t>(history.size());
        }
        state[channel] = hold ? 1 : 2;
        kind_gap[channel * 2 + hold] = gap;
        last_frame = frame;
    }
}

int ReplayIO::pack_button(int button) {
    if (button == static_cast<int>(PlayerButton::Left))
        return 1;
    if (button == static_cast<int>(PlayerButton::Right))
        return 2;
    return 0;
}

int ReplayIO::unpack_button(int code) {
    if (code == 1)
        return static_cast<int>(PlayerButton::Left);
    if (code == 2)
        return static_cast<int>(PlayerButton::Right);
    return static_cast<int>(PlayerButton::Jump);
}

template <typename A1>
void ReplayIO::write_pod(std::ostream& file, A1 const& value) {
    file.write(reinterpret_cast<char const*>(&value), sizeof(value));
}

template <typename A1>
bool ReplayIO::read_pod(std::istream& file, A1& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}

bool read_varint(std::istream& file, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        int byte = file.get();
        if (byte == EOF)
            return false;
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0)
            return true;
    }
    return false;
}

constexpr uint8_t FLAG_SAMPLES = 1, FLAG_WHOLE_FPS = 2, FLAG_SEED = 4, FLAG_UNROUNDED = 8;
constexpr uint8_t FLAG_SILICATE_VARIANCE = 16;
constexpr uint8_t FLAG_LDM_KNOWN = 32, FLAG_LDM = 64; // old macros have neither = dont know

struct PackedBody {
    float fps = 240.f;
    float end_x = 0.f;
    double seconds = 0.0;
    uint64_t attempts = 0;
    uint64_t seed = 0;
    bool has_seed = false;
    bool unrounded = false;
    bool silicate_variance = false;
    bool ldm = false, ldm_known = false;
    std::vector<Frame> inputs;
    std::vector<FramePose> poses;
    std::vector<RateChange> rates;
};

std::vector<Frame> inputs_in_pack_order(std::vector<Frame> inputs, std::vector<FramePose>* poses) {
    std::vector<size_t> order(inputs.size());
    std::iota(order.begin(), order.end(), 0);
    for (auto& input : inputs)
        input.number = std::max(input.number, 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto const& left = inputs[a];
        auto const& right = inputs[b];
        // by frame ONLY, the order on a frame is kept as is (see Logic::sort_inputs)
        return left.number < right.number;
    });
    std::vector<Frame> next;
    next.reserve(inputs.size());
    for (size_t i : order)
        next.push_back(inputs[i]);
    if (poses && poses->size() == inputs.size()) {
        std::vector<FramePose> sorted;
        sorted.reserve(poses->size());
        for (size_t i : order)
            sorted.push_back((*poses)[i]);
        *poses = std::move(sorted);
    }
    return next;
}

std::vector<uint8_t> read_rest(std::istream& file) {
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

RangeReader open_range(std::vector<uint8_t> const& packed) {
    RangeReader body;
    body.data = packed.data();
    body.size = packed.size();
    body.start();
    return body;
}

void read_version1_samples(std::vector<Frame> const& inputs, std::vector<FramePose>& poses, std::vector<uint8_t> const& packed, BitReader const& stream) {
    size_t cursor = (stream.index * 8 - static_cast<size_t>(stream.left)) / 8;
    if (stream.left != 0)
        ++cursor;
    auto read_at = [&](auto& pod) {
        if (cursor + sizeof(pod) > packed.size())
            return false;
        std::memcpy(&pod, packed.data() + cursor, sizeof(pod));
        cursor += sizeof(pod);
        return true;
    };
    poses.resize(inputs.size());
    for (auto& pose : poses) {
        if (!read_at(pose.x) || !read_at(pose.y) || !read_at(pose.rotation))
            break;
        if (!read_at(pose.yVelocity) || !read_at(pose.x_velocity))
            break;
    }
}

bool load_version1(std::istream& file, uint8_t flags, PackedBody& out) {
    uint16_t reserved = 0;
    uint32_t attempts = 0;
    uint32_t amt = 0;
    if (!ReplayIO::read_pod(file, reserved))
        return false;
    if (!ReplayIO::read_pod(file, out.fps) || !ReplayIO::read_pod(file, out.end_x) || !ReplayIO::read_pod(file, out.seconds))
        return false;
    if (!ReplayIO::read_pod(file, attempts) || !ReplayIO::read_pod(file, amt))
        return false;
    out.attempts = attempts;
    if (amt > MAX_INPUTS)
        return false;
    auto packed = read_rest(file);
    BitReader stream;
    stream.data = packed.data();
    stream.size = packed.size();
    out.inputs.reserve(std::min<uint64_t>(amt, packed.size() * 8 / 5 + 1));
    int frame = 0;
    for (uint32_t i = 0; i < amt; ++i) {
        Frame input;
        input.holding = stream.bit() != 0;
        input.player2 = stream.bit() != 0;
        int code = (stream.bit() << 1) | stream.bit();
        input.button = ReplayIO::unpack_button(code);
        uint32_t delta = stream.gamma();
        if (delta == 0)
            return false;
        frame += static_cast<int>(delta - 1);
        input.number = frame;
        out.inputs.push_back(input);
    }
    if (flags & FLAG_SAMPLES)
        read_version1_samples(out.inputs, out.poses, packed, stream);
    return true;
}

bool load_version2(std::istream& file, uint8_t flags, PackedBody& out) {
    if (flags & FLAG_WHOLE_FPS) {
        uint64_t whole = 0;
        if (!read_varint(file, whole))
            return false;
        out.fps = static_cast<float>(whole);
    }
    else if (!ReplayIO::read_pod(file, out.fps)) {
        return false;
    }
    if (!ReplayIO::read_pod(file, out.end_x) || !ReplayIO::read_pod(file, out.seconds))
        return false;
    uint64_t amt = 0;
    if (!read_varint(file, out.attempts) || !read_varint(file, amt))
        return false;
    auto packed = read_rest(file);
    RangeReader body = open_range(packed);
    out.inputs.reserve(static_cast<size_t>(std::min<uint64_t>(amt, 1u << 20)));
    read_version2(body, out.inputs, out.poses, static_cast<size_t>(amt), flags & FLAG_SAMPLES);
    return !body.failed && out.inputs.size() == amt;
}

bool load_version3(std::istream& file, uint8_t flags, int version, PackedBody& out) {
    auto packed = read_rest(file);
    RangeReader body = open_range(packed);
    auto odds = std::make_unique<EventOdds>();
    out.fps = std::bit_cast<float>(static_cast<uint32_t>(code_bits(body, 0, 32)));
    out.end_x = std::bit_cast<float>(static_cast<uint32_t>(code_bits(body, 0, 32)));
    out.seconds = std::bit_cast<double>(code_bits(body, 0, 64));
    out.attempts = code_count(body, 0);
    uint64_t amt = code_count(body, 0);
    if (body.failed || amt > MAX_INPUTS || out.attempts > INT32_MAX)
        return false;
    out.inputs.reserve(static_cast<size_t>(std::min<uint64_t>(amt, 1u << 20)));
    code_events(body, *odds, out.inputs, static_cast<size_t>(amt));
    if (!body.failed && (flags & FLAG_SAMPLES))
        code_samples(body, out.inputs, out.poses, odds->sample);
    if (version == 4 && !body.failed) {
        uint64_t rate_count = code_count(body, 0);
        if (body.failed || rate_count > (uint64_t{ 1 } << 16))
            return false;
        code_rates(body, out.rates, static_cast<size_t>(rate_count));
        if (body.failed || out.rates.size() != rate_count)
            return false;
    }
    if (!body.failed && (flags & FLAG_SEED)) {
        out.seed = code_bits(body, 0, 64);
        out.has_seed = !body.failed;
    }
    return !body.failed && out.inputs.size() == amt;
}

void store_packed(PackedBody& body) {
    auto& logic = Logic::get();
    logic.inputs = std::move(body.inputs);
    logic.poses = std::move(body.poses);
    logic.input_ids.clear();
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    logic.rates = std::move(body.rates);
    logic.fps = body.fps > 0.f ? body.fps : logic.fps;
    logic.macro_fps = logic.fps;
    logic.end_x = body.end_x;
    logic.recording_seconds = body.seconds;
    logic.attempts = static_cast<int>(body.attempts);
    logic.lvl_seed = body.seed;
    logic.lvl_seed_known = body.has_seed;
    logic.lvl_seed_from_file = body.has_seed;
    logic.macro_unrounded = body.unrounded;
    logic.macro_silicate_variance = body.silicate_variance;
    logic.macro_ldm = body.ldm;
    logic.macro_ldm_known = body.ldm_known;
    logic.format = ReplayFormat::Meta;
}

bool ReplayIO::write_packed_echo(fs::path const& path) {
    Logic::get().normalize_rates();
    auto const& logic = Logic::get();
    auto poses = logic.poses.size() == logic.inputs.size() ? logic.poses : std::vector<FramePose>{};
    auto inputs = inputs_in_pack_order(logic.inputs, poses.empty() ? nullptr : &poses);
    bool samples = logic.save_debug && poses.size() == inputs.size();
    bool rates = !logic.rates.empty();

    auto odds = std::make_unique<EventOdds>();
    RangeWriter body;
    float header_fps = rates ? logic.macro_fps : static_cast<float>(logic.sample_rate());
    code_bits(body, std::bit_cast<uint32_t>(header_fps), 32);
    code_bits(body, std::bit_cast<uint32_t>(logic.end_x), 32);
    code_bits(body, std::bit_cast<uint64_t>(logic.recording_seconds), 64);
    code_count(body, static_cast<uint64_t>(std::max(logic.attempts, 0)));
    code_count(body, inputs.size());
    code_events(body, *odds, inputs, inputs.size());
    if (samples)
        code_samples(body, inputs, poses, odds->sample);
    if (rates) {
        auto stored = logic.rates;
        code_count(body, stored.size());
        code_rates(body, stored, stored.size());
    }
    if (logic.lvl_seed_known)
        code_bits(body, logic.lvl_seed, 64);
    body.finish();

    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[4] = { 'E', 'C', 'H', '2' };
    file.write(magic, 4);
    uint8_t version = rates ? 4 : 3;
    uint8_t flags = samples ? FLAG_SAMPLES : 0;
    if (logic.lvl_seed_known)
        flags = static_cast<uint8_t>(flags | FLAG_SEED);
    if (logic.macro_unrounded)
        flags = static_cast<uint8_t>(flags | FLAG_UNROUNDED);
    if (logic.macro_silicate_variance)
        flags = static_cast<uint8_t>(flags | FLAG_SILICATE_VARIANCE);
    if (logic.macro_ldm_known)
        flags = static_cast<uint8_t>(flags | FLAG_LDM_KNOWN | (logic.macro_ldm ? FLAG_LDM : 0));
    ReplayIO::write_pod(file, version);
    ReplayIO::write_pod(file, flags);
    if (!body.bytes.empty())
        file.write(reinterpret_cast<char const*>(body.bytes.data()), static_cast<std::streamsize>(body.bytes.size()));
    return static_cast<bool>(file);
}

bool ReplayIO::read_packed_echo(fs::path const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[4] = {};
    file.read(magic, 4);
    if (!file || std::string_view(magic, 4) != "ECH2")
        return false;
    uint8_t version = 0;
    uint8_t flags = 0;
    if (!ReplayIO::read_pod(file, version) || !ReplayIO::read_pod(file, flags))
        return false;

    PackedBody body;
    bool loaded = false;
    body.unrounded = (flags & FLAG_UNROUNDED) != 0;
    body.silicate_variance = (flags & FLAG_SILICATE_VARIANCE) != 0;
    body.ldm_known = (flags & FLAG_LDM_KNOWN) != 0;
    body.ldm = (flags & FLAG_LDM) != 0;
    if (version == 1)
        loaded = load_version1(file, flags, body);
    else if (version == 2)
        loaded = load_version2(file, flags, body);
    else if (version == 3 || version == 4)
        loaded = load_version3(file, flags, version, body);
    if (!loaded)
        return false;
    store_packed(body);
    return true;
}
