#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <vector>

namespace fs = std::filesystem;

using namespace geode::prelude;

// every number below is part of the .echo format. change one & old saves stop loading

// a bit's odds are counted out of 65536
constexpr int ODDS_SCALE = 65536;
constexpr int ODDS_HALF = 32768;
// odds never go all the way to 0 or 65536
constexpr int ODDS_EDGE = 64;
// an Odds stops getting more sure after this many bits
constexpr int ODDS_MEMORY = 10;
// a Tally counts out of 2^32
constexpr unsigned TALLY_HALF = 1u << 31;
constexpr long long TALLY_MAX = 4294967295ll;
// how fast a tally learns: QUICK for most, STEADY for the "same as last time" bit
constexpr int QUICK = 30;
constexpr int STEADY = 4000;

constexpr unsigned FULL_RANGE = 4294967295u;
// the range coder puts out a byte whenever its range drops under this
constexpr unsigned RANGE_TOP = 1u << 24;
// low's top byte is 255, so a carry could still change it. hold it back
constexpr unsigned CARRY_ZONE = 255u << 24;
constexpr int FLUSH_BYTES = 5;
// a reader can run this far past the end of the data before the file counts as broken
constexpr size_t READ_PAST_END = 64;

// a number is stored as its length in bits (a tree of NUMBER_SLOT_BITS bits), then the bits under its top 1.
// the first TREE_DEPTH of those have odds for every path, the rest have odds per position
constexpr int NUMBER_SLOT_BITS = 7;
constexpr int NUMBER_MAX_LEN = 64;
constexpr int GAP_SLOT_BITS = 6;
constexpr int GAP_MAX_LEN = 32;
constexpr int TREE_DEPTH = 4;
constexpr int COUNT_BITS = 7;

// x, y & rotation are floats, the 2 velocities are doubles
constexpr int POSE_FIELDS = 5;
constexpr int POSE_FLOATS = 3;

// a channel is a button (jump, left, right) of a player
constexpr int BUTTONS = 3;
constexpr int CHANNELS = BUTTONS * 2;
// a symbol is channel << 1 | holding
constexpr int SYMBOL_KINDS = CHANNELS * 2;
constexpr int SYMBOL_SHIFT = 4;
// each channel is unknown, held or released (3 states). 4 of them are looked at together
constexpr int HELD_STATES = 81;
constexpr int CHANNEL_NODES = 5;
constexpr int HOLD_BASES = 12;
constexpr int SHORT_GAPS = 6;
constexpr int GAP_BASES = 4;
constexpr int GAP_STAGES = 3;
constexpr int LENGTH_BUCKETS = 40;
constexpr int MATCH_KINDS = 6;

// the 12 bit chances blending works in
constexpr int CHANCE_MAX = 4095;
constexpr int SQUASH_LIMIT = 2047;
constexpr int BLEND_CELLS = 3;
constexpr int BLEND_ONE = 1 << 16;
constexpr int BLEND_BIAS = 256;

constexpr int MIN_MATCH = 4;
constexpr unsigned long long LONGEST_CHECK = 64;
constexpr int MATCH_SLOT_BITS = 18;
constexpr unsigned long long MATCH_HASH_START = 2685821657736338717ull;
constexpr unsigned long long MATCH_HASH_MIX = 11400714819323198485ull;
constexpr unsigned long long MATCH_HASH_ADD = 7146057691288625177ull;

constexpr unsigned long long MAX_RATE_CHANGES = 1ull << 16;
constexpr size_t RESERVE_LIMIT = 1u << 20;

// version 1 files are plain bits
struct BitReader {
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t index = 0;
    int left = 0;
    unsigned char acc = 0;

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

    unsigned gamma() {
        unsigned zeros = 0;
        while (bit() == 0) {
            if (++zeros > 31 || index > size)
                return 0;
        }
        unsigned n = 1;
        for (unsigned i = 0; i < zeros; ++i)
            n = (n << 1) | (unsigned)bit();
        return n;
    }
};

// how likely the next bit is a 0. version 2 files learn w/ these
struct Odds {
    unsigned short value = ODDS_HALF;
    unsigned char seen = 0;

    unsigned chance() const {
        return std::clamp<unsigned>(value, ODDS_EDGE, ODDS_SCALE - 1 - ODDS_EDGE);
    }

    void learn(int bit) {
        int target = bit ? 0 : ODDS_SCALE - 1;
        value = (unsigned short)(value + (target - (int)value) / ((int)seen + 2));
        if (seen < ODDS_MEMORY)
            ++seen;
    }
};

// how likely the next bit is a 1. versions 3 & 4 learn w/ these
struct Tally {
    unsigned one = TALLY_HALF;
    unsigned short seen = 0;

    unsigned zero16() const {
        return ODDS_SCALE - std::clamp<unsigned>(one >> 16, 1, ODDS_SCALE - 1);
    }

    int one12() const {
        return (int)(one >> 20);
    }

    void learn(int bit, int cap) {
        long long target = bit ? TALLY_MAX : 0;
        one = (unsigned)(one + (target - (long long)one) / (seen + 2));
        if (seen < cap)
            ++seen;
    }
};

struct RangeWriter {
    std::vector<unsigned char> bytes;
    unsigned long long low = 0;
    unsigned range = FULL_RANGE;
    unsigned char cache = 0;
    unsigned long long pending = 1;
    bool first = true;

    void shift_low() {
        if ((unsigned)low < CARRY_ZONE || (low >> 32) != 0) {
            unsigned char carry = (unsigned char)(low >> 32);
            unsigned char out = cache;
            do {
                if (!first)
                    bytes.push_back((unsigned char)(out + carry));
                first = false;
                out = 255;
            } while (--pending != 0);
            cache = (unsigned char)(low >> 24);
        }
        ++pending;
        low = (low & (RANGE_TOP - 1)) << 8;
    }

    void bit(unsigned zero, int value) {
        unsigned bound = (range >> 16) * zero;
        if (value) {
            low += bound;
            range -= bound;
        }
        else {
            range = bound;
        }
        while (range < RANGE_TOP) {
            range <<= 8;
            shift_low();
        }
    }

