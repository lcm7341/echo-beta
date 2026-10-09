#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>






template <typename A1>
bool ReplayIO::readPod(std::vector<uint8_t> const& data, size_t& cursor, A1& value) {
	if (cursor + sizeof(value) > data.size()) return false;
	std::memcpy(&value, data.data() + cursor, sizeof(value));
	cursor += sizeof(value);
	return true;
}

bool ReplayIO::parseLegacy(std::vector<uint8_t> const& data, int stringBytes, bool withMeta, LegacyMacro& out) {
	if (stringBytes != 24 && stringBytes != 32) return false;
	size_t cursor = 0;
	if (data.size() < static_cast<size_t>(stringBytes) + 8) return false;
	uint64_t length = 0;
	uint64_t capacity = 0;
	if (stringBytes == 32) {
		std::memcpy(&length, data.data() + 16, 8);
		std::memcpy(&capacity, data.data() + 24, 8);
	} else {
		uint32_t shortLength = 0;
		uint32_t shortCapacity = 0;
		std::memcpy(&shortLength, data.data() + 16, 4);
		std::memcpy(&shortCapacity, data.data() + 20, 4);
		length = shortLength;
		capacity = shortCapacity;
	}
	if (length < 4 || length > 7 || capacity < length || capacity > 15) return false;
	std::string name(reinterpret_cast<char const*>(data.data()), static_cast<size_t>(length));
	if (name != "SIMPLE" && name != "DEBUG" && name != "META" && name != "METADBG") return false;
	cursor = static_cast<size_t>(stringBytes);
	float fps = 0.f;
	float endX = 0.f;
	double seconds = 0.0;
	int32_t clicks = 0;
	int32_t attempts = 1;
	if (!ReplayIO::readPod(data, cursor, fps) || !ReplayIO::readPod(data, cursor, endX)) return false;
	if (withMeta) {
		if (!ReplayIO::readPod(data, cursor, seconds) || !ReplayIO::readPod(data, cursor, clicks) || !ReplayIO::readPod(data, cursor, attempts)) return false;
	}
	if (!std::isfinite(fps) || fps < 1.f || fps > 100000.f) return false;
	if (!std::isfinite(endX) || !std::isfinite(seconds)) return false;
	if (attempts < 0 || attempts > 100000000 || clicks < 0) return false;
	bool debug = name == "DEBUG" || name == "METADBG";
	LegacyMacro parsed;
	parsed.format = ReplayIO::formatFromName(name);
	parsed.fps = fps;
	parsed.endX = endX;
	parsed.seconds = seconds;
	parsed.attempts = attempts;
	int previous = -1;
	while (cursor < data.size()) {
		if (cursor + 6 > data.size()) return false;
		int32_t number = 0;
		uint8_t holding = 0;
		uint8_t player2 = 0;
		if (!ReplayIO::readPod(data, cursor, number) || !ReplayIO::readPod(data, cursor, holding) || !ReplayIO::readPod(data, cursor, player2)) return false;
		if (holding > 1 || player2 > 1 || number < 0 || number > 100000000 || previous > number) return false;
		previous = number;
		Frame input;
		input.number = number;
		input.holding = holding != 0;
		input.player2 = player2 != 0;
		input.button = static_cast<uint8_t>(PlayerButton::Jump);
		FramePose pose;
		if (debug) {
			if (!ReplayIO::readPod(data, cursor, pose.x) || !ReplayIO::readPod(data, cursor, pose.yVelocity) || !ReplayIO::readPod(data, cursor, pose.xVelocity)) return false;
			if (!ReplayIO::readPod(data, cursor, pose.y) || !ReplayIO::readPod(data, cursor, pose.rotation)) return false;
			if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.rotation) || !std::isfinite(pose.yVelocity) || !std::isfinite(pose.xVelocity)) return false;
		}
		parsed.inputs.push_back(input);
		if (debug) parsed.poses.push_back(pose);
	}
	if (cursor != data.size()) return false;
	out = std::move(parsed);
	return true;
}

std::optional<LegacyMacro> ReplayIO::readLegacyBytes(std::vector<uint8_t> const& data) {
	int const widths[] = {32, 24};
	bool const metas[] = {true, false};
	for (int width : widths) {
		for (bool meta : metas) {
			LegacyMacro parsed;
			if (ReplayIO::parseLegacy(data, width, meta, parsed)) return parsed;
		}
	}
	return std::nullopt;
}

std::vector<uint8_t> ReplayIO::readAll(std::filesystem::path const& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) return {};
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}



bool ReplayIO::readLegacyEcho(std::filesystem::path const& path) {
	auto parsed = ReplayIO::readLegacyBytes(ReplayIO::readAll(path));
	if (!parsed) return false;
	auto& value = Logic::get();
	value.inputs = std::move(parsed->inputs);
	value.poses = std::move(parsed->poses);
	if (value.poses.size() != value.inputs.size()) value.poses.clear();
	value.inputIds.clear();
	value.touchInputs();
	Logic::get().refreshLiveHold();
	value.format = parsed->format;
	value.fps = parsed->fps;
	value.endX = parsed->endX;
	value.recordingSeconds = parsed->seconds;
	value.attempts = parsed->attempts;
	return true;
}

