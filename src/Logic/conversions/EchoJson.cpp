#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using namespace geode::prelude;

class EchoJson {
public:
    static const int version = 2;

    static std::string number(double value) {
        return std::isfinite(value) ? fmt::format("{}", value) : "0";
    }

    static std::string number(float value) {
        return std::isfinite(value) ? fmt::format("{}", value) : "0";
    }

    static std::string_view button_name(int button) {
        switch (button) {
        case 2:
            return "left";
        case 3:
            return "right";
        default:
            return "jump";
        }
    }

    static int button_from_json(const matjson::Value& json) {
        if (auto name = json.asString()) {
            const auto& text = name.unwrap();
            if (text == "left") return 2;
            if (text == "right") return 3;
            return 1;
        }
        int button = ReplayIO::read_int(json, 1);
        return button >= 1 && button <= 3 ? button : 1;
    }

    static std::string input_line(const Frame& input, const FramePose& pose, bool debug) {
        auto line = fmt::format(R"({{"frame": {}, "button": "{}", "player": {}, "down": {})",
            input.number, button_name(input.button), input.player2 ? 2 : 1, input.holding);
        if (debug) {
            line += fmt::format(R"(, "x": {}, "y": {}, "rotation": {}, "xVelocity": {}, "yVelocity": {})",
                number(pose.x), number(pose.y), number(pose.rotation), number(pose.x_velocity), number(pose.y_velocity));
        }
        return line + "}";
    }

    static void read_seed_and_flags(const matjson::Value& json, MacroSnap& out) {
        if (json.contains("seed")) {
            if (auto text = json["seed"].asString()) {
                out.lvl_seed = std::strtoull(text.unwrap().c_str(), nullptr, 10);
            }
            else {
                out.lvl_seed = (unsigned long long)ReplayIO::read_number(json["seed"], 0.0);
            }
            out.lvl_seed_known = true;
        }
        if (auto flag = json["velocity_unrounding"].asBool()) out.unrounded = flag.unwrap();
        if (auto flag = json["silicate_variance"].asBool()) out.silicate_variance = flag.unwrap();
        if (auto flag = json["low_detail_mode"].asBool()) {
            out.ldm = flag.unwrap();
            out.ldm_known = true;
        }
    }

    static bool is_current(const matjson::Value& json) {
        return json["format"].asString().unwrapOr("") == "echo";
    }

    static bool read_current(const matjson::Value& json, MacroSnap& out) {
        bool debug = json["debug"].asBool().unwrapOr(false);
        std::vector<Frame> inputs;
        std::vector<FramePose> poses;
        for (const auto& item : json["inputs"]) {
            Frame input;
            input.number = ReplayIO::read_int(item["frame"], 0);
            input.button = (unsigned char)button_from_json(item["button"]);
            input.player2 = ReplayIO::read_int(item["player"], 1) == 2;
            input.holding = ReplayIO::read_boolish(item["down"]);
            if (debug || item.contains("x")) {
                FramePose pose;
                pose.x = (float)ReplayIO::read_number(item["x"], 0.0);
                pose.y = (float)ReplayIO::read_number(item["y"], 0.0);
                pose.rotation = (float)ReplayIO::read_number(item["rotation"], 0.0);
                pose.x_velocity = ReplayIO::read_number(item["xVelocity"], 0.0);
                pose.y_velocity = ReplayIO::read_number(item["yVelocity"], 0.0);
                poses.push_back(pose);
            }
            inputs.push_back(input);
        }
        if (poses.size() != inputs.size()) poses.clear();
        ReplayIO::adopt_inputs(out, std::move(inputs), (float)ReplayIO::read_number(json["fps"], 240.0), std::move(poses));
        if (json.contains("rates") && json["rates"].isArray()) {
            for (const auto& item : json["rates"]) {
                RateChange change;
                change.frame = ReplayIO::read_int(item["frame"], 0);
                change.fps = (float)ReplayIO::read_number(item["fps"], 240.0);
                if (change.frame >= 0 && change.fps >= 1.f) out.rates.push_back(change);
            }
            tidy_rates(out.rates, out.fps);
        }
        out.end_x = (float)ReplayIO::read_number(json["endX"], 0.0);
        out.recording_seconds = ReplayIO::read_number(json["recordingSeconds"], 0.0);
        out.attempts = ReplayIO::read_int(json["attempts"], 1);
        read_seed_and_flags(json, out);
        return true;
    }

