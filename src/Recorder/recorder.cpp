#include "Logic/build_info.hpp"
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
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined(GEODE_IS_ANDROID) && defined(ECHO_FFMPEG_API)
struct AVCodecContext;
struct AVCodec;
struct AVDictionary;
struct SwsContext;
struct SwsFilter;
using OpenCodecFunction = int (*)(AVCodecContext*, const AVCodec*, AVDictionary**);
using SetDictionaryFunction = int (*)(AVDictionary**, const char*, const char*, int);
using FreeDictionaryFunction = void (*)(AVDictionary**);
using GetSwsContextFunction = SwsContext* (*)(int, int, int, int, int, int, int, SwsFilter*, SwsFilter*, const double*);
using SetSwsColorspaceFunction = int (*)(SwsContext*, const int*, int, const int*, int, int, int, int);
using GetSwsCoefficientsFunction = const int* (*)(int);
static OpenCodecFunction g_open_codec = nullptr;
static SetDictionaryFunction g_set_dictionary = nullptr;
static FreeDictionaryFunction g_free_dictionary = nullptr;
static thread_local const char* g_encoder_preset = nullptr;
// api forgets a flag mp4 needs :/
static thread_local bool g_force_global_header = false;
// color fix on android. the api's colorspace filter cant take nv12 (what mediacodec gets) so it never worked,
// instead the rgba -> yuv conversion itself uses bt709 & the video gets tagged bt709, same as mac ends up w/
static thread_local bool g_force_bt709_colors = false;
static GetSwsContextFunction g_get_sws_context = nullptr;
static SetSwsColorspaceFunction g_set_sws_colorspace = nullptr;
static GetSwsCoefficientsFunction g_get_sws_coefficients = nullptr;

static SwsContext* hooked_get_sws_context(int srcW, int srcH, int srcFormat, int dstW, int dstH, int dstFormat, int flags, SwsFilter* srcFilter,
    SwsFilter* dstFilter, const double* param) {
    auto* context = g_get_sws_context(srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter, dstFilter, param);
    if (context && g_force_bt709_colors && g_set_sws_colorspace && g_get_sws_coefficients) {
        const int ITU709 = 1;
        const auto* bt709 = g_get_sws_coefficients(ITU709);
        g_set_sws_colorspace(context, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    }
    return context;
}

static int hooked_open_codec(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    AVDictionary* own_options = nullptr;
    AVDictionary** options_used = options ? options : &own_options;
    if (g_encoder_preset) g_set_dictionary(options_used, "preset", g_encoder_preset, 0);
    if (g_force_global_header) g_set_dictionary(options_used, "flags", "+global_header", 0);
    if (g_force_bt709_colors) {
        g_set_dictionary(options_used, "colorspace", "bt709", 0);
        g_set_dictionary(options_used, "color_primaries", "bt709", 0);
        g_set_dictionary(options_used, "color_trc", "bt709", 0);
        g_set_dictionary(options_used, "color_range", "tv", 0);
    }
    int result = g_open_codec(context, codec, options_used);
    if (own_options) g_free_dictionary(&own_options);
    return result;
}

static void install_encoder_options() {
    static bool codec_hooked = false;
    if (codec_hooked) return;
    auto* ffmpeg_mod = Loader::get()->getLoadedMod("eclipse.ffmpeg-api");
    if (!ffmpeg_mod) return;
    void* ffmpeg_library = dlopen(ffmpeg_mod->getBinaryPath().c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!ffmpeg_library) return;
    g_open_codec = reinterpret_cast<OpenCodecFunction>(dlsym(ffmpeg_library, "avcodec_open2"));
    g_set_dictionary = reinterpret_cast<SetDictionaryFunction>(dlsym(ffmpeg_library, "av_dict_set"));
    g_free_dictionary = reinterpret_cast<FreeDictionaryFunction>(dlsym(ffmpeg_library, "av_dict_free"));
    if (g_open_codec && g_set_dictionary && g_free_dictionary) {
        auto hook = Mod::get()->hook(reinterpret_cast<void*>(g_open_codec), &hooked_open_codec, "Echo encoder preset");
        codec_hooked = hook.isOk();
    }
    g_get_sws_context = reinterpret_cast<GetSwsContextFunction>(dlsym(ffmpeg_library, "sws_getContext"));
    g_set_sws_colorspace = reinterpret_cast<SetSwsColorspaceFunction>(dlsym(ffmpeg_library, "sws_setColorspaceDetails"));
    g_get_sws_coefficients = reinterpret_cast<GetSwsCoefficientsFunction>(dlsym(ffmpeg_library, "sws_getCoefficients"));
    static bool colors_hooked = false;
    if (!colors_hooked && g_get_sws_context && g_set_sws_colorspace && g_get_sws_coefficients)
        colors_hooked = Mod::get()->hook(reinterpret_cast<void*>(g_get_sws_context), &hooked_get_sws_context, "Echo color fix").isOk();
    dlclose(ffmpeg_library);
}
#endif

struct RenderTiming {
    std::atomic<long long> read_back_ns{ 0 }, encoder_wait_ns{ 0 }, encoder_write_ns{ 0 };
    chrono::steady_clock::time_point started_at{};

    void reset() {
        read_back_ns = 0;
        encoder_wait_ns = 0;
        encoder_write_ns = 0;
        Logic::get().render_physics = 0.0;
        started_at = chrono::steady_clock::now();
    }
};

static RenderTiming g_render_timing;
static std::string g_render_notes;

struct ScopedTimer {
    std::atomic<long long>& total_ns;
    chrono::steady_clock::time_point started_at = chrono::steady_clock::now();

    ~ScopedTimer() {
        total_ns += chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now() - started_at).count();
    }
};

class AudioCapture {
public:
    static AudioCapture& get() {
        static AudioCapture value;
        return value;
    }

    bool attach(float music, float sfx, const fs::path& file);
    void detach();
    void advance(float dt, float fade);
    void keep_effects_playing();
    static FMOD_RESULT F_CALLBACK dsp_read(FMOD_DSP_STATE* state, float* in, float* out, unsigned int length, int in_channels, int* out_channels);

    bool attached = false;
    bool engine_update_allowed = false;
    bool mixer_allowed = false;
    int sample_rate = 0;
    int channels = 0;
    unsigned long long frames_written = 0;
    unsigned song_start_ms = 0;
    unsigned long long stray_blocks = 0;

private:
    FMOD::DSP* capture_dsp = nullptr;
    FMOD::ChannelGroup* master_group = nullptr;
    std::vector<std::pair<FMOD::ChannelGroup*, FMOD::ChannelGroup*>> original_parent_groups;
    std::mutex lock;
    std::vector<float> pending_samples;
    std::ofstream raw_audio_file;
    double captured_seconds = 0.0;
    float saved_music_volume = 1.f, saved_sfx_volume = 1.f;
    FMOD_OUTPUTTYPE saved_output_type = FMOD_OUTPUTTYPE_AUTODETECT;
    int speaker_channels = 2;
    float effects_volume = 1.f;
    double last_song_position = 0.0;
    unsigned long long frames_mixed_at_last_check = 0;
    bool song_tracked = false;
    std::unordered_set<std::string> undecodable_paths;
    void attach_to_master_group();
    void keep_song_on_time(FMODAudioEngine* engine);
    void convert_streams_to_samples(FMODAudioEngine* engine);
    void freeze(bool frozen);
};

class Recorder {
public:
    static Recorder& get() {
        static Recorder value;
        return value;
    }
    ~Recorder() {
        if (encode_thread.joinable()) encode_thread.detach();
    }
#ifdef GEODE_IS_WINDOWS
    void close_handle(HANDLE& handle);
    bool connect_pipe(HANDLE pipe);
    bool write_all(HANDLE pipe, const void* data, size_t size);
    fs::path game_directory();
    fs::path ffmpeg_path();
    std::string run_hidden(std::string command, const fs::path& log_path);
    bool setup_nv12();
    void create_nv12_targets();
    void close_nv12();
    void convert_nv12();
    void draw_nv12_plane(GLuint framebuffer, GLuint program, int plane_width, int plane_height);
    void draw_full_quad();
    void read_into_pixel_buffer();
    void read_nv12_planes();
    bool wait_and_delete_fence(int slot, bool wait);
    bool mux_audio();
#endif
#ifdef GEODE_IS_MACOS
    static fs::path ffmpeg_path();
    bool write_all(int fd, const void* data, size_t size);
    bool mux_audio();
#endif
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    void open_gpu_readback();
    void close_gpu_readback();
    void copy_pixel_buffer_to_queue(int slot);
    void close_ffmpeg_process();
#endif
#ifdef ECHO_FFMPEG_API
    std::string open_api_encoder(int fps);
    bool send_to_api_encoder(const std::vector<unsigned char>& frame);
#endif
    static bool api_available();
    static bool prefer_api();
    bool idle() const;
    void start_capturing();
    const char* rendering_message() const;
    bool send_to_encoder(const std::vector<unsigned char>& frame);
    void save_api_render();
    void silent_change_size(CCSize size);
    void resize_shader_layer(CCSize size);
    std::optional<CCSize> window_pixels();
    void hold_view();
    void release_view();
    bool open_capture_target();
    bool create_capture_framebuffer();
    void create_overlay_sprites();
    void close_capture_target();
    void release_overlay_sprites();
    void draw_and_read_frame();
    void draw_scene_into_capture(CCDirector* director);
    void read_into_frame();
    void show_overlay();
    void draw_preview(CCSize pixels);
#ifdef GEODE_IS_IOS
    bool draw_direct_frame();
    GLuint direct_fbo = 0;
    bool direct_failed = false;
#endif
    void draw_progress();
    void queue_frame(std::vector<unsigned char> frame);
    std::vector<unsigned char> acquire_frame();
    void encode_loop();
    bool take_queued_frame(std::vector<unsigned char>& frame);
    void recycle_frame(std::vector<unsigned char>& frame);
    void stop_encode_thread();
    void finish_video();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    bool collect_pixel_buffer(bool wait);
#endif
#ifdef GEODE_IS_MACOS
    bool capture_bgra_buffered();
#endif
    size_t frame_bytes() const;
    void uncap_frame_rate();
    void restore_frame_rate();

    static const int READBACK_RING_SIZE = 6;
#ifdef GEODE_IS_WINDOWS
    static const size_t MAX_QUEUED_FRAMES = 12;
#else
    static const size_t MAX_QUEUED_FRAMES = 4;
#endif
    static const size_t MAX_SPARE_FRAMES = MAX_QUEUED_FRAMES + 1;

    bool active = false;
    bool paused = false;
    bool starting = false;
    bool cancel = false;
    bool view_held = false;
    bool use_api = false;
    bool saving = false;
    std::atomic<bool> encoding_broken = false;
    std::string failure_reason;
#ifdef GEODE_IS_WINDOWS
    HANDLE video_pipe = INVALID_HANDLE_VALUE;
    HANDLE ffmpeg_process = nullptr;
#endif
#ifdef ECHO_FFMPEG_API
    std::unique_ptr<ffmpeg::events::Recorder> api_encoder;
#endif
#ifdef GEODE_IS_MACOS
    pid_t ffmpeg_process = 0;
    int video_pipe = -1;
    fs::path ffmpeg_log;
    std::string encode_error;
#endif
    GLint screen_fbo = 0;
    fs::path output;
    fs::path raw_audio_path;
    int width = 0, height = 0;
    int frames = 0, seen_frames = 0;
    float time_since_end = 0.f;
    float fade = 1.f, video_fade = 1.f;
    float audio_speed = 1.f;
    CCSize saved_frame_size{}, saved_window_pixels{};
    float saved_win_width = 0.f, ui_shift = 0.f;
    struct SavedShaderLayer {
        ShaderLayer* layer = nullptr;
        CCRenderTexture* texture = nullptr;
        CCRect texture_rect;
        float scale_factor = 1.f, aspect_ratio = 1.f, texture_scale_x = 1.f, texture_scale_y = 1.f;
        CCSize screen_size, texture_content_size, target_texture_size, target_texture_size_extra;
    } saved_shader;
    PlayLayer* shifted_play_layer = nullptr;
    void shift_ui(PlayLayer* play, float distance);

    GLuint capture_fbo = 0;
    GLuint capture_texture = 0;
    GLuint y_fbo = 0, uv_fbo = 0;
    GLuint y_texture = 0, uv_texture = 0;
    GLuint y_program = 0, uv_program = 0;
    bool nv12 = false;
    bool buffered_capture = false;
    int saved_swap_interval = 1;
    float saved_animation_interval = 1.f / 60.f;
    bool frame_rate_uncapped = false;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    std::array<GLuint, READBACK_RING_SIZE> pixel_buffers{};
    std::deque<int> buffers_in_flight;
    int next_slot = 0;
#endif
#ifdef GEODE_IS_WINDOWS
    std::array<GLsync, READBACK_RING_SIZE> fences{};
#endif
    CCLayerColor* fade_shade = nullptr;
    CCSprite* bar_background = nullptr;
    CCSprite* bar_fill = nullptr;
    CCLabelBMFont* bar_text = nullptr;
    std::string bar_text_shown;

    chrono::steady_clock::time_point last_tick{};
    bool ticked = false;
    float wall_delta = 0.f;
    double busy_seconds = 0.0;
    double recent_speed = 0.0;
    std::deque<std::pair<double, double>> speed_samples;
    chrono::steady_clock::time_point bar_text_updated_at{};
    CCSize real_frame_size();
    float start_progress = -1.f;
    int last_input_frame = 0;

    std::thread encode_thread;
    std::mutex lock;
    std::condition_variable queue_changed;
    std::deque<std::vector<unsigned char>> frame_queue;
    std::vector<std::vector<unsigned char>> spare_frames;
    bool closing = false;
};

FMOD_RESULT F_CALLBACK AudioCapture::dsp_read(FMOD_DSP_STATE*, float* in, float* out, unsigned int length, int in_channels, int* out_channels) {
    auto& audio_capture = AudioCapture::get();
    size_t sample_count = (size_t)length * (size_t)in_channels;
    std::memcpy(out, in, sample_count * sizeof(float));
    if (out_channels) *out_channels = in_channels;
    std::lock_guard guard(audio_capture.lock);
    if (!audio_capture.mixer_allowed) {
        if (audio_capture.attached) ++audio_capture.stray_blocks;
        return FMOD_OK;
    }
    audio_capture.channels = in_channels;
    audio_capture.pending_samples.insert(audio_capture.pending_samples.end(), in, in + sample_count);
    return FMOD_OK;
}

void AudioCapture::attach_to_master_group() {
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system || !capture_dsp) return;
    FMOD::ChannelGroup* current_master = nullptr;
    if (engine->m_system->getMasterChannelGroup(&current_master) != FMOD_OK || !current_master) return;
    if (current_master != master_group) {
        // DONT leave it frozen
        if (master_group && attached) master_group->setPaused(false);
        if (master_group) master_group->removeDSP(capture_dsp);
        if (current_master->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, capture_dsp) == FMOD_OK) master_group = current_master;
    }
    if (!master_group) return;
    auto move_under_master = [&](FMOD::ChannelGroup* group) {
        if (!group || group == master_group) return;
        group->setPaused(false);
        master_group->addGroup(group, true, nullptr);
    };
    move_under_master(engine->m_backgroundMusicChannel);
    move_under_master(engine->m_globalChannel);
    if (engine->m_globalChannel && engine->m_reverbChannel)
        engine->m_globalChannel->addGroup(engine->m_reverbChannel, true, nullptr);
}

void AudioCapture::keep_effects_playing() {
    if (!attached) return;
    auto* engine = FMODAudioEngine::get();
    if (!engine) return;
    float volume = std::clamp(effects_volume, 0.f, 1.f);
    engine->setEffectsVolume(volume);
    if (engine->m_globalChannel) {
        engine->m_globalChannel->setVolume(volume);
        engine->m_globalChannel->setPaused(false);
    }
    if (engine->m_reverbChannel) engine->m_reverbChannel->setPaused(false);
}

