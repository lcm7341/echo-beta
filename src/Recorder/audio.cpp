#include <chrono>
#include "Logic/logic.hpp"
#include "Logic/child_type_cache.hpp"
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

using namespace geode::prelude;

namespace {
bool g_liveOpeningHeld = false;
double g_liveOpeningSec = 0.0;
}

void Logic::prepareLevelView(PlayLayer* play) {
	if (!play || !play->m_shaderLayer || Logic::get().renderHoldsView()) return;
	// The shader layer is created at the game's own target size, which is not the
	// point size on a scaled window. Rebuilding that buffer at getWinSize() makes
	// every effect sample a coarse texture, so blur and the rest look pixelated.
	play->m_shaderLayer->performCalculations();
}

float Logic::renderStep() {
	return 1.f / static_cast<float>(std::max(Logic::get().renderFps, 1));
}

float Logic::scrollSpeedFactor() {
	auto* layer = GJBaseGameLayer::get();
	auto* player = layer ? layer->m_player1 : nullptr;
	if (!player) return 1.f;
	static float const speeds[5] = {251.1601f, 311.5801f, 387.4201f, 468.0002f, 576.0002f};
	float speed = player->m_playerSpeed;
	int index = 1;
	if (speed == 0.7f) index = 0;
	else if (speed == 1.1f) index = 2;
	else if (speed == 1.3f) index = 3;
	else if (speed == 1.6f) index = 4;
	float step = speeds[index] / static_cast<float>(std::max(Logic::get().sampleRate(), 1.0));
	float moved = (player->getPositionX() + step) - player->getPositionX();
	float factor = step > 0.f ? moved / step : 1.f;
	if (!(factor > 0.f) || layer->m_levelEndAnimationStarted) return 1.f;
	return factor;
}

float Logic::renderSpeed() {
	if (!Logic::get().renderSpeedhack) return 1.f;
	float speed = Logic::get().speedhack;
	if (!std::isfinite(speed) || speed < 0.01f) return 1.f;
	return speed;
}

// Each video frame moves the level renderStep() (one frame of video) times the render speed. The
// audio still advances one frame of video, with the song pitched to the speed in syncMusicPitch.
float Logic::renderGameStep() {
	float dt = Logic::get().renderStep() * Logic::get().renderSpeed();
	if (Logic::get().renderSsbFix) {
		float factor = Logic::get().scrollSpeedFactor();
		if (factor > 0.f) dt /= factor;
	}
	return dt;
}

bool Logic::pauseMenuOpen() {
	auto* director = CCDirector::sharedDirector();
	auto* scene = director ? director->getRunningScene() : nullptr;
#ifdef GEODE_IS_WINDOWS
	// Releases query this several times in one physics step. Classify unchanged
	// children once instead of repeatedly entering Geode's RTTI machinery.
	static thread_local replay::ChildTypeCache<CCObject> sceneTypes, playTypes;
	auto containsPause = [](CCNode* node, auto& cache) {
		auto* children = node ? node->getChildren() : nullptr;
		std::span<CCObject*> objects;
		if (children && children->data) objects = {children->data->arr, children->data->num};
		return cache.contains(objects, [](CCObject* object) { return object->m_uID; },
			[](CCObject* object) { return typeinfo_cast<PauseLayer*>(object) != nullptr; });
	};
	if (containsPause(scene, sceneTypes)) return true;
	return containsPause(PlayLayer::get(), playTypes);
#else
	if (scene && scene->getChildByType<PauseLayer>(0)) return true;
	auto* play = PlayLayer::get();
	return play && play->getChildByType<PauseLayer>(0);
#endif
}

