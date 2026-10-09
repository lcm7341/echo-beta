#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>



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
	static std::string_view buttonName(int button) {
		switch (button) {
			case 2: return "left";
			case 3: return "right";
			default: return "jump";
		}
	}
	static int buttonFromJson(matjson::Value const& json) {
		if (auto name = json.asString()) {
			auto const& text = name.unwrap();
			if (text == "left") return 2;
			if (text == "right") return 3;
			return 1;
		}
		int button = ReplayIO::readInt(json, 1);
		return button >= 1 && button <= 3 ? button : 1;
	}
	static std::string inputLine(Frame const& input, FramePose const& pose, bool debug) {
		auto line = fmt::format(R"({{"frame": {}, "button": "{}", "player": {}, "down": {})",
			input.number, buttonName(input.button), input.player2 ? 2 : 1, input.holding);
		if (debug) {
			line += fmt::format(R"(, "x": {}, "y": {}, "rotation": {}, "xVelocity": {}, "yVelocity": {})",
				number(pose.x), number(pose.y), number(pose.rotation), number(pose.xVelocity), number(pose.yVelocity));
		}
		return line + "}";
	}
	static bool isCurrent(matjson::Value const& json) {
		return json["format"].asString().unwrapOr("") == "echo";
	}
	static bool readCurrent(matjson::Value const& json) {
		bool debug = json["debug"].asBool().unwrapOr(false);
		std::vector<Frame> inputs;
		std::vector<FramePose> poses;
		for (auto const& item : json["inputs"]) {
			Frame input;
			input.number = ReplayIO::readInt(item["frame"], 0);
			input.button = static_cast<uint8_t>(buttonFromJson(item["button"]));
			input.player2 = ReplayIO::readInt(item["player"], 1) == 2;
			input.holding = ReplayIO::readBoolish(item["down"]);
			if (debug || item.contains("x")) {
				FramePose pose;
				pose.x = static_cast<float>(ReplayIO::readNumber(item["x"], 0.0));
				pose.y = static_cast<float>(ReplayIO::readNumber(item["y"], 0.0));
				pose.rotation = static_cast<float>(ReplayIO::readNumber(item["rotation"], 0.0));
				pose.xVelocity = ReplayIO::readNumber(item["xVelocity"], 0.0);
				pose.yVelocity = ReplayIO::readNumber(item["yVelocity"], 0.0);
				poses.push_back(pose);
			}
			inputs.push_back(input);
		}
		if (poses.size() != inputs.size()) poses.clear();
		ReplayIO::adoptInputs(std::move(inputs), static_cast<float>(ReplayIO::readNumber(json["fps"], 240.0)), std::move(poses));
		auto& value = Logic::get();
		if (json.contains("rates") && json["rates"].isArray()) {
			for (auto const& item : json["rates"]) {
				RateChange change;
				change.frame = ReplayIO::readInt(item["frame"], 0);
				change.fps = static_cast<float>(ReplayIO::readNumber(item["fps"], 240.0));
				if (change.frame >= 0 && change.fps >= 1.f) value.rates.push_back(change);
			}
			Logic::get().normalizeRates();
		}
		value.endX = static_cast<float>(ReplayIO::readNumber(json["endX"], 0.0));
		value.recordingSeconds = ReplayIO::readNumber(json["recordingSeconds"], 0.0);
		value.attempts = ReplayIO::readInt(json["attempts"], 1);
		return true;
	}
	static bool readOld(matjson::Value const& json) {
		auto& value = Logic::get();
		auto format = ReplayFormat::Meta;
		if (auto name = json["version"].asString()) format = ReplayIO::formatFromName(name.unwrap());
		bool debug = ReplayIO::debugFormat(format);
		std::vector<Frame> inputs;
		std::vector<FramePose> poses;
		for (auto const& item : json["inputs"]) {
			Frame input;
			input.number = ReplayIO::readInt(item["frame"], 0);
			input.holding = ReplayIO::readBoolish(item["holding"]);
			input.player2 = item.contains("player_2") && ReplayIO::readBoolish(item["player_2"]);
			int button = ReplayIO::readInt(item["button"], 1);
			if (button < 1 || button > 3) button = 1;
			input.button = static_cast<uint8_t>(button);
			if (debug || item.contains("x_position")) {
				FramePose pose;
				pose.x = static_cast<float>(ReplayIO::readNumber(item["x_position"], 0.0));
				pose.y = static_cast<float>(ReplayIO::readNumber(item["y_position"], 0.0));
				pose.yVelocity = ReplayIO::readNumber(item["y_vel"], 0.0);
				pose.xVelocity = ReplayIO::readNumber(item["x_vel"], 0.0);
				pose.rotation = static_cast<float>(ReplayIO::readNumber(item["rotation"], 0.0));
				poses.push_back(pose);
			}
			inputs.push_back(input);
		}
		if (poses.size() != inputs.size()) poses.clear();
		ReplayIO::adoptInputs(std::move(inputs), static_cast<float>(ReplayIO::readNumber(json["fps"], 240.0)), std::move(poses));
		value.endX = static_cast<float>(ReplayIO::readNumber(json["end_xpos"], 0.0));
		if (json["metadata"].isObject()) {
			value.recordingSeconds = ReplayIO::readNumber(json["metadata"]["recording_time"], 0.0);
			value.attempts = ReplayIO::readInt(json["metadata"]["total_attempts"], 1);
		}
		if (json.contains("seed")) {
			if (auto text = json["seed"].asString()) {
				value.levelSeed = std::strtoull(text.unwrap().c_str(), nullptr, 10);
			} else {
				value.levelSeed = static_cast<uint64_t>(ReplayIO::readNumber(json["seed"], 0.0));
			}
			value.levelSeedKnown = true;
			value.levelSeedFromFile = true;
		}
		if (auto flag = json["velocity_unrounding"].asBool()) value.macroUnrounded = flag.unwrap();
		if (auto flag = json["silicate_variance"].asBool()) value.macroSilicateVariance = flag.unwrap();
		return true;
	}
};

