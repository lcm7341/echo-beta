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




using namespace geode::prelude;


struct LiveClick {
	FMOD::Channel* channel = nullptr;
};
enum ClickKind { kMicro, kSoft, kHard, kNormal, kKinds };
struct KindFolder {
	bool known = false;
	std::filesystem::path folder;
	std::vector<std::filesystem::path> files;
};
// A macro can click thousands of times a second, so the pack's folders, file lists, and decoded
// sounds are kept. The folders are looked at again every couple of seconds so edits to the pack
// still show up.
struct PackCache {
	std::string stored;
	std::filesystem::path root;
	std::chrono::steady_clock::time_point checked;
	std::array<KindFolder, kKinds * 2> kinds;
	std::map<std::filesystem::path, FMOD::Sound*> sounds;
	FMOD::System* system = nullptr;
};
struct QueuedClick {
	int slot = 0;
	float volume = 1.f;
	bool player1 = true;
};
// Only this many clicks play at once. A newer one stops the oldest.
constexpr size_t kLiveClicks = 24;
class Clickbot {
public:
	static Clickbot& get() {
		static Clickbot value;
		return value;
	}
	std::string lowerName(std::string text);
	std::filesystem::path packRoot(std::string const& stored);
	PackCache& cacheFor(bool player1);
	std::filesystem::path const& livePack(bool player1, std::string const& stored);
	std::filesystem::path findFolder(std::filesystem::path const& pack, std::initializer_list<char const*> names);
	bool audioFile(std::filesystem::path const& path);
	std::vector<std::filesystem::path> audioFiles(std::filesystem::path const& directory);
	std::vector<LiveClick>& liveClicks();
	void reapClicks();
	float clickGain(float volume);
	FMOD::Sound* soundFor(PackCache& cache, FMOD::System* system, std::filesystem::path const& file);
	void dropSounds(PackCache& cache);
	void playFile(bool player1, std::filesystem::path const& file, float volume);
	std::filesystem::path findKind(std::filesystem::path const& pack, int kind, bool down);
	KindFolder const& kindFolder(bool player1, int slot);
	std::array<PackCache, 2> caches{};
	std::vector<QueuedClick> queued;
};

static int kindSlot(int kind, bool down) {
	return kind * 2 + (down ? 1 : 0);
}

std::string Clickbot::lowerName(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return text;
}

std::filesystem::path Clickbot::packRoot(std::string const& stored) {
	std::filesystem::path path(stored);
	std::error_code error;
	if (!stored.empty() && std::filesystem::is_directory(path, error)) return path;
	if (stored.empty()) return {};
	auto nested = Logic::get().saveRoot() / "clickpacks" / stored;
	if (std::filesystem::is_directory(nested, error)) return nested;
	auto byName = Logic::get().saveRoot() / "clickpacks" / std::filesystem::path(stored).filename();
	if (std::filesystem::is_directory(byName, error)) return byName;
	return path;
}

PackCache& Clickbot::cacheFor(bool player1) {
	return Clickbot::get().caches[player1 ? 0 : 1];
}

std::filesystem::path const& Clickbot::livePack(bool player1, std::string const& stored) {
	auto& cache = Clickbot::get().cacheFor(player1);
	auto now = std::chrono::steady_clock::now();
	bool stale = cache.stored != stored || now - cache.checked > std::chrono::seconds(2);
	if (!stale) return cache.root;
	auto root = Clickbot::get().packRoot(stored);
	if (root != cache.root) Clickbot::get().dropSounds(cache);
	cache.stored = stored;
	cache.root = std::move(root);
	cache.checked = now;
	cache.kinds = {};
	return cache.root;
}

KindFolder const& Clickbot::kindFolder(bool player1, int slot) {
	auto& cache = Clickbot::get().cacheFor(player1);
	auto& entry = cache.kinds[static_cast<size_t>(slot)];
	if (entry.known) return entry;
	entry.folder = Clickbot::get().findKind(cache.root, slot / 2, slot % 2 == 1);
	entry.files = Clickbot::get().audioFiles(entry.folder);
	entry.known = true;
	return entry;
}

