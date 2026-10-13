// renders a macro to a video
//
// gd draws into a texture at the videos size, 1 fixed step per frame, so the video is smooth no matter how slow the pc is
// windows & mac pipe the frames to an ffmpeg process. android uses the ffmpeg api mod. ios uses apples encoder (ios_encoder)
// the window keeps its real size while gd is told it has the videos size, & the shader layer is resized to match (silicate)
// audio is tapped off gds own mixer (AudioTap): frozen between frames & moved on by exactly 1 frame of sound each time,
//   so it cant drift from the picture. it is muxed into the video in a 2nd pass
// gd streams songs off disk in pieces, too slow for a render that runs faster than real time. render_song loads the whole song
#include "GUI/gui.hpp"
#include "Recorder/stereo_audio.hpp"
#include "Logic/conversions/codecs.hpp"
#include "Logic/logic.hpp"
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/FMODMusic.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJShaderState.hpp>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/ShaderLayer.hpp>
#include <Geode/fmod/fmod.hpp>
#include <Geode/loader/Loader.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#if ECHO_FFMPEG_API_ENABLED && __has_include(<eclipse.ffmpeg-api/include/events.hpp>)
#include <eclipse.ffmpeg-api/include/events.hpp>
#define ECHO_FFMPEG_API 1
#elif defined(GEODE_IS_IOS)
#include "Recorder/ios_encoder.hpp"
#define ECHO_FFMPEG_API 1
#endif

#include <Geode/ui/OverlayManager.hpp>

namespace fs = std::filesystem;
namespace chrono = std::chrono;

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif
#ifdef GEODE_IS_ANDROID
#include <dlfcn.h>
#endif
#ifdef GEODE_IS_MACOS
#include <OpenGL/OpenGL.h>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined(GEODE_IS_ANDROID) && defined(ECHO_FFMPEG_API)
namespace {
    struct AVCodecContext;
    struct AVCodec;
    struct AVDictionary;
    using OpenCodec = int (*)(AVCodecContext*, AVCodec const*, AVDictionary**);
    using SetDictionary = int (*)(AVDictionary**, char const*, char const*, int);
    using FreeDictionary = void (*)(AVDictionary**);
    OpenCodec s_openCodec = nullptr;
    SetDictionary s_setDictionary = nullptr;
    FreeDictionary s_freeDictionary = nullptr;
    thread_local char const* s_echoPreset = nullptr;
    // api forgets a flag mp4 needs :/
    thread_local bool s_echoGlobalHeader = false;

    int echo_open_codec(AVCodecContext* context, AVCodec const* codec, AVDictionary** options) {
        AVDictionary* local = nullptr;
        AVDictionary** chosen = options ? options : &local;
        if (s_echoPreset)
            s_setDictionary(chosen, "preset", s_echoPreset, 0);
        if (s_echoGlobalHeader)
            s_setDictionary(chosen, "flags", "+global_header", 0);
        int res = s_openCodec(context, codec, chosen);
        if (local)
            s_freeDictionary(&local);
        return res;
    }

    void install_encoder_options() {
        static bool installed = false;
        if (installed)
            return;
        auto* dependency = Loader::get()->getLoadedMod("eclipse.ffmpeg-api");
        if (!dependency)
            return;
        void* binary = dlopen(dependency->getBinaryPath().c_str(), RTLD_NOW | RTLD_NOLOAD);
        if (!binary)
            return;
        s_openCodec = reinterpret_cast<OpenCodec>(dlsym(binary, "avcodec_open2"));
        s_setDictionary = reinterpret_cast<SetDictionary>(dlsym(binary, "av_dict_set"));
        s_freeDictionary = reinterpret_cast<FreeDictionary>(dlsym(binary, "av_dict_free"));
        if (s_openCodec && s_setDictionary && s_freeDictionary) {
            auto hook = Mod::get()->hook(reinterpret_cast<void*>(s_openCodec), &echo_open_codec, "Echo encoder preset");
            installed = hook.isOk();
        }
        dlclose(binary);
    }
}
#endif

// the fast capture: the gpu turns the frame into nv12 itself & it is read back a few frames late through a ring of buffers,
// so nothing waits on the gpu. windows had it first. mac had been reading every frame back as full bgra, on the spot
// (133mb a frame at 8k) & leaving the colour conversion to ffmpeg on the cpu
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
#define ECHO_GPU_CAPTURE 1
#endif

namespace {
    struct RenderTiming {
        std::atomic<long long> read_ns{ 0 }, wait_ns{ 0 }, encode_ns{ 0 };
        chrono::steady_clock::time_point began{};

        void reset() {
            read_ns = 0;
            wait_ns = 0;
            encode_ns = 0;
            began = chrono::steady_clock::now();
        }
    };

    RenderTiming g_timing;
    // lines for logs/render.txt, 1 every 5 seconds of video: where the level, the song & the icon are
    std::string g_render_notes;

    struct Timed {
        std::atomic<long long>& into;
        chrono::steady_clock::time_point from = chrono::steady_clock::now();

        ~Timed() {
            into += chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now() - from).count();
        }
    };
}

class AudioTap {
public:
    static AudioTap& get() {
        static AudioTap value;
        return value;
    }

    bool attach(float music, float sfx, fs::path const& file);
    void detach();
    void advance(float dt, float fade);
    void hold_effects();
    static FMOD_RESULT F_CALLBACK read(FMOD_DSP_STATE* state, float* in, float* out, unsigned int length, int in_channels, int* out_channels);

    bool attached = false;
    bool engine_open = false;
    bool mixing = false;
    int rate = 0;
    int channels = 0;
    uint64_t written = 0;
    unsigned song_at_start = 0; // where the song was when the render took it over
    uint64_t stray_blocks = 0;

private:
    FMOD::DSP* dsp = nullptr;
    FMOD::ChannelGroup* master = nullptr;
    std::mutex lock;
    std::vector<float> pending;
    std::ofstream out;
    double clock = 0.0;
    float loudest = 0.f;
    std::string heard;
    float old_music = 1.f, old_sfx = 1.f;
    float effects = 1.f;
    double song_lead = 0.0;
    uint64_t song_mixed = 0;
    bool song_tracked = false;
    std::unordered_set<std::string> undecodable;
    void bind_bus();
    void keep_song_on_time(FMODAudioEngine* engine);
    void sample_streams(FMODAudioEngine* engine);
    void freeze(bool frozen);
};

class Recorder {
public:
    static Recorder& get() {
        static Recorder value;
        return value;
    }
#ifdef GEODE_IS_WINDOWS
    void close_handle(HANDLE& handle);
    bool connect_pipe(HANDLE pipe);
    bool write_all(HANDLE pipe, void const* data, size_t size);
    fs::path game_directory();
    std::string ffmpeg_path();
    std::string run_hidden(std::string command, fs::path const& log_path);
    bool mux_audio();
#endif
#ifdef ECHO_GPU_CAPTURE
    bool harvest(bool wait);
    bool setup_nv12();
    void close_nv12();
    void convert_nv12();
    void draw_full_quad();
#endif
#ifdef GEODE_IS_MACOS
    static fs::path ffmpeg_path();
    bool write_all(int fd, void const* data, size_t size);
    bool mux_audio();
#endif
#ifdef ECHO_FFMPEG_API
    std::string open_api(int fps);
#endif
    static bool api_available();
    static bool prefer_api();
    bool idle() const;
    void go_live();
    bool write_out(std::vector<uint8_t> const& frame);
    void save_api_render();
    void silent_change_size(CCSize size);
    void resize_shader_layer(CCSize size);
    std::optional<CCSize> window_pixels();
    void hold_view();
    void release_view();
    bool open_target();
    void close_target();
    void drawFrame();
    void show_overlay();
    void draw_preview(CCSize pixels);
#ifdef GEODE_IS_IOS
    bool draw_direct();
    GLuint direct_fbo = 0;
    bool direct_failed = false;
#endif
    void draw_progress();
    void queue_frame(std::vector<uint8_t> frame);
    std::vector<uint8_t> acquire_frame();
    void encode_loop();
    void finish_video();
    size_t frame_bytes() const;
    void uncap_pace();
    void restore_pace();

    static constexpr int RING = 6;
#ifdef GEODE_IS_WINDOWS
    static constexpr size_t QUEUE = 12;
#else
    static constexpr size_t QUEUE = 4;
#endif

    bool active = false;
    bool paused = false;
    bool starting = false;
    bool cancel = false;
    bool holding = false;
    bool use_api = false;
    bool saving = false;
    std::atomic<bool> broken = false;
    std::string failure;
#ifdef GEODE_IS_WINDOWS
    HANDLE video = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
#endif
#ifdef ECHO_FFMPEG_API
    std::unique_ptr<ffmpeg::events::Recorder> api;
#endif
#ifdef GEODE_IS_MACOS
    pid_t process = 0;
    int video = -1;
    fs::path video_log;
    std::string encode_error;
#endif
    GLint screen_fbo = 0;
    fs::path output;
    fs::path audio;
    int width = 0, height = 0;
    int frames = 0, seen_frames = 0;
    float endTime = 0.f;
    float fade = 1.f;
    float audio_speed = 1.f;
    CCSize saved_frame{}, saved_pixels{};
    float saved_win_width = 0.f, ui_shift = 0.f;
    PlayLayer* ui_owner = nullptr;
    void shift_ui(PlayLayer* play, float by);

    GLuint fbo = 0;
    GLuint color = 0;
    GLuint y_fbo = 0, uv_fbo = 0;
    GLuint y_color = 0, uv_color = 0;
    GLuint y_prog = 0, uv_prog = 0;
    bool nv12 = false;
    int saved_swap = 1;
    float saved_interval = 1.f / 60.f;
    bool pace_held = false;
#ifdef ECHO_GPU_CAPTURE
    std::array<GLuint, RING> pbo{};
    std::deque<int> in_flight;
    int next_slot = 0;
#endif
#ifdef GEODE_IS_WINDOWS
    std::array<GLsync, RING> fences{};
#endif
    CCLayerColor* shade = nullptr;
    // gds own level progress bar (the groove sprite w/ the fill behind it), drawn in the middle of the window
    CCSprite* bar_back = nullptr;
    CCSprite* bar_fill = nullptr;
    CCLabelBMFont* bar_text = nullptr;
    std::string bar_shown;
    double text_clock = 0.0;

    chrono::steady_clock::time_point last_tick{};
    bool ticked = false;
    float wall_delta = 0.f;
    double busy_seconds = 0.0;
    // how fast the last few seconds went, for the time left. the average of the whole render is way off whenever
    // the level gets heavier or lighter part way
    double pace_video = 0.0, pace_wall = 0.0, pace = 0.0;
    float start_fraction = -1.f;
    int last_input_frame = 0;

    std::thread worker;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::vector<uint8_t>> queue;
    std::vector<std::vector<uint8_t>> spare_frames;
    bool closing = false;
};

FMOD_RESULT F_CALLBACK AudioTap::read(FMOD_DSP_STATE*, float* in, float* out, unsigned int length, int in_channels, int* out_channels) {
    auto& tap = AudioTap::get();
    size_t amt = static_cast<size_t>(length) * static_cast<size_t>(in_channels);
    std::memcpy(out, in, amt * sizeof(float));
    if (out_channels)
        *out_channels = in_channels;
    std::lock_guard guard(tap.lock);
    if (!tap.mixing) {
        if (tap.attached)
            ++tap.stray_blocks;
        return FMOD_OK;
    }
    tap.channels = in_channels;
    tap.pending.insert(tap.pending.end(), in, in + amt);
    return FMOD_OK;
}

void AudioTap::bind_bus() {
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system || !dsp)
        return;
    FMOD::ChannelGroup* live = nullptr;
    if (engine->m_system->getMasterChannelGroup(&live) != FMOD_OK || !live)
        return;
    if (live != master) {
        // DONT leave it frozen
        if (master && attached)
            master->setPaused(false);
        if (master)
            master->removeDSP(dsp);
        if (live->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, dsp) == FMOD_OK)
            master = live;
    }
    if (!master)
        return;
    auto adopt = [&](FMOD::ChannelGroup* group) {
        if (!group || group == master)
            return;
        group->setPaused(false);
        master->addGroup(group, true, nullptr);
    };
    adopt(engine->m_backgroundMusicChannel);
    adopt(engine->m_globalChannel);
    if (engine->m_globalChannel && engine->m_reverbChannel)
        engine->m_globalChannel->addGroup(engine->m_reverbChannel, true, nullptr);
}

void AudioTap::hold_effects() {
    if (!attached)
        return;
    auto* engine = FMODAudioEngine::get();
    if (!engine)
        return;
    float volume = std::clamp(effects, 0.f, 1.f);
    engine->setEffectsVolume(volume);
    if (engine->m_globalChannel) {
        engine->m_globalChannel->setVolume(volume);
        engine->m_globalChannel->setPaused(false);
    }
    if (engine->m_reverbChannel)
        engine->m_reverbChannel->setPaused(false);
}

