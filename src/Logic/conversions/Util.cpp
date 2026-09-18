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
    // port this
    return {};
}

std::string ReplayIO::strip_numbering(std::string const& base, std::string const& format) {
    // later
    return {};
}

fs::path ReplayIO::unique_file(fs::path const& directory, std::string const& base, std::string const& ext) {
    // TODO
    return {};
}

bool ReplayIO::read_boolish(matjson::Value const& json) {
    // TODO
    return {};
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
    // later
    return {};
}

ReplayFormat ReplayIO::format_from_name(std::string const& name) {
    // port this
    return {};
}

bool ReplayIO::debug_format(ReplayFormat format) {
    return format == ReplayFormat::Debug || format == ReplayFormat::MetaDebug;
}

std::string ReplayIO::clock(double seconds) {
    // TODO
    return {};
}

void ReplayIO::adopt_inputs(std::vector<Frame> inputs, float fps, std::vector<FramePose> poses) {
    // TODO
}

std::vector<Frame> ReplayIO::sorted_inputs() {
    // TODO
    return {};
}

void ReplayIO::finish_load(std::string const& detail) {
    // TODO
}
