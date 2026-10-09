cmake_minimum_required(VERSION 3.21)
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build configuration" FORCE)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Echo VERSION 1.2.0)


file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER SOURCES EXCLUDE REGEX "src/eUI/")

add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_compile_definitions(${PROJECT_NAME} PRIVATE ECHO_FFMPEG_API_ENABLED=0)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
target_link_libraries(${PROJECT_NAME} imgui-cocos)

if (WIN32)
    CPMAddPackage("gh:cursey/safetyhook#2f28386")
    target_link_libraries(${PROJECT_NAME} safetyhook)
endif()

setup_geode_mod(${PROJECT_NAME})

if (APPLE AND NOT IOS)
    set_source_files_properties(src/Logic/mac_refresh_clock.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    enable_language(OBJCXX)
    target_sources(${PROJECT_NAME} PRIVATE src/Logic/mac_promotion.mm)
    set_source_files_properties(src/Logic/mac_promotion.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(${PROJECT_NAME} "-framework CoreVideo" "-framework QuartzCore" "-framework AppKit")
endif()