    void write(Tally& tally, int value, int cap = QUICK) {
        bit(tally.zero16(), value);
        tally.learn(value, cap);
    }

    void finish() {
        for (int zeros = 32; zeros > 0; --zeros) {
            unsigned long long mask = (1ull << zeros) - 1;
            unsigned long long rounded = (low + mask) & ~mask;
            if (rounded < low + range) {
                low = rounded;
                break;
            }
        }
        for (int i = 0; i < FLUSH_BYTES; ++i)
            shift_low();
        while (!bytes.empty() && bytes.back() == 0)
            bytes.pop_back();
    }
};

struct RangeReader {
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t index = 0;
    unsigned range = FULL_RANGE;
    unsigned value = 0;
    bool failed = false;

    unsigned char next() {
        if (index >= size) {
            if (++index > size + READ_PAST_END)
                failed = true;
            return 0;
        }
        return data[index++];
    }

    void start() {
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | next();
    }

    int bit(unsigned zero) {
        unsigned bound = (range >> 16) * zero;
        int res = value >= bound ? 1 : 0;
        if (res) {
            value -= bound;
            range -= bound;
        }
        else {
            range = bound;
        }
        while (range < RANGE_TOP) {
            range <<= 8;
            value = (value << 8) | next();
        }
        return res;
    }

    int read(Odds& odds) {
        int res = bit(odds.chance());
        odds.learn(res);
        return res;
    }

    int read(Tally& tally, int cap = QUICK) {
        int res = bit(tally.zero16());
        tally.learn(res, cap);
        return res;
    }
};

struct OddsNumbers {
    int slot_bits = NUMBER_SLOT_BITS;
    int max_len = NUMBER_MAX_LEN;
    std::array<Odds, 1 << NUMBER_SLOT_BITS> slot{};
    std::array<std::array<Odds, 1 << TREE_DEPTH>, NUMBER_MAX_LEN + 1> high{};
    std::array<std::array<Odds, NUMBER_MAX_LEN>, NUMBER_MAX_LEN + 1> low{};
};

struct TallyNumbers {
    std::array<Tally, 1 << NUMBER_SLOT_BITS> slot{};
    std::array<std::array<Tally, 1 << TREE_DEPTH>, NUMBER_MAX_LEN + 1> high{};
    std::array<std::array<Tally, NUMBER_MAX_LEN>, NUMBER_MAX_LEN + 1> low{};
};

static unsigned long long read_old_number(RangeReader& reader, OddsNumbers& odds) {
    int node = 1;
    for (int i = odds.slot_bits - 1; i >= 0; --i)
        node = (node << 1) | reader.read(odds.slot[node]);
    int len = node - (1 << odds.slot_bits);
    if (len > odds.max_len) {
        reader.failed = true;
        return 0;
    }
    if (len <= 1)
        return (unsigned long long)len;
    unsigned long long res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = depth < TREE_DEPTH ? reader.read(odds.high[len][tree]) : reader.read(odds.low[len][i]);
        if (depth < TREE_DEPTH)
            tree = (tree << 1) | bit;
        res = (res << 1) | (unsigned long long)bit;
    }
    return res;
}

static unsigned long long read_number(RangeReader& reader, TallyNumbers& odds) {
    int node = 1;
    for (int i = NUMBER_SLOT_BITS - 1; i >= 0; --i)
        node = (node << 1) | reader.read(odds.slot[node]);
    int len = node - (1 << NUMBER_SLOT_BITS);
    if (len > NUMBER_MAX_LEN) {
        reader.failed = true;
        return 0;
    }
    if (len <= 1)
        return (unsigned long long)len;
    unsigned long long res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = depth < TREE_DEPTH ? reader.read(odds.high[len][tree]) : reader.read(odds.low[len][i]);
        if (depth < TREE_DEPTH)
            tree = (tree << 1) | bit;
        res = (res << 1) | (unsigned long long)bit;
    }
    return res;
}

static void write_number(RangeWriter& writer, TallyNumbers& odds, unsigned long long number) {
    int len = std::bit_width(number);
    int node = 1;
    for (int i = NUMBER_SLOT_BITS - 1; i >= 0; --i) {
        int bit = (len >> i) & 1;
        writer.write(odds.slot[node], bit);
        node = (node << 1) | bit;
    }
    if (len <= 1)
        return;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = (int)((number >> i) & 1);
        if (depth < TREE_DEPTH) {
            writer.write(odds.high[len][tree], bit);
            tree = (tree << 1) | bit;
        }
        else {
            writer.write(odds.low[len][i], bit);
        }
    }
}

static unsigned long long zigzag(long long value) {
    return ((unsigned long long)value << 1) ^ (unsigned long long)(value >> 63);
}

static long long unzigzag(unsigned long long value) {
    return (long long)(value >> 1) ^ -(long long)(value & 1);
}

// poses are stored as how much each field changed since that players last pose
static std::array<unsigned long long, POSE_FIELDS> pose_fields(const FramePose& pose) {
    return {
        std::bit_cast<unsigned>(pose.x),
        std::bit_cast<unsigned>(pose.y),
        std::bit_cast<unsigned>(pose.rotation),
        std::bit_cast<unsigned long long>(pose.y_velocity),
        std::bit_cast<unsigned long long>(pose.x_velocity),
    };
}

static long long field_change(int field, unsigned long long now, unsigned long long before) {
    if (field >= POSE_FLOATS)
        return (long long)(now - before);
    return (long long)((int)((unsigned)now - (unsigned)before));
}

static unsigned long long apply_change(int field, unsigned long long before, long long change) {
    if (field >= POSE_FLOATS)
        return before + (unsigned long long)change;
    return (unsigned)((unsigned)before + (unsigned)change);
}

