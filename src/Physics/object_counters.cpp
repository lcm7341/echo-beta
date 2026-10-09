// gds global counters: the next unique object id & the variance generator. windows
// neither is exported, so each address is read out of an instruction that uses it (resolve), after checking the bytes
//   around it are what 2.2081 has (known)
// the variance generator is reseeded every attempt so every object gets the same variance index each time
// createCheckpoint makes a checkpoint w/out using up ids or variance, otherwise a recording breaks later in the level
#include "object_counters.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cstring>
#include <initializer_list>
#include <utility>

using namespace geode::prelude;

namespace {
#ifdef GEODE_IS_WINDOWS
    template <class T>
    T* resolve(std::uintptr_t site, std::size_t operand, std::size_t length, std::initializer_list<std::uint8_t> prefix, std::uintptr_t expected) {
        auto const* code = reinterpret_cast<std::uint8_t const*>(geode::base::get() + site);
        std::size_t i = 0;
        for (auto byte : prefix) {
            if (code[i++] != byte)
                return nullptr;
        }
        std::int32_t rel = 0;
        std::memcpy(&rel, code + operand, 4);
        auto target = reinterpret_cast<std::uintptr_t>(code + length) + rel;
        if (target != geode::base::get() + expected)
            return nullptr;
        return reinterpret_cast<T*>(target);
    }
#endif

    constexpr std::uint64_t VARIANCE_SEED = 0;
}

namespace echo::objects {
    int* unique_id_counter() {
#ifdef GEODE_IS_WINDOWS
        static int* const counter = resolve<int>(0x18de5b, 2, 6, { 0x8b, 0x05 }, 0x6ba158);
        return counter;
#else
        return nullptr;
#endif
    }

    std::uint64_t* variance_generator() {
#ifdef GEODE_IS_WINDOWS
        static std::uint64_t* const generator = resolve<std::uint64_t>(0x18ddec, 3, 11, { 0x48, 0x69, 0x05 }, 0x6c2ee0);
        return generator;
#else
        return nullptr;
#endif
    }

    void reseed_variance() {
        if (auto* generator = variance_generator())
            *generator = VARIANCE_SEED;
    }
}

class $modify(CheckpointCounters, PlayLayer) {
    CheckpointObject* createCheckpoint() {
        auto* ids = echo::objects::unique_id_counter();
        auto* variance = echo::objects::variance_generator();
        int id = ids ? *ids : 0;
        std::uint64_t draw = variance ? *variance : 0;
        auto* checkpoint = PlayLayer::createCheckpoint();
        if (ids)
            *ids = id;
        if (variance)
            *variance = draw;
        return checkpoint;
    }
};