bool AudioCapture::attach(float music, float sfx, const fs::path& file) {
    auto* engine = FMODAudioEngine::get();
    if (attached || !engine || !engine->m_system) return false;
    sample_rate = 0;
    channels = 0;
    frames_written = 0;
    auto* system = engine->m_system;
    FMOD_DSP_DESCRIPTION description{};
    description.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    std::strncpy(description.name, "Echo Render", sizeof(description.name) - 1);
    description.numinputbuffers = 1;
    description.numoutputbuffers = 1;
    description.read = &AudioCapture::dsp_read;
    if (system->createDSP(&description, &capture_dsp) != FMOD_OK || !capture_dsp) return false;
    if (system->getMasterChannelGroup(&master_group) != FMOD_OK || !master_group || master_group->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, capture_dsp) != FMOD_OK) {
        capture_dsp->release();
        capture_dsp = nullptr;
        return false;
    }
    gd::string song_path;
    unsigned song_ms = 0;
    if (auto it = engine->m_channelIDToSoundPath.find(0); it != engine->m_channelIDToSoundPath.end()) {
        song_path = it->second;
        song_ms = engine->getMusicTimeMS(0);
    }
    if (auto it = engine->m_fmodMusic.find(0); song_path.empty() && it != engine->m_fmodMusic.end() && !it->second.m_filePath.empty()) {
        song_path = it->second.m_filePath;
        song_ms = engine->getMusicTimeMS(0);
    }
    raw_audio_file.open(file, std::ios::binary | std::ios::trunc);
    system->getOutput(&saved_output_type);
    if (!raw_audio_file || system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT) != FMOD_OK) {
        raw_audio_file.close();
        master_group->removeDSP(capture_dsp);
        capture_dsp->release();
        capture_dsp = nullptr;
        system->setOutput(saved_output_type);
        return false;
    }
    FMOD_SPEAKERMODE speaker_mode = FMOD_SPEAKERMODE_STEREO;
    system->getSoftwareFormat(&sample_rate, &speaker_mode, nullptr);
    system->getSpeakerModeChannels(speaker_mode, &speaker_channels);
    if (!song_path.empty()) {
        engine->stopAndRemoveMusic(0);
        if (auto it = engine->m_fmodMusic.find(0); it != engine->m_fmodMusic.end() && it->second.m_sound) {
            FMOD_MODE mode = 0;
            it->second.m_sound->getMode(&mode);
            if (mode & FMOD_CREATESTREAM) {
                FMOD::Sound* sample = nullptr;
                FMOD_MODE sample_mode = (mode & ~(FMOD_MODE)(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
                auto result = system->createSound(it->second.m_filePath.c_str(), sample_mode, nullptr, &sample);
                // DONT free ts gd might still use it
                if (result == FMOD_OK && sample) it->second.m_sound = sample;
            }
        }
        engine->queueStartMusic(song_path, 1.f, 0.f, 1.f, false, (int)song_ms, 0, 0, 0, 0, false, 0, false, false);
        engine->updateQueuedMusic();
        for (int tries = 0; tries < 600 && !engine->getActiveMusicChannel(0); ++tries) {
            std::this_thread::sleep_for(chrono::milliseconds(5));
            system->update();
            engine->updateQueuedMusic();
        }
        convert_streams_to_samples(engine);
        // play once NOT loop
        auto* channel = engine->getActiveMusicChannel(0);
        if (channel) channel->setLoopCount(0);
        // jukebox (replaced songs) adds the songs start offset inside queueStartMusic. song_ms was read off the channel, so it
        // already had that offset in it & the song came back that much AHEAD for the whole render (0.4s on wndystein).
        // setting the spot on the channel itself goes around every hook
        if (channel) channel->setPosition(song_ms, FMOD_TIMEUNIT_MS);
        song_start_ms = song_ms;
        bool playing = false;
        if (channel) channel->isPlaying(&playing);
    }
    if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPaused(false);
    saved_music_volume = engine->getBackgroundMusicVolume();
    saved_sfx_volume = engine->getEffectsVolume();
    original_parent_groups.clear();
    for (auto* group : { engine->m_backgroundMusicChannel, engine->m_globalChannel, engine->m_reverbChannel }) {
        FMOD::ChannelGroup* parent = nullptr;
        if (group && group->getParentGroup(&parent) == FMOD_OK && parent)
            original_parent_groups.push_back({ group, parent });
    }
    effects_volume = std::clamp(sfx, 0.f, 1.f);
    engine->setBackgroundMusicVolume(std::clamp(music, 0.f, 1.f));
    attach_to_master_group();
    keep_effects_playing();
    pending_samples.clear();
    captured_seconds = 0.0;
    song_tracked = false;
    undecodable_paths.clear();
    stray_blocks = 0;
    attached = true;
    freeze(true);
    return true;
}

void AudioCapture::detach() {
    if (!attached) return;
    freeze(false);
    attached = false;
    engine_update_allowed = false;
    mixer_allowed = false;
    raw_audio_file.close();
    pending_samples.clear();
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system) return;
    if (master_group && capture_dsp) master_group->removeDSP(capture_dsp);
    if (capture_dsp) capture_dsp->release();
    capture_dsp = nullptr;
    master_group = nullptr;
    engine->setBackgroundMusicVolume(saved_music_volume);
    engine->setEffectsVolume(saved_sfx_volume);
    for (const auto& [group, parent] : original_parent_groups)
        parent->addGroup(group, true, nullptr);
    original_parent_groups.clear();
    engine->m_system->setOutput(saved_output_type);
}

// freezes all sound between frames
// without ts sfx play like 2-3x speed :/
void AudioCapture::freeze(bool frozen) {
    if (master_group) master_group->setPaused(frozen);
}

void AudioCapture::convert_streams_to_samples(FMODAudioEngine* engine) {
    auto* system = engine->m_system;
    std::vector<FMOD::Channel*> playing;
    auto add_channel = [&](FMOD::Channel* channel) {
        if (channel && std::ranges::find(playing, channel) == playing.end())
            playing.push_back(channel);
    };
    for (auto& [id, channel] : engine->m_channelIDToChannel)
        add_channel(channel);
    auto walk = [&](auto& self, FMOD::ChannelGroup* group, int depth) -> void {
        if (!group || depth > 8) return;
        int channel_count = 0;
        if (group->getNumChannels(&channel_count) == FMOD_OK) {
            for (int i = 0; i < channel_count; ++i) {
                FMOD::Channel* channel = nullptr;
                if (group->getChannel(i, &channel) == FMOD_OK) add_channel(channel);
            }
        }
        int groups = 0;
        if (group->getNumGroups(&groups) != FMOD_OK) return;
        for (int i = 0; i < groups; ++i) {
            FMOD::ChannelGroup* child = nullptr;
            if (group->getGroup(i, &child) == FMOD_OK) self(self, child, depth + 1);
        }
    };
    FMOD::ChannelGroup* root = nullptr;
    if (system->getMasterChannelGroup(&root) == FMOD_OK) walk(walk, root, 0);

    for (auto* channel : playing) {
        bool still_playing = false;
        FMOD::Sound* sound = nullptr;
        FMOD_MODE mode = 0;
        if (channel->isPlaying(&still_playing) != FMOD_OK || !still_playing || channel->getCurrentSound(&sound) != FMOD_OK || !sound ||
            sound->getMode(&mode) != FMOD_OK || !(mode & FMOD_CREATESTREAM))
            continue;
        std::string path;
        for (auto& [music_id, entry] : engine->m_fmodMusic) {
            if (entry.m_sound == sound) path = entry.m_filePath;
        }
        for (auto& [name, entry] : engine->m_fmodSounds) {
            if (path.empty() && entry.m_sound == sound)
                path = entry.m_filePath.empty() ? std::string(name) : std::string(entry.m_filePath);
        }
        for (auto& [name, cached] : engine->m_soundPathToSound) {
            if (path.empty() && cached == sound) path = name;
        }
        if (path.empty() || undecodable_paths.contains(path)) continue;
        FMOD::Sound* sample = nullptr;
        FMOD_MODE sample_mode = (mode & ~(FMOD_MODE)(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
        auto result = system->createSound(path.c_str(), sample_mode, nullptr, &sample);
        if (result != FMOD_OK || !sample) {
            undecodable_paths.insert(path);
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
        unsigned fade_count = 0;
        std::vector<unsigned long long> fade_clocks;
        std::vector<float> fade_volumes;
        if (channel->getFadePoints(&fade_count, nullptr, nullptr) == FMOD_OK && fade_count > 0) {
            fade_clocks.resize(fade_count);
            fade_volumes.resize(fade_count);
            channel->getFadePoints(&fade_count, fade_clocks.data(), fade_volumes.data());
        }
        FMOD::Channel* replacement = nullptr;
        if (system->playSound(sample, group, true, &replacement) != FMOD_OK || !replacement) {
            sample->release();
            undecodable_paths.insert(path);
            continue;
        }
        replacement->setVolumeRamp(false);
        replacement->setLoopCount(loops);
        replacement->setPosition(position, FMOD_TIMEUNIT_PCM);
        replacement->setVolume(volume);
        replacement->setPitch(pitch);
        if (frequency > 0.f) replacement->setFrequency(frequency);
        for (unsigned i = 0; i < fade_count; ++i)
            replacement->addFadePoint(fade_clocks[i], fade_volumes[i]);
        void* user_data = nullptr;
        channel->getUserData(&user_data);
        replacement->setUserData(user_data);
        // if i dont clear ts gd thinks the new one ended xd
        channel->setCallback(nullptr);
        channel->stop();
        for (auto& [id, held] : engine->m_channelIDToChannel) {
            if (held == channel) held = replacement;
        }
        for (auto& [music_id, entry] : engine->m_fmodMusic) {
            if (entry.m_sound == sound) entry.m_sound = sample;
        }
        for (auto& [name, entry] : engine->m_fmodSounds) {
            if (entry.m_sound == sound) entry.m_sound = sample;
        }
        for (auto& [name, cached] : engine->m_soundPathToSound) {
            if (cached == sound) cached = sample;
        }
        replacement->setPaused(paused);
    }
}

void AudioCapture::keep_song_on_time(FMODAudioEngine* engine) {
    auto* channel = engine->getActiveMusicChannel(0);
    bool playing = false;
    unsigned position = 0;
    if (!channel || channel->isPlaying(&playing) != FMOD_OK || !playing || channel->getPosition(&position, FMOD_TIMEUNIT_PCM) != FMOD_OK) {
        song_tracked = false;
        return;
    }
    bool paused = false;
    channel->getPaused(&paused);
    if (paused) return;
    size_t buffered = 0;
    {
        std::lock_guard guard(lock);
        buffered = channels > 0 ? pending_samples.size() / (size_t)channels : 0;
    }
    unsigned long long mixed = frames_written + buffered;
    double song_position = (double)(position);
    float frequency = 0.f;
    if (song_tracked && sample_rate > 0 && channel->getFrequency(&frequency) == FMOD_OK && frequency > 0.f) {
        float pitch = 1.f;
        channel->getPitch(&pitch);
        double seconds = (double)(mixed - std::min(mixed, frames_mixed_at_last_check)) / (double)(sample_rate);
        double expected = last_song_position + seconds * (double)(frequency) * (double)(pitch);
        double drift = std::abs(song_position - expected);
        if (drift > 2.0 && drift < 0.15 * (double)(frequency) && expected >= 0.0 &&
            channel->setPosition((unsigned)std::llround(expected), FMOD_TIMEUNIT_PCM) == FMOD_OK)
            song_position = expected;
    }
    last_song_position = song_position;
    frames_mixed_at_last_check = mixed;
    song_tracked = true;
}

void AudioCapture::advance(float dt, float fade) {
    auto* engine = FMODAudioEngine::get();
    if (!attached || !engine || !engine->m_system || sample_rate <= 0) return;
    auto* play = PlayLayer::get();
    bool waiting_for_level_start = Logic::get().render_record_pause && play && !play->m_started;
    if (waiting_for_level_start) {
        if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPaused(true);
        captured_seconds += (double)(dt);
        unsigned long long target = (unsigned long long)std::llround(captured_seconds * (double)(sample_rate));
        int channel_count = channels > 0 ? channels : speaker_channels;
        if (frames_written < target) {
            size_t frames = (size_t)(target - frames_written);
            std::vector<float> silence(frames * (size_t)channel_count, 0.f);
            raw_audio_file.write(reinterpret_cast<const char*>(silence.data()), (std::streamsize)(silence.size() * sizeof(float)));
            frames_written = target;
        }
        return;
    }
    captured_seconds += dt;
    engine_update_allowed = true;
    engine->update(dt);
    engine_update_allowed = false;
    keep_effects_playing();
    convert_streams_to_samples(engine);
    keep_song_on_time(engine);
    unsigned long long target = (unsigned long long)std::llround(captured_seconds * (double)(sample_rate));
    auto buffered = [this] {
        std::lock_guard guard(lock);
        return channels > 0 ? pending_samples.size() / (size_t)channels : 0;
    };
    mixer_allowed = true;
    freeze(false);
    int stalls = 0;
    while (frames_written + buffered() < target && stalls < 64) {
        size_t before = buffered();
        engine->m_system->update();
        stalls = buffered() == before ? stalls + 1 : 0;
    }
    freeze(true);
    mixer_allowed = false;
    std::lock_guard guard(lock);
    if (channels <= 0) return;
    size_t frames_to_write = (size_t)(std::min<unsigned long long>(target - std::min(target, frames_written), pending_samples.size() / (size_t)channels));
    size_t sample_count = frames_to_write * (size_t)channels;
    float gain = std::clamp(fade, 0.f, 1.f);
    for (size_t i = 0; i < sample_count; ++i)
        pending_samples[i] = std::clamp(pending_samples[i] * gain, -1.f, 1.f);
    raw_audio_file.write(reinterpret_cast<const char*>(pending_samples.data()), (std::streamsize)(sample_count * sizeof(float)));
    pending_samples.erase(pending_samples.begin(), pending_samples.begin() + (std::ptrdiff_t)sample_count);
    frames_written += frames_to_write;
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
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;
    BOOL ok = ConnectNamedPipe(pipe, &overlapped);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = WaitForSingleObject(overlapped.hEvent, 4000) == WAIT_OBJECT_0;
    if (!ok && GetLastError() == ERROR_PIPE_CONNECTED) ok = TRUE;
    CloseHandle(overlapped.hEvent);
    return ok == TRUE;
}

bool Recorder::write_all(HANDLE pipe, const void* data, size_t size) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;
    size_t sent = 0;
    while (sent < size) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        ResetEvent(event);
        DWORD wrote = 0;
        DWORD chunk = (DWORD)std::min(size - sent, (size_t)(1 << 20));
        BOOL ok = WriteFile(pipe, bytes + sent, chunk, &wrote, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped);
                CloseHandle(event);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &wrote, FALSE)) {
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
    wchar_t module[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, module, MAX_PATH))
        return {};
    return fs::path(module).parent_path();
}

fs::path Recorder::ffmpeg_path() {
    auto beside = Recorder::get().game_directory() / "ffmpeg.exe";
    if (fs::exists(beside)) return beside;
    return "ffmpeg.exe";
}

std::string Recorder::run_hidden(std::string command, const fs::path& log_path) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return "could not open the ffmpeg log";
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    std::wstring mutable_command = geode::utils::string::utf8ToWide(command);
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(log);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return "could not start ffmpeg";
    }
    CloseHandle(process.hThread);
    CloseHandle(log);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (WaitForSingleObject(process.hProcess, 180000) == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code == 0)
        return {};
    std::ifstream in(log_path);
    std::string line;
    std::string last_message;
    while (std::getline(in, line)) {
        if (!line.empty()) last_message = line;
    }
    if (last_message.empty()) last_message = "ffmpeg failed";
    if (last_message.size() > 160) last_message = last_message.substr(last_message.size() - 160);
    return last_message;
}

static std::string quote_arg(const fs::path& path) {
    return "\"" + geode::utils::string::pathToString(path) + "\"";
}

static fs::path ffprobe_path() {
    auto beside = fs::path(Recorder::get().ffmpeg_path()).replace_filename("ffprobe.exe");
    if (fs::exists(beside)) return beside;
    return "ffprobe.exe";
}

