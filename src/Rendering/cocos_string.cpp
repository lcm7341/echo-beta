// CCString::createWithFormat allocates a 100kb buffer on every call & labels / frame names call it constantly. windows
// ts formats into 1kb on the stack & only goes to the original when the text doesnt fit
#include <Geode/Geode.hpp>
#include "circle_signatures.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace geode::prelude;

// rob mallocs 100kb every format xd
namespace {
    using Init = bool (*)(CCString*, char const*, va_list);
    Init native_init = nullptr;

    bool init_formatted(CCString* self, char const* format, va_list ap) {
        char buffer[1024];
        va_list copy;
        va_copy(copy, ap);
        int len = std::vsnprintf(buffer, sizeof(buffer), format, copy);
        va_end(copy);
        if (len < 0 || len >= static_cast<int>(sizeof(buffer)))
            return native_init(self, format, ap);
        self->m_sString.assign(buffer, std::strlen(buffer));
        return true;
    }
}

$execute {
    auto cocos = geode::base::getCocos();
    // gd code hash. no match = wrong version or another mod changed it
    if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x105f0), 0x89) != 0xd5e0fb5fb6f4739dull) { // CCString::initWithFormatAndValist
        return;
    }
    native_init = reinterpret_cast<Init>(cocos + 0x105e0);
    (void)Mod::get()->hook(reinterpret_cast<void*>(native_init), &init_formatted, "CCString::initWithFormatAndValist");
}
