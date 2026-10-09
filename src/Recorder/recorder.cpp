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
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif
#ifdef GEODE_IS_ANDROID
#include <dlfcn.h>
#endif

#if defined(GEODE_IS_ANDROID) && defined(ECHO_FFMPEG_API)
namespace {
// The API does not expose encoder options. Set the preset only while Echo
// initializes its own x264 encoder; other mods and audio muxing keep their options.
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

int echoOpenCodec(AVCodecContext* context, AVCodec const* codec, AVDictionary** options) {
	AVDictionary* local = nullptr;
	AVDictionary** chosen = options ? options : &local;
	if (s_echoPreset) s_setDictionary(chosen, "preset", s_echoPreset, 0);
	int result = s_openCodec(context, codec, chosen);
	if (local) s_freeDictionary(&local);
	return result;
}

void installEncoderOptions() {
	static bool installed = false;
	if (installed) return;
	auto* dependency = Loader::get()->getLoadedMod("eclipse.ffmpeg-api");
	if (!dependency) return;
	void* binary = dlopen(dependency->getBinaryPath().c_str(), RTLD_NOW | RTLD_NOLOAD);
	if (!binary) return;
	s_openCodec = reinterpret_cast<OpenCodec>(dlsym(binary, "avcodec_open2"));
	s_setDictionary = reinterpret_cast<SetDictionary>(dlsym(binary, "av_dict_set"));
	s_freeDictionary = reinterpret_cast<FreeDictionary>(dlsym(binary, "av_dict_free"));
	if (s_openCodec && s_setDictionary && s_freeDictionary) {
		auto hook = Mod::get()->hook(reinterpret_cast<void*>(s_openCodec), &echoOpenCodec, "Echo encoder preset");
		installed = hook.isOk();
		if (!installed) log::warn("Could not configure Echo's encoder preset: {}", hook.unwrapErr());
	}
	dlclose(binary);
}
}
#endif

class AudioTap {
public:
	static AudioTap& get() {
		static AudioTap value;
		return value;
	}
	bool attach(float music, float sfx, std::filesystem::path const& file);
	void detach();
	void advance(float dt, float fade);
	void holdEffects();
	static FMOD_RESULT F_CALLBACK read(FMOD_DSP_STATE* state, float* in, float* out, unsigned int length, int inChannels, int* outChannels);

	bool attached = false;
	bool engineOpen = false;
	bool mixing = false;
	int rate = 0;
	int channels = 0;
	uint64_t written = 0;

private:
	FMOD::DSP* dsp = nullptr;
	FMOD::ChannelGroup* master = nullptr;
	std::mutex lock;
	std::vector<float> pending;
	std::ofstream out;
	double clock = 0.0;
	float oldMusic = 1.f;
	float oldSfx = 1.f;
	float effects = 1.f;
	void bindBus();
};

class Recorder {
public:
	static Recorder& get() {
		static Recorder value;
		return value;
	}
#ifdef GEODE_IS_WINDOWS
	void closeHandle(HANDLE& handle);
	bool connectPipe(HANDLE pipe);
	bool writeAll(HANDLE pipe, void const* data, size_t size);
	std::filesystem::path gameDirectory();
	std::string ffmpegPath();
	std::string runHidden(std::string command, std::filesystem::path const& logPath);
	bool harvest(bool wait);
	bool setupNv12();
	void closeNv12();
	void convertNv12();
	void drawFullQuad();
	bool muxAudio();
#endif
#ifdef ECHO_FFMPEG_API
	std::string openApi(int fps);
#endif
	static bool apiAvailable();
	static bool preferApi();
	bool idle() const;
	void goLive();
	bool writeOut(std::vector<uint8_t> const& frame);
	void saveApiRender();
	void silentChangeSize(CCSize size);
	void resizeShaderLayer(CCSize size);
	std::optional<CCSize> windowPixels();
	void holdView();
	void releaseView();
	bool openTarget();
	void closeTarget();
	void drawFrame();
	void showOverlay();
	void drawProgress();
	void queueFrame(std::vector<uint8_t> frame);
	std::vector<uint8_t> acquireFrame();
	void encodeLoop();
	void finishVideo();
	size_t frameBytes() const;
	void uncapPace();
	void restorePace();

	static constexpr int kRing = 6;
#ifdef GEODE_IS_WINDOWS
	static constexpr size_t kQueue = 12;
#else
	static constexpr size_t kQueue = 4;
#endif

	bool active = false;
	bool paused = false;
	bool starting = false;
	bool cancel = false;
	bool holding = false;
	bool useApi = false;
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
	GLint screenFbo = 0;
	std::filesystem::path output;
	std::filesystem::path audio;
	int width = 0;
	int height = 0;
	int frames = 0;
	int seenFrames = 0;
	float endTime = 0.f;
	float fade = 1.f;
	CCSize savedFrame{};
	CCSize savedPixels{};

	GLuint fbo = 0;
	GLuint color = 0;
	GLuint yFbo = 0;
	GLuint uvFbo = 0;
	GLuint yColor = 0;
	GLuint uvColor = 0;
	GLuint yProg = 0;
	GLuint uvProg = 0;
	bool nv12 = false;
	int savedSwap = 1;
	float savedInterval = 1.f / 60.f;
	bool paceHeld = false;
#ifdef GEODE_IS_WINDOWS
	std::array<GLuint, kRing> pbo{};
	std::array<GLsync, kRing> fences{};
	std::deque<int> inFlight;
	int nextSlot = 0;
#endif
	CCLayerColor* shade = nullptr;
	CCLayerColor* barBack = nullptr;
	CCLayerColor* barFill = nullptr;
	CCLabelBMFont* barText = nullptr;
	std::string barShown;
	double textClock = 0.0;

	std::chrono::steady_clock::time_point lastTick{};
	bool ticked = false;
	float wallDelta = 0.f;
	double busySeconds = 0.0;
	float startFraction = -1.f;
	int lastInputFrame = 0;

	std::thread worker;
	std::mutex lock;
	std::condition_variable wake;
	std::deque<std::vector<uint8_t>> queue;
	std::vector<std::vector<uint8_t>> spareFrames;
	bool closing = false;
};


FMOD_RESULT F_CALLBACK AudioTap::read(FMOD_DSP_STATE*, float* in, float* out, unsigned int length, int inChannels, int* outChannels) {
	auto& tap = AudioTap::get();
	size_t count = static_cast<size_t>(length) * static_cast<size_t>(inChannels);
	std::memcpy(out, in, count * sizeof(float));
	if (outChannels) *outChannels = inChannels;
	std::lock_guard guard(tap.lock);
	if (!tap.mixing) return FMOD_OK;
	tap.channels = inChannels;
	tap.pending.insert(tap.pending.end(), in, in + count);
	return FMOD_OK;
}

void AudioTap::bindBus() {
	auto* engine = FMODAudioEngine::get();
	if (!engine || !engine->m_system || !dsp) return;
	FMOD::ChannelGroup* live = nullptr;
	if (engine->m_system->getMasterChannelGroup(&live) != FMOD_OK || !live) return;
	if (live != master) {
		if (master) master->removeDSP(dsp);
		if (live->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, dsp) == FMOD_OK) master = live;
	}
	if (!master) return;
	auto adopt = [&](FMOD::ChannelGroup* group) {
		if (!group || group == master) return;
		group->setPaused(false);
		master->addGroup(group, true, nullptr);
	};
	adopt(engine->m_backgroundMusicChannel);
	adopt(engine->m_globalChannel);
	if (engine->m_globalChannel && engine->m_reverbChannel) engine->m_globalChannel->addGroup(engine->m_reverbChannel, true, nullptr);
}

void AudioTap::holdEffects() {
	if (!attached) return;
	auto* engine = FMODAudioEngine::get();
	if (!engine) return;
	float volume = std::clamp(effects, 0.f, 1.f);
	engine->setEffectsVolume(volume);
	if (engine->m_globalChannel) {
		engine->m_globalChannel->setVolume(volume);
		engine->m_globalChannel->setPaused(false);
	}
	if (engine->m_reverbChannel) engine->m_reverbChannel->setPaused(false);
}