static FramePose pose_from_fields(const std::array<unsigned long long, POSE_FIELDS>& fields) {
    FramePose pose;
    pose.x = std::bit_cast<float>((unsigned)fields[0]);
    pose.y = std::bit_cast<float>((unsigned)fields[1]);
    pose.rotation = std::bit_cast<float>((unsigned)fields[2]);
    pose.y_velocity = std::bit_cast<double>(fields[3]);
    pose.x_velocity = std::bit_cast<double>(fields[4]);
    return pose;
}

static void read_old_poses(RangeReader& reader, const std::vector<Frame>& inputs, std::vector<FramePose>& poses, std::array<OddsNumbers, POSE_FIELDS * 2>& odds) {
    poses.resize(inputs.size());
    std::array<std::array<unsigned long long, POSE_FIELDS>, 2> prev{};
    for (size_t i = 0; i < inputs.size() && !reader.failed; ++i) {
        int player = inputs[i].player2 ? 1 : 0;
        std::array<unsigned long long, POSE_FIELDS> fields{};
        for (int field = 0; field < POSE_FIELDS; ++field) {
            long long change = unzigzag(read_old_number(reader, odds[player * POSE_FIELDS + field]));
            fields[field] = apply_change(field, prev[player][field], change);
            prev[player][field] = fields[field];
        }
        poses[i] = pose_from_fields(fields);
    }
}

static void read_poses(RangeReader& reader, const std::vector<Frame>& inputs, std::vector<FramePose>& poses, std::array<TallyNumbers, POSE_FIELDS * 2>& odds) {
    poses.resize(inputs.size());
    std::array<std::array<unsigned long long, POSE_FIELDS>, 2> prev{};
    for (size_t i = 0; i < inputs.size() && !reader.failed; ++i) {
        int player = inputs[i].player2 ? 1 : 0;
        std::array<unsigned long long, POSE_FIELDS> fields{};
        for (int field = 0; field < POSE_FIELDS; ++field) {
            long long change = unzigzag(read_number(reader, odds[player * POSE_FIELDS + field]));
            fields[field] = apply_change(field, prev[player][field], change);
            prev[player][field] = fields[field];
        }
        poses[i] = pose_from_fields(fields);
    }
}

static void write_poses(RangeWriter& writer, const std::vector<Frame>& inputs, const std::vector<FramePose>& poses, std::array<TallyNumbers, POSE_FIELDS * 2>& odds) {
    std::array<std::array<unsigned long long, POSE_FIELDS>, 2> prev{};
    for (size_t i = 0; i < inputs.size(); ++i) {
        int player = inputs[i].player2 ? 1 : 0;
        auto fields = pose_fields(i < poses.size() ? poses[i] : FramePose{});
        for (int field = 0; field < POSE_FIELDS; ++field) {
            write_number(writer, odds[player * POSE_FIELDS + field], zigzag(field_change(field, fields[field], prev[player][field])));
            prev[player][field] = fields[field];
        }
    }
}

struct OldInputOdds {
    std::array<Odds, CHANNELS * 2> player{};
    std::array<std::array<Odds, 4>, CHANNELS * 4> button{};
    std::array<Odds, 6> hold{};
    std::array<OddsNumbers, GAP_BASES> gap{};
    std::array<OddsNumbers, POSE_FIELDS * 2> pose{};

    OldInputOdds() {
        for (auto& numbers : gap) {
            numbers.slot_bits = GAP_SLOT_BITS;
            numbers.max_len = GAP_MAX_LEN;
        }
    }
};

static void read_version2_inputs(RangeReader& reader, std::vector<Frame>& inputs, std::vector<FramePose>& poses, size_t count, bool samples) {
    auto odds = std::make_unique<OldInputOdds>();
    std::array<int, CHANNELS> state{};
    int last_channel = 0, last_hold = 0, last_frame = 0;
    for (size_t i = 0; i < count && !reader.failed; ++i) {
        Frame input;
        int context = last_channel * 2 + last_hold;
        int player2 = reader.read(odds->player[context]);
        auto& button = odds->button[context * 2 + player2];
        int high = reader.read(button[1]);
        int low = reader.read(button[2 + high]);
        int code = (high << 1) | low;
        if (code == 3) {
            reader.failed = true;
            break;
        }
        int channel = player2 * BUTTONS + code;
        int hold = reader.read(odds->hold[state[channel] * 2 + last_hold]);
        auto& gap = odds->gap[hold * 2 + (channel == last_channel ? 1 : 0)];
        unsigned long long step = read_old_number(reader, gap);
        if (step > (unsigned long long)(INT_MAX - last_frame)) {
            reader.failed = true;
            break;
        }
        int frame = last_frame + (int)step;
        input.number = frame;
        input.holding = hold != 0;
        input.player2 = player2 != 0;
        input.button = ReplayIO::code_button(code);
        inputs.push_back(input);
        state[channel] = hold ? 1 : 2;
        last_channel = channel;
        last_hold = hold;
        last_frame = frame;
    }
    if (samples && !reader.failed)
        read_old_poses(reader, inputs, poses, odds->pose);
}

