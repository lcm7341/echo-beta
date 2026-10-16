#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct VideoWriter;

std::string open_video(VideoWriter*& writer, const std::string& path, int width, int height, int fps, long long bitrate);
std::string write_frame(VideoWriter* writer, const unsigned char* rgba, bool bottom_up);
std::string close_video(VideoWriter* writer);

std::string begin_direct_frame(VideoWriter* writer, unsigned& texture);
std::string end_direct_frame(VideoWriter* writer);

std::string add_audio(const std::string& video, const float* stereo, std::size_t frames, const std::string& output);