static std::string run_logged(std::string command, const fs::path& log_path, const fs::path& capture) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE output_handle = capture.empty()
                     ? log
                     : CreateFileW(capture.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE || output_handle == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (output_handle != INVALID_HANDLE_VALUE && output_handle != log) CloseHandle(output_handle);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return "could not open the ffmpeg log";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;
    startup.hStdOutput = output_handle;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    std::wstring mutable_command = geode::utils::string::utf8ToWide(command);
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process)) {
        CloseHandle(log);
        if (output_handle != log) CloseHandle(output_handle);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return "could not start ffmpeg";
    }
    CloseHandle(process.hThread);
    CloseHandle(log);
    if (output_handle != log) CloseHandle(output_handle);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (code == 0)
        return {};
    std::ifstream in(log_path);
    std::string line;
    std::string last_message;
    while (std::getline(in, line)) {
        if (!line.empty()) last_message = line;
    }
    if (last_message.empty()) last_message = "ffmpeg failed";
    if (last_message.size() > 180) last_message = last_message.substr(last_message.size() - 180);
    return last_message;
}

static std::optional<double> probe_duration_seconds(const fs::path& file, const fs::path& scratch, std::string& error) {
    auto command = quote_arg(ffprobe_path()) + " -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 " + quote_arg(file);
    auto capture = scratch / "echo-probe.txt";
    auto log = scratch / "echo-probe.log";
    error = run_logged(command, log, capture);
    if (!error.empty()) return std::nullopt;
    std::ifstream in(capture);
    std::string text;
    std::getline(in, text);
    try {
        size_t used = 0;
        double seconds = std::stod(text, &used);
        if (seconds > 0.0) return seconds;
    }
    catch (...) {
    }
    error = "Could not read the video length";
    return std::nullopt;
}

static bool has_audio_stream(const fs::path& file, const fs::path& scratch) {
    auto command = quote_arg(ffprobe_path()) + " -v error -select_streams a:0 -show_entries stream=index -of csv=p=0 " + quote_arg(file);
    auto capture = scratch / "echo-probe-audio.txt";
    auto log = scratch / "echo-probe.log";
    if (!run_logged(command, log, capture).empty()) return false;
    std::ifstream in(capture);
    std::string text;
    std::getline(in, text);
    return !text.empty();
}

static std::string crossfade_videos(const fs::path& first, const fs::path& second, const fs::path& dest, const std::string& encode);
static std::string rate_control_args(const std::string& codec, int bitrate, const std::string& extra);

static std::string join_videos(const fs::path& first, const fs::path& second, const fs::path& dest, const std::string& encode) {
    auto result = crossfade_videos(first, second, dest, encode);
    auto scratch = dest.parent_path();
    std::error_code error;
    for (const char* name : { "echo-probe.txt", "echo-probe-audio.txt", "echo-probe.log", "echo-merge.log", "echo-merge-progress.txt" })
        fs::remove(scratch / name, error);
    return result;
}

static std::string merge_encoder_args() {
    auto& logic = Logic::get();
    std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
    // -vf NOT allowed next to filter_complex
    bool has_filter_args = logic.render_args.find("-vf") != std::string::npos || logic.render_args.find("-filter") != std::string::npos;
    std::string extra_args = has_filter_args ? std::string() : logic.render_args;
    bool hardware_encoder = codec.find("nvenc") != std::string::npos || codec.find("amf") != std::string::npos || codec.find("qsv") != std::string::npos;
    std::string flags = " -c:v " + codec + rate_control_args(codec, logic.render_bitrate, extra_args);
    if (extra_args.find("-pix_fmt") == std::string::npos)
        flags += logic.render_bitrate <= 0 && !hardware_encoder ? " -pix_fmt yuv444p" : " -pix_fmt yuv420p";
    if (!extra_args.empty()) flags += " " + extra_args;
    return flags;
}

static std::string crossfade_videos(const fs::path& first, const fs::path& second, const fs::path& dest, const std::string& encode) {
    auto scratch = dest.parent_path();
    std::string probe_error;
    auto first_length = probe_duration_seconds(first, scratch, probe_error);
    if (!first_length) {
        std::error_code error;
        auto ffprobe_beside = fs::path(Recorder::get().ffmpeg_path()).replace_filename("ffprobe.exe");
        if (!fs::exists(ffprobe_beside, error))
            return "Merging needs ffprobe.exe next to ffmpeg.exe (" + probe_error + ")";
        return probe_error;
    }
    auto second_length = probe_duration_seconds(second, scratch, probe_error);
    if (!second_length) return probe_error;
    double fade_seconds = std::min(1.0, std::min(*first_length, *second_length) * 0.45);
    if (!(fade_seconds > 0.05)) return "The videos are too short to crossfade";
    double offset = *first_length - fade_seconds;
    Logic::get().merge_seconds = (float)(*first_length + *second_length - fade_seconds);
    bool both_have_audio = has_audio_stream(first, scratch) && has_audio_stream(second, scratch);
    std::string filter = fmt::format(
        "[1:v][0:v]scale2ref[v1][v0];[v0][v1]xfade=transition=fadeblack:duration={:.3f}:offset={:.3f}[v]",
        fade_seconds,
        offset);
    std::string map_args = "-map [v]";
    std::string audio_codec;
    if (both_have_audio) {
        filter += fmt::format(";[0:a][1:a]acrossfade=d={:.3f}[a]", fade_seconds);
        map_args += " -map [a]";
        audio_codec = "-c:a aac -b:a 320k";
    }
    auto command = fmt::format(
        "{} -y -nostdin -progress {} -i {} -i {} -filter_complex {} {}{} {} {}",
        quote_arg(Recorder::get().ffmpeg_path()),
        quote_arg(scratch / "echo-merge-progress.txt"),
        quote_arg(first),
        quote_arg(second),
        filter,
        map_args,
        encode,
        audio_codec,
        quote_arg(dest));
    return run_logged(command, scratch / "echo-merge.log", {});
}

void Logic::merge_renders(const fs::path& first_path, const fs::path& second_path) {
    if (merge_busy) return;
    auto folder = save_root() / "renders";
    std::error_code error;
    fs::create_directories(folder, error);
    merge_busy = true;
    merge_seconds = 0.f;
    merge_folder = folder;
    merge_label = fmt::format("{} & {}", geode::utils::string::pathToString(first_path.stem()), geode::utils::string::pathToString(second_path.stem()));
    notify("Joining in the background", true);
    std::thread([this, folder, first_path, second_path, encode = merge_encoder_args()] {
        std::string failure;
        fs::path dest;
        try {
            auto first_stem = geode::utils::string::pathToString(first_path.stem());
            auto second_stem = geode::utils::string::pathToString(second_path.stem());
            dest = ReplayIO::unique_file(folder, ReplayIO::sanitize(first_stem + "_" + second_stem), ".mp4");
            failure = join_videos(first_path, second_path, dest, encode);
        }
        catch (const std::exception& ex) {
            failure = ex.what();
        }
        Loader::get()->queueInMainThread([this, failure, dest] {
            merge_busy = false;
            if (failure.empty())
                notify(fmt::format("Joined {}", geode::utils::string::pathToString(dest.filename())), true);
            else notify(failure, false);
        });
    }).detach();
}
#elif defined(GEODE_IS_MACOS)
static fs::path find_tool(const char* name) {
    std::vector<fs::path> candidates{ Logic::get().save_root() / name };
    for (const char* dir : { "/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin" })
        candidates.push_back(fs::path(dir) / name);
    if (const char* path = std::getenv("PATH")) {
        std::string_view remaining_path(path);
        while (!remaining_path.empty()) {
            auto colon = remaining_path.find(':');
            auto dir = remaining_path.substr(0, colon);
            if (!dir.empty()) candidates.push_back(fs::path(std::string(dir)) / name);
            if (colon == std::string_view::npos) break;
            remaining_path.remove_prefix(colon + 1);
        }
    }
    for (const auto& candidate : candidates) {
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

static pid_t spawn_tool(const std::vector<std::string>& args, int input, const fs::path& out, const fs::path& err) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    if (input >= 0) posix_spawn_file_actions_adddup2(&actions, input, 0);
    else posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    auto redirect_to = [&](int fd, const fs::path& file) {
        if (file.empty()) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_WRONLY, 0);
        else
            posix_spawn_file_actions_addopen(&actions, fd, file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    };
    redirect_to(2, err);
    if (!out.empty() && out == err) posix_spawn_file_actions_adddup2(&actions, 2, 1);
    else redirect_to(1, out);
    std::vector<char*> argv;
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int failed = posix_spawn(&pid, args.front().c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return failed ? 0 : pid;
}

static unsigned long long process_cpu_time(pid_t pid) {
    rusage_info_v2 usage{};
    if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) != 0) return 0;
    return usage.ri_user_time + usage.ri_system_time;
}

static int wait_tool(pid_t pid, double timeout_seconds, bool while_working = false) {
    auto started = chrono::steady_clock::now();
    unsigned long long last_cpu = 0;
    while (true) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, timeout_seconds > 0.0 ? WNOHANG : 0);
        if (waited == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (waited < 0 && errno == ECHILD) return 0;
        if (waited < 0 && errno != EINTR) return -1;
        if (timeout_seconds > 0.0) {
            if (while_working) {
                unsigned long long cpu = process_cpu_time(pid);
                if (cpu != last_cpu) {
                    last_cpu = cpu;
                    started = chrono::steady_clock::now();
                }
            }
            if (chrono::duration<double>(chrono::steady_clock::now() - started).count() > timeout_seconds) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                return -1;
            }
            std::this_thread::sleep_for(chrono::milliseconds(10));
        }
    }
}

static std::string last_log_line(const fs::path& log, size_t limit) {
    std::ifstream in(log);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        if (!line.empty()) last = line;
    }
    if (last.size() > limit) last = last.substr(last.size() - limit);
    return last;
}

static std::string run_tool(const std::vector<std::string>& args, const fs::path& log, const fs::path& capture, double timeout_seconds = 0.0) {
    pid_t pid = spawn_tool(args, -1, capture.empty() ? log : capture, log);
    if (!pid) return "could not start " + fs::path(args.front()).filename().string();
    if (wait_tool(pid, timeout_seconds) == 0)
        return {};
    auto last = last_log_line(log, 180);
    return last.empty() ? std::string("ffmpeg failed") : last;
}

static std::vector<std::string> split_args(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    bool has_word = false;
    char quote = 0;
    for (char c : text) {
        if (quote) {
            if (c == quote) quote = 0;
            else word += c;
        }
        else if (c == '"' || c == '\'') {
            quote = c;
            has_word = true;
        }
        else if (c == ' ' || c == '\t' || c == '\n') {
            if (has_word) words.push_back(std::move(word));
            word.clear();
            has_word = false;
        }
        else {
            word += c;
            has_word = true;
        }
    }
    if (has_word) words.push_back(std::move(word));
    return words;
}

static std::vector<std::string> rate_control_args(const std::string& codec, int bitrate, const std::string& extra, long long pixel_rate) {
    auto has_arg = [&](const char* key) {
        return extra.find(key) != std::string::npos;
    };
    std::vector<std::string> flags;
    if ((codec == "libx264" || codec == "libx265") && !has_arg("-preset"))
        flags.insert(flags.end(), { "-preset", Logic::get().render_preset });
    bool sets_own_rate = has_arg("-crf") || has_arg("-qp") || has_arg("-b:v") || has_arg("-q:v") || has_arg("-cq") || has_arg("-global_quality") || has_arg("-qscale") || has_arg("-rc ");
    if (sets_own_rate) return flags;
    if (bitrate > 0)
        flags.insert(flags.end(), { "-b:v", std::to_string(bitrate) });
    else if (codec.find("videotoolbox") != std::string::npos)
        flags.insert(flags.end(), { "-b:v", std::to_string(std::max<long long>(pixel_rate / 4, 8'000'000)) });
    else
        flags.insert(flags.end(), { "-qp", "0" });
    return flags;
}

static std::optional<double> probe_duration_seconds(const fs::path& probe, const fs::path& file, const fs::path& scratch, std::string& error) {
    auto capture = scratch / "echo-probe.txt";
    error = run_tool({ probe.string(), "-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1", file.string() }, scratch / "echo-probe.log", capture);
    if (!error.empty()) return std::nullopt;
    std::ifstream in(capture);
    std::string text;
    std::getline(in, text);
    try {
        double seconds = std::stod(text);
        if (seconds > 0.0) return seconds;
    }
    catch (...) {
    }
    error = "Could not read the video length";
    return std::nullopt;
}

static bool has_audio_stream(const fs::path& probe, const fs::path& file, const fs::path& scratch) {
    auto capture = scratch / "echo-probe-audio.txt";
    if (!run_tool({ probe.string(), "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=index", "-of", "csv=p=0", file.string() }, scratch / "echo-probe.log", capture).empty())
        return false;
    std::ifstream in(capture);
    std::string text;
    std::getline(in, text);
    return !text.empty();
}

static std::vector<std::string> merge_encoder_args() {
    auto& logic = Logic::get();
    std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
    // -vf NOT allowed next to filter_complex
    bool has_filter_args = logic.render_args.find("-vf") != std::string::npos || logic.render_args.find("-filter") != std::string::npos;
    std::string extra_args = has_filter_args ? std::string() : logic.render_args;
    bool hardware_encoder = codec.find("videotoolbox") != std::string::npos;
    std::vector<std::string> flags{ "-c:v", codec };
    auto rate_args = rate_control_args(codec, logic.render_bitrate, extra_args, (long long)1920 * 1080 * 60);
    flags.insert(flags.end(), rate_args.begin(), rate_args.end());
    if (extra_args.find("-pix_fmt") == std::string::npos)
        flags.insert(flags.end(), { "-pix_fmt", logic.render_bitrate <= 0 && !hardware_encoder ? "yuv444p" : "yuv420p" });
    auto words = split_args(extra_args);
    flags.insert(flags.end(), words.begin(), words.end());
    return flags;
}

static std::string crossfade_videos(const fs::path& first, const fs::path& second, const fs::path& dest, const std::vector<std::string>& encode) {
    auto ffmpeg = Recorder::ffmpeg_path();
    if (ffmpeg.empty()) return "ffmpeg was not found";
    auto probe = ffmpeg.parent_path() / "ffprobe";
    if (access(probe.c_str(), X_OK) != 0) probe = find_tool("ffprobe");
    if (probe.empty()) return "Merging needs ffprobe, which comes with ffmpeg";
    auto scratch = dest.parent_path();
    std::string probe_error;
    auto first_length = probe_duration_seconds(probe, first, scratch, probe_error);
    if (!first_length) return probe_error;
    auto second_length = probe_duration_seconds(probe, second, scratch, probe_error);
    if (!second_length) return probe_error;
    double fade_seconds = std::min(1.0, std::min(*first_length, *second_length) * 0.45);
    if (!(fade_seconds > 0.05)) return "The videos are too short to crossfade";
    double offset = *first_length - fade_seconds;
    Logic::get().merge_seconds = (float)(*first_length + *second_length - fade_seconds);
    bool both_have_audio = has_audio_stream(probe, first, scratch) && has_audio_stream(probe, second, scratch);
    std::string filter = fmt::format("[1:v][0:v]scale2ref[v1][v0];[v0][v1]xfade=transition=fadeblack:duration={:.3f}:offset={:.3f}[v]", fade_seconds, offset);
    std::vector<std::string> args{ ffmpeg.string(), "-y", "-nostdin", "-progress", (scratch / "echo-merge-progress.txt").string(), "-i", first.string(), "-i", second.string() };
    if (both_have_audio)
        filter += fmt::format(";[0:a][1:a]acrossfade=d={:.3f}[a]", fade_seconds);
    args.insert(args.end(), { "-filter_complex", filter, "-map", "[v]" });
    if (both_have_audio)
        args.insert(args.end(), { "-map", "[a]" });
    args.insert(args.end(), encode.begin(), encode.end());
    if (both_have_audio)
        args.insert(args.end(), { "-c:a", "aac", "-b:a", "320k" });
    args.push_back(dest.string());
    return run_tool(args, scratch / "echo-merge.log", {});
}

static std::string join_videos(const fs::path& first, const fs::path& second, const fs::path& dest, const std::vector<std::string>& encode) {
    auto result = crossfade_videos(first, second, dest, encode);
    std::error_code error;
    for (const char* name : { "echo-probe.txt", "echo-probe-audio.txt", "echo-probe.log", "echo-merge.log", "echo-merge-progress.txt" })
        fs::remove(dest.parent_path() / name, error);
    return result;
}

