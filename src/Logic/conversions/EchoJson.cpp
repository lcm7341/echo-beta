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
    static constexpr int version = 2;

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

    static int button_from_json(matjson::Value const& json) {
        if (auto name = json.asString()) {
            auto const& text = name.unwrap();
            if (text == "left")
                return 2;
            if (text == "right")
                return 3;
            return 1;
        }
        int button = ReplayIO::read_int(json, 1);
        return button >= 1 && button <= 3 ? button : 1;
    }

    static std::string input_line(Frame const& input, FramePose const& pose, bool debug) {
        auto line = fmt::format(R"({{"frame": {}, "button": "{}", "player": {}, "down": {})",
            input.number, button_name(input.button), input.player2 ? 2 : 1, input.holding);
        if (debug) {
            line += fmt::format(R"(, "x": {}, "y": {}, "rotation": {}, "xVelocity": {}, "yVelocity": {})",
                number(pose.x), number(pose.y), number(pose.rotation), number(pose.x_velocity), number(pose.yVelocity));
        }
        return line + "}";
    }

    static void read_seed_and_flags(matjson::Value const& json) {
        auto& logic = Logic::get();
        if (json.contains("seed")) {
            if (auto text = json["seed"].asString()) {
                logic.lvl_seed = std::strtoull(text.unwrap().c_str(), nullptr, 10);
            }
            else {
                logic.lvl_seed = static_cast<uint64_t>(ReplayIO::read_number(json["seed"], 0.0));
            }
            logic.lvl_seed_known = true;
            logic.lvl_seed_from_file = true;
        }
        if (auto flag = json["velocity_unrounding"].asBool())
            logic.macro_unrounded = flag.unwrap();
        if (auto flag = json["silicate_variance"].asBool())
            logic.macro_silicate_variance = flag.unwrap();
        if (auto flag = json["low_detail_mode"].asBool()) {
            logic.macro_ldm = flag.unwrap();
            logic.macro_ldm_known = true;
        }
    }

    static bool is_current(matjson::Value const& json) {
        return json["format"].asString().unwrapOr("") == "echo";
    }

    static bool read_current(matjson::Value const& json) {
        bool debug = json["debug"].asBool().unwrapOr(false);
        std::vector<Frame> inputs;
        std::vector<FramePose> poses;
        for (auto const& item : json["inputs"]) {
            Frame input;
            input.number = ReplayIO::read_int(item["frame"], 0);
            input.button = static_cast<uint8_t>(button_from_json(item["button"]));
            input.player2 = ReplayIO::read_int(item["player"], 1) == 2;
            input.holding = ReplayIO::read_boolish(item["down"]);
            if (debug || item.contains("x")) {
                FramePose pose;
                pose.x = static_cast<float>(ReplayIO::read_number(item["x"], 0.0));
                pose.y = static_cast<float>(ReplayIO::read_number(item["y"], 0.0));
                pose.rotation = static_cast<float>(ReplayIO::read_number(item["rotation"], 0.0));
                pose.x_velocity = ReplayIO::read_number(item["xVelocity"], 0.0);
                pose.yVelocity = ReplayIO::read_number(item["yVelocity"], 0.0);
                poses.push_back(pose);
            }
            inputs.push_back(input);
        }
        if (poses.size() != inputs.size())
            poses.clear();
        ReplayIO::adopt_inputs(std::move(inputs), static_cast<float>(ReplayIO::read_number(json["fps"], 240.0)), std::move(poses));
        auto& logic = Logic::get();
        if (json.contains("rates") && json["rates"].isArray()) {
            for (auto const& item : json["rates"]) {
                RateChange change;
                change.frame = ReplayIO::read_int(item["frame"], 0);
                change.fps = static_cast<float>(ReplayIO::read_number(item["fps"], 240.0));
                if (change.frame >= 0 && change.fps >= 1.f)
                    logic.rates.push_back(change);
            }
            Logic::get().normalize_rates();
        }
        logic.end_x = static_cast<float>(ReplayIO::read_number(json["endX"], 0.0));
        logic.recording_seconds = ReplayIO::read_number(json["recordingSeconds"], 0.0);
        logic.attempts = ReplayIO::read_int(json["attempts"], 1);
        read_seed_and_flags(json);
        return true;
    }

    static bool read_old(matjson::Value const& json) {
        auto& logic = Logic::get();
        auto format = ReplayFormat::Meta;
        if (auto name = json["version"].asString())
            format = ReplayIO::format_from_name(name.unwrap());
        bool debug = ReplayIO::debug_format(format);
        std::vector<Frame> inputs;
        std::vector<FramePose> poses;
        for (auto const& item : json["inputs"]) {
            Frame input;
            input.number = ReplayIO::read_int(item["frame"], 0);
            input.holding = ReplayIO::read_boolish(item["holding"]);
            input.player2 = item.contains("player_2") && ReplayIO::read_boolish(item["player_2"]);
            int button = ReplayIO::read_int(item["button"], 1);
            if (button < 1 || button > 3)
                button = 1;
            input.button = static_cast<uint8_t>(button);
            if (debug || item.contains("x_position")) {
                FramePose pose;
                pose.x = static_cast<float>(ReplayIO::read_number(item["x_position"], 0.0));
                pose.y = static_cast<float>(ReplayIO::read_number(item["y_position"], 0.0));
                pose.yVelocity = ReplayIO::read_number(item["y_vel"], 0.0);
                pose.x_velocity = ReplayIO::read_number(item["x_vel"], 0.0);
                pose.rotation = static_cast<float>(ReplayIO::read_number(item["rotation"], 0.0));
                poses.push_back(pose);
            }
            inputs.push_back(input);
        }
        if (poses.size() != inputs.size())
            poses.clear();
        ReplayIO::adopt_inputs(std::move(inputs), static_cast<float>(ReplayIO::read_number(json["fps"], 240.0)), std::move(poses));
        logic.end_x = static_cast<float>(ReplayIO::read_number(json["end_xpos"], 0.0));
        if (json["metadata"].isObject()) {
            logic.recording_seconds = ReplayIO::read_number(json["metadata"]["recording_time"], 0.0);
            logic.attempts = ReplayIO::read_int(json["metadata"]["total_attempts"], 1);
        }
        read_seed_and_flags(json);
        return true;
    }
};

