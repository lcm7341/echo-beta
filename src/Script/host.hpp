#pragma once

#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace Script {
	class Host {
	public:
		virtual ~Host() = default;

		virtual const char* name() const = 0;
		virtual std::filesystem::path folder() const = 0;
		virtual std::vector<std::filesystem::path> files() const = 0;
		virtual std::vector<std::filesystem::path> examples() const = 0;
		virtual std::string label(const std::filesystem::path& file) const = 0;
		virtual std::filesystem::path current() const = 0;
		virtual const std::vector<std::string>& names() const = 0;

		virtual bool make_new(std::filesystem::path& made) = 0;
		virtual bool install_example(const std::filesystem::path& example, std::filesystem::path& made) = 0;
		virtual bool remove_file(const std::filesystem::path& file) = 0;

		virtual void run_text(const std::string& text, const std::filesystem::path& file) = 0;
		virtual void run_file(const std::filesystem::path& file) = 0;
		virtual void stop() = 0;
		virtual bool running() const = 0;
		virtual bool reruns() const = 0;

		virtual std::vector<std::filesystem::path> running_files() const {
			if (running()) return { current() };
			return {};
		}

		virtual void stop_file(const std::filesystem::path&) {
			stop();
		}

		virtual bool can_autorun() const {
			return false;
		}

		virtual bool autoruns(const std::filesystem::path&) const {
			return false;
		}

		virtual void set_autorun(const std::filesystem::path&, bool) {}

		virtual const std::string& error() const = 0;
		virtual const std::deque<std::string>& output() const = 0;
		virtual void clear_output() = 0;
	};
}