bool AudioTap::attach(float music, float sfx, fs::path const& file) {
    auto* engine = FMODAudioEngine::get();
    if (attached || !engine || !engine->m_system)
        return false;
    auto* system = engine->m_system;
    FMOD_DSP_DESCRIPTION description{};
    description.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    std::strncpy(description.name, "Echo Render", sizeof(description.name) - 1);
    description.numinputbuffers = 1;
    description.numoutputbuffers = 1;
    description.read = &AudioTap::read;
    if (system->createDSP(&description, &dsp) != FMOD_OK || !dsp)
        return false;
    if (system->getMasterChannelGroup(&master) != FMOD_OK || !master || master->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, dsp) != FMOD_OK) {
        dsp->release();
        dsp = nullptr;
        return false;
    }
    gd::string song;
    unsigned song_ms = 0;
    if (auto it = engine->m_channelIDToSoundPath.find(0); it != engine->m_channelIDToSoundPath.end()) {
        song = it->second;
        song_ms = engine->getMusicTimeMS(0);
    }
    if (auto it = engine->m_fmodMusic.find(0); song.empty() && it != engine->m_fmodMusic.end() && !it->second.m_filePath.empty()) {
        song = it->second.m_filePath;
        song_ms = engine->getMusicTimeMS(0);
    }
    out.open(file, std::ios::binary | std::ios::trunc);
    if (!out || system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT) != FMOD_OK) {
        out.close();
        master->removeDSP(dsp);
        dsp->release();
        dsp = nullptr;
        system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
        return false;
    }
    system->getSoftwareFormat(&rate, nullptr, nullptr);
    if (!song.empty()) {
        engine->stopAndRemoveMusic(0);
        // gd loads songs bit by bit & render is faster than that so the song ran out
        // i just load the whole thing
        if (auto it = engine->m_fmodMusic.find(0); it != engine->m_fmodMusic.end() && it->second.m_sound) {
            FMOD_MODE mode = 0;
            it->second.m_sound->getMode(&mode);
            if (mode & FMOD_CREATESTREAM) {
                FMOD::Sound* sample = nullptr;
                FMOD_MODE wanted = (mode & ~static_cast<FMOD_MODE>(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
                auto res = system->createSound(it->second.m_filePath.c_str(), wanted, nullptr, &sample);
                // DONT free ts gd might still use it
                if (res == FMOD_OK && sample)
                    it->second.m_sound = sample;
            }
        }
        engine->queueStartMusic(song, 1.f, 0.f, std::clamp(music, 0.f, 1.f), false, static_cast<int>(song_ms), 0, 0, 0, 0, false, 0, false, false);
        engine->updateQueuedMusic();
        for (int tries = 0; tries < 600 && !engine->getActiveMusicChannel(0); ++tries) {
            std::this_thread::sleep_for(chrono::milliseconds(5));
            system->update();
            engine->updateQueuedMusic();
        }
        sample_streams(engine);
        // play once NOT loop
        auto* channel = engine->getActiveMusicChannel(0);
        if (channel)
            channel->setLoopCount(0);
        // jukebox (replaced songs) adds the songs start offset inside queueStartMusic. song_ms was read off the channel, so it
        // already had that offset in it & the song came back that much AHEAD for the whole render (0.4s on wndystein).
        // setting the spot on the channel itself goes around every hook
        if (channel)
            channel->setPosition(song_ms, FMOD_TIMEUNIT_MS);
        song_at_start = song_ms;
        bool playing = false;
        if (channel)
            channel->isPlaying(&playing);
    }
    if (engine->m_backgroundMusicChannel)
        engine->m_backgroundMusicChannel->setPaused(false);
    old_music = engine->getBackgroundMusicVolume();
    old_sfx = engine->getEffectsVolume();
    effects = std::clamp(sfx, 0.f, 1.f);
    engine->setBackgroundMusicVolume(std::clamp(music, 0.f, 1.f));
    bind_bus();
    hold_effects();
    pending.clear();
    channels = 0;
    written = 0;
    clock = 0.0;
    song_tracked = false;
    undecodable.clear();
    stray_blocks = 0;
    attached = true;
    freeze(true);
    return true;
}

void AudioTap::detach() {
    if (!attached)
        return;
    freeze(false);
    heard.clear();
    loudest = 0.f;
    attached = false;
    engine_open = false;
    mixing = false;
    out.close();
    pending.clear();
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system)
        return;
    if (master && dsp)
        master->removeDSP(dsp);
    if (dsp)
        dsp->release();
    dsp = nullptr;
    master = nullptr;
    engine->setBackgroundMusicVolume(old_music);
    engine->setEffectsVolume(old_sfx);
    engine->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
}

// freezes all sound between frames
// without ts sfx play like 2-3x speed :/
void AudioTap::freeze(bool frozen) {
    if (master)
        master->setPaused(frozen);
}

void AudioTap::sample_streams(FMODAudioEngine* engine) {
    auto* system = engine->m_system;
    std::vector<FMOD::Channel*> playing;
    auto add = [&](FMOD::Channel* channel) {
        if (channel && std::ranges::find(playing, channel) == playing.end())
            playing.push_back(channel);
    };
    for (auto& [id, channel] : engine->m_channelIDToChannel)
        add(channel);
    auto walk = [&](auto& self, FMOD::ChannelGroup* group, int depth) -> void {
        if (!group || depth > 8)
            return;
        int amt = 0;
        if (group->getNumChannels(&amt) == FMOD_OK) {
            for (int i = 0; i < amt; ++i) {
                FMOD::Channel* channel = nullptr;
                if (group->getChannel(i, &channel) == FMOD_OK)
                    add(channel);
            }
        }
        int groups = 0;
        if (group->getNumGroups(&groups) != FMOD_OK)
            return;
        for (int i = 0; i < groups; ++i) {
            FMOD::ChannelGroup* child = nullptr;
            if (group->getGroup(i, &child) == FMOD_OK)
                self(self, child, depth + 1);
        }
    };
    FMOD::ChannelGroup* root = nullptr;
    if (system->getMasterChannelGroup(&root) == FMOD_OK)
        walk(walk, root, 0);

    for (auto* channel : playing) {
        bool active = false;
        FMOD::Sound* sound = nullptr;
        FMOD_MODE mode = 0;
        if (channel->isPlaying(&active) != FMOD_OK || !active || channel->getCurrentSound(&sound) != FMOD_OK || !sound ||
            sound->getMode(&mode) != FMOD_OK || !(mode & FMOD_CREATESTREAM))
            continue;
        std::string path;
        for (auto& [music_id, entry] : engine->m_fmodMusic) {
            if (entry.m_sound == sound)
                path = entry.m_filePath;
        }
        for (auto& [name, entry] : engine->m_fmodSounds) {
            if (path.empty() && entry.m_sound == sound)
                path = entry.m_filePath.empty() ? std::string(name) : std::string(entry.m_filePath);
        }
        for (auto& [name, cached] : engine->m_soundPathToSound) {
            if (path.empty() && cached == sound)
                path = name;
        }
        if (path.empty() || undecodable.contains(path))
            continue;
        FMOD::Sound* sample = nullptr;
        FMOD_MODE wanted = (mode & ~static_cast<FMOD_MODE>(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
        auto res = system->createSound(path.c_str(), wanted, nullptr, &sample);
        if (res != FMOD_OK || !sample) {
            undecodable.insert(path);
            continue;
        }
        unsigned position = 0;
        bool paused = false;
        float volume = 1.f, pitch = 1.f, frequency = 0.f;
        int loops = 0;
        FMOD::ChannelGroup* group = nullptr;
        channel->getPosition(&position, FMOD_TIMEUNIT_PCM);
        channel->getPaused(&paused);
        channel->getVolume(&volume);
        channel->getPitch(&pitch);
        channel->getFrequency(&frequency);
        channel->getLoopCount(&loops);
        channel->getChannelGroup(&group);
        unsigned fades = 0;
        std::vector<unsigned long long> fade_clocks;
        std::vector<float> fade_volumes;
        if (channel->getFadePoints(&fades, nullptr, nullptr) == FMOD_OK && fades > 0) {
            fade_clocks.resize(fades);
            fade_volumes.resize(fades);
            channel->getFadePoints(&fades, fade_clocks.data(), fade_volumes.data());
        }
        FMOD::Channel* next = nullptr;
        if (system->playSound(sample, group, true, &next) != FMOD_OK || !next) {
            sample->release();
            undecodable.insert(path);
            continue;
        }
        next->setVolumeRamp(false);
        next->setLoopCount(loops);
        next->setPosition(position, FMOD_TIMEUNIT_PCM);
        next->setVolume(volume);
        next->setPitch(pitch);
        if (frequency > 0.f)
            next->setFrequency(frequency);
        for (unsigned i = 0; i < fades; ++i)
            next->addFadePoint(fade_clocks[i], fade_volumes[i]);
        void* data = nullptr;
        channel->getUserData(&data);
        next->setUserData(data);
        // if i dont clear ts gd thinks the new one ended xd
        channel->setCallback(nullptr);
        channel->stop();
        for (auto& [id, held] : engine->m_channelIDToChannel) {
            if (held == channel)
                held = next;
        }
        for (auto& [music_id, entry] : engine->m_fmodMusic) {
            if (entry.m_sound == sound)
                entry.m_sound = sample;
        }
        for (auto& [name, entry] : engine->m_fmodSounds) {
            if (entry.m_sound == sound)
                entry.m_sound = sample;
        }
        for (auto& [name, cached] : engine->m_soundPathToSound) {
            if (cached == sound)
                cached = sample;
        }
        next->setPaused(paused);
    }
}

void AudioTap::keep_song_on_time(FMODAudioEngine* engine) {
    auto* channel = engine->getActiveMusicChannel(0);
    bool playing = false;
    unsigned position = 0;
    if (!channel || channel->isPlaying(&playing) != FMOD_OK || !playing || channel->getPosition(&position, FMOD_TIMEUNIT_PCM) != FMOD_OK) {
        song_tracked = false;
        return;
    }
    bool paused = false;
    channel->getPaused(&paused);
    if (paused)
        return;
    size_t buffered = 0;
    {
        std::lock_guard guard(lock);
        buffered = channels > 0 ? pending.size() / static_cast<size_t>(channels) : 0;
    }
    uint64_t mixed = written + buffered;
    double now = (double)(position);
    float frequency = 0.f;
    if (song_tracked && rate > 0 && channel->getFrequency(&frequency) == FMOD_OK && frequency > 0.f) {
        float pitch = 1.f;
        channel->getPitch(&pitch);
        double seconds = (double)(mixed - std::min(mixed, song_mixed)) / (double)(rate);
        double expected = song_lead + seconds * (double)(frequency) * (double)(pitch);
        double moved = std::abs(now - expected);
        if (moved > 2.0 && moved < 0.15 * (double)(frequency) && expected >= 0.0 &&
            channel->setPosition(static_cast<unsigned>(std::llround(expected)), FMOD_TIMEUNIT_PCM) == FMOD_OK)
            now = expected;
    }
    song_lead = now;
    song_mixed = mixed;
    song_tracked = true;
}

void AudioTap::advance(float dt, float fade) {
    auto* engine = FMODAudioEngine::get();
    if (!attached || !engine || !engine->m_system || rate <= 0)
        return;
    auto* play = PlayLayer::get();
    bool opening = Logic::get().render_record_pause && play && !play->m_started;
    if (opening) {
        if (engine->m_backgroundMusicChannel)
            engine->m_backgroundMusicChannel->setPaused(true);
        clock += (double)(dt);
        uint64_t target = static_cast<uint64_t>(std::llround(clock * (double)(rate)));
        int ch = channels > 0 ? channels : 2;
        if (written < target) {
            size_t frames = static_cast<size_t>(target - written);
            std::vector<float> silence(frames * static_cast<size_t>(ch), 0.f);
            out.write(reinterpret_cast<char const*>(silence.data()), static_cast<std::streamsize>(silence.size() * sizeof(float)));
            written = target;
        }
        return;
    }
    clock += dt;
    engine_open = true;
    engine->update(dt);
    engine_open = false;
    hold_effects();
    sample_streams(engine);
    keep_song_on_time(engine);
    uint64_t target = static_cast<uint64_t>(std::llround(clock * (double)(rate)));
    auto buffered = [this] {
        std::lock_guard guard(lock);
        return channels > 0 ? pending.size() / static_cast<size_t>(channels) : 0;
    };
    mixing = true;
    freeze(false);
    int stalls = 0;
    while (written + buffered() < target && stalls < 64) {
        size_t before = buffered();
        engine->m_system->update();
        stalls = buffered() == before ? stalls + 1 : 0;
    }
    freeze(true);
    mixing = false;
    std::lock_guard guard(lock);
    if (channels <= 0)
        return;
    size_t take = static_cast<size_t>(std::min<uint64_t>(target - std::min(target, written), pending.size() / static_cast<size_t>(channels)));
    size_t amt = take * static_cast<size_t>(channels);
    float gain = std::clamp(fade, 0.f, 1.f);
    for (size_t i = 0; i < amt; ++i)
        pending[i] = std::clamp(pending[i] * gain, -1.f, 1.f);
    for (size_t i = 0; i < amt; ++i)
        loudest = std::max(loudest, std::abs(pending[i]));
    if (written / static_cast<uint64_t>(rate) != (written + take) / static_cast<uint64_t>(rate)) {
        if (heard.size() < 24)
            heard += fmt::format("{:.2f} ", loudest);
        loudest = 0.f;
    }
    out.write(reinterpret_cast<char const*>(pending.data()), static_cast<std::streamsize>(amt * sizeof(float)));
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(amt));
    written += take;
}

#ifdef GEODE_IS_WINDOWS
void Recorder::close_handle(HANDLE& handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        handle = INVALID_HANDLE_VALUE;
        return;
    }
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
}

bool Recorder::connect_pipe(HANDLE pipe) {
    OVERLAPPED wait{};
    wait.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!wait.hEvent)
        return false;
    BOOL ok = ConnectNamedPipe(pipe, &wait);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WaitForSingleObject(wait.hEvent, 4000) == WAIT_OBJECT_0;
    if (!ok && GetLastError() == ERROR_PIPE_CONNECTED)
        ok = TRUE;
    CloseHandle(wait.hEvent);
    return ok == TRUE;
}

bool Recorder::write_all(HANDLE pipe, void const* data, size_t size) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE)
        return false;
    auto const* bytes = static_cast<uint8_t const*>(data);
    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!event)
        return false;
    size_t sent = 0;
    while (sent < size) {
        OVERLAPPED wait{};
        wait.hEvent = event;
        ResetEvent(event);
        DWORD wrote = 0;
        DWORD chunk = static_cast<DWORD>(std::min(size - sent, static_cast<size_t>(1 << 20)));
        BOOL ok = WriteFile(pipe, bytes + sent, chunk, &wrote, &wait);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &wait);
                CloseHandle(event);
                return false;
            }
            if (!GetOverlappedResult(pipe, &wait, &wrote, FALSE)) {
                CloseHandle(event);
                return false;
            }
        }
        else if (!ok || wrote == 0) {
            CloseHandle(event);
            return false;
        }
        sent += wrote;
    }
    CloseHandle(event);
    return true;
}

fs::path Recorder::game_directory() {
    char module[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, module, MAX_PATH))
        return {};
    return fs::path(module).parent_path();
}

std::string Recorder::ffmpeg_path() {
    auto beside = Recorder::get().game_directory() / "ffmpeg.exe";
    if (fs::exists(beside))
        return beside.string();
    return "ffmpeg.exe";
}

std::string Recorder::run_hidden(std::string command, fs::path const& log_path) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log = CreateFileA(log_path.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        return "could not open the ffmpeg log";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle(input);
        return "could not start ffmpeg";
    }
    CloseHandle(process.hThread);
    CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    if (WaitForSingleObject(process.hProcess, 180000) == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code == 0)
        return {};
    std::ifstream in(log_path);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        if (!line.empty())
            last = line;
    }
    if (last.empty())
        last = "ffmpeg failed";
    if (last.size() > 160)
        last = last.substr(last.size() - 160);
    return last;
}

namespace {
    std::string quote_arg(fs::path const& path) {
        return "\"" + path.string() + "\"";
    }

    fs::path probe_binary() {
        auto beside = fs::path(Recorder::get().ffmpeg_path()).replace_filename("ffprobe.exe");
        if (fs::exists(beside))
            return beside;
        return "ffprobe.exe";
    }

