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

bool ReplayIO::read_plain(const fs::path& path, MacroData& out) {
    std::ifstream file(path);
    if (!file) return false;
    float fps = 0.f;
    file >> fps;
    if (!file) return false;
    std::vector<Frame> inputs;
    while (file) {
        Frame input;
        int holding = 0;
        int player2 = 0;
        file >> input.number >> holding >> player2;
        if (!file) break;
        input.holding = holding != 0;
        input.player2 = player2 != 0;
        inputs.push_back(input);
    }
    if (!file.eof()) return false;
    ReplayIO::adopt_inputs(out, std::move(inputs), fps);
    return true;
}

bool ReplayIO::write_plain(const fs::path& path, const MacroData& macro) {
    auto temp = ReplayIO::temp_path(path);
    std::ofstream file(temp);
    if (!file) return false;
    file << fmt::format("{}", macro.fps) << "\n";
    for (const auto& input : macro.inputs) {
        file << input.number << " " << (input.holding ? 1 : 0) << " " << (input.player2 ? 1 : 0) << "\n";
    }
    return ReplayIO::finish_temp_file(file, temp, path);
}