bool ReplayIO::write_echo_json(fs::path const& path) {
    auto const& logic = Logic::get();
    bool debug = logic.save_debug && logic.poses.size() == logic.inputs.size();
    auto inputs = logic.inputs;
    auto poses = debug ? logic.poses : std::vector<FramePose>{};
    std::vector<size_t> order(inputs.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return inputs[a].number < inputs[b].number;
    });
    std::vector<Frame> sorted;
    std::vector<FramePose> sorted_poses;
    sorted.reserve(inputs.size());
    if (debug) {
        sorted_poses.reserve(inputs.size());
    }
    for (size_t i : order) {
        auto input = inputs[i];
        if (input.button < 1 || input.button > 3) {
            input.button = 1;
        }
        input.number = std::max(input.number, 0);
        sorted.push_back(input);
        if (debug) {
            sorted_poses.push_back(poses[i]);
        }
    }
    inputs.swap(sorted);
    poses.swap(sorted_poses);
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    file << "{\n";
    file << "\t\"format\": \"echo\",\n";
    file << fmt::format("\t\"version\": {},\n", EchoJson::version);
    file << fmt::format("\t\"fps\": {},\n", EchoJson::number(logic.macro_fps));
    if (!logic.rates.empty()) {
        file << "\t\"rates\": [\n";
        for (size_t i = 0; i < logic.rates.size(); ++i) {
            file << fmt::format("\t\t{{\"frame\": {}, \"fps\": {}}}", logic.rates[i].frame, EchoJson::number(logic.rates[i].fps));
            file << (i + 1 == logic.rates.size() ? "\n" : ",\n");
        }
        file << "\t],\n";
    }
    file << fmt::format("\t\"endX\": {},\n", EchoJson::number(logic.end_x));
    file << fmt::format("\t\"recordingSeconds\": {},\n", EchoJson::number(logic.recording_seconds));
    file << fmt::format("\t\"attempts\": {},\n", logic.attempts);
    if (logic.lvl_seed_known) {
        file << fmt::format("\t\"seed\": \"{}\",\n", logic.lvl_seed);
    }
    if (logic.macro_unrounded) {
        file << "\t\"velocity_unrounding\": true,\n";
    }
    if (logic.macro_silicate_variance) {
        file << "\t\"silicate_variance\": true,\n";
    }
    if (logic.macro_ldm_known) {
        file << fmt::format("\t\"low_detail_mode\": {},\n", logic.macro_ldm);
    }
    file << fmt::format("\t\"debug\": {},\n", debug);
    file << "\t\"inputs\": [";
    for (size_t i = 0; i < inputs.size(); ++i) {
        file << (i == 0 ? "\n\t\t" : ",\n\t\t") << EchoJson::input_line(inputs[i], debug ? poses[i] : FramePose{}, debug);
    }
    file << (inputs.empty() ? "]\n}\n" : "\n\t]\n}\n");
    return static_cast<bool>(file);
}

bool ReplayIO::read_echo_json(fs::path const& path) {
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed) {
        return false;
    }
    auto const& json = parsed.unwrap();
    if (!json.contains("inputs") || !json["inputs"].isArray()) {
        return false;
    }
    return EchoJson::is_current(json) ? EchoJson::read_current(json) : EchoJson::read_old(json);
}