    std::string run_logged(std::string command, fs::path const& log_path, fs::path const& capture) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE log = CreateFileA(log_path.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE out = capture.empty()
                         ? log
                         : CreateFileA(capture.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
            if (log != INVALID_HANDLE_VALUE)
                CloseHandle(log);
            if (out != INVALID_HANDLE_VALUE && out != log)
                CloseHandle(out);
            if (input != INVALID_HANDLE_VALUE)
                CloseHandle(input);
            return "could not open the ffmpeg log";
        }
        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;
        startup.hStdOutput = out;
        startup.hStdError = log;
        PROCESS_INFORMATION process{};
        std::vector<char> mutable_command(command.begin(), command.end());
        mutable_command.push_back('\0');
        if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process)) {
            CloseHandle(log);
            if (out != log)
                CloseHandle(out);
            if (input != INVALID_HANDLE_VALUE)
                CloseHandle(input);
            return "could not start ffmpeg";
        }
        CloseHandle(process.hThread);
        CloseHandle(log);
        if (out != log)
            CloseHandle(out);
        if (input != INVALID_HANDLE_VALUE)
            CloseHandle(input);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        if (code == 0)
            return {};
        std::ifstream in(log_path);
        std::string line;
        std::string last;
        while (std::getline(in, line)) {
            if (!line.empty())
                last = line;
        }
        if (last.empty())
            last = "ffmpeg failed";
        if (last.size() > 180)
            last = last.substr(last.size() - 180);
        return last;
    }

    std::optional<double> probe_seconds(fs::path const& file, fs::path const& scratch, std::string& error) {
        auto command = quote_arg(probe_binary()) + " -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 " + quote_arg(file);
        auto capture = scratch / "echo-probe.txt";
        auto log = scratch / "echo-probe.log";
        error = run_logged(command, log, capture);
        if (!error.empty())
            return std::nullopt;
        std::ifstream in(capture);
        std::string text;
        std::getline(in, text);
        try {
            size_t used = 0;
            double seconds = std::stod(text, &used);
            if (seconds > 0.0)
                return seconds;
        }
        catch (...) {
        }
        error = "Could not read the video length";
        return std::nullopt;
    }

    bool probe_audio(fs::path const& file, fs::path const& scratch) {
        auto command = quote_arg(probe_binary()) + " -v error -select_streams a:0 -show_entries stream=index -of csv=p=0 " + quote_arg(file);
        auto capture = scratch / "echo-probe-audio.txt";
        auto log = scratch / "echo-probe.log";
        if (!run_logged(command, log, capture).empty())
            return false;
        std::ifstream in(capture);
        std::string text;
        std::getline(in, text);
        return !text.empty();
    }

    std::string join_films_in(fs::path const& first, fs::path const& second, fs::path const& dest, std::string const& encode);
    std::string video_rate(std::string const& codec, int bitrate, std::string const& extra);

    std::string join_films(fs::path const& first, fs::path const& second, fs::path const& dest, std::string const& encode) {
        auto res = join_films_in(first, second, dest, encode);
        auto scratch = dest.parent_path();
        std::error_code error;
        for (char const* name : { "echo-probe.txt", "echo-probe-audio.txt", "echo-probe.log", "echo-merge.log", "echo-merge-progress.txt" })
            fs::remove(scratch / name, error);
        return res;
    }

    // same codec & quality as a normal render
    std::string merge_encode() {
        auto& logic = Logic::get();
        std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
        // -vf NOT allowed next to filter_complex
        bool clash = logic.render_args.find("-vf") != std::string::npos || logic.render_args.find("-filter") != std::string::npos;
        std::string extra = clash ? std::string() : logic.render_args;
        bool hardware = codec.find("nvenc") != std::string::npos || codec.find("amf") != std::string::npos || codec.find("qsv") != std::string::npos;
        std::string flags = " -c:v " + codec + video_rate(codec, logic.render_bitrate, extra);
        if (extra.find("-pix_fmt") == std::string::npos)
            flags += logic.render_bitrate <= 0 && !hardware ? " -pix_fmt yuv444p" : " -pix_fmt yuv420p";
        if (!extra.empty())
            flags += " " + extra;
        return flags;
    }

    std::string join_films_in(fs::path const& first, fs::path const& second, fs::path const& dest, std::string const& encode) {
        auto scratch = dest.parent_path();
        std::string probe_error;
        auto first_length = probe_seconds(first, scratch, probe_error);
        if (!first_length) {
            std::error_code error;
            auto beside = fs::path(Recorder::get().ffmpeg_path()).replace_filename("ffprobe.exe");
            if (!fs::exists(beside, error))
                return "Merging needs ffprobe.exe next to ffmpeg.exe (" + probe_error + ")";
            return probe_error;
        }
        auto second_length = probe_seconds(second, scratch, probe_error);
        if (!second_length)
            return probe_error;
        double fade = std::min(1.0, std::min(*first_length, *second_length) * 0.45);
        if (!(fade > 0.05))
            return "The videos are too short to crossfade";
        double offset = *first_length - fade;
        Logic::get().merge_seconds = (float)(*first_length + *second_length - fade);
        bool audio = probe_audio(first, scratch) && probe_audio(second, scratch);
        std::string filter = fmt::format(
            "[1:v][0:v]scale2ref[v1][v0];[v0][v1]xfade=transition=fadeblack:duration={:.3f}:offset={:.3f}[v]",
            fade,
            offset);
        std::string maps = "-map [v]";
        std::string audio_codec;
        if (audio) {
            filter += fmt::format(";[0:a][1:a]acrossfade=d={:.3f}[a]", fade);
            maps += " -map [a]";
            audio_codec = "-c:a aac -b:a 320k";
        }
        auto command = fmt::format(
            "{} -y -nostdin -progress {} -i {} -i {} -filter_complex {} {}{} {} {}",
            quote_arg(Recorder::get().ffmpeg_path()),
            quote_arg(scratch / "echo-merge-progress.txt"),
            quote_arg(first),
            quote_arg(second),
            filter,
            maps,
            encode,
            audio_codec,
            quote_arg(dest));
        return run_logged(command, scratch / "echo-merge.log", {});
    }
}

void Logic::merge_renders(fs::path const& first_path, fs::path const& second_path) {
    if (merge_busy)
        return;
    auto folder = save_root() / "renders";
    std::error_code error;
    fs::create_directories(folder, error);
    merge_busy = true;
    merge_seconds = 0.f;
    merge_folder = folder;
    merge_label = fmt::format("{} & {}", first_path.stem().string(), second_path.stem().string());
    notify("Joining in the background", true);
    std::thread([this, folder, first_path, second_path, encode = merge_encode()] {
        std::string failure;
        fs::path dest;
        try {
            dest = ReplayIO::unique_file(folder, ReplayIO::sanitize(first_path.stem().string() + "_" + second_path.stem().string()), ".mp4");
            failure = join_films(first_path, second_path, dest, encode);
        }
        catch (std::exception const& ex) {
            failure = ex.what();
        }
        Loader::get()->queueInMainThread([this, failure, dest] {
            merge_busy = false;
            if (failure.empty())
                notify(fmt::format("Joined {}", dest.filename().string()), true);
            else
                notify(failure, false);
        });
    }).detach();
}
#elif defined(GEODE_IS_MACOS)
namespace {
    fs::path find_tool(char const* name) {
        std::vector<fs::path> places{ Logic::get().save_root() / name };
        for (char const* dir : { "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin" })
            places.push_back(fs::path(dir) / name);
        if (char const* path = std::getenv("PATH")) {
            std::string_view rest(path);
            while (!rest.empty()) {
                auto colon = rest.find(':');
                auto dir = rest.substr(0, colon);
                if (!dir.empty())
                    places.push_back(fs::path(std::string(dir)) / name);
                if (colon == std::string_view::npos)
                    break;
                rest.remove_prefix(colon + 1);
            }
        }
        for (auto const& place : places) {
            if (access(place.c_str(), X_OK) == 0)
                return place;
        }
        return {};
    }

