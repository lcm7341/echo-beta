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

static bool g_opening_music_held = false;
static double g_opening_music_seconds = 0.0;

void Logic::prepare_lvl_view(PlayLayer* play) {
    if (!play || !play->m_shaderLayer || render_holds_view()) return;
    // DONT use win size here shaders go pixelated
    play->m_shaderLayer->performCalculations();
}

float Logic::render_step() {
    return 1.f / (float)std::max(render_fps, 1);
}

float Logic::scroll_speed_factor() {
    auto* layer = GJBaseGameLayer::get();
    auto* player = layer ? layer->m_player1 : nullptr;
    if (!player) return 1.f;
    static const float speeds[5] = { 251.1601f, 311.5801f, 387.4201f, 468.0002f, 576.0002f };
    float speed = player->m_playerSpeed;
    int speed_index = 1;
    if (speed == 0.7f) speed_index = 0;
    else if (speed == 1.1f) speed_index = 2;
    else if (speed == 1.3f) speed_index = 3;
    else if (speed == 1.6f) speed_index = 4;
    float step = speeds[speed_index] / (float)sample_rate();
    float moved = (player->getPositionX() + step) - player->getPositionX();
    float factor = step > 0.f ? moved / step : 1.f;
    if (!(factor > 0.f) || layer->m_levelEndAnimationStarted) return 1.f;
    return factor;
}

float Logic::render_speed() {
    if (render_speed_locked > 0.f) return render_speed_locked;
    if (!render_speedhack) return 1.f;
    return speedhack;
}

float Logic::render_game_step() {
    float dt = render_step() * render_speed();
    if (render_ssb_fix) {
        float factor = scroll_speed_factor();
        auto& logic = Logic::get();
        logic.render_factor_low = std::min(logic.render_factor_low, factor);
        logic.render_factor_high = std::max(logic.render_factor_high, factor);
        logic.render_factor_sum += factor;
        ++logic.render_factor_count;
        if (factor > 0.f) dt /= factor;
    }
    return dt;
}

bool Logic::pause_menu_open() {
    if (pause_known) return pause_cached;
    auto* scene = CCDirector::sharedDirector()->getRunningScene();
    if (scene && scene->getChildByType<PauseLayer>(0)) return true;
    auto* play = PlayLayer::get();
    return play && play->getChildByType<PauseLayer>(0);
}

void Logic::sync_music_pitch() {
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine) return;
    auto* play = PlayLayer::get();
    bool rendering = render_active();
    float pitch = 1.f;
    if (play) pitch = rendering || !audio_speedhack ? 1.f : speedhack;

    if (play && !play->m_started) {
        if (!g_opening_music_held) {
            g_opening_music_held = true;
            g_opening_music_seconds = (double)(engine->getMusicTimeMS(0)) / 1000.0;
        }
        unsigned ms = (unsigned)std::llround(std::max(g_opening_music_seconds, 0.0) * 1000.0);
        unsigned now = engine->getMusicTimeMS(0);
        if ((now > ms ? now - ms : ms - now) > 2) engine->setMusicTimeMS(ms, false, 0);
        if (engine->m_backgroundMusicChannel) {
            engine->m_backgroundMusicChannel->setPitch(pitch);
            engine->m_backgroundMusicChannel->setPaused(true);
        }
        if (engine->m_globalChannel) engine->m_globalChannel->setPitch(pitch);
        return;
    }
    if (g_opening_music_held) {
        g_opening_music_held = false;
        unsigned ms = (unsigned)std::llround(std::max(g_opening_music_seconds, 0.0) * 1000.0);
        engine->setMusicTimeMS(ms, false, 0);
        if (engine->m_backgroundMusicChannel && play && !play->m_isPaused) {
            engine->m_backgroundMusicChannel->setPaused(false);
        }
        g_opening_music_seconds = 0.0;
    }

    if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPitch(pitch);
    if (engine->m_globalChannel) engine->m_globalChannel->setPitch(pitch);
    sync_audio_clock();
}

struct EffectChannelAnchor {
    unsigned position_ms = 0;
    double level_time = 0.0;
};

