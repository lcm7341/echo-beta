#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <Geode/utils/async.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>




using namespace geode::prelude;


std::string ReplayIO::lowerExt(std::filesystem::path const& path) {
	auto ext = path.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
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
	return name;
}
std::string ReplayIO::stripNumbering(std::string const& base, std::string const& format) {
	auto hash = format.find('#');
	if (hash == std::string::npos) return base;
	auto prefix = format.substr(0, hash);
	auto suffix = format.substr(hash + 1);
	std::string root = base;
	for (;;) {
		if (root.size() < prefix.size() + suffix.size() + 1) break;
		if (root.compare(root.size() - suffix.size(), suffix.size(), suffix) != 0) break;
		size_t numberEnd = root.size() - suffix.size();
		size_t numberStart = numberEnd;
		while (numberStart > 0 && std::isdigit(static_cast<unsigned char>(root[numberStart - 1]))) --numberStart;
		if (numberStart == numberEnd) break;
		if (numberStart < prefix.size()) break;
		if (root.compare(numberStart - prefix.size(), prefix.size(), prefix) != 0) break;
		root.resize(numberStart - prefix.size());
	}
	return root.empty() ? base : root;
}
std::filesystem::path ReplayIO::uniqueFile(std::filesystem::path const& directory, std::string const& base, std::string const& ext) {
	std::error_code error;
	std::filesystem::create_directories(directory, error);
	auto format = Logic::get().renameFormat.empty() ? std::string("_#") : Logic::get().renameFormat;
	if (format.find('#') == std::string::npos) format += "#";
	auto root = ReplayIO::stripNumbering(base, format);
	auto plain = directory / (root + ext);
	if (!std::filesystem::exists(plain)) return plain;
	for (int n = 1; n < 10000; ++n) {
		auto suffix = format;
		auto hash = suffix.find('#');
		suffix.replace(hash, 1, std::to_string(n));
		auto candidate = directory / (root + suffix + ext);
		if (!std::filesystem::exists(candidate)) return candidate;
	}
	return plain;
}
bool ReplayIO::readBoolish(matjson::Value const& json) {
	if (json.isBool()) return json.asBool().unwrapOr(false);
	if (json.isNumber()) return json.asInt().unwrapOr(0) != 0;
	return false;
}
double ReplayIO::readNumber(matjson::Value const& json, double fallback) {
	if (auto number = json.asDouble()) return number.unwrap();
	return fallback;
}
int ReplayIO::readInt(matjson::Value const& json, int fallback) {
	if (auto number = json.asInt()) return static_cast<int>(number.unwrap());
	return fallback;
}
std::string ReplayIO::formatName(ReplayFormat format) {
	switch (format) {
		case ReplayFormat::Simple: return "SIMPLE";
		case ReplayFormat::Debug: return "DEBUG";
		case ReplayFormat::MetaDebug: return "METADBG";
		case ReplayFormat::Meta: return "META";
	}
	return "META";
}
ReplayFormat ReplayIO::formatFromName(std::string const& name) {
	if (name == "SIMPLE") return ReplayFormat::Simple;
	if (name == "DEBUG") return ReplayFormat::Debug;
	if (name == "METADBG") return ReplayFormat::MetaDebug;
	return ReplayFormat::Meta;
}
bool ReplayIO::debugFormat(ReplayFormat format) {
	return format == ReplayFormat::Debug || format == ReplayFormat::MetaDebug;
}
std::string ReplayIO::clock(double seconds) {
	int total = static_cast<int>(seconds);
	int hours = total / 3600;
	int minutes = (total % 3600) / 60;
	int secs = total % 60;
	return fmt::format("{:02}:{:02}:{:02}", hours, minutes, secs);
}
void ReplayIO::adoptInputs(std::vector<Frame> inputs, float fps, std::vector<FramePose> poses) {
	auto& value = Logic::get();
	value.inputs = std::move(inputs);
	value.poses = std::move(poses);
	if (value.poses.size() != value.inputs.size()) value.poses.clear();
	value.inputIds.clear();
	value.touchInputs();
	Logic::get().refreshLiveHold();
	value.fps = std::isfinite(fps) && fps > 0.f ? fps : 240.f;
	value.macroFps = value.fps;
	value.rates.clear();
	value.format = ReplayFormat::Meta;
	value.endX = 0.f;
	value.recordingSeconds = 0.0;
	value.attempts = 1;
}
std::vector<Frame> ReplayIO::sortedInputs() {
	auto inputs = Logic::get().inputs;
	std::stable_sort(inputs.begin(), inputs.end(), [](Frame const& a, Frame const& b) {
		return a.number < b.number;
	});
	for (auto& input : inputs) {
		if (input.button < 1 || input.button > 3) input.button = 1;
		input.number = std::max(input.number, 0);
	}
	return inputs;
}
void ReplayIO::finishLoad(std::string const& detail) {
	auto& value = Logic::get();
	value.accuracy = 1;
	if ((value).isRecording()) value.state = RunState::Playing;
	else if (!(value).isPlaying()) value.state = RunState::Playing;
	value.replayPos = 0;
	value.message = detail;
	Logic::get().notify(detail, true);
}