    pid_t spawn_tool(std::vector<std::string> const& args, int input, fs::path const& out, fs::path const& err) {
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        posix_spawn_file_actions_init(&actions);
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
        if (input >= 0)
            posix_spawn_file_actions_adddup2(&actions, input, 0);
        else
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        auto direct = [&](int fd, fs::path const& file) {
            if (file.empty())
                posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_WRONLY, 0);
            else
                posix_spawn_file_actions_addopen(&actions, fd, file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        };
        direct(2, err);
        if (!out.empty() && out == err)
            posix_spawn_file_actions_adddup2(&actions, 2, 1);
        else
            direct(1, out);
        std::vector<char*> argv;
        for (auto const& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        int failed = posix_spawn(&pid, args.front().c_str(), &actions, &attributes, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        return failed ? 0 : pid;
    }

    int wait_tool(pid_t pid, double seconds) {
        auto started = chrono::steady_clock::now();
        while (true) {
            int status = 0;
            pid_t done = waitpid(pid, &status, seconds > 0.0 ? WNOHANG : 0);
            if (done == pid)
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (done < 0 && errno == ECHILD)
                return 0;
            if (done < 0 && errno != EINTR)
                return -1;
            if (seconds > 0.0) {
                if (chrono::duration<double>(chrono::steady_clock::now() - started).count() > seconds) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    return -1;
                }
                std::this_thread::sleep_for(chrono::milliseconds(10));
            }
        }
    }

    std::string last_line(fs::path const& log, size_t limit) {
        std::ifstream in(log);
        std::string line;
        std::string last;
        while (std::getline(in, line)) {
            if (!line.empty())
                last = line;
        }
        if (last.size() > limit)
            last = last.substr(last.size() - limit);
        return last;
    }

    std::string run_tool(std::vector<std::string> const& args, fs::path const& log, fs::path const& capture, double seconds = 0.0) {
        pid_t pid = spawn_tool(args, -1, capture.empty() ? log : capture, log);
        if (!pid)
            return "could not start " + fs::path(args.front()).filename().string();
        if (wait_tool(pid, seconds) == 0)
            return {};
        auto last = last_line(log, 180);
        return last.empty() ? std::string("ffmpeg failed") : last;
    }

    std::vector<std::string> split_args(std::string const& text) {
        std::vector<std::string> words;
        std::string word;
        bool any = false;
        char quote = 0;
        for (char c : text) {
            if (quote) {
                if (c == quote)
                    quote = 0;
                else
                    word += c;
            }
            else if (c == '"' || c == '\'') {
                quote = c;
                any = true;
            }
            else if (c == ' ' || c == '\t' || c == '\n') {
                if (any)
                    words.push_back(std::move(word));
                word.clear();
                any = false;
            }
            else {
                word += c;
                any = true;
            }
        }
        if (any)
            words.push_back(std::move(word));
        return words;
    }

    std::vector<std::string> video_rate(std::string const& codec, int bitrate, std::string const& extra, int64_t pixel_rate) {
        auto has = [&](char const* key) {
            return extra.find(key) != std::string::npos;
        };
        std::vector<std::string> flags;
        if ((codec == "libx264" || codec == "libx265") && !has("-preset"))
            flags.insert(flags.end(), { "-preset", Logic::get().render_preset });
        bool own_rate = has("-crf") || has("-qp") || has("-b:v") || has("-q:v") || has("-cq") || has("-global_quality") || has("-qscale") || has("-rc ");
        if (own_rate)
            return flags;
        if (bitrate > 0)
            flags.insert(flags.end(), { "-b:v", std::to_string(bitrate) });
        else if (codec.find("videotoolbox") != std::string::npos)
            flags.insert(flags.end(), { "-b:v", std::to_string(std::max<int64_t>(pixel_rate / 4, 8'000'000)) });
        else
            flags.insert(flags.end(), { "-qp", "0" });
        return flags;
    }

    std::optional<double> probe_seconds(fs::path const& probe, fs::path const& file, fs::path const& scratch, std::string& error) {
        auto capture = scratch / "echo-probe.txt";
        error = run_tool({ probe.string(), "-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", file.string() }, scratch / "echo-probe.log", capture);
        if (!error.empty())
            return std::nullopt;
        std::ifstream in(capture);
        std::string text;
        std::getline(in, text);
        try {
            double seconds = std::stod(text);
            if (seconds > 0.0)
                return seconds;
        }
        catch (...) {
        }
        error = "Could not read the video length";
        return std::nullopt;
    }

    bool probe_audio(fs::path const& probe, fs::path const& file, fs::path const& scratch) {
        auto capture = scratch / "echo-probe-audio.txt";
        if (!run_tool({ probe.string(), "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=index", "-of", "csv=p=0", file.string() }, scratch / "echo-probe.log", capture).empty())
            return false;
        std::ifstream in(capture);
        std::string text;
        std::getline(in, text);
        return !text.empty();
    }

    // same codec & quality as a normal render
    std::vector<std::string> merge_encode() {
        auto& logic = Logic::get();
        std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
        // -vf NOT allowed next to filter_complex
        bool clash = logic.render_args.find("-vf") != std::string::npos || logic.render_args.find("-filter") != std::string::npos;
        std::string extra = clash ? std::string() : logic.render_args;
        bool hardware = codec.find("videotoolbox") != std::string::npos;
        std::vector<std::string> flags{ "-c:v", codec };
        // no size to go off of so 1080p60
        auto rate = video_rate(codec, logic.render_bitrate, extra, static_cast<int64_t>(1920) * 1080 * 60);
        flags.insert(flags.end(), rate.begin(), rate.end());
        if (extra.find("-pix_fmt") == std::string::npos)
            flags.insert(flags.end(), { "-pix_fmt", logic.render_bitrate <= 0 && !hardware ? "yuv444p" : "yuv420p" });
        auto words = split_args(extra);
        flags.insert(flags.end(), words.begin(), words.end());
        return flags;
    }

    std::string join_films_in(fs::path const& first, fs::path const& second, fs::path const& dest, std::vector<std::string> const& encode) {
        auto ffmpeg = Recorder::ffmpeg_path();
        if (ffmpeg.empty())
            return "ffmpeg was not found";
        auto probe = ffmpeg.parent_path() / "ffprobe";
        if (access(probe.c_str(), X_OK) != 0)
            probe = find_tool("ffprobe");
        if (probe.empty())
            return "Merging needs ffprobe, which comes with ffmpeg";
        auto scratch = dest.parent_path();
        std::string probe_error;
        auto first_length = probe_seconds(probe, first, scratch, probe_error);
        if (!first_length)
            return probe_error;
        auto second_length = probe_seconds(probe, second, scratch, probe_error);
        if (!second_length)
            return probe_error;
        double fade = std::min(1.0, std::min(*first_length, *second_length) * 0.45);
        if (!(fade > 0.05))
            return "The videos are too short to crossfade";
        double offset = *first_length - fade;
        Logic::get().merge_seconds = (float)(*first_length + *second_length - fade);
        bool audio = probe_audio(probe, first, scratch) && probe_audio(probe, second, scratch);
        std::string filter = fmt::format("[1:v][0:v]scale2ref[v1][v0];[v0][v1]xfade=transition=fadeblack:duration={:.3f}:offset={:.3f}[v]", fade, offset);
        std::vector<std::string> args{ ffmpeg.string(), "-y", "-nostdin", "-progress", (scratch / "echo-merge-progress.txt").string(), "-i", first.string(), "-i", second.string() };
        if (audio)
            filter += fmt::format(";[0:a][1:a]acrossfade=d={:.3f}[a]", fade);
        args.insert(args.end(), { "-filter_complex", filter, "-map", "[v]" });
        if (audio)
            args.insert(args.end(), { "-map", "[a]" });
        args.insert(args.end(), encode.begin(), encode.end());
        if (audio)
            args.insert(args.end(), { "-c:a", "aac", "-b:a", "320k" });
        args.push_back(dest.string());
        return run_tool(args, scratch / "echo-merge.log", {});
    }

    std::string join_films(fs::path const& first, fs::path const& second, fs::path const& dest, std::vector<std::string> const& encode) {
        auto res = join_films_in(first, second, dest, encode);
        std::error_code error;
        for (char const* name : { "echo-probe.txt", "echo-probe-audio.txt", "echo-probe.log", "echo-merge.log", "echo-merge-progress.txt" })
            fs::remove(dest.parent_path() / name, error);
        return res;
    }
}

fs::path Recorder::ffmpeg_path() {
    return find_tool("ffmpeg");
}

bool Recorder::write_all(int fd, void const* data, size_t size) {
    if (fd < 0)
        return false;
    auto const* bytes = static_cast<uint8_t const*>(data);
    while (size > 0) {
        ssize_t wrote = ::write(fd, bytes, size);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        bytes += wrote;
        size -= static_cast<size_t>(wrote);
    }
    return true;
}

void Logic::merge_renders(fs::path const& first_path, fs::path const& second_path) {
    if (merge_busy)
        return;
    auto folder = save_root() / "renders";
    std::error_code error;
    fs::create_directories(folder, error);
    merge_busy = true;
    merge_seconds = 0.f;
    merge_folder = folder;
    merge_label = fmt::format("{} & {}", first_path.stem().string(), second_path.stem().string());
    notify("Joining in the background", true);
    std::thread([this, folder, first_path, second_path, encode = merge_encode()] {
        std::string failure;
        fs::path dest;
        try {
            dest = ReplayIO::unique_file(folder, ReplayIO::sanitize(first_path.stem().string() + "_" + second_path.stem().string()), ".mp4");
            failure = join_films(first_path, second_path, dest, encode);
        }
        catch (std::exception const& ex) {
            failure = ex.what();
        }
        Loader::get()->queueInMainThread([this, failure, dest] {
            merge_busy = false;
            if (failure.empty())
                notify(fmt::format("Joined {}", dest.filename().string()), true);
            else
                notify(failure, false);
        });
    }).detach();
}

#else
void Logic::merge_renders(fs::path const&, fs::path const&) {
    notify("Merging renders needs ffmpeg.exe on Windows", false);
}
#endif

// ffmpeg writes how far it is to a file, -1 = dont know yet
float Logic::merge_progress() {
    static float shown = -1.f;
    static chrono::steady_clock::time_point last;
    if (!merge_busy)
        return shown = -1.f;
    auto now = chrono::steady_clock::now();
    if (now - last < chrono::milliseconds(250))
        return shown;
    last = now;
    float total = merge_seconds;
    std::ifstream in(merge_folder / "echo-merge-progress.txt", std::ios::binary | std::ios::ate);
    if (!in || total <= 0.f)
        return shown = -1.f;
    auto size = static_cast<std::streamoff>(in.tellg());
    std::streamoff len = std::min<std::streamoff>(size, 600);
    std::string tail(static_cast<size_t>(len), '\0');
    in.seekg(size - len);
    in.read(tail.data(), len);
    // old ffmpeg calls it out_time_ms but its STILL microseconds
    auto at = tail.rfind("out_time_us=");
    if (at == std::string::npos)
        at = tail.rfind("out_time_ms=");
    if (at == std::string::npos)
        return shown;
    double micro = std::strtod(tail.c_str() + at + 12, nullptr);
    return shown = std::clamp((float)(micro / 1e6 / total), 0.f, 1.f);
}

// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
void Recorder::silent_change_size(CCSize size) {
    auto director = CCDirector::sharedDirector();
    auto view = CCEGLView::sharedOpenGLView();
    view->CCEGLViewProtocol::setFrameSize(size.width, size.height);
    director->updateScreenScale(size);
    director->setViewport();
    director->setProjection(kCCDirectorProjection2D);
    glViewport(0, 0, size.width, size.height);
}

// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
void Recorder::resize_shader_layer(CCSize size) {
    auto gjbgl = GJBaseGameLayer::get();
    if (!gjbgl || !gjbgl->m_shaderLayer) {
        return;
    }

    ShaderLayer* sh = gjbgl->m_shaderLayer;
    auto winSize = CCDirector::get()->getWinSize();

    sh->m_screenSize = size;
    sh->m_scaleFactor = size.height / winSize.height;
    sh->m_aspectRatio = size.width / size.height;

    float csf = CCDirector::get()->getContentScaleFactor();
    CCDirector::get()->setContentScaleFactor(1.0);
    sh->m_renderTexture->release();
    sh->m_renderTexture = CCRenderTexture::create(
        size.width, size.height, kCCTexture2DPixelFormat_RGBA8888);
    sh->m_renderTexture->retain();
    CCDirector::get()->setContentScaleFactor(csf);
    sh->m_sprite->setTexture(sh->m_renderTexture->getSprite()->getTexture());
    sh->m_textureContentSize = sh->m_sprite->getTexture()->getContentSize();
    sh->m_targetTextureSize = size;
    sh->m_targetTextureSizeExtra = CCSize(0.0f, 0.0f);
    sh->m_sprite->setTextureRect(
        { -1.0f, -1.0f, size.width + 2.0f, size.height + 2.0f });
    sh->m_state.m_textureScaleX = size.width / winSize.width;
    sh->m_state.m_textureScaleY = size.height / winSize.height;
}

std::optional<CCSize> Recorder::window_pixels() {
#ifdef GEODE_IS_WINDOWS
    HDC context = wglGetCurrentDC();
    HWND window = context ? WindowFromDC(context) : nullptr;
    RECT client{};
    if (!window || !GetClientRect(window, &client))
        return std::nullopt;
    if (client.right <= client.left || client.bottom <= client.top)
        return std::nullopt;
    return CCSize((float)(client.right - client.left), (float)(client.bottom - client.top));
#else
    return std::nullopt;
#endif
}

// the bar & the % were placed for the screens width. a video of another shape has to move them or they sit off centre
void Recorder::shift_ui(PlayLayer* play, float by) {
    if (!play || by == 0.f)
        return;
    if (play->m_progressBar)
        play->m_progressBar->setPositionX(play->m_progressBar->getPositionX() + by * .5f);
    if (play->m_percentageLabel)
        play->m_percentageLabel->setPositionX(play->m_percentageLabel->getPositionX() + by * .5f);
    if (play->m_uiLayer && play->m_uiLayer->m_pauseBtn)
        play->m_uiLayer->m_pauseBtn->setPositionX(play->m_uiLayer->m_pauseBtn->getPositionX() + by);
}

void Recorder::hold_view() {
    CCSize size((float)(width), (float)(height));
    if (!holding) {
        saved_win_width = CCDirector::sharedDirector()->getWinSize().width;
        saved_frame = CCEGLView::sharedOpenGLView()->getFrameSize();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MOBILE)
        saved_pixels = window_pixels().value_or(saved_frame);
#else
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        saved_pixels = CCSize((float)(viewport[0] * 2 + viewport[2]), (float)(viewport[1] * 2 + viewport[3]));
        if (saved_pixels.width < 1.f || saved_pixels.height < 1.f)
            saved_pixels = saved_frame;
#endif
        holding = true;
    }
    if (!CCEGLView::sharedOpenGLView()->getFrameSize().equals(size))
        Recorder::get().silent_change_size(size);
    if (auto* play = PlayLayer::get(); play && !ui_owner) {
        float by = CCDirector::sharedDirector()->getWinSize().width - saved_win_width;
        if (std::fabs(by) > .5f) {
            shift_ui(play, by);
            ui_shift = by;
            ui_owner = play;
        }
    }
    auto* layer = GJBaseGameLayer::get();
    auto* shader = layer ? layer->m_shaderLayer : nullptr;
    if (shader && (!shader->m_screenSize.equals(size) || !shader->m_targetTextureSize.equals(size)))
        Recorder::get().resize_shader_layer(size);
}

void Recorder::release_view() {
    if (!holding)
        return;
    holding = false;
    auto pixels = window_pixels().value_or(saved_pixels);
    CCSize restore(
        saved_frame.width * pixels.width / std::max(saved_pixels.width, 1.f),
        saved_frame.height * pixels.height / std::max(saved_pixels.height, 1.f));
    Recorder::get().silent_change_size(restore);
    Recorder::get().resize_shader_layer(restore);
    // only the level that got moved, a new one was built at the videos shape already
    if (ui_owner && ui_owner == PlayLayer::get())
        shift_ui(ui_owner, -ui_shift);
    ui_owner = nullptr;
    ui_shift = 0.f;
    CCDirector::sharedDirector()->setNextDeltaTimeZero(true);
}

size_t Recorder::frame_bytes() const {
    size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    return nv12 ? pixels + pixels / 2 : pixels * 4;
}

#ifdef ECHO_GPU_CAPTURE
namespace {
    GLuint compileShader(GLenum kind, char const* source) {
        GLuint shader = glCreateShader(kind);
        if (!shader)
            return 0;
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok)
            return shader;
        glDeleteShader(shader);
        return 0;
    }

    GLuint link_program(char const* vertex, char const* fragment) {
        GLuint vs = compileShader(GL_VERTEX_SHADER, vertex);
        GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragment);
        if (!vs || !frag) {
            if (vs)
                glDeleteShader(vs);
            if (frag)
                glDeleteShader(frag);
            return 0;
        }
        GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, frag);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(frag);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (ok)
            return program;
        glDeleteProgram(program);
        return 0;
    }

    GLuint make_color_target(GLint internal, GLenum format, int width, int height) {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return texture;
    }

    GLuint make_framebuffer(GLuint texture) {
        GLuint buf = 0;
        glGenFramebuffers(1, &buf);
        glBindFramebuffer(GL_FRAMEBUFFER, buf);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &buf);
            return 0;
        }
        return buf;
    }
}
#endif

#ifdef GEODE_IS_WINDOWS
namespace {
    std::string encoder_flags(std::string const& codec, std::string const& extra, bool with_rate) {
        auto has = [&](char const* key) {
            return extra.find(key) != std::string::npos;
        };
        std::string flags;
        if (!has("-preset") && !has("-quality")) {
            if (codec.find("nvenc") != std::string::npos)
                flags += " -preset p1";
            else if (codec.find("amf") != std::string::npos)
                flags += " -quality speed";
            else if (codec.find("qsv") != std::string::npos)
                flags += " -preset veryfast";
            else if (codec == "libx264" || codec == "libx265")
                flags += " -preset " + Logic::get().render_preset;
        }
        if (with_rate && codec.find("nvenc") != std::string::npos && !has("-rc "))
            flags += " -rc vbr";
        return flags;
    }

    std::string video_rate(std::string const& codec, int bitrate, std::string const& extra) {
        auto has = [&](char const* key) {
            return extra.find(key) != std::string::npos;
        };
        bool own_rate = has("-crf") || has("-qp") || has("-b:v") || has("-q:v") || has("-cq") || has("-global_quality") || has("-qscale") || has("-rc ");
        if (own_rate)
            return encoder_flags(codec, extra, false);
        if (bitrate > 0)
            return encoder_flags(codec, extra, true) + fmt::format(" -b:v {}", bitrate);
        std::string flags;
        if (!has("-preset") && !has("-quality")) {
            if (codec.find("nvenc") != std::string::npos)
                flags += " -preset p1";
            else if (codec.find("amf") != std::string::npos)
                flags += " -quality speed";
            else if (codec.find("qsv") != std::string::npos)
                flags += " -preset veryfast";
            else if (codec == "libx264" || codec == "libx265")
                flags += " -preset " + Logic::get().render_preset;
        }
        if (codec.find("nvenc") != std::string::npos)
            flags += " -rc constqp -qp 0";
        else if (codec.find("amf") != std::string::npos)
            flags += " -rc cqp -qp_i 0 -qp_p 0";
        else if (codec.find("qsv") != std::string::npos)
            flags += " -global_quality 1";
        else
            flags += " -qp 0";
        return flags;
    }

    using SwapIntervalFn = BOOL(WINAPI*)(int);
    using GetSwapIntervalFn = int(WINAPI*)();
}
#endif

#ifdef ECHO_GPU_CAPTURE
void Recorder::draw_full_quad() {
    glBegin(GL_QUADS);
    glTexCoord2f(0.f, 0.f);
    glVertex2f(-1.f, -1.f);
    glTexCoord2f(1.f, 0.f);
    glVertex2f(1.f, -1.f);
    glTexCoord2f(1.f, 1.f);
    glVertex2f(1.f, 1.f);
    glTexCoord2f(0.f, 1.f);
    glVertex2f(-1.f, 1.f);
    glEnd();
}

bool Recorder::setup_nv12() {
    static char const* vertex = R"(
#version 120
void main() {
	gl_Position = gl_Vertex;
	gl_TexCoord[0].xy = vec2(gl_MultiTexCoord0.x, 1.0 - gl_MultiTexCoord0.y);
}
)";
    static char const* luma = R"(
#version 120
uniform sampler2D u_texture;
void main() {
	vec3 rgb = texture2D(u_texture, gl_TexCoord[0].xy).rgb;
	float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	y = y * 219.0 / 255.0 + 16.0 / 255.0;
	gl_FragColor = vec4(y, 0.0, 0.0, 1.0);
}
)";
    static char const* chroma = R"(
#version 120
uniform sampler2D u_texture;
void main() {
	vec3 rgb = texture2D(u_texture, gl_TexCoord[0].xy).rgb;
	float u = dot(rgb, vec3(-0.11457, -0.38543, 0.5)) + 0.5;
	float v = dot(rgb, vec3(0.5, -0.45415, -0.04585)) + 0.5;
	u = u * 224.0 / 255.0 + 16.0 / 255.0;
	v = v * 224.0 / 255.0 + 16.0 / 255.0;
	gl_FragColor = vec4(u, v, 0.0, 1.0);
}
)";
    y_prog = link_program(vertex, luma);
    uv_prog = link_program(vertex, chroma);
    if (!y_prog || !uv_prog)
        return false;
    GLint old_texture = 0, old_fbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    y_color = make_color_target(GL_R8, GL_RED, width, height);
    uv_color = make_color_target(GL_RG8, GL_RG, width / 2, height / 2);
    y_fbo = make_framebuffer(y_color);
    uv_fbo = make_framebuffer(uv_color);
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    return y_color && uv_color && y_fbo && uv_fbo;
}

void Recorder::close_nv12() {
    if (y_prog)
        glDeleteProgram(y_prog);
    if (uv_prog)
        glDeleteProgram(uv_prog);
    if (y_fbo)
        glDeleteFramebuffers(1, &y_fbo);
    if (uv_fbo)
        glDeleteFramebuffers(1, &uv_fbo);
    if (y_color)
        glDeleteTextures(1, &y_color);
    if (uv_color)
        glDeleteTextures(1, &uv_color);
    y_prog = 0;
    uv_prog = 0;
    y_fbo = 0;
    uv_fbo = 0;
    y_color = 0;
    uv_color = 0;
    nv12 = false;
}

