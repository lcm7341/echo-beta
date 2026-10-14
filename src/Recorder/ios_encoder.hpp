#pragma once
#include "ios_encoder_core.hpp"
#include <Geode/Result.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ffmpeg {
	enum class PixelFormat : int {
		RGBA,
		BGRA
	};

	struct RenderSettings {
		PixelFormat m_pixelFormat = PixelFormat::RGBA;
		std::string m_codec;
		std::int64_t m_bitrate = 0;
		std::uint32_t m_width = 0, m_height = 0;
		std::uint16_t m_fps = 60;
		fs::path m_outputFile;
		bool m_doVerticalFlip = true;
		std::string m_colorspaceFilters;
	};

	namespace events {
		class Recorder {
		public:
			Recorder() = default;
			Recorder(Recorder const&) = delete;
			Recorder& operator=(Recorder const&) = delete;

			~Recorder() {
				stop();
			}

			bool isValid() const {
				return true;
			}

			static std::vector<std::string> getAvailableCodecs() {
				return { "h264_videotoolbox" };
			}

			geode::Result<void, std::string> init(RenderSettings const& settings) {
				stop();
				if (settings.m_pixelFormat != PixelFormat::RGBA)
					return geode::Err(std::string("Only RGBA frames are supported"));
				m_bytes = static_cast<std::size_t>(settings.m_width) * settings.m_height * 4;
				m_bottomUp = settings.m_doVerticalFlip;
				auto failed = echo::apple::open_video(m_writer, settings.m_outputFile.string(), static_cast<int>(settings.m_width), static_cast<int>(settings.m_height), settings.m_fps, settings.m_bitrate);
				if (!failed.empty())
					return geode::Err(std::move(failed));
				return geode::Ok();
			}

			geode::Result<void, std::string> writeFrame(std::vector<std::uint8_t> const& frame) {
				if (frame.size() < m_bytes)
					return geode::Err(std::string("A frame was the wrong size"));
				auto failed = echo::apple::writeFrame(m_writer, frame.data(), m_bottomUp);
				if (!failed.empty())
					return geode::Err(std::move(failed));
				return geode::Ok();
			}

			bool beginDirect(unsigned& texture, std::string& failed) {
				failed = echo::apple::begin_direct_frame(m_writer, texture);
				return failed.empty();
			}

			bool endDirect(std::string& failed) {
				failed = echo::apple::end_direct_frame(m_writer);
				return failed.empty();
			}

			void stop() {
				if (!m_writer)
					return;
				echo::apple::close_video(m_writer);
				m_writer = nullptr;
			}

		private:
			echo::apple::VideoWriter* m_writer = nullptr;
			std::size_t m_bytes = 0;
			bool m_bottomUp = true;
		};

		struct AudioMixer {
			static geode::Result<void, std::string> mixVideoRaw(fs::path const& video, std::vector<float> const& stereo, fs::path const& output) {
				auto failed = echo::apple::add_audio(video.string(), stereo.data(), stereo.size() / 2, output.string());
				if (!failed.empty())
					return geode::Err(std::move(failed));
				return geode::Ok();
			}
		};
	}
}