// a few tallies are mixed into 1 guess. how much each one counts is learned as it goes
// squash turns a blended score into a 12 bit chance. the curve has a point every 128 & is filled in between
static int squash(int d) {
    static constexpr int curve[33] = { 1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
        2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
    if (d > SQUASH_LIMIT)
        return CHANCE_MAX;
    if (d < -SQUASH_LIMIT)
        return 1;
    int w = d & 127;
    int i = (d >> 7) + 16;
    return (curve[i] * (128 - w) + curve[i + 1] * w + 64) >> 7;
}

static int stretch(int p) {
    static const auto table = [] {
        std::array<short, CHANCE_MAX + 1> res{};
        int at = 0;
        for (int x = -SQUASH_LIMIT; x <= SQUASH_LIMIT; ++x) {
            int v = squash(x);
            for (int i = at; i <= v; ++i)
                res[i] = (short)x;
            at = v + 1;
        }
        for (int i = at; i <= CHANCE_MAX; ++i)
            res[i] = SQUASH_LIMIT;
        return res;
    }();
    return table[p];
}

// the last weight is for a fixed BLEND_BIAS, not a tally
struct Blend {
    std::array<int, BLEND_CELLS + 1> weight;

    Blend() {
        weight.fill(BLEND_ONE / BLEND_CELLS);
        weight[BLEND_CELLS] = 0;
    }
};

using BlendCells = std::array<Tally*, BLEND_CELLS>;

static int blend_chance(const BlendCells& cells, const Blend& blend, std::array<int, BLEND_CELLS + 1>& stretched) {
    long long dot = 0;
    for (size_t i = 0; i < BLEND_CELLS; ++i) {
        stretched[i] = stretch(cells[i]->one12());
        dot += (long long)blend.weight[i] * stretched[i];
    }
    stretched[BLEND_CELLS] = BLEND_BIAS;
    dot += (long long)blend.weight[BLEND_CELLS] * stretched[BLEND_CELLS];
    return std::clamp(squash((int)(dot >> 16)), 1, CHANCE_MAX);
}

static void blend_learn(const BlendCells& cells, Blend& blend, const std::array<int, BLEND_CELLS + 1>& stretched, int one, int bit, int rate) {
    int error = ((bit << 12) - one) * rate;
    for (size_t i = 0; i <= BLEND_CELLS; ++i)
        blend.weight[i] += (stretched[i] * error) >> 10;
    for (auto* cell : cells)
        cell->learn(bit, QUICK);
}

static void write_blend(RangeWriter& writer, const BlendCells& cells, Blend& blend, int bit, int rate) {
    std::array<int, BLEND_CELLS + 1> stretched;
    int one = blend_chance(cells, blend, stretched);
    writer.bit(ODDS_SCALE - ((unsigned)one << 4), bit);
    blend_learn(cells, blend, stretched, one, bit, rate);
}

static int read_blend(RangeReader& reader, const BlendCells& cells, Blend& blend, int rate) {
    std::array<int, BLEND_CELLS + 1> stretched;
    int one = blend_chance(cells, blend, stretched);
    int bit = reader.bit(ODDS_SCALE - ((unsigned)one << 4));
    blend_learn(cells, blend, stretched, one, bit, rate);
    return bit;
}

static int length_bucket(unsigned long long length) {
    if (length < 4)
        return (int)length;
    int width = std::bit_width(length);
    return std::min(4 + (width - 3) * 2 + (int)((length >> (width - 2)) & 1), LENGTH_BUCKETS - 1);
}

// a gap tree: GAP_SLOT_BITS of length, then the top TREE_DEPTH bits, then the rest by position
constexpr int SLOT_NODES = 1 << GAP_SLOT_BITS;
constexpr int HIGH_NODES = (GAP_MAX_LEN + 1) * (1 << TREE_DEPTH);
using GapTree = std::array<Tally, SLOT_NODES + HIGH_NODES + (GAP_MAX_LEN + 1) * GAP_MAX_LEN>;

struct EventOdds {
    std::array<std::array<Tally, MATCH_KINDS>, LENGTH_BUCKETS> match{};
    std::array<std::array<Tally, CHANNEL_NODES>, SYMBOL_KINDS * SYMBOL_KINDS> channel_pair{};
    std::array<std::array<Tally, CHANNEL_NODES>, SYMBOL_KINDS> channel_last{};
    std::array<std::array<Tally, CHANNEL_NODES>, HELD_STATES * SYMBOL_KINDS> channel_held{};
    std::array<Blend, CHANNEL_NODES> channel_blend{};
    std::array<Tally, HOLD_BASES * SHORT_GAPS> hold_short{};
    std::array<Tally, HOLD_BASES> hold_state{};
    std::array<Tally, 2 * LENGTH_BUCKETS> hold_gap{};
    std::array<Blend, 2> hold_blend{};
    std::array<GapTree, GAP_BASES> gap_kind{};
    std::array<GapTree, GAP_BASES * LENGTH_BUCKETS> gap_same{}, gap_last{};
    std::array<Blend, GAP_BASES * GAP_STAGES> gap_blend{};
    std::array<TallyNumbers, POSE_FIELDS * 2> pose{};
    std::vector<int> recent = std::vector<int>(1 << MATCH_SLOT_BITS, -1);
};

static BlendCells gap_cells(EventOdds& odds, int base, int same_bucket, int last_bucket, int node) {
    return { &odds.gap_kind[base][node], &odds.gap_same[base * LENGTH_BUCKETS + same_bucket][node], &odds.gap_last[base * LENGTH_BUCKETS + last_bucket][node] };
}

static void write_gap(RangeWriter& writer, EventOdds& odds, int base, int same_bucket, int last_bucket, unsigned long long number) {
    int len = std::bit_width(number);
    int node = 1;
    for (int i = GAP_SLOT_BITS - 1; i >= 0; --i) {
        int bit = (len >> i) & 1;
        write_blend(writer, gap_cells(odds, base, same_bucket, last_bucket, node), odds.gap_blend[base * GAP_STAGES], bit, 1);
        node = (node << 1) | bit;
    }
    if (len <= 1)
        return;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = (int)((number >> i) & 1);
        if (depth < TREE_DEPTH) {
            write_blend(writer, gap_cells(odds, base, same_bucket, last_bucket, SLOT_NODES + len * (1 << TREE_DEPTH) + tree), odds.gap_blend[base * GAP_STAGES + 1], bit, 1);
            tree = (tree << 1) | bit;
        }
        else {
            write_blend(writer, gap_cells(odds, base, same_bucket, last_bucket, SLOT_NODES + HIGH_NODES + len * GAP_MAX_LEN + i), odds.gap_blend[base * GAP_STAGES + 2], bit, 1);
        }
    }
}