bool ReplayIO::writeEchoJson(std::filesystem::path const& path) {
	auto const& value = Logic::get();
	bool debug = value.saveDebug && value.poses.size() == value.inputs.size();
	auto inputs = value.inputs;
	auto poses = debug ? value.poses : std::vector<FramePose>{};
	std::vector<size_t> order(inputs.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return inputs[a].number < inputs[b].number; });
	std::vector<Frame> sorted;
	std::vector<FramePose> sortedPoses;
	sorted.reserve(inputs.size());
	if (debug) sortedPoses.reserve(inputs.size());
	for (size_t i : order) {
		auto input = inputs[i];
		if (input.button < 1 || input.button > 3) input.button = 1;
		input.number = std::max(input.number, 0);
		sorted.push_back(input);
		if (debug) sortedPoses.push_back(poses[i]);
	}
	inputs.swap(sorted);
	poses.swap(sortedPoses);
	std::ofstream file(path);
	if (!file) return false;
	file << "{\n";
	file << "\t\"format\": \"echo\",\n";
	file << fmt::format("\t\"version\": {},\n", EchoJson::version);
	file << fmt::format("\t\"fps\": {},\n", EchoJson::number(value.macroFps));
	if (!value.rates.empty()) {
		file << "\t\"rates\": [\n";
		for (size_t i = 0; i < value.rates.size(); ++i) {
			file << fmt::format("\t\t{{\"frame\": {}, \"fps\": {}}}", value.rates[i].frame, EchoJson::number(value.rates[i].fps));
			file << (i + 1 == value.rates.size() ? "\n" : ",\n");
		}
		file << "\t],\n";
	}
	file << fmt::format("\t\"endX\": {},\n", EchoJson::number(value.endX));
	file << fmt::format("\t\"recordingSeconds\": {},\n", EchoJson::number(value.recordingSeconds));
	file << fmt::format("\t\"attempts\": {},\n", value.attempts);
	if (value.levelSeedKnown) file << fmt::format("\t\"seed\": \"{}\",\n", value.levelSeed);
	if (value.macroUnrounded) file << "\t\"velocity_unrounding\": true,\n";
	if (value.macroSilicateVariance) file << "\t\"silicate_variance\": true,\n";
	file << fmt::format("\t\"debug\": {},\n", debug);
	file << "\t\"inputs\": [";
	for (size_t i = 0; i < inputs.size(); ++i) {
		file << (i == 0 ? "\n\t\t" : ",\n\t\t") << EchoJson::inputLine(inputs[i], debug ? poses[i] : FramePose{}, debug);
	}
	file << (inputs.empty() ? "]\n}\n" : "\n\t]\n}\n");
	return static_cast<bool>(file);
}
bool ReplayIO::readEchoJson(std::filesystem::path const& path) {
	std::ifstream file(path);
	auto parsed = matjson::Value::parse(file);
	if (!parsed) return false;
	auto const& json = parsed.unwrap();
	if (!json.contains("inputs") || !json["inputs"].isArray()) return false;
	return EchoJson::isCurrent(json) ? EchoJson::readCurrent(json) : EchoJson::readOld(json);
}
