// click sounds
// a clickpack folder has sets for hard, normal, soft & micro clicks, picked by how long its been since the last click
// decoded sounds are cached, only LIVE_CLICKS play at once & all the clicks of 1 frame are started together
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

struct LiveClick {
    FMOD::Channel* channel = nullptr;
};

enum ClickKind {
    MICRO_CLICK,
    SOFT_CLICK,
    HARD_CLICK,
    NORMAL_CLICK,
    KINDS
};

struct KindFolder {
    bool known = false;
    fs::path folder;
    std::vector<fs::path> files;
};

// W optimization?
struct PackCache {
    std::string stored;
    fs::path root;
    chrono::steady_clock::time_point checked;
    std::array<KindFolder, KINDS * 2> kinds;
    std::map<fs::path, FMOD::Sound*> sounds;
    FMOD::System* system = nullptr;
};

struct QueuedClick {
    int slot = 0;
    float volume = 1.f;
    bool player1 = true;
};

// TODO ehh maybe increase
constexpr size_t LIVE_CLICKS = 16;

class Clickbot {
public:
    static Clickbot& get() {
        static Clickbot value;
        return value;
    }

    std::string lower_name(std::string text);
    fs::path pack_root(std::string const& stored);
    PackCache& cache_for(bool player1);
    fs::path const& live_pack(bool player1, std::string const& stored);
    fs::path find_folder(fs::path const& pack, std::initializer_list<char const*> names);
    bool audio_file(fs::path const& path);
    std::vector<fs::path> audio_files(fs::path const& directory);
    std::vector<LiveClick>& live_clicks();
    void reap_clicks();
    float click_gain(float volume);
    FMOD::Sound* sound_for(PackCache& cache, FMOD::System* system, fs::path const& file);
    void drop_sounds(PackCache& cache);
    void play_file(bool player1, fs::path const& file, float volume);
    fs::path find_kind(fs::path const& pack, int kind, bool down);
    KindFolder const& kind_folder(bool player1, int slot);
    std::array<PackCache, 2> caches{};
    std::vector<QueuedClick> queued;
};

static int kind_slot(int kind, bool down) {
    return kind * 2 + (down ? 1 : 0);
}