bool AudioTap::attach(float music, float sfx, std::filesystem::path const& file) {
	auto* engine = FMODAudioEngine::get();
	if (attached || !engine || !engine->m_system) return false;
	auto* system = engine->m_system;
	FMOD_DSP_DESCRIPTION description{};
	description.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
	std::strncpy(description.name, "Echo Render", sizeof(description.name) - 1);
	description.numinputbuffers = 1;
	description.numoutputbuffers = 1;
	description.read = &AudioTap::read;
	if (system->createDSP(&description, &dsp) != FMOD_OK || !dsp) return false;
	if (system->getMasterChannelGroup(&master) != FMOD_OK || !master || master->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, dsp) != FMOD_OK) {
		dsp->release();
		dsp = nullptr;
		return false;
	}
	gd::string song;
	unsigned songMs = 0;
	if (auto found = engine->m_channelIDToSoundPath.find(0); found != engine->m_channelIDToSoundPath.end()) {
		song = found->second;
		songMs = engine->getMusicTimeMS(0);
	}
	// The channel table is not always filled; the loaded song is still in m_fmodMusic.
	if (auto found = engine->m_fmodMusic.find(0); song.empty() && found != engine->m_fmodMusic.end() && !found->second.m_filePath.empty()) {
		song = found->second.m_filePath;
		songMs = engine->getMusicTimeMS(0);
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
		// Recreate the song on the offline mixer. Jukebox swaps this path for the custom track.
		engine->stopAndRemoveMusic(0);
		// GD opens songs as streams, decoded a chunk ahead at real-time pace on FMOD's stream
		// thread. The render mixes faster than that, so the stream ran dry and replayed its last
		// 0.68 s until the thread caught up. GD keeps the open stream for this song, so swap in a
		// copy decoded whole into memory (render_song.cpp does the same for songs opened later).
		if (auto found = engine->m_fmodMusic.find(0); found != engine->m_fmodMusic.end() && found->second.m_sound) {
			FMOD_MODE mode = 0;
			found->second.m_sound->getMode(&mode);
			if (mode & FMOD_CREATESTREAM) {
				FMOD::Sound* sample = nullptr;
				FMOD_MODE wanted = (mode & ~static_cast<FMOD_MODE>(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
				auto result = system->createSound(found->second.m_filePath.c_str(), wanted, nullptr, &sample);
				if (result != FMOD_OK) log::warn("Echo render song: {} could not be decoded into memory ({})", std::string(found->second.m_filePath), static_cast<int>(result));
				// The stream stays allocated: GD may still hold it elsewhere.
				if (result == FMOD_OK && sample) found->second.m_sound = sample;
			}
		}
		// A level plays its song once; it does not start over if the level runs longer.
		engine->queueStartMusic(song, 1.f, 0.f, std::clamp(music, 0.f, 1.f), false, static_cast<int>(songMs), 0, 0, 0, 0, false, 0, false, false);
		engine->updateQueuedMusic();
		// The in-memory copy keeps the stream's loop flag; play it through once.
		if (auto* channel = engine->getActiveMusicChannel(0)) channel->setLoopCount(0);
	}
	if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPaused(false);
	oldMusic = engine->getBackgroundMusicVolume();
	oldSfx = engine->getEffectsVolume();
	effects = std::clamp(sfx, 0.f, 1.f);
	engine->setBackgroundMusicVolume(std::clamp(music, 0.f, 1.f));
	bindBus();
	holdEffects();
	pending.clear();
	channels = 0;
	written = 0;
	clock = 0.0;
	attached = true;
	return true;
}

void AudioTap::detach() {
	if (!attached) return;
	attached = false;
	engineOpen = false;
	mixing = false;
	out.close();
	pending.clear();
	auto* engine = FMODAudioEngine::get();
	if (!engine || !engine->m_system) return;
	if (master && dsp) master->removeDSP(dsp);
	if (dsp) dsp->release();
	dsp = nullptr;
	master = nullptr;
	engine->setBackgroundMusicVolume(oldMusic);
	engine->setEffectsVolume(oldSfx);
	engine->m_system->setOutput(FMOD_OUTPUTTYPE_AUTODETECT);
}

void AudioTap::advance(float dt, float fade) {
	auto* engine = FMODAudioEngine::get();
	if (!attached || !engine || !engine->m_system || rate <= 0) return;
	auto* play = PlayLayer::get();
	bool opening = Logic::get().renderRecordPause && play && !play->m_started;
	if (opening) {
		if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPaused(true);
		clock += static_cast<double>(dt);
		uint64_t target = static_cast<uint64_t>(std::llround(clock * static_cast<double>(rate)));
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
	engineOpen = true;
	engine->update(dt);
	engineOpen = false;
	holdEffects();
	uint64_t target = static_cast<uint64_t>(std::llround(clock * static_cast<double>(rate)));
	auto buffered = [this] {
		std::lock_guard guard(lock);
		return channels > 0 ? pending.size() / static_cast<size_t>(channels) : 0;
	};
	mixing = true;
	int stalls = 0;
	while (written + buffered() < target && stalls < 64) {
		size_t before = buffered();
		engine->m_system->update();
		stalls = buffered() == before ? stalls + 1 : 0;
	}
	mixing = false;
	std::lock_guard guard(lock);
	if (channels <= 0) return;
	size_t take = static_cast<size_t>(std::min<uint64_t>(target - std::min(target, written), pending.size() / static_cast<size_t>(channels)));
	size_t count = take * static_cast<size_t>(channels);
	float gain = std::clamp(fade, 0.f, 1.f);
	for (size_t i = 0; i < count; ++i) pending[i] = std::clamp(pending[i] * gain, -1.f, 1.f);
	out.write(reinterpret_cast<char const*>(pending.data()), static_cast<std::streamsize>(count * sizeof(float)));
	pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(count));
	written += take;
}

#ifdef GEODE_IS_WINDOWS
void Recorder::closeHandle(HANDLE& handle) {
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
		handle = INVALID_HANDLE_VALUE;
		return;
	}
	CloseHandle(handle);
	handle = INVALID_HANDLE_VALUE;
}

bool Recorder::connectPipe(HANDLE pipe) {
	OVERLAPPED wait{};
	wait.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!wait.hEvent) return false;
	BOOL ok = ConnectNamedPipe(pipe, &wait);
	if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitForSingleObject(wait.hEvent, 4000) == WAIT_OBJECT_0;
	if (!ok && GetLastError() == ERROR_PIPE_CONNECTED) ok = TRUE;
	CloseHandle(wait.hEvent);
	return ok == TRUE;
}

bool Recorder::writeAll(HANDLE pipe, void const* data, size_t size) {
	if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return false;
	auto const* bytes = static_cast<uint8_t const*>(data);
	HANDLE event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!event) return false;
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
		} else if (!ok || wrote == 0) {
			CloseHandle(event);
			return false;
		}
		sent += wrote;
	}
	CloseHandle(event);
	return true;
}

std::filesystem::path Recorder::gameDirectory() {
	char module[MAX_PATH] = {};
	if (!GetModuleFileNameA(nullptr, module, MAX_PATH)) return {};
	return std::filesystem::path(module).parent_path();
}

std::string Recorder::ffmpegPath() {
	auto beside = Recorder::get().gameDirectory() / "ffmpeg.exe";
	if (std::filesystem::exists(beside)) return beside.string();
	return "ffmpeg.exe";
}

std::string Recorder::runHidden(std::string command, std::filesystem::path const& logPath) {
	SECURITY_ATTRIBUTES security{};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	HANDLE log = CreateFileA(logPath.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log == INVALID_HANDLE_VALUE) return "could not open the ffmpeg log";
	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	startup.wShowWindow = SW_HIDE;
	startup.hStdInput = input == INVALID_HANDLE_VALUE ? nullptr : input;
	startup.hStdOutput = log;
	startup.hStdError = log;
	PROCESS_INFORMATION process{};
	std::vector<char> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back('\0');
	if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		CloseHandle(log);
		if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
		return "could not start ffmpeg";
	}
	CloseHandle(process.hThread);
	CloseHandle(log);
	if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
	if (WaitForSingleObject(process.hProcess, 180000) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 1);
	DWORD code = 1;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	if (code == 0) return {};
	std::ifstream in(logPath);
	std::string line;
	std::string last;
	while (std::getline(in, line)) {
		if (!line.empty()) last = line;
	}
	if (last.empty()) last = "ffmpeg failed";
	if (last.size() > 160) last = last.substr(last.size() - 160);
	return last;
}

