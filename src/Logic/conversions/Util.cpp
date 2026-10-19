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

std::string ReplayIO::lower_ext(const fs::path& path) {
    auto ext = geode::utils::string::pathToString(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return ext;
}

std::string ReplayIO::sanitize(std::string name) {
    if (name.empty()) name = "Replay";
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
#ifdef GEODE_IS_WINDOWS
    std::string device = name.substr(0, name.find('.'));
    std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) {
        return (char)std::toupper(c);
    });
    bool numbered = device.size() == 4 && (device.starts_with("COM") || device.starts_with("LPT")) &&
                    std::isdigit((unsigned char)device[3]);
    if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" || numbered)
        name.insert(device.size(), "_");
#endif
    return name;
}

fs::path ReplayIO::utf8_path(const std::string& text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

std::string ReplayIO::strip_numbering(const std::string& base, const std::string& format) {
    auto hash = format.find('#');
    if (hash == std::string::npos) return base;
    auto prefix = format.substr(0, hash);
    auto suffix = format.substr(hash + 1);
    std::string root = base;
    for (;;) {
        if (root.size() < prefix.size() + suffix.size() + 1) break;
        if (root.compare(root.size() - suffix.size(), suffix.size(), suffix) != 0) break;
        size_t number_end = root.size() - suffix.size(), number_start = number_end;
        while (number_start > 0 && std::isdigit((unsigned char)root[number_start - 1]))
            --number_start;
        if (number_start == number_end) break;
        if (number_start < prefix.size()) break;
        if (root.compare(number_start - prefix.size(), prefix.size(), prefix) != 0) break;
        root.resize(number_start - prefix.size());
    }
    return root.empty() ? base : root;
}

fs::path ReplayIO::unique_file(const fs::path& directory, const std::string& base, const std::string& ext, bool keep_numbering) {
    std::error_code error;
    fs::create_directories(directory, error);
    auto format = Logic::get().rename_format.empty() ? std::string("_#") : Logic::get().rename_format;
    if (format.find('#') == std::string::npos) format += "#";
    auto root = keep_numbering ? base : ReplayIO::strip_numbering(base, format);
    auto plain = directory / ReplayIO::utf8_path(root + ext);
    if (!fs::exists(plain)) return plain;
    for (int n = 1; n < 10000; ++n) {
        auto suffix = format;
        auto hash = suffix.find('#');
        suffix.replace(hash, 1, std::to_string(n));
        auto candidate = directory / ReplayIO::utf8_path(root + suffix + ext);
        if (!fs::exists(candidate)) return candidate;
    }
    return plain;
}

fs::path ReplayIO::temp_path(const fs::path& path) {
    auto temp = path;
    temp += ".tmp";
    return temp;
}

bool ReplayIO::finish_temp_file(std::ofstream& file, const fs::path& temp, const fs::path& path) {
    file.close();
    std::error_code error;
    if (!file) {
        fs::remove(temp, error);
        return false;
    }
    fs::rename(temp, path, error);
    if (!error) return true;
    fs::remove(temp, error);
    return false;
}

bool ByteWriter::save(const fs::path& path) const {
    auto temp = ReplayIO::temp_path(path);
    std::ofstream file(temp, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
    return ReplayIO::finish_temp_file(file, temp, path);
}

std::optional<matjson::Value> ReplayIO::parse_json(std::string_view text) {
    const int max_depth = 64;
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (in_string && c == '\\') ++i;
        else if (c == '"') in_string = !in_string;
        else if (in_string) continue;
        else if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') --depth;
        if (depth > max_depth) return std::nullopt;
    }
    auto parsed = matjson::parse(text);
    if (!parsed) return std::nullopt;
    return std::move(parsed).unwrap();
}

bool ReplayIO::read_boolish(const matjson::Value& json) {
    if (json.isBool()) return json.asBool().unwrapOr(false);
    if (json.isNumber()) return json.asInt().unwrapOr(0) != 0;
    return false;
}

double ReplayIO::read_number(const matjson::Value& json, double fallback) {
    if (auto number = json.asDouble()) return number.unwrap();
    return fallback;
}

int ReplayIO::read_int(const matjson::Value& json, int fallback) {
    if (auto number = json.asInt()) return (int)number.unwrap();
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

ReplayFormat ReplayIO::format_from_name(const std::string& name) {
    if (name == "SIMPLE") return ReplayFormat::Simple;
    if (name == "DEBUG") return ReplayFormat::Debug;
    if (name == "METADBG") return ReplayFormat::MetaDebug;
    return ReplayFormat::Meta;
}

bool ReplayIO::debug_format(ReplayFormat format) {
    return format == ReplayFormat::Debug || format == ReplayFormat::MetaDebug;
}

std::string ReplayIO::clock(double seconds) {
    int total = (int)seconds;
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int secs = total % 60;
    return fmt::format("{:02}:{:02}:{:02}", hours, minutes, secs);
}

void ReplayIO::adopt_inputs(MacroData& out, std::vector<Frame> inputs, float fps, std::vector<FramePose> poses) {
    out.inputs = std::move(inputs);
    out.poses = std::move(poses);
    if (out.poses.size() != out.inputs.size()) out.poses.clear();
    out.fps = std::isfinite(fps) && fps > 0.f ? fps : 240.f;
}

double ReplayIO::macro_sample_rate(const MacroData& macro) {
    return (double)std::max(macro.fps, step_patched ? 1.f : VANILLA_TPS);
}

std::vector<Frame> ReplayIO::sorted_inputs(const MacroData& macro) {
    auto inputs = macro.inputs;
    std::stable_sort(inputs.begin(), inputs.end(), [](const Frame& a, const Frame& b) {
        return a.number < b.number;
    });
    for (auto& input : inputs) {
        if (input.button < 1 || input.button > 3) input.button = 1;
        input.number = std::max(input.number, 0);
    }
    return inputs;
}

void ReplayIO::finish_load(const std::string& detail) {
    auto& logic = Logic::get();
    if (logic.is_recording()) logic.state = RunState::Playing;
    else if (!logic.is_playing()) logic.state = RunState::Playing;
    logic.replay_pos = 0;
    logic.message = detail;
    Logic::get().notify(detail, true);
}
