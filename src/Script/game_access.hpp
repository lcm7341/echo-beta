#pragma once

#include <Geode/Geode.hpp>

#include <lauxlib.h>
#include <lua.h>

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace GameAccess {
	using FieldAccess = int (*)(lua_State* lua, void* self, int value_index);
	using Upcast = void* (*)(void* self);

	struct Field {
		const char* name;
		FieldAccess access;
	};

	struct Method {
		const char* name;
		lua_CFunction call;
		int args;
	};

	struct ClassInfo {
		const char* name;
		const Field* fields;
		int field_count;
		const Method* methods;
		int method_count;
		int base;
		Upcast to_base;
	};

	template <class T>
	inline const int class_id = -1;
	template <>
	inline const int class_id<GJBaseGameLayer> = 0;
	template <>
	inline const int class_id<PlayLayer> = 1;
	template <>
	inline const int class_id<PlayerObject> = 2;
	template <>
	inline const int class_id<GJGameLevel> = 3;

	const ClassInfo& class_at(int id);

	void open(lua_State* lua);
	void expire();
	void changed_game();
	void push_object(lua_State* lua, void* pointer, int id);
	void* to_object(lua_State* lua, int index, int id);

	cocos2d::CCPoint read_point(lua_State* lua, int index);
	cocos2d::ccColor3B read_color(lua_State* lua, int index);
	void push_point(lua_State* lua, const cocos2d::CCPoint& point);
	void push_color(lua_State* lua, const cocos2d::ccColor3B& color);

	template <class T>
	T read(lua_State* lua, int index) {
		if constexpr (std::is_same_v<T, bool>) return lua_toboolean(lua, index) != 0;
		else if constexpr (std::is_enum_v<T>) return static_cast<T>(luaL_checkinteger(lua, index));
		else if constexpr (std::is_integral_v<T>) return static_cast<T>(luaL_checkinteger(lua, index));
		else if constexpr (std::is_floating_point_v<T>) return static_cast<T>(luaL_checknumber(lua, index));
		else if constexpr (std::is_same_v<T, gd::string>) return gd::string(luaL_checkstring(lua, index));
		else if constexpr (std::is_pointer_v<T>) return static_cast<T>(to_object(lua, index, class_id<std::remove_pointer_t<T>>));
		else if constexpr (std::is_same_v<T, cocos2d::CCPoint>) return read_point(lua, index);
		else return read_color(lua, index);
	}

	template <class T>
	void push(lua_State* lua, const T& value) {
		if constexpr (std::is_same_v<T, bool>) lua_pushboolean(lua, value);
		else if constexpr (std::is_enum_v<T> || std::is_integral_v<T>) lua_pushinteger(lua, (lua_Integer)value);
		else if constexpr (std::is_floating_point_v<T>) lua_pushnumber(lua, (lua_Number)value);
		else if constexpr (std::is_same_v<T, gd::string>) lua_pushlstring(lua, value.c_str(), value.size());
		else if constexpr (std::is_pointer_v<T>) push_object(lua, value, class_id<std::remove_pointer_t<T>>);
		else if constexpr (std::is_same_v<T, cocos2d::CCPoint>) push_point(lua, value);
		else push_color(lua, value);
	}

	template <class>
	struct MemberOf;

	template <class C, class T>
	struct MemberOf<T C::*> {
		using Type = T;
	};

	template <class Owner, auto Member>
	int field(lua_State* lua, void* self, int value_index) {
		auto* object = static_cast<Owner*>(self);
		if (value_index == 0) {
			push(lua, object->*Member);
			return 1;
		}
		object->*Member = read<typename MemberOf<decltype(Member)>::Type>(lua, value_index);
		return 0;
	}

	template <class Owner, auto Member>
	int seed_field(lua_State* lua, void* self, int value_index) {
		if (value_index != 0) return luaL_error(lua, "this field can't be changed");
		lua_pushinteger(lua, (static_cast<Owner*>(self)->*Member).value());
		return 1;
	}

	template <class>
	struct FunctionOf;

	template <class C, class R, class... A>
	struct FunctionOf<R (C::*)(A...)> {
		using Owner = C;
		using Result = R;
		using Args = std::tuple<A...>;
	};

	template <auto Function, class Owner, class Result, class... Args, size_t... I>
	int call(lua_State* lua, std::tuple<Args...>*, std::index_sequence<I...>) {
		std::tuple<std::remove_cvref_t<Args>...> values{ read<std::remove_cvref_t<Args>>(lua, 2 + (int)I)... };
		auto* self = static_cast<Owner*>(to_object(lua, 1, class_id<Owner>));
		if (!self) return luaL_error(lua, "can't call a function on nil");
		changed_game();
		if constexpr (std::is_void_v<Result>) {
			(self->*Function)(std::get<I>(values)...);
			return 0;
		}
		else {
			push(lua, (self->*Function)(std::get<I>(values)...));
			return 1;
		}
	}

	template <auto Function>
	int method(lua_State* lua) {
		using Info = FunctionOf<decltype(Function)>;
		using Args = typename Info::Args;
		return call<Function, typename Info::Owner, typename Info::Result>(lua, (Args*)nullptr, std::make_index_sequence<std::tuple_size_v<Args>>());
	}
}
