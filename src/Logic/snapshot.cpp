// recording
//
// record_input is called from gds button handler. the input is stored for step_frame + 1 bc gd counts a step BEFORE it reads the button
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
    log::debug("capture layer");
    LayerSnapshot shot;
    auto const& state = play->m_gameState;
    shot.levelTime = state.m_levelTime;
    shot.totalTime = state.m_totalTime;
    shot.unk_time = state.m_unkDouble3;
    shot.attemptTime = play->m_attemptTime;
    shot.progress = state.m_currentProgress;
    shot.timeWarp = state.m_timeWarp;
    shot.queued_time_warp = state.m_queuedTimeWarp;
    shot.time_warp_related = state.m_timeWarpRelated;
    shot.timeMod = state.m_timeModRelated;
    shot.camera_angle = state.m_cameraAngle;
    shot.levelFlipping = state.m_levelFlipping;
    shot.camera_pos = state.m_cameraPosition;
    shot.dual = state.m_isDualMode;
    shot.streak_blend = state.m_playerStreakBlend;
    shot.gravity_related = state.m_gravityRelated;
    shot.time_mod_related = state.m_timeModRelated2;
    return shot;
}

void Logic::apply_layer(PlayLayer* play, LayerSnapshot const& shot) {
    auto& state = play->m_gameState;
    state.m_levelTime = shot.levelTime;
    state.m_totalTime = shot.totalTime;
    state.m_unkDouble3 = shot.unk_time;
    play->m_attemptTime = shot.attemptTime;
    state.m_currentProgress = shot.progress;
    state.m_timeWarp = shot.timeWarp;
    state.m_queuedTimeWarp = shot.queued_time_warp;
    state.m_timeWarpRelated = shot.time_warp_related;
    state.m_timeModRelated = shot.timeMod;
    state.m_cameraAngle = shot.camera_angle;
    state.m_levelFlipping = shot.levelFlipping;
    state.m_cameraPosition = shot.camera_pos;
    state.m_isDualMode = shot.dual;
    state.m_playerStreakBlend = shot.streak_blend;
    state.m_gravityRelated = shot.gravity_related;
    state.m_timeModRelated2 = shot.time_mod_related;
}
