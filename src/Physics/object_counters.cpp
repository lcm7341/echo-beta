#include "object_counters.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cstring>
#include <initializer_list>
#include <utility>

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
template <class T>
static T* resolve(std::uintptr_t site, std::size_t operand, std::size_t length, std::initializer_list<unsigned char> prefix, std::uintptr_t expected) {
    const auto* code = reinterpret_cast<const unsigned char*>(geode::base::get() + site);
    std::size_t i = 0;
    for (auto byte : prefix) {
        if (code[i++] != byte)
            return nullptr;
    }
    int rel = 0;
    std::memcpy(&rel, code + operand, 4);
    auto target = reinterpret_cast<std::uintptr_t>(code + length) + rel;
    if (target != geode::base::get() + expected)
        return nullptr;
    return reinterpret_cast<T*>(target);
}
#endif

#ifdef GEODE_IS_ANDROID64
template <class T>
static T* known(std::uintptr_t data, std::initializer_list<std::pair<std::uintptr_t, unsigned>> code) {
    auto base = geode::base::get();
    for (const auto& [offset, word] : code) {
        if (*reinterpret_cast<const unsigned*>(base + offset) != word)
            return nullptr;
    }
    return reinterpret_cast<T*>(base + data);
}
#endif
constexpr unsigned long long VARIANCE_SEED = 0;

int* unique_id_counter() {
#ifdef GEODE_IS_WINDOWS
    static int* const counter = resolve<int>(0x18de5b, 2, 6, { 0x8b, 0x05 }, 0x6ba158);
    return counter;
#elif defined(GEODE_IS_ANDROID64)
    static int* const counter = known<int>(0x122f018, { { 0x64225c, 0xb0005f62 }, { 0x642260, 0xb9401841 }, { 0x642270, 0xb9001843 } });
    return counter;
#else
    return nullptr;
#endif
}

unsigned long long* variance_generator() {
#ifdef GEODE_IS_WINDOWS
    static unsigned long long* const generator = resolve<unsigned long long>(0x18ddec, 3, 11, { 0x48, 0x69, 0x05 }, 0x6c2ee0);
    return generator;
#elif defined(GEODE_IS_ANDROID64)
    static unsigned long long* const generator = known<unsigned long long>(0x1242080,
        { { 0x64336c, 0xf0005fe3 }, { 0x643374, 0x9101a063 }, { 0x64338c, 0xf9400c62 }, { 0x6433cc, 0xf9000c62 } });
    return generator;
#else
    return nullptr;
#endif
}

void reseed_variance() {
    if (auto* generator = variance_generator())
        *generator = VARIANCE_SEED;
}

class $modify(CheckpointCounters, PlayLayer) {
    CheckpointObject* createCheckpoint() {
        auto* ids = unique_id_counter();
        auto* variance = variance_generator();
        int id = ids ? *ids : 0;
        unsigned long long draw = variance ? *variance : 0;
        auto* checkpoint = PlayLayer::createCheckpoint();
        if (ids)
            *ids = id;
        if (variance)
            *variance = draw;
        return checkpoint;
    }
};
