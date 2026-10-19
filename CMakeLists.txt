cmake_minimum_required(VERSION 3.21)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build configuration" FORCE)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Echo VERSION 2.2.1)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
# these only build on their own os (added further down)
list(FILTER SOURCES EXCLUDE REGEX "src/Logic/(mac|win)_refresh_clock\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX "src/Physics/android_sort\\.cpp$")

add_library(${PROJECT_NAME} SHARED ${SOURCES})
if (ANDROID)
    target_compile_definitions(${PROJECT_NAME} PRIVATE ECHO_FFMPEG_API_ENABLED=1)
    if (ANDROID_ABI STREQUAL "arm64-v8a")
        target_sources(${PROJECT_NAME} PRIVATE src/Physics/android_sort.cpp)
    endif()
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE ECHO_FFMPEG_API_ENABLED=0)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(ECHO_BUILD_INFO "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.cpp")
add_custom_target(echo_build_info ALL
    COMMAND ${CMAKE_COMMAND} -E env TZ=America/Chicago ${CMAKE_COMMAND} -DOUT=${ECHO_BUILD_INFO} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${ECHO_BUILD_INFO}
    VERBATIM)
add_dependencies(${PROJECT_NAME} echo_build_info)
target_sources(${PROJECT_NAME} PRIVATE ${ECHO_BUILD_INFO})

if (NOT DEFINED ENV{GEODE_SDK} OR NOT EXISTS "$ENV{GEODE_SDK}/CMakeLists.txt")
    foreach(_geode_candidate "$ENV{HOME}/Documents/Geode" "$ENV{HOME}/.local/share/Geode/sdk")
        if (EXISTS "${_geode_candidate}/CMakeLists.txt")
            set(ENV{GEODE_SDK} "${_geode_candidate}")
            break()
        endif()
    endforeach()
endif()

if (NOT DEFINED ENV{GEODE_SDK} OR NOT EXISTS "$ENV{GEODE_SDK}/CMakeLists.txt")
    message(FATAL_ERROR "Unable to find Geode SDK! Install it with `geode sdk install` or `geode sdk set-path`.")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

set(IMGUI_VERSION "v1.92.4")
CPMAddPackage("gh:matcool/gd-imgui-cocos#b93f08ccef778a53ebba09b20c347f6a63980119")
if (IOS)
    set(_echo_backend_hooks "${CPM_PACKAGE_gd-imgui-cocos_SOURCE_DIR}/src/hooks.cpp")
    file(READ "${_echo_backend_hooks}" _echo_backend_text)
    string(REPLACE "#include <Geode/modify/CCKeyboardDispatcher.hpp>\n\n#include <imgui.h>" "#ifndef GEODE_IS_IOS\n#include <Geode/modify/CCKeyboardDispatcher.hpp>\n#endif\n\n#include <imgui.h>" _echo_backend_patched "${_echo_backend_text}")
    if (NOT _echo_backend_patched STREQUAL _echo_backend_text)
        file(WRITE "${_echo_backend_hooks}" "${_echo_backend_patched}")
    endif()
endif()
target_link_libraries(${PROJECT_NAME} imgui-cocos)

CPMAddPackage(NAME lua VERSION 5.4.7 URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30 DOWNLOAD_ONLY YES)
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|linit|liolib|loslib|loadlib|ldblib|lstrlib)\\.c$")
file(READ ${lua_SOURCE_DIR}/src/lstrlib.c LUA_STRLIB)
function(patch_lua_strlib FROM TO)
    string(FIND "${LUA_STRLIB}" "${FROM}" LUA_PATCH_AT)
    if(LUA_PATCH_AT EQUAL -1)
        message(FATAL_ERROR "Could not patch Lua's lstrlib.c: ${FROM}")
    endif()
    string(REPLACE "${FROM}" "${TO}" PATCHED "${LUA_STRLIB}")
    set(LUA_STRLIB "${PATCHED}" PARENT_SCOPE)
