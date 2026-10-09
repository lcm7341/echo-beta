#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>

using namespace geode::prelude;

// GD opens songs as FMOD streams, decoded a chunk ahead (about 0.68 s) on FMOD's stream thread at
// real-time pace. A render mixes faster than real time (more so with SSB Fix, which leaves less
// physics per frame), so the stream ran out and replayed its last chunk until the thread caught up:
// the song stopped, repeated a spot, then jumped. While rendering, a song is opened as a sample
// instead, decoded whole into memory, so the offline mixer can take it as fast as it likes.
namespace {
// GD opens songs non-blocking. A stream is ready almost at once, but a sample loads the whole song
// first, and GD left the song paused for good when it was not ready in time. Load it before
// returning instead; it is one decode when the render starts.
FMOD_MODE asSample(FMOD_MODE mode) {
	return (mode & ~static_cast<FMOD_MODE>(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
}

FMOD_RESULT echoCreateStream(FMOD::System* self, char const* name, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO* info, FMOD::Sound** sound) {
	if (Logic::get().renderActive()) {
		auto result = self->createSound(name, asSample(mode), info, sound);
		if (result != FMOD_OK) log::warn("Echo render song: {} could not open as a sample ({})", name ? name : "?", static_cast<int>(result));
		return result;
	}
	return self->createStream(name, mode, info, sound);
}

FMOD_RESULT echoCreateSound(FMOD::System* self, char const* name, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO* info, FMOD::Sound** sound) {
	if ((mode & FMOD_CREATESTREAM) && Logic::get().renderActive()) mode = asSample(mode);
	return self->createSound(name, mode, info, sound);
}
}

$execute {
	(void)Mod::get()->hook(reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::createStream)), &echoCreateStream,
		"FMOD::System::createStream", tulip::hook::TulipConvention::Stdcall);
	(void)Mod::get()->hook(reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::createSound)), &echoCreateSound,
		"FMOD::System::createSound", tulip::hook::TulipConvention::Stdcall);
}