static unsigned long long read_gap(RangeReader& reader, EventOdds& odds, int base, int same_bucket, int last_bucket) {
    int node = 1;
    for (int i = GAP_SLOT_BITS - 1; i >= 0; --i)
        node = (node << 1) | read_blend(reader, gap_cells(odds, base, same_bucket, last_bucket, node), odds.gap_blend[base * GAP_STAGES], 1);
    int len = node - SLOT_NODES;
    if (len > GAP_MAX_LEN) {
        reader.failed = true;
        return 0;
    }
    if (len <= 1)
        return (unsigned long long)len;
    unsigned long long res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = 0;
        if (depth < TREE_DEPTH) {
            bit = read_blend(reader, gap_cells(odds, base, same_bucket, last_bucket, SLOT_NODES + len * (1 << TREE_DEPTH) + tree), odds.gap_blend[base * GAP_STAGES + 1], 1);
            tree = (tree << 1) | bit;
        }
        else {
            bit = read_blend(reader, gap_cells(odds, base, same_bucket, last_bucket, SLOT_NODES + HIGH_NODES + len * GAP_MAX_LEN + i), odds.gap_blend[base * GAP_STAGES + 2], 1);
        }
        res = (res << 1) | (unsigned long long)bit;
    }
    return res;
}

// plain bits, every bit 50/50
static void write_plain(RangeWriter& writer, unsigned long long value, int width) {
    for (int i = width - 1; i >= 0; --i)
        writer.bit(ODDS_HALF, (int)((value >> i) & 1));
}

static unsigned long long read_plain(RangeReader& reader, int width) {
    unsigned long long res = 0;
    for (int i = width - 1; i >= 0; --i)
        res |= (unsigned long long)reader.bit(ODDS_HALF) << i;
    return res;
}

static void write_count(RangeWriter& writer, unsigned long long value) {
    int len = std::bit_width(value);
    write_plain(writer, (unsigned long long)len, COUNT_BITS);
    if (len > 1)
        write_plain(writer, value, len - 1);
}

static unsigned long long read_count(RangeReader& reader) {
    int len = (int)read_plain(reader, COUNT_BITS);
    if (len > 64) {
        reader.failed = true;
        return 0;
    }
    if (len <= 1)
        return (unsigned long long)len;
    return (1ull << (len - 1)) | read_plain(reader, len - 1);
}

// a float or double's bits: how many 0s at the bottom, then whats above the lowest 1
static void write_bits(RangeWriter& writer, unsigned long long bits, int width) {
    int zeros = bits == 0 ? width : std::countr_zero(bits);
    write_plain(writer, (unsigned long long)zeros, width == 64 ? 7 : 6);
    if (zeros >= width)
        return;
    write_plain(writer, zeros + 1 < 64 ? bits >> (zeros + 1) : 0, width - zeros - 1);
}

static unsigned long long read_bits(RangeReader& reader, int width) {
    int zeros = (int)read_plain(reader, width == 64 ? 7 : 6);
    if (zeros >= width) {
        if (zeros > width)
            reader.failed = true;
        return 0;
    }
    unsigned long long rest = read_plain(reader, width - zeros - 1);
    return ((rest << 1) | 1) << zeros;
}

static void write_rates(RangeWriter& writer, const std::vector<RateChange>& rates) {
    int last = 0;
    unsigned prev = 0;
    for (const auto& change : rates) {
        write_count(writer, (unsigned long long)std::max(change.frame - last, 0));
        write_bits(writer, std::bit_cast<unsigned>(change.fps) ^ prev, 32);
        last = change.frame;
        prev = std::bit_cast<unsigned>(change.fps);
    }
}

static void read_rates(RangeReader& reader, std::vector<RateChange>& rates, size_t count) {
    int last = 0;
    unsigned prev = 0;
    rates.clear();
    for (size_t i = 0; i < count && !reader.failed; ++i) {
        unsigned long long gap = read_count(reader);
        unsigned bits = (unsigned)read_bits(reader, 32);
        if (gap > (unsigned long long)(INT_MAX - last)) {
            reader.failed = true;
            break;
        }
        last += (int)gap;
        bits ^= prev;
        float fps = std::bit_cast<float>(bits);
        if (!(fps >= 1.f) || !std::isfinite(fps)) {
            reader.failed = true;
            break;
        }
        rates.push_back({ last, fps });
        prev = bits;
    }
}

// a click is stored as 1 number: (frames since the last click << SYMBOL_SHIFT) | (channel << 1) | holding
// channel = button (0 jump, 1 left, 2 right) + BUTTONS for player 2
struct Symbol {
    unsigned long long gap = 0;
    int channel = 0;
    int hold = 0;

    unsigned long long bits() const {
        return (gap << SYMBOL_SHIFT) | (unsigned long long)((channel << 1) | hold);
    }

    static Symbol unpack(unsigned long long raw) {
        return { raw >> SYMBOL_SHIFT, (int)((raw >> 1) & 7), (int)(raw & 1) };
    }

    static Symbol from_input(const Frame& input, int last_frame) {
        int channel = (input.player2 ? BUTTONS : 0) + ReplayIO::button_code(input.button);
        return { (unsigned long long)(input.number - last_frame), channel, input.holding ? 1 : 0 };
    }

    Frame to_input(int frame) const {
        Frame input;
        input.number = frame;
        input.holding = hold != 0;
        input.player2 = channel >= BUTTONS;
        input.button = ReplayIO::code_button(channel % BUTTONS);
        return input;
    }

    bool overflows(int last_frame) const {
        return gap > (unsigned long long)(INT_MAX - last_frame) || channel >= CHANNELS;
    }
};

// where the last MIN_MATCH clicks were seen before. this exact math is part of the file format: old saves only load if it never changes
static unsigned match_slot(const std::vector<unsigned long long>& history) {
    unsigned long long hash = MIN_MATCH * MATCH_HASH_START;
    for (size_t k = history.size() - MIN_MATCH; k < history.size(); ++k)
        hash = (hash ^ history[k]) * MATCH_HASH_MIX + MATCH_HASH_ADD;
    return (unsigned)(hash >> (64 - MATCH_SLOT_BITS));
}

// clicks repeat a lot, so the writer & the reader both remember what came after the same clicks last time,
// & store "same as last time" in 1 bit when its right
struct EventHistory {
    std::vector<unsigned long long> history;
    size_t ahead = 0;
    unsigned long long matched = 0;
    int misses = 0;
    std::array<int, CHANNELS> state{};
    std::array<unsigned long long, SYMBOL_KINDS> last_gap_of{};
    int last_frame = 0;

