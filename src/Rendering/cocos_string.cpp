#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include "circle_signatures.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace geode::prelude;

// CCString::initWithFormatAndValist mallocs a 100 KB buffer, formats into it, keeps strlen of the
// result and frees it. Under Wine a block that size goes straight to virtual-memory syscalls, and
// a spammed dash orb formats a frame name in every startDashing: two syscalls per click. A result
// that fits a small stack buffer is formatted by the same CRT and stored the same way; anything
// longer takes the original.
namespace {
using Init = bool (*)(CCString*, char const*, va_list);
Init nativeInit = nullptr;

bool initFormatted(CCString* self, char const* format, va_list ap) {
    char buffer[1024];
    va_list copy;
    va_copy(copy, ap);
    int length = std::vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
    if (length < 0 || length >= static_cast<int>(sizeof(buffer))) return nativeInit(self, format, ap);
    self->m_sString.assign(buffer, std::strlen(buffer));
    return true;
}
}

$execute {
    auto cocos = geode::base::getCocos();
    // The body past the prologue a hook rewrites.
    if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x105f0), 0x89) != 0xd5e0fb5fb6f4739dull) {
        log::warn("Echo CCString format buffer skipped: native code differs");
        return;
    }
    nativeInit = reinterpret_cast<Init>(cocos + 0x105e0);
    if (!Mod::get()->hook(reinterpret_cast<void*>(nativeInit), &initFormatted, "CCString::initWithFormatAndValist"))
        log::warn("Echo CCString format buffer hook failed");
}
#endif