std::string Clickbot::lower_name(std::string text) {
    log::debug("lower name");
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

fs::path Clickbot::pack_root(std::string const& stored) {
    fs::path path(stored);
    std::error_code error;
    if (!stored.empty() && fs::is_directory(path, error)) {
        return path;
    }
    if (stored.empty()) {
        return {};
    }
    auto nested = Logic::get().save_root() / "clickpacks" / stored;
    if (fs::is_directory(nested, error)) {
        return nested;
    }
    auto by_name = Logic::get().save_root() / "clickpacks" / fs::path(stored).filename();
    if (fs::is_directory(by_name, error)) {
        return by_name;
    }
    return path;
}

PackCache& Clickbot::cache_for(bool player1) {
    return Clickbot::get().caches[player1 ? 0 : 1];
}

fs::path const& Clickbot::live_pack(bool player1, std::string const& stored) {
    auto& cache = Clickbot::get().cache_for(player1);
    auto now = chrono::steady_clock::now();
    bool stale = cache.stored != stored || now - cache.checked > chrono::seconds(2);
    if (!stale)
        return cache.root;
    auto root = Clickbot::get().pack_root(stored);
    if (root != cache.root)
        Clickbot::get().drop_sounds(cache);
    cache.stored = stored;
    cache.root = std::move(root);
    cache.checked = now;
    cache.kinds = {};
    return cache.root;
}

KindFolder const& Clickbot::kind_folder(bool player1, int slot) {
    auto& cache = Clickbot::get().cache_for(player1);
    auto& entry = cache.kinds[static_cast<size_t>(slot)];
    if (entry.known)
        return entry;
    entry.folder = Clickbot::get().find_kind(cache.root, slot / 2, slot % 2 == 1);
    entry.files = Clickbot::get().audio_files(entry.folder);
    entry.known = true;
    return entry;
}

fs::path Clickbot::find_folder(fs::path const& pack, std::initializer_list<char const*> names) {
    std::error_code error;
    if (pack.empty() || !fs::is_directory(pack, error)) {
        return {};
    }
    std::vector<std::string> want;
    for (char const* name : names)
        want.push_back(Clickbot::get().lower_name(name));
    for (auto const& entry : fs::directory_iterator(pack, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }
        auto it = Clickbot::get().lower_name(geode::utils::string::pathToString(entry.path().filename()));
        for (auto const& name : want) {
            if (it == name) {
                return entry.path();
            }
        }
    }
    return {};
}

bool Clickbot::audio_file(fs::path const& path) {
    auto ext = Clickbot::get().lower_name(geode::utils::string::pathToString(path.extension()));
    return ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

std::vector<fs::path> Clickbot::audio_files(fs::path const& directory) {
    std::vector<fs::path> files;
    std::error_code error;
    if (directory.empty() || !fs::is_directory(directory, error)) {
        return files;
    }
    for (auto const& entry : fs::directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (entry.is_regular_file() && Clickbot::get().audio_file(entry.path())) {
            files.push_back(entry.path());
        }
    }
    return files;
}

std::vector<LiveClick>& Clickbot::live_clicks() {
    static std::vector<LiveClick> live;
    return live;
}

void Clickbot::reap_clicks() {
    auto& live = Clickbot::get().live_clicks();
    std::erase_if(live, [](LiveClick const& item) {
        bool playing = false;
        if (item.channel) {
            item.channel->isPlaying(&playing);
        }
        return !playing;
    });
    if (live.size() > LIVE_CLICKS) {
        auto extra = live.size() - LIVE_CLICKS;
        for (size_t i = 0; i < extra; ++i) {
            if (live[i].channel) {
                live[i].channel->stop();
            }
        }
        live.erase(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(extra));
    }
}

FMOD::Sound* Clickbot::sound_for(PackCache& cache, FMOD::System* system, fs::path const& file) {
    if (cache.system != system) {
        cache.sounds.clear();
        Clickbot::get().live_clicks().clear();
        cache.system = system;
    }
    if (auto it = cache.sounds.find(file); it != cache.sounds.end()) {
        return it->second;
    }
    std::error_code error;
    if (!fs::is_regular_file(file, error)) {
        return nullptr;
    }
    FMOD::Sound* sound = nullptr;
    auto path = geode::utils::string::pathToString(file);
    FMOD_MODE mode = FMOD_LOOP_OFF | FMOD_2D | FMOD_ACCURATETIME;
    if (system->createSound(path.c_str(), mode, nullptr, &sound) != FMOD_OK || !sound) {
        return nullptr;
    }
    cache.sounds.emplace(file, sound);
    return sound;
}

void Clickbot::drop_sounds(PackCache& cache) {
    for (auto& item : Clickbot::get().live_clicks()) {
        if (item.channel) {
            item.channel->stop();
        }
    }
    Clickbot::get().live_clicks().clear();
    auto* engine = FMODAudioEngine::get();
    bool alive = engine && engine->m_system && engine->m_system == cache.system;
    for (auto& [file, sound] : cache.sounds) {
        if (alive && sound) {
            sound->release();
        }
    }
    cache.sounds.clear();
}

float Clickbot::click_gain(float volume) {
    float gain = std::clamp(volume, 0.f, 10.f);
    if (gain <= 1.f) {
        return gain;
    }
    return std::pow(2.f, gain / 2.f) / std::sqrt(2.f);
}

void Clickbot::play_file(bool player1, fs::path const& file, float volume) {
    auto* engine = FMODAudioEngine::get();
    if (!engine || !engine->m_system || file.empty())
        return;
    Clickbot::get().reap_clicks();
    auto& cache = Clickbot::get().cache_for(player1);
    auto* sound = Clickbot::get().sound_for(cache, engine->m_system, file);
    if (!sound)
        return;

    FMOD::ChannelGroup* group = nullptr;
    engine->m_system->getMasterChannelGroup(&group);

    FMOD::Channel* channel = nullptr;
    if (engine->m_system->playSound(sound, group, true, &channel) != FMOD_OK || !channel)
        return;
    channel->setVolume(Clickbot::get().click_gain(volume));
    channel->setPaused(false);
    Clickbot::get().live_clicks().push_back({ channel });
}

fs::path Clickbot::find_kind(fs::path const& pack, int kind, bool down) {
    if (kind == MICRO_CLICK) {
        return down
                   ? Clickbot::get().find_folder(pack, { "micro_clicks", "microclicks", "micro clicks", "micro" })
                   : Clickbot::get().find_folder(pack, { "micro_releases", "microreleases", "micro releases" });
    }
    if (kind == SOFT_CLICK) {
        return down
                   ? Clickbot::get().find_folder(pack, { "soft_clicks", "softclicks", "soft clicks", "soft" })
                   : Clickbot::get().find_folder(pack, { "soft_releases", "softreleases", "soft releases" });
    }
    if (kind == HARD_CLICK) {
        return down
                   ? Clickbot::get().find_folder(pack, { "hard_clicks", "hardclicks", "hard clicks", "hard" })
                   : Clickbot::get().find_folder(pack, { "hard_releases", "hardreleases", "hard releases" });
    }
    return down
               ? Clickbot::get().find_folder(pack, { "clicks", "click", "normal", "normals" })
               : Clickbot::get().find_folder(pack, { "releases", "release", "normal_releases" });
}

void Logic::play_click(PlayLayer* play, bool down, bool player1) {
    log::debug("play click");
    auto& logic = Logic::get();
    if (!logic.clickbot || !play)
        return;
    auto const& pack = player1 ? logic.clickpack : logic.clickpack_p2;
    if (Clickbot::get().live_pack(player1, pack).empty())
        return;
    double now = play->m_gameState.m_levelTime;
    double& last = player1 ? logic.last_click_p1 : logic.last_click_p2;
    double gap = last <= 0.0 ? 1.0 : now - last;
    last = now;

    bool micros = player1 ? logic.p1_micros : logic.p2_micros;
    bool softs = player1 ? logic.p1_softs : logic.p2_softs, hards = player1 ? logic.p1_hards : logic.p2_hards;
    float micro_sec = (player1 ? logic.p1_micro_ms : logic.p2_micro_ms) / 1000.f;
    float soft_sec = (player1 ? logic.p1_soft_ms : logic.p2_soft_ms) / 1000.f;
    float hard_sec = player1 ? logic.p1_hard_seconds : logic.p2_hard_seconds;

    auto present = [&](int kind) {
        return !Clickbot::get().kind_folder(player1, kind_slot(kind, down)).folder.empty();
    };
    int kind = NORMAL_CLICK;
    if (micros && gap <= micro_sec && present(MICRO_CLICK))
        kind = MICRO_CLICK;
    else if (softs && gap < soft_sec && present(SOFT_CLICK))
        kind = SOFT_CLICK;
    else if (hards && gap > hard_sec && present(HARD_CLICK))
        kind = HARD_CLICK;

    auto sounds = [&](int kind) {
        return !Clickbot::get().kind_folder(player1, kind_slot(kind, down)).files.empty();
    };
    if (!sounds(kind)) {
        kind = NORMAL_CLICK;
    }
    if (!sounds(kind))
        return;
    float volume = player1 ? logic.clickbot_volume : logic.clickbot_volume_p2;
    if (Logic::get().render_capturing()) {
        volume *= std::max(logic.render_sfx, 0.f);
    }
    Clickbot::get().queued.push_back({ kind_slot(kind, down), volume, player1 });
}

void Logic::flush_clicks() {
    auto& queued = Clickbot::get().queued;
    if (queued.empty())
        return;
    static std::mt19937 rng{ std::random_device{}() };
    size_t from = queued.size() > LIVE_CLICKS ? queued.size() - LIVE_CLICKS : 0;
    for (size_t i = from; i < queued.size(); ++i) {
        auto const& click = queued[i];
        auto const& files = Clickbot::get().kind_folder(click.player1, click.slot).files;
        if (files.empty()) {
            continue;
        }
        std::uniform_int_distribution<size_t> dist(0, files.size() - 1);
        Clickbot::get().play_file(click.player1, files[dist(rng)], click.volume);
    }
    queued.clear();
}
