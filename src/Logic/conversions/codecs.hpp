#pragma once

#include "Logic/logic.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct LegacyMacro {
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	ReplayFormat format = ReplayFormat::Meta;
	float fps = 240.f;
	float endX = 0.f;
	double seconds = 0.0;
	int attempts = 1;
};

struct MacroSnap {
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	ReplayFormat format = ReplayFormat::Meta;
	float fps = 240.f;
	float endX = 0.f;
	double recordingSeconds = 0.0;
	int attempts = 1;
	bool saveDebug = false;
	uint64_t levelSeed = 0;
	bool levelSeedKnown = false;
	bool unrounded = false;
	bool silicateVariance = false;
	std::vector<RateChange> rates;
	std::string highestCps = "P1: 0";
	std::vector<CpsNote> cpsNotesP1;
	std::vector<CpsNote> cpsNotesP2;
};

class ByteCursor {
public:
	std::vector<uint8_t> data;
	size_t at = 0;
	bool failed = false;

	size_t left() const {
		return failed || at >= data.size() ? 0 : data.size() - at;
	}
	bool take(size_t count) {
		if (left() < count) {
			failed = true;
			return false;
		}
		at += count;
		return true;
	}
	uint64_t uint(size_t width) {
		if (!take(width)) return 0;
		uint64_t value = 0;
		for (size_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(data[at - width + i]) << (8 * i);
		return value;
	}
	template <typename T>
	T pod() {
		T value{};
		if (take(sizeof(T))) std::memcpy(&value, data.data() + at - sizeof(T), sizeof(T));
		return value;
	}
	uint64_t leb128() {
		uint64_t value = 0;
		for (int shift = 0; shift < 64; shift += 7) {
			auto byte = uint(1);
			if (failed) return 0;
			value |= (byte & 0x7f) << shift;
			if (!(byte & 0x80)) return value;
		}
		failed = true;
		return 0;
	}
};

class ByteSink {
public:
	std::vector<uint8_t> data;

	void uint(uint64_t value, size_t width) {
		for (size_t i = 0; i < width; ++i) data.push_back(static_cast<uint8_t>(value >> (8 * i)));
	}
	template <typename T>
	void pod(T const& value) {
		auto const* bytes = reinterpret_cast<uint8_t const*>(&value);
		data.insert(data.end(), bytes, bytes + sizeof(T));
	}
	void leb128(uint64_t value) {
		do {
			uint8_t byte = value & 0x7f;
			value >>= 7;
			data.push_back(value ? byte | 0x80 : byte);
		} while (value);
	}
	void text(std::string_view value) {
		data.insert(data.end(), value.begin(), value.end());
	}
	bool save(std::filesystem::path const& path) const {
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
		return static_cast<bool>(file);
	}
};

class ReplayIO {
public:
	static std::string lowerExt(std::filesystem::path const& path);
	static std::string sanitize(std::string name);
	static std::string stripNumbering(std::string const& base, std::string const& format);
	static std::filesystem::path uniqueFile(std::filesystem::path const& directory, std::string const& base, std::string const& ext);
	static bool readBoolish(matjson::Value const& json);
	static double readNumber(matjson::Value const& json, double fallback);
	static int readInt(matjson::Value const& json, int fallback);
	static std::string formatName(ReplayFormat format);
	static ReplayFormat formatFromName(std::string const& name);
	static bool debugFormat(ReplayFormat format);
	static std::string clock(double seconds);
	static void finishLoad(std::string const& detail);
	static bool writeEchoJson(std::filesystem::path const& path);
	static bool readEchoJson(std::filesystem::path const& path);
	template <typename A1>
	static bool readPod(std::vector<uint8_t> const& data, size_t& cursor, A1& value);
	static bool parseLegacy(std::vector<uint8_t> const& data, int stringBytes, bool withMeta, LegacyMacro& out);
	static std::optional<LegacyMacro> readLegacyBytes(std::vector<uint8_t> const& data);
	static std::vector<uint8_t> readAll(std::filesystem::path const& path);
	static bool readLegacyEcho(std::filesystem::path const& path);
	static std::optional<MacroSnap> loadSnap(std::filesystem::path const& path);
	static bool fillReplay(std::filesystem::path const& path);
	static bool fillRaw(std::filesystem::path const& path);
	static bool saveAs(std::filesystem::path const& path, bool (*write)(std::filesystem::path const&));
	static bool isAutosave(std::filesystem::path const& path);
	static MacroSnap captureMacro();
	static void restoreMacro(MacroSnap const& snap);
	static bool fileIsPacked(std::filesystem::path const& path);
	static std::string importStem(std::filesystem::path const& path);
	static void adoptInputs(std::vector<Frame> inputs, float fps, std::vector<FramePose> poses = {});
	static std::vector<Frame> sortedInputs();
	static int packButton(int button);
	static int unpackButton(int code);
	template <typename A1>
	static void writePod(std::ostream& file, A1 const& value);
	template <typename A1>
	static bool readPod(std::istream& file, A1& value);
	static bool writePackedEcho(std::filesystem::path const& path);
	static bool readPackedEcho(std::filesystem::path const& path);
	static bool readPlain(std::filesystem::path const& path);
	static bool writePlain(std::filesystem::path const& path);
	static bool readSilicate(std::filesystem::path const& path);
	static bool writeSilicate(std::filesystem::path const& path);
	static bool readTcbot(std::filesystem::path const& path);
	static bool writeTcbot(std::filesystem::path const& path);
	static bool readGdr(std::filesystem::path const& path);
	static bool writeGdr(std::filesystem::path const& path);
};