endfunction()
patch_lua_strlib([=[#include "lualib.h"
]=] [=[#include "lualib.h"

void (*lua_pattern_work)(lua_State *L, size_t amount) = NULL;
#define PATTERN_WORK(L, amount) if (lua_pattern_work) lua_pattern_work(L, amount)
]=])
patch_lua_strlib([=[    while (++s < ms->src_end) {
]=] [=[    while (++s < ms->src_end) {
      PATTERN_WORK(ms->L, 1);
]=])
patch_lua_strlib([=[  while (singlematch(ms, s + i, p, ep))
    i++;
]=] [=[  while (singlematch(ms, s + i, p, ep)) {
    PATTERN_WORK(ms->L, 1);
    i++;
  }
]=])
patch_lua_strlib([=[  len = ms->capture[l].len;
]=] [=[  len = ms->capture[l].len;
  PATTERN_WORK(ms->L, len + 1);
]=])
patch_lua_strlib([=[  init: /* using goto to optimize tail recursion */
]=] [=[  init: /* using goto to optimize tail recursion */
  PATTERN_WORK(ms->L, 1);
]=])
patch_lua_strlib([=[static const char *lmemfind (const char *s1, size_t l1,
]=] [=[static const char *lmemfind (lua_State *L, const char *s1, size_t l1,
]=])
patch_lua_strlib([=[      init++;   /* 1st char is already checked */
]=] [=[      init++;   /* 1st char is already checked */
      PATTERN_WORK(L, l2 + 1);
]=])
patch_lua_strlib([=[lmemfind(s + init, ls - init, p, lp)]=] [=[lmemfind(L, s + init, ls - init, p, lp)]=])
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/lua-patched/lstrlib.c "${LUA_STRLIB}")
list(APPEND LUA_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/lua-patched/lstrlib.c)
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(echo_lua STATIC ${LUA_SOURCES})
set_target_properties(echo_lua PROPERTIES POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE CXX)
target_include_directories(echo_lua PUBLIC ${lua_SOURCE_DIR}/src)
target_link_libraries(${PROJECT_NAME} echo_lua)

if (WIN32)
    CPMAddPackage("gh:cursey/safetyhook#2f28386")
    target_link_libraries(${PROJECT_NAME} safetyhook)
    target_sources(${PROJECT_NAME} PRIVATE src/Logic/win_refresh_clock.cpp)
    target_link_libraries(${PROJECT_NAME} dwmapi)
endif()

setup_geode_mod(${PROJECT_NAME})

if (IOS)
    enable_language(OBJCXX)
    target_sources(${PROJECT_NAME} PRIVATE src/Recorder/ios_encoder.mm)
    set_source_files_properties(src/Recorder/ios_encoder.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_FLAGS "-fobjc-arc")
    target_sources(${PROJECT_NAME} PRIVATE src/Hack/ios_screen.mm)
    set_source_files_properties(src/Hack/ios_screen.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_FLAGS "-fobjc-arc -Wno-deprecated-declarations")
    target_link_libraries(${PROJECT_NAME} "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework AudioToolbox" "-framework QuartzCore" "-framework UIKit")
endif()

if (APPLE AND NOT IOS)
    # apple calls the display link "old" now, this hides those warnings
    target_sources(${PROJECT_NAME} PRIVATE src/Logic/mac_refresh_clock.cpp)
    set_source_files_properties(src/Logic/mac_refresh_clock.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_FLAGS "-Wno-deprecated-declarations")
    enable_language(OBJCXX)
    target_sources(${PROJECT_NAME} PRIVATE src/Logic/mac_promotion.mm)
    set_source_files_properties(src/Logic/mac_promotion.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(${PROJECT_NAME} PRIVATE src/GUI/mac_scroll.mm)
    set_source_files_properties(src/GUI/mac_scroll.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(${PROJECT_NAME} "-framework CoreVideo" "-framework QuartzCore" "-framework AppKit")
endif()