    int symbol_at(size_t back) const {
        return history.size() > back ? (int)(history[history.size() - 1 - back] & ((1 << SYMBOL_SHIFT) - 1)) : 0;
    }

    Tally& match_tally(EventOdds& odds) const {
        unsigned long long guess_gap = history[ahead] >> SYMBOL_SHIFT;
        int gap_shape = guess_gap == 0 ? 0 : guess_gap == 1 ? 1 : 2;
        return odds.match[length_bucket(matched)][gap_shape * 2 + std::min(misses, 1)];
    }

    void remember(EventOdds& odds, unsigned long long got, const Symbol& symbol, int frame) {
        if (matched > 0 && history[ahead] == got) {
            ++matched;
            ++ahead;
        }
        else {
            matched = 0;
        }
        history.push_back(got);
        if (history.size() >= MIN_MATCH) {
            unsigned slot = match_slot(history);
            int at = odds.recent[slot];
            if (matched == 0 && at >= 0) {
                unsigned long long len = 0;
                while (len < (unsigned long long)at && len < LONGEST_CHECK) {
                    size_t here = history.size() - 1 - len;
                    size_t there = (size_t)at - 1 - len;
                    if (history[there] != history[here])
                        break;
                    ++len;
                }
                if (len >= MIN_MATCH) {
                    matched = len;
                    ahead = (size_t)at;
                }
            }
            odds.recent[slot] = (int)history.size();
        }
        state[symbol.channel] = symbol.hold ? 1 : 2;
        last_gap_of[symbol.channel * 2 + symbol.hold] = symbol.gap;
        last_frame = frame;
    }
};

// what the channel, hold & gap of a click are guessed from
struct ClickContext {
    int last = 0, prev = 0, held = 0;
    unsigned long long last_gap = 0;

    explicit ClickContext(const EventHistory& events) {
        last = events.symbol_at(0);
        prev = events.symbol_at(1);
        last_gap = events.history.empty() ? 0 : events.history.back() >> SYMBOL_SHIFT;
        held = events.state[0] + events.state[1] * 3 + events.state[3] * 9 + events.state[4] * 27;
    }

    BlendCells channel_cells(EventOdds& odds, int node) const {
        return { &odds.channel_pair[last * SYMBOL_KINDS + prev][node], &odds.channel_last[last][node], &odds.channel_held[held * SYMBOL_KINDS + last][node] };
    }

    int hold_base(const EventHistory& events, int channel) const {
        int same = channel == (last >> 1) ? 1 : 0;
        return (events.state[channel] * 2 + (last & 1)) * 2 + same;
    }

    BlendCells hold_cells(EventOdds& odds, const EventHistory& events, int channel) const {
        int base = hold_base(events, channel);
        int short_gap = (int)(std::min<unsigned long long>(last_gap, SHORT_GAPS - 1));
        int gap_band = (events.state[channel] == 1 ? LENGTH_BUCKETS : 0) + length_bucket(last_gap);
        return { &odds.hold_short[base * SHORT_GAPS + short_gap], &odds.hold_state[base], &odds.hold_gap[gap_band] };
    }
};

static void write_click(RangeWriter& writer, EventOdds& odds, const EventHistory& events, unsigned long long want) {
    ClickContext context(events);
    int channel = (int)((want >> 1) & 7);
    int player2 = channel >= BUTTONS ? 1 : 0;
    int high = (channel % BUTTONS) >> 1;
    int low = high ? 0 : (channel % BUTTONS) & 1;
    write_blend(writer, context.channel_cells(odds, 0), odds.channel_blend[0], player2, 2);
    write_blend(writer, context.channel_cells(odds, 1 + player2), odds.channel_blend[1 + player2], high, 2);
    if (!high)
        write_blend(writer, context.channel_cells(odds, 3 + player2), odds.channel_blend[3 + player2], low, 2);
    int same = channel == (context.last >> 1) ? 1 : 0;
    int hold = (int)(want & 1);
    write_blend(writer, context.hold_cells(odds, events, channel), odds.hold_blend[same], hold, 2);
    write_gap(writer, odds, hold * 2 + same, length_bucket(events.last_gap_of[channel * 2 + hold]), length_bucket(context.last_gap), want >> SYMBOL_SHIFT);
}

static unsigned long long read_click(RangeReader& reader, EventOdds& odds, const EventHistory& events) {
    ClickContext context(events);
    int player2 = read_blend(reader, context.channel_cells(odds, 0), odds.channel_blend[0], 2);
    int high = read_blend(reader, context.channel_cells(odds, 1 + player2), odds.channel_blend[1 + player2], 2);
    int low = high ? 0 : read_blend(reader, context.channel_cells(odds, 3 + player2), odds.channel_blend[3 + player2], 2);
    int channel = player2 * BUTTONS + ((high << 1) | low);
    int same = channel == (context.last >> 1) ? 1 : 0;
    int hold = read_blend(reader, context.hold_cells(odds, events, channel), odds.hold_blend[same], 2);
    unsigned long long gap = read_gap(reader, odds, hold * 2 + same, length_bucket(events.last_gap_of[channel * 2 + hold]), length_bucket(context.last_gap));
    return Symbol{ gap, channel, hold }.bits();
}

static void write_clicks(RangeWriter& writer, EventOdds& odds, const std::vector<Frame>& inputs) {
    EventHistory events;
    events.history.reserve(std::min<size_t>(inputs.size(), RESERVE_LIMIT));
    for (const auto& input : inputs) {
        unsigned long long want = Symbol::from_input(input, events.last_frame).bits();
        bool guessed = false;
        if (events.matched > 0) {
            guessed = want == events.history[events.ahead];
            writer.write(events.match_tally(odds), guessed ? 1 : 0, STEADY);
            events.misses = guessed ? 0 : events.misses + 1;
        }
        if (!guessed)
            write_click(writer, odds, events, want);
        Symbol symbol = Symbol::unpack(want);
        if (symbol.overflows(events.last_frame))
            break;
        events.remember(odds, want, symbol, events.last_frame + (int)symbol.gap);
    }
}

