#include "Logic/logic.hpp"
#include <Geode/fmod/fmod.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace chrono = std::chrono;

using namespace geode::prelude;

enum ClickType {
    MICRO_CLICK,
    SOFT_CLICK,
    HARD_CLICK,
    NORMAL_CLICK,
    CLICK_TYPES
};

struct SoundFolder {
    bool searched = false;
    fs::path path;
    std::vector<fs::path> files;
};

// W optimization?
struct Clickpack {
    std::string setting;
    fs::path folder;
    chrono::steady_clock::time_point checked_at;
    std::array<SoundFolder, CLICK_TYPES * 2> folders;
    std::map<fs::path, FMOD::Sound*> sounds;
    FMOD::System* fmod = nullptr;
};

struct QueuedClick {
    int folder = 0;
    float volume = 1.f;
    bool player1 = true;
};

#define CLICK_PRIORITY 256

class Clickbot {
public:
    static Clickbot& get() {
        static Clickbot value;
        return value;
    }

    std::string lowercase(std::string text);
    fs::path find_pack_folder(const std::string& setting);
    Clickpack& pack(bool player1);
    const fs::path& refresh_pack(bool player1, const std::string& setting);
    fs::path find_subfolder(const fs::path& pack_folder, std::initializer_list<const char*> names);
    bool is_audio_file(const fs::path& path);
    std::vector<fs::path> list_audio_files(const fs::path& folder);
    void forget_finished();
    float volume_gain(float volume);
    FMOD::Sound* load_sound(Clickpack& pack, FMOD::System* fmod, const fs::path& file);
    void unload_sounds(Clickpack& pack);
    void play_file(bool player1, const fs::path& file, float volume);
    fs::path find_type_folder(const fs::path& pack_folder, int type, bool down);
    const SoundFolder& sound_folder(bool player1, int index);
    std::array<Clickpack, 2> packs{};
    std::vector<QueuedClick> queue;
    std::vector<FMOD::Channel*> playing;
};

static int folder_index(int type, bool down) {
    return type * 2 + (down ? 1 : 0);
}

std::string Clickbot::lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return text;
}

fs::path Clickbot::find_pack_folder(const std::string& setting) {
    fs::path path(setting);
    std::error_code error;
    if (!setting.empty() && fs::is_directory(path, error)) return path;
    if (setting.empty())
        return {};
    auto nested = Logic::get().save_root() / "clickpacks" / setting;
    if (fs::is_directory(nested, error)) return nested;
    auto by_name = Logic::get().save_root() / "clickpacks" / fs::path(setting).filename();
    if (fs::is_directory(by_name, error)) return by_name;
    return path;
}

Clickpack& Clickbot::pack(bool player1) {
    return packs[player1 ? 0 : 1];
}

const fs::path& Clickbot::refresh_pack(bool player1, const std::string& setting) {
    auto& current = pack(player1);
    auto now = chrono::steady_clock::now();
    bool stale = current.setting != setting || now - current.checked_at > chrono::seconds(2);
    if (!stale) return current.folder;
    auto folder = find_pack_folder(setting);
    if (folder != current.folder) unload_sounds(current);
    current.setting = setting;
    current.folder = std::move(folder);
    current.checked_at = now;
    current.folders = {};
    return current.folder;
}

const SoundFolder& Clickbot::sound_folder(bool player1, int index) {
    auto& current = pack(player1);
    auto& folder = current.folders[(size_t)index];
    if (folder.searched) return folder;
    folder.path = find_type_folder(current.folder, index / 2, index % 2 == 1);
    folder.files = list_audio_files(folder.path);
    folder.searched = true;
    return folder;
}

fs::path Clickbot::find_subfolder(const fs::path& pack_folder, std::initializer_list<const char*> names) {
    std::error_code error;
    if (pack_folder.empty() || !fs::is_directory(pack_folder, error))
        return {};
    std::vector<std::string> wanted;
    for (const char* name : names)
        wanted.push_back(lowercase(name));
    for (const auto& entry : fs::directory_iterator(pack_folder, error)) {
        if (error || !entry.is_directory()) continue;
        auto folder_name = lowercase(geode::utils::string::pathToString(entry.path().filename()));
        for (const auto& name : wanted) {
            if (folder_name == name) return entry.path();
        }
    }
    return {};
}

bool Clickbot::is_audio_file(const fs::path& path) {
    auto ext = lowercase(geode::utils::string::pathToString(path.extension()));
    return ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

std::vector<fs::path> Clickbot::list_audio_files(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return files;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
        if (error) break;
        if (entry.is_regular_file() && is_audio_file(entry.path()))
            files.push_back(entry.path());
    }
    return files;
}

void Clickbot::forget_finished() {
    std::erase_if(playing, [](FMOD::Channel* channel) {
        bool still_playing = false;
        if (channel) channel->isPlaying(&still_playing);
        return !still_playing;
    });
}