void Recorder::convert_nv12() {
    GLint old_prog = 0, old_texture = 0, old_fbo = 0;
    GLint viewport[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLboolean blend = glIsEnabled(GL_BLEND);
    GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
    if (blend)
        glDisable(GL_BLEND);
    if (depth)
        glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glBindFramebuffer(GL_FRAMEBUFFER, y_fbo);
    glViewport(0, 0, width, height);
    glUseProgram(y_prog);
    glUniform1i(glGetUniformLocation(y_prog, "u_texture"), 0);
    draw_full_quad();
    glBindFramebuffer(GL_FRAMEBUFFER, uv_fbo);
    glViewport(0, 0, width / 2, height / 2);
    glUseProgram(uv_prog);
    glUniform1i(glGetUniformLocation(uv_prog, "u_texture"), 0);
    draw_full_quad();
    glUseProgram(old_prog);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (blend)
        glEnable(GL_BLEND);
    if (depth)
        glEnable(GL_DEPTH_TEST);
}
#endif

void Recorder::uncap_pace() {
    if (pace_held)
        return;
    if (auto* director = CCDirector::sharedDirector()) {
        saved_interval = director->getAnimationInterval();
        if (!(saved_interval > 0.f))
            saved_interval = 1.f / 60.f;
        director->setAnimationInterval(1.f / 10000.f);
    }
#ifdef GEODE_IS_WINDOWS
    saved_swap = 1;
    if (auto get_swap = reinterpret_cast<GetSwapIntervalFn>(wglGetProcAddress("wglGetSwapIntervalEXT")))
        saved_swap = get_swap();
    if (auto set_swap = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT")))
        set_swap(0);
#endif
#ifdef GEODE_IS_MACOS
    if (auto context = CGLGetCurrentContext()) {
        GLint swap = 1;
        saved_swap = CGLGetParameter(context, kCGLCPSwapInterval, &swap) == kCGLNoError ? swap : 1;
        GLint none = 0;
        CGLSetParameter(context, kCGLCPSwapInterval, &none);
    }
#endif
    pace_held = true;
}

void Recorder::restore_pace() {
    if (!pace_held)
        return;
    pace_held = false;
    if (auto* director = CCDirector::sharedDirector())
        director->setAnimationInterval(saved_interval > 0.f ? saved_interval : 1.f / 60.f);
#ifdef GEODE_IS_WINDOWS
    if (auto set_swap = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT")))
        set_swap(saved_swap);
#endif
#ifdef GEODE_IS_MACOS
    if (auto context = CGLGetCurrentContext()) {
        GLint swap = saved_swap;
        CGLSetParameter(context, kCGLCPSwapInterval, &swap);
    }
#endif
}

bool Recorder::open_target() {
    GLint old_fbo = 0, old_texture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
#ifdef GEODE_IS_WINDOWS
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glBindTexture(GL_TEXTURE_2D, old_texture);

#ifdef ECHO_GPU_CAPTURE
    bool want_nv12 = !use_api;
#ifdef GEODE_IS_MACOS
    {
        // nv12 only has half the colour detail (4:2:0), so its just for renders that end up 4:2:0 anyway.
        // bitrate 0 w/ a software codec = lossless 4:4:4, that keeps getting whole bgra frames like before.
        // (lossless 4:2:0 is also a file apples decoder refuses, players open it & show nothing)
        auto const& logic = Logic::get();
        bool hardware = logic.render_codec.find("videotoolbox") != std::string::npos;
        bool own_format = logic.render_args.find("-pix_fmt") != std::string::npos;
        if ((logic.render_bitrate <= 0 && !hardware && !own_format) || logic.render_args.find("444") != std::string::npos)
            want_nv12 = false;
    }
#endif
    nv12 = want_nv12 && Recorder::get().setup_nv12();
    if (!nv12)
        Recorder::get().close_nv12();
    bool ring = true;
#ifdef GEODE_IS_MACOS
    // mac only has the ring when the nv12 shaders worked. w/out them it reads frames the old way
    ring = nv12;
#endif
    if (ring) {
        auto bytes = static_cast<GLsizeiptr>(Recorder::get().frame_bytes());
        glGenBuffers(RING, pbo.data());
        for (auto buf : pbo) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, buf);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
#ifdef GEODE_IS_WINDOWS
    fences.fill(nullptr);
#endif
    in_flight.clear();
    next_slot = 0;
#endif

    shade = CCLayerColor::create(ccc4(0, 0, 0, 0));
    shade->retain();
    bar_back = CCSprite::create("slidergroove2.png");
    bar_fill = CCSprite::create("sliderBar2.png");
    if (bar_back)
        bar_back->retain();
    if (bar_fill)
        bar_fill->retain();
    if (bar_back && bar_fill) {
        // same setup PlayLayer does for its bar: the fill repeats sideways & sits 2px in, behind the groove
        ccTexParams params = { GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE };
        bar_fill->getTexture()->setTexParameters(&params);
        bar_fill->setAnchorPoint({ 0.f, 0.5f });
        bar_fill->setPosition({ 2.f, bar_back->getContentSize().height * 0.5f });
        bar_back->addChild(bar_fill, -1);
    }
    bar_text = CCLabelBMFont::create("Rendering", "bigFont.fnt");
    if (bar_text)
        bar_text->retain();
    bar_shown.clear();
    text_clock = 0.0;
    return complete;
}

void Recorder::close_target() {
#ifdef GEODE_IS_WINDOWS
    for (auto& fence : fences) {
        if (fence)
            glDeleteSync(fence);
        fence = nullptr;
    }
#endif
#ifdef ECHO_GPU_CAPTURE
    in_flight.clear();
    if (pbo[0])
        glDeleteBuffers(RING, pbo.data());
    pbo.fill(0);
    Recorder::get().close_nv12();
#endif
#ifdef GEODE_IS_IOS
    if (direct_fbo)
        glDeleteFramebuffers(1, &direct_fbo);
    direct_fbo = 0;
    direct_failed = false;
#endif
    if (fbo)
        glDeleteFramebuffers(1, &fbo);
    if (color)
        glDeleteTextures(1, &color);
    fbo = 0;
    color = 0;
    if (shade)
        shade->release();
    shade = nullptr;
    if (bar_back)
        bar_back->release();
    if (bar_fill)
        bar_fill->release();
    if (bar_text)
        bar_text->release();
    bar_back = nullptr;
    bar_fill = nullptr;
    bar_text = nullptr;
}

void Recorder::drawFrame() {
    auto* director = CCDirector::sharedDirector();
    GLint window = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &window);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    kmGLPushMatrix();
    if (auto* play = PlayLayer::get())
        play->visit();
    else if (auto* scene = director->getRunningScene())
        scene->visit();
    if (fade < 1.f && shade) {
        shade->setContentSize(director->getWinSize());
        shade->setOpacity(static_cast<GLubyte>(std::lround((1.f - fade) * 255.f)));
        shade->visit();
    }
    kmGLPopMatrix();

#ifdef ECHO_GPU_CAPTURE
    bool ring = true;
#ifdef GEODE_IS_MACOS
    ring = nv12;
#endif
    if (ring) {
        if (nv12)
            Recorder::get().convert_nv12();
        int slot = next_slot;
        // the slot ts frame goes in has to be empty first
        while (static_cast<int>(in_flight.size()) >= RING && Recorder::get().harvest(true)) {
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        if (nv12) {
            glBindFramebuffer(GL_FRAMEBUFFER, y_fbo);
            glReadPixels(0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            glBindFramebuffer(GL_FRAMEBUFFER, uv_fbo);
            auto offset = static_cast<uintptr_t>(width) * static_cast<uintptr_t>(height);
            glReadPixels(0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(offset));
        }
        else {
            glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
#ifdef GEODE_IS_WINDOWS
        fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
        in_flight.push_back(slot);
        next_slot = (slot + 1) % RING;
        while (Recorder::get().harvest(false)) {
        }
        glBindFramebuffer(GL_FRAMEBUFFER, window);
        return;
    }
#endif
#ifndef GEODE_IS_WINDOWS
#ifdef GEODE_IS_IOS
    if (Recorder::get().draw_direct()) {
        glBindFramebuffer(GL_FRAMEBUFFER, window);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
#endif
    auto frame = Recorder::get().acquire_frame();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    {
        Timed timed{ g_timing.read_ns };
#ifdef GEODE_IS_MACOS
        if (!use_api)
            glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frame.data());
        else
#endif
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
    }
    Recorder::get().queue_frame(std::move(frame));
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, window);
}

void Recorder::show_overlay() {
    if (!active)
        return;
#ifdef GEODE_IS_MOBILE
    // touches use the real screen size NOT the video size
    if (holding)
        silent_change_size(saved_frame);
#endif
    auto pixels = window_pixels().value_or(saved_pixels);
    if (pixels.width < 1.f || pixels.height < 1.f)
        pixels = CCSize(1.f, 1.f);
    GLfloat clear[4] = {};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor)
        glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(screen_fbo));
    glViewport(0, 0, static_cast<GLsizei>(pixels.width), static_cast<GLsizei>(pixels.height));
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (Logic::get().render_show_lvl)
        Recorder::get().draw_preview(pixels);
    PauseLayer* pause = nullptr;
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene())
        pause = scene->getChildByType<PauseLayer>(0);
    if (!pause) {
        if (auto* play = PlayLayer::get())
            pause = play->getChildByType<PauseLayer>(0);
    }
    if (pause) {
        kmGLPushMatrix();
        pause->visit();
        kmGLPopMatrix();
    }
    Recorder::get().draw_progress();
    // geode stuff on top goes on screen NOT in the video
    if (auto* overlay = OverlayManager::get()) {
        kmGLPushMatrix();
        overlay->visit();
        kmGLPopMatrix();
    }

    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
}

#ifdef GEODE_IS_IOS
namespace {
    GLuint flip_program() {
        static GLuint program = 0;
        static bool tried = false;
        if (tried)
            return program;
        tried = true;
        auto compile = [](GLenum type, char const* source) {
            GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint ok = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (ok != GL_TRUE) {
                glDeleteShader(shader);
                return GLuint{ 0 };
            }
            return shader;
        };
        GLuint vertex = compile(GL_VERTEX_SHADER, "attribute vec2 a_position; attribute vec2 a_texCoord; varying vec2 v_texCoord; void main() { v_texCoord = a_texCoord; gl_Position = vec4(a_position, 0.0, 1.0); }");
        GLuint fragment = compile(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 v_texCoord; uniform sampler2D u_texture; void main() { gl_FragColor = texture2D(u_texture, v_texCoord); }");
        if (!vertex || !fragment)
            return 0;
        GLuint made = glCreateProgram();
        glAttachShader(made, vertex);
        glAttachShader(made, fragment);
        glBindAttribLocation(made, kCCVertexAttrib_Position, "a_position");
        glBindAttribLocation(made, kCCVertexAttrib_TexCoords, "a_texCoord");
        glLinkProgram(made);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint ok = GL_FALSE;
        glGetProgramiv(made, GL_LINK_STATUS, &ok);
        if (ok != GL_TRUE) {
            glDeleteProgram(made);
            return 0;
        }
        ccGLUseProgram(made);
        glUniform1i(glGetUniformLocation(made, "u_texture"), 0);
        program = made;
        return program;
    }
}

bool Recorder::draw_direct() {
    if (!use_api || !api || direct_failed)
        return false;
    Timed timed{ g_timing.read_ns };
    GLuint program = flip_program();
    unsigned target = 0;
    std::string failed;
    if (!program || !api->beginDirect(target, failed)) {
        direct_failed = true;
        return false;
    }
    if (!direct_fbo)
        glGenFramebuffers(1, &direct_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, direct_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glViewport(0, 0, width, height);
        GLfloat const corners[] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
        GLfloat const coords[] = { 0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f };
        ccGLUseProgram(program);
        ccGLBindTexture2D(color);
        ccGLBlendFunc(GL_ONE, GL_ZERO);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position | kCCVertexAttribFlag_TexCoords);
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, corners);
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    bool sent = complete ? api->endDirect(failed) : true;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    if (!complete) {
        direct_failed = true;
        return false;
    }
    if (!sent) {
        std::lock_guard guard(lock);
        failure = failed;
        broken = true;
    }
    return true;
}
#endif

void Recorder::draw_preview(CCSize pixels) {
    if (!color || width < 1 || height < 1 || pixels.width < 1.f || pixels.height < 1.f)
        return;
    auto* program = CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTexture);
    if (!program)
        return;
    auto win = CCDirector::sharedDirector()->getWinSize();
    float screen = pixels.width / pixels.height;
    float video = (float)(width) / (float)(height);
    float across = video >= screen ? 1.f : video / screen;
    float up = video >= screen ? screen / video : 1.f;
    float x0 = win.width * (1.f - across) * .5f, y0 = win.height * (1.f - up) * .5f, x1 = win.width - x0;
    float y1 = win.height - y0;
    GLfloat const corners[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    GLfloat const coords[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    program->use();
    program->setUniformsForBuiltins();
    ccGLBindTexture2D(color);
    ccGLBlendFunc(GL_ONE, GL_ZERO);
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position | kCCVertexAttribFlag_TexCoords);
    glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, corners);
    glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Recorder::draw_progress() {
    if (!bar_text)
        return;
    auto status = Logic::get().render_status();
    if (!status.active)
        return;
    auto win = CCDirector::sharedDirector()->getWinSize();
    float scale = win.height / 320.f;
    CCPoint middle = { win.width * 0.5f, win.height * 0.5f };
    if (bar_back && bar_fill) {
        float full = bar_back->getTextureRect().size.width - 4.f;
        bar_fill->setTextureRect({ 0.f, 0.f, full * std::clamp(status.known ? status.progress : 0.f, 0.f, 1.f), 8.f });
        if (auto* manager = GameManager::get())
            bar_fill->setColor(manager->colorForIdx(manager->getPlayerColor()));
        bar_back->setScale(1.3f * scale);
        bar_back->setPosition(middle);
    }
    text_clock += wall_delta;
    if (bar_shown.empty() || text_clock >= 0.2) {
        text_clock = 0.0;
        if (status.text != bar_shown) {
            bar_shown = status.text;
            bar_text->setString(bar_shown.c_str());
        }
    }
    bar_text->setScale(0.4f * scale);
    bar_text->setPosition({ middle.x, middle.y + 22.f * scale });
    kmGLPushMatrix();
    if (bar_back && bar_fill)
        bar_back->visit();
    bar_text->visit();
    kmGLPopMatrix();
}

#ifdef ECHO_GPU_CAPTURE
bool Recorder::harvest(bool wait) {
    if (in_flight.empty())
        return false;
    int slot = in_flight.front();
#ifdef GEODE_IS_WINDOWS
    GLenum state = glClientWaitSync(fences[slot], wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, wait ? 5'000'000'000ull : 0);
    if (!wait && state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
        return false;
    glDeleteSync(fences[slot]);
    fences[slot] = nullptr;
#else
    // macs old gl has no fences to ask. a frame is only taken back once the ring is nearly full,
    // which is frames after it was drawn, so the map below doesnt have to wait for the gpu
    if (!wait && static_cast<int>(in_flight.size()) < RING - 1)
        return false;
    Timed timed{ g_timing.read_ns };
#endif
    in_flight.pop_front();
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
    if (auto const* data = static_cast<uint8_t const*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY))) {
        auto frame = Recorder::get().acquire_frame();
        std::memcpy(frame.data(), data, frame.size());
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        Recorder::get().queue_frame(std::move(frame));
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}
#endif

std::vector<uint8_t> Recorder::acquire_frame() {
    std::vector<uint8_t> frame;
    {
        std::lock_guard guard(lock);
        if (!spare_frames.empty()) {
            frame = std::move(spare_frames.back());
            spare_frames.pop_back();
        }
    }
    frame.resize(frame_bytes());
    return frame;
}

void Recorder::queue_frame(std::vector<uint8_t> frame) {
    std::unique_lock guard(lock);
    {
        Timed timed{ g_timing.wait_ns };
        wake.wait(guard, [this] {
            return queue.size() < QUEUE || broken;
        });
    }
    if (broken)
        return;
    queue.push_back(std::move(frame));
    wake.notify_all();
}

void Recorder::encode_loop() {
    while (true) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock guard(lock);
            wake.wait(guard, [this] {
                return closing || !queue.empty();
            });
            if (queue.empty())
                return;
            frame = std::move(queue.front());
            queue.pop_front();
        }
        wake.notify_all();
        if (!broken && !Recorder::get().write_out(frame)) {
            broken = true;
            wake.notify_all();
        }
        {
            std::lock_guard guard(lock);
            if (spare_frames.size() < QUEUE + 1)
                spare_frames.push_back(std::move(frame));
        }
    }
}