namespace {
std::string quoteArg(std::filesystem::path const& path) {
	return "\"" + path.string() + "\"";
}
std::filesystem::path probeBinary() {
	auto beside = std::filesystem::path(Recorder::get().ffmpegPath()).replace_filename("ffprobe.exe");
	if (std::filesystem::exists(beside)) return beside;
	return "ffprobe.exe";
}
std::string runLogged(std::string command, std::filesystem::path const& logPath, std::filesystem::path const& capture) {
	SECURITY_ATTRIBUTES security{};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;
	HANDLE log = CreateFileA(logPath.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	HANDLE out = capture.empty()
		? log
		: CreateFileA(capture.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) {
		if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
		if (out != INVALID_HANDLE_VALUE && out != log) CloseHandle(out);
		if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
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
	std::vector<char> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back('\0');
	if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process)) {
		CloseHandle(log);
		if (out != log) CloseHandle(out);
		if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
		return "could not start ffmpeg";
	}
	CloseHandle(process.hThread);
	CloseHandle(log);
	if (out != log) CloseHandle(out);
	if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	if (code == 0) return {};
	std::ifstream in(logPath);
	std::string line;
	std::string last;
	while (std::getline(in, line)) {
		if (!line.empty()) last = line;
	}
	if (last.empty()) last = "ffmpeg failed";
	if (last.size() > 180) last = last.substr(last.size() - 180);
	return last;
}
std::optional<double> probeSeconds(std::filesystem::path const& file, std::filesystem::path const& scratch, std::string& error) {
	auto command = quoteArg(probeBinary()) + " -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 " + quoteArg(file);
	auto capture = scratch / "echo-probe.txt";
	auto log = scratch / "echo-probe.log";
	error = runLogged(command, log, capture);
	if (!error.empty()) return std::nullopt;
	std::ifstream in(capture);
	std::string text;
	std::getline(in, text);
	try {
		size_t used = 0;
		double seconds = std::stod(text, &used);
		if (seconds > 0.0) return seconds;
	} catch (...) {}
	error = "Could not read the video length";
	return std::nullopt;
}
bool probeAudio(std::filesystem::path const& file, std::filesystem::path const& scratch) {
	auto command = quoteArg(probeBinary()) + " -v error -select_streams a:0 -show_entries stream=index -of csv=p=0 " + quoteArg(file);
	auto capture = scratch / "echo-probe-audio.txt";
	auto log = scratch / "echo-probe.log";
	if (!runLogged(command, log, capture).empty()) return false;
	std::ifstream in(capture);
	std::string text;
	std::getline(in, text);
	return !text.empty();
}
std::string joinFilms(std::filesystem::path const& first, std::filesystem::path const& second, std::filesystem::path const& dest) {
	auto scratch = dest.parent_path();
	std::string probeError;
	auto firstLength = probeSeconds(first, scratch, probeError);
	if (!firstLength) return probeError;
	auto secondLength = probeSeconds(second, scratch, probeError);
	if (!secondLength) return probeError;
	double fade = std::min(1.0, std::min(*firstLength, *secondLength) * 0.45);
	if (!(fade > 0.05)) return "The videos are too short to crossfade";
	double offset = *firstLength - fade;
	bool audio = probeAudio(first, scratch) && probeAudio(second, scratch);
	std::string filter = fmt::format(
		"[1:v][0:v]scale2ref[v1][v0];[v0][v1]xfade=transition=fadeblack:duration={:.3f}:offset={:.3f}[v]",
		fade,
		offset
	);
	std::string maps = "-map [v]";
	std::string audioCodec;
	if (audio) {
		filter += fmt::format(";[0:a][1:a]acrossfade=d={:.3f}[a]", fade);
		maps += " -map [a]";
		audioCodec = "-c:a alac";
	}
	auto command = fmt::format(
		"{} -y -nostdin -i {} -i {} -filter_complex {} {} -c:v libx264 -crf 0 -preset veryfast {} {}",
		quoteArg(Recorder::get().ffmpegPath()),
		quoteArg(first),
		quoteArg(second),
		filter,
		maps,
		audioCodec,
		quoteArg(dest)
	);
	return runLogged(command, scratch / "echo-merge.log", {});
}
}

void Logic::mergeRenders(std::filesystem::path const& firstPath, std::filesystem::path const& secondPath) {
	if (mergeBusy) return;
	auto folder = saveRoot() / "renders";
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	mergeBusy = true;
	notify("Merging in the background", true);
	std::thread([this, folder, firstPath, secondPath] {
		std::string failure;
		std::filesystem::path dest;
		try {
			dest = ReplayIO::uniqueFile(folder, ReplayIO::sanitize(firstPath.stem().string() + "_" + secondPath.stem().string()), ".mp4");
			failure = joinFilms(firstPath, secondPath, dest);
		} catch (std::exception const& ex) {
			failure = ex.what();
		}
		Loader::get()->queueInMainThread([this, failure, dest] {
			mergeBusy = false;
			if (failure.empty()) notify(fmt::format("Merged {}", dest.filename().string()), true);
			else notify(failure, false);
		});
	}).detach();
}
#else
void Logic::mergeRenders(std::filesystem::path const&, std::filesystem::path const&) {
	notify("Merging renders needs ffmpeg.exe on Windows", false);
}
#endif

// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
// Resizes cocos' idea of the screen without resizing the real window.
void Recorder::silentChangeSize(CCSize size) {
	auto director = CCDirector::sharedDirector();
	auto view = CCEGLView::sharedOpenGLView();
	view->CCEGLViewProtocol::setFrameSize(size.width, size.height);
	director->updateScreenScale(size);
	director->setViewport();
	director->setProjection(kCCDirectorProjection2D);
	glViewport(0, 0, size.width, size.height);
}

// Taken directly from Silicate (https://git.silicate.dev/silicate/silicate), GPL-3.0.
// GD's ShaderLayer caches the screen size in several fields; all of them must follow the new size.
void Recorder::resizeShaderLayer(CCSize size) {
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
		{-1.0f, -1.0f, size.width + 2.0f, size.height + 2.0f});
	sh->m_state.m_textureScaleX = size.width / winSize.width;
	sh->m_state.m_textureScaleY = size.height / winSize.height;
}


std::optional<CCSize> Recorder::windowPixels() {
#ifdef GEODE_IS_WINDOWS
	HDC context = wglGetCurrentDC();
	HWND window = context ? WindowFromDC(context) : nullptr;
	RECT client{};
	if (!window || !GetClientRect(window, &client)) return std::nullopt;
	if (client.right <= client.left || client.bottom <= client.top) return std::nullopt;
	return CCSize(static_cast<float>(client.right - client.left), static_cast<float>(client.bottom - client.top));
#else
	return std::nullopt;
#endif
}

void Recorder::holdView() {
	CCSize size(static_cast<float>(width), static_cast<float>(height));
	if (!holding) {
		savedFrame = CCEGLView::sharedOpenGLView()->getFrameSize();
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_MOBILE)
		savedPixels = windowPixels().value_or(savedFrame);
#else
		GLint viewport[4] = {};
		glGetIntegerv(GL_VIEWPORT, viewport);
		savedPixels = CCSize(static_cast<float>(viewport[0] * 2 + viewport[2]), static_cast<float>(viewport[1] * 2 + viewport[3]));
		if (savedPixels.width < 1.f || savedPixels.height < 1.f) savedPixels = savedFrame;
#endif
		holding = true;
	}
	if (!CCEGLView::sharedOpenGLView()->getFrameSize().equals(size)) Recorder::get().silentChangeSize(size);
	auto* layer = GJBaseGameLayer::get();
	auto* shader = layer ? layer->m_shaderLayer : nullptr;
	if (shader && (!shader->m_screenSize.equals(size) || !shader->m_targetTextureSize.equals(size))) Recorder::get().resizeShaderLayer(size);
}

void Recorder::releaseView() {
	if (!holding) return;
	holding = false;
	auto pixels = windowPixels().value_or(savedPixels);
	CCSize restore(
		savedFrame.width * pixels.width / std::max(savedPixels.width, 1.f),
		savedFrame.height * pixels.height / std::max(savedPixels.height, 1.f)
	);
	Recorder::get().silentChangeSize(restore);
	Recorder::get().resizeShaderLayer(restore);
	CCDirector::sharedDirector()->setNextDeltaTimeZero(true);
}

