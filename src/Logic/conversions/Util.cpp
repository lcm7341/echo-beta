#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <Geode/utils/async.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

using namespace geode::prelude;

std::string ReplayIO::lower_ext(fs::path const& path) {
    log::debug("lower ext");
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

std::string ReplayIO::sanitize(std::string name) {
    if (name.empty()) {
        name = "Replay";
    }
    for (char& c : name) {
        switch (c) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            c = '_';
            break;
        default:
            break;
        }
    }
    return name;
}

std::string ReplayIO::strip_numbering(std::string const& base, std::string const& format) {
    auto hash = format.find('#');
    if (hash == std::string::npos) {
        return base;
    }
    auto prefix = format.substr(0, hash);
    auto suffix = format.substr(hash + 1);
    std::string root = base;
    for (;;) {
        if (root.size() < prefix.size() + suffix.size() + 1) {
            break;
        }
        if (root.compare(root.size() - suffix.size(), suffix.size(), suffix) != 0) {
            break;
        }
        size_t number_end = root.size() - suffix.size(), number_start = number_end;
        while (number_start > 0 && std::isdigit(static_cast<unsigned char>(root[number_start - 1])))
            --number_start;
        if (number_start == number_end) {
            break;
        }
        if (number_start < prefix.size()) {
            break;
        }
        if (root.compare(number_start - prefix.size(), prefix.size(), prefix) != 0) {
            break;
        }
        root.resize(number_start - prefix.size());
    }
    return root.empty() ? base : root;
}

fs::path ReplayIO::unique_file(fs::path const& directory, std::string const& base, std::string const& ext) {
    std::error_code error;
    fs::create_directories(directory, error);
    auto format = Logic::get().rename_format.empty() ? std::string("_#") : Logic::get().rename_format;
    if (format.find('#') == std::string::npos)
        format += "#";
    auto root = ReplayIO::strip_numbering(base, format);
    auto plain = directory / (root + ext);
    if (!fs::exists(plain)) {
        return plain;
    }
    for (int n = 1; n < 10000; ++n) {
        auto suffix = format;
        auto hash = suffix.find('#');
        suffix.replace(hash, 1, std::to_string(n));
        auto candidate = directory / (root + suffix + ext);
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }
    return plain;
}

bool ReplayIO::read_boolish(matjson::Value const& json) {
    if (json.isBool()) {
        return json.asBool().unwrapOr(false);
    }
    if (json.isNumber()) {
        return json.asInt().unwrapOr(0) != 0;
    }
    return false;
}

double ReplayIO::read_number(matjson::Value const& json, double fallback) {
    if (auto number = json.asDouble())
        return number.unwrap();
    return fallback;
}

int ReplayIO::read_int(matjson::Value const& json, int fallback) {
    if (auto number = json.asInt())
        return static_cast<int>(number.unwrap());
    return fallback;
}

std::string ReplayIO::format_name(ReplayFormat format) {
    switch (format) {
    case ReplayFormat::Simple:
        return "SIMPLE";
    case ReplayFormat::Debug:
        return "DEBUG";
    case ReplayFormat::MetaDebug:
        return "METADBG";
    case ReplayFormat::Meta:
        return "META";
    }
    return "META";
}

ReplayFormat ReplayIO::format_from_name(std::string const& name) {
    if (name == "SIMPLE") {
        return ReplayFormat::Simple;
    }
    if (name == "DEBUG") {
        return ReplayFormat::Debug;
    }
    if (name == "METADBG") {
        return ReplayFormat::MetaDebug;
    }
    return ReplayFormat::Meta;
}

bool ReplayIO::debug_format(ReplayFormat format) {
    return format == ReplayFormat::Debug || format == ReplayFormat::MetaDebug;
}

std::string ReplayIO::clock(double seconds) {
    int total = static_cast<int>(seconds);
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int secs = total % 60;
    return fmt::format("{:02}:{:02}:{:02}", hours, minutes, secs);
}

void ReplayIO::adopt_inputs(std::vector<Frame> inputs, float fps, std::vector<FramePose> poses) {
    log::debug("adopt inputs");
    auto& logic = Logic::get();
    logic.inputs = std::move(inputs);
    logic.poses = std::move(poses);
    if (logic.poses.size() != logic.inputs.size()) {
        logic.poses.clear();
    }
    logic.input_ids.clear();
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    logic.fps = std::isfinite(fps) && fps > 0.f ? fps : 240.f;
    logic.macro_fps = logic.fps;
    logic.rates.clear();
    logic.format = ReplayFormat::Meta;
    logic.end_x = 0.f;
    logic.recording_seconds = 0.0;
    logic.attempts = 1;
    logic.lvl_seed = 0;
    logic.lvl_seed_known = false;
    logic.lvl_seed_from_file = false;
    logic.macro_unrounded = false;
    logic.macro_silicate_variance = false;
    logic.macro_ldm = logic.macro_ldm_known = false;
}

std::vector<Frame> ReplayIO::sorted_inputs() {
    log::debug("sorted inputs");
    auto inputs = Logic::get().inputs;
    std::stable_sort(inputs.begin(), inputs.end(), [](Frame const& a, Frame const& b) {
        return a.number < b.number;
    });
    for (auto& input : inputs) {
        if (input.button < 1 || input.button > 3) {
            input.button = 1;
        }
        input.number = std::max(input.number, 0);
    }
    return inputs;
}

void ReplayIO::finish_load(std::string const& detail) {
    auto& logic = Logic::get();
    logic.accuracy = 1;
    if ((logic).is_recording())
        logic.state = RunState::Playing;
    else if (!(logic).is_playing())
        logic.state = RunState::Playing;
    logic.replay_pos = 0;
    logic.message = detail;
    Logic::get().notify(detail, true);
}
