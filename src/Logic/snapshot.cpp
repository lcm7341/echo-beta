// recording
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
#include <Geode/utils/async.hpp>
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>

using namespace geode::prelude;

PlayerSave Logic::capture_player(PlayerObject* player) {
    // TODO
    return {};
}

void Logic::apply_player(PlayerObject* player, PlayerSave const& shot) {
    // TODO
}

LayerSnapshot Logic::capture_layer(PlayLayer* play) {
    // TODO
    return {};
}

void Logic::apply_layer(PlayLayer* play, LayerSnapshot const& shot) {
    // TODO
}
