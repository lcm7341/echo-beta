// where moving objects were, for backstep saves & respawns. gd restores neither properly
#include "Logic/logic.hpp"
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/GroupCommandObject2.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>

using namespace geode::prelude;

namespace {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

    struct Span {
        std::uint16_t begin;
        std::uint16_t size;
        bool moves;
    };

#define ECHO_SPAN(first, last, moves)                                                                                        \
    Span {                                                                                                                   \
        static_cast<std::uint16_t>(offsetof(GameObject, first)),                                                             \
            static_cast<std::uint16_t>(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)), \
            moves                                                                                                            \
    }
    constexpr std::array SPANS = {
        ECHO_SPAN(m_positionXOffset, m_scaleYOffset, true),
        ECHO_SPAN(m_unmodifiedPositionX, m_positionY, true),
        ECHO_SPAN(m_startRotationX, m_customScaleY, false),
        ECHO_SPAN(m_scaleX, m_scaleY, false),
        ECHO_SPAN(m_lastPosition, m_unk4CC, false),
        ECHO_SPAN(m_isDirty, m_isOrientedBoxDirty, false),
    };
#undef ECHO_SPAN
#pragma clang diagnostic pop
    static_assert(SPANS.size() <= 8, "a record's span mask is one byte");

    constexpr std::size_t STRIDE = [] {
        std::size_t total = 0;
        for (auto const& span : SPANS)
            total += span.size;
        return total;
    }();

    void gather(GameObject const* object, std::uint8_t* out) {
        auto const* raw = reinterpret_cast<std::uint8_t const*>(object);
        for (auto const& span : SPANS) {
            std::memcpy(out, raw + span.begin, span.size);
            out += span.size;
        }
    }

    std::uint8_t put(GJBaseGameLayer* play, GameObject* object, std::uint8_t const* record) {
        auto* raw = reinterpret_cast<std::uint8_t*>(object);
        std::uint8_t written = 0;
        bool moved = false;
        for (std::size_t i = 0; i < SPANS.size(); ++i) {
            auto const& span = SPANS[i];
            if (std::memcmp(raw + span.begin, record, span.size) != 0) {
                std::memcpy(raw + span.begin, record, span.size);
                written |= static_cast<std::uint8_t>(1u << i);
                moved = moved || span.moves;
            }
            record += span.size;
        }
        if (!written)
            return 0;
        object->m_isOrientedBoxDirty = true;
        if (moved)
            play->updateObjectSection(object);
        return written;
    }

    std::vector<GameObject*> grouped_objects(PlayLayer* play) {
        std::vector<GameObject*> out;
        if (!play->m_objects)
            return out;
        for (unsigned i = 0; i < play->m_objects->count(); ++i) {
            auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
            if (obj && obj->m_groupCount > 0)
                out.push_back(obj);
        }
        return out;
    }

    std::shared_ptr<ObjectBase const> shared_base(PlayLayer* play) {
        static std::weak_ptr<ObjectBase const> shared;
        unsigned amt = play->m_objects->count();
        if (auto base = shared.lock(); base && base->objects.size() == amt) {
            bool same = true;
            for (unsigned i = 0; i < amt && same; ++i)
                same = play->m_objects->objectAtIndex(i) == base->objects[i];
            if (same)
                return base;
        }
        auto base = std::make_shared<ObjectBase>();
        base->objects.reserve(amt);
        base->bytes.resize(static_cast<std::size_t>(amt) * STRIDE);
        for (unsigned i = 0; i < amt; ++i) {
            auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
            base->objects.push_back(obj);
            if (obj)
                gather(obj, base->bytes.data() + static_cast<std::size_t>(i) * STRIDE);
        }
        shared = base;
        return base;
    }

    // a full copy was 31mb per checkpoint on big levels :/
    // so 1 full copy (base) & each checkpoint only keeps what is different
    std::shared_ptr<ObjectStates> capture(PlayLayer* play, int frame) {
        auto states = std::make_shared<ObjectStates>();
        states->frame = frame;
        if (!play->m_objects)
            return states;
        states->base = shared_base(play);
        auto const& base = *states->base;
        auto& diff = states->diff;
        std::array<std::uint8_t, STRIDE> cur;
        for (unsigned i = 0; i < base.objects.size(); ++i) {
            auto* obj = base.objects[i];
            if (!obj)
                continue;
            auto const* slot = base.bytes.data() + static_cast<std::size_t>(i) * STRIDE;
            gather(obj, cur.data());
            if (std::memcmp(slot, cur.data(), STRIDE) == 0)
                continue;
            std::uint32_t idx = i;
            std::size_t at = diff.size();
            diff.resize(at + sizeof(idx) + 1);
            std::memcpy(diff.data() + at, &idx, sizeof(idx));
            std::uint8_t mask = 0;
            std::size_t offset = 0;
            for (std::size_t s = 0; s < SPANS.size(); ++s) {
                auto size = SPANS[s].size;
                if (std::memcmp(slot + offset, cur.data() + offset, size) != 0) {
                    mask |= static_cast<std::uint8_t>(1u << s);
                    diff.insert(diff.end(), cur.data() + offset, cur.data() + offset + size);
                }
                offset += size;
            }
            diff[at + sizeof(idx)] = mask;
        }
        diff.shrink_to_fit();
        return states;
    }

    void restore(PlayLayer* play, ObjectStates const& states) {
        if (!states.base || !play->m_objects || play->m_objects->count() != states.base->objects.size()) {
            return;
        }
        auto const& base = *states.base;
        auto const* in = states.diff.data();
        auto const* end = in + states.diff.size();
        std::array<std::uint8_t, STRIDE> cur;
        for (unsigned i = 0; i < base.objects.size(); ++i) {
            auto const* record = base.bytes.data() + static_cast<std::size_t>(i) * STRIDE;
            std::uint32_t idx = 0;
            if (in < end)
                std::memcpy(&idx, in, sizeof(idx));
            if (in < end && idx == i) {
                in += sizeof(idx);
                std::uint8_t mask = *in++;
                std::memcpy(cur.data(), record, STRIDE);
                auto* slot = cur.data();
                for (std::size_t s = 0; s < SPANS.size(); ++s) {
                    if (mask & (1u << s)) {
                        std::memcpy(slot, in, SPANS[s].size);
                        in += SPANS[s].size;
                    }
                    slot += SPANS[s].size;
                }
                record = cur.data();
            }
            auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
            if (obj != base.objects[i]) {
                return;
            }
            if (obj)
                put(play, obj, record);
        }
    }

    struct Timeline {
        PlayLayer* play = nullptr;
        CCArray* array = nullptr;
        std::vector<GameObject*> objects;
        std::vector<std::uint8_t> baseline;
        std::vector<std::uint8_t> scratch;
        int pending_frame = -1;
        std::vector<std::uint32_t> by_uid;
        std::vector<std::uint32_t> touched_stamp, touched;
        std::uint32_t epoch = 1;
        bool full = true, always_full = false;
        int saves = 0;
    };

    Timeline& timeline() {
        static Timeline value;
        return value;
    }

    void touch_object(GameObject* object) {
        auto& line = timeline();
        if (!object || line.by_uid.empty() || Logic::get().predicting)
            return;
        auto uid = static_cast<std::size_t>(object->m_uniqueID);
        if (uid >= line.by_uid.size())
            return;
        auto at = line.by_uid[uid];
        if (!at || line.objects[at - 1] != object)
            return;
        auto i = at - 1;
        if (line.touched_stamp[i] == line.epoch)
            return;
        line.touched_stamp[i] = line.epoch;
        line.touched.push_back(i);
    }

    bool untracked_motion(PlayLayer* play) {
        auto const& game = play->m_gameState;
        if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty())
            return true;
        auto* manager = play->m_effectManager;
        if (!manager)
            return false;
        for (auto const& command : manager->m_unkVector560) {
            auto* trigger = command.m_gameObject;
            int id = trigger ? trigger->m_objectID : 0;
            if (id != 901 && id != 1346)
                return true;
        }
        return false;
    }

    bool lvl_has_untracked_movers(PlayLayer* play) {
        if (!play->m_objects)
            return false;
        for (unsigned i = 0; i < play->m_objects->count(); ++i) {
            auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
            if (!obj)
                continue;
            switch (obj->m_objectID) {
            case 3016:
            case 3660:
            case 3661:
            case 3032:
            case 3033:
                return true;
            default:
                break;
            }
        }
        return false;
    }

    void rewind(Timeline& line, std::vector<std::uint8_t> const& undo) {
        auto const* in = undo.data();
        auto const* end = in + undo.size();
        while (in < end) {
            std::uint32_t idx = 0;
            std::memcpy(&idx, in, sizeof(idx));
            in += sizeof(idx);
            std::uint8_t mask = *in++;
            auto* slot = line.baseline.data() + static_cast<std::size_t>(idx) * STRIDE;
            for (std::size_t s = 0; s < SPANS.size(); ++s) {
                if (mask & (1u << s)) {
                    std::memcpy(slot, in, SPANS[s].size);
                    in += SPANS[s].size;
                }
                slot += SPANS[s].size;
            }
        }
    }
}

void Logic::step_object_states(PlayLayer* play) {
    // TODO
}

void Logic::note_backstep_objects(PlayLayer* play) {
    // TODO
}

void Logic::pop_backstep() {
    // TODO
}

void Logic::drop_backstep(std::size_t index) {
    // TODO
}

void Logic::clear_backstep_objects() {
    auto& line = timeline();
    line = Timeline{};
}

void Logic::load_backstep_objects(int frame) {
    // TODO
}