void Logic::syncMusicPitch() {
	auto* engine = FMODAudioEngine::sharedEngine();
	if (!engine) return;
	auto* play = PlayLayer::get();
	bool rendering = Logic::get().renderActive();
	float pitch = 1.f;
	if (play) pitch = rendering ? Logic::get().renderSpeed() : std::max(Logic::get().speedhack, 0.01f);

	// Keep the song frozen until the attempt actually starts, including while the
	// opening pause is being recorded. Advancing it there plays the song early.
	if (play && !play->m_started) {
		if (!g_liveOpeningHeld) {
			g_liveOpeningHeld = true;
			g_liveOpeningSec = static_cast<double>(engine->getMusicTimeMS(0)) / 1000.0;
		}
		unsigned ms = static_cast<unsigned>(std::llround(std::max(g_liveOpeningSec, 0.0) * 1000.0));
		// The song is paused here, so it stays where it was put. Seeking waits on FMOD
		// (setMusicTimeMS polls the stream's open state), and doing it every frame before
		// the attempt started was a quarter of the frame.
		unsigned now = engine->getMusicTimeMS(0);
		if ((now > ms ? now - ms : ms - now) > 2) engine->setMusicTimeMS(ms, false, 0);
		if (engine->m_backgroundMusicChannel) {
			engine->m_backgroundMusicChannel->setPitch(pitch);
			engine->m_backgroundMusicChannel->setPaused(true);
		}
		if (engine->m_globalChannel) engine->m_globalChannel->setPitch(pitch);
		return;
	}
	if (g_liveOpeningHeld) {
		g_liveOpeningHeld = false;
		unsigned ms = static_cast<unsigned>(std::llround(std::max(g_liveOpeningSec, 0.0) * 1000.0));
		engine->setMusicTimeMS(ms, false, 0);
		if (engine->m_backgroundMusicChannel && play && !play->m_isPaused) {
			engine->m_backgroundMusicChannel->setPaused(false);
		}
		g_liveOpeningSec = 0.0;
	}

	if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setPitch(pitch);
	if (engine->m_globalChannel) engine->m_globalChannel->setPitch(pitch);
	Logic::get().syncAudioClock();
}

struct SfxMark {
	unsigned pos = 0;
	double level = 0.0;
};

void syncEffectGroup(FMOD::ChannelGroup* group, double level, std::unordered_map<FMOD::Channel*, SfxMark>& marks, std::unordered_set<FMOD::Channel*>& seen) {
	if (!group) return;
	int count = 0;
	group->getNumChannels(&count);
	for (int i = 0; i < count; ++i) {
		FMOD::Channel* channel = nullptr;
		if (group->getChannel(i, &channel) != FMOD_OK || !channel) continue;
		bool playing = false;
		if (channel->isPlaying(&playing) != FMOD_OK || !playing) continue;
		unsigned pos = 0;
		if (channel->getPosition(&pos, FMOD_TIMEUNIT_MS) != FMOD_OK) continue;
		seen.insert(channel);
		auto found = marks.find(channel);
		bool fresh = found == marks.end() || level + 0.0005 < found->second.level || pos + 80 < found->second.pos;
		if (fresh) {
			marks[channel] = {pos, level};
			continue;
		}
		double expectedD = static_cast<double>(found->second.pos) + (level - found->second.level) * 1000.0;
		if (expectedD < 0.0) expectedD = 0.0;
		auto expected = static_cast<unsigned>(std::llround(expectedD));
		int drift = static_cast<int>(pos) - static_cast<int>(expected);
		if (std::abs(drift) < 250) continue;
		if (channel->setPosition(expected, FMOD_TIMEUNIT_MS) == FMOD_OK) found->second.pos = expected;
	}
	int groups = 0;
	group->getNumGroups(&groups);
	for (int i = 0; i < groups; ++i) {
		FMOD::ChannelGroup* child = nullptr;
		if (group->getGroup(i, &child) == FMOD_OK) syncEffectGroup(child, level, marks, seen);
	}
}

void pauseGroup(FMOD::ChannelGroup* group) {
	if (!group) return;
	group->setPaused(true);
}

void releaseGroup(FMOD::ChannelGroup* group) {
	if (!group) return;
	group->setDelay(0, 0, false);
	group->setPaused(false);
}

