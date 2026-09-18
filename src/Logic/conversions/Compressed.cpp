// the .echo format
//
// v1: 4 bits a click (hold, player, button) + the gap to the last click as an elias gamma code
// debug samples sit raw after the clicks
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

struct BitWriter {
    std::vector<uint8_t> bytes;
    uint8_t acc = 0;
    int used = 0;

    void bit(int value) {
        acc = static_cast<uint8_t>((acc << 1) | (value & 1));
        if (++used == 8) {
            bytes.push_back(acc);
            acc = 0;
            used = 0;
        }
    }

    // elias gamma. small numbers = few bits & most gaps are small
    void gamma(uint32_t n) {
        int width = std::bit_width(n);
        for (int i = 1; i < width; ++i)
            bit(0);
        for (int i = width - 1; i >= 0; --i)
            bit(static_cast<int>((n >> i) & 1));
    }

    void flush() {
        while (used != 0)
            bit(0);
    }
};

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

constexpr uint8_t FLAG_SAMPLES = 1;

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

    // 4 bits a click + the gap to the last one
    BitWriter stream;
    int frame = 0;
    for (auto const& input : inputs) {
        stream.bit(input.holding ? 1 : 0);
        stream.bit(input.player2 ? 1 : 0);
        int code = ReplayIO::pack_button(input.button);
        stream.bit(code >> 1);
        stream.bit(code & 1);
        stream.gamma(static_cast<uint32_t>(input.number - frame) + 1);
        frame = input.number;
    }
    stream.flush();

    std::ofstream file(path, std::ios::binary);
    if (!file)
        return false;
    char magic[4] = { 'E', 'C', 'H', '2' };
    file.write(magic, 4);
    uint8_t version = 1;
    uint8_t flags = samples ? FLAG_SAMPLES : 0;
    uint16_t reserved = 0;
    ReplayIO::write_pod(file, version);
    ReplayIO::write_pod(file, flags);
    ReplayIO::write_pod(file, reserved);
    ReplayIO::write_pod(file, static_cast<float>(logic.sample_rate()));
    ReplayIO::write_pod(file, logic.end_x);
    ReplayIO::write_pod(file, logic.recording_seconds);
    ReplayIO::write_pod(file, static_cast<uint32_t>(std::max(logic.attempts, 0)));
    ReplayIO::write_pod(file, static_cast<uint32_t>(inputs.size()));
    if (!stream.bytes.empty())
        file.write(reinterpret_cast<char const*>(stream.bytes.data()), static_cast<std::streamsize>(stream.bytes.size()));
    // TODO the samples are HUGE like ts
    if (samples) {
        for (auto const& pose : poses) {
            ReplayIO::write_pod(file, pose.x);
            ReplayIO::write_pod(file, pose.y);
            ReplayIO::write_pod(file, pose.rotation);
            ReplayIO::write_pod(file, pose.yVelocity);
            ReplayIO::write_pod(file, pose.x_velocity);
        }
    }
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
    if (version != 1)
        return false;
    PackedBody body;
    if (!load_version1(file, flags, body))
        return false;
    store_packed(body);
    return true;
}