void sync_effect_group(FMOD::ChannelGroup* group, double level_time, std::unordered_map<FMOD::Channel*, EffectChannelAnchor>& anchors, std::unordered_set<FMOD::Channel*>& seen_channels) {
    if (!group) return;
    int channel_count = 0;
    group->getNumChannels(&channel_count);
    for (int i = 0; i < channel_count; ++i) {
        FMOD::Channel* channel = nullptr;
        if (group->getChannel(i, &channel) != FMOD_OK || !channel) continue;
        bool playing = false;
        if (channel->isPlaying(&playing) != FMOD_OK || !playing) continue;
        unsigned position_ms = 0;
        if (channel->getPosition(&position_ms, FMOD_TIMEUNIT_MS) != FMOD_OK) continue;
        seen_channels.insert(channel);
        auto it = anchors.find(channel);
        bool restarted = it == anchors.end() || level_time + 0.0005 < it->second.level_time || position_ms + 80 < it->second.position_ms;
        if (restarted) {
            anchors[channel] = { position_ms, level_time };
            continue;
        }
        double exact_expected_ms = (double)(it->second.position_ms) + (level_time - it->second.level_time) * 1000.0;
        if (exact_expected_ms < 0.0) exact_expected_ms = 0.0;
        auto expected_ms = (unsigned)std::llround(exact_expected_ms);
        int drift = (int)position_ms - (int)expected_ms;
        if (std::abs(drift) < 250) continue;
        if (channel->setPosition(expected_ms, FMOD_TIMEUNIT_MS) == FMOD_OK) it->second.position_ms = expected_ms;
    }
    int group_count = 0;
    group->getNumGroups(&group_count);
    for (int i = 0; i < group_count; ++i) {
        FMOD::ChannelGroup* child = nullptr;
        if (group->getGroup(i, &child) == FMOD_OK) sync_effect_group(child, level_time, anchors, seen_channels);
    }
}

void pause_group(FMOD::ChannelGroup* group) {
    if (!group) return;
    group->setPaused(true);
}

void resume_group(FMOD::ChannelGroup* group) {
    if (!group) return;
    group->setDelay(0, 0, false);
    group->setPaused(false);
}

void play_group_slice(FMOD::ChannelGroup* group, unsigned long long samples) {
    if (!group || samples == 0) return;
    unsigned long long clock = 0;
    if (group->getDSPClock(nullptr, &clock) != FMOD_OK) return;
    unsigned long long start = 0, end = 0;
    bool stop = false;
    group->getDelay(&start, &end, &stop);
    unsigned long long from = end > clock ? end : clock;
    group->setPaused(false);
    group->setDelay(0, from + samples, false);
}

bool slice_still_running(FMOD::ChannelGroup* group) {
    if (!group) return false;
    unsigned long long clock = 0;
    unsigned long long start = 0, end = 0;
    bool stop = false;
    if (group->getDSPClock(nullptr, &clock) != FMOD_OK) return false;
    if (group->getDelay(&start, &end, &stop) != FMOD_OK) return false;
    return end > clock;
}

static bool g_song_slicing = false;

static void stop_song_slicing(FMODAudioEngine* engine) {
    if (!g_song_slicing) return;
    g_song_slicing = false;
    if (engine) resume_group(engine->m_backgroundMusicChannel);
}