size_t Recorder::frameBytes() const {
	size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
	return nv12 ? pixels + pixels / 2 : pixels * 4;
}

#ifdef GEODE_IS_WINDOWS
namespace {
GLuint compileShader(GLenum kind, char const* source) {
	GLuint shader = glCreateShader(kind);
	if (!shader) return 0;
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok) return shader;
	glDeleteShader(shader);
	return 0;
}

GLuint linkProgram(char const* vertex, char const* fragment) {
	GLuint vs = compileShader(GL_VERTEX_SHADER, vertex);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}
	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	GLint ok = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (ok) return program;
	glDeleteProgram(program);
	return 0;
}

GLuint makeColorTarget(GLint internal, GLenum format, int width, int height) {
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

GLuint makeFramebuffer(GLuint texture) {
	GLuint buffer = 0;
	glGenFramebuffers(1, &buffer);
	glBindFramebuffer(GL_FRAMEBUFFER, buffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		glDeleteFramebuffers(1, &buffer);
		return 0;
	}
	return buffer;
}

std::string encoderFlags(std::string const& codec, std::string const& extra, bool withRate) {
	auto has = [&](char const* key) { return extra.find(key) != std::string::npos; };
	std::string flags;
	if (!has("-preset") && !has("-quality")) {
		if (codec.find("nvenc") != std::string::npos) flags += " -preset p1";
		else if (codec.find("amf") != std::string::npos) flags += " -quality speed";
		else if (codec.find("qsv") != std::string::npos) flags += " -preset veryfast";
		else if (codec == "libx264" || codec == "libx265") flags += " -preset " + Logic::get().renderPreset;
	}
	if (withRate && codec.find("nvenc") != std::string::npos && !has("-rc ")) flags += " -rc vbr";
	return flags;
}
std::string videoRate(std::string const& codec, int bitrate, std::string const& extra) {
	auto has = [&](char const* key) { return extra.find(key) != std::string::npos; };
	bool ownRate = has("-crf") || has("-qp") || has("-b:v") || has("-q:v") || has("-cq") || has("-global_quality") || has("-qscale") || has("-rc ");
	if (ownRate) return encoderFlags(codec, extra, false);
	if (bitrate > 0) return encoderFlags(codec, extra, true) + fmt::format(" -b:v {}", bitrate);
	std::string flags;
	if (!has("-preset") && !has("-quality")) {
		if (codec.find("nvenc") != std::string::npos) flags += " -preset p1";
		else if (codec.find("amf") != std::string::npos) flags += " -quality speed";
		else if (codec.find("qsv") != std::string::npos) flags += " -preset veryfast";
		else if (codec == "libx264" || codec == "libx265") flags += " -preset " + Logic::get().renderPreset;
	}
	if (codec.find("nvenc") != std::string::npos) flags += " -rc constqp -qp 0";
	else if (codec.find("amf") != std::string::npos) flags += " -rc cqp -qp_i 0 -qp_p 0";
	else if (codec.find("qsv") != std::string::npos) flags += " -global_quality 1";
	else flags += " -qp 0";
	return flags;
}

using SwapIntervalFn = BOOL (WINAPI*)(int);
using GetSwapIntervalFn = int (WINAPI*)();
}

void Recorder::drawFullQuad() {
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

bool Recorder::setupNv12() {
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
	yProg = linkProgram(vertex, luma);
	uvProg = linkProgram(vertex, chroma);
	if (!yProg || !uvProg) return false;
	GLint oldTexture = 0;
	GLint oldFbo = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
	yColor = makeColorTarget(GL_R8, GL_RED, width, height);
	uvColor = makeColorTarget(GL_RG8, GL_RG, width / 2, height / 2);
	yFbo = makeFramebuffer(yColor);
	uvFbo = makeFramebuffer(uvColor);
	glBindFramebuffer(GL_FRAMEBUFFER, oldFbo);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	return yColor && uvColor && yFbo && uvFbo;
}

void Recorder::closeNv12() {
	if (yProg) glDeleteProgram(yProg);
	if (uvProg) glDeleteProgram(uvProg);
	if (yFbo) glDeleteFramebuffers(1, &yFbo);
	if (uvFbo) glDeleteFramebuffers(1, &uvFbo);
	if (yColor) glDeleteTextures(1, &yColor);
	if (uvColor) glDeleteTextures(1, &uvColor);
	yProg = 0;
	uvProg = 0;
	yFbo = 0;
	uvFbo = 0;
	yColor = 0;
	uvColor = 0;
	nv12 = false;
}

void Recorder::convertNv12() {
	GLint oldProg = 0;
	GLint oldTexture = 0;
	GLint oldFbo = 0;
	GLint viewport[4] = {};
	glGetIntegerv(GL_CURRENT_PROGRAM, &oldProg);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
	glGetIntegerv(GL_VIEWPORT, viewport);
	GLboolean blend = glIsEnabled(GL_BLEND);
	GLboolean depth = glIsEnabled(GL_DEPTH_TEST);
	if (blend) glDisable(GL_BLEND);
	if (depth) glDisable(GL_DEPTH_TEST);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, color);
	glBindFramebuffer(GL_FRAMEBUFFER, yFbo);
	glViewport(0, 0, width, height);
	glUseProgram(yProg);
	glUniform1i(glGetUniformLocation(yProg, "u_texture"), 0);
	drawFullQuad();
	glBindFramebuffer(GL_FRAMEBUFFER, uvFbo);
	glViewport(0, 0, width / 2, height / 2);
	glUseProgram(uvProg);
	glUniform1i(glGetUniformLocation(uvProg, "u_texture"), 0);
	drawFullQuad();
	glUseProgram(oldProg);
	glBindTexture(GL_TEXTURE_2D, oldTexture);
	glBindFramebuffer(GL_FRAMEBUFFER, oldFbo);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	if (blend) glEnable(GL_BLEND);
	if (depth) glEnable(GL_DEPTH_TEST);
}
#endif

