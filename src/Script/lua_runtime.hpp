#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

struct lua_State;

namespace Script {
	class Runtime {
	public:
		explicit Runtime(unsigned long long memory_limit);
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		static Runtime* from(lua_State* lua);
		lua_State* state() const;
		bool run_file(const std::filesystem::path& path);
		bool run_text(std::string_view text, const std::string& name);
		bool call(int args, int results);
		void limit_instructions(long long instructions);
		void charge(lua_State* running, long long work);
		void guard_freezes(const std::filesystem::path& marker, const std::string& note);
		const std::string& error() const;

		lua_State* lua = nullptr;
		unsigned long long memory_used = 0;
		unsigned long long memory_limit = 0;
		long long instructions_left = 0;
		bool counting = false;
		std::string last_error;
		std::function<void(const std::string&)> on_print;
		std::filesystem::path freeze_marker;
		std::string freeze_note;
	};

	bool froze_last_time(const std::filesystem::path& marker, std::string& note);
}