fs::path Recorder::ffmpeg_path() {
    return find_tool("ffmpeg");
}

bool Recorder::write_all(int fd, const void* data, size_t size) {
    if (fd < 0) return false;
    const auto* bytes = static_cast<const unsigned char*>(data);
    while (size > 0) {
        ssize_t wrote = ::write(fd, bytes, size);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += wrote;
        size -= (size_t)wrote;
    }
    return true;
}

void Logic::merge_renders(const fs::path& first_path, const fs::path& second_path) {
    if (merge_busy) return;
    auto folder = save_root() / "renders";
    std::error_code error;
    fs::create_directories(folder, error);
    merge_busy = true;
    merge_seconds = 0.f;
    merge_folder = folder;
    merge_label = fmt::format("{} & {}", first_path.stem().string(), second_path.stem().string());
    notify("Joining in the background", true);
    std::thread([this, folder, first_path, second_path, encode = merge_encoder_args()] {
        std::string failure;
        fs::path dest;
        try {
            dest = ReplayIO::unique_file(folder, ReplayIO::sanitize(first_path.stem().string() + "_" + second_path.stem().string()), ".mp4");
            failure = join_videos(first_path, second_path, dest, encode);
        }
        catch (const std::exception& ex) {
            failure = ex.what();
        }
        Loader::get()->queueInMainThread([this, failure, dest] {
            merge_busy = false;
            if (failure.empty())
                notify(fmt::format("Joined {}", dest.filename().string()), true);
            else notify(failure, false);
        });
    }).detach();
}

#else
void Logic::merge_renders(const fs::path&, const fs::path&) {
    notify("Merging renders needs ffmpeg.exe on Windows", false);
}
#endif

float Logic::merge_progress() {
    static float shown_progress = -1.f;
    static chrono::steady_clock::time_point last_read;
    if (!merge_busy) return shown_progress = -1.f;
    auto now = chrono::steady_clock::now();
    if (now - last_read < chrono::milliseconds(250)) return shown_progress;
    last_read = now;
    float total = merge_seconds;
    std::ifstream in(merge_folder / "echo-merge-progress.txt", std::ios::binary | std::ios::ate);
    if (!in || total <= 0.f) return shown_progress = -1.f;
    auto size = (std::streamoff)in.tellg();
    std::streamoff tail_size = std::min<std::streamoff>(size, 600);
    std::string tail((size_t)tail_size, '\0');
    in.seekg(size - tail_size);
    in.read(tail.data(), tail_size);
    auto key_at = tail.rfind("out_time_us=");
    if (key_at == std::string::npos) key_at = tail.rfind("out_time_ms=");
    if (key_at == std::string::npos) return shown_progress;
    double out_time_us = std::strtod(tail.c_str() + key_at + 12, nullptr);
    return shown_progress = std::clamp((float)(out_time_us / 1e6 / total), 0.f, 1.f);
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
    if (!window || !GetClientRect(window, &client)) return std::nullopt;
    if (client.right <= client.left || client.bottom <= client.top) return std::nullopt;
    return CCSize((float)(client.right - client.left), (float)(client.bottom - client.top));
#else
    return std::nullopt;
#endif
}

void Recorder::shift_ui(PlayLayer* play, float distance) {
    if (!play || distance == 0.f) return;
    if (play->m_progressBar)
        play->m_progressBar->setPositionX(play->m_progressBar->getPositionX() + distance * .5f);
    if (play->m_percentageLabel)
        play->m_percentageLabel->setPositionX(play->m_percentageLabel->getPositionX() + distance * .5f);
    if (play->m_uiLayer && play->m_uiLayer->m_pauseBtn)
        play->m_uiLayer->m_pauseBtn->setPositionX(play->m_uiLayer->m_pauseBtn->getPositionX() + distance);
}

void Recorder::hold_view() {
    CCSize size((float)(width), (float)(height));
    if (!view_held) {
        saved_win_width = CCDirector::sharedDirector()->getWinSize().width;
        saved_frame_size = CCEGLView::sharedOpenGLView()->getFrameSize();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MOBILE)
        saved_window_pixels = window_pixels().value_or(saved_frame_size);
#else
        GLint viewport[4] = {};
        glGetIntegerv(GL_VIEWPORT, viewport);
        saved_window_pixels = CCSize((float)(viewport[0] * 2 + viewport[2]), (float)(viewport[1] * 2 + viewport[3]));
        if (saved_window_pixels.width < 1.f || saved_window_pixels.height < 1.f) saved_window_pixels = saved_frame_size;
#endif
        view_held = true;
    }
    if (!CCEGLView::sharedOpenGLView()->getFrameSize().equals(size))
        Recorder::get().silent_change_size(size);
    if (auto* play = PlayLayer::get(); play && !shifted_play_layer) {
        float width_change = CCDirector::sharedDirector()->getWinSize().width - saved_win_width;
        if (std::fabs(width_change) > .5f) {
            shift_ui(play, width_change);
            ui_shift = width_change;
            shifted_play_layer = play;
        }
    }
    auto* layer = GJBaseGameLayer::get();
    auto* shader = layer ? layer->m_shaderLayer : nullptr;
    if (shader && (!shader->m_screenSize.equals(size) || !shader->m_targetTextureSize.equals(size))) {
        if (!saved_shader.layer && shader->m_renderTexture && shader->m_sprite) {
            saved_shader.layer = shader;
            saved_shader.texture = shader->m_renderTexture;
            saved_shader.texture->retain();
            saved_shader.texture_rect = shader->m_sprite->getTextureRect();
            saved_shader.scale_factor = shader->m_scaleFactor;
            saved_shader.aspect_ratio = shader->m_aspectRatio;
            saved_shader.screen_size = shader->m_screenSize;
            saved_shader.texture_content_size = shader->m_textureContentSize;
            saved_shader.target_texture_size = shader->m_targetTextureSize;
            saved_shader.target_texture_size_extra = shader->m_targetTextureSizeExtra;
            saved_shader.texture_scale_x = shader->m_state.m_textureScaleX;
            saved_shader.texture_scale_y = shader->m_state.m_textureScaleY;
        }
        Recorder::get().resize_shader_layer(size);
    }
}

CCSize Recorder::real_frame_size() {
    auto pixels = window_pixels().value_or(saved_window_pixels);
    return CCSize(
        saved_frame_size.width * pixels.width / std::max(saved_window_pixels.width, 1.f),
        saved_frame_size.height * pixels.height / std::max(saved_window_pixels.height, 1.f));
}

void Recorder::release_view() {
    if (!view_held) return;
    view_held = false;
    auto restored_size = real_frame_size();
    Recorder::get().silent_change_size(restored_size);
    auto* layer = GJBaseGameLayer::get();
    auto* shader = layer ? layer->m_shaderLayer : nullptr;
    if (shader && shader == saved_shader.layer && saved_shader.texture && shader->m_sprite) {
        if (shader->m_renderTexture && shader->m_renderTexture != saved_shader.texture)
            shader->m_renderTexture->release();
        shader->m_renderTexture = saved_shader.texture;
        shader->m_sprite->setTexture(saved_shader.texture->getSprite()->getTexture());
        shader->m_sprite->setTextureRect(saved_shader.texture_rect);
        shader->m_scaleFactor = saved_shader.scale_factor;
        shader->m_aspectRatio = saved_shader.aspect_ratio;
        shader->m_screenSize = saved_shader.screen_size;
        shader->m_textureContentSize = saved_shader.texture_content_size;
        shader->m_targetTextureSize = saved_shader.target_texture_size;
        shader->m_targetTextureSizeExtra = saved_shader.target_texture_size_extra;
        shader->m_state.m_textureScaleX = saved_shader.texture_scale_x;
        shader->m_state.m_textureScaleY = saved_shader.texture_scale_y;
    }
    else {
        if (saved_shader.texture) saved_shader.texture->release();
        Recorder::get().resize_shader_layer(restored_size);
    }
    saved_shader = {};
    if (shifted_play_layer && shifted_play_layer == PlayLayer::get()) shift_ui(shifted_play_layer, -ui_shift);
    shifted_play_layer = nullptr;
    ui_shift = 0.f;
    CCDirector::sharedDirector()->setNextDeltaTimeZero(true);
}

size_t Recorder::frame_bytes() const {
    size_t pixels = (size_t)width * (size_t)height;
    return nv12 ? pixels + pixels / 2 : pixels * 4;
}

#ifdef GEODE_IS_WINDOWS
static GLuint compile_shader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return shader;
    glDeleteShader(shader);
    return 0;
}

static GLuint link_program(const char* vertex, const char* fragment) {
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex);
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment);
    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return program;
    glDeleteProgram(program);
    return 0;
}

static GLuint make_color_target(GLint internal, GLenum format, int width, int height) {
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

static GLuint make_framebuffer(GLuint texture) {
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &framebuffer);
        return 0;
    }
    return framebuffer;
}

static std::string encoder_speed_args(const std::string& codec, const std::string& extra, bool with_rate) {
    auto has_arg = [&](const char* key) {
        return extra.find(key) != std::string::npos;
    };
    std::string flags;
    if (!has_arg("-preset") && !has_arg("-quality")) {
        if (codec.find("nvenc") != std::string::npos) flags += " -preset p1";
        else if (codec.find("amf") != std::string::npos) flags += " -quality speed";
        else if (codec.find("qsv") != std::string::npos) flags += " -preset veryfast";
        else if (codec == "libx264" || codec == "libx265")
            flags += " -preset " + Logic::get().render_preset;
    }
    if (with_rate && codec.find("nvenc") != std::string::npos && !has_arg("-rc ")) flags += " -rc vbr";
    return flags;
}

static std::string rate_control_args(const std::string& codec, int bitrate, const std::string& extra) {
    auto has_arg = [&](const char* key) {
        return extra.find(key) != std::string::npos;
    };
    bool sets_own_rate = has_arg("-crf") || has_arg("-qp") || has_arg("-b:v") || has_arg("-q:v") || has_arg("-cq") || has_arg("-global_quality") || has_arg("-qscale") || has_arg("-rc ");
    if (sets_own_rate) return encoder_speed_args(codec, extra, false);
    if (bitrate > 0)
        return encoder_speed_args(codec, extra, true) + fmt::format(" -b:v {}", bitrate);
    std::string flags = encoder_speed_args(codec, extra, false);
    if (codec.find("nvenc") != std::string::npos) flags += " -rc constqp -qp 0";
    else if (codec.find("amf") != std::string::npos) flags += " -rc cqp -qp_i 0 -qp_p 0";
    else if (codec.find("qsv") != std::string::npos) flags += " -global_quality 1";
    else flags += " -qp 0";
    return flags;
}

using SetSwapIntervalFunction = BOOL(WINAPI*)(int);
using GetSwapIntervalFunction = int(WINAPI*)();

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

static const char* nv12_vertex_shader = R"(
#version 120
void main() {
	gl_Position = gl_Vertex;
	gl_TexCoord[0].xy = vec2(gl_MultiTexCoord0.x, 1.0 - gl_MultiTexCoord0.y);
}
)";
static const char* nv12_luma_shader = R"(
#version 120
uniform sampler2D u_texture;
void main() {
	vec3 rgb = texture2D(u_texture, gl_TexCoord[0].xy).rgb;
	float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	y = y * 219.0 / 255.0 + 16.0 / 255.0;
	gl_FragColor = vec4(y, 0.0, 0.0, 1.0);
}
)";
static const char* nv12_chroma_shader = R"(
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

void Recorder::create_nv12_targets() {
    GLint old_texture = 0, old_fbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    y_texture = make_color_target(GL_R8, GL_RED, width, height);
    uv_texture = make_color_target(GL_RG8, GL_RG, width / 2, height / 2);
    y_fbo = make_framebuffer(y_texture);
    uv_fbo = make_framebuffer(uv_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glBindTexture(GL_TEXTURE_2D, old_texture);
}

bool Recorder::setup_nv12() {
    y_program = link_program(nv12_vertex_shader, nv12_luma_shader);
    uv_program = link_program(nv12_vertex_shader, nv12_chroma_shader);
    if (!y_program || !uv_program) return false;
    create_nv12_targets();
    return y_texture && uv_texture && y_fbo && uv_fbo;
}

void Recorder::close_nv12() {
    if (y_program) glDeleteProgram(y_program);
    if (uv_program) glDeleteProgram(uv_program);
    if (y_fbo) glDeleteFramebuffers(1, &y_fbo);
    if (uv_fbo) glDeleteFramebuffers(1, &uv_fbo);
    if (y_texture) glDeleteTextures(1, &y_texture);
    if (uv_texture) glDeleteTextures(1, &uv_texture);
    y_program = 0;
    uv_program = 0;
    y_fbo = 0;
    uv_fbo = 0;
    y_texture = 0;
    uv_texture = 0;
    nv12 = false;
}

void Recorder::draw_nv12_plane(GLuint framebuffer, GLuint program, int plane_width, int plane_height) {
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, plane_width, plane_height);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_texture"), 0);
    draw_full_quad();
}

void Recorder::convert_nv12() {
    GLint old_program = 0, old_texture = 0, old_fbo = 0;
    GLint viewport[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLboolean blend_was_on = glIsEnabled(GL_BLEND);
    GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
    if (blend_was_on) glDisable(GL_BLEND);
    if (depth_was_on) glDisable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, capture_texture);
    draw_nv12_plane(y_fbo, y_program, width, height);
    draw_nv12_plane(uv_fbo, uv_program, width / 2, height / 2);
    glUseProgram(old_program);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (blend_was_on) glEnable(GL_BLEND);
    if (depth_was_on) glEnable(GL_DEPTH_TEST);
}
#endif

#define UNCAPPED_ANIMATION_INTERVAL (1.f / 10000.f)

void Recorder::uncap_frame_rate() {
    if (frame_rate_uncapped) return;
    auto* director = CCDirector::sharedDirector();
    saved_animation_interval = director->getAnimationInterval();
    director->setAnimationInterval(UNCAPPED_ANIMATION_INTERVAL);
#ifdef GEODE_IS_WINDOWS
    saved_swap_interval = 1;
    if (auto get_swap = reinterpret_cast<GetSwapIntervalFunction>(wglGetProcAddress("wglGetSwapIntervalEXT")))
        saved_swap_interval = get_swap();
    if (auto set_swap = reinterpret_cast<SetSwapIntervalFunction>(wglGetProcAddress("wglSwapIntervalEXT")))
        set_swap(0);
#endif
#ifdef GEODE_IS_MACOS
    if (auto context = CGLGetCurrentContext()) {
        GLint swap = 1;
        saved_swap_interval = CGLGetParameter(context, kCGLCPSwapInterval, &swap) == kCGLNoError ? swap : 1;
        GLint none = 0;
        CGLSetParameter(context, kCGLCPSwapInterval, &none);
    }
#endif
    frame_rate_uncapped = true;
}

void Recorder::restore_frame_rate() {
    if (!frame_rate_uncapped) return;
    frame_rate_uncapped = false;
    CCDirector::sharedDirector()->setAnimationInterval(saved_animation_interval);
#ifdef GEODE_IS_WINDOWS
    if (auto set_swap = reinterpret_cast<SetSwapIntervalFunction>(wglGetProcAddress("wglSwapIntervalEXT")))
        set_swap(saved_swap_interval);
#endif
#ifdef GEODE_IS_MACOS
    if (auto context = CGLGetCurrentContext()) {
        GLint swap = saved_swap_interval;
        CGLSetParameter(context, kCGLCPSwapInterval, &swap);
    }
#endif
}

bool Recorder::create_capture_framebuffer() {
    GLint old_fbo = 0, old_texture = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGenTextures(1, &capture_texture);
    glBindTexture(GL_TEXTURE_2D, capture_texture);
#ifdef GEODE_IS_WINDOWS
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
#else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &capture_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture_texture, 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    return complete;
}

#ifdef GEODE_IS_WINDOWS
void Recorder::open_gpu_readback() {
    nv12 = !use_api && Recorder::get().setup_nv12();
    if (!nv12) Recorder::get().close_nv12();
    auto bytes = (GLsizeiptr)Recorder::get().frame_bytes();
    glGenBuffers(READBACK_RING_SIZE, pixel_buffers.data());
    for (auto buf : pixel_buffers) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buf);
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    fences.fill(nullptr);
    buffers_in_flight.clear();
    next_slot = 0;
}
#endif