void playGroupSlice(FMOD::ChannelGroup* group, unsigned long long samples) {
	if (!group || samples == 0) return;
	unsigned long long clock = 0;
	if (group->getDSPClock(nullptr, &clock) != FMOD_OK) return;
	unsigned long long start = 0;
	unsigned long long end = 0;
	bool stop = false;
	group->getDelay(&start, &end, &stop);
	unsigned long long from = end > clock ? end : clock;
	group->setPaused(false);
	group->setDelay(0, from + samples, false);
}

bool sliceStillRunning(FMOD::ChannelGroup* group) {
	if (!group) return false;
	unsigned long long clock = 0;
	unsigned long long start = 0;
	unsigned long long end = 0;
	bool stop = false;
	if (group->getDSPClock(nullptr, &clock) != FMOD_OK) return false;
	if (group->getDelay(&start, &end, &stop) != FMOD_OK) return false;
	return end > clock;
}

// Sync Audio keeps the song at its normal pitch and at the level's position. While the level runs
// near real speed the song plays on, with at most 1% faster or slower playback to pull in drift.
// When it runs slower (a heavy part, in Real Time or not), each frame plays exactly the stretch of
// song the level advanced and then stops on a scheduled DSP delay, as frame advance does. Slices
// follow on from each other, so the song needs no seek unless it jumps; seeking every frame
// stalled the game, and slowing the song changed its pitch.
namespace {
bool g_songSliced = false;

void releaseSong(FMODAudioEngine* engine) {
	if (!g_songSliced) return;
	g_songSliced = false;
	if (engine) releaseGroup(engine->m_backgroundMusicChannel);
}
}