std::filesystem::path Clickbot::findFolder(std::filesystem::path const& pack, std::initializer_list<char const*> names) {
	std::error_code error;
	if (pack.empty() || !std::filesystem::is_directory(pack, error)) return {};
	std::vector<std::string> want;
	for (char const* name : names) want.push_back(Clickbot::get().lowerName(name));
	for (auto const& entry : std::filesystem::directory_iterator(pack, error)) {
		if (error || !entry.is_directory()) continue;
		auto found = Clickbot::get().lowerName(entry.path().filename().string());
		for (auto const& name : want) {
			if (found == name) return entry.path();
		}
	}
	return {};
}

bool Clickbot::audioFile(std::filesystem::path const& path) {
	auto ext = Clickbot::get().lowerName(path.extension().string());
	return ext == ".wav" || ext == ".ogg" || ext == ".mp3" || ext == ".flac";
}

std::vector<std::filesystem::path> Clickbot::audioFiles(std::filesystem::path const& directory) {
	std::vector<std::filesystem::path> files;
	std::error_code error;
	if (directory.empty() || !std::filesystem::is_directory(directory, error)) return files;
	for (auto const& entry : std::filesystem::directory_iterator(directory, error)) {
		if (error) break;
		if (entry.is_regular_file() && Clickbot::get().audioFile(entry.path())) files.push_back(entry.path());
	}
	return files;
}

std::vector<LiveClick>& Clickbot::liveClicks() {
	static std::vector<LiveClick> live;
	return live;
}

void Clickbot::reapClicks() {
	auto& live = Clickbot::get().liveClicks();
	std::erase_if(live, [](LiveClick const& item) {
		bool playing = false;
		if (item.channel) item.channel->isPlaying(&playing);
		return !playing;
	});
	if (live.size() > kLiveClicks) {
		auto extra = live.size() - kLiveClicks;
		for (size_t i = 0; i < extra; ++i) {
			if (live[i].channel) live[i].channel->stop();
		}
		live.erase(live.begin(), live.begin() + static_cast<std::ptrdiff_t>(extra));
	}
}

FMOD::Sound* Clickbot::soundFor(PackCache& cache, FMOD::System* system, std::filesystem::path const& file) {
	if (cache.system != system) {
		cache.sounds.clear();
		Clickbot::get().liveClicks().clear();
		cache.system = system;
	}
	if (auto found = cache.sounds.find(file); found != cache.sounds.end()) return found->second;
	std::error_code error;
	if (!std::filesystem::is_regular_file(file, error)) return nullptr;
	FMOD::Sound* sound = nullptr;
	auto path = file.string();
	FMOD_MODE mode = FMOD_LOOP_OFF | FMOD_2D | FMOD_ACCURATETIME;
	if (system->createSound(path.c_str(), mode, nullptr, &sound) != FMOD_OK || !sound) return nullptr;
	cache.sounds.emplace(file, sound);
	return sound;
}

void Clickbot::dropSounds(PackCache& cache) {
	for (auto& item : Clickbot::get().liveClicks()) {
		if (item.channel) item.channel->stop();
	}
	Clickbot::get().liveClicks().clear();
	auto* engine = FMODAudioEngine::get();
	bool alive = engine && engine->m_system && engine->m_system == cache.system;
	for (auto& [file, sound] : cache.sounds) {
		if (alive && sound) sound->release();
	}
	cache.sounds.clear();
}

float Clickbot::clickGain(float volume) {
	float gain = std::clamp(volume, 0.f, 10.f);
	if (gain <= 1.f) return gain;
	return std::pow(2.f, gain / 2.f) / std::sqrt(2.f);
}

void Clickbot::playFile(bool player1, std::filesystem::path const& file, float volume) {
	auto* engine = FMODAudioEngine::get();
	if (!engine || !engine->m_system || file.empty()) return;
	Clickbot::get().reapClicks();
	auto& cache = Clickbot::get().cacheFor(player1);
	auto* sound = Clickbot::get().soundFor(cache, engine->m_system, file);
	if (!sound) return;

	// Master bus — independent of GD's SFX volume (m_globalChannel).
	FMOD::ChannelGroup* group = nullptr;
	engine->m_system->getMasterChannelGroup(&group);

	FMOD::Channel* channel = nullptr;
	if (engine->m_system->playSound(sound, group, true, &channel) != FMOD_OK || !channel) return;
	channel->setVolume(Clickbot::get().clickGain(volume));
	channel->setPaused(false);
	Clickbot::get().liveClicks().push_back({channel});
}

