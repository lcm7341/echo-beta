#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>

using namespace geode::prelude;

static FMOD_MODE as_sample_mode(FMOD_MODE mode) {
    return (mode & ~(FMOD_MODE)(FMOD_CREATESTREAM | FMOD_NONBLOCKING)) | FMOD_CREATESAMPLE;
}

static FMOD_RESULT hooked_create_stream(FMOD::System* self, const char* name, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO* info, FMOD::Sound** sound) {
    if (Logic::get().render_active()) {
        auto result = self->createSound(name, as_sample_mode(mode), info, sound);
        return result;
    }
    return self->createStream(name, mode, info, sound);
}

static FMOD_RESULT hooked_create_sound(FMOD::System* self, const char* name, FMOD_MODE mode, FMOD_CREATESOUNDEXINFO* info, FMOD::Sound** sound) {
    if (Logic::get().render_active()) {
        if (mode & FMOD_CREATESTREAM) mode = as_sample_mode(mode);
        else mode &= ~(FMOD_MODE)FMOD_NONBLOCKING;
    }
    return self->createSound(name, mode, info, sound);
}

$execute {
    (void)Mod::get()->hook(reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::createStream)), &hooked_create_stream,
        "FMOD::System::createStream", tulip::hook::TulipConvention::Stdcall);
    (void)Mod::get()->hook(reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::createSound)), &hooked_create_sound,
        "FMOD::System::createSound", tulip::hook::TulipConvention::Stdcall);
}
