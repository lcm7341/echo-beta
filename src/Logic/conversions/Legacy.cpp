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

template <typename T>
bool ReplayIO::read_pod(const std::vector<unsigned char>& data, size_t& cursor, T& value) {
    if (cursor + sizeof(value) > data.size()) return false;
    std::memcpy(&value, data.data() + cursor, sizeof(value));
    cursor += sizeof(value);
    return true;
}

bool ReplayIO::parse_legacy(const std::vector<unsigned char>& data, int string_bytes, bool with_meta, MacroData& out) {
    if (string_bytes != 24 && string_bytes != 32) return false;
    size_t cursor = 0;
    if (data.size() < (size_t)string_bytes + 8) return false;
    unsigned long long len = 0;
    unsigned long long capacity = 0;
    if (string_bytes == 32) {
        std::memcpy(&len, data.data() + 16, 8);
        std::memcpy(&capacity, data.data() + 24, 8);
    }
    else {
        unsigned short_length = 0, short_capacity = 0;
        std::memcpy(&short_length, data.data() + 16, 4);
        std::memcpy(&short_capacity, data.data() + 20, 4);
        len = short_length;
        capacity = short_capacity;
    }
    if (len < 4 || len > 7 || capacity < len || capacity > 15) return false;
    std::string name(reinterpret_cast<const char*>(data.data()), (size_t)len);
    if (name != "SIMPLE" && name != "DEBUG" && name != "META" && name != "METADBG") return false;
    cursor = (size_t)string_bytes;
    float fps = 0.f;
    float end_x = 0.f;
    double seconds = 0.0;
    int clicks = 0;
    int attempts = 1;
    if (!ReplayIO::read_pod(data, cursor, fps) || !ReplayIO::read_pod(data, cursor, end_x))
        return false;
    if (with_meta) {
        if (!ReplayIO::read_pod(data, cursor, seconds) || !ReplayIO::read_pod(data, cursor, clicks) || !ReplayIO::read_pod(data, cursor, attempts))
            return false;
    }
    if (!std::isfinite(fps) || fps < 1.f || fps > 100000.f) return false;
    if (!std::isfinite(end_x) || !std::isfinite(seconds)) return false;
    if (attempts < 0 || attempts > 100000000 || clicks < 0) return false;
    bool debug = name == "DEBUG" || name == "METADBG";
    MacroData parsed;
    parsed.format = ReplayIO::format_from_name(name);
    parsed.fps = fps;
    parsed.end_x = end_x;
    parsed.recording_seconds = seconds;
    parsed.attempts = attempts;
    int prev = -1;
    while (cursor < data.size()) {
        if (cursor + 6 > data.size()) return false;
        int number = 0;
        unsigned char holding = 0;
        unsigned char player2 = 0;
        if (!ReplayIO::read_pod(data, cursor, number) || !ReplayIO::read_pod(data, cursor, holding) || !ReplayIO::read_pod(data, cursor, player2))
            return false;
        if (holding > 1 || player2 > 1 || number < 0 || number > 100000000 || prev > number)
            return false;
        prev = number;
        Frame input;
        input.number = number;
        input.holding = holding != 0;
        input.player2 = player2 != 0;
        input.button = (unsigned char)PlayerButton::Jump;
        FramePose pose;
        if (debug) {
            if (!ReplayIO::read_pod(data, cursor, pose.x) || !ReplayIO::read_pod(data, cursor, pose.y_velocity) || !ReplayIO::read_pod(data, cursor, pose.x_velocity))
                return false;
            if (!ReplayIO::read_pod(data, cursor, pose.y) || !ReplayIO::read_pod(data, cursor, pose.rotation))
                return false;
            if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.rotation) || !std::isfinite(pose.y_velocity) || !std::isfinite(pose.x_velocity))
                return false;
        }
        parsed.inputs.push_back(input);
        if (debug) parsed.poses.push_back(pose);
    }
    if (cursor != data.size()) return false;
    out = std::move(parsed);
    return true;
}

std::vector<unsigned char> ReplayIO::read_all(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool ReplayIO::read_legacy_echo(const fs::path& path, MacroData& out) {
    auto data = ReplayIO::read_all(path);
    const int widths[] = { 32, 24 };
    const bool metas[] = { true, false };
    for (int width : widths) {
        for (bool meta : metas) {
            if (ReplayIO::parse_legacy(data, width, meta, out)) return true;
        }
    }
    return false;
}