void Recorder::uncapPace() {
	if (paceHeld) return;
	if (auto* director = CCDirector::sharedDirector()) {
		savedInterval = director->getAnimationInterval();
		if (!(savedInterval > 0.f)) savedInterval = 1.f / 60.f;
		director->setAnimationInterval(1.f / 10000.f);
	}
#ifdef GEODE_IS_WINDOWS
	savedSwap = 1;
	if (auto getSwap = reinterpret_cast<GetSwapIntervalFn>(wglGetProcAddress("wglGetSwapIntervalEXT"))) savedSwap = getSwap();
	if (auto setSwap = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"))) setSwap(0);
#endif
	paceHeld = true;
}

void Recorder::restorePace() {
	if (!paceHeld) return;
	paceHeld = false;
	if (auto* director = CCDirector::sharedDirector()) director->setAnimationInterval(savedInterval > 0.f ? savedInterval : 1.f / 60.f);
#ifdef GEODE_IS_WINDOWS
	if (auto setSwap = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"))) setSwap(savedSwap);
#endif
}

bool Recorder::openTarget() {
	GLint oldFbo = 0;
	GLint oldTexture = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFbo);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
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
	glBindFramebuffer(GL_FRAMEBUFFER, oldFbo);
	glBindTexture(GL_TEXTURE_2D, oldTexture);

#ifdef GEODE_IS_WINDOWS
	nv12 = !useApi && Recorder::get().setupNv12();
	if (!nv12) Recorder::get().closeNv12();
	auto bytes = static_cast<GLsizeiptr>(Recorder::get().frameBytes());
	glGenBuffers(kRing, pbo.data());
	for (auto buffer : pbo) {
		glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
		glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	fences.fill(nullptr);
	inFlight.clear();
	nextSlot = 0;
#endif

	shade = CCLayerColor::create(ccc4(0, 0, 0, 0));
	shade->retain();
	barBack = CCLayerColor::create(ccc4(255, 255, 255, 45));
	barBack->retain();
	barFill = CCLayerColor::create(ccc4(80, 210, 130, 255));
	barFill->retain();
	barText = CCLabelBMFont::create("Rendering", "bigFont.fnt");
	if (barText) barText->retain();
	barShown.clear();
	textClock = 0.0;
	return complete;
}

void Recorder::closeTarget() {
#ifdef GEODE_IS_WINDOWS
	for (auto& fence : fences) {
		if (fence) glDeleteSync(fence);
		fence = nullptr;
	}
	inFlight.clear();
	if (pbo[0]) glDeleteBuffers(kRing, pbo.data());
	pbo.fill(0);
	Recorder::get().closeNv12();
#endif
	if (fbo) glDeleteFramebuffers(1, &fbo);
	if (color) glDeleteTextures(1, &color);
	fbo = 0;
	color = 0;
	if (shade) shade->release();
	shade = nullptr;
	if (barBack) barBack->release();
	if (barFill) barFill->release();
	if (barText) barText->release();
	barBack = nullptr;
	barFill = nullptr;
	barText = nullptr;
}

void Recorder::drawFrame() {
	auto* director = CCDirector::sharedDirector();
	GLint window = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &window);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, width, height);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	kmGLPushMatrix();
	if (auto* play = PlayLayer::get()) play->visit();
	else if (auto* scene = director->getRunningScene()) scene->visit();
	if (fade < 1.f && shade) {
		shade->setContentSize(director->getWinSize());
		shade->setOpacity(static_cast<GLubyte>(std::lround((1.f - fade) * 255.f)));
		shade->visit();
	}
	kmGLPopMatrix();

#ifdef GEODE_IS_WINDOWS
	if (nv12) Recorder::get().convertNv12();
	int slot = nextSlot;
	while (fences[slot] && Recorder::get().harvest(true)) {}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	if (nv12) {
		glBindFramebuffer(GL_FRAMEBUFFER, yFbo);
		glReadPixels(0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		glBindFramebuffer(GL_FRAMEBUFFER, uvFbo);
		auto offset = static_cast<uintptr_t>(width) * static_cast<uintptr_t>(height);
		glReadPixels(0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE, reinterpret_cast<void*>(offset));
	} else {
		glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	inFlight.push_back(slot);
	nextSlot = (slot + 1) % kRing;
	while (Recorder::get().harvest(false)) {}
#else
	auto frame = Recorder::get().acquireFrame();
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
	Recorder::get().queueFrame(std::move(frame));
#endif
	glBindFramebuffer(GL_FRAMEBUFFER, window);
}

void Recorder::showOverlay() {
	if (!active) return;
#ifdef GEODE_IS_MOBILE
	// Touch coordinates and screen controls use the physical view, not the video dimensions.
	if (holding) silentChangeSize(savedFrame);
#endif
	auto pixels = windowPixels().value_or(savedPixels);
	if (pixels.width < 1.f || pixels.height < 1.f) pixels = CCSize(1.f, 1.f);
	GLfloat clear[4] = {};
	glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
	GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (scissor) glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(screenFbo));
	glViewport(0, 0, static_cast<GLsizei>(pixels.width), static_cast<GLsizei>(pixels.height));
	glClearColor(0.f, 0.f, 0.f, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	PauseLayer* pause = nullptr;
	if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) pause = scene->getChildByType<PauseLayer>(0);
	if (!pause) {
		if (auto* play = PlayLayer::get()) pause = play->getChildByType<PauseLayer>(0);
	}
	if (pause) {
		kmGLPushMatrix();
		pause->visit();
		kmGLPopMatrix();
	}
	Recorder::get().drawProgress();

	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	if (scissor) glEnable(GL_SCISSOR_TEST);
}

void Recorder::drawProgress() {
	if (!barBack || !barFill || !barText) return;
	auto status = Logic::get().renderStatus();
	if (!status.active) return;
	auto win = CCDirector::sharedDirector()->getWinSize();
	float barW = win.width * 0.6f;
	float barH = std::max(win.height * 0.014f, 2.f);
	float x = (win.width - barW) * 0.5f;
	float y = win.height * 0.08f;
	barBack->setContentSize({barW, barH});
	barBack->setPosition({x, y});
	barFill->setContentSize({barW * (status.known ? status.progress : 0.f), barH});
	barFill->setPosition({x, y});
	textClock += wallDelta;
	if (barShown.empty() || textClock >= 0.2) {
		textClock = 0.0;
		if (status.text != barShown) {
			barShown = status.text;
			barText->setString(barShown.c_str());
		}
	}
	barText->setScale(0.4f * win.height / 320.f);
	barText->setPosition({win.width * 0.5f, y + barH + win.height * 0.04f});
	kmGLPushMatrix();
	barBack->visit();
	barFill->visit();
	barText->visit();
	kmGLPopMatrix();
}

#ifdef GEODE_IS_WINDOWS
bool Recorder::harvest(bool wait) {
	if (inFlight.empty()) return false;
	int slot = inFlight.front();
	GLenum state = glClientWaitSync(fences[slot], wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, wait ? 5'000'000'000ull : 0);
	if (!wait && state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED) return false;
	glDeleteSync(fences[slot]);
	fences[slot] = nullptr;
	inFlight.pop_front();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[slot]);
	if (auto const* data = static_cast<uint8_t const*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY))) {
		auto frame = Recorder::get().acquireFrame();
		std::memcpy(frame.data(), data, frame.size());
		glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
		Recorder::get().queueFrame(std::move(frame));
	}
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	return true;
}
#endif

std::vector<uint8_t> Recorder::acquireFrame() {
	std::vector<uint8_t> frame;
	{
		std::lock_guard guard(lock);
		if (!spareFrames.empty()) {
			frame = std::move(spareFrames.back());
			spareFrames.pop_back();
		}
	}
	frame.resize(frameBytes());
	return frame;
}

void Recorder::queueFrame(std::vector<uint8_t> frame) {
	std::unique_lock guard(lock);
	wake.wait(guard, [this] { return queue.size() < kQueue || broken; });
	if (broken) return;
	queue.push_back(std::move(frame));
	wake.notify_all();
}

void Recorder::encodeLoop() {
	while (true) {
		std::vector<uint8_t> frame;
		{
			std::unique_lock guard(lock);
			wake.wait(guard, [this] { return closing || !queue.empty(); });
			if (queue.empty()) return;
			frame = std::move(queue.front());
			queue.pop_front();
		}
		wake.notify_all();
		if (!broken && !Recorder::get().writeOut(frame)) {
			broken = true;
			wake.notify_all();
		}
		{
			std::lock_guard guard(lock);
			if (spareFrames.size() < kQueue + 1) spareFrames.push_back(std::move(frame));
		}
	}
}

void Recorder::finishVideo() {
#ifdef GEODE_IS_WINDOWS
	while (Recorder::get().harvest(true)) {}
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
	spareFrames.clear();
	closing = false;
#ifdef ECHO_FFMPEG_API
	if (api) api->stop();
	api.reset();
#endif
#ifdef GEODE_IS_WINDOWS
	Recorder::get().closeHandle(video);
	if (process) {
		if (WaitForSingleObject(process, 30000) != WAIT_OBJECT_0) TerminateProcess(process, 0);
		CloseHandle(process);
		process = nullptr;
	}
#endif
}

bool Recorder::apiAvailable() {
#ifdef ECHO_FFMPEG_API
	return Loader::get()->isModLoaded("eclipse.ffmpeg-api") && ffmpeg::events::impl::getVTable().createRecorder;
#else
	return false;
#endif
}

bool Recorder::preferApi() {
#ifndef ECHO_FFMPEG_API
	return false;
#elif defined(GEODE_IS_WINDOWS)
	return !std::filesystem::exists(Recorder::get().gameDirectory() / "ffmpeg.exe") && Recorder::apiAvailable();
#else
	return true;
#endif
}

bool Recorder::idle() const {
	if (active) return false;
#ifdef ECHO_FFMPEG_API
	if (api) return false;
#endif
#ifdef GEODE_IS_WINDOWS
	return process == nullptr && video == INVALID_HANDLE_VALUE;
#else
	return true;
#endif
}

bool Recorder::writeOut(std::vector<uint8_t> const& frame) {
#ifdef ECHO_FFMPEG_API
	if (api) {
		auto result = api->writeFrame(frame);
		if (result.isErr()) {
			std::lock_guard guard(lock);
			failure = result.unwrapErr();
		}
		return result.isOk();
	}
#endif
#ifdef GEODE_IS_WINDOWS
	return Recorder::get().writeAll(video, frame.data(), frame.size());
#else
	return false;
#endif
}