static void read_clicks(RangeReader& reader, EventOdds& odds, std::vector<Frame>& inputs, size_t count) {
    EventHistory events;
    events.history.reserve(std::min<size_t>(count, RESERVE_LIMIT));
    for (size_t i = 0; i < count && !reader.failed; ++i) {
        unsigned long long got = 0;
        bool guessed = false;
        if (events.matched > 0) {
            guessed = reader.read(events.match_tally(odds), STEADY) != 0;
            if (guessed)
                got = events.history[events.ahead];
            events.misses = guessed ? 0 : events.misses + 1;
        }
        if (!guessed)
            got = read_click(reader, odds, events);
        Symbol symbol = Symbol::unpack(got);
        if (symbol.overflows(events.last_frame)) {
            reader.failed = true;
            break;
        }
        int frame = events.last_frame + (int)symbol.gap;
        inputs.push_back(symbol.to_input(frame));
        events.remember(odds, got, symbol, frame);
    }
}

int ReplayIO::button_code(int button) {
    if (button == LEFT)
        return 1;
    if (button == RIGHT)
        return 2;
    return 0;
}

int ReplayIO::code_button(int code) {
    if (code == 1)
        return LEFT;
    if (code == 2)
        return RIGHT;
    return JUMP;
}

template <typename T>
void ReplayIO::write_pod(std::ostream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
bool ReplayIO::read_pod(std::istream& file, T& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return (bool)file;
}

static bool read_varint(std::istream& file, unsigned long long& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        int byte = file.get();
        if (byte == EOF)
            return false;
        value |= (unsigned long long)(byte & 127) << shift;
        if ((byte & 128) == 0)
            return true;
    }
    return false;
}

constexpr unsigned char FLAG_SAMPLES = 1, FLAG_WHOLE_FPS = 2, FLAG_SEED = 4, FLAG_UNROUNDED = 8;
constexpr unsigned char FLAG_SILICATE_VARIANCE = 16;
constexpr unsigned char FLAG_LDM_KNOWN = 32, FLAG_LDM = 64;

struct EchoBody {
    float fps = 240.f;
    float end_x = 0.f;
    double seconds = 0.0;
    unsigned long long attempts = 0;
    unsigned long long seed = 0;
    bool has_seed = false;
    bool unrounded = false;
    bool silicate_variance = false;
    bool ldm = false, ldm_known = false;
    std::vector<Frame> inputs;
    std::vector<FramePose> poses;
    std::vector<RateChange> rates;
};

