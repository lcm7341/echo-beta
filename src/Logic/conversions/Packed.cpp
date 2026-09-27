// the .echo format
//
// range coder: every bit is coded against odds that adapt as bits go by. reader & writer have to update them identically or it desyncs
// a click = player, button, hold or release, & the gap in frames since the last one. each has its own odds picked by what came before
// versions: 1 = bit packed, 2 = ts. v1 files still load, only v2 gets written
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

    int code(Odds& odds, int value) {
        bit(odds.chance(), value);
        odds.learn(value);
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

void write_version2(RangeWriter& coder, std::vector<Frame> const& inputs, std::vector<FramePose>& poses, bool samples) {
    auto odds = std::make_unique<InputOdds>();
    std::array<int, 6> state{};
    int last_channel = 0, last_hold = 0, last_frame = 0;
    for (auto const& input : inputs) {
        int context = last_channel * 2 + last_hold;
        int player2 = input.player2 ? 1 : 0;
        coder.code(odds->player[context], player2);
        auto& button = odds->button[context * 2 + player2];
        int code = ReplayIO::pack_button(input.button);
        int high = code >> 1, low = code & 1;
        coder.code(button[1], high);
        coder.code(button[2 + high], low);
        int channel = player2 * 3 + code;
        int hold = input.holding ? 1 : 0;
        coder.code(odds->hold[state[channel] * 2 + last_hold], hold);
        auto& gap = odds->delta[hold * 2 + (channel == last_channel ? 1 : 0)];
        code_number(coder, gap, static_cast<uint64_t>(input.number - last_frame));
        state[channel] = hold ? 1 : 2;
        last_channel = channel;
        last_hold = hold;
        last_frame = input.number;
    }
    if (samples)
        code_samples(coder, inputs, poses, odds->sample);
}

constexpr uint64_t MAX_INPUTS = uint64_t{ 1 } << 24;

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

void write_varint(std::ostream& file, uint64_t value) {
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        file.put(static_cast<char>(value ? byte | 0x80 : byte));
    } while (value);
}

constexpr uint8_t FLAG_SAMPLES = 1, FLAG_WHOLE_FPS = 2;

struct PackedBody {
    float fps = 240.f;
    float end_x = 0.f;
    double seconds = 0.0;
    uint64_t attempts = 0;
    std::vector<Frame> inputs;
    std::vector<FramePose> poses;
};

std::vector<Frame> inputs_in_pack_order(std::vector<Frame> inputs, std::vector<FramePose>* poses) {
    std::vector<size_t> order(inputs.size());
    std::iota(order.begin(), order.end(), 0);
    for (auto& input : inputs)
        input.number = std::max(input.number, 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto const& left = inputs[a];
        auto const& right = inputs[b];
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

void store_packed(PackedBody& body) {
    auto& logic = Logic::get();
    logic.inputs = std::move(body.inputs);
    logic.poses = std::move(body.poses);
    logic.input_ids.clear();
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    logic.fps = body.fps > 0.f ? body.fps : logic.fps;
    logic.macro_fps = logic.fps;
    logic.end_x = body.end_x;
    logic.recording_seconds = body.seconds;
    logic.attempts = static_cast<int>(body.attempts);
    logic.format = ReplayFormat::Meta;
}

bool ReplayIO::write_packed_echo(fs::path const& path) {
    auto const& logic = Logic::get();
    auto poses = logic.poses.size() == logic.inputs.size() ? logic.poses : std::vector<FramePose>{};
    auto inputs = inputs_in_pack_order(logic.inputs, poses.empty() ? nullptr : &poses);
    bool samples = logic.save_debug && poses.size() == inputs.size();

    RangeWriter body;
    write_version2(body, inputs, poses, samples);
    body.finish();

    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[4] = { 'E', 'C', 'H', '2' };
    file.write(magic, 4);
    float fps = static_cast<float>(logic.sample_rate());
    bool whole = fps >= 0.f && fps == std::floor(fps);
    uint8_t version = 2;
    uint8_t flags = samples ? FLAG_SAMPLES : 0;
    if (whole)
        flags = static_cast<uint8_t>(flags | FLAG_WHOLE_FPS);
    ReplayIO::write_pod(file, version);
    ReplayIO::write_pod(file, flags);
    if (whole)
        write_varint(file, static_cast<uint64_t>(fps));
    else
        ReplayIO::write_pod(file, fps);
    ReplayIO::write_pod(file, logic.end_x);
    ReplayIO::write_pod(file, logic.recording_seconds);
    write_varint(file, static_cast<uint64_t>(std::max(logic.attempts, 0)));
    write_varint(file, inputs.size());
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
    if (version == 1)
        loaded = load_version1(file, flags, body);
    else if (version == 2)
        loaded = load_version2(file, flags, body);
    if (!loaded)
        return false;
    store_packed(body);
    return true;
}
