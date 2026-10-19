#pragma once

#include <Geode/Geode.hpp>

#include <lauxlib.h>
#include <lua.h>

#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#define MAX_OUTPUT_LINES 200
#define MAX_OUTPUT_LINE_BYTES 4096

namespace Script {
	inline bool is_bundled(const std::filesystem::path& file) {
		return file.parent_path() == geode::Mod::get()->getResourcesDir();
	}

	inline std::string bundled_name(const std::filesystem::path& file, const char* prefix) {
		std::string slug = geode::utils::string::pathToString(file.stem()).substr(std::strlen(prefix));
		std::string name;
		bool word_start = true;
		for (char letter : slug) {
			if (letter == '-') {
				name += ' ';
				word_start = true;
				continue;
			}
			name += word_start ? (char)std::toupper((unsigned char)letter) : letter;
			word_start = false;
		}
		return name;
	}

	inline std::string chunk_name(const std::filesystem::path& file, const std::filesystem::path& folder) {
		std::filesystem::path relative = file.lexically_relative(folder);
		if (relative.empty() || *relative.begin() == "..") return geode::utils::string::pathToString(file.filename());
		auto text = relative.generic_u8string();
		return std::string(text.begin(), text.end());
	}

	inline bool read_text(const std::filesystem::path& file, std::string& text) {
		std::ifstream stream(file, std::ios::binary);
		if (!stream) return false;
		text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		return true;
	}

	inline bool write_text(const std::filesystem::path& file, const std::string& text) {
		std::ofstream stream(file, std::ios::binary | std::ios::trunc);
		stream << text;
		return (bool)stream;
	}

	inline void add_output(std::deque<std::string>& output, const std::string& text) {
		output.push_back(text.substr(0, MAX_OUTPUT_LINE_BYTES));
		if (output.size() > MAX_OUTPUT_LINES) output.pop_front();
	}

	inline void set_number(lua_State* lua, const char* key, double value) {
		lua_pushnumber(lua, value);
		lua_setfield(lua, -2, key);
	}

	inline void set_bool(lua_State* lua, const char* key, bool value) {
		lua_pushboolean(lua, value);
		lua_setfield(lua, -2, key);
	}

	inline void set_string(lua_State* lua, const char* key, const char* value) {
		lua_pushstring(lua, value);
		lua_setfield(lua, -2, key);
	}

	inline int take_function(lua_State* lua, const char* name) {
		lua_getglobal(lua, name);
		if (lua_isfunction(lua, -1)) return luaL_ref(lua, LUA_REGISTRYINDEX);
		lua_pop(lua, 1);
		return LUA_NOREF;
	}
}
