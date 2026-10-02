// keeps the music in step w/ the level, not only for renders
// w/ sync audio on the song follows the levels position. when the level runs slower than real time,
//   each frame plays exactly the slice of song the level moved thru
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
    auto& logic = Logic::get();
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system)
        return;
    // song at 1x & the level at another speed cant be lined up
    bool apart = !logic.audio_speedhack && std::fabs(logic.speedhack - 1.f) > 0.001f;
    if (!logic.sync_audio || logic.render_active() || apart) {
        release_song(engine);
        return;
    }
    int threshold = std::clamp(logic.sync_audio_threshold, 20, 2000);

    GJBaseGameLayer* layer = nullptr;
    bool paused = false;
    bool started = false;
    bool dead = false;
    if (auto* play = PlayLayer::get()) {
        layer = play;
        paused = play->m_isPaused;
        started = play->m_started;
        dead = play->m_playerDied;
    }
    else if (logic.editor_testing) {
        if (auto* editor = LevelEditorLayer::get()) {
            layer = editor;
            paused = logic.editor_paused;
            started = true;
        }
    }
    if (!layer) {
        release_song(engine);
        return;
    }

    static bool anchored = false;
    static double anchor_lvl = 0.0;
    static unsigned anchor_music = 0;
    static double last_lvl = 0.0;
    static bool frame_held = false;
    static std::unordered_map<FMOD::Channel*, SfxMark> marks;

    auto groups = [&](auto&& use) {
        use(engine->m_backgroundMusicChannel);
        use(engine->m_globalChannel);
        use(engine->m_reverbChannel);
    };
    if (!logic.frame_advance) {
        if (frame_held) {
            groups([](FMOD::ChannelGroup* group) {
                release_group(group);
            });
            frame_held = false;
        }
    }

    double level = layer->m_gameState.m_levelTime;
    if (logic.frame_advance && started && !paused && !dead) {
        release_song(engine);
        frame_held = true;
        double stepped = level - last_lvl;
        if (stepped <= 0.0005) {
            if (!slice_still_running(engine->m_backgroundMusicChannel))
                groups([](FMOD::ChannelGroup* group) {
                    pause_group(group);
                });
            return;
        }
        last_lvl = level;
        if (!anchored) {
            anchored = true;
            anchor_lvl = level - stepped;
            anchor_music = engine->getMusicTimeMS(0);
        }
        double start_d = (double)(anchor_music) + (level - stepped - anchor_lvl) * 1000.0;
        if (start_d < 0.0)
            start_d = 0.0;
        engine->setMusicTimeMS(static_cast<unsigned>(std::llround(start_d)), false, 0);
        int rate = 44100;
        engine->m_system->getSoftwareFormat(&rate, nullptr, nullptr);
        unsigned long long samples = std::max(1ull, static_cast<unsigned long long>(std::llround(stepped * (double)(std::max(rate, 1)))));
        groups([&](FMOD::ChannelGroup* group) {
            play_group_slice(group, samples);
        });
        return;
    }

    if (!started || paused || dead || level + 0.0005 < last_lvl) {
        if (!started || dead || level + 0.0005 < last_lvl) {
            anchored = false;
            marks.clear();
        }
        if (!started || paused || dead) {
            if (g_songSliced) {
                g_songSliced = false;
                if (engine->m_backgroundMusicChannel)
                    engine->m_backgroundMusicChannel->setDelay(0, 0, false);
            }
            return;
        }
    }

    using Clock = chrono::steady_clock;
    static Clock::time_point last_wall;
    static double rate = 1.0;
    auto now = Clock::now();
    if (!anchored) {
        anchored = true;
        anchor_lvl = level;
        anchor_music = engine->getMusicTimeMS(0);
        last_lvl = level;
        last_wall = now;
        rate = std::max((double)(logic.speedhack), 0.01);
        return;
    }
    double wall = chrono::duration<double>(now - last_wall).count();
    last_wall = now;
    double stepped = std::max(level - last_lvl, 0.0);
    if (wall > 0.0005 && wall < 1.0)
        rate += (stepped / wall - rate) * std::min(wall * 4.0, 1.0);
    last_lvl = level;

    double expected_d = (double)(anchor_music) + (level - anchor_lvl) * 1000.0;
    if (expected_d < 0.0)
        expected_d = 0.0;
    float base = std::max(logic.speedhack, 0.01f);
    auto* music = engine->m_backgroundMusicChannel;
    if (rate < base * 0.9 && music) {
        double start_d = std::max(expected_d - stepped * 1000.0, 0.0);
        unsigned actual = engine->getMusicTimeMS(0);
        if (std::abs((double)(actual) - start_d) >= (double)(threshold))
            engine->setMusicTimeMS(static_cast<unsigned>(std::llround(start_d)), false, 0);
        music->setPitch(base);
        static int slice_logs = 0;
        if (!g_songSliced && slice_logs < 5) {
            ++slice_logs;
        }
        g_songSliced = true;
        if (stepped > 0.0) {
            int output = 44100;
            engine->m_system->getSoftwareFormat(&output, nullptr, nullptr);
            auto samples = static_cast<unsigned long long>(std::llround(stepped / base * (double)(std::max(output, 1))));
            play_group_slice(music, std::max(samples, 1ull));
        }
    }
    else {
        release_song(engine);
        auto expected = static_cast<unsigned>(std::llround(expected_d));
        unsigned actual = engine->getMusicTimeMS(0);
        int drift = static_cast<int>(actual) - static_cast<int>(expected);
        if (std::abs(drift) >= threshold) {
            engine->setMusicTimeMS(expected, false, 0);
            drift = 0;
        }
        double correction = std::clamp(-(double)(drift) / 1000.0 * 0.5, -0.01, 0.01);
        if (music)
            music->setPitch(static_cast<float>(base * (1.0 + correction)));
    }

    std::unordered_set<FMOD::Channel*> seen;
    sync_effect_group(engine->m_globalChannel, level, marks, seen);
    sync_effect_group(engine->m_reverbChannel, level, marks, seen);
    std::erase_if(marks, [&](auto const& item) {
        return !seen.contains(item.first);
    });
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