void Logic::syncAudioClock() {
	auto& value = Logic::get();
	auto* engine = FMODAudioEngine::sharedEngine();
	if (!engine || !engine->m_system) return;
	if (!value.syncAudio || value.renderActive()) {
		releaseSong(engine);
		return;
	}

	GJBaseGameLayer* layer = nullptr;
	bool paused = false;
	bool started = false;
	bool dead = false;
	if (auto* play = PlayLayer::get()) {
		layer = play;
		paused = play->m_isPaused;
		started = play->m_started;
		dead = play->m_playerDied;
	} else if (value.editorTesting) {
		if (auto* editor = LevelEditorLayer::get()) {
			layer = editor;
			paused = value.editorPaused;
			started = true;
		}
	}
	if (!layer) {
		releaseSong(engine);
		return;
	}

	static bool anchored = false;
	static double anchorLevel = 0.0;
	static unsigned anchorMusic = 0;
	static double lastLevel = 0.0;
	static bool frameHeld = false;
	static std::unordered_map<FMOD::Channel*, SfxMark> marks;

	auto groups = [&](auto&& use) {
		use(engine->m_backgroundMusicChannel);
		use(engine->m_globalChannel);
		use(engine->m_reverbChannel);
	};
	if (!value.frameAdvance) {
		if (frameHeld) {
			groups([](FMOD::ChannelGroup* group) { releaseGroup(group); });
			frameHeld = false;
		}
	}

	double level = layer->m_gameState.m_levelTime;
	if (value.frameAdvance && started && !paused && !dead) {
		releaseSong(engine);
		frameHeld = true;
		double stepped = level - lastLevel;
		if (stepped <= 0.0005) {
			if (!sliceStillRunning(engine->m_backgroundMusicChannel)) groups([](FMOD::ChannelGroup* group) { pauseGroup(group); });
			return;
		}
		lastLevel = level;
		if (!anchored) {
			anchored = true;
			anchorLevel = level - stepped;
			anchorMusic = engine->getMusicTimeMS(0);
		}
		double startD = static_cast<double>(anchorMusic) + (level - stepped - anchorLevel) * 1000.0;
		if (startD < 0.0) startD = 0.0;
		engine->setMusicTimeMS(static_cast<unsigned>(std::llround(startD)), false, 0);
		int rate = 44100;
		engine->m_system->getSoftwareFormat(&rate, nullptr, nullptr);
		unsigned long long samples = std::max(1ull, static_cast<unsigned long long>(std::llround(stepped * static_cast<double>(std::max(rate, 1)))));
		groups([&](FMOD::ChannelGroup* group) { playGroupSlice(group, samples); });
		return;
	}

	if (!started || paused || dead || level + 0.0005 < lastLevel) {
		if (!started || dead || level + 0.0005 < lastLevel) {
			anchored = false;
			marks.clear();
		}
		if (!started || paused || dead) {
			// GD pauses and restarts the song itself outside the running level.
			if (g_songSliced) {
				g_songSliced = false;
				if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->setDelay(0, 0, false);
			}
			return;
		}
	}

	using Clock = std::chrono::steady_clock;
	static Clock::time_point lastWall;
	static double rate = 1.0;
	auto now = Clock::now();
	if (!anchored) {
		anchored = true;
		anchorLevel = level;
		anchorMusic = engine->getMusicTimeMS(0);
		lastLevel = level;
		lastWall = now;
		rate = std::max(static_cast<double>(value.speedhack), 0.01);
		return;
	}
	// How fast the level is advancing against the wall clock. Real Time behind a heavy part
	// advances the level at a fraction of real speed; the song follows at that rate instead of
	// being seeked back every frame, which stalled the game and made the music stutter.
	double wall = std::chrono::duration<double>(now - lastWall).count();
	lastWall = now;
	double stepped = std::max(level - lastLevel, 0.0);
	if (wall > 0.0005 && wall < 1.0) rate += (stepped / wall - rate) * std::min(wall * 4.0, 1.0);
	lastLevel = level;

	double expectedD = static_cast<double>(anchorMusic) + (level - anchorLevel) * 1000.0;
	if (expectedD < 0.0) expectedD = 0.0;
	float base = std::max(value.speedhack, 0.01f);
	auto* music = engine->m_backgroundMusicChannel;
	if (rate < base * 0.9 && music) {
		// Slow: this frame's slice starts where the level was at the last frame. A slice in
		// progress ends there on its own, so only a real jump is seeked.
		double startD = std::max(expectedD - stepped * 1000.0, 0.0);
		unsigned actual = engine->getMusicTimeMS(0);
		if (std::abs(static_cast<double>(actual) - startD) >= 400.0)
			engine->setMusicTimeMS(static_cast<unsigned>(std::llround(startD)), false, 0);
		music->setPitch(base);
		g_songSliced = true;
		if (stepped > 0.0) {
			int output = 44100;
			engine->m_system->getSoftwareFormat(&output, nullptr, nullptr);
			// Song seconds play at the speedhack pitch, so they take stepped / base of mixer time.
			auto samples = static_cast<unsigned long long>(std::llround(stepped / base * static_cast<double>(std::max(output, 1))));
			playGroupSlice(music, std::max(samples, 1ull));
		}
	} else {
		releaseSong(engine);
		auto expected = static_cast<unsigned>(std::llround(expectedD));
		unsigned actual = engine->getMusicTimeMS(0);
		int drift = static_cast<int>(actual) - static_cast<int>(expected);
		// Seek only for a real jump (restart, checkpoint, a long stall).
		if (std::abs(drift) >= 400) {
			engine->setMusicTimeMS(expected, false, 0);
			drift = 0;
		}
		double correction = std::clamp(-static_cast<double>(drift) / 1000.0 * 0.5, -0.01, 0.01);
		if (music) music->setPitch(static_cast<float>(base * (1.0 + correction)));
	}

	std::unordered_set<FMOD::Channel*> seen;
	syncEffectGroup(engine->m_globalChannel, level, marks, seen);
	syncEffectGroup(engine->m_reverbChannel, level, marks, seen);
	std::erase_if(marks, [&](auto const& item) { return !seen.contains(item.first); });
}

void Logic::resetOpeningMusicHold() {
	g_liveOpeningHeld = false;
	g_liveOpeningSec = 0.0;
}

std::string Logic::renderFileStem() const {
	if (!renderName.empty() && renderName != "output") return renderName;
	if (!macroName.empty()) return macroName;
	return "Replay";
}
