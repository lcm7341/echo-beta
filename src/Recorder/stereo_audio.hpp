#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

inline std::vector<float> read_captured_stereo(const fs::path& path, int channels) {
	if (channels < 1 || channels > 64) throw std::runtime_error("invalid audio channel count");
	std::ifstream input(path, std::ios::binary | std::ios::ate);
	if (!input) throw std::runtime_error("could not open captured audio");
	auto bytes = input.tellg();
	auto frame_bytes = sizeof(float) * (size_t)channels;
	if (bytes <= 0 || (size_t)bytes % frame_bytes != 0)
		throw std::runtime_error("incomplete captured audio");
	std::vector<float> stereo;
	stereo.reserve((size_t)bytes / frame_bytes * 2);
	input.seekg(0);
	std::vector<float> block((size_t)channels * 65536);
	auto clamp_sample = [](float sample) {
		return std::isfinite(sample) ? std::clamp(sample, -1.f, 1.f) : 0.f;
	};
	while (input) {
		input.read(reinterpret_cast<char*>(block.data()), (std::streamsize)(block.size() * sizeof(float)));
		auto count = (size_t)input.gcount() / sizeof(float);
		for (size_t i = 0; i + (size_t)channels <= count; i += channels) {
			stereo.push_back(clamp_sample(block[i]));
			stereo.push_back(clamp_sample(block[i + (channels > 1 ? 1 : 0)]));
		}
	}
	if (input.bad()) throw std::runtime_error("could not read captured audio");
	return stereo;
}