#ifdef GEODE_IS_MACOS
void Recorder::open_gpu_readback() {
    buffered_capture = !use_api;
    if (buffered_capture) {
        glGenBuffers(READBACK_RING_SIZE, pixel_buffers.data());
        auto bytes = (GLsizeiptr)frame_bytes();
        bool allocated = true;
        for (auto buffer : pixel_buffers) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
            GLint size = 0;
            glGetBufferParameteriv(GL_PIXEL_PACK_BUFFER, GL_BUFFER_SIZE, &size);
            allocated = allocated && buffer && size == bytes;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        if (!allocated) {
            glDeleteBuffers(READBACK_RING_SIZE, pixel_buffers.data());
            pixel_buffers.fill(0);
            buffered_capture = false;
        }
    }
    buffers_in_flight.clear();
    next_slot = 0;
}
#endif

void Recorder::create_overlay_sprites() {
    fade_shade = CCLayerColor::create(ccc4(0, 0, 0, 0));
    fade_shade->retain();
    bar_background = CCSprite::create("slidergroove2.png");
    bar_fill = CCSprite::create("sliderBar2.png");
    if (bar_background) bar_background->retain();
    if (bar_fill) bar_fill->retain();
    if (bar_background && bar_fill) {
        ccTexParams params = { GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_CLAMP_TO_EDGE };
        bar_fill->getTexture()->setTexParameters(&params);
        bar_fill->setAnchorPoint({ 0.f, 0.5f });
        bar_fill->setPosition({ 2.f, bar_background->getContentSize().height * 0.5f });
        bar_background->addChild(bar_fill, -1);
    }
    bar_text = CCLabelBMFont::create("Rendering", "bigFont.fnt");
    if (bar_text) bar_text->retain();
    bar_text_shown.clear();
    bar_text_updated_at = {};
}

bool Recorder::open_capture_target() {
    bool complete = create_capture_framebuffer();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    open_gpu_readback();
#endif
    create_overlay_sprites();
    return complete;
}

#ifdef GEODE_IS_MACOS
void Recorder::close_gpu_readback() {
    buffers_in_flight.clear();
    if (pixel_buffers[0]) glDeleteBuffers(READBACK_RING_SIZE, pixel_buffers.data());
    pixel_buffers.fill(0);
    buffered_capture = false;
}
#endif

#ifdef GEODE_IS_WINDOWS
void Recorder::close_gpu_readback() {
    for (auto& fence : fences) {
        if (fence) glDeleteSync(fence);
        fence = nullptr;
    }
    buffers_in_flight.clear();
    if (pixel_buffers[0]) glDeleteBuffers(READBACK_RING_SIZE, pixel_buffers.data());
    pixel_buffers.fill(0);
    Recorder::get().close_nv12();
}
#endif

void Recorder::release_overlay_sprites() {
    if (fade_shade) fade_shade->release();
    fade_shade = nullptr;
    if (bar_background) bar_background->release();
    if (bar_fill) bar_fill->release();
    if (bar_text) bar_text->release();
    bar_background = nullptr;
    bar_fill = nullptr;
    bar_text = nullptr;
}

void Recorder::close_capture_target() {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    close_gpu_readback();
#endif
#ifdef GEODE_IS_IOS
    if (direct_fbo) glDeleteFramebuffers(1, &direct_fbo);
    direct_fbo = 0;
    direct_failed = false;
#endif
    if (capture_fbo) glDeleteFramebuffers(1, &capture_fbo);
    if (capture_texture) glDeleteTextures(1, &capture_texture);
    capture_fbo = 0;
    capture_texture = 0;
    release_overlay_sprites();
}

#ifdef GEODE_IS_MACOS
bool Recorder::capture_bgra_buffered() {
    if (!buffered_capture) return false;
    while ((int)buffers_in_flight.size() >= READBACK_RING_SIZE && collect_pixel_buffer(true)) {
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_buffers[next_slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    buffers_in_flight.push_back(next_slot);
    next_slot = (next_slot + 1) % READBACK_RING_SIZE;
    while (collect_pixel_buffer(false)) {
    }
    return true;
}
#endif

void Recorder::draw_scene_into_capture(CCDirector* director) {
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    kmGLPushMatrix();
    if (auto* play = PlayLayer::get()) play->visit();
    else if (auto* scene = director->getRunningScene()) scene->visit();
    if (video_fade < 1.f && fade_shade) {
        fade_shade->setContentSize(director->getWinSize());
        fade_shade->setOpacity((GLubyte)std::lround((1.f - video_fade) * 255.f));
        fade_shade->visit();
    }
    kmGLPopMatrix();
}

#ifdef GEODE_IS_WINDOWS
void Recorder::read_nv12_planes() {
    glBindFramebuffer(GL_FRAMEBUFFER, y_fbo);
    glReadPixels(0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, uv_fbo);
    auto offset = (uintptr_t)width * (uintptr_t)height;
    glReadPixels(0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(offset));
}

void Recorder::read_into_pixel_buffer() {
    if (nv12) Recorder::get().convert_nv12();
    int slot = next_slot;
    while (fences[slot] && Recorder::get().collect_pixel_buffer(true)) {
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_buffers[slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (nv12) read_nv12_planes();
    else glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    buffers_in_flight.push_back(slot);
    next_slot = (slot + 1) % READBACK_RING_SIZE;
    while (Recorder::get().collect_pixel_buffer(false)) {
    }
}
#else
void Recorder::read_into_frame() {
    auto frame = Recorder::get().acquire_frame();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    {
        ScopedTimer timer{ g_render_timing.read_back_ns };
#ifdef GEODE_IS_MACOS
        if (!use_api)
            glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, frame.data());
        else
#endif
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
    }
    Recorder::get().queue_frame(std::move(frame));
}
#endif

void Recorder::draw_and_read_frame() {
    auto* director = CCDirector::sharedDirector();
    GLint window_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &window_fbo);
    draw_scene_into_capture(director);

#ifdef GEODE_IS_MACOS
    if (capture_bgra_buffered()) {
        glBindFramebuffer(GL_FRAMEBUFFER, window_fbo);
        return;
    }
#endif
#ifdef GEODE_IS_WINDOWS
    read_into_pixel_buffer();
#else
#ifdef GEODE_IS_IOS
    if (Recorder::get().draw_direct_frame()) {
        glBindFramebuffer(GL_FRAMEBUFFER, window_fbo);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, capture_fbo);
#endif
    read_into_frame();
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, window_fbo);
}

void Recorder::show_overlay() {
    if (!active) return;
    if (view_held) {
#ifdef GEODE_IS_MOBILE
        silent_change_size(saved_frame_size);
#else
        silent_change_size(real_frame_size());
#endif
    }
    auto pixels = window_pixels().value_or(saved_window_pixels);
    if (pixels.width < 1.f || pixels.height < 1.f) pixels = CCSize(1.f, 1.f);
    GLfloat old_clear_color[4] = {};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);
    GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor_was_on) glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)screen_fbo);
    glViewport(0, 0, (GLsizei)pixels.width, (GLsizei)pixels.height);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (Logic::get().render_show_lvl) Recorder::get().draw_preview(pixels);
    PauseLayer* pause_layer = nullptr;
    if (auto* scene = CCDirector::sharedDirector()->getRunningScene())
        pause_layer = scene->getChildByType<PauseLayer>(0);
    if (!pause_layer) {
        if (auto* play = PlayLayer::get()) pause_layer = play->getChildByType<PauseLayer>(0);
    }
    if (pause_layer) {
        kmGLPushMatrix();
        pause_layer->visit();
        kmGLPopMatrix();
    }
    Recorder::get().draw_progress();
    // geode stuff on top goes on screen NOT in the video
    if (auto* overlay = OverlayManager::get()) {
        kmGLPushMatrix();
        overlay->visit();
        kmGLPopMatrix();
    }

    glClearColor(old_clear_color[0], old_clear_color[1], old_clear_color[2], old_clear_color[3]);
    if (scissor_was_on) glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, (GLsizei)pixels.width, (GLsizei)pixels.height);
}

#ifdef GEODE_IS_IOS
static GLuint flip_program() {
    static GLuint program = 0;
    static bool tried = false;
    if (tried) return program;
    tried = true;
    auto compile = [](GLenum type, const char* source) {
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
    if (!vertex || !fragment) return 0;
    GLuint new_program = glCreateProgram();
    glAttachShader(new_program, vertex);
    glAttachShader(new_program, fragment);
    glBindAttribLocation(new_program, kCCVertexAttrib_Position, "a_position");
    glBindAttribLocation(new_program, kCCVertexAttrib_TexCoords, "a_texCoord");
    glLinkProgram(new_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(new_program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(new_program);
        return 0;
    }
    ccGLUseProgram(new_program);
    glUniform1i(glGetUniformLocation(new_program, "u_texture"), 0);
    program = new_program;
    return program;
}

bool Recorder::draw_direct_frame() {
    if (!use_api || !api_encoder || direct_failed) return false;
    ScopedTimer timer{ g_render_timing.read_back_ns };
    GLuint program = flip_program();
    unsigned encoder_texture = 0;
    std::string failed;
    if (!program || !api_encoder->beginDirect(encoder_texture, failed)) {
        direct_failed = true;
        return false;
    }
    if (!direct_fbo) glGenFramebuffers(1, &direct_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, direct_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, encoder_texture, 0);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glViewport(0, 0, width, height);
        const GLfloat corners[] = { -1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
        const GLfloat coords[] = { 0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 1.f, 0.f };
        ccGLUseProgram(program);
        ccGLBindTexture2D(capture_texture);
        ccGLBlendFunc(GL_ONE, GL_ZERO);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position | kCCVertexAttribFlag_TexCoords);
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, corners);
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    bool sent = complete ? api_encoder->endDirect(failed) : true;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    if (!complete) {
        direct_failed = true;
        return false;
    }
    if (!sent) {
        std::lock_guard guard(lock);
        failure_reason = failed;
        encoding_broken = true;
    }
    return true;
}
#endif

void Recorder::draw_preview(CCSize pixels) {
    if (!capture_texture || width < 1 || height < 1 || pixels.width < 1.f || pixels.height < 1.f) return;
    auto* program = CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTexture);
    if (!program) return;
    auto win_size = CCDirector::sharedDirector()->getWinSize();
    float screen_aspect = pixels.width / pixels.height;
    float video_aspect = (float)(width) / (float)(height);
    float width_share = video_aspect >= screen_aspect ? 1.f : video_aspect / screen_aspect;
    float height_share = video_aspect >= screen_aspect ? screen_aspect / video_aspect : 1.f;
    float x0 = win_size.width * (1.f - width_share) * .5f, y0 = win_size.height * (1.f - height_share) * .5f, x1 = win_size.width - x0;
    float y1 = win_size.height - y0;
    const GLfloat corners[] = { x0, y0, x1, y0, x0, y1, x1, y1 };
    const GLfloat coords[] = { 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f };
    program->use();
    program->setUniformsForBuiltins();
    ccGLBindTexture2D(capture_texture);
    ccGLBlendFunc(GL_ONE, GL_ZERO);
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position | kCCVertexAttribFlag_TexCoords);
    glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, corners);
    glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, 0, coords);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Recorder::draw_progress() {
    if (!bar_text) return;
    auto status = Logic::get().render_status();
    if (!status.active) return;
    auto win = CCDirector::sharedDirector()->getWinSize();
    float scale = win.height / 320.f;
    CCPoint center = { win.width * 0.5f, win.height * 0.5f };
    if (bar_background && bar_fill) {
        float full_width = bar_background->getTextureRect().size.width - 4.f;
        bar_fill->setTextureRect({ 0.f, 0.f, full_width * std::clamp(status.known ? status.progress : 0.f, 0.f, 1.f), 8.f });
        auto* manager = GameManager::get();
        bar_fill->setColor(manager->colorForIdx(manager->getPlayerColor()));
        bar_background->setScale(1.3f * scale);
        bar_background->setPosition(center);
    }
    auto now = chrono::steady_clock::now();
    if (bar_text_shown.empty() || now - bar_text_updated_at >= chrono::milliseconds(200)) {
        bar_text_updated_at = now;
        if (status.text != bar_text_shown) {
            bar_text_shown = status.text;
            bar_text->setString(bar_text_shown.c_str());
        }
    }
    bar_text->setScale(0.4f * scale);
    bar_text->setPosition({ center.x, center.y + 22.f * scale });
    kmGLPushMatrix();
    if (bar_background && bar_fill) bar_background->visit();
    bar_text->visit();
    kmGLPopMatrix();
}

#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
#ifdef GEODE_IS_WINDOWS
#define FENCE_WAIT_TIMEOUT_NS 5'000'000'000ull

bool Recorder::wait_and_delete_fence(int slot, bool wait) {
    GLenum wait_result = glClientWaitSync(fences[slot], wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, wait ? FENCE_WAIT_TIMEOUT_NS : 0);
    if (!wait && wait_result != GL_ALREADY_SIGNALED && wait_result != GL_CONDITION_SATISFIED) return false;
    glDeleteSync(fences[slot]);
    fences[slot] = nullptr;
    return true;
}
#endif

