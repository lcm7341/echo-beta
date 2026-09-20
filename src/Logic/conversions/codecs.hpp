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

namespace fs = std::filesystem;

struct LegacyMacro {
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	ReplayFormat format = ReplayFormat::Meta;
	float fps = 240.f;
	float end_x = 0.f;
	double seconds = 0.0;
	int attempts = 1;
};

struct MacroSnap {
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	ReplayFormat format = ReplayFormat::Meta;
	float fps = 240.f;
	float end_x = 0.f;
	double recording_seconds = 0.0;
	int attempts = 1;
	bool save_debug = false;
	uint64_t lvl_seed = 0;
	bool lvl_seed_known = false;
	bool unrounded = false;
	bool silicate_variance = false;
	std::vector<RateChange> rates;
	std::string highest_cps = "P1: 0";
	std::vector<CpsNote> cps_notes_p1, cps_notes_p2;
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
		if (!take(width))
			return 0;
		uint64_t value = 0;
		for (size_t i = 0; i < width; ++i)
			value |= static_cast<uint64_t>(data[at - width + i]) << (8 * i);
		return value;
	}

	template <typename T>
	T pod() {
		T value{};
		if (take(sizeof(T)))
			std::memcpy(&value, data.data() + at - sizeof(T), sizeof(T));
		return value;
	}

	uint64_t leb128() {
		uint64_t value = 0;
		for (int shift = 0; shift < 64; shift += 7) {
			auto byte = uint(1);
			if (failed)
				return 0;
			value |= (byte & 0x7f) << shift;
			if (!(byte & 0x80))
				return value;
		}
		failed = true;
		return 0;
	}
};

class ByteSink {
public:
	std::vector<uint8_t> data;

	void uint(uint64_t value, size_t width) {
		for (size_t i = 0; i < width; ++i)
			data.push_back(static_cast<uint8_t>(value >> (8 * i)));
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

	bool save(fs::path const& path) const {
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
		return static_cast<bool>(file);
	}
};

class ReplayIO {
public:
	static std::string lower_ext(fs::path const& path);
	static std::string sanitize(std::string name);
	static std::string strip_numbering(std::string const& base, std::string const& format);
	static fs::path unique_file(fs::path const& directory, std::string const& base, std::string const& ext);
	static bool read_boolish(matjson::Value const& json);
	static double read_number(matjson::Value const& json, double fallback);
	static int read_int(matjson::Value const& json, int fallback);
	static std::string format_name(ReplayFormat format);
	static ReplayFormat format_from_name(std::string const& name);
	static bool debug_format(ReplayFormat format);
	static std::string clock(double seconds);
	static void finish_load(std::string const& detail);
	static bool write_echo_json(fs::path const& path);
	static bool read_echo_json(fs::path const& path);
	template <typename A1>
	static bool read_pod(std::vector<uint8_t> const& data, size_t& cursor, A1& value);
	static std::vector<uint8_t> read_all(fs::path const& path);
	static bool read_legacy_echo(fs::path const& path);
	static std::optional<MacroSnap> load_snap(fs::path const& path);
	static bool fill_replay(fs::path const& path);
	static bool fill_raw(fs::path const& path);
	static bool save_as(fs::path const& path, bool (*write)(fs::path const&));
	static bool is_autosave(fs::path const& path);
	static MacroSnap capture_macro();
	static void restore_macro(MacroSnap const& snap);
	static bool file_is_packed(fs::path const& path);
	static std::string import_stem(fs::path const& path);
	static void adopt_inputs(std::vector<Frame> inputs, float fps, std::vector<FramePose> poses = {});
	static std::vector<Frame> sorted_inputs();
	static int pack_button(int button);
	static int unpack_button(int code);
	template <typename A1>
	static void write_pod(std::ostream& file, A1 const& value);
	template <typename A1>
	static bool read_pod(std::istream& file, A1& value);
	static bool write_packed_echo(fs::path const& path);
	static bool read_packed_echo(fs::path const& path);
	static bool read_plain(fs::path const& path);
	static bool write_plain(fs::path const& path);
	static bool read_silicate(fs::path const& path);
	static bool write_silicate(fs::path const& path);
	static bool read_tcbot(fs::path const& path);
	static bool write_tcbot(fs::path const& path);
	static bool read_gdr(fs::path const& path);
	static bool write_gdr(fs::path const& path);
};