std::filesystem::path Clickbot::findKind(std::filesystem::path const& pack, int kind, bool down) {
	if (kind == kMicro) {
		return down
			? Clickbot::get().findFolder(pack, {"micro_clicks", "microclicks", "micro clicks", "micro"})
			: Clickbot::get().findFolder(pack, {"micro_releases", "microreleases", "micro releases"});
	}
	if (kind == kSoft) {
		return down
			? Clickbot::get().findFolder(pack, {"soft_clicks", "softclicks", "soft clicks", "soft"})
			: Clickbot::get().findFolder(pack, {"soft_releases", "softreleases", "soft releases"});
	}
	if (kind == kHard) {
		return down
			? Clickbot::get().findFolder(pack, {"hard_clicks", "hardclicks", "hard clicks", "hard"})
			: Clickbot::get().findFolder(pack, {"hard_releases", "hardreleases", "hard releases"});
	}
	return down
		? Clickbot::get().findFolder(pack, {"clicks", "click", "normal", "normals"})
		: Clickbot::get().findFolder(pack, {"releases", "release", "normal_releases"});
}

void Logic::playClick(PlayLayer* play, bool down, bool player1) {
	auto& value = Logic::get();
	if (!value.clickbot || !play) return;
	auto const& pack = player1 ? value.clickpack : value.clickpackP2;
	if (Clickbot::get().livePack(player1, pack).empty()) return;
	double now = play->m_gameState.m_levelTime;
	double& last = player1 ? value.lastClickP1 : value.lastClickP2;
	double gap = last <= 0.0 ? 1.0 : now - last;
	last = now;

	bool micros = player1 ? value.p1Micros : value.p2Micros;
	bool softs = player1 ? value.p1Softs : value.p2Softs;
	bool hards = player1 ? value.p1Hards : value.p2Hards;
	float microSec = (player1 ? value.p1MicroMs : value.p2MicroMs) / 1000.f;
	float softSec = (player1 ? value.p1SoftMs : value.p2SoftMs) / 1000.f;
	float hardSec = player1 ? value.p1HardSeconds : value.p2HardSeconds;

	auto present = [&](int kind) { return !Clickbot::get().kindFolder(player1, kindSlot(kind, down)).folder.empty(); };
	int kind = kNormal;
	if (micros && gap <= microSec && present(kMicro)) kind = kMicro;
	else if (softs && gap < softSec && present(kSoft)) kind = kSoft;
	else if (hards && gap > hardSec && present(kHard)) kind = kHard;

	auto sounds = [&](int kind) { return !Clickbot::get().kindFolder(player1, kindSlot(kind, down)).files.empty(); };
	if (!sounds(kind)) kind = kNormal;
	if (!sounds(kind)) return;
	float volume = player1 ? value.clickbotVolume : value.clickbotVolumeP2;
	if (Logic::get().renderCapturing()) volume *= std::max(value.renderSfx, 0.f);
	Clickbot::get().queued.push_back({kindSlot(kind, down), volume, player1});
}

// Every click of a frame starts at the same moment, and only the newest few can play at once, so
// the frame's clicks are played together and the ones that would be cut off at once are skipped.
void Logic::flushClicks() {
	auto& queued = Clickbot::get().queued;
	if (queued.empty()) return;
	static std::mt19937 rng{std::random_device{}()};
	size_t from = queued.size() > kLiveClicks ? queued.size() - kLiveClicks : 0;
	for (size_t i = from; i < queued.size(); ++i) {
		auto const& click = queued[i];
		auto const& files = Clickbot::get().kindFolder(click.player1, click.slot).files;
		if (files.empty()) continue;
		std::uniform_int_distribution<size_t> dist(0, files.size() - 1);
		Clickbot::get().playFile(click.player1, files[dist(rng)], click.volume);
	}
	queued.clear();
}
