#include "Logic/logic.hpp"
#include "GUI/gui.hpp"
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

matjson::Value Logic::placement_json(LabelPlacement const& placement) {
    // later
    return {};
}

LabelPlacement Logic::read_placement(matjson::Value const& json, LabelPlacement fallback) {
    // todo
    return {};
}
