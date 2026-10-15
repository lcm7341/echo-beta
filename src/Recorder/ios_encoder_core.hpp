#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace echo::apple {
	struct VideoWriter;

	std::string open_video(VideoWriter*& writer, std::string const& path, int width, int height, int fps, long long bitrate);
	std::string writeFrame(VideoWriter* writer, std::uint8_t const* rgba, bool bottom_up);
	std::string close_video(VideoWriter* writer);

	std::string begin_direct_frame(VideoWriter* writer, unsigned& texture);
	std::string end_direct_frame(VideoWriter* writer);

	std::string add_audio(std::string const& video, float const* stereo, std::size_t frames, std::string const& output);
}
