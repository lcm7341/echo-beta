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

// all these numbers are part of the .echo format. change any of them & old saves wont load anymore

// a guess is how sure we are the next bit is a 0, out of 65536
#define CHANCE_SCALE 65536
#define CHANCE_HALF 32768
// never 100% sure either way
#define CHANCE_EDGE 64
// old guesses stop learning after this many bits
#define OLD_GUESS_MEMORY 10
// newer guesses count out of 2^32 instead
const unsigned GUESS_START = 1u << 31;
#define GUESS_MAX 4294967295ll
// how fast a guess learns. QUICK for most stuff, STEADY for the "same as last time" guess
#define QUICK 30
#define STEADY 4000

#define FULL_RANGE 4294967295u
// once the compressors range gets this small it writes a byte to the file
const unsigned RANGE_TOP = 1u << 24;
// if the next byte is 255 it could still change, so it waits
const unsigned CARRY_ZONE = 255u << 24;
#define FLUSH_BYTES 5
// how far a broken file can make the reader run off the end before we give up
const size_t READ_PAST_END = 64;

// numbers are saved as how many bits long they are, then the bits themselves. the first TREE_DEPTH bits get their own guesses
#define NUMBER_SLOT_BITS 7
#define NUMBER_MAX_LEN 64
#define GAP_SLOT_BITS 6
#define GAP_MAX_LEN 32
#define TREE_DEPTH 4
#define COUNT_BITS 7

// x, y & rotation are floats, the 2 velocities are doubles
#define POSE_FIELDS 5
#define POSE_FLOATS 3

// a channel is 1 button (jump, left, right) of 1 player
#define BUTTONS 3
#define CHANNELS (BUTTONS * 2)
// a click kind is its channel & whether its a press or a release
#define CLICK_KINDS (CHANNELS * 2)
#define CLICK_SHIFT 4
// each channel is pressed, released or not used yet. 4 of them get looked at together (3^4 = 81)
#define HELD_STATES 81
#define CHANNEL_NODES 5
#define HOLD_BASES 12
#define SHORT_GAPS 6
#define GAP_BASES 4
#define GAP_STAGES 3
#define LENGTH_BUCKETS 40
#define MATCH_KINDS 6

// when guesses get mixed theyre out of 4096 instead
#define CHANCE_MAX 4095
#define SCORE_LIMIT 2047
#define MIX_PARTS 3
#define MIX_ONE (1 << 16)
#define MIX_BIAS 256

#define MIN_MATCH 4
const unsigned long long LONGEST_CHECK = 64;
#define MATCH_SLOT_BITS 18
const unsigned long long MATCH_HASH_START = 2685821657736338717ull;
const unsigned long long MATCH_HASH_MIX = 11400714819323198485ull;
const unsigned long long MATCH_HASH_ADD = 7146057691288625177ull;

const unsigned long long MAX_RATE_CHANGES = 1ull << 16;
const size_t RESERVE_LIMIT = 1u << 20;

// version 1 files are just plain bits
struct BitReader {
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t index = 0;
    int left = 0;
    unsigned char acc = 0;

    int bit() {
        if (left == 0) {
            if (index >= size) return 0;
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
            if (++zeros > 31 || index > size) return 0;
        }
        unsigned n = 1;
        for (unsigned i = 0; i < zeros; ++i)
            n = (n << 1) | (unsigned)bit();
        return n;
    }
};

// version 2 guesses
struct OldGuess {
    unsigned short value = CHANCE_HALF;
    unsigned char seen = 0;

    unsigned chance() const {
        return std::clamp<unsigned>(value, CHANCE_EDGE, CHANCE_SCALE - 1 - CHANCE_EDGE);
    }

    void learn(int bit) {
        int target = bit ? 0 : CHANCE_SCALE - 1;
        value = (unsigned short)(value + (target - (int)value) / ((int)seen + 2));
        if (seen < OLD_GUESS_MEMORY) ++seen;
    }
};

// version 3 & 4 guesses
struct Guess {
    unsigned one = GUESS_START;
    unsigned short seen = 0;

    unsigned zero_chance() const {
        return CHANCE_SCALE - std::clamp<unsigned>(one >> 16, 1, CHANCE_SCALE - 1);
    }

    int one_chance() const {
        return (int)(one >> 20);
    }

    void learn(int bit, int cap) {
        long long target = bit ? GUESS_MAX : 0;
        one = (unsigned)(one + (target - (long long)one) / (seen + 2));
        if (seen < cap) ++seen;
    }
};

