// is a GJGameState safe to copy?
// a backstep save once had a broken map inside & gd crashed copying it
// broken() walks the containers in the state (vectors, the effect lists, object pointers) & checks each 1 looks sane:
//   sizes that make sense, pointers that land on a real object w/ a vtable inside the exe
// it returns what was wrong as text, empty = fine
#include "Logic/state_check.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/EnterEffectInstance.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/GameObject.hpp>
#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

#ifdef GEODE_IS_WINDOWS
#include "Logic/state_check_walk.hpp"

namespace {
    using echo::state_check::detail::list;
    using echo::state_check::detail::tree;

    template <class T>
    bool vector(std::vector<T> const& items) {
        auto const* raw = reinterpret_cast<std::uintptr_t const*>(&items);
        auto first = raw[0], last = raw[1], end = raw[2];
        if (!first)
            return !last && !end;
        return (first & 7) == 0 && first <= last && last <= end && (last - first) % sizeof(T) == 0;
    }

    std::string failure;

    bool read_words(std::uintptr_t address, std::uintptr_t (&out)[4]) {
        __try {
            for (int i = 0; i < 4; ++i)
                out[i] = reinterpret_cast<std::uintptr_t const*>(address)[i];
            return true;
        }
        __except (1) {
            return false;
        }
    }

    struct ObjectFields {
        int id, uid, type, slot, inner, outer, middle;
        float x, y;
    };

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

    template <class T>
    T field(std::uintptr_t object, std::size_t offset) {
        return *reinterpret_cast<T const*>(object + offset);
    }

    bool read_object(std::uintptr_t object, ObjectFields& out) {
        __try {
            auto vtable = field<std::uintptr_t>(object, 0);
            auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            if (vtable < base || vtable > base + 0x1000000)
                return false; // vtable has to be inside the exe
            out.id = field<int>(object, offsetof(GameObject, m_objectID));
            out.uid = field<int>(object, offsetof(GameObject, m_uniqueID));
            out.type = static_cast<int>(field<GameObjectType>(object, offsetof(GameObject, m_objectType)));
            out.slot = field<int>(object, offsetof(GameObject, m_someOtherIndex));
            out.inner = field<int>(object, offsetof(GameObject, m_innerSectionIndex));
            out.outer = field<int>(object, offsetof(GameObject, m_outerSectionIndex));
            out.middle = field<int>(object, offsetof(GameObject, m_middleSectionIndex));
            out.x = static_cast<float>(field<double>(object, offsetof(GameObject, m_positionX)));
            out.y = static_cast<float>(field<double>(object, offsetof(GameObject, m_positionY)));
            return true;
        }
        __except (1) {
            return false;
        }
    }

#pragma clang diagnostic pop

    std::string dump_region(std::uintptr_t address, std::size_t bytes) {
        std::string out;
        for (std::size_t at = 0; at < bytes; at += 8) {
            std::uintptr_t words[4]{};
            if (!read_words(address + at, words)) {
                out += " unreadable";
                break;
            }
            auto word = words[0];
            ObjectFields o{};
            if (echo::state_check::detail::plausible(word) && read_object(word, o))
                out += fmt::format(" +{:#x}:obj[id {} uid {} type {} pos {:.1f},{:.1f} sect {},{} slot {} inner {}]", at, o.id, o.uid, o.type, o.x, o.y, o.outer, o.middle, o.slot, o.inner);
            else
                out += fmt::format(" +{:#x}:{:#x}", at, word);
        }
        return out;
    }

    bool effects(std::vector<EnterEffectInstance> const& items) {
        if (!vector(items))
            return false;
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto const& item = items[i];
            if (tree(item.m_enterEffectAnimMap) && vector(item.m_easeIndices))
                continue;
            auto head = reinterpret_cast<std::uintptr_t>(item.m_enterEffectAnimMap.end()._Ptr);
            std::uintptr_t words[4]{};
            bool read = read_words(head, words);
            failure = fmt::format(" [instance {} of {} at {:#x}, target {} control {}, map size {} head {:#x}: {}]", i, items.size(),
                reinterpret_cast<std::uintptr_t>(&item), item.m_targetID, item.m_controlID, item.m_enterEffectAnimMap.size(), head,
                read ? fmt::format("{:#x} {:#x} {:#x} {:#x}", words[0], words[1], words[2], words[3]) : std::string{ "unreadable" });
            failure += " instance bytes:" + dump_region(reinterpret_cast<std::uintptr_t>(&item), sizeof(EnterEffectInstance));
            return false;
        }
        return true;
    }

    template <class Hash>
    bool effect_lists(Hash const& hash) {
        if (!list(hash))
            return false;
        for (auto const& [key, items] : hash) {
            if (!effects(items))
                return false;
        }
        return true;
    }
}

std::string echo::state_check::broken(GJGameState const& s) {
    failure.clear();
#define ECHO_CHECK(test, member) \
    if (!test(s.member))         \
        return #member + failure;
    ECHO_CHECK(list, m_spawnChannelRelated0)
    ECHO_CHECK(list, m_spawnChannelRelated1)
    ECHO_CHECK(list, m_tweenActions)
    ECHO_CHECK(list, m_gameObjectPhysics)
    ECHO_CHECK(tree, m_activatedObjectIDs)
    ECHO_CHECK(list, m_stateObjects)
    ECHO_CHECK(tree, m_unkMapPairGJGameEventIntVectorEventTriggerInstance)
    ECHO_CHECK(tree, m_unkMapPairGJGameEventIntInt)
    ECHO_CHECK(effect_lists, m_enterEffectInstanceVectors)
    ECHO_CHECK(effect_lists, m_exitEffectInstanceVectors)
    ECHO_CHECK(effects, m_moveEffectInstances)
    ECHO_CHECK(effects, m_rotateEffectInstances)
    ECHO_CHECK(effects, m_scaleEffectInstances)
    ECHO_CHECK(effects, m_fadeEffectInstances)
    ECHO_CHECK(effects, m_tintEffectInstances)
    ECHO_CHECK(list, m_unsortedAreaEffects)
    ECHO_CHECK(list, m_unkUMap8)
    ECHO_CHECK(tree, m_proximityVolumeRelated)
    ECHO_CHECK(list, m_songChannelStates)
    ECHO_CHECK(list, m_songTriggerStateVectors)
#undef ECHO_CHECK
    return {};
}

#else
std::string echo::state_check::broken(GJGameState const&) {
    return {};
}
#endif
