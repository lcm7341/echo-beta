#include "Recorder/stereo_audio.hpp"
#include <cassert>
#include <limits>

int main() {
	auto path = std::filesystem::temp_directory_path() / "echo-stereo-test.raw";
	auto write = [&](std::vector<float> const& samples) {
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<char const*>(samples.data()), samples.size() * sizeof(float));
	};
	write({.25f, -.5f, 2.f, std::numeric_limits<float>::quiet_NaN()});
	assert((echo::audio::readStereo(path, 1) == std::vector<float>{.25f, .25f, -.5f, -.5f, 1.f, 1.f, 0.f, 0.f}));
	write({.25f, -.5f, -2.f, std::numeric_limits<float>::infinity()});
	assert((echo::audio::readStereo(path, 2) == std::vector<float>{.25f, -.5f, -1.f, 0.f}));
	// Cross the reader's block boundary: no missing or duplicated samples.
	std::vector<float> samples(65537 * 2);
	for (size_t i = 0; i < samples.size(); ++i) samples[i] = (i % 7) * .1f;
	write(samples);
	assert(echo::audio::readStereo(path, 2) == samples);
	write({.2f, .4f, .6f, .8f, .1f, .3f});
	assert((echo::audio::readStereo(path, 6) == std::vector<float>{.2f, .4f}));
	write({.5f});
	bool failed = false;
	try { echo::audio::readStereo(path, 2); } catch (std::runtime_error const&) { failed = true; }
	assert(failed);
	failed = false;
	try { echo::audio::readStereo(path, 0); } catch (std::runtime_error const&) { failed = true; }
	assert(failed);
	std::filesystem::remove(path);
	failed = false;
	try { echo::audio::readStereo(path, 2); } catch (std::runtime_error const&) { failed = true; }
	assert(failed);
}
