// keeps the music in step w/ the level, not only for renders
// w/ sync audio on the song follows the levels position. when the level runs slower than real time,
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
    if (!play || !play->m_shaderLayer || Logic::get().render_holds_view())
        return;
    // DONT use win size here shaders go pixelated
    play->m_shaderLayer->performCalculations();
}

float Logic::render_step() {
    return 1.f / static_cast<float>(std::max(Logic::get().render_fps, 1));
}

float Logic::scroll_speed_factor() {
    auto* layer = GJBaseGameLayer::get();
    auto* player = layer ? layer->m_player1 : nullptr;
    if (!player)
        return 1.f;
    static float const speeds[5] = { 251.1601f, 311.5801f, 387.4201f, 468.0002f, 576.0002f };
    float speed = player->m_playerSpeed;
    int idx = 1;
    if (speed == 0.7f)
        idx = 0;
    else if (speed == 1.1f)
        idx = 2;
    else if (speed == 1.3f)
        idx = 3;
    else if (speed == 1.6f)
        idx = 4;
    float step = speeds[idx] / static_cast<float>(std::max(Logic::get().sample_rate(), 1.0));
    float moved = (player->getPositionX() + step) - player->getPositionX();
    float factor = step > 0.f ? moved / step : 1.f;
    if (!(factor > 0.f) || layer->m_levelEndAnimationStarted)
        return 1.f;
    return factor;
}

float Logic::render_speed() {
    // todo
    return {};
}

float Logic::render_game_step() {
    float dt = Logic::get().render_step() * Logic::get().render_speed();
    if (Logic::get().render_ssb_fix) {
        float factor = Logic::get().scroll_speed_factor();
        if (factor > 0.f)
            dt /= factor;
    }
    return dt;
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
    if (!group)
        return;
    int amt = 0;
    group->getNumChannels(&amt);
    for (int i = 0; i < amt; ++i) {
        FMOD::Channel* channel = nullptr;
        if (group->getChannel(i, &channel) != FMOD_OK || !channel)
            continue;
        bool playing = false;
        if (channel->isPlaying(&playing) != FMOD_OK || !playing)
            continue;
        unsigned pos = 0;
        if (channel->getPosition(&pos, FMOD_TIMEUNIT_MS) != FMOD_OK)
            continue;
        seen.insert(channel);
        auto it = marks.find(channel);
        bool fresh = it == marks.end() || level + 0.0005 < it->second.level || pos + 80 < it->second.pos;
        if (fresh) {
            marks[channel] = { pos, level };
            continue;
        }
        double expected_d = (double)(it->second.pos) + (level - it->second.level) * 1000.0;
        if (expected_d < 0.0)
            expected_d = 0.0;
        auto expected = static_cast<unsigned>(std::llround(expected_d));
        int drift = static_cast<int>(pos) - static_cast<int>(expected);
        if (std::abs(drift) < 250)
            continue;
        if (channel->setPosition(expected, FMOD_TIMEUNIT_MS) == FMOD_OK)
            it->second.pos = expected;
    }
    int groups = 0;
    group->getNumGroups(&groups);
    for (int i = 0; i < groups; ++i) {
        FMOD::ChannelGroup* child = nullptr;
        if (group->getGroup(i, &child) == FMOD_OK)
            sync_effect_group(child, level, marks, seen);
    }
}

void pause_group(FMOD::ChannelGroup* group) {
    if (!group)
        return;
    group->setPaused(true);
}

void release_group(FMOD::ChannelGroup* group) {
    if (!group)
        return;
    group->setDelay(0, 0, false);
    group->setPaused(false);
}

void play_group_slice(FMOD::ChannelGroup* group, unsigned long long samples) {
    if (!group || samples == 0)
        return;
    unsigned long long clock = 0;
    if (group->getDSPClock(nullptr, &clock) != FMOD_OK)
        return;
    unsigned long long start = 0, end = 0;
    bool stop = false;
    group->getDelay(&start, &end, &stop);
    unsigned long long from = end > clock ? end : clock;
    group->setPaused(false);
    group->setDelay(0, from + samples, false);
}

bool slice_still_running(FMOD::ChannelGroup* group) {
    if (!group)
        return false;
    unsigned long long clock = 0;
    unsigned long long start = 0, end = 0;
    bool stop = false;
    if (group->getDSPClock(nullptr, &clock) != FMOD_OK)
        return false;
    if (group->getDelay(&start, &end, &stop) != FMOD_OK)
        return false;
    return end > clock;
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
    if (!render_name.empty() && render_name != "output")
        return render_name;
    if (!macro_name.empty())
        return macro_name;
    return "Replay";
}