void Recorder::goLive() {
	auto& value = Logic::get();
	worker = std::thread([] { Recorder::get().encodeLoop(); });
	uncapPace();
	active = true;
#ifdef ECHO_FFMPEG_API
	value.message = useApi ? "Rendering (FFmpeg API)" : nv12 ? "Rendering (GPU)" : "Rendering";
#else
	value.message = nv12 ? "Rendering (GPU)" : "Rendering";
#endif
	if (!AudioTap::get().attach(value.renderMusic, value.renderSfx, audio)) {
		value.message = "Rendering (audio capture failed, video only)";
	}
}

#ifdef ECHO_FFMPEG_API
std::string Recorder::openApi(int fps) {
#ifdef GEODE_IS_ANDROID
	installEncoderOptions();
#endif
	auto const& value = Logic::get();
	if (!Recorder::apiAvailable()) return "FFmpeg API is not installed";
	auto encoder = std::make_unique<ffmpeg::events::Recorder>();
	if (!encoder->isValid()) return "FFmpeg API did not respond";
	auto codecs = ffmpeg::events::Recorder::getAvailableCodecs();
	auto offered = [&](std::string const& name) { return codecs.empty() || std::ranges::find(codecs, name) != codecs.end(); };
	std::vector<std::string> wanted;
#ifdef GEODE_IS_ANDROID
	// Default software selection may use Android's hardware encoder when available.
	if (value.renderCodec.empty() || value.renderCodec == "libx264") wanted.push_back("h264_mediacodec");
#endif
	std::vector<std::string> fallbacks{value.renderCodec, "libx264", "h264_videotoolbox", "h264_mediacodec", "libopenh264", "mpeg4"};
	wanted.insert(wanted.end(), fallbacks.begin(), fallbacks.end());
	std::vector<std::string> picks;
	std::ranges::copy_if(wanted, std::back_inserter(picks), [&](std::string const& name) {
		return !name.empty() && offered(name) && std::ranges::find(picks, name) == picks.end();
	});
	auto colorModes = value.renderColorFix ? std::vector<bool>{true, false} : std::vector<bool>{false};
	std::vector<std::pair<std::string, bool>> attempts;
	std::ranges::for_each(picks, [&](std::string const& codec) {
		std::ranges::transform(colorModes, std::back_inserter(attempts), [&](bool colorFix) { return std::pair{codec, colorFix}; });
	});
	int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
	int64_t bitrate = value.renderBitrate > 0 ? value.renderBitrate : std::max<int64_t>(pixels * fps / 4, 8'000'000);
	std::string error = "No usable video codec";
	auto opened = std::ranges::find_if(attempts, [&](auto const& attempt) {
		auto const& [codec, colorFix] = attempt;
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
		// FFmpeg API creates the colorspace filter itself and expects only its options.
		settings.m_colorspaceFilters = colorFix ? "all=bt709:iall=bt470bg:fast=1" : "";
#ifdef GEODE_IS_ANDROID
		s_echoPreset = codec == "libx264" ? value.renderPreset.c_str() : nullptr;
		struct PresetScope { ~PresetScope() { s_echoPreset = nullptr; } } resetPreset;
#endif
		// A failed filter/codec setup must not leave partial encoder state in the retry.
		encoder = std::make_unique<ffmpeg::events::Recorder>();
		auto result = encoder->init(settings);
		if (result.isErr()) error = result.unwrapErr();
		return result.isOk();
	});
	if (opened == attempts.end()) {
		std::error_code ignored;
		std::filesystem::remove(output, ignored);
		return error.empty() ? std::string("FFmpeg API could not open the encoder") : error;
	}
	api = std::move(encoder);
	return {};
}
#endif

namespace {
bool writeWav(std::filesystem::path const& raw, std::filesystem::path const& wav, int rate, int channels) {
	std::ifstream in(raw, std::ios::binary);
	std::ofstream out(wav, std::ios::binary | std::ios::trunc);
	if (!in || !out || rate <= 0 || channels <= 0) return false;
	auto put = [&](auto value) { out.write(reinterpret_cast<char const*>(&value), sizeof(value)); };
	out.write("RIFF", 4);
	put(uint32_t{0});
	out.write("WAVEfmt ", 8);
	put(uint32_t{16});
	put(uint16_t{1});
	put(uint16_t{2});
	put(static_cast<uint32_t>(rate));
	put(static_cast<uint32_t>(rate) * 4u);
	put(uint16_t{4});
	put(uint16_t{16});
	out.write("data", 4);
	put(uint32_t{0});
	auto sample = [](float value) { return static_cast<int16_t>(std::lround((std::isfinite(value) ? std::clamp(value, -1.f, 1.f) : 0.f) * 32767.f)); };
	size_t width = static_cast<size_t>(channels);
	std::vector<float> block(width * 65536);
	uint64_t bytes = 0;
	while (in) {
		in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size() * sizeof(float)));
		size_t count = static_cast<size_t>(in.gcount()) / (sizeof(float) * width);
		if (count == 0) break;
		auto pcm = std::views::iota(size_t{0}, count)
			| std::views::transform([&](size_t i) {
				float const* frame = block.data() + i * width;
				return std::array<int16_t, 2>{sample(frame[0]), sample(width > 1 ? frame[1] : frame[0])};
			})
			| std::ranges::to<std::vector>();
		out.write(reinterpret_cast<char const*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * sizeof(pcm[0])));
		bytes += count * 4;
	}
	auto size = static_cast<uint32_t>(std::min<uint64_t>(bytes, 0xFFFFFFFFull - 36));
	out.seekp(4);
	put(36u + size);
	out.seekp(40);
	put(size);
	return bytes > 0 && out.good();
}
}

void Recorder::saveApiRender() {
#ifdef ECHO_FFMPEG_API
	auto& tap = AudioTap::get();
	std::error_code error;
	bool hasAudio = !audio.empty() && frames > 0 && tap.written > 0 && tap.rate > 0 && tap.channels > 0 && std::filesystem::exists(output, error);
	auto finish = [](std::filesystem::path const& video, std::string const& failed) {
		Recorder::get().saving = false;
		auto& value = Logic::get();
		if (failed.empty()) {
			value.message = "Render stopped";
			value.notify(fmt::format("Saved {}", video.filename().string()), true);
		} else {
			value.message = "Video saved, audio mux failed: " + failed;
			value.notify(value.message, false);
		}
		value.refreshPopup();
	};
	if (!hasAudio) {
		if (!audio.empty()) std::filesystem::remove(audio, error);
		if (frames > 0 && std::filesystem::exists(output, error)) finish(output, {});
		else Logic::get().message = "Render stopped";
		return;
	}
	saving = true;
	std::thread([finish, video = output, raw = audio, rate = tap.rate, channels = tap.channels] {
		auto folder = video.parent_path();
		auto wav = folder / (video.stem().string() + ".audio.wav");
		auto temp = folder / (video.stem().string() + ".part" + video.extension().string());
		std::string failed;
		std::error_code error;
		// FFmpeg API 2.0 opens WAV decoders before assigning stream parameters.
		// Its raw mixer accepts interleaved stereo float audio and derives the
		// source sample rate from the finalized video duration.
		std::vector<float> stereo;
		try { stereo = echo::audio::readStereo(raw, channels); }
		catch (std::exception const& error) { failed = error.what(); }
#ifdef ECHO_FFMPEG_API
		if (failed.empty()) {
			try {
				if (auto mixed = ffmpeg::events::AudioMixer::mixVideoRaw(video, stereo, temp); mixed.isErr()) failed = mixed.unwrapErr();
			} catch (std::exception const& error) { failed = error.what(); }
		}
#endif
		if (failed.empty() && !std::filesystem::exists(temp, error)) failed = "FFmpeg API did not write the video";
		if (failed.empty()) {
			auto backup = folder / (video.filename().string() + ".unmuxed");
			std::filesystem::rename(video, backup, error);
			if (!error) {
				std::filesystem::rename(temp, video, error);
				if (error) {
					std::error_code restoreError;
					std::filesystem::rename(backup, video, restoreError);
				} else std::filesystem::remove(backup, error);
			}
			if (error) failed = "could not replace the video";
		}
		if (failed.empty()) std::filesystem::remove(raw, error);
		else {
			std::filesystem::remove(temp, error);
			if (writeWav(raw, wav, rate, channels)) {
				std::filesystem::remove(raw, error);
				failed += "; audio kept in " + wav.filename().string();
			} else failed += "; raw audio kept in " + raw.filename().string();
		}
		Loader::get()->queueInMainThread([finish, video, failed] { finish(video, failed); });
	}).detach();
#endif
}