    static bool read_old(const matjson::Value& json, MacroSnap& out) {
        auto format = ReplayFormat::Meta;
        if (auto name = json["version"].asString())
            format = ReplayIO::format_from_name(name.unwrap());
        bool debug = ReplayIO::debug_format(format);
        std::vector<Frame> inputs;
        std::vector<FramePose> poses;
        for (const auto& item : json["inputs"]) {
            Frame input;
            input.number = ReplayIO::read_int(item["frame"], 0);
            input.holding = ReplayIO::read_boolish(item["holding"]);
            input.player2 = item.contains("player_2") && ReplayIO::read_boolish(item["player_2"]);
            int button = ReplayIO::read_int(item["button"], 1);
            if (button < 1 || button > 3) button = 1;
            input.button = (unsigned char)button;
            if (debug || item.contains("x_position")) {
                FramePose pose;
                pose.x = (float)ReplayIO::read_number(item["x_position"], 0.0);
                pose.y = (float)ReplayIO::read_number(item["y_position"], 0.0);
                pose.y_velocity = ReplayIO::read_number(item["y_vel"], 0.0);
                pose.x_velocity = ReplayIO::read_number(item["x_vel"], 0.0);
                pose.rotation = (float)ReplayIO::read_number(item["rotation"], 0.0);
                poses.push_back(pose);
            }
            inputs.push_back(input);
        }
        if (poses.size() != inputs.size()) poses.clear();
        ReplayIO::adopt_inputs(out, std::move(inputs), (float)ReplayIO::read_number(json["fps"], 240.0), std::move(poses));
        out.end_x = (float)ReplayIO::read_number(json["end_xpos"], 0.0);
        if (json["metadata"].isObject()) {
            out.recording_seconds = ReplayIO::read_number(json["metadata"]["recording_time"], 0.0);
            out.attempts = ReplayIO::read_int(json["metadata"]["total_attempts"], 1);
        }
        read_seed_and_flags(json, out);
        return true;
    }
};

bool ReplayIO::write_echo_json(const fs::path& path, const MacroSnap& snap) {
    bool debug = snap.save_debug && snap.poses.size() == snap.inputs.size();
    auto inputs = snap.inputs;
    auto poses = debug ? snap.poses : std::vector<FramePose>{};
    std::vector<size_t> order(inputs.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return inputs[a].number < inputs[b].number;
    });
    std::vector<Frame> sorted;
    std::vector<FramePose> sorted_poses;
    sorted.reserve(inputs.size());
    if (debug) sorted_poses.reserve(inputs.size());
    for (size_t i : order) {
        auto input = inputs[i];
        if (input.button < 1 || input.button > 3) input.button = 1;
        input.number = std::max(input.number, 0);
        sorted.push_back(input);
        if (debug) sorted_poses.push_back(poses[i]);
    }
    inputs.swap(sorted);
    poses.swap(sorted_poses);
    std::ofstream file(path);
    if (!file) return false;
    file << "{\n";
    file << "\t\"format\": \"echo\",\n";
    file << fmt::format("\t\"version\": {},\n", EchoJson::version);
    file << fmt::format("\t\"fps\": {},\n", EchoJson::number(snap.fps));
    if (!snap.rates.empty()) {
        file << "\t\"rates\": [\n";
        for (size_t i = 0; i < snap.rates.size(); ++i) {
            file << fmt::format("\t\t{{\"frame\": {}, \"fps\": {}}}", snap.rates[i].frame, EchoJson::number(snap.rates[i].fps));
            file << (i + 1 == snap.rates.size() ? "\n" : ",\n");
        }
        file << "\t],\n";
    }
    file << fmt::format("\t\"endX\": {},\n", EchoJson::number(snap.end_x));
    file << fmt::format("\t\"recordingSeconds\": {},\n", EchoJson::number(snap.recording_seconds));
    file << fmt::format("\t\"attempts\": {},\n", snap.attempts);
    if (snap.lvl_seed_known)
        file << fmt::format("\t\"seed\": \"{}\",\n", snap.lvl_seed);
    if (snap.unrounded) file << "\t\"velocity_unrounding\": true,\n";
    if (snap.silicate_variance) file << "\t\"silicate_variance\": true,\n";
    if (snap.ldm_known)
        file << fmt::format("\t\"low_detail_mode\": {},\n", snap.ldm);
    file << fmt::format("\t\"debug\": {},\n", debug);
    file << "\t\"inputs\": [";
    for (size_t i = 0; i < inputs.size(); ++i) {
        file << (i == 0 ? "\n\t\t" : ",\n\t\t") << EchoJson::input_line(inputs[i], debug ? poses[i] : FramePose{}, debug);
    }
    file << (inputs.empty() ? "]\n}\n" : "\n\t]\n}\n");
    return (bool)file;
}

bool ReplayIO::read_echo_json(const fs::path& path, MacroSnap& out) {
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed) return false;
    const auto& json = parsed.unwrap();
    if (!json.contains("inputs") || !json["inputs"].isArray()) return false;
    return EchoJson::is_current(json) ? EchoJson::read_current(json, out) : EchoJson::read_old(json, out);
}