void Recorder::copy_pixel_buffer_to_queue(int slot) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pixel_buffers[slot]);
    if (const auto* data = static_cast<const unsigned char*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY))) {
        auto frame = Recorder::get().acquire_frame();
        std::memcpy(frame.data(), data, frame.size());
        if (glUnmapBuffer(GL_PIXEL_PACK_BUFFER)) {
            Recorder::get().queue_frame(std::move(frame));
        }
        else {
            std::lock_guard guard(lock);
            failure_reason = "The captured frame buffer was invalid";
            encoding_broken = true;
        }
    }
    else {
        std::lock_guard guard(lock);
        failure_reason = "Could not read the captured frame buffer";
        encoding_broken = true;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

bool Recorder::collect_pixel_buffer(bool wait) {
    if (buffers_in_flight.empty()) return false;
    int slot = buffers_in_flight.front();
#ifdef GEODE_IS_WINDOWS
    if (!wait_and_delete_fence(slot, wait)) return false;
#else
    if (!wait && (int)buffers_in_flight.size() < READBACK_RING_SIZE - 1) return false;
    ScopedTimer timer{ g_render_timing.read_back_ns };
#endif
    buffers_in_flight.pop_front();
    copy_pixel_buffer_to_queue(slot);
    return true;
}
#endif

std::vector<unsigned char> Recorder::acquire_frame() {
    std::vector<unsigned char> frame;
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

void Recorder::queue_frame(std::vector<unsigned char> frame) {
    std::unique_lock guard(lock);
    {
        ScopedTimer timer{ g_render_timing.encoder_wait_ns };
        queue_changed.wait(guard, [this] {
            return frame_queue.size() < MAX_QUEUED_FRAMES || encoding_broken;
        });
    }
    if (encoding_broken) return;
    frame_queue.push_back(std::move(frame));
    queue_changed.notify_all();
}

bool Recorder::take_queued_frame(std::vector<unsigned char>& frame) {
    std::unique_lock guard(lock);
    queue_changed.wait(guard, [this] {
        return closing || !frame_queue.empty();
    });
    if (frame_queue.empty()) return false;
    frame = std::move(frame_queue.front());
    frame_queue.pop_front();
    return true;
}

void Recorder::recycle_frame(std::vector<unsigned char>& frame) {
    std::lock_guard guard(lock);
    if (spare_frames.size() < MAX_SPARE_FRAMES) spare_frames.push_back(std::move(frame));
}

void Recorder::encode_loop() {
    while (true) {
        std::vector<unsigned char> frame;
        if (!take_queued_frame(frame)) return;
        queue_changed.notify_all();
        if (!encoding_broken && !Recorder::get().send_to_encoder(frame)) {
            {
                std::lock_guard guard(lock);
                encoding_broken = true;
            }
            queue_changed.notify_all();
        }
        recycle_frame(frame);
    }
}

void Recorder::stop_encode_thread() {
    if (encode_thread.joinable()) {
        {
            std::lock_guard guard(lock);
            closing = true;
        }
        queue_changed.notify_all();
        encode_thread.join();
    }
    frame_queue.clear();
    spare_frames.clear();
    closing = false;
}

#ifdef GEODE_IS_WINDOWS
#define FFMPEG_EXIT_TIMEOUT_MS 30000

static unsigned long long process_cpu_time(HANDLE process) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER kernel_time, user_time;
    kernel_time.LowPart = kernel.dwLowDateTime;
    kernel_time.HighPart = kernel.dwHighDateTime;
    user_time.LowPart = user.dwLowDateTime;
    user_time.HighPart = user.dwHighDateTime;
    return kernel_time.QuadPart + user_time.QuadPart;
}

void Recorder::close_ffmpeg_process() {
    Recorder::get().close_handle(video_pipe);
    if (ffmpeg_process) {
        const DWORD check_every_ms = 1000;
        unsigned long long last_cpu = process_cpu_time(ffmpeg_process);
        DWORD idle_ms = 0;
        while (WaitForSingleObject(ffmpeg_process, check_every_ms) == WAIT_TIMEOUT) {
            unsigned long long cpu = process_cpu_time(ffmpeg_process);
            idle_ms = cpu == last_cpu ? idle_ms + check_every_ms : 0;
            last_cpu = cpu;
            if (idle_ms >= FFMPEG_EXIT_TIMEOUT_MS) {
                TerminateProcess(ffmpeg_process, 0);
                break;
            }
        }
        CloseHandle(ffmpeg_process);
        ffmpeg_process = nullptr;
    }
}
#endif

#ifdef GEODE_IS_MACOS
#define FFMPEG_EXIT_TIMEOUT_SECONDS 60.0
#define FFMPEG_ERROR_LINE_LIMIT 160
#define PIPE_ERROR_LOG_DELAY_MS 100

void Recorder::close_ffmpeg_process() {
    if (video_pipe >= 0) close(video_pipe);
    video_pipe = -1;
    if (ffmpeg_process) {
        int code = wait_tool(ffmpeg_process, FFMPEG_EXIT_TIMEOUT_SECONDS, true);
        ffmpeg_process = 0;
        if (code != 0) {
            encode_error = last_log_line(ffmpeg_log, FFMPEG_ERROR_LINE_LIMIT);
            if (encode_error.empty()) encode_error = "ffmpeg did not finish the video";
        }
    }
    std::error_code ignored;
    if (encode_error.empty() && !ffmpeg_log.empty()) fs::remove(ffmpeg_log, ignored);
}
#endif

void Recorder::finish_video() {
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    while (Recorder::get().collect_pixel_buffer(true)) {
    }
#endif
    stop_encode_thread();
#ifdef ECHO_FFMPEG_API
    if (api_encoder) api_encoder->stop();
    api_encoder.reset();
#endif
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MACOS)
    close_ffmpeg_process();
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
    if (active) return false;
#ifdef ECHO_FFMPEG_API
    if (api_encoder) return false;
#endif
#ifdef GEODE_IS_WINDOWS
    return ffmpeg_process == nullptr && video_pipe == INVALID_HANDLE_VALUE;
#elif defined(GEODE_IS_MACOS)
    return ffmpeg_process == 0 && video_pipe < 0;
#else
    return true;
#endif
}

#ifdef ECHO_FFMPEG_API
bool Recorder::send_to_api_encoder(const std::vector<unsigned char>& frame) {
    auto result = api_encoder->writeFrame(frame);
    if (result.isErr()) {
        std::lock_guard guard(lock);
        failure_reason = result.unwrapErr();
    }
    return result.isOk();
}
#endif

bool Recorder::send_to_encoder(const std::vector<unsigned char>& frame) {
    ScopedTimer timer{ g_render_timing.encoder_write_ns };
#ifdef ECHO_FFMPEG_API
    if (api_encoder) return send_to_api_encoder(frame);
#endif
#ifdef GEODE_IS_WINDOWS
    return Recorder::get().write_all(video_pipe, frame.data(), frame.size());
#elif defined(GEODE_IS_MACOS)
    if (Recorder::get().write_all(video_pipe, frame.data(), frame.size())) return true;
    std::this_thread::sleep_for(chrono::milliseconds(PIPE_ERROR_LOG_DELAY_MS));
    auto reason = last_log_line(ffmpeg_log, FFMPEG_ERROR_LINE_LIMIT);
    std::lock_guard guard(lock);
    failure_reason = reason;
    return false;
#else
    return false;
#endif
}

const char* Recorder::rendering_message() const {
#if defined(GEODE_IS_IOS)
    return "Rendering";
#elif defined(ECHO_FFMPEG_API)
    return use_api ? "Rendering (FFmpeg API)" : nv12 ? "Rendering (GPU)"
                                                     : "Rendering";
#else
    return nv12 ? "Rendering (GPU)" : "Rendering";
#endif
}

void Recorder::start_capturing() {
    auto& logic = Logic::get();
    encode_thread = std::thread([] {
        Recorder::get().encode_loop();
    });
    uncap_frame_rate();
    active = true;
    logic.render_speed_locked = audio_speed;
    logic.message = rendering_message();
    if (!AudioCapture::get().attach(logic.render_music, logic.render_sfx, raw_audio_path)) {
        logic.message = "Rendering (audio capture failed, video only)";
    }
}

#ifdef ECHO_FFMPEG_API
std::string Recorder::open_api_encoder(int fps) {
#ifdef GEODE_IS_ANDROID
    install_encoder_options();
#endif
    const auto& logic = Logic::get();
    if (!Recorder::api_available()) return "FFmpeg API is not installed";
    auto encoder = std::make_unique<ffmpeg::events::Recorder>();
    if (!encoder->isValid()) return "FFmpeg API did not respond";
    auto codecs = ffmpeg::events::Recorder::getAvailableCodecs();
    auto codec_offered = [&](const std::string& name) {
        return codecs.empty() || std::ranges::find(codecs, name) != codecs.end();
    };
    std::vector<std::string> preferred_codecs;
#ifdef GEODE_IS_ANDROID
    if (logic.render_codec.empty() || logic.render_codec == "libx264")
        preferred_codecs.push_back("h264_mediacodec");
#endif
    std::vector<std::string> fallbacks{ logic.render_codec, "libx264", "h264_videotoolbox", "h264_mediacodec", "libopenh264", "mpeg4" };
    preferred_codecs.insert(preferred_codecs.end(), fallbacks.begin(), fallbacks.end());
    std::vector<std::string> codecs_to_try;
    std::ranges::copy_if(preferred_codecs, std::back_inserter(codecs_to_try), [&](const std::string& name) {
        return !name.empty() && codec_offered(name) && std::ranges::find(codecs_to_try, name) == codecs_to_try.end();
    });
#ifdef GEODE_IS_ANDROID
    auto color_modes = std::vector<bool>{ false };
#else
    auto color_modes = logic.render_color_fix ? std::vector<bool>{ true, false } : std::vector<bool>{ false };
#endif
    std::vector<std::pair<std::string, bool>> attempts;
    std::ranges::for_each(codecs_to_try, [&](const std::string& codec) {
        std::ranges::transform(color_modes, std::back_inserter(attempts), [&](bool color_fix) {
            return std::pair{ codec, color_fix };
        });
    });
    long long pixels = (long long)width * (long long)height;
    long long bitrate = logic.render_bitrate > 0 ? logic.render_bitrate : std::max<long long>(pixels * fps / 4, 8'000'000);
    std::string error = "No usable video codec";
    auto opened = std::ranges::find_if(attempts, [&](const auto& attempt) {
        const auto& [codec, color_fix] = attempt;
        ffmpeg::RenderSettings settings;
#ifdef GEODE_IS_WINDOWS
        settings.m_pixelFormat = ffmpeg::PixelFormat::BGRA;
#else
        settings.m_pixelFormat = ffmpeg::PixelFormat::RGBA;
#endif
        settings.m_codec = codec;
        settings.m_bitrate = bitrate;
        settings.m_width = (unsigned)width;
        settings.m_height = (unsigned)height;
        settings.m_fps = (unsigned short)std::clamp(fps, 1, 65535);
        settings.m_outputFile = output;
        settings.m_doVerticalFlip = true;
        settings.m_colorspaceFilters = color_fix ? "all=bt709:iall=bt470bg:fast=1" : "";
#ifdef GEODE_IS_ANDROID
        g_encoder_preset = codec == "libx264" ? logic.render_preset.c_str() : nullptr;
        g_force_bt709_colors = logic.render_color_fix;
        struct ClearEncoderOverrides {
            ~ClearEncoderOverrides() {
                g_encoder_preset = nullptr;
                g_force_bt709_colors = false;
            }
        } clear_overrides;
#endif
        encoder = std::make_unique<ffmpeg::events::Recorder>();
        auto result = encoder->init(settings);
        if (result.isErr()) error = result.unwrapErr();
        return result.isOk();
    });
    if (opened == attempts.end()) {
        std::error_code ignored;
        fs::remove(output, ignored);
        return error.empty() ? std::string("FFmpeg API could not open the encoder") : error;
    }
    api_encoder = std::move(encoder);
    return {};
}
#endif

static bool write_wav(const fs::path& raw, const fs::path& wav, int rate, int channels) {
    std::ifstream in(raw, std::ios::binary);
    std::ofstream out(wav, std::ios::binary | std::ios::trunc);
    if (!in || !out || rate <= 0 || channels <= 0) return false;
    auto write_value = [&](auto value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    out.write("RIFF", 4);
    write_value(0u);
    out.write("WAVEfmt ", 8);
    write_value(16u);
    write_value((unsigned short)1);
    write_value((unsigned short)2);
    write_value((unsigned)rate);
    write_value((unsigned)rate * 4u);
    write_value((unsigned short)4);
    write_value((unsigned short)16);
    out.write("data", 4);
    write_value(0u);
    auto to_pcm16 = [](float value) {
        return (short)std::lround((std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f) * 32767.f);
    };
    size_t frame_size = (size_t)channels;
    std::vector<float> block(frame_size * 65536);
    unsigned long long bytes = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(block.data()), (std::streamsize)(block.size() * sizeof(float)));
        size_t frame_count = (size_t)in.gcount() / (sizeof(float) * frame_size);
        if (frame_count == 0) break;
        auto pcm = std::views::iota(size_t{ 0 }, frame_count) | std::views::transform([&](size_t i) {
            const float* frame = block.data() + i * frame_size;
            return std::array<short, 2>{ to_pcm16(frame[0]), to_pcm16(frame_size > 1 ? frame[1] : frame[0]) };
        }) | std::ranges::to<std::vector>();
        out.write(reinterpret_cast<const char*>(pcm.data()), (std::streamsize)(pcm.size() * sizeof(pcm[0])));
        bytes += frame_count * 4;
    }
    auto size = (unsigned)(std::min<unsigned long long>(bytes, 4294967295ull - 36));
    out.seekp(4);
    write_value(36u + size);
    out.seekp(40);
    write_value(size);
    return bytes > 0 && out.good();
}

void Recorder::save_api_render() {
#ifdef ECHO_FFMPEG_API
    auto& audio_capture = AudioCapture::get();
    std::error_code error;
    bool has_audio = !raw_audio_path.empty() && frames > 0 && audio_capture.frames_written > 0 && audio_capture.sample_rate > 0 && audio_capture.channels > 0 && fs::exists(output, error);
    auto finish_saving = [](const fs::path& video, const std::string& failed) {
        Recorder::get().saving = false;
        auto& logic = Logic::get();
        if (failed.empty()) {
            logic.message = "Render stopped";
            logic.notify(fmt::format("Saved {}", geode::utils::string::pathToString(video.filename())), true);
        }
        else {
            logic.message = "Video saved, audio mux failed: " + failed;
            logic.notify(logic.message, false);
        }
    };
    if (!has_audio) {
        if (!raw_audio_path.empty()) fs::remove(raw_audio_path, error);
        if (frames > 0 && fs::exists(output, error))
            finish_saving(output, {});
        else Logic::get().message = "Render stopped";
        return;
    }
    saving = true;
    int audio_source_rate = (int)std::max(1ll, std::llround((double)(audio_capture.sample_rate) * std::max(audio_speed, 0.01f)));
    std::thread([finish_saving, video = output, raw = raw_audio_path, rate = audio_source_rate, channels = audio_capture.channels] {
        auto wav = fs::path(video).replace_extension(".audio.wav");
        auto temp = fs::path(video).replace_extension(".part");
        temp += video.extension();
        std::string failed;
        std::error_code error;
        std::vector<float> stereo;
        try {
#ifdef GEODE_IS_IOS
            stereo = read_captured_stereo(raw, channels);
#else
            if (rate == 44100) stereo = read_captured_stereo(raw, channels);
#endif
        }
        catch (const std::exception& error) {
            failed = error.what();
        }
        if (failed.empty()) {
#ifdef GEODE_IS_ANDROID
            g_force_global_header = true;
            struct ClearGlobalHeader {
                ~ClearGlobalHeader() {
                    g_force_global_header = false;
                }
            } clear_global_header;
#endif
            try {
#ifdef GEODE_IS_IOS
                if (auto mixed = ffmpeg::events::AudioMixer::mixVideoRaw(video, stereo, temp); mixed.isErr())
                    failed = mixed.unwrapErr();
#else
                if (rate == 44100) {
                    if (auto mixed = ffmpeg::events::AudioMixer::mixVideoRaw(video, stereo, temp); mixed.isErr())
                        failed = mixed.unwrapErr();
                }
                else if (!write_wav(raw, wav, rate, channels)) failed = "could not write the audio";
                else {
                    if (auto mixed = ffmpeg::events::AudioMixer::mixVideoAudio(video, wav, temp); mixed.isErr())
                        failed = mixed.unwrapErr();
                    else fs::remove(wav, error);
                }
#endif
            }
            catch (const std::exception& error) {
                failed = error.what();
            }
        }
        if (failed.empty() && !fs::exists(temp, error))
            failed = "FFmpeg API did not write the video";
        if (failed.empty()) {
            auto backup = video;
            backup += ".unmuxed";
            fs::rename(video, backup, error);
            if (!error) {
                fs::rename(temp, video, error);
                if (error) {
                    std::error_code restore_error;
                    fs::rename(backup, video, restore_error);
                }
                else fs::remove(backup, error);
            }
            if (error) failed = "could not replace the video";
        }
        if (failed.empty()) fs::remove(raw, error);
        else {
            fs::remove(temp, error);
            if (write_wav(raw, wav, rate, channels)) {
                fs::remove(raw, error);
                failed += "; audio kept in " + geode::utils::string::pathToString(wav.filename());
            }
            else failed += "; raw audio kept in " + geode::utils::string::pathToString(raw.filename());
        }
        Loader::get()->queueInMainThread([finish_saving, video, failed] {
            finish_saving(video, failed);
        });
    }).detach();
#endif
}

