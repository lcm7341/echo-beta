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
    matjson::Value json;
    json["x"] = placement.x;
    json["y"] = placement.y;
    json["scale"] = placement.scale;
    json["opacity"] = placement.opacity;
    return json;
}

LabelPlacement Logic::read_placement(matjson::Value const& json, LabelPlacement fallback) {
    if (!json.isObject())
        return fallback;
    if (auto number = json["x"].asDouble())
        fallback.x = static_cast<float>(number.unwrap());
    if (auto number = json["y"].asDouble())
        fallback.y = static_cast<float>(number.unwrap());
    if (auto number = json["scale"].asDouble())
        fallback.scale = static_cast<float>(number.unwrap());
    if (auto number = json["opacity"].asInt())
        fallback.opacity = static_cast<int>(number.unwrap());
    return fallback;
}