static std::vector<Frame> inputs_in_file_order(std::vector<Frame> inputs, std::vector<FramePose>* poses) {
    std::vector<size_t> order(inputs.size());
    std::iota(order.begin(), order.end(), 0);
    for (auto& input : inputs)
        input.number = std::max(input.number, 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        // by frame ONLY, the order on a frame is kept as is (see Logic::sort_inputs)
        return inputs[a].number < inputs[b].number;
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

static std::vector<unsigned char> read_rest(std::istream& file) {
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

static RangeReader open_range(const std::vector<unsigned char>& bytes) {
    RangeReader reader;
    reader.data = bytes.data();
    reader.size = bytes.size();
    reader.start();
    return reader;
}

static bool take(const std::vector<unsigned char>& bytes, size_t& cursor, void* out, size_t size) {
    if (cursor + size > bytes.size())
        return false;
    std::memcpy(out, bytes.data() + cursor, size);
    cursor += size;
    return true;
}

static void read_version1_poses(const std::vector<Frame>& inputs, std::vector<FramePose>& poses, const std::vector<unsigned char>& bytes, const BitReader& stream) {
    size_t cursor = (stream.index * 8 - (size_t)stream.left) / 8;
    if (stream.left != 0)
        ++cursor;
    poses.resize(inputs.size());
    for (auto& pose : poses) {
        if (!take(bytes, cursor, &pose.x, sizeof(pose.x)) || !take(bytes, cursor, &pose.y, sizeof(pose.y)) || !take(bytes, cursor, &pose.rotation, sizeof(pose.rotation)))
            break;
        if (!take(bytes, cursor, &pose.y_velocity, sizeof(pose.y_velocity)) || !take(bytes, cursor, &pose.x_velocity, sizeof(pose.x_velocity)))
            break;
    }
}

static bool load_version1(std::istream& file, unsigned char flags, EchoBody& out) {
    unsigned short reserved = 0;
    unsigned attempts = 0;
    unsigned amt = 0;
    if (!ReplayIO::read_pod(file, reserved))
        return false;
    if (!ReplayIO::read_pod(file, out.fps) || !ReplayIO::read_pod(file, out.end_x) || !ReplayIO::read_pod(file, out.seconds))
        return false;
    if (!ReplayIO::read_pod(file, attempts) || !ReplayIO::read_pod(file, amt))
        return false;
    out.attempts = attempts;
    if (amt > ReplayIO::max_inputs)
        return false;
    auto bytes = read_rest(file);
    BitReader stream;
    stream.data = bytes.data();
    stream.size = bytes.size();
    out.inputs.reserve(std::min<unsigned long long>(amt, bytes.size() * 8 / 5 + 1));
    int frame = 0;
    for (unsigned i = 0; i < amt; ++i) {
        Frame input;
        input.holding = stream.bit() != 0;
        input.player2 = stream.bit() != 0;
        int high = stream.bit();
        int code = (high << 1) | stream.bit();
        input.button = ReplayIO::code_button(code);
        unsigned delta = stream.gamma();
        if (delta == 0)
            return false;
        frame += (int)(delta - 1);
        input.number = frame;
        out.inputs.push_back(input);
    }
    if (flags & FLAG_SAMPLES)
        read_version1_poses(out.inputs, out.poses, bytes, stream);
    return true;
}

static bool load_version2(std::istream& file, unsigned char flags, EchoBody& out) {
    if (flags & FLAG_WHOLE_FPS) {
        unsigned long long whole = 0;
        if (!read_varint(file, whole))
            return false;
        out.fps = (float)whole;
    }
    else if (!ReplayIO::read_pod(file, out.fps)) {
        return false;
    }
    if (!ReplayIO::read_pod(file, out.end_x) || !ReplayIO::read_pod(file, out.seconds))
        return false;
    unsigned long long amt = 0;
    if (!read_varint(file, out.attempts) || !read_varint(file, amt))
        return false;
    if (amt > ReplayIO::max_inputs)
        return false;
    auto bytes = read_rest(file);
    RangeReader reader = open_range(bytes);
    out.inputs.reserve((size_t)(std::min<unsigned long long>(amt, RESERVE_LIMIT)));
    read_version2_inputs(reader, out.inputs, out.poses, (size_t)amt, flags & FLAG_SAMPLES);
    return !reader.failed && out.inputs.size() == amt;
}

static bool load_version3(std::istream& file, unsigned char flags, int version, EchoBody& out) {
    auto bytes = read_rest(file);
    RangeReader reader = open_range(bytes);
    auto odds = std::make_unique<EventOdds>();
    out.fps = std::bit_cast<float>((unsigned)read_bits(reader, 32));
    out.end_x = std::bit_cast<float>((unsigned)read_bits(reader, 32));
    out.seconds = std::bit_cast<double>(read_bits(reader, 64));
    out.attempts = read_count(reader);
    unsigned long long amt = read_count(reader);
    if (reader.failed || amt > ReplayIO::max_inputs || out.attempts > INT_MAX)
        return false;
    out.inputs.reserve((size_t)(std::min<unsigned long long>(amt, RESERVE_LIMIT)));
    read_clicks(reader, *odds, out.inputs, (size_t)amt);
    if (!reader.failed && (flags & FLAG_SAMPLES))
        read_poses(reader, out.inputs, out.poses, odds->pose);
    if (version == 4 && !reader.failed) {
        unsigned long long rate_count = read_count(reader);
        if (reader.failed || rate_count > MAX_RATE_CHANGES)
            return false;
        read_rates(reader, out.rates, (size_t)rate_count);
        if (reader.failed || out.rates.size() != rate_count)
            return false;
    }
    if (!reader.failed && (flags & FLAG_SEED)) {
        out.seed = read_bits(reader, 64);
        out.has_seed = !reader.failed;
    }
    return !reader.failed && out.inputs.size() == amt;
}

static void store_echo_body(EchoBody& body, MacroSnap& out) {
    out.inputs = std::move(body.inputs);
    out.poses = std::move(body.poses);
    if (out.poses.size() != out.inputs.size())
        out.poses.clear();
    out.rates = std::move(body.rates);
    out.fps = body.fps > 0.f ? body.fps : 240.f;
    out.end_x = body.end_x;
    out.recording_seconds = body.seconds;
    out.attempts = (int)body.attempts;
    out.lvl_seed = body.seed;
    out.lvl_seed_known = body.has_seed;
    out.unrounded = body.unrounded;
    out.silicate_variance = body.silicate_variance;
    out.ldm = body.ldm;
    out.ldm_known = body.ldm_known;
    out.format = ReplayFormat::Meta;
}

bool ReplayIO::write_echo(const fs::path& path, const MacroSnap& snap) {
    auto rates = snap.rates;
    tidy_rates(rates, snap.fps);
    auto poses = snap.poses.size() == snap.inputs.size() ? snap.poses : std::vector<FramePose>{};
    auto inputs = inputs_in_file_order(snap.inputs, poses.empty() ? nullptr : &poses);
    bool samples = snap.save_debug && poses.size() == inputs.size();

    auto odds = std::make_unique<EventOdds>();
    RangeWriter writer;
    float header_fps = !rates.empty() ? snap.fps : (float)ReplayIO::snap_sample_rate(snap);
    write_bits(writer, std::bit_cast<unsigned>(header_fps), 32);
    write_bits(writer, std::bit_cast<unsigned>(snap.end_x), 32);
    write_bits(writer, std::bit_cast<unsigned long long>(snap.recording_seconds), 64);
    write_count(writer, (unsigned long long)std::max(snap.attempts, 0));
    write_count(writer, inputs.size());
    write_clicks(writer, *odds, inputs);
    if (samples)
        write_poses(writer, inputs, poses, odds->pose);
    if (!rates.empty()) {
        write_count(writer, rates.size());
        write_rates(writer, rates);
    }
    if (snap.lvl_seed_known)
        write_bits(writer, snap.lvl_seed, 64);
    writer.finish();

    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[4] = { 'E', 'C', 'H', '2' };
    file.write(magic, 4);
    unsigned char version = !rates.empty() ? 4 : 3;
    unsigned char flags = samples ? FLAG_SAMPLES : 0;
    if (snap.lvl_seed_known)
        flags |= FLAG_SEED;
    if (snap.unrounded)
        flags |= FLAG_UNROUNDED;
    if (snap.silicate_variance)
        flags |= FLAG_SILICATE_VARIANCE;
    if (snap.ldm_known)
        flags |= FLAG_LDM_KNOWN | (snap.ldm ? FLAG_LDM : 0);
    ReplayIO::write_pod(file, version);
    ReplayIO::write_pod(file, flags);
    if (!writer.bytes.empty())
        file.write(reinterpret_cast<const char*>(writer.bytes.data()), (std::streamsize)writer.bytes.size());
    return (bool)file;
}

bool ReplayIO::read_echo(const fs::path& path, MacroSnap& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[4] = {};
    file.read(magic, 4);
    if (!file || std::string_view(magic, 4) != "ECH2")
        return false;
    unsigned char version = 0;
    unsigned char flags = 0;
    if (!ReplayIO::read_pod(file, version) || !ReplayIO::read_pod(file, flags))
        return false;

    EchoBody body;
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
    store_echo_body(body, out);
    return true;
}
