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

struct MacroSnap {
	std::string name;
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	ReplayFormat format = ReplayFormat::Meta;
	float fps = 240.f;
	float end_x = 0.f;
	double recording_seconds = 0.0;
	int attempts = 1;
	bool save_debug = false;
	unsigned long long lvl_seed = 0;
	bool lvl_seed_known = false;
	bool unrounded = false;
	bool silicate_variance = false;
	bool ldm = false, ldm_known = false;
	std::vector<RateChange> rates;
	std::string highest_cps = "P1: 0";
	std::vector<CpsNote> cps_notes_p1, cps_notes_p2;
};

class ByteCursor {
public:
	std::vector<unsigned char> data;
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

	unsigned long long uint(size_t width) {
		if (!take(width)) return 0;
		unsigned long long value = 0;
		for (size_t i = 0; i < width; ++i)
			value |= (unsigned long long)data[at - width + i] << (8 * i);
		return value;
	}

	template <typename T>
	T pod() {
		T value{};
		if (take(sizeof(T))) std::memcpy(&value, data.data() + at - sizeof(T), sizeof(T));
		return value;
	}

	unsigned long long leb128() {
		unsigned long long value = 0;
		for (int shift = 0; shift < 64; shift += 7) {
			auto byte = uint(1);
			if (failed) return 0;
			value |= (byte & 127) << shift;
			if (!(byte & 128)) return value;
		}
		failed = true;
		return 0;
	}
};

class ByteSink {
public:
	std::vector<unsigned char> data;

	void uint(unsigned long long value, size_t width) {
		for (size_t i = 0; i < width; ++i)
			data.push_back((unsigned char)(value >> (8 * i)));
	}

	template <typename T>
	void pod(const T& value) {
		const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
		data.insert(data.end(), bytes, bytes + sizeof(T));
	}

	void leb128(unsigned long long value) {
		do {
			unsigned char byte = value & 127;
			value >>= 7;
			data.push_back(value ? byte | 128 : byte);
		} while (value);
	}

	void text(std::string_view value) {
		data.insert(data.end(), value.begin(), value.end());
	}

	bool save(const fs::path& path) const {
		std::ofstream file(path, std::ios::binary);
		file.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
		return (bool)file;
	}
};

class ReplayIO {
public:
	static const unsigned long long max_inputs = 1ull << 26;

	static std::string lower_ext(const fs::path& path);
	static std::string sanitize(std::string name);
	static std::string strip_numbering(const std::string& base, const std::string& format);
	static fs::path unique_file(const fs::path& directory, const std::string& base, const std::string& ext);
	static bool read_boolish(const matjson::Value& json);
	static double read_number(const matjson::Value& json, double fallback);
	static int read_int(const matjson::Value& json, int fallback);
	static std::string format_name(ReplayFormat format);
	static ReplayFormat format_from_name(const std::string& name);
	static bool debug_format(ReplayFormat format);
	static std::string clock(double seconds);
	static void finish_load(const std::string& detail);
	static bool write_echo_json(const fs::path& path, const MacroSnap& snap);
	static bool read_echo_json(const fs::path& path, MacroSnap& out);
	template <typename T>
	static bool read_pod(const std::vector<unsigned char>& data, size_t& cursor, T& value);
	static bool parse_legacy(const std::vector<unsigned char>& data, int string_bytes, bool with_meta, MacroSnap& out);
	static std::vector<unsigned char> read_all(const fs::path& path);
	static bool read_legacy_echo(const fs::path& path, MacroSnap& out);
	static std::optional<MacroSnap> load_snap(const fs::path& path);
	static bool read_any(const fs::path& path, MacroSnap& out);
	static bool save_as(const fs::path& path, bool (*write)(const fs::path&, const MacroSnap&));
	static bool save_snap(const fs::path& path, MacroSnap snap, bool (*write)(const fs::path&, const MacroSnap&));
	static bool is_autosave(const fs::path& path);
	static MacroSnap capture_macro();
	static void restore_macro(const MacroSnap& snap);
	static bool is_echo_file(const fs::path& path);
	static std::string import_stem(const fs::path& path);
	static void adopt_inputs(MacroSnap& out, std::vector<Frame> inputs, float fps, std::vector<FramePose> poses = {});
	static std::vector<Frame> sorted_inputs(const MacroSnap& snap);
	static double snap_sample_rate(const MacroSnap& snap);
	static int button_code(int button);
	static int code_button(int code);
	template <typename T>
	static void write_pod(std::ostream& file, const T& value);
	template <typename T>
	static bool read_pod(std::istream& file, T& value);
	static bool write_echo(const fs::path& path, const MacroSnap& snap);
	static bool read_echo(const fs::path& path, MacroSnap& out);
	static bool read_plain(const fs::path& path, MacroSnap& out);
	static bool write_plain(const fs::path& path, const MacroSnap& snap);
	static bool read_silicate(const fs::path& path, MacroSnap& out);
	static bool write_silicate(const fs::path& path, const MacroSnap& snap);
	static bool read_tcbot(const fs::path& path, MacroSnap& out);
	static bool write_tcbot(const fs::path& path, const MacroSnap& snap);
	static bool read_gdr(const fs::path& path, MacroSnap& out);
	static bool write_gdr(const fs::path& path, const MacroSnap& snap);
};