#ifdef GEODE_IS_WINDOWS
bool Recorder::muxAudio() {
	auto& tap = AudioTap::get();
	std::error_code error;
	if (audio.empty()) return true;
	bool hasAudio = frames > 0 && tap.written > 0 && tap.rate > 0 && tap.channels > 0 && std::filesystem::exists(output, error);
	if (!hasAudio) {
		std::filesystem::remove(audio, error);
		return true;
	}
	auto folder = output.parent_path();
	auto log = folder / "echo-ffmpeg.log";
	auto temp = folder / (output.stem().string() + ".part" + output.extension().string());
	bool lossless = Logic::get().renderBitrate <= 0;
	std::string audioCodec = lossless ? "-c:a flac" : "-c:a aac -b:a 320k";
	std::string command = fmt::format(
		"\"{}\" -y -nostdin -loglevel error -i \"{}\" -f f32le -ar {} -ac {} -i \"{}\" -map 0:v:0 -map 1:a:0 -c:v copy {} -ac 2 -muxdelay 0 -muxpreload 0 -shortest \"{}\"",
		Recorder::get().ffmpegPath(),
		output.string(),
		tap.rate,
		tap.channels,
		audio.string(),
		audioCodec,
		temp.string()
	);
	std::string failed = Recorder::get().runHidden(command, log);
	std::filesystem::remove(audio, error);
	std::filesystem::remove(log, error);
	if (!failed.empty()) {
		std::filesystem::remove(temp, error);
		Logic::get().message = "Video saved, audio mux failed: " + failed;
		return false;
	}
	std::filesystem::remove(output, error);
	std::filesystem::rename(temp, output, error);
	if (error) {
		Logic::get().message = "Video saved, audio mux failed: could not replace the video";
		return false;
	}
	return true;
}
#endif



namespace {
float levelFraction(PlayLayer* play, int lastInputFrame) {
	if (!play) return -1.f;
	if (!play->m_isPlatformer && play->m_player1 && play->m_endPortal) {
		float endX = play->m_endPortal->getPositionX();
		if (endX > 0.f) return std::clamp(play->m_player1->getPositionX() / endX, 0.f, 1.f);
	}
	if (Logic::get().isPlaying() && lastInputFrame > 0) {
		return std::clamp(static_cast<float>(Logic::get().stepFrame) / static_cast<float>(lastInputFrame), 0.f, 1.f);
	}
	return -1.f;
}

std::string clockText(double seconds) {
	int total = static_cast<int>(std::max(seconds, 0.0) + 0.5);
	if (total >= 3600) return fmt::format("{}:{:02}:{:02}", total / 3600, total / 60 % 60, total % 60);
	return fmt::format("{}:{:02}", total / 60, total % 60);
}
}

RenderStatus Logic::renderStatus() {
	auto const& film = Recorder::get();
	RenderStatus status;
	if (!film.active) return status;
	status.active = true;
	if (film.paused) {
		status.text = "Render paused";
		return status;
	}
	auto* play = PlayLayer::get();
	double video = static_cast<double>(film.frames) * static_cast<double>(Logic::get().renderStep());
	if (film.busySeconds > 0.05) status.speed = static_cast<float>(video / film.busySeconds);
	std::string speed = status.speed > 0.f ? fmt::format("{:.1f}x realtime", status.speed) : std::string("measuring speed");
	if (play && play->m_hasCompletedLevel) {
		status.known = true;
		status.finishing = true;
		status.progress = 1.f;
		status.text = fmt::format("Finishing  |  {}", speed);
		return status;
	}
	float now = levelFraction(play, film.lastInputFrame);
	if (now < 0.f) {
		status.text = fmt::format("{} rendered  |  {}", clockText(video), speed);
		return status;
	}
	status.known = true;
	status.progress = now;
	float done = now - std::max(film.startFraction, 0.f);
	std::string left = "estimating";
	if (done > 0.002f && film.busySeconds > 0.5) {
		double tail = status.speed > 0.f ? static_cast<double>(Logic::get().renderAfterEnd) / status.speed : 0.0;
		status.eta = static_cast<float>(film.busySeconds * (1.0 - now) / done + tail);
		left = clockText(status.eta) + " left";
	}
	status.text = fmt::format("{:.1f}%  |  {}  |  {}", now * 100.f, speed, left);
	return status;
}

float Logic::renderWallDelta() {
	return Recorder::get().wallDelta;
}

bool Logic::renderActive() {
	return Recorder::get().active || Recorder::get().starting;
}
bool Logic::renderCapturing() {
	return Recorder::get().active;
}
bool Logic::renderPaused() {
	return Recorder::get().active && Recorder::get().paused;
}
bool Logic::renderHoldsView() {
	return Recorder::get().holding;
}
bool Logic::renderBlocksEngine() {
	return AudioTap::get().attached && !AudioTap::get().engineOpen;
}
bool Logic::renderBlocksMixer() {
	return AudioTap::get().attached && !AudioTap::get().mixing;
}
bool Logic::renderShouldSkip() {
	auto* play = PlayLayer::get();
	if (!play || Logic::get().renderPaused()) return true;
	if (Logic::get().pauseMenuOpen()) return true;
	bool started = play->m_started || (Logic::get().renderRecordPause && Recorder::get().seenFrames >= 2);
	if (!started) return true;
	if (play->m_isPaused) return true;
	return false;
}
bool Logic::beginRenderFrame() {
	auto& film = Recorder::get();
	if (!film.active) return false;
	auto now = std::chrono::steady_clock::now();
	double gap = film.ticked ? std::chrono::duration<double>(now - film.lastTick).count() : 0.0;
	film.lastTick = now;
	film.ticked = true;
	film.wallDelta = static_cast<float>(std::clamp(gap, 0.0, 0.25));
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &film.screenFbo);
	++film.seenFrames;
	film.holdView();
	bool capture = !Logic::get().renderShouldSkip();
	if (capture) {
		film.busySeconds += film.wallDelta;
		AudioTap::get().holdEffects();
	}
	return capture;
}
void Logic::holdRenderEffects() {
	AudioTap::get().holdEffects();
}
void Logic::showRenderOverlay() {
	Recorder::get().showOverlay();
}
bool Logic::captureRenderFrame() {
	auto& film = Recorder::get();
	auto* play = PlayLayer::get();
	if (!film.active || !play) return false;
	auto const& value = Logic::get();
	float dt = Logic::get().renderStep();
	if (play->m_hasCompletedLevel) {
		film.endTime += dt;
		if (film.endTime >= value.renderAfterEnd) {
			bool leave = value.renderAutoExit;
			Logic::get().stopRender();
			if (leave) {
				Loader::get()->queueInMainThread([] {
					if (auto* layer = PlayLayer::get()) layer->onQuit();
				});
			}
			return false;
		}
	}
	float fadeIn = value.renderFadeIn <= 0.f ? 1.f : std::clamp(static_cast<float>(film.frames) * dt / value.renderFadeIn, 0.f, 1.f);
	float fadeOut = 1.f;
	if (play->m_hasCompletedLevel && value.renderFadeOut > 0.f) {
		fadeOut = std::clamp((value.renderAfterEnd - film.endTime) / value.renderFadeOut, 0.f, 1.f);
	}
	film.fade = std::min(fadeIn, fadeOut);
	if (film.startFraction < 0.f) film.startFraction = levelFraction(play, film.lastInputFrame);
	// The song plays at its own speed on the video clock; the picture follows it.
	AudioTap::get().advance(dt, film.fade);
	film.drawFrame();
	++film.frames;
	if (film.broken) {
		std::string reason = film.useApi ? "Encoding failed: " + film.failure : std::string("The render pipe closed");
		Logic::get().stopRender();
		Logic::get().notify(reason, false);
	}
	return true;
}
bool Logic::renderBatchMore(std::chrono::steady_clock::time_point started) {
#ifdef GEODE_IS_WINDOWS
	return false;
#else
	return Recorder::get().active && !Logic::get().renderShouldSkip() && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(40);
#endif
}






