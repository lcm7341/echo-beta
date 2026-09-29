// keeps the music in step w/ the level, not only for renders
#include <chrono>
#include "Logic/logic.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/ShaderLayer.hpp>
#include <Geode/fmod/fmod.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace chrono = std::chrono;

using namespace geode::prelude;

namespace {
    bool g_liveOpeningHeld = false;
    double g_liveOpeningSec = 0.0;
}

void Logic::prepare_lvl_view(PlayLayer* play) {
    // TODO
}

float Logic::render_step() {
    return 1.f / static_cast<float>(std::max(Logic::get().render_fps, 1));
}

float Logic::scroll_speed_factor() {
    // TODO
    return {};
}

float Logic::render_speed() {
    // todo
    return {};
}

float Logic::render_game_step() {
    // later
    return {};
}

bool Logic::pause_menu_open() {
    // TODO
    return {};
}

void Logic::sync_music_pitch() {
    // TODO
}

struct SfxMark {
    unsigned pos = 0;
    double level = 0.0;
};

void sync_effect_group(FMOD::ChannelGroup* group, double level, std::unordered_map<FMOD::Channel*, SfxMark>& marks, std::unordered_set<FMOD::Channel*>& seen) {
    // TODO
}

void pause_group(FMOD::ChannelGroup* group) {
    if (!group)
        return;
    group->setPaused(true);
}

void release_group(FMOD::ChannelGroup* group) {
    // TODO
}

void play_group_slice(FMOD::ChannelGroup* group, unsigned long long samples) {
    // todo
}

bool slice_still_running(FMOD::ChannelGroup* group) {
    // TODO
    return {};
}

namespace {
    bool g_songSliced = false;

    void release_song(FMODAudioEngine* engine) {
        if (!g_songSliced)
            return;
        g_songSliced = false;
        if (engine)
            release_group(engine->m_backgroundMusicChannel);
    }
}

void Logic::sync_audio_clock() {
    // later
}

void Logic::reset_opening_music_hold() {
    g_liveOpeningHeld = false;
    g_liveOpeningSec = 0.0;
}

std::string Logic::render_file_stem() const {
    // later
    return {};
}
