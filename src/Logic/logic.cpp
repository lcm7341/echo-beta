#include "Logic/logic.hpp"
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/ModSettingsManager.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>

namespace fs = std::filesystem;

using namespace geode::prelude;

namespace {
    bool bulk_settings = false;
}

namespace {
    thread_local Logic* g_loading = nullptr;
}

void Logic::set_loading(Logic* session) {
    g_loading = session;
}

Logic& Logic::live() {
    static Logic value;
    return value;
}

Logic& Logic::get() {
    if (g_loading)
        return *g_loading;
    return Logic::live();
}

const char* Logic::state_name(RunState state) {
    // TODO
    return {};
}

fs::path Logic::save_root() {
    return Mod::get()->getSaveDir();
}

std::string Logic::read_folder_setting(char const* key) {
    auto value = Mod::get()->getSettingValue<fs::path>(key);
    return value.string();
}

void Logic::boot() {
    // TODO
}

void Logic::pull_settings() {
    // TODO
}

void Logic::push_runtime_settings() {
    // TODO
}

void Logic::save_settings_file() {
    // port this
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