#ifdef GEODE_IS_WINDOWS
bool Recorder::mux_audio() {
    auto& audio_capture = AudioCapture::get();
    std::error_code error;
    if (raw_audio_path.empty()) return true;
    bool has_audio = frames > 0 && audio_capture.frames_written > 0 && audio_capture.sample_rate > 0 && audio_capture.channels > 0 && fs::exists(output, error);
    if (!has_audio) {
        fs::remove(raw_audio_path, error);
        return true;
    }
    auto folder = output.parent_path();
    auto log = folder / "echo-ffmpeg.log";
    auto temp = fs::path(output).replace_extension(".part");
    temp += output.extension();
    bool lossless = Logic::get().render_bitrate <= 0;
    auto extension = geode::utils::string::pathToString(output.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    std::string audio_codec = extension == ".webm" ? "-c:a libopus -b:a 320k" : lossless ? "-c:a flac"
                                                                                         : "-c:a aac -b:a 320k";
    long long source_rate = std::max(1ll, std::llround((double)(audio_capture.sample_rate) * std::max(audio_speed, 0.01f)));
    std::string command = fmt::format(
        "\"{}\" -y -nostdin -loglevel error -i \"{}\" -f f32le -ar {} -ac {} -i \"{}\" -map 0:v:0 -map 1:a:0 -c:v copy {} -ar {} -ac 2 -muxdelay 0 -muxpreload 0 -shortest \"{}\"",
        geode::utils::string::pathToString(Recorder::get().ffmpeg_path()),
        geode::utils::string::pathToString(output),
        source_rate,
        audio_capture.channels,
        geode::utils::string::pathToString(raw_audio_path),
        audio_codec,
        audio_capture.sample_rate,
        geode::utils::string::pathToString(temp));
    std::string failed = Recorder::get().run_hidden(command, log);
    fs::remove(log, error);
    if (failed.empty()) {
        fs::rename(temp, output, error);
        if (error) failed = "could not replace the video";
    }
    if (failed.empty()) {
        fs::remove(raw_audio_path, error);
        return true;
    }
    fs::remove(temp, error);
    auto wav = fs::path(output).replace_extension(".wav");
    if (write_wav(raw_audio_path, wav, (int)source_rate, audio_capture.channels)) {
        fs::remove(raw_audio_path, error);
        failed += "; audio kept in " + geode::utils::string::pathToString(wav.filename());
    }
    else {
        failed += "; raw audio kept in " + geode::utils::string::pathToString(raw_audio_path.filename());
    }
    Logic::get().message = "Video saved, audio mux failed: " + failed;
    return false;
}
#endif

#ifdef GEODE_IS_MACOS
bool Recorder::mux_audio() {
    auto& audio_capture = AudioCapture::get();
    std::error_code error;
    if (raw_audio_path.empty()) return true;
    bool has_audio = frames > 0 && audio_capture.frames_written > 0 && audio_capture.sample_rate > 0 && audio_capture.channels > 0 && fs::exists(output, error);
    if (!has_audio) {
        fs::remove(raw_audio_path, error);
        return true;
    }
    auto folder = output.parent_path();
    auto log = folder / "echo-ffmpeg.log";
    auto temp = folder / (output.stem().string() + ".part" + output.extension().string());
    bool lossless = Logic::get().render_bitrate <= 0;
    auto extension = output.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    std::vector<std::string> audio_codec = extension == ".webm" ? std::vector<std::string>{ "-c:a", "libopus", "-b:a", "320k" }
                                           : lossless           ? std::vector<std::string>{ "-c:a", "flac" }
                                                                : std::vector<std::string>{ "-c:a", "aac", "-b:a", "320k" };
    long long source_rate = std::max(1ll, std::llround((double)(audio_capture.sample_rate) * std::max(audio_speed, 0.01f)));
    std::vector<std::string> args{
        Recorder::ffmpeg_path().string(), "-y", "-nostdin", "-loglevel", "error", "-i", output.string(),
        "-f", "f32le", "-ar", std::to_string(source_rate), "-ac", std::to_string(audio_capture.channels), "-i", raw_audio_path.string(),
        "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy"
    };
    args.insert(args.end(), audio_codec.begin(), audio_codec.end());
    args.insert(args.end(), { "-ar", std::to_string(audio_capture.sample_rate), "-ac", "2", "-muxdelay", "0", "-muxpreload", "0", "-shortest", temp.string() });
    std::string failed = run_tool(args, log, {}, 180.0);
    fs::remove(log, error);
    if (failed.empty()) {
        fs::rename(temp, output, error);
        if (error) failed = "could not replace the video";
    }
    if (failed.empty()) {
        fs::remove(raw_audio_path, error);
        return true;
    }
    fs::remove(temp, error);
    auto wav = output.parent_path() / (output.stem().string() + ".wav");
    if (write_wav(raw_audio_path, wav, (int)source_rate, audio_capture.channels)) {
        fs::remove(raw_audio_path, error);
        failed += "; audio kept in " + wav.filename().string();
    }
    else {
        failed += "; raw audio kept in " + raw_audio_path.filename().string();
    }
    Logic::get().message = "Video saved, audio mux failed: " + failed;
    return false;
}
#endif

static float level_progress(PlayLayer* play, int last_input_frame) {
    if (!play) return -1.f;
    if (!play->m_isPlatformer && play->m_player1 && play->m_endPortal) {
        float end_x = play->m_endPortal->getPositionX();
        if (end_x > 0.f) return std::clamp(play->m_player1->getPositionX() / end_x, 0.f, 1.f);
    }
    if (Logic::get().is_playing() && last_input_frame > 0) {
        return std::clamp((float)(Logic::get().step_frame) / (float)(last_input_frame), 0.f, 1.f);
    }
    return -1.f;
}

static std::string duration_text(double seconds) {
    int total_seconds = (int)(std::max(seconds, 0.0) + 0.5);
    if (total_seconds >= 3600)
        return fmt::format("{}:{:02}:{:02}", total_seconds / 3600, total_seconds / 60 % 60, total_seconds % 60);
    return fmt::format("{}:{:02}", total_seconds / 60, total_seconds % 60);
}

RenderStatus Logic::render_status() {
    auto& recorder = Recorder::get();
    RenderStatus status;
    if (!recorder.active) return status;
    status.active = true;
    if (recorder.paused) {
        status.text = "Render paused";
        return status;
    }
    auto* play = PlayLayer::get();
    double video_seconds = (double)(recorder.frames) * (double)(render_step());
    if (recorder.busy_seconds > 0.05) status.speed = (float)(video_seconds / recorder.busy_seconds);
    auto& speed_samples = recorder.speed_samples;
    if (!speed_samples.empty() && (recorder.busy_seconds < speed_samples.back().first || video_seconds < speed_samples.back().second)) {
        speed_samples.clear();
        recorder.recent_speed = 0.0;
    }
    if (speed_samples.empty() || recorder.busy_seconds - speed_samples.back().first >= 0.5) {
        speed_samples.emplace_back(recorder.busy_seconds, video_seconds);
        while (speed_samples.size() > 2 && recorder.busy_seconds - speed_samples[1].first >= 10.0)
            speed_samples.pop_front();
        if (double wall_seconds = recorder.busy_seconds - speed_samples.front().first; wall_seconds >= 1.0)
            recorder.recent_speed = (video_seconds - speed_samples.front().second) / wall_seconds;
    }
    double pace = recorder.recent_speed > 0.0 ? recorder.recent_speed : (double)(status.speed);
    std::string speed = pace > 0.0 ? fmt::format("{:.2f}x", pace) : std::string("measuring speed");
    std::string elapsed = duration_text(recorder.busy_seconds) + " elapsed";
    if (play && play->m_hasCompletedLevel) {
        status.known = true;
        status.finishing = true;
        status.progress = 1.f;
        status.text = fmt::format("Finishing  |  {}  |  {}", speed, elapsed);
        return status;
    }
    double game_seconds_left = -1.0;
    if (play && !play->m_isPlatformer && play->m_player1 && play->m_endPortal) {
        int channel = play->m_gameState.m_currentChannel;
        double end_time = play->timeForPos(play->m_endPortal->getPosition(), 0, channel, true, 0);
        double player_time = play->timeForPos(play->m_player1->getPosition(), 0, channel, true, 0);
        if (end_time > 0.0) game_seconds_left = std::max(end_time - player_time, 0.0);
    }
    double game_speed = std::max((double)(render_speed()), 0.01);
    double after_end_seconds = (double)(render_after_end);
    if (game_seconds_left >= 0.0) {
        double video_seconds_left = game_seconds_left / game_speed + after_end_seconds;
        double total_video = video_seconds + video_seconds_left;
        status.known = true;
        status.progress = total_video > 0.0 ? (float)(std::clamp(video_seconds / total_video, 0.0, 1.0)) : 0.f;
        std::string time_left = "estimating";
        if (pace > 0.0 && recorder.busy_seconds > 1.0) {
            status.eta = (float)(video_seconds_left / pace);
            time_left = duration_text(status.eta) + " left";
        }
        status.text = fmt::format("{:.1f}%  |  {}  |  {}  |  {}", status.progress * 100.f, speed, elapsed, time_left);
        return status;
    }
    float progress_now = level_progress(play, recorder.last_input_frame);
    if (progress_now < 0.f) {
        status.text = fmt::format("{} rendered  |  {}  |  {}", duration_text(video_seconds), speed, elapsed);
        return status;
    }
    status.known = true;
    status.progress = progress_now;
    float progress_made = progress_now - std::max(recorder.start_progress, 0.f);
    std::string time_left = "estimating";
    if (progress_made > 0.002f && recorder.busy_seconds > 0.5) {
        double after_end_video = pace > 0.0 ? after_end_seconds / pace : 0.0;
        status.eta = (float)(recorder.busy_seconds * (1.0 - progress_now) / progress_made + after_end_video);
        time_left = duration_text(status.eta) + " left";
    }
    status.text = fmt::format("{:.1f}%  |  {}  |  {}  |  {}", progress_now * 100.f, speed, elapsed, time_left);
    return status;
}

float Logic::render_wall_delta() {
    return Recorder::get().wall_delta;
}

void Logic::sync_render_pause_button() {
    static CCNode* hidden_button = nullptr;
    auto* play = PlayLayer::get();
    CCNode* button = play && play->m_uiLayer ? play->m_uiLayer->m_pauseBtn : nullptr;
    bool should_hide = button && render_hide_pause && Recorder::get().active;
#ifndef GEODE_IS_MOBILE
    should_hide = false;
#endif
    if (hidden_button && hidden_button != button) hidden_button = nullptr;
    if (should_hide && !hidden_button && button->isVisible()) {
        button->setVisible(false);
        hidden_button = button;
    }
    else if (!should_hide && hidden_button) {
        hidden_button->setVisible(true);
        hidden_button = nullptr;
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
    if (!render_uses_screen_shape()) return width;
    auto* view = CCEGLView::get();
    if (!view) return width;
    auto frame = view->getFrameSize();
    if (Recorder::get().view_held) frame = Recorder::get().saved_frame_size;
    if (frame.width <= 1.f || frame.height <= 1.f) return width;
    float wide = std::max(frame.width, frame.height);
    float tall = std::min(frame.width, frame.height);
    return std::max((int)std::lround((float)(height) * wide / tall), 2) & ~1;
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
    return Recorder::get().view_held;
}

bool Logic::render_blocks_engine() {
    return AudioCapture::get().attached && !AudioCapture::get().engine_update_allowed;
}

bool Logic::render_blocks_mixer() {
    return AudioCapture::get().attached && !AudioCapture::get().mixer_allowed;
}

bool Logic::render_should_skip() {
    auto* play = PlayLayer::get();
    if (!play || render_paused()) return true;
    if (pause_menu_open()) return true;
    bool started = play->m_started || (render_record_pause && Recorder::get().seen_frames >= 2);
    if (!started) return true;
    if (play->m_isPaused) return true;
    return false;
}

bool Logic::begin_render_frame() {
    auto& recorder = Recorder::get();
    if (!recorder.active) return false;
    auto now = chrono::steady_clock::now();
    double since_last_tick = recorder.ticked ? chrono::duration<double>(now - recorder.last_tick).count() : 0.0;
    recorder.last_tick = now;
    recorder.ticked = true;
    recorder.wall_delta = (float)(std::clamp(since_last_tick, 0.0, 0.25));
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &recorder.screen_fbo);
    ++recorder.seen_frames;
    recorder.hold_view();
    bool should_capture = !render_should_skip();
    if (should_capture) {
        recorder.busy_seconds += std::clamp(since_last_tick, 0.0, 30.0);
        AudioCapture::get().keep_effects_playing();
    }
    return should_capture;
}

void Logic::hold_render_effects() {
    AudioCapture::get().keep_effects_playing();
}

void Logic::show_render_overlay() {
    Recorder::get().show_overlay();
}

#define RENDER_NOTE_EVERY_SECONDS 5
#define MAX_RENDER_NOTES_SIZE 60000

static float render_fade_amount(const Logic& logic, const Recorder& recorder, PlayLayer* play, float dt, float fade_in_seconds, float fade_out_seconds) {
    float fade_in = fade_in_seconds <= 0.f ? 1.f : std::clamp((float)(recorder.frames) * dt / fade_in_seconds, 0.f, 1.f);
    float fade_out = 1.f;
    if (play->m_hasCompletedLevel && fade_out_seconds > 0.f) {
        fade_out = std::clamp((logic.render_after_end - recorder.time_since_end) / fade_out_seconds, 0.f, 1.f);
    }
    return std::min(fade_in, fade_out);
}

static void append_render_note(Logic& logic, const Recorder& recorder, PlayLayer* play, float dt) {
    auto* engine = FMODAudioEngine::get();
    auto& audio_capture = AudioCapture::get();
    unsigned raw_song = 0;
    if (auto* channel = engine ? engine->getActiveMusicChannel(0) : nullptr)
        channel->getPosition(&raw_song, FMOD_TIMEUNIT_MS);
    g_render_notes += fmt::format("at video={:.3f}s level_time={:.4f}s song={}ms raw_song={}ms audio_written={:.3f}s x={:.3f} tps={:g} step_frame={}\n",
        (double)recorder.frames * dt, play->m_gameState.m_levelTime, engine ? engine->getMusicTimeMS(0) : 0, raw_song,
        audio_capture.sample_rate > 0 ? (double)audio_capture.frames_written / audio_capture.sample_rate : 0.0, play->m_player1 ? play->m_player1->getPositionX() : 0.f,
        logic.sample_rate(), logic.step_frame);
}

static std::string encoding_failure_message(Recorder& recorder) {
    std::string failure;
    {
        std::lock_guard guard(recorder.lock);
        failure = recorder.failure_reason;
    }
    std::string reason = recorder.use_api ? "Encoding failed: " + failure : std::string("The render pipe closed");
#ifdef GEODE_IS_MACOS
    if (!recorder.use_api && !failure.empty()) reason = "Encoding failed: " + failure;
#endif
    return reason;
}

bool Logic::capture_frame() {
    auto& recorder = Recorder::get();
    auto* play = PlayLayer::get();
    if (!recorder.active || !play) return false;
    float dt = render_step();
    if (play->m_hasCompletedLevel) {
        recorder.time_since_end += dt;
        if (recorder.time_since_end >= render_after_end) {
            bool quit_after = render_auto_exit;
            stop_render();
            if (quit_after) {
                Loader::get()->queueInMainThread([] {
                    if (auto* layer = PlayLayer::get()) layer->onQuit();
                });
            }
            return false;
        }
    }
    recorder.fade = render_fade_amount(*this, recorder, play, dt, render_fade_in, render_fade_out);
    recorder.video_fade = render_fade_amount(*this, recorder, play, dt, render_video_fade_in, render_video_fade_out);
    if (recorder.start_progress < 0.f) recorder.start_progress = level_progress(play, recorder.last_input_frame);
    AudioCapture::get().advance(dt * recorder.audio_speed, recorder.fade);
    recorder.draw_and_read_frame();
    bool note_due = recorder.frames % (std::max(render_fps, 1) * RENDER_NOTE_EVERY_SECONDS) == 0;
    if (note_due && g_render_notes.size() < MAX_RENDER_NOTES_SIZE)
        append_render_note(*this, recorder, play, dt);
    ++recorder.frames;
    if (recorder.encoding_broken) {
        std::string reason = encoding_failure_message(recorder);
        stop_render();
        notify(reason, false);
    }
    return true;
}

bool Logic::render_batch_more(chrono::steady_clock::time_point started) {
#ifdef GEODE_IS_WINDOWS
    return false;
#else
    // showing the window costs 10-20ms so batches are long, short w/ the menu open
    auto longest = chrono::milliseconds(GUI::get().menu_shown() ? 25 : 200);
    return Recorder::get().active && !render_should_skip() && chrono::steady_clock::now() - started < longest;
#endif
}

bool Logic::ffmpeg_ready() {
#ifdef GEODE_IS_WINDOWS
    return fs::exists(Recorder::get().game_directory() / "ffmpeg.exe") || Recorder::api_available();
#elif defined(GEODE_IS_MACOS)
    static chrono::steady_clock::time_point last_checked{};
    static bool ffmpeg_found = false;
    auto now = chrono::steady_clock::now();
    if (last_checked == chrono::steady_clock::time_point{} || now - last_checked > chrono::seconds(2)) {
        ffmpeg_found = !Recorder::ffmpeg_path().empty();
        last_checked = now;
    }
    return ffmpeg_found || Recorder::api_available();
#else
    return Recorder::api_available();
#endif
}

static void write_render_log(const Recorder& recorder) {
    double frames = (double)(recorder.frames);
    double wall_seconds = chrono::duration<double>(chrono::steady_clock::now() - g_render_timing.started_at).count();
    auto ms_per_frame = [frames](const std::atomic<long long>& ns) {
        return (double)(ns.load()) / 1e6 / frames;
    };
    auto& logic = Logic::get();
    auto& audio_capture = AudioCapture::get();
    std::error_code error;
    fs::create_directories(logic.save_root() / "logs", error);
    std::ofstream render_log(logic.save_root() / "logs" / "render.txt", std::ios::trunc);
    render_log << fmt::format("echo {} build {}\n", Mod::get()->getVersion().toVString(), build_id());
    render_log << fmt::format("render {}x{} @{}fps codec={} platform={} capture={}\n", recorder.width, recorder.height, logic.render_fps, logic.render_codec,
        GEODE_PLATFORM_NAME, recorder.use_api ? "api" : recorder.nv12 ? "nv12 ring" : recorder.buffered_capture ? "bgra ring" : "bgra direct");
    render_log << fmt::format("frames={} video={:.3f}s wall={:.3f}s speed={:.2f} fps ({:.2f}x real time)\n", recorder.frames, frames / std::max(logic.render_fps, 1), wall_seconds,
        frames / std::max(wall_seconds, 1e-6), frames / std::max(logic.render_fps, 1) / std::max(wall_seconds, 1e-6));
    render_log << fmt::format("per frame: total={:.2f}ms read_back={:.2f}ms waiting_on_encoder={:.2f}ms encoder_write={:.2f}ms (encoder is on its own thread)\n",
        wall_seconds * 1000.0 / frames, ms_per_frame(g_render_timing.read_back_ns), ms_per_frame(g_render_timing.encoder_wait_ns), ms_per_frame(g_render_timing.encoder_write_ns));
    render_log << fmt::format("physics={:.2f}ms per frame ({:.0f}% of the frame) steps_per_frame={:.0f}\n", logic.render_physics * 1000.0 / frames,
        logic.render_physics / std::max(wall_seconds, 1e-6) * 100.0, logic.sample_rate() / std::max(logic.render_fps, 1));
    render_log << fmt::format("ssb_fix={} factor low={:.6f} high={:.6f} mean={:.6f} samples={} tps={:g} render_speed={:g}\n", logic.render_ssb_fix, logic.render_factor_low,
        logic.render_factor_high, logic.render_factor_count > 0 ? logic.render_factor_sum / logic.render_factor_count : 1.0, logic.render_factor_count,
        logic.sample_rate(), recorder.audio_speed);
    render_log << fmt::format("audio rate={} channels={} written={} ({:.3f}s) stray_blocks={}\n", audio_capture.sample_rate, audio_capture.channels, audio_capture.frames_written,
        audio_capture.sample_rate > 0 ? (double)audio_capture.frames_written / audio_capture.sample_rate : 0.0, audio_capture.stray_blocks);
    {
        auto* play = PlayLayer::get();
        auto* engine = FMODAudioEngine::get();
        auto* manager = GameManager::get();
        render_log << fmt::format("level song_offset={:.4f}s gd_music_offset={}ms gd_time_offset={:.4f} song_at_start={}ms\n",
            play && play->m_levelSettings ? play->m_levelSettings->m_songOffset : -1.f, engine ? engine->m_musicOffset : 0,
            manager ? manager->m_timeOffset : 0.f, audio_capture.song_start_ms);
    }
    render_log << g_render_notes;
}

void Logic::stop_render() {
    auto& recorder = Recorder::get();
    if (recorder.active && recorder.frames > 0) write_render_log(recorder);
#ifdef GEODE_IS_WINDOWS
    if (recorder.starting && !recorder.active) {
        recorder.cancel = true;
        if (recorder.video_pipe != INVALID_HANDLE_VALUE) CancelIoEx(recorder.video_pipe, nullptr);
        message = "Render canceled";
        return;
    }
#endif
    if (recorder.idle()) return;
    recorder.starting = false;
    recorder.cancel = false;
    recorder.active = false;
    render_speed_locked = 0.f;
    recorder.restore_frame_rate();
    AudioCapture::get().detach();
    recorder.finish_video();
    recorder.close_capture_target();
    recorder.release_view();
    if (recorder.use_api) {
        recorder.save_api_render();
        return;
    }
#ifdef GEODE_IS_WINDOWS
    bool muxed = recorder.mux_audio();
    if (muxed) message = "Render stopped";
#endif
#ifdef GEODE_IS_MACOS
    if (!recorder.encode_error.empty()) {
        message = "Render failed: " + recorder.encode_error;
        notify(message, false);
        std::error_code ignored;
        fs::remove(recorder.raw_audio_path, ignored);
    }
    else if (recorder.mux_audio()) {
        message = "Render stopped";
        if (recorder.frames > 0)
            notify(fmt::format("Saved {}", recorder.output.filename().string()), true);
    }
#endif
}

#define MAX_RENDER_WIDTH 7680
#define MAX_RENDER_HEIGHT 4320

static void reset_for_new_render(Logic& logic, Recorder& recorder, const fs::path& output, const fs::path& folder) {
    recorder.output = output;
    recorder.raw_audio_path = folder / output.stem();
    recorder.raw_audio_path += ".audio.raw";
    recorder.paused = false;
    recorder.frames = 0;
    g_render_timing.reset();
    g_render_notes.clear();
    recorder.recent_speed = 0.0;
    recorder.speed_samples.clear();
    {
        logic.render_game_seconds = 0.0;
        logic.render_factor_sum = 0.0;
        logic.render_factor_low = logic.render_factor_high = 1.f;
        logic.render_factor_count = 0;
    }
    recorder.seen_frames = 0;
    recorder.time_since_end = 0.f;
    recorder.fade = 1.f;
    recorder.video_fade = 1.f;
    recorder.cancel = false;
    recorder.encoding_broken = false;
    recorder.failure_reason.clear();
    recorder.ticked = false;
    recorder.wall_delta = 0.f;
    recorder.busy_seconds = 0.0;
    recorder.start_progress = -1.f;
    recorder.last_input_frame = logic.inputs.empty() ? 0 : std::ranges::max(logic.inputs, {}, &Frame::number).number;
    logic.ffmpeg_missing = false;
    logic.render_queued = false;
    logic.frame_advance = false;
    logic.holding_advance = false;
    logic.advance_queued = false;
}

#ifdef GEODE_IS_WINDOWS
#define PIPE_BUFFER_FRAMES 4
#define MAX_PIPE_BUFFER_BYTES (256u << 20)

static HANDLE open_video_pipe(const std::string& video_name, size_t frame_bytes) {
    DWORD pipe_mode = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED;
    DWORD pipe_buffer_size = (DWORD)std::min(std::max(frame_bytes * PIPE_BUFFER_FRAMES, frame_bytes), (size_t)MAX_PIPE_BUFFER_BYTES);
    return CreateNamedPipeA(video_name.c_str(), pipe_mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, pipe_buffer_size, 0, 0, nullptr);
}

static std::string ffmpeg_command(const Logic& logic, const Recorder& recorder, const std::string& video_name, int width, int height, int fps, const fs::path& output) {
    std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
    bool hardware_encoder = codec.find("nvenc") != std::string::npos || codec.find("amf") != std::string::npos || codec.find("qsv") != std::string::npos;
    const char* output_pixel_format = logic.render_bitrate <= 0 && !hardware_encoder ? "yuv444p" : "yuv420p";
    // shader already did bt709 DONT do it twice
    std::string video_filter;
    if (!recorder.nv12)
        video_filter = logic.render_color_fix ? "-vf vflip,colorspace=all=bt709:iall=bt470bg:fast=1 " : "-vf vflip ";
    auto has_arg = [&](const char* key) {
        return logic.render_args.find(key) != std::string::npos;
    };
    std::string pixel_format_args = has_arg("-pix_fmt") ? std::string() : fmt::format(" -pix_fmt {}", output_pixel_format);
    std::string colorspace_args = has_arg("-colorspace") || has_arg("-color_primaries") || has_arg("-color_trc") || has_arg("-color_range")
                            ? std::string()
                            : " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
    std::string extra = logic.render_args.empty() ? std::string() : " " + logic.render_args;
    return fmt::format(
        "\"{}\" -y -nostdin -loglevel error -f rawvideo -pix_fmt {} -s {}x{} -r {} -probesize 32 -analyzeduration 0 -fflags nobuffer -i {} -an "
        "{}-c:v {}{}{}{}{} \"{}\"",
        geode::utils::string::pathToString(Recorder::get().ffmpeg_path()),
        recorder.nv12 ? "nv12" : "bgra",
        width,
        height,
        fps,
        video_name,
        video_filter,
        codec,
        rate_control_args(codec, logic.render_bitrate, logic.render_args),
        pixel_format_args,
        colorspace_args,
        extra,
        geode::utils::string::pathToString(output));
}

static bool launch_hidden(const std::string& command, PROCESS_INFORMATION& process) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    std::wstring mutable_command = geode::utils::string::utf8ToWide(command);
    return CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
}