// every code_ function below both saves & loads. the Compressor writes the value its given,
// the Decompressor ignores it & returns what it read instead
struct Compressor {
    static const bool reading = false;
    std::vector<unsigned char> bytes;
    unsigned long long low = 0;
    unsigned range = FULL_RANGE;
    unsigned char cache = 0;
    unsigned long long pending = 1;
    bool first = true;
    bool failed = false;

    void shift_low() {
        if ((unsigned)low < CARRY_ZONE || (low >> 32) != 0) {
            unsigned char carry = (unsigned char)(low >> 32);
            unsigned char out = cache;
            do {
                if (!first) bytes.push_back((unsigned char)(out + carry));
                first = false;
                out = 255;
            } while (--pending != 0);
            cache = (unsigned char)(low >> 24);
        }
        ++pending;
        low = (low & (RANGE_TOP - 1)) << 8;
    }

    int bit(unsigned zero, int value) {
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
        return value;
    }

    int code(Guess& guess, int value, int cap = QUICK) {
        bit(guess.zero_chance(), value);
        guess.learn(value, cap);
        return value;
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

struct Decompressor {
    static const bool reading = true;
    const unsigned char* data = nullptr;
    size_t size = 0;
    size_t index = 0;
    unsigned range = FULL_RANGE;
    unsigned value = 0;
    bool failed = false;

    unsigned char next() {
        if (index >= size) {
            if (++index > size + READ_PAST_END) failed = true;
            return 0;
        }
        return data[index++];
    }

    void start() {
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | next();
    }

    int bit(unsigned zero, int = 0) {
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

    int code(OldGuess& guess, int = 0) {
        int res = bit(guess.chance());
        guess.learn(res);
        return res;
    }

    int code(Guess& guess, int = 0, int cap = QUICK) {
        int res = bit(guess.zero_chance());
        guess.learn(res, cap);
        return res;
    }
};

struct OldNumberGuesses {
    int slot_bits = NUMBER_SLOT_BITS;
    int max_len = NUMBER_MAX_LEN;
    std::array<OldGuess, 1 << NUMBER_SLOT_BITS> slot{};
    std::array<std::array<OldGuess, 1 << TREE_DEPTH>, NUMBER_MAX_LEN + 1> high{};
    std::array<std::array<OldGuess, NUMBER_MAX_LEN>, NUMBER_MAX_LEN + 1> low{};
};

struct NumberGuesses {
    static const int slot_bits = NUMBER_SLOT_BITS;
    static const int max_len = NUMBER_MAX_LEN;
    std::array<Guess, 1 << NUMBER_SLOT_BITS> slot{};
    std::array<std::array<Guess, 1 << TREE_DEPTH>, NUMBER_MAX_LEN + 1> high{};
    std::array<std::array<Guess, NUMBER_MAX_LEN>, NUMBER_MAX_LEN + 1> low{};
};

template <typename Coder, typename Guesses>
static unsigned long long code_number(Coder& coder, Guesses& guesses, unsigned long long number) {
    int len = std::bit_width(number);
    int node = 1;
    for (int i = guesses.slot_bits - 1; i >= 0; --i)
        node = (node << 1) | coder.code(guesses.slot[node], (len >> i) & 1);
    len = node - (1 << guesses.slot_bits);
    if (len > guesses.max_len) {
        coder.failed = true;
        return 0;
    }
    if (len <= 1) return (unsigned long long)len;
    unsigned long long res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = (int)((number >> i) & 1);
        if (depth < TREE_DEPTH) {
            bit = coder.code(guesses.high[len][tree], bit);
            tree = (tree << 1) | bit;
        }
        else {
            bit = coder.code(guesses.low[len][i], bit);
        }
        res = (res << 1) | (unsigned long long)bit;
    }
    return res;
}

static unsigned long long zigzag(long long value) {
    return ((unsigned long long)value << 1) ^ (unsigned long long)(value >> 63);
}

static long long unzigzag(unsigned long long value) {
    return (long long)(value >> 1) ^ -(long long)(value & 1);
}

// poses only save how much they changed since that players last pose
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
    if (field >= POSE_FLOATS) return (long long)(now - before);
    return (long long)((int)((unsigned)now - (unsigned)before));
}

static unsigned long long apply_change(int field, unsigned long long before, long long change) {
    if (field >= POSE_FLOATS) return before + (unsigned long long)change;
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

template <typename Coder, typename Guesses>
static void code_poses(Coder& coder, const std::vector<Frame>& inputs, std::vector<FramePose>& poses, std::array<Guesses, POSE_FIELDS * 2>& guesses) {
    poses.resize(inputs.size());
    std::array<std::array<unsigned long long, POSE_FIELDS>, 2> prev{};
    for (size_t i = 0; i < inputs.size() && !coder.failed; ++i) {
        int player = inputs[i].player2 ? 1 : 0;
        auto fields = pose_fields(poses[i]);
        for (int field = 0; field < POSE_FIELDS; ++field) {
            unsigned long long change = code_number(coder, guesses[player * POSE_FIELDS + field], zigzag(field_change(field, fields[field], prev[player][field])));
            fields[field] = apply_change(field, prev[player][field], unzigzag(change));
            prev[player][field] = fields[field];
        }
        poses[i] = pose_from_fields(fields);
    }
}

struct OldClickGuesses {
    std::array<OldGuess, CHANNELS * 2> player{};
    std::array<std::array<OldGuess, 4>, CHANNELS * 4> button{};
    std::array<OldGuess, 6> hold{};
    std::array<OldNumberGuesses, GAP_BASES> gap{};
    std::array<OldNumberGuesses, POSE_FIELDS * 2> pose{};

    OldClickGuesses() {
        for (auto& numbers : gap) {
            numbers.slot_bits = GAP_SLOT_BITS;
            numbers.max_len = GAP_MAX_LEN;
        }
    }
};

static void read_version2_inputs(Decompressor& reader, std::vector<Frame>& inputs, std::vector<FramePose>& poses, size_t count, bool samples) {
    auto guesses = std::make_unique<OldClickGuesses>();
    std::array<int, CHANNELS> state{};
    int last_channel = 0, last_hold = 0, last_frame = 0;
    for (size_t i = 0; i < count && !reader.failed; ++i) {
        Frame input;
        int context = last_channel * 2 + last_hold;
        int player2 = reader.code(guesses->player[context]);
        auto& button = guesses->button[context * 2 + player2];
        int high = reader.code(button[1]);
        int low = reader.code(button[2 + high]);
        int code = (high << 1) | low;
        if (code == 3) {
            reader.failed = true;
            break;
        }
        int channel = player2 * BUTTONS + code;
        int hold = reader.code(guesses->hold[state[channel] * 2 + last_hold]);
        auto& gap = guesses->gap[hold * 2 + (channel == last_channel ? 1 : 0)];
        unsigned long long step = code_number(reader, gap, 0);
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
    if (samples && !reader.failed) code_poses(reader, inputs, poses, guesses->pose);
}

// 3 guesses get mixed into 1. it learns which one to trust more as it goes
// score_to_chance turns the mixed score back into a chance (a curve w/ a point every 128)
static int score_to_chance(int d) {
    static const int curve[33] = { 1, 2, 3, 6, 10, 16, 27, 45, 73, 120, 194, 310, 488, 747, 1101, 1546, 2047,
        2549, 2994, 3348, 3607, 3785, 3901, 3975, 4022, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094 };
    if (d > SCORE_LIMIT) return CHANCE_MAX;
    if (d < -SCORE_LIMIT) return 1;
    int w = d & 127;
    int i = (d >> 7) + 16;
    return (curve[i] * (128 - w) + curve[i + 1] * w + 64) >> 7;
}

static int chance_to_score(int p) {
    static const auto table = [] {
        std::array<short, CHANCE_MAX + 1> res{};
        int at = 0;
        for (int x = -SCORE_LIMIT; x <= SCORE_LIMIT; ++x) {
            int v = score_to_chance(x);
            for (int i = at; i <= v; ++i)
                res[i] = (short)x;
            at = v + 1;
        }
        for (int i = at; i <= CHANCE_MAX; ++i)
            res[i] = SCORE_LIMIT;
        return res;
    }();
    return table[p];
}

// the last weight is for a fixed number (MIX_BIAS), not a guess
struct Mix {
    std::array<int, MIX_PARTS + 1> weight;

    Mix() {
        weight.fill(MIX_ONE / MIX_PARTS);
        weight[MIX_PARTS] = 0;
    }
};

using MixGuesses = std::array<Guess*, MIX_PARTS>;

static int mix_chance(const MixGuesses& parts, const Mix& mix, std::array<int, MIX_PARTS + 1>& scores) {
    long long dot = 0;
    for (size_t i = 0; i < MIX_PARTS; ++i) {
        scores[i] = chance_to_score(parts[i]->one_chance());
        dot += (long long)mix.weight[i] * scores[i];
    }
    scores[MIX_PARTS] = MIX_BIAS;
    dot += (long long)mix.weight[MIX_PARTS] * scores[MIX_PARTS];
    return std::clamp(score_to_chance((int)(dot >> 16)), 1, CHANCE_MAX);
}

static void mix_learn(const MixGuesses& parts, Mix& mix, const std::array<int, MIX_PARTS + 1>& scores, int one, int bit, int rate) {
    int error = ((bit << 12) - one) * rate;
    for (size_t i = 0; i <= MIX_PARTS; ++i)
        mix.weight[i] += (scores[i] * error) >> 10;
    for (auto* cell : parts)
        cell->learn(bit, QUICK);
}

template <typename Coder>
static int code_mixed(Coder& coder, const MixGuesses& parts, Mix& mix, int bit, int rate) {
    std::array<int, MIX_PARTS + 1> scores;
    int one = mix_chance(parts, mix, scores);
    bit = coder.bit(CHANCE_SCALE - ((unsigned)one << 4), bit);
    mix_learn(parts, mix, scores, one, bit, rate);
    return bit;
}

static int length_bucket(unsigned long long length) {
    if (length < 4) return (int)length;
    int width = std::bit_width(length);
    return std::min(4 + (width - 3) * 2 + (int)((length >> (width - 2)) & 1), LENGTH_BUCKETS - 1);
}

// guesses for 1 gap: how many bits long it is, then its top bits, then the rest
#define SLOT_NODES (1 << GAP_SLOT_BITS)
#define HIGH_NODES ((GAP_MAX_LEN + 1) * (1 << TREE_DEPTH))
using GapGuesses = std::array<Guess, SLOT_NODES + HIGH_NODES + (GAP_MAX_LEN + 1) * GAP_MAX_LEN>;

struct ClickGuesses {
    std::array<std::array<Guess, MATCH_KINDS>, LENGTH_BUCKETS> match{};
    std::array<std::array<Guess, CHANNEL_NODES>, CLICK_KINDS * CLICK_KINDS> channel_pair{};
    std::array<std::array<Guess, CHANNEL_NODES>, CLICK_KINDS> channel_last{};
    std::array<std::array<Guess, CHANNEL_NODES>, HELD_STATES * CLICK_KINDS> channel_held{};
    std::array<Mix, CHANNEL_NODES> channel_mix{};
    std::array<Guess, HOLD_BASES * SHORT_GAPS> hold_short{};
    std::array<Guess, HOLD_BASES> hold_state{};
    std::array<Guess, 2 * LENGTH_BUCKETS> hold_gap{};
    std::array<Mix, 2> hold_mix{};
    std::array<GapGuesses, GAP_BASES> gap_kind{};
    std::array<GapGuesses, GAP_BASES * LENGTH_BUCKETS> gap_same{}, gap_last{};
    std::array<Mix, GAP_BASES * GAP_STAGES> gap_mix{};
    std::array<NumberGuesses, POSE_FIELDS * 2> pose{};
    std::vector<int> recent = std::vector<int>(1 << MATCH_SLOT_BITS, -1);
};

static MixGuesses gap_guesses(ClickGuesses& guesses, int base, int same_bucket, int last_bucket, int node) {
    return { &guesses.gap_kind[base][node], &guesses.gap_same[base * LENGTH_BUCKETS + same_bucket][node], &guesses.gap_last[base * LENGTH_BUCKETS + last_bucket][node] };
}

template <typename Coder>
static unsigned long long code_gap(Coder& coder, ClickGuesses& guesses, int base, int same_bucket, int last_bucket, unsigned long long number) {
    int len = std::bit_width(number);
    int node = 1;
    for (int i = GAP_SLOT_BITS - 1; i >= 0; --i)
        node = (node << 1) | code_mixed(coder, gap_guesses(guesses, base, same_bucket, last_bucket, node), guesses.gap_mix[base * GAP_STAGES], (len >> i) & 1, 1);
    len = node - SLOT_NODES;
    if (len > GAP_MAX_LEN) {
        coder.failed = true;
        return 0;
    }
    if (len <= 1) return (unsigned long long)len;
    unsigned long long res = 1;
    int tree = 1;
    for (int i = len - 2; i >= 0; --i) {
        int depth = len - 2 - i;
        int bit = (int)((number >> i) & 1);
        if (depth < TREE_DEPTH) {
            bit = code_mixed(coder, gap_guesses(guesses, base, same_bucket, last_bucket, SLOT_NODES + len * (1 << TREE_DEPTH) + tree), guesses.gap_mix[base * GAP_STAGES + 1], bit, 1);
            tree = (tree << 1) | bit;
        }
        else {
            bit = code_mixed(coder, gap_guesses(guesses, base, same_bucket, last_bucket, SLOT_NODES + HIGH_NODES + len * GAP_MAX_LEN + i), guesses.gap_mix[base * GAP_STAGES + 2], bit, 1);
        }
        res = (res << 1) | (unsigned long long)bit;
    }
    return res;
}

// plain bits, no guessing
template <typename Coder>
static unsigned long long code_plain(Coder& coder, unsigned long long value, int width) {
    unsigned long long res = 0;
    for (int i = width - 1; i >= 0; --i)
        res |= (unsigned long long)coder.bit(CHANCE_HALF, (int)((value >> i) & 1)) << i;
    return res;
}

template <typename Coder>
static unsigned long long code_count(Coder& coder, unsigned long long value) {
    int len = (int)code_plain(coder, (unsigned long long)std::bit_width(value), COUNT_BITS);
    if (len > 64) {
        coder.failed = true;
        return 0;
    }
    if (len <= 1) return (unsigned long long)len;
    return (1ull << (len - 1)) | code_plain(coder, value, len - 1);
}

// floats & doubles: how many 0 bits at the end, then the rest
template <typename Coder>
static unsigned long long code_bits(Coder& coder, unsigned long long bits, int width) {
    int zeros = (int)code_plain(coder, (unsigned long long)(bits == 0 ? width : std::countr_zero(bits)), width == 64 ? 7 : 6);
    if (zeros >= width) {
        if (zeros > width) coder.failed = true;
        return 0;
    }
    unsigned long long rest = code_plain(coder, zeros + 1 < 64 ? bits >> (zeros + 1) : 0, width - zeros - 1);
    return ((rest << 1) | 1) << zeros;
}

template <typename Coder>
static void code_rates(Coder& coder, std::vector<RateChange>& rates, size_t count) {
    int last = 0;
    unsigned prev = 0;
    for (size_t i = 0; i < count && !coder.failed; ++i) {
        RateChange change = Coder::reading ? RateChange{} : rates[i];
        unsigned long long gap = code_count(coder, (unsigned long long)std::max(change.frame - last, 0));
        unsigned bits = (unsigned)code_bits(coder, std::bit_cast<unsigned>(change.fps) ^ prev, 32) ^ prev;
        if (gap > (unsigned long long)(INT_MAX - last)) {
            coder.failed = true;
            break;
        }
        last += (int)gap;
        float fps = std::bit_cast<float>(bits);
        if (!(fps >= 1.f) || !std::isfinite(fps)) {
            coder.failed = true;
            break;
        }
        if (Coder::reading) rates.push_back({ last, fps });
        prev = bits;
    }
}

// each click is saved as 1 number: frames since the last click, which channel & press or release
// channel = button (0 jump, 1 left, 2 right), +3 for player 2
struct SavedClick {
    unsigned long long gap = 0;
    int channel = 0;
    int hold = 0;

    unsigned long long bits() const {
        return (gap << CLICK_SHIFT) | (unsigned long long)((channel << 1) | hold);
    }

    static SavedClick unpack(unsigned long long raw) {
        return { raw >> CLICK_SHIFT, (int)((raw >> 1) & 7), (int)(raw & 1) };
    }

    static SavedClick from_input(const Frame& input, int last_frame) {
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

// where these same 4 clicks showed up before. has to stay EXACTLY like this or old saves break
static unsigned match_slot(const std::vector<unsigned long long>& history) {
    unsigned long long hash = MIN_MATCH * MATCH_HASH_START;
    for (size_t k = history.size() - MIN_MATCH; k < history.size(); ++k)
        hash = (hash ^ history[k]) * MATCH_HASH_MIX + MATCH_HASH_ADD;
    return (unsigned)(hash >> (64 - MATCH_SLOT_BITS));
}

// macros repeat the same clicks a lot. saving & loading both remember what came next last time,
// so a click that matches costs almost nothing
struct PastClicks {
    std::vector<unsigned long long> history;
    size_t ahead = 0;
    unsigned long long matched = 0;
    int misses = 0;
    std::array<int, CHANNELS> state{};
    std::array<unsigned long long, CLICK_KINDS> last_gap_of{};
    int last_frame = 0;

    int click_at(size_t back) const {
        return history.size() > back ? (int)(history[history.size() - 1 - back] & ((1 << CLICK_SHIFT) - 1)) : 0;
    }

    Guess& match_guess(ClickGuesses& guesses) const {
        unsigned long long guess_gap = history[ahead] >> CLICK_SHIFT;
        int gap_shape = guess_gap == 0 ? 0 : guess_gap == 1 ? 1 : 2;
        return guesses.match[length_bucket(matched)][gap_shape * 2 + std::min(misses, 1)];
    }

    void remember(ClickGuesses& guesses, unsigned long long got, const SavedClick& click, int frame) {
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
            int at = guesses.recent[slot];
            if (matched == 0 && at >= 0) {
                unsigned long long len = 0;
                while (len < (unsigned long long)at && len < LONGEST_CHECK) {
                    size_t here = history.size() - 1 - len;
                    size_t there = (size_t)at - 1 - len;
                    if (history[there] != history[here]) break;
                    ++len;
                }
                if (len >= MIN_MATCH) {
                    matched = len;
                    ahead = (size_t)at;
                }
            }
            guesses.recent[slot] = (int)history.size();
        }
        state[click.channel] = click.hold ? 1 : 2;
        last_gap_of[click.channel * 2 + click.hold] = click.gap;
        last_frame = frame;
    }
};

// what we guess the next click from
struct ClickHints {
    int last = 0, prev = 0, held = 0;
    unsigned long long last_gap = 0;

    explicit ClickHints(const PastClicks& past) {
        last = past.click_at(0);
        prev = past.click_at(1);
        last_gap = past.history.empty() ? 0 : past.history.back() >> CLICK_SHIFT;
        held = past.state[0] + past.state[1] * 3 + past.state[3] * 9 + past.state[4] * 27;
    }

    MixGuesses channel_guesses(ClickGuesses& guesses, int node) const {
        return { &guesses.channel_pair[last * CLICK_KINDS + prev][node], &guesses.channel_last[last][node], &guesses.channel_held[held * CLICK_KINDS + last][node] };
    }

    int hold_base(const PastClicks& past, int channel) const {
        int same = channel == (last >> 1) ? 1 : 0;
        return (past.state[channel] * 2 + (last & 1)) * 2 + same;
    }

    MixGuesses hold_guesses(ClickGuesses& guesses, const PastClicks& past, int channel) const {
        int base = hold_base(past, channel);
        int short_gap = (int)(std::min<unsigned long long>(last_gap, SHORT_GAPS - 1));
        int gap_band = (past.state[channel] == 1 ? LENGTH_BUCKETS : 0) + length_bucket(last_gap);
        return { &guesses.hold_short[base * SHORT_GAPS + short_gap], &guesses.hold_state[base], &guesses.hold_gap[gap_band] };
    }
};

template <typename Coder>
static unsigned long long code_click(Coder& coder, ClickGuesses& guesses, const PastClicks& past, unsigned long long want) {
    ClickHints context(past);
    int channel = (int)((want >> 1) & 7);
    int player2 = code_mixed(coder, context.channel_guesses(guesses, 0), guesses.channel_mix[0], channel >= BUTTONS ? 1 : 0, 2);
    int high = code_mixed(coder, context.channel_guesses(guesses, 1 + player2), guesses.channel_mix[1 + player2], (channel % BUTTONS) >> 1, 2);
    int low = high ? 0 : code_mixed(coder, context.channel_guesses(guesses, 3 + player2), guesses.channel_mix[3 + player2], (channel % BUTTONS) & 1, 2);
    channel = player2 * BUTTONS + ((high << 1) | low);
    int same = channel == (context.last >> 1) ? 1 : 0;
    int hold = code_mixed(coder, context.hold_guesses(guesses, past, channel), guesses.hold_mix[same], (int)(want & 1), 2);
    unsigned long long gap = code_gap(coder, guesses, hold * 2 + same, length_bucket(past.last_gap_of[channel * 2 + hold]), length_bucket(context.last_gap), want >> CLICK_SHIFT);
    return SavedClick{ gap, channel, hold }.bits();
}

template <typename Coder>
static void code_clicks(Coder& coder, ClickGuesses& guesses, std::vector<Frame>& inputs, size_t count) {
    PastClicks past;
    past.history.reserve(std::min<size_t>(count, RESERVE_LIMIT));
    for (size_t i = 0; i < count && !coder.failed; ++i) {
        unsigned long long want = Coder::reading ? 0 : SavedClick::from_input(inputs[i], past.last_frame).bits();
        unsigned long long got = want;
        bool guessed = false;
        if (past.matched > 0) {
            guessed = coder.code(past.match_guess(guesses), want == past.history[past.ahead] ? 1 : 0, STEADY) != 0;
            if (guessed) got = past.history[past.ahead];
            past.misses = guessed ? 0 : past.misses + 1;
        }
        if (!guessed) got = code_click(coder, guesses, past, want);
        SavedClick click = SavedClick::unpack(got);
        if (click.overflows(past.last_frame)) {
            coder.failed = true;
            break;
        }
        int frame = past.last_frame + (int)click.gap;
        if (Coder::reading) inputs.push_back(click.to_input(frame));
        past.remember(guesses, got, click, frame);
    }
}

int ReplayIO::button_code(int button) {
    if (button == LEFT) return 1;
    if (button == RIGHT) return 2;
    return 0;
}

int ReplayIO::code_button(int code) {
    if (code == 1) return LEFT;
    if (code == 2) return RIGHT;
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
        if (byte == EOF) return false;
        value |= (unsigned long long)(byte & 127) << shift;
        if ((byte & 128) == 0) return true;
    }
    return false;
}

const unsigned char FLAG_SAMPLES = 1, FLAG_WHOLE_FPS = 2, FLAG_SEED = 4, FLAG_UNROUNDED = 8;
const unsigned char FLAG_SILICATE_VARIANCE = 16;
const unsigned char FLAG_LDM_KNOWN = 32, FLAG_LDM = 64;

struct EchoFile {
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

static Decompressor open_range(const std::vector<unsigned char>& bytes) {
    Decompressor reader;
    reader.data = bytes.data();
    reader.size = bytes.size();
    reader.start();
    return reader;
}

static bool take(const std::vector<unsigned char>& bytes, size_t& cursor, void* out, size_t size) {
    if (cursor + size > bytes.size()) return false;
    std::memcpy(out, bytes.data() + cursor, size);
    cursor += size;
    return true;
}

static void read_version1_poses(const std::vector<Frame>& inputs, std::vector<FramePose>& poses, const std::vector<unsigned char>& bytes, const BitReader& stream) {
    size_t cursor = (stream.index * 8 - (size_t)stream.left) / 8;
    if (stream.left != 0) ++cursor;
    poses.resize(inputs.size());
    for (auto& pose : poses) {
        if (!take(bytes, cursor, &pose.x, sizeof(pose.x)) || !take(bytes, cursor, &pose.y, sizeof(pose.y)) || !take(bytes, cursor, &pose.rotation, sizeof(pose.rotation)))
            break;
        if (!take(bytes, cursor, &pose.y_velocity, sizeof(pose.y_velocity)) || !take(bytes, cursor, &pose.x_velocity, sizeof(pose.x_velocity)))
            break;
    }
}

static bool load_version1(std::istream& file, unsigned char flags, EchoFile& out) {
    unsigned short reserved = 0;
    unsigned attempts = 0;
    unsigned amt = 0;
    if (!ReplayIO::read_pod(file, reserved)) return false;
    if (!ReplayIO::read_pod(file, out.fps) || !ReplayIO::read_pod(file, out.end_x) || !ReplayIO::read_pod(file, out.seconds))
        return false;
    if (!ReplayIO::read_pod(file, attempts) || !ReplayIO::read_pod(file, amt)) return false;
    out.attempts = attempts;
    if (amt > ReplayIO::max_inputs) return false;
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
        if (delta == 0) return false;
        frame += (int)(delta - 1);
        input.number = frame;
        out.inputs.push_back(input);
    }
    if (flags & FLAG_SAMPLES) read_version1_poses(out.inputs, out.poses, bytes, stream);
    return true;
}

static bool load_version2(std::istream& file, unsigned char flags, EchoFile& out) {
    if (flags & FLAG_WHOLE_FPS) {
        unsigned long long whole = 0;
        if (!read_varint(file, whole)) return false;
        out.fps = (float)whole;
    }
    else if (!ReplayIO::read_pod(file, out.fps)) {
        return false;
    }
    if (!ReplayIO::read_pod(file, out.end_x) || !ReplayIO::read_pod(file, out.seconds))
        return false;
    unsigned long long amt = 0;
    if (!read_varint(file, out.attempts) || !read_varint(file, amt)) return false;
    if (amt > ReplayIO::max_inputs) return false;
    auto bytes = read_rest(file);
    Decompressor reader = open_range(bytes);
    out.inputs.reserve((size_t)(std::min<unsigned long long>(amt, RESERVE_LIMIT)));
    read_version2_inputs(reader, out.inputs, out.poses, (size_t)amt, flags & FLAG_SAMPLES);
    return !reader.failed && out.inputs.size() == amt;
}

static bool load_version3(std::istream& file, unsigned char flags, int version, EchoFile& out) {
    auto bytes = read_rest(file);
    Decompressor reader = open_range(bytes);
    auto guesses = std::make_unique<ClickGuesses>();
    out.fps = std::bit_cast<float>((unsigned)code_bits(reader, 0, 32));
    out.end_x = std::bit_cast<float>((unsigned)code_bits(reader, 0, 32));
    out.seconds = std::bit_cast<double>(code_bits(reader, 0, 64));
    out.attempts = code_count(reader, 0);
    unsigned long long amt = code_count(reader, 0);
    if (reader.failed || amt > ReplayIO::max_inputs || out.attempts > INT_MAX) return false;
    out.inputs.reserve((size_t)(std::min<unsigned long long>(amt, RESERVE_LIMIT)));
    code_clicks(reader, *guesses, out.inputs, (size_t)amt);
    if (!reader.failed && (flags & FLAG_SAMPLES))
        code_poses(reader, out.inputs, out.poses, guesses->pose);
    if (version == 4 && !reader.failed) {
        unsigned long long rate_count = code_count(reader, 0);
        if (reader.failed || rate_count > MAX_RATE_CHANGES) return false;
        code_rates(reader, out.rates, (size_t)rate_count);
        if (reader.failed || out.rates.size() != rate_count) return false;
    }
    if (!reader.failed && (flags & FLAG_SEED)) {
        out.seed = code_bits(reader, 0, 64);
        out.has_seed = !reader.failed;
    }
    return !reader.failed && out.inputs.size() == amt;
}

static void store_echo_body(EchoFile& body, MacroSnap& out) {
    out.inputs = std::move(body.inputs);
    out.poses = std::move(body.poses);
    if (out.poses.size() != out.inputs.size()) out.poses.clear();
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

    auto guesses = std::make_unique<ClickGuesses>();
    Compressor writer;
    float header_fps = !rates.empty() ? snap.fps : (float)ReplayIO::snap_sample_rate(snap);
    code_bits(writer, std::bit_cast<unsigned>(header_fps), 32);
    code_bits(writer, std::bit_cast<unsigned>(snap.end_x), 32);
    code_bits(writer, std::bit_cast<unsigned long long>(snap.recording_seconds), 64);
    code_count(writer, (unsigned long long)std::max(snap.attempts, 0));
    code_count(writer, inputs.size());
    code_clicks(writer, *guesses, inputs, inputs.size());
    if (samples) code_poses(writer, inputs, poses, guesses->pose);
    if (!rates.empty()) {
        code_count(writer, rates.size());
        code_rates(writer, rates, rates.size());
    }
    if (snap.lvl_seed_known) code_bits(writer, snap.lvl_seed, 64);
    writer.finish();

    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    char magic[4] = { 'E', 'C', 'H', '2' };
    file.write(magic, 4);
    unsigned char version = !rates.empty() ? 4 : 3;
    unsigned char flags = samples ? FLAG_SAMPLES : 0;
    if (snap.lvl_seed_known) flags |= FLAG_SEED;
    if (snap.unrounded) flags |= FLAG_UNROUNDED;
    if (snap.silicate_variance) flags |= FLAG_SILICATE_VARIANCE;
    if (snap.ldm_known) flags |= FLAG_LDM_KNOWN | (snap.ldm ? FLAG_LDM : 0);
    ReplayIO::write_pod(file, version);
    ReplayIO::write_pod(file, flags);
    if (!writer.bytes.empty())
        file.write(reinterpret_cast<const char*>(writer.bytes.data()), (std::streamsize)writer.bytes.size());
    return (bool)file;
}

bool ReplayIO::read_echo(const fs::path& path, MacroSnap& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    char magic[4] = {};
    file.read(magic, 4);
    if (!file || std::string_view(magic, 4) != "ECH2") return false;
    unsigned char version = 0;
    unsigned char flags = 0;
    if (!ReplayIO::read_pod(file, version) || !ReplayIO::read_pod(file, flags)) return false;

    EchoFile body;
    bool loaded = false;
    body.unrounded = (flags & FLAG_UNROUNDED) != 0;
    body.silicate_variance = (flags & FLAG_SILICATE_VARIANCE) != 0;
    body.ldm_known = (flags & FLAG_LDM_KNOWN) != 0;
    body.ldm = (flags & FLAG_LDM) != 0;
    if (version == 1) loaded = load_version1(file, flags, body);
    else if (version == 2) loaded = load_version2(file, flags, body);
    else if (version == 3 || version == 4) loaded = load_version3(file, flags, version, body);
    if (!loaded) return false;
    store_echo_body(body, out);
    return true;
}
