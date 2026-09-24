#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

template <typename A1>
bool ReplayIO::read_pod(std::vector<uint8_t> const& data, size_t& cursor, A1& value) {
    if (cursor + sizeof(value) > data.size())
        return false;
    std::memcpy(&value, data.data() + cursor, sizeof(value));
    cursor += sizeof(value);
    return true;
}

bool ReplayIO::parse_legacy(std::vector<uint8_t> const& data, int string_bytes, bool with_meta, LegacyMacro& out) {
    if (string_bytes != 24 && string_bytes != 32)
        return false;
    size_t cursor = 0;
    if (data.size() < static_cast<size_t>(string_bytes) + 8)
        return false;
    uint64_t len = 0;
    uint64_t capacity = 0;
    if (string_bytes == 32) {
        std::memcpy(&len, data.data() + 16, 8);
        std::memcpy(&capacity, data.data() + 24, 8);
    }
    else {
        uint32_t short_length = 0, short_capacity = 0;
        std::memcpy(&short_length, data.data() + 16, 4);
        std::memcpy(&short_capacity, data.data() + 20, 4);
        len = short_length;
        capacity = short_capacity;
    }
    if (len < 4 || len > 7 || capacity < len || capacity > 15)
        return false;
    std::string name(reinterpret_cast<char const*>(data.data()), static_cast<size_t>(len));
    if (name != "SIMPLE" && name != "DEBUG" && name != "META" && name != "METADBG")
        return false;
    cursor = static_cast<size_t>(string_bytes);
    float fps = 0.f;
    float end_x = 0.f;
    double seconds = 0.0;
    int32_t clicks = 0;
    int32_t attempts = 1;
    if (!ReplayIO::read_pod(data, cursor, fps) || !ReplayIO::read_pod(data, cursor, end_x))
        return false;
    if (with_meta) {
        if (!ReplayIO::read_pod(data, cursor, seconds) || !ReplayIO::read_pod(data, cursor, clicks) || !ReplayIO::read_pod(data, cursor, attempts))
            return false;
    }
    if (!std::isfinite(fps) || fps < 1.f || fps > 100000.f)
        return false;
    if (!std::isfinite(end_x) || !std::isfinite(seconds))
        return false;
    if (attempts < 0 || attempts > 100000000 || clicks < 0)
        return false;
    bool debug = name == "DEBUG" || name == "METADBG";
    LegacyMacro parsed;
    parsed.format = ReplayIO::format_from_name(name);
    parsed.fps = fps;
    parsed.end_x = end_x;
    parsed.seconds = seconds;
    parsed.attempts = attempts;
    int prev = -1;
    while (cursor < data.size()) {
        if (cursor + 6 > data.size())
            return false;
        int32_t number = 0;
        uint8_t holding = 0;
        uint8_t player2 = 0;
        if (!ReplayIO::read_pod(data, cursor, number) || !ReplayIO::read_pod(data, cursor, holding) || !ReplayIO::read_pod(data, cursor, player2))
            return false;
        if (holding > 1 || player2 > 1 || number < 0 || number > 100000000 || prev > number)
            return false;
        prev = number;
        Frame input;
        input.number = number;
        input.holding = holding != 0;
        input.player2 = player2 != 0;
        input.button = static_cast<uint8_t>(PlayerButton::Jump);
        FramePose pose;
        if (debug) {
            if (!ReplayIO::read_pod(data, cursor, pose.x) || !ReplayIO::read_pod(data, cursor, pose.yVelocity) || !ReplayIO::read_pod(data, cursor, pose.x_velocity))
                return false;
            if (!ReplayIO::read_pod(data, cursor, pose.y) || !ReplayIO::read_pod(data, cursor, pose.rotation))
                return false;
            if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.rotation) || !std::isfinite(pose.yVelocity) || !std::isfinite(pose.x_velocity))
                return false;
        }
        parsed.inputs.push_back(input);
        if (debug)
            parsed.poses.push_back(pose);
    }
    if (cursor != data.size())
        return false;
    out = std::move(parsed);
    return true;
}

std::optional<LegacyMacro> ReplayIO::read_legacy_bytes(std::vector<uint8_t> const& data) {
    int const widths[] = { 32, 24 };
    bool const metas[] = { true, false };
    for (int width : widths) {
        for (bool meta : metas) {
            LegacyMacro parsed;
            if (ReplayIO::parse_legacy(data, width, meta, parsed))
                return parsed;
        }
    }
    return std::nullopt;
}

std::vector<uint8_t> ReplayIO::read_all(fs::path const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool ReplayIO::read_legacy_echo(fs::path const& path) {
    auto parsed = ReplayIO::read_legacy_bytes(ReplayIO::read_all(path));
    if (!parsed)
        return false;
    ReplayIO::adopt_inputs(std::move(parsed->inputs), parsed->fps, std::move(parsed->poses));
    auto& logic = Logic::get();
    logic.format = parsed->format;
    logic.end_x = parsed->end_x;
    logic.recording_seconds = parsed->seconds;
    logic.attempts = parsed->attempts;
    return true;
}