void Recorder::finish_video() {
#ifdef ECHO_GPU_CAPTURE
    while (Recorder::get().harvest(true)) {
    }
#endif
    if (worker.joinable()) {
        {
            std::lock_guard guard(lock);
            closing = true;
        }
        wake.notify_all();
        worker.join();
    }
    queue.clear();
    spare_frames.clear();
    closing = false;
#ifdef ECHO_FFMPEG_API
    if (api)
        api->stop();
    api.reset();
#endif
#ifdef GEODE_IS_WINDOWS
    Recorder::get().close_handle(video);
    if (process) {
        if (WaitForSingleObject(process, 30000) != WAIT_OBJECT_0)
            TerminateProcess(process, 0);
        CloseHandle(process);
        process = nullptr;
    }
#endif
#ifdef GEODE_IS_MACOS
    if (video >= 0)
        close(video);
    video = -1;
    if (process) {
        int code = wait_tool(process, 60.0);
        process = 0;
        if (code != 0) {
            encode_error = last_line(video_log, 160);
            if (encode_error.empty())
                encode_error = "ffmpeg did not finish the video";
        }
    }
    std::error_code ignored;
    if (encode_error.empty() && !video_log.empty())
        fs::remove(video_log, ignored);
#endif
}

bool Recorder::api_available() {
#if defined(GEODE_IS_IOS)
    return true;
#elif defined(ECHO_FFMPEG_API)
    return Loader::get()->isModLoaded("eclipse.ffmpeg-api") && ffmpeg::events::impl::getVTable().createRecorder;
#else
    return false;
#endif
}

bool Recorder::prefer_api() {
#ifndef ECHO_FFMPEG_API
    return false;
#elif defined(GEODE_IS_WINDOWS)
    return !fs::exists(Recorder::get().game_directory() / "ffmpeg.exe") && Recorder::api_available();
#else
    return true;
#endif
}

bool Recorder::idle() const {
    if (active)
        return false;
#ifdef ECHO_FFMPEG_API
    if (api)
        return false;
#endif
#ifdef GEODE_IS_WINDOWS
    return process == nullptr && video == INVALID_HANDLE_VALUE;
#elif defined(GEODE_IS_MACOS)
    return process == 0 && video < 0;
#else
    return true;
#endif
}

bool Recorder::write_out(std::vector<uint8_t> const& frame) {
    Timed timed{ g_timing.encode_ns };
#ifdef ECHO_FFMPEG_API
    if (api) {
        auto res = api->writeFrame(frame);
        if (res.isErr()) {
            std::lock_guard guard(lock);
            failure = res.unwrapErr();
        }
        return res.isOk();
    }
#endif
#ifdef GEODE_IS_WINDOWS
    return Recorder::get().write_all(video, frame.data(), frame.size());
#elif defined(GEODE_IS_MACOS)
    if (Recorder::get().write_all(video, frame.data(), frame.size()))
        return true;
    std::this_thread::sleep_for(chrono::milliseconds(100));
    auto reason = last_line(video_log, 160);
    std::lock_guard guard(lock);
    failure = reason;
    return false;
#else
    return false;
#endif
}

void Recorder::go_live() {
    auto& logic = Logic::get();
    worker = std::thread([] {
        Recorder::get().encode_loop();
    });
    uncap_pace();
    active = true;
    logic.render_speed_locked = audio_speed;
#if defined(GEODE_IS_IOS)
    logic.message = "Rendering";
#elif defined(ECHO_FFMPEG_API)
    logic.message = use_api ? "Rendering (FFmpeg API)" : nv12 ? "Rendering (GPU)"
                                                              : "Rendering";
#else
    logic.message = nv12 ? "Rendering (GPU)" : "Rendering";
#endif
    if (!AudioTap::get().attach(logic.render_music, logic.render_sfx, audio)) {
        logic.message = "Rendering (audio capture failed, video only)";
    }
}

#ifdef ECHO_FFMPEG_API
std::string Recorder::open_api(int fps) {
#ifdef GEODE_IS_ANDROID
    install_encoder_options();
#endif
    auto const& logic = Logic::get();
    if (!Recorder::api_available())
        return "FFmpeg API is not installed";
    auto encoder = std::make_unique<ffmpeg::events::Recorder>();
    if (!encoder->isValid())
        return "FFmpeg API did not respond";
    auto codecs = ffmpeg::events::Recorder::getAvailableCodecs();
    auto offered = [&](std::string const& name) {
        return codecs.empty() || std::ranges::find(codecs, name) != codecs.end();
    };
    std::vector<std::string> wanted;
#ifdef GEODE_IS_ANDROID
    if (logic.render_codec.empty() || logic.render_codec == "libx264")
        wanted.push_back("h264_mediacodec");
#endif
    std::vector<std::string> fallbacks{ logic.render_codec, "libx264", "h264_videotoolbox", "h264_mediacodec", "libopenh264", "mpeg4" };
    wanted.insert(wanted.end(), fallbacks.begin(), fallbacks.end());
    std::vector<std::string> picks;
    std::ranges::copy_if(wanted, std::back_inserter(picks), [&](std::string const& name) {
        return !name.empty() && offered(name) && std::ranges::find(picks, name) == picks.end();
    });
    auto color_modes = logic.render_color_fix ? std::vector<bool>{ true, false } : std::vector<bool>{ false };
    std::vector<std::pair<std::string, bool>> attempts;
    std::ranges::for_each(picks, [&](std::string const& codec) {
        std::ranges::transform(color_modes, std::back_inserter(attempts), [&](bool color_fix) {
            return std::pair{ codec, color_fix };
        });
    });
    int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
    int64_t bitrate = logic.render_bitrate > 0 ? logic.render_bitrate : std::max<int64_t>(pixels * fps / 4, 8'000'000);
    std::string error = "No usable video codec";
    auto opened = std::ranges::find_if(attempts, [&](auto const& attempt) {
        auto const& [codec, color_fix] = attempt;
        ffmpeg::RenderSettings settings;
#ifdef GEODE_IS_WINDOWS
        settings.m_pixelFormat = ffmpeg::PixelFormat::BGRA;
#else
        settings.m_pixelFormat = ffmpeg::PixelFormat::RGBA;
#endif
        settings.m_codec = codec;
        settings.m_bitrate = bitrate;
        settings.m_width = static_cast<uint32_t>(width);
        settings.m_height = static_cast<uint32_t>(height);
        settings.m_fps = static_cast<uint16_t>(std::clamp(fps, 1, 65535));
        settings.m_outputFile = output;
        settings.m_doVerticalFlip = true;
        settings.m_colorspaceFilters = color_fix ? "all=bt709:iall=bt470bg:fast=1" : "";
#ifdef GEODE_IS_ANDROID
        s_echoPreset = codec == "libx264" ? logic.render_preset.c_str() : nullptr;
        struct PresetScope {
            ~PresetScope() {
                s_echoPreset = nullptr;
            }
        } reset_preset;
#endif
        encoder = std::make_unique<ffmpeg::events::Recorder>();
        auto res = encoder->init(settings);
        if (res.isErr())
            error = res.unwrapErr();
        return res.isOk();
    });
    if (opened == attempts.end()) {
        std::error_code ignored;
        fs::remove(output, ignored);
        return error.empty() ? std::string("FFmpeg API could not open the encoder") : error;
    }
    api = std::move(encoder);
    return {};
}
#endif

namespace {
    bool write_wav(fs::path const& raw, fs::path const& wav, int rate, int channels) {
        std::ifstream in(raw, std::ios::binary);
        std::ofstream out(wav, std::ios::binary | std::ios::trunc);
        if (!in || !out || rate <= 0 || channels <= 0)
            return false;
        auto put = [&](auto value) {
            out.write(reinterpret_cast<char const*>(&value), sizeof(value));
        };
        out.write("RIFF", 4);
        put(uint32_t{ 0 });
        out.write("WAVEfmt ", 8);
        put(uint32_t{ 16 });
        put(uint16_t{ 1 });
        put(uint16_t{ 2 });
        put(static_cast<uint32_t>(rate));
        put(static_cast<uint32_t>(rate) * 4u);
        put(uint16_t{ 4 });
        put(uint16_t{ 16 });
        out.write("data", 4);
        put(uint32_t{ 0 });
        auto sample = [](float value) {
            return static_cast<int16_t>(std::lround((std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f) * 32767.f));
        };
        size_t width = static_cast<size_t>(channels);
        std::vector<float> block(width * 65536);
        uint64_t bytes = 0;
        while (in) {
            in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size() * sizeof(float)));
            size_t amt = static_cast<size_t>(in.gcount()) / (sizeof(float) * width);
            if (amt == 0)
                break;
            auto pcm = std::views::iota(size_t{ 0 }, amt) | std::views::transform([&](size_t i) {
                float const* frame = block.data() + i * width;
                return std::array<int16_t, 2>{ sample(frame[0]), sample(width > 1 ? frame[1] : frame[0]) };
            }) | std::ranges::to<std::vector>();
            out.write(reinterpret_cast<char const*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(pcm[0])));
            bytes += amt * 4;
        }
        auto size = static_cast<uint32_t>(std::min<uint64_t>(bytes, 0xFFFFFFFFull - 36));
        out.seekp(4);
        put(36u + size);
        out.seekp(40);
        put(size);
        return bytes > 0 && out.good();
    }
}

void Recorder::save_api_render() {
#ifdef ECHO_FFMPEG_API
    auto& tap = AudioTap::get();
    std::error_code error;
    bool has_audio = !audio.empty() && frames > 0 && tap.written > 0 && tap.rate > 0 && tap.channels > 0 && fs::exists(output, error);
    auto finish = [](fs::path const& video, std::string const& failed) {
        Recorder::get().saving = false;
        auto& logic = Logic::get();
        if (failed.empty()) {
            logic.message = "Render stopped";
            logic.notify(fmt::format("Saved {}", video.filename().string()), true);
        }
        else {
            logic.message = "Video saved, audio mux failed: " + failed;
            logic.notify(logic.message, false);
        }
    };
    if (!has_audio) {
        if (!audio.empty())
            fs::remove(audio, error);
        if (frames > 0 && fs::exists(output, error))
            finish(output, {});
        else
            Logic::get().message = "Render stopped";
        return;
    }
    saving = true;
    int kept_rate = static_cast<int>(std::max(1ll, std::llround((double)(tap.rate) * std::max(audio_speed, 0.01f))));
    std::thread([finish, video = output, raw = audio, rate = kept_rate, channels = tap.channels] {
        auto folder = video.parent_path();
        auto wav = folder / (video.stem().string() + ".audio.wav");
        auto temp = folder / (video.stem().string() + ".part" + video.extension().string());
        std::string failed;
        std::error_code error;
        std::vector<float> stereo;
        try {
            stereo = echo::audio::read_stereo(raw, channels);
        }
        catch (std::exception const& error) {
            failed = error.what();
        }
#ifdef ECHO_FFMPEG_API
        if (failed.empty()) {
#ifdef GEODE_IS_ANDROID
            s_echoGlobalHeader = true;
            struct HeaderScope {
                ~HeaderScope() {
                    s_echoGlobalHeader = false;
                }
            } reset_header;
#endif
            try {
                // mixVideoRaw takes the samples as 44100 no matter what (phones are 48000, & render speed moves it too)
                // so anything else goes in as a wav, the api reads the real rate off that & converts it
#ifdef GEODE_IS_IOS
                // ios is my own mixer, it works the rate out from the videos length
                if (auto mixed = ffmpeg::events::AudioMixer::mixVideoRaw(video, stereo, temp); mixed.isErr())
                    failed = mixed.unwrapErr();
#else
                if (rate == 44100) {
                    if (auto mixed = ffmpeg::events::AudioMixer::mixVideoRaw(video, stereo, temp); mixed.isErr())
                        failed = mixed.unwrapErr();
                }
                else if (!write_wav(raw, wav, rate, channels))
                    failed = "could not write the audio";
                else {
                    if (auto mixed = ffmpeg::events::AudioMixer::mixVideoAudio(video, wav, temp); mixed.isErr())
                        failed = mixed.unwrapErr();
                    else
                        fs::remove(wav, error);
                }
#endif
            }
            catch (std::exception const& error) {
                failed = error.what();
            }
        }
#endif
        if (failed.empty() && !fs::exists(temp, error))
            failed = "FFmpeg API did not write the video";
        if (failed.empty()) {
            auto backup = folder / (video.filename().string() + ".unmuxed");
            fs::rename(video, backup, error);
            if (!error) {
                fs::rename(temp, video, error);
                if (error) {
                    std::error_code restore_error;
                    fs::rename(backup, video, restore_error);
                }
                else
                    fs::remove(backup, error);
            }
            if (error)
                failed = "could not replace the video";
        }
        if (failed.empty())
            fs::remove(raw, error);
        else {
            fs::remove(temp, error);
            if (write_wav(raw, wav, rate, channels)) {
                fs::remove(raw, error);
                failed += "; audio kept in " + wav.filename().string();
            }
            else
                failed += "; raw audio kept in " + raw.filename().string();
        }
        Loader::get()->queueInMainThread([finish, video, failed] {
            finish(video, failed);
        });
    }).detach();
#endif
}