bool Logic::ffmpegReady() {
#ifdef GEODE_IS_WINDOWS
	return std::filesystem::exists(Recorder::get().gameDirectory() / "ffmpeg.exe") || Recorder::apiAvailable();
#else
	return Recorder::apiAvailable();
#endif
}
void Logic::stopRender() {
	auto& film = Recorder::get();
#ifdef GEODE_IS_WINDOWS
	if (film.starting && !film.active) {
		film.cancel = true;
		if (film.video != INVALID_HANDLE_VALUE) CancelIoEx(film.video, nullptr);
		Logic::get().message = "Render canceled";
		Logic::get().refreshPopup();
		return;
	}
#endif
	if (film.idle()) return;
	film.starting = false;
	film.cancel = false;
	film.active = false;
	film.restorePace();
	AudioTap::get().detach();
	film.finishVideo();
	film.closeTarget();
	film.releaseView();
	if (film.useApi) {
		film.saveApiRender();
		Logic::get().refreshPopup();
		return;
	}
#ifdef GEODE_IS_WINDOWS
	bool muxed = film.muxAudio();
	if (muxed) Logic::get().message = "Render stopped";
#endif
	Logic::get().refreshPopup();
}
void Logic::startRender() {
	auto& film = Recorder::get();
	if (film.active || film.starting) return;
	if (film.saving) {
		Logic::get().notify("Still saving the last render", false);
		return;
	}
	auto* play = PlayLayer::get();
	auto* director = CCDirector::sharedDirector();
	auto* view = director ? director->getOpenGLView() : nullptr;
	if (!play || !view) {
		Logic::get().renderQueued = true;
		Logic::get().message = "Render starts when the level opens";
		Logic::get().refreshPopup();
		return;
	}
	auto& value = Logic::get();
	int width = std::max(value.renderWidth, 2) & ~1;
	int height = std::max(value.renderHeight, 2) & ~1;
	if (width > 7680 || height > 4320) {
		Logic::get().notify("Render size is too large. Use 7680x4320 or smaller.", false);
		return;
	}
	int fps = std::max(value.renderFps, 1);
	std::string ext = value.renderExtension.empty() ? "mp4" : value.renderExtension;
	if (ext.front() == '.') ext.erase(ext.begin());
	auto folder = Logic::get().saveRoot() / "renders";
	std::filesystem::create_directories(folder);
	auto output = ReplayIO::uniqueFile(folder, ReplayIO::sanitize(Logic::get().renderFileStem()), "." + ext);
	film.width = width;
	film.height = height;
	film.useApi = Recorder::preferApi();
#ifdef ECHO_FFMPEG_API
	if (film.useApi && !Recorder::apiAvailable()) {
		value.ffmpegMissing = true;
		value.message = "FFmpeg API is not installed";
		Logic::get().notify("Rendering needs the FFmpeg API mod. Install it from the Geode mod index.", false);
		Logic::get().refreshPopup();
		return;
	}
#endif
	if (!film.openTarget()) {
		film.closeTarget();
		Logic::get().notify("Could not create the render target", false);
		return;
	}
	auto arm = [&] {
		film.output = output;
		film.audio = folder / (output.stem().string() + ".audio.raw");
		film.paused = false;
		film.frames = 0;
		film.seenFrames = 0;
		film.endTime = 0.f;
		film.fade = 1.f;
		film.cancel = false;
		film.broken = false;
		film.failure.clear();
		film.ticked = false;
		film.wallDelta = 0.f;
		film.busySeconds = 0.0;
		film.startFraction = -1.f;
		film.lastInputFrame = value.inputs.empty() ? 0 : std::ranges::max(value.inputs, {}, &Frame::number).number;
		value.ffmpegMissing = false;
		value.renderQueued = false;
		value.frameAdvance = false;
		value.holdingAdvance = false;
		value.advanceQueued = false;
	};
#ifdef ECHO_FFMPEG_API
	if (film.useApi) {
		film.output = output;
		if (auto failed = film.openApi(fps); !failed.empty()) {
			film.closeTarget();
			value.message = "FFmpeg API failed: " + failed;
			Logic::get().notify(value.message, false);
			Logic::get().refreshPopup();
			return;
		}
		arm();
		film.goLive();
		Logic::get().refreshPopup();
		return;
	}
#endif
#ifdef GEODE_IS_WINDOWS
	DWORD id = GetTickCount();
	std::string videoName = fmt::format("\\\\.\\pipe\\echo-film-v-{}", id);
	DWORD pipeMode = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED;
	size_t frameBytes = film.frameBytes();
	DWORD videoCap = static_cast<DWORD>(std::min(std::max(frameBytes * 4, frameBytes), static_cast<size_t>(256u << 20)));
	film.video = CreateNamedPipeA(videoName.c_str(), pipeMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, videoCap, 0, 0, nullptr);
	if (film.video == INVALID_HANDLE_VALUE) {
		film.closeTarget();
		Logic::get().notify("Could not open the render pipe", false);
		return;
	}
	std::string codec = value.renderCodec.empty() ? "libx264" : value.renderCodec;
	bool hardware = codec.find("nvenc") != std::string::npos || codec.find("amf") != std::string::npos || codec.find("qsv") != std::string::npos;
	char const* outPix = value.renderBitrate <= 0 && !hardware ? "yuv444p" : "yuv420p";
	// NV12 shaders already convert RGB directly to BT.709. Do not convert it a second time.
	std::string colorFix = value.renderColorFix && !film.nv12 ? "-vf colorspace=all=bt709:iall=bt470bg:fast=1 " : "";
	auto hasArg = [&](char const* key) { return value.renderArgs.find(key) != std::string::npos; };
	std::string pix = hasArg("-pix_fmt") ? std::string() : fmt::format(" -pix_fmt {}", outPix);
	std::string color = hasArg("-colorspace") || hasArg("-color_primaries") || hasArg("-color_trc") || hasArg("-color_range")
		? std::string()
		: " -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv";
	std::string extra = value.renderArgs.empty() ? std::string() : " " + value.renderArgs;
	std::string command = fmt::format(
		"\"{}\" -y -nostdin -loglevel error -f rawvideo -pix_fmt {} -s {}x{} -r {} -probesize 32 -analyzeduration 0 -fflags nobuffer -i {} -an "
		"{}-c:v {}{}{}{}{} \"{}\"",
		Recorder::get().ffmpegPath(),
		film.nv12 ? "nv12" : "bgra",
		width,
		height,
		fps,
		videoName,
		colorFix,
		codec,
		videoRate(codec, value.renderBitrate, value.renderArgs),
		pix,
		color,
		extra,
		output.string()
	);
	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process{};
	std::vector<char> mutableCommand(command.begin(), command.end());
	mutableCommand.push_back('\0');
	if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
		film.closeTarget();
		Recorder::get().closeHandle(film.video);
		Logic::get().ffmpegMissing = true;
		Logic::get().message = "ffmpeg.exe was not found";
		Logic::get().notify("ffmpeg.exe was not found. Use the download link on the Render tab, then place ffmpeg.exe next to Geometry Dash.", false);
		GUI::get().revealMenu();
		return;
	}
	CloseHandle(process.hThread);
	film.process = process.hProcess;
	arm();
	film.starting = true;
	value.message = "Starting render...";
	Logic::get().refreshPopup();
	std::thread([] {
		bool ok = Recorder::get().connectPipe(Recorder::get().video);
		Loader::get()->queueInMainThread([ok] {
			auto& film = Recorder::get();
			bool canceled = film.cancel;
			film.starting = false;
			film.cancel = false;
			if (!ok || canceled) {
				Logic::get().stopRender();
				if (!ok && !canceled) {
					Logic::get().message = "ffmpeg did not connect";
					Logic::get().notify("ffmpeg did not connect", false);
					Logic::get().refreshPopup();
				}
				return;
			}
			if (!PlayLayer::get()) {
				Logic::get().stopRender();
				Logic::get().notify("The level closed before the render started", false);
				return;
			}
			film.goLive();
		});
	}).detach();
#else
	film.closeTarget();
#endif
}