static void finish_starting_render(bool connected) {
    auto& recorder = Recorder::get();
    bool canceled = recorder.cancel;
    recorder.starting = false;
    recorder.cancel = false;
    if (!connected || canceled) {
        Logic::get().stop_render();
        if (!connected && !canceled) {
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
    recorder.start_capturing();
}

static void connect_ffmpeg_then_start() {
    bool connected = Recorder::get().connect_pipe(Recorder::get().video_pipe);
    Loader::get()->queueInMainThread([connected] {
        finish_starting_render(connected);
    });
}
#endif

#ifdef GEODE_IS_MACOS
static std::vector<std::string> ffmpeg_args(const Logic& logic, const fs::path& ffmpeg, int width, int height, int fps, const fs::path& output) {
    std::string codec = logic.render_codec.empty() ? "libx264" : logic.render_codec;
    bool hardware_encoder = codec.find("videotoolbox") != std::string::npos;
    auto has_arg = [&](const char* key) {
        return logic.render_args.find(key) != std::string::npos;
    };
    std::string filters = "vflip";
    if (logic.render_color_fix) filters += ",colorspace=all=bt709:iall=bt470bg:fast=1";
    std::vector<std::string> args{
        ffmpeg.string(), "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra",
        "-s", fmt::format("{}x{}", width, height), "-r", std::to_string(fps), "-i", "pipe:0", "-an",
        "-vf", filters, "-c:v", codec
    };
    auto rate_args = rate_control_args(codec, logic.render_bitrate, logic.render_args, (long long)width * height * fps);
    args.insert(args.end(), rate_args.begin(), rate_args.end());
    if (!has_arg("-pix_fmt"))
        args.insert(args.end(), { "-pix_fmt", logic.render_bitrate <= 0 && !hardware_encoder ? "yuv444p" : "yuv420p" });
    if (!has_arg("-colorspace") && !has_arg("-color_primaries") && !has_arg("-color_trc") && !has_arg("-color_range"))
        args.insert(args.end(), { "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv" });
    auto extra = split_args(logic.render_args);
    args.insert(args.end(), extra.begin(), extra.end());
    args.push_back(output.string());
    return args;
}

static void set_render_pipe_flags(const int* pipe_ends) {
    fcntl(pipe_ends[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_ends[1], F_SETFD, FD_CLOEXEC);
    fcntl(pipe_ends[1], F_SETNOSIGPIPE, 1);
}
#endif

void Logic::start_render() {
    auto& recorder = Recorder::get();
    if (recorder.active || recorder.starting) return;
    if (recorder.saving) {
        notify("Still saving the last render", false);
        return;
    }
    auto* play = PlayLayer::get();
    if (!play) {
        render_queued = true;
        notify("Rendering starts when you enter a level", true);
        return;
    }
    int height = std::max(render_height, 2) & ~1;
    int width = std::max(render_width_for(render_width, height), 2) & ~1;
    if (width > MAX_RENDER_WIDTH || height > MAX_RENDER_HEIGHT) {
        notify("Render size is too large. Use 7680x4320 or smaller.", false);
        return;
    }
    int fps = std::max(render_fps, 1);
    std::string extension = render_extension.empty() ? "mp4" : render_extension;
    if (extension.front() == '.') extension.erase(extension.begin());
    render_speed_locked = 0.f;
    recorder.audio_speed = render_speed();
    auto folder = save_root() / "renders";
    std::error_code folder_error;
    fs::create_directories(folder, folder_error);
    if (folder_error || !fs::is_directory(folder, folder_error)) {
        notify("Could not create the renders folder", false);
        return;
    }
    auto output = ReplayIO::unique_file(folder, ReplayIO::sanitize(render_file_stem()), "." + extension);
    recorder.width = width;
    recorder.height = height;
    recorder.use_api = Recorder::prefer_api();
#ifdef ECHO_FFMPEG_API
    if (recorder.use_api && !Recorder::api_available()) {
        ffmpeg_missing = true;
        message = "FFmpeg API is not installed";
        notify("Rendering needs the FFmpeg API mod. Install it from the Geode mod index.", false);
        return;
    }
#endif
    if (!recorder.open_capture_target()) {
        recorder.close_capture_target();
        notify("Could not create the render target", false);
        return;
    }
#ifdef ECHO_FFMPEG_API
    if (recorder.use_api) {
        recorder.output = output;
        if (auto failed = recorder.open_api_encoder(fps); !failed.empty()) {
            recorder.close_capture_target();
            message = "FFmpeg API failed: " + failed;
            notify(message, false);
            return;
        }
        reset_for_new_render(*this, recorder, output, folder);
        recorder.start_capturing();
        return;
    }
#endif
#ifdef GEODE_IS_WINDOWS
    DWORD pipe_id = GetTickCount();
    std::string video_name = fmt::format("\\\\.\\pipe\\echo-film-v-{}", pipe_id);
    recorder.video_pipe = open_video_pipe(video_name, recorder.frame_bytes());
    if (recorder.video_pipe == INVALID_HANDLE_VALUE) {
        recorder.close_capture_target();
        notify("Could not open the render pipe", false);
        return;
    }
    std::string command = ffmpeg_command(*this, recorder, video_name, width, height, fps, output);
    PROCESS_INFORMATION process{};
    if (!launch_hidden(command, process)) {
        recorder.close_capture_target();
        Recorder::get().close_handle(recorder.video_pipe);
        ffmpeg_missing = true;
        message = "ffmpeg.exe was not found";
        notify("ffmpeg.exe was not found. Use the download link on the Render tab, then place ffmpeg.exe next to Geometry Dash.", false);
        GUI::get().reveal_menu();
        return;
    }
    CloseHandle(process.hThread);
    recorder.ffmpeg_process = process.hProcess;
    reset_for_new_render(*this, recorder, output, folder);
    recorder.starting = true;
    message = "Starting render...";
    std::thread(connect_ffmpeg_then_start).detach();
#elif defined(GEODE_IS_MACOS)
    auto ffmpeg = Recorder::ffmpeg_path();
    if (ffmpeg.empty()) {
        recorder.close_capture_target();
        ffmpeg_missing = true;
        message = "ffmpeg was not found";
        notify("ffmpeg was not found. Install it with Homebrew (brew install ffmpeg), then render again.", false);
        GUI::get().reveal_menu();
        return;
    }
    auto args = ffmpeg_args(*this, ffmpeg, width, height, fps, output);
    int pipe_ends[2] = { -1, -1 };
    if (pipe(pipe_ends) != 0) {
        recorder.close_capture_target();
        notify("Could not open the render pipe", false);
        return;
    }
    set_render_pipe_flags(pipe_ends);
    recorder.ffmpeg_log = folder / (output.stem().string() + ".ffmpeg.log");
    pid_t pid = spawn_tool(args, pipe_ends[0], {}, recorder.ffmpeg_log);
    close(pipe_ends[0]);
    if (!pid) {
        close(pipe_ends[1]);
        recorder.close_capture_target();
        notify("Could not start ffmpeg", false);
        return;
    }
    recorder.ffmpeg_process = pid;
    recorder.video_pipe = pipe_ends[1];
    recorder.encode_error.clear();
    reset_for_new_render(*this, recorder, output, folder);
    recorder.start_capturing();
#else
    recorder.close_capture_target();
#endif
}
