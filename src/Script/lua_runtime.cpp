#include "Script/lua_runtime.hpp"

#include <Geode/Geode.hpp>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

using namespace geode::prelude;

#define INSTRUCTION_CHECK_STEP 1000
#define FREEZE_SECONDS 3
#define FREEZE_CHECK_MILLISECONDS 500

extern void (*lua_pattern_work)(lua_State* L, size_t amount);

namespace Script {
    struct FreezeWatch {
        std::mutex lock;
        std::filesystem::path marker;
        std::string note;
        const Runtime* owner = nullptr;
    };

    static std::atomic<long long> g_call_started{ 0 };
    static std::atomic<bool> g_froze{ false };
    static int g_call_depth = 0;

    static FreezeWatch& freeze_watch() {
        static auto* watch = new FreezeWatch;
        return *watch;
    }

    static long long clock_now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    static void watch_for_freezes() {
        const long long freeze_after = FREEZE_SECONDS * 1000ll;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(FREEZE_CHECK_MILLISECONDS));
            long long started = g_call_started.load();
            if (started == 0 || g_froze.load() || clock_now() - started < freeze_after) continue;
            auto& watch = freeze_watch();
            std::lock_guard lock(watch.lock);
            if (watch.marker.empty()) continue;
            std::ofstream file(watch.marker, std::ios::binary);
            file << watch.note;
            g_froze = true;
        }
    }

    struct CallTimer {
        explicit CallTimer(const Runtime& runtime) {
            if (g_call_depth++ > 0 || runtime.freeze_marker.empty()) return;
            auto& watch = freeze_watch();
            if (watch.owner != &runtime) {
                std::lock_guard lock(watch.lock);
                watch.marker = runtime.freeze_marker;
                watch.note = runtime.freeze_note;
                watch.owner = &runtime;
            }
            g_call_started = clock_now();
        }

        ~CallTimer() {
            if (--g_call_depth > 0) return;
            g_call_started = 0;
            if (!g_froze.exchange(false)) return;
            auto& watch = freeze_watch();
            std::lock_guard lock(watch.lock);
            std::error_code error;
            std::filesystem::remove(watch.marker, error);
        }
    };

    static Runtime* owner(lua_State* lua) {
        void* runtime = nullptr;
        lua_getallocf(lua, &runtime);
        return static_cast<Runtime*>(runtime);
    }

    static void count_pattern_work(lua_State* lua, size_t amount) {
        owner(lua)->charge(lua, (long long)amount);
    }

    static void* allocate(void* runtime_pointer, void* block, size_t old_size, size_t new_size) {
        auto* runtime = static_cast<Runtime*>(runtime_pointer);
        if (!block) old_size = 0;
        if (new_size == 0) {
            std::free(block);
            runtime->memory_used -= old_size;
            return nullptr;
        }
        if (new_size > old_size && runtime->memory_used + (new_size - old_size) > runtime->memory_limit)
            return nullptr;
        void* moved = std::realloc(block, new_size);
        if (!moved) return nullptr;
        runtime->memory_used += new_size;
        runtime->memory_used -= old_size;
        return moved;
    }

    static void count_instructions(lua_State* lua, lua_Debug*) {
        owner(lua)->charge(lua, INSTRUCTION_CHECK_STEP);
    }

    static int print(lua_State* lua) {
        auto* runtime = owner(lua);
        std::string line;
        int count = lua_gettop(lua);
        for (int i = 1; i <= count; ++i) {
            if (i > 1) line += '\t';
            size_t length = 0;
            const char* text = luaL_tolstring(lua, i, &length);
            line.append(text, length);
            lua_pop(lua, 1);
        }
        log::info("[lua] {}", line);
        if (runtime->on_print) runtime->on_print(line);
        return 0;
    }

    static int traceback(lua_State* lua) {
        const char* message = lua_tostring(lua, 1);
        if (!message) message = luaL_tolstring(lua, 1, nullptr);
        luaL_traceback(lua, lua, message, 1);
        return 1;
    }

    static int on_panic(lua_State* lua) {
        const char* message = lua_tostring(lua, -1);
        log::error("[lua] unprotected error: {}", message ? message : "unknown");
        return 0;
    }

    Runtime::Runtime(unsigned long long limit) : memory_limit(limit) {
        lua_pattern_work = count_pattern_work;
        lua = lua_newstate(allocate, this);
        if (!lua) return;
        lua_atpanic(lua, on_panic);
        const luaL_Reg libraries[] = {
            { LUA_GNAME, luaopen_base },
            { LUA_TABLIBNAME, luaopen_table },
            { LUA_STRLIBNAME, luaopen_string },
            { LUA_MATHLIBNAME, luaopen_math },
            { LUA_UTF8LIBNAME, luaopen_utf8 },
            { LUA_COLIBNAME, luaopen_coroutine },
        };
        for (const auto& library : libraries) {
            luaL_requiref(lua, library.name, library.func, 1);
            lua_pop(lua, 1);
        }
        for (const char* unsafe : { "dofile", "loadfile", "load", "collectgarbage" }) {
            lua_pushnil(lua);
            lua_setglobal(lua, unsafe);
        }
        lua_register(lua, "print", print);
    }

    Runtime::~Runtime() {
        if (lua) lua_close(lua);
        auto& watch = freeze_watch();
        if (watch.owner != this) return;
        std::lock_guard lock(watch.lock);
        watch.marker.clear();
        watch.owner = nullptr;
    }

    Runtime* Runtime::from(lua_State* lua) {
        return owner(lua);
    }

    lua_State* Runtime::state() const {
        return lua;
    }

    bool Runtime::run_file(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            last_error = "Could not open " + geode::utils::string::pathToString(path.filename());
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return run_text(text, geode::utils::string::pathToString(path.filename()));
    }

    bool Runtime::run_text(std::string_view text, const std::string& name) {
        if (!lua) {
            last_error = "Lua could not start";
            return false;
        }
        std::string chunk = "@" + name;
        if (luaL_loadbufferx(lua, text.data(), text.size(), chunk.c_str(), "t") != LUA_OK) {
            const char* message = lua_tostring(lua, -1);
            last_error = message ? message : "Lua could not read the script";
            lua_pop(lua, 1);
            return false;
        }
        return call(0, 0);
    }

    bool Runtime::call(int args, int results) {
        int handler = lua_gettop(lua) - args;
        lua_pushcfunction(lua, traceback);
        lua_insert(lua, handler);
        CallTimer timer(*this);
        int status = lua_pcall(lua, args, results, handler);
        lua_remove(lua, handler);
        if (status == LUA_OK) return true;
        size_t length = 0;
        const char* message = lua_tolstring(lua, -1, &length);
        last_error = message ? std::string(message, length) : "Lua error";
        lua_pop(lua, 1);
        return false;
    }

    void Runtime::limit_instructions(long long instructions) {
        instructions_left = instructions;
        if (counting || !lua) return;
        counting = true;
        lua_sethook(lua, count_instructions, LUA_MASKCOUNT, INSTRUCTION_CHECK_STEP);
    }

    void Runtime::charge(lua_State* running, long long work) {
        if (!counting) return;
        instructions_left -= work;
        if (instructions_left < 0) luaL_error(running, "script ran too long");
    }

    void Runtime::guard_freezes(const std::filesystem::path& marker, const std::string& note) {
        static std::once_flag watching;
        freeze_marker = marker;
        freeze_note = note;
        std::call_once(watching, [] {
            std::thread(watch_for_freezes).detach();
        });
    }

    bool froze_last_time(const std::filesystem::path& marker, std::string& note) {
        std::error_code error;
        if (!std::filesystem::exists(marker, error)) return false;
        std::ifstream file(marker, std::ios::binary);
        note.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        std::filesystem::remove(marker, error);
        return true;
    }

    const std::string& Runtime::error() const {
        return last_error;
    }
}