#ifdef GEODE_IS_WINDOWS
bool Recorder::mux_audio() {
    auto& tap = AudioTap::get();
    std::error_code error;
    if (audio.empty())
        return true;
    bool has_audio = frames > 0 && tap.written > 0 && tap.rate > 0 && tap.channels > 0 && fs::exists(output, error);
    if (!has_audio) {
        fs::remove(audio, error);
        return true;
    }
    auto folder = output.parent_path();
    auto log = folder / "echo-ffmpeg.log";
    auto temp = folder / (output.stem().string() + ".part" + output.extension().string());
    bool lossless = Logic::get().render_bitrate <= 0;
    auto extension = output.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::string audio_codec = extension == ".webm" ? "-c:a libopus -b:a 320k" : lossless ? "-c:a flac"
                                                                                         : "-c:a aac -b:a 320k";
    long long source_rate = std::max(1ll, std::llround((double)(tap.rate) * std::max(audio_speed, 0.01f)));
    std::string command = fmt::format(
        "\"{}\" -y -nostdin -loglevel error -i \"{}\" -f f32le -ar {} -ac {} -i \"{}\" -map 0:v:0 -map 1:a:0 -c:v copy {} -ar {} -ac 2 -muxdelay 0 -muxpreload 0 -shortest \"{}\"",
        Recorder::get().ffmpeg_path(),
        output.string(),
        source_rate,
        tap.channels,
        audio.string(),
        audio_codec,
        tap.rate,
        temp.string());
    std::string failed = Recorder::get().run_hidden(command, log);
    fs::remove(log, error);
    if (failed.empty()) {
        fs::rename(temp, output, error);
        if (error)
            failed = "could not replace the video";
    }
    if (failed.empty()) {
        fs::remove(audio, error);
        return true;
    }
    fs::remove(temp, error);
    auto wav = output.parent_path() / (output.stem().string() + ".wav");
    if (write_wav(audio, wav, static_cast<int>(source_rate), tap.channels)) {
        fs::remove(audio, error);
        failed += "; audio kept in " + wav.filename().string();
    }
    else {
        failed += "; raw audio kept in " + audio.filename().string();
    }
    Logic::get().message = "Video saved, audio mux failed: " + failed;
    return false;
}
#endif

#ifdef GEODE_IS_MACOS
bool Recorder::mux_audio() {
    auto& tap = AudioTap::get();
    std::error_code error;
    if (audio.empty())
        return true;
    bool has_audio = frames > 0 && tap.written > 0 && tap.rate > 0 && tap.channels > 0 && fs::exists(output, error);
    if (!has_audio) {
        fs::remove(audio, error);
        return true;
    }
    auto folder = output.parent_path();
    auto log = folder / "echo-ffmpeg.log";
    auto temp = folder / (output.stem().string() + ".part" + output.extension().string());
    bool lossless = Logic::get().render_bitrate <= 0;
    auto extension = output.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::vector<std::string> audio_codec = extension == ".webm" ? std::vector<std::string>{ "-c:a", "libopus", "-b:a", "320k" }
                                           : lossless           ? std::vector<std::string>{ "-c:a", "flac" }
                                                                : std::vector<std::string>{ "-c:a", "aac", "-b:a", "320k" };
    long long source_rate = std::max(1ll, std::llround((double)(tap.rate) * std::max(audio_speed, 0.01f)));
    std::vector<std::string> args{
        Recorder::ffmpeg_path().string(), "-y", "-nostdin", "-loglevel", "error", "-i", output.string(),
        "-f", "f32le", "-ar", std::to_string(source_rate), "-ac", std::to_string(tap.channels), "-i", audio.string(),
        "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy"
    };
    args.insert(args.end(), audio_codec.begin(), audio_codec.end());
    args.insert(args.end(), { "-ar", std::to_string(tap.rate), "-ac", "2", "-muxdelay", "0", "-muxpreload", "0", "-shortest", temp.string() });
    std::string failed = run_tool(args, log, {}, 180.0);
    fs::remove(log, error);
    if (failed.empty()) {
        fs::rename(temp, output, error);
        if (error)
            failed = "could not replace the video";
    }
    if (failed.empty()) {
        fs::remove(audio, error);
        return true;
    }
    fs::remove(temp, error);
    auto wav = output.parent_path() / (output.stem().string() + ".wav");
    if (write_wav(audio, wav, static_cast<int>(source_rate), tap.channels)) {
        fs::remove(audio, error);
        failed += "; audio kept in " + wav.filename().string();
    }
    else {
        failed += "; raw audio kept in " + audio.filename().string();
    }
    Logic::get().message = "Video saved, audio mux failed: " + failed;
    return false;
}
#endif

namespace {
    float lvl_fraction(PlayLayer* play, int last_input_frame) {
        if (!play)
            return -1.f;
        if (!play->m_isPlatformer && play->m_player1 && play->m_endPortal) {
            float end_x = play->m_endPortal->getPositionX();
            if (end_x > 0.f)
                return std::clamp(play->m_player1->getPositionX() / end_x, 0.f, 1.f);
        }
        if (Logic::get().is_playing() && last_input_frame > 0) {
            return std::clamp((float)(Logic::get().step_frame) / (float)(last_input_frame), 0.f, 1.f);
        }
        return -1.f;
    }

    std::string clock_text(double seconds) {
        int total = static_cast<int>(std::max(seconds, 0.0) + 0.5);
        if (total >= 3600)
            return fmt::format("{}:{:02}:{:02}", total / 3600, total / 60 % 60, total % 60);
        return fmt::format("{}:{:02}", total / 60, total % 60);
    }
}

RenderStatus Logic::render_status() {
    auto& film = Recorder::get();
    RenderStatus status;
    if (!film.active)
        return status;
    status.active = true;
    if (film.paused) {
        status.text = "Render paused";
        return status;
    }
    auto* play = PlayLayer::get();
    double video = (double)(film.frames) * (double)(Logic::get().render_step());
    if (film.busy_seconds > 0.05)
        status.speed = (float)(video / film.busy_seconds);
    // the pace of the last second or so, smoothed. the time left uses ts, NOT the average since the start
    if (film.busy_seconds < film.pace_wall || video < film.pace_video)
        film.pace_wall = film.pace_video = film.pace = 0.0;
    if (film.busy_seconds - film.pace_wall >= 1.0) {
        double fresh = (video - film.pace_video) / (film.busy_seconds - film.pace_wall);
        film.pace = film.pace > 0.0 ? film.pace * 0.7 + fresh * 0.3 : fresh;
        film.pace_wall = film.busy_seconds;
        film.pace_video = video;
    }
    double pace = film.pace > 0.0 ? film.pace : (double)(status.speed);
    std::string speed = pace > 0.0 ? fmt::format("{:.2f}x", pace) : std::string("measuring speed");
    std::string elapsed = clock_text(film.busy_seconds) + " elapsed";
    if (play && play->m_hasCompletedLevel) {
        status.known = true;
        status.finishing = true;
        status.progress = 1.f;
        status.text = fmt::format("Finishing  |  {}  |  {}", speed, elapsed);
        return status;
    }
    // how much level is left in SECONDS (gd works it out from the speed portals), not in distance.
    // by distance a 4x speed part counted the same as a half speed part & the time left jumped around
    double left_game = -1.0;
    if (play && !play->m_isPlatformer && play->m_player1 && play->m_endPortal) {
        int channel = play->m_gameState.m_currentChannel;
        double end = play->timeForPos(play->m_endPortal->getPosition(), 0, channel, true, 0);
        double here = play->timeForPos(play->m_player1->getPosition(), 0, channel, true, 0);
        if (std::isfinite(end) && std::isfinite(here) && end > 0.0)
            left_game = std::max(end - here, 0.0);
    }
    double game_speed = std::max((double)(Logic::get().render_speed()), 0.01);
    double tail = (double)(Logic::get().render_after_end);
    if (left_game >= 0.0) {
        double left_video = left_game / game_speed + tail;
        double whole = video + left_video;
        status.known = true;
        status.progress = whole > 0.0 ? (float)(std::clamp(video / whole, 0.0, 1.0)) : 0.f;
        std::string left = "estimating";
        if (pace > 0.0 && film.busy_seconds > 1.0) {
            status.eta = (float)(left_video / pace);
            left = clock_text(status.eta) + " left";
        }
        status.text = fmt::format("{:.1f}%  |  {}  |  {}  |  {}", status.progress * 100.f, speed, elapsed, left);
        return status;
    }
    float now = lvl_fraction(play, film.last_input_frame);
    if (now < 0.f) {
        status.text = fmt::format("{} rendered  |  {}  |  {}", clock_text(video), speed, elapsed);
        return status;
    }
    status.known = true;
    status.progress = now;
    float done = now - std::max(film.start_fraction, 0.f);
    std::string left = "estimating";
    if (done > 0.002f && film.busy_seconds > 0.5) {
        double after = pace > 0.0 ? tail / pace : 0.0;
        status.eta = (float)(film.busy_seconds * (1.0 - now) / done + after);
        left = clock_text(status.eta) + " left";
    }
    status.text = fmt::format("{:.1f}%  |  {}  |  {}  |  {}", now * 100.f, speed, elapsed, left);
    return status;
}

float Logic::render_wall_delta() {
    return Recorder::get().wall_delta;
}

void Logic::sync_render_pause_button() {
    static CCNode* hidden = nullptr;
    auto* play = PlayLayer::get();
    CCNode* button = play && play->m_uiLayer ? play->m_uiLayer->m_pauseBtn : nullptr;
    bool hide = button && render_hide_pause && Recorder::get().active;
#ifndef GEODE_IS_MOBILE
    hide = false;
#endif
    if (hidden && hidden != button)
        hidden = nullptr;
    if (hide && !hidden && button->isVisible()) {
        button->setVisible(false);
        hidden = button;
    }
    else if (!hide && hidden) {
        hidden->setVisible(true);
        hidden = nullptr;
    }
}

bool Logic::render_uses_screen_shape() {
#if defined(GEODE_IS_IOS)
    // ios keeps the screen shape no matter what i tell it :/
    return true;
#elif defined(GEODE_IS_MOBILE)
    return render_screen_shape;
#else
    return false;
#endif
}

int Logic::render_width_for(int width, int height) {
    if (!render_uses_screen_shape())
        return width;
    auto* view = CCEGLView::get();
    if (!view)
        return width;
    auto frame = view->getFrameSize();
    if (Recorder::get().holding)
        frame = Recorder::get().saved_frame;
    if (frame.width <= 1.f || frame.height <= 1.f)
        return width;
    float wide = std::max(frame.width, frame.height);
    float tall = std::min(frame.width, frame.height);
    return std::max(static_cast<int>(std::lround((float)(height) * wide / tall)), 2) & ~1;
}

bool Logic::render_active() {
    return Recorder::get().active || Recorder::get().starting;
}

bool Logic::render_capturing() {
    return Recorder::get().active;
}

bool Logic::render_paused() {
    return Recorder::get().active && Recorder::get().paused;
}

bool Logic::render_holds_view() {
    return Recorder::get().holding;
}

bool Logic::render_blocks_engine() {
    return AudioTap::get().attached && !AudioTap::get().engine_open;
}

bool Logic::render_blocks_mixer() {
    return AudioTap::get().attached && !AudioTap::get().mixing;
}

bool Logic::render_should_skip() {
    auto* play = PlayLayer::get();
    if (!play || Logic::get().render_paused())
        return true;
    if (Logic::get().pause_menu_open())
        return true;
    bool started = play->m_started || (Logic::get().render_record_pause && Recorder::get().seen_frames >= 2);
    if (!started)
        return true;
    if (play->m_isPaused)
        return true;
    return false;
}

bool Logic::begin_render_frame() {
    auto& film = Recorder::get();
    if (!film.active)
        return false;
    auto now = chrono::steady_clock::now();
    double gap = film.ticked ? chrono::duration<double>(now - film.last_tick).count() : 0.0;
    film.last_tick = now;
    film.ticked = true;
    film.wall_delta = (float)(std::clamp(gap, 0.0, 0.25));
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &film.screen_fbo);
    ++film.seen_frames;
    film.hold_view();
    bool capture = !Logic::get().render_should_skip();
    if (capture) {
        film.busy_seconds += film.wall_delta;
        AudioTap::get().hold_effects();
    }
    return capture;
}

void Logic::hold_render_effects() {
    AudioTap::get().hold_effects();
}

void Logic::show_render_overlay() {
    Recorder::get().show_overlay();
}

bool Logic::capture_frame() {
    auto& film = Recorder::get();
    auto* play = PlayLayer::get();
    if (!film.active || !play)
        return false;
    auto const& logic = Logic::get();
    float dt = Logic::get().render_step();
    if (play->m_hasCompletedLevel) {
        film.endTime += dt;
        if (film.endTime >= logic.render_after_end) {
            bool leave = logic.render_auto_exit;
            Logic::get().stop_render();
            if (leave) {
                Loader::get()->queueInMainThread([] {
                    if (auto* layer = PlayLayer::get())
                        layer->onQuit();
                });
            }
            return false;
        }
    }
    float fadeIn = logic.render_fade_in <= 0.f ? 1.f : std::clamp((float)(film.frames) * dt / logic.render_fade_in, 0.f, 1.f);
    float fadeOut = 1.f;
    if (play->m_hasCompletedLevel && logic.render_fade_out > 0.f) {
        fadeOut = std::clamp((logic.render_after_end - film.endTime) / logic.render_fade_out, 0.f, 1.f);
    }
    film.fade = std::min(fadeIn, fadeOut);
    if (film.start_fraction < 0.f)
        film.start_fraction = lvl_fraction(play, film.last_input_frame);
    AudioTap::get().advance(dt * film.audio_speed, film.fade);
    film.drawFrame();
    if (film.frames % (std::max(logic.render_fps, 1) * 5) == 0 && g_render_notes.size() < 60000) {
        auto* engine = FMODAudioEngine::get();
        auto& tap = AudioTap::get();
        unsigned raw_song = 0;
        if (auto* channel = engine ? engine->getActiveMusicChannel(0) : nullptr)
            channel->getPosition(&raw_song, FMOD_TIMEUNIT_MS);
        g_render_notes += fmt::format("at video={:.3f}s level_time={:.4f}s song={}ms raw_song={}ms audio_written={:.3f}s x={:.3f} tps={:g} step_frame={}\n",
            static_cast<double>(film.frames) * dt, play->m_gameState.m_levelTime, engine ? engine->getMusicTimeMS(0) : 0, raw_song,
            tap.rate > 0 ? static_cast<double>(tap.written) / tap.rate : 0.0, play->m_player1 ? play->m_player1->getPositionX() : 0.f,
            logic.sample_rate(), logic.step_frame);
    }
    ++film.frames;
    if (film.broken) {
        std::string reason = film.use_api ? "Encoding failed: " + film.failure : std::string("The render pipe closed");
#ifdef GEODE_IS_MACOS
        if (!film.use_api && !film.failure.empty())
            reason = "Encoding failed: " + film.failure;
#endif
        Logic::get().stop_render();
        Logic::get().notify(reason, false);
    }
    return true;
}

