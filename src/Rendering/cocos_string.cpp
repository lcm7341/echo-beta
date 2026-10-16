#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <cstdarg>
#include <cstdio>
#include <cstring>

using namespace geode::prelude;

// rob mallocs 100kb every format xd
using Init = bool (*)(CCString*, const char*, va_list);
static Init native_init = nullptr;

static bool init_formatted(CCString* self, const char* format, va_list ap) {
    char buffer[1024];
    va_list copy;
    va_copy(copy, ap);
    int len = std::vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
    if (len < 0 || len >= (int)sizeof(buffer))
        return native_init(self, format, ap);
    self->m_sString.assign(buffer, std::strlen(buffer));
    return true;
}

$execute {
    auto cocos = geode::base::getCocos();
    native_init = reinterpret_cast<Init>(cocos + 0x105e0);
    (void)Mod::get()->hook(reinterpret_cast<void*>(native_init), &init_formatted, "CCString::initWithFormatAndValist");
}
#endif