void Logic::sync_audio_clock() {
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;
    bool music_speed_differs = !audio_speedhack && std::fabs(speedhack - 1.f) > 0.001f;
    if (!sync_audio || render_active() || music_speed_differs) {
        stop_song_slicing(engine);
        return;
    }
    int threshold = std::clamp(sync_audio_threshold, 20, 2000);

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
    else if (editor_testing) {
        if (auto* editor = LevelEditorLayer::get()) {
            layer = editor;
            paused = editor_paused;
            started = true;
        }
    }
    if (!layer) {
        stop_song_slicing(engine);
        return;
    }

    static bool anchor_set = false;
    static double anchor_level_time = 0.0;
    static unsigned anchor_music_ms = 0;
    static double last_level_time = 0.0;
    static bool frame_advance_holding = false;
    static std::unordered_map<FMOD::Channel*, EffectChannelAnchor> effect_anchors;

    auto for_each_group = [&](auto&& use) {
        use(engine->m_backgroundMusicChannel);
        use(engine->m_globalChannel);
        use(engine->m_reverbChannel);
    };
    if (!frame_advance) {
        if (frame_advance_holding) {
            for_each_group([](FMOD::ChannelGroup* group) {
                resume_group(group);
            });
            frame_advance_holding = false;
        }
    }

    double level_time = layer->m_gameState.m_levelTime;
    if (frame_advance && started && !paused && !dead) {
        stop_song_slicing(engine);
        frame_advance_holding = true;
        double level_step = level_time - last_level_time;
        if (level_step <= 0.0005) {
            if (!slice_still_running(engine->m_backgroundMusicChannel))
                for_each_group([](FMOD::ChannelGroup* group) {
                    pause_group(group);
                });
            return;
        }
        last_level_time = level_time;
        if (!anchor_set) {
            anchor_set = true;
            anchor_level_time = level_time - level_step;
            anchor_music_ms = engine->getMusicTimeMS(0);
        }
        double start_ms = (double)(anchor_music_ms) + (level_time - level_step - anchor_level_time) * 1000.0;
        if (start_ms < 0.0) start_ms = 0.0;
        engine->setMusicTimeMS((unsigned)std::llround(start_ms), false, 0);
        int output_rate = 44100;
        engine->m_system->getSoftwareFormat(&output_rate, nullptr, nullptr);
        unsigned long long samples = std::max(1ull, (unsigned long long)std::llround(level_step * (double)(std::max(output_rate, 1))));
        for_each_group([&](FMOD::ChannelGroup* group) {
            play_group_slice(group, samples);
        });
        return;
    }

    if (!started || paused || dead || level_time + 0.0005 < last_level_time) {
        if (!started || dead || level_time + 0.0005 < last_level_time) {
            anchor_set = false;
            effect_anchors.clear();
        }
        if (!started || paused || dead) {
            if (g_song_slicing) {
                g_song_slicing = false;
                if (engine->m_backgroundMusicChannel)
                    engine->m_backgroundMusicChannel->setDelay(0, 0, false);
            }
            return;
        }
    }

    using Clock = chrono::steady_clock;
    static Clock::time_point last_wall_time;
    static double measured_speed = 1.0;
    auto now = Clock::now();
    if (!anchor_set) {
        anchor_set = true;
        anchor_level_time = level_time;
        anchor_music_ms = engine->getMusicTimeMS(0);
        last_level_time = level_time;
        last_wall_time = now;
        measured_speed = speedhack;
        return;
    }
    double wall_seconds = chrono::duration<double>(now - last_wall_time).count();
    last_wall_time = now;
    double level_step = std::max(level_time - last_level_time, 0.0);
    if (wall_seconds > 0.0005 && wall_seconds < 1.0) measured_speed += (level_step / wall_seconds - measured_speed) * std::min(wall_seconds * 4.0, 1.0);
    last_level_time = level_time;

    double exact_expected_ms = (double)(anchor_music_ms) + (level_time - anchor_level_time) * 1000.0;
    if (exact_expected_ms < 0.0) exact_expected_ms = 0.0;
    float base_pitch = speedhack;
    auto* music = engine->m_backgroundMusicChannel;
    if (measured_speed < base_pitch * 0.9 && music) {
        double start_ms = std::max(exact_expected_ms - level_step * 1000.0, 0.0);
        unsigned actual_ms = engine->getMusicTimeMS(0);
        if (std::abs((double)(actual_ms) - start_ms) >= (double)(threshold))
            engine->setMusicTimeMS((unsigned)std::llround(start_ms), false, 0);
        music->setPitch(base_pitch);
        g_song_slicing = true;
        if (level_step > 0.0) {
            int output_rate = 44100;
            engine->m_system->getSoftwareFormat(&output_rate, nullptr, nullptr);
            auto samples = (unsigned long long)std::llround(level_step / base_pitch * (double)(std::max(output_rate, 1)));
            play_group_slice(music, std::max(samples, 1ull));
        }
    }
    else {
        stop_song_slicing(engine);
        auto expected_ms = (unsigned)std::llround(exact_expected_ms);
        unsigned actual_ms = engine->getMusicTimeMS(0);
        int drift = (int)actual_ms - (int)expected_ms;
        if (std::abs(drift) >= threshold) {
            engine->setMusicTimeMS(expected_ms, false, 0);
            drift = 0;
        }
        double correction = std::clamp(-(double)(drift) / 1000.0 * 0.5, -0.01, 0.01);
        if (music) music->setPitch((float)(base_pitch * (1.0 + correction)));
    }

    std::unordered_set<FMOD::Channel*> seen_channels;
    sync_effect_group(engine->m_globalChannel, level_time, effect_anchors, seen_channels);
    sync_effect_group(engine->m_reverbChannel, level_time, effect_anchors, seen_channels);
    std::erase_if(effect_anchors, [&](const auto& item) {
        return !seen_channels.contains(item.first);
    });
}

void Logic::reset_opening_music_hold() {
    g_opening_music_held = false;
    g_opening_music_seconds = 0.0;
}

std::string Logic::render_file_stem() const {
    if (!render_name.empty() && render_name != "output") return render_name;
    if (!macro_name.empty()) return macro_name;
    return "Replay";
}