bool Logic::render_batch_more(chrono::steady_clock::time_point started) {
#ifdef GEODE_IS_WINDOWS
    return false;
#else
    return Recorder::get().active && !Logic::get().render_should_skip() && chrono::steady_clock::now() - started < chrono::milliseconds(40);
#endif
}

bool Logic::ffmpeg_ready() {
#ifdef GEODE_IS_WINDOWS
    return fs::exists(Recorder::get().game_directory() / "ffmpeg.exe") || Recorder::api_available();
#elif defined(GEODE_IS_MACOS)
    static chrono::steady_clock::time_point checked{};
    static bool it = false;
    auto now = chrono::steady_clock::now();
    if (checked == chrono::steady_clock::time_point{} || now - checked > chrono::seconds(2)) {
        it = !Recorder::ffmpeg_path().empty();
        checked = now;
    }
    return it || Recorder::api_available();
#else
    return Recorder::api_available();
#endif
}

void Logic::stop_render() {
    auto& film = Recorder::get();
    if (film.active && film.frames > 0) {
        double frames = (double)(film.frames);
        double wall = chrono::duration<double>(chrono::steady_clock::now() - g_timing.began).count();
        auto each = [frames](std::atomic<long long> const& ns) {
            return (double)(ns.load()) / 1e6 / frames;
        };
        // where the time went & whether the song stayed with the level. logs/render.txt, written over by every render
        auto& logic = Logic::get();
        auto& tap = AudioTap::get();
        std::error_code error;
        fs::create_directories(logic.save_root() / "logs", error);
        std::ofstream out(logic.save_root() / "logs" / "render.txt", std::ios::trunc);
        out << fmt::format("render {}x{} @{}fps codec={} platform={} capture={}\n", film.width, film.height, logic.render_fps, logic.render_codec,
            GEODE_PLATFORM_NAME, film.use_api ? "api" : film.nv12 ? "nv12 ring" : "bgra direct");
        out << fmt::format("frames={} video={:.3f}s wall={:.3f}s speed={:.2f} fps ({:.2f}x real time)\n", film.frames, frames / std::max(logic.render_fps, 1), wall,
            frames / std::max(wall, 1e-6), frames / std::max(logic.render_fps, 1) / std::max(wall, 1e-6));
        out << fmt::format("per frame: total={:.2f}ms read_back={:.2f}ms waiting_on_encoder={:.2f}ms encoder_write={:.2f}ms (encoder is on its own thread)\n",
            wall * 1000.0 / frames, each(g_timing.read_ns), each(g_timing.wait_ns), each(g_timing.encode_ns));
        out << fmt::format("ssb_fix={} factor low={:.6f} high={:.6f} mean={:.6f} samples={} tps={:g} render_speed={:g}\n", logic.render_ssb_fix, logic.render_factor_low,
            logic.render_factor_high, logic.render_factor_count > 0 ? logic.render_factor_sum / logic.render_factor_count : 1.0, logic.render_factor_count,
            logic.sample_rate(), film.audio_speed);
        out << fmt::format("audio rate={} channels={} written={} ({:.3f}s) stray_blocks={}\n", tap.rate, tap.channels, tap.written,
            tap.rate > 0 ? static_cast<double>(tap.written) / tap.rate : 0.0, tap.stray_blocks);
        {
            auto* play = PlayLayer::get();
            auto* engine = FMODAudioEngine::get();
            auto* manager = GameManager::get();
            out << fmt::format("level song_offset={:.4f}s gd_music_offset={}ms gd_time_offset={:.4f} song_at_start={}ms\n",
                play && play->m_levelSettings ? play->m_levelSettings->m_songOffset : -1.f, engine ? engine->m_musicOffset : 0,
                manager ? manager->m_timeOffset : 0.f, tap.song_at_start);
        }
        out << g_render_notes;
    }
#ifdef GEODE_IS_WINDOWS
    if (film.starting && !film.active) {
        film.cancel = true;
        if (film.video != INVALID_HANDLE_VALUE)
            CancelIoEx(film.video, nullptr);
        Logic::get().message = "Render canceled";
        return;
    }
#endif
    if (film.idle())
        return;
    film.starting = false;
    film.cancel = false;
    film.active = false;
    Logic::get().render_speed_locked = 0.f;
    film.restore_pace();
    AudioTap::get().detach();
    film.finish_video();
    film.close_target();
    film.release_view();
    if (film.use_api) {
        film.save_api_render();
        return;
    }
#ifdef GEODE_IS_WINDOWS
    bool muxed = film.mux_audio();
    if (muxed)
        Logic::get().message = "Render stopped";
#endif
#ifdef GEODE_IS_MACOS
    if (!film.encode_error.empty()) {
        Logic::get().message = "Render failed: " + film.encode_error;
        Logic::get().notify(Logic::get().message, false);
    }
    else if (film.mux_audio()) {
        Logic::get().message = "Render stopped";
        if (film.frames > 0)
            Logic::get().notify(fmt::format("Saved {}", film.output.filename().string()), true);
    }
#endif
}

void Logic::start_render() {
    auto& film = Recorder::get();
    if (film.active || film.starting)
        return;
    if (film.saving) {
        Logic::get().notify("Still saving the last render", false);
        return;
    }
    auto* play = PlayLayer::get();
    auto* director = CCDirector::sharedDirector();
    auto* view = director ? director->getOpenGLView() : nullptr;
    if (!play || !view) {
        Logic::get().render_queued = true;
        Logic::get().notify("Rendering starts when you enter a level", true);
        return;
    }
    auto& logic = Logic::get();
    int height = std::max(logic.render_height, 2) & ~1;
    int width = std::max(Logic::get().render_width_for(logic.render_width, height), 2) & ~1;
    if (width > 7680 || height > 4320) {
        Logic::get().notify("Render size is too large. Use 7680x4320 or smaller.", false);
        return;
    }
    int fps = std::max(logic.render_fps, 1);
    std::string ext = logic.render_extension.empty() ? "mp4" : logic.render_extension;
    if (ext.front() == '.')
        ext.erase(ext.begin());
    logic.render_speed_locked = 0.f;
    film.audio_speed = Logic::get().render_speed();
    auto folder = Logic::get().save_root() / "renders";
    std::error_code folder_error;
    fs::create_directories(folder, folder_error);
    if (folder_error || !fs::is_directory(folder, folder_error)) {
        Logic::get().notify("Could not create the renders folder", false);
        return;
    }
    auto output = ReplayIO::unique_file(folder, ReplayIO::sanitize(Logic::get().render_file_stem()), "." + ext);
    film.width = width;
    film.height = height;
    film.use_api = Recorder::prefer_api();
#ifdef ECHO_FFMPEG_API
    if (film.use_api && !Recorder::api_available()) {
        logic.ffmpeg_missing = true;
        logic.message = "FFmpeg API is not installed";
        Logic::get().notify("Rendering needs the FFmpeg API mod. Install it from the Geode mod index.", false);
        return;
    }
#endif
    if (!film.open_target()) {
        film.close_target();
        Logic::get().notify("Could not create the render target", false);
        return;
    }
    auto arm = [&] {
        film.output = output;
        film.audio = folder / (output.stem().string() + ".audio.raw");
        film.paused = false;
        film.frames = 0;
        g_timing.reset();
        g_render_notes.clear();
        film.pace_video = film.pace_wall = film.pace = 0.0;
        {
            auto& logic = Logic::get();
            logic.render_game_seconds = 0.0;
            logic.render_factor_sum = 0.0;
            logic.render_factor_low = logic.render_factor_high = 1.f;
            logic.render_factor_count = 0;
        }
        film.seen_frames = 0;
        film.endTime = 0.f;
        film.fade = 1.f;
        film.cancel = false;
        film.broken = false;
        film.failure.clear();
        film.ticked = false;
        film.wall_delta = 0.f;
        film.busy_seconds = 0.0;
        film.start_fraction = -1.f;
        film.last_input_frame = logic.inputs.empty() ? 0 : std::ranges::max(logic.inputs, {}, &Frame::number).number;
        logic.ffmpeg_missing = false;
        logic.render_queued = false;
        logic.frame_advance = false;
        logic.holding_advance = false;
        logic.advance_queued = false;
    };
#ifdef ECHO_FFMPEG_API
    if (film.use_api) {
        film.output = output;
        if (auto failed = film.open_api(fps); !failed.empty()) {
            film.close_target();
            logic.message = "FFmpeg API failed: " + failed;
            Logic::get().notify(logic.message, false);
            return;
        }
        arm();
        film.go_live();
        return;
    }
#endif
#ifdef GEODE_IS_WINDOWS
    DWORD id = GetTickCount();
    std::string video_name = fmt::format("\\\\.\\pipe\\echo-film-v-{}", id);
    DWORD pipe_mode = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED;
    size_t frame_bytes = film.frame_bytes();
    DWORD video_cap = static_cast<DWORD>(std::min(std::max(frame_bytes * 4, frame_bytes), static_cast<size_t>(256u << 20)));
    film.video = CreateNamedPipeA(video_name.c_str(), pipe_mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, video_cap, 0, 0, nullptr);
    if (film.video == INVALID_HANDLE_VALUE) {
        film.close_target();
        Logic::get().notify("Could not open the render pipe", false);
        return;
    }
    std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
    bool hardware = codec.find("nvenc") != std::string::npos || codec.find("amf") != std::string::npos || codec.find("qsv") != std::string::npos;
    char const* out_pix = logic.render_bitrate <= 0 && !hardware ? "yuv444p" : "yuv420p";
    // shader already did bt709 DONT do it twice
    std::string color_fix = logic.render_color_fix && !film.nv12 ? "-vf colorspace=all=bt709:iall=bt470bg:fast=1 " : "";
    auto has_arg = [&](char const* key) {
        return logic.render_args.find(key) != std::string::npos;
    };
    std::string pix = has_arg("-pix_fmt") ? std::string() : fmt::format(" -pix_fmt {}", out_pix);
    std::string color = has_arg("-colorspace") || has_arg("-color_primaries") || has_arg("-color_trc") || has_arg("-color_range")
                            ? std::string()
                            : " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
    std::string extra = logic.render_args.empty() ? std::string() : " " + logic.render_args;
    std::string command = fmt::format(
        "\"{}\" -y -nostdin -loglevel error -f rawvideo -pix_fmt {} -s {}x{} -r {} -probesize 32 -analyzeduration 0 -fflags nobuffer -i {} -an "
        "{}-c:v {}{}{}{}{} \"{}\"",
        Recorder::get().ffmpeg_path(),
        film.nv12 ? "nv12" : "bgra",
        width,
        height,
        fps,
        video_name,
        color_fix,
        codec,
        video_rate(codec, logic.render_bitrate, logic.render_args),
        pix,
        color,
        extra,
        output.string());
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    if (!CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        film.close_target();
        Recorder::get().close_handle(film.video);
        Logic::get().ffmpeg_missing = true;
        Logic::get().message = "ffmpeg.exe was not found";
        Logic::get().notify("ffmpeg.exe was not found. Use the download link on the Render tab, then place ffmpeg.exe next to Geometry Dash.", false);
        GUI::get().reveal_menu();
        return;
    }
    CloseHandle(process.hThread);
    film.process = process.hProcess;
    arm();
    film.starting = true;
    logic.message = "Starting render...";
    std::thread([] {
        bool ok = Recorder::get().connect_pipe(Recorder::get().video);
        Loader::get()->queueInMainThread([ok] {
            auto& film = Recorder::get();
            bool canceled = film.cancel;
            film.starting = false;
            film.cancel = false;
            if (!ok || canceled) {
                Logic::get().stop_render();
                if (!ok && !canceled) {
                    Logic::get().message = "ffmpeg did not connect";
                    Logic::get().notify("ffmpeg did not connect", false);
                }
                return;
            }
            if (!PlayLayer::get()) {
                Logic::get().stop_render();
                Logic::get().notify("The level closed before the render started", false);
                return;
            }
            film.go_live();
        });
    }).detach();
#elif defined(GEODE_IS_MACOS)
    auto ffmpeg = Recorder::ffmpeg_path();
    if (ffmpeg.empty()) {
        film.close_target();
        logic.ffmpeg_missing = true;
        logic.message = "ffmpeg was not found";
        Logic::get().notify("ffmpeg was not found. Install it with Homebrew (brew install ffmpeg), then render again.", false);
        GUI::get().reveal_menu();
        return;
    }
    std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
    bool hardware = codec.find("videotoolbox") != std::string::npos;
    auto has_arg = [&](char const* key) {
        return logic.render_args.find(key) != std::string::npos;
    };
    std::string filters = "vflip";
    if (logic.render_color_fix)
        filters += ",colorspace=all=bt709:iall=bt470bg:fast=1";
    std::vector<std::string> args{
        ffmpeg.string(), "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", film.nv12 ? "nv12" : "bgra",
        "-s", fmt::format("{}x{}", width, height), "-r", std::to_string(fps), "-i", "pipe:0", "-an"
    };
    // nv12 frames come out of the shader the right way up & already bt709, so ffmpeg gets no filter at all
    if (!film.nv12)
        args.insert(args.end(), { "-vf", filters });
    args.insert(args.end(), { "-c:v", codec });
    auto rate = video_rate(codec, logic.render_bitrate, logic.render_args, static_cast<int64_t>(width) * height * fps);
    args.insert(args.end(), rate.begin(), rate.end());
    if (!has_arg("-pix_fmt"))
        args.insert(args.end(), { "-pix_fmt", logic.render_bitrate <= 0 && !hardware ? "yuv444p" : "yuv420p" });
    if (!has_arg("-colorspace") && !has_arg("-color_primaries") && !has_arg("-color_trc") && !has_arg("-color_range"))
        args.insert(args.end(), { "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv" });
    auto extra = split_args(logic.render_args);
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back(output.string());
    int ends[2] = { -1, -1 };
    if (pipe(ends) != 0) {
        film.close_target();
        Logic::get().notify("Could not open the render pipe", false);
        return;
    }
    fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETFD, FD_CLOEXEC);
    fcntl(ends[1], F_SETNOSIGPIPE, 1);
    film.video_log = folder / (output.stem().string() + ".ffmpeg.log");
    pid_t pid = spawn_tool(args, ends[0], {}, film.video_log);
    close(ends[0]);
    if (!pid) {
        close(ends[1]);
        film.close_target();
        Logic::get().notify("Could not start ffmpeg", false);
        return;
    }
    film.process = pid;
    film.video = ends[1];
    film.encode_error.clear();
    arm();
    film.go_live();
#else
    film.close_target();
#endif
}