FMOD::Sound* Clickbot::load_sound(Clickpack& pack, FMOD::System* fmod, const fs::path& file) {
    if (pack.fmod != fmod) {
        pack.sounds.clear();
        playing.clear();
        pack.fmod = fmod;
    }
    if (auto found = pack.sounds.find(file); found != pack.sounds.end()) return found->second;
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return nullptr;
    FMOD::Sound* sound = nullptr;
    auto path = geode::utils::string::pathToString(file);
    FMOD_MODE mode = FMOD_LOOP_OFF | FMOD_2D | FMOD_ACCURATETIME;
    if (fmod->createSound(path.c_str(), mode, nullptr, &sound) != FMOD_OK || !sound)
        return nullptr;
    pack.sounds.emplace(file, sound);
    return sound;
}

void Clickbot::unload_sounds(Clickpack& pack) {
    for (auto* channel : playing) {
        if (channel) channel->stop();
    }
    playing.clear();
    auto* engine = FMODAudioEngine::get();
    bool same_fmod = engine && engine->m_system && engine->m_system == pack.fmod;
    for (auto& [file, sound] : pack.sounds) {
        if (same_fmod && sound) sound->release();
    }
    pack.sounds.clear();
}

float Clickbot::volume_gain(float volume) {
    float gain = std::clamp(volume, 0.f, 10.f);
    if (gain <= 1.f) return gain;
    return std::pow(2.f, gain / 2.f) / std::sqrt(2.f);
}

void Clickbot::play_file(bool player1, const fs::path& file, float volume) {
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system || file.empty()) return;
    forget_finished();
    auto* sound = load_sound(pack(player1), engine->m_system, file);
    if (!sound) return;

    FMOD::ChannelGroup* group = nullptr;
    engine->m_system->getMasterChannelGroup(&group);

    FMOD::Channel* channel = nullptr;
    if (engine->m_system->playSound(sound, group, true, &channel) != FMOD_OK || !channel) return;
    channel->setPriority(CLICK_PRIORITY);
    channel->setVolume(volume_gain(volume));
    channel->setPaused(false);
    playing.push_back(channel);
}

fs::path Clickbot::find_type_folder(const fs::path& pack_folder, int type, bool down) {
    if (type == MICRO_CLICK) {
        return down
                   ? find_subfolder(pack_folder, { "micro_clicks", "microclicks", "micro clicks", "micro" })
                   : find_subfolder(pack_folder, { "micro_releases", "microreleases", "micro releases" });
    }
    if (type == SOFT_CLICK) {
        return down
                   ? find_subfolder(pack_folder, { "soft_clicks", "softclicks", "soft clicks", "soft" })
                   : find_subfolder(pack_folder, { "soft_releases", "softreleases", "soft releases" });
    }
    if (type == HARD_CLICK) {
        return down
                   ? find_subfolder(pack_folder, { "hard_clicks", "hardclicks", "hard clicks", "hard" })
                   : find_subfolder(pack_folder, { "hard_releases", "hardreleases", "hard releases" });
    }
    return down
               ? find_subfolder(pack_folder, { "clicks", "click", "normal", "normals" })
               : find_subfolder(pack_folder, { "releases", "release", "normal_releases" });
}

void Logic::play_click(PlayLayer* play, bool down, bool player1) {
    if (!clickbot || !play) return;
    const auto& setting = player1 ? clickpack : clickpack_p2;
    if (Clickbot::get().refresh_pack(player1, setting).empty()) return;
    double now = play->m_gameState.m_levelTime;
    double& last = player1 ? last_click_p1 : last_click_p2;
    double gap = last <= 0.0 ? 1.0 : now - last;
    last = now;

    bool micros = player1 ? p1_micros : p2_micros;
    bool softs = player1 ? p1_softs : p2_softs, hards = player1 ? p1_hards : p2_hards;
    float micro_sec = (player1 ? p1_micro_ms : p2_micro_ms) / 1000.f;
    float soft_sec = (player1 ? p1_soft_ms : p2_soft_ms) / 1000.f;
    float hard_sec = player1 ? p1_hard_seconds : p2_hard_seconds;

    auto has_folder = [&](int type) {
        return !Clickbot::get().sound_folder(player1, folder_index(type, down)).path.empty();
    };
    int type = NORMAL_CLICK;
    if (micros && gap <= micro_sec && has_folder(MICRO_CLICK)) type = MICRO_CLICK;
    else if (softs && gap < soft_sec && has_folder(SOFT_CLICK)) type = SOFT_CLICK;
    else if (hards && gap > hard_sec && has_folder(HARD_CLICK)) type = HARD_CLICK;

    auto has_files = [&](int type) {
        return !Clickbot::get().sound_folder(player1, folder_index(type, down)).files.empty();
    };
    if (!has_files(type)) type = NORMAL_CLICK;
    if (!has_files(type)) return;
    float volume = player1 ? clickbot_volume : clickbot_volume_p2;
    if (render_capturing()) volume *= std::max(render_sfx, 0.f);
    Clickbot::get().queue.push_back({ folder_index(type, down), volume, player1 });
}

void Logic::flush_clicks() {
    auto& queue = Clickbot::get().queue;
    if (queue.empty()) return;
    static std::mt19937 rng{ std::random_device{}() };
    for (const auto& click : queue) {
        const auto& files = Clickbot::get().sound_folder(click.player1, click.folder).files;
        if (files.empty()) continue;
        std::uniform_int_distribution<size_t> pick(0, files.size() - 1);
        Clickbot::get().play_file(click.player1, files[pick(rng)], click.volume);
    }
    queue.clear();
}
