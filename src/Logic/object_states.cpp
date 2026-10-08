// where moving objects were, for backstep saves & respawns. gd restores neither properly
//
// a full copy per save was 31mb on big levels, so there is 1 full copy (the base) & each save stores only what differs from it
// only objects that belong to a group are tracked, nothing else can move
// every 32nd save compares EVERYTHING against the base in case a change slipped past
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
    auto& logic = Logic::get();
    if (!play || logic.predicting || logic.backstepping)
        return;
    int frame = logic.step_frame;
    auto& line = timeline();
    if (!line.by_uid.empty() && !line.full && untracked_motion(play)) {
        line.full = true;
    }
    if (line.pending_frame >= 0) {
        int saved = std::exchange(line.pending_frame, -1);
        if (saved == frame && line.play == play && line.array == play->m_objects) {
            for (std::size_t i = 0; i < line.objects.size(); ++i)
                put(play, line.objects[i], line.baseline.data() + i * STRIDE);
        }
    }
    if (auto* obj = std::exchange(logic.object_restore, nullptr)) {
        logic.object_capture = nullptr;
        auto it = std::ranges::find(logic.checkpoints, obj, &Checkpoint::object);
        if (it == logic.checkpoints.end() || !it->objects)
            return;
        if (it->objects->frame != frame) {
            return;
        }
        restore(play, *it->objects);
        return;
    }
    if (auto* obj = std::exchange(logic.object_capture, nullptr)) {
        auto it = std::ranges::find(logic.checkpoints, obj, &Checkpoint::object);
        if (it == logic.checkpoints.end())
            return;
        if (it->frame != frame)
            return;
        it->objects = capture(play, frame);
    }
}

void Logic::note_backstep_objects(PlayLayer* play) {
    log::debug("note backstep objects");
    auto& logic = Logic::get();
    auto& line = timeline();
    if (line.play != play || line.array != play->m_objects) {
        line.play = play;
        line.array = play->m_objects;
        line.objects = grouped_objects(play);
        line.baseline.resize(line.objects.size() * STRIDE);
        for (std::size_t i = 0; i < line.objects.size(); ++i)
            gather(line.objects[i], line.baseline.data() + i * STRIDE);
        int max_uid = 0;
        for (auto* obj : line.objects)
            max_uid = std::max(max_uid, obj->m_uniqueID);
        line.by_uid.assign(static_cast<std::size_t>(std::max(max_uid, 0)) + 1, 0);
        for (std::size_t i = 0; i < line.objects.size(); ++i) {
            auto uid = line.objects[i]->m_uniqueID;
            if (uid >= 0) {
                line.by_uid[static_cast<std::size_t>(uid)] = static_cast<std::uint32_t>(i + 1);
            }
        }
        line.touched_stamp.assign(line.objects.size(), 0);
        line.touched.clear();
        ++line.epoch;
        line.always_full = lvl_has_untracked_movers(play);
        line.full = line.always_full || untracked_motion(play);
        line.saves = 0;
        while (logic.backsteps.size() > 1) {
            release_backstep(logic.backsteps.front(), "new timeline");
            logic.backsteps.pop_front();
        }
        return;
    }
    if (logic.backsteps.size() < 2)
        return;
    auto& undo = logic.backsteps[logic.backsteps.size() - 2].object_undo;
    undo.clear();
    line.scratch.resize(STRIDE);
    bool verify = false;
    bool everything = line.full || line.always_full || verify;
    int missed = 0;
    std::string missed_sample;
    auto visit = [&](std::size_t i) {
        auto* slot = line.baseline.data() + i * STRIDE;
        gather(line.objects[i], line.scratch.data());
        if (std::memcmp(slot, line.scratch.data(), STRIDE) == 0)
            return;
        std::uint32_t idx = static_cast<std::uint32_t>(i);
        std::size_t at = undo.size();
        undo.resize(at + sizeof(idx) + 1);
        std::memcpy(undo.data() + at, &idx, sizeof(idx));
        std::uint8_t mask = 0;
        std::size_t offset = 0;
        for (std::size_t s = 0; s < SPANS.size(); ++s) {
            auto size = SPANS[s].size;
            if (std::memcmp(slot + offset, line.scratch.data() + offset, size) != 0) {
                mask |= static_cast<std::uint8_t>(1u << s);
                undo.insert(undo.end(), slot + offset, slot + offset + size);
            }
            offset += size;
        }
        undo[at + sizeof(idx)] = mask;
        std::memcpy(slot, line.scratch.data(), STRIDE);
        if (verify && (mask & 0x1f) && line.touched_stamp[i] != line.epoch) {
            if (missed++ < 4) {
                missed_sample += fmt::format(" id {} uid {} spans {:#x}", line.objects[i]->m_objectID, line.objects[i]->m_uniqueID, mask);
            }
        }
    };
    if (everything) {
        for (std::size_t i = 0; i < line.objects.size(); ++i)
            visit(i);
    }
    else {
        for (auto i : line.touched)
            visit(i);
    }
    if (missed) {
        line.always_full = true;
    }
    line.touched.clear();
    ++line.epoch;
    line.full = line.always_full || untracked_motion(play);
    undo.shrink_to_fit();
}

void Logic::pop_backstep() {
    auto& logic = Logic::get();
    if (logic.backsteps.empty())
        return;
    release_backstep(logic.backsteps.back(), "pop");
    logic.backsteps.pop_back();
    if (logic.backsteps.empty()) {
        clear_backstep_objects();
        return;
    }
    auto& line = timeline();
    line.full = true;
    auto& undo = logic.backsteps.back().object_undo;
    if (!line.baseline.empty()) {
        rewind(line, undo);
    }
    undo.clear();
    undo.shrink_to_fit();
}

void Logic::drop_backstep(std::size_t index) {
    auto& logic = Logic::get();
    if (index == 0 || index + 1 >= logic.backsteps.size())
        return;
    auto& older = logic.backsteps[index - 1].object_undo;
    auto const& newer = logic.backsteps[index].object_undo;
    if (!newer.empty()) {
        std::vector<std::uint8_t> masks;
        auto const* in = older.data();
        auto const* end = in + older.size();
        std::vector<std::pair<std::uint32_t, std::uint8_t>> have;
        while (in < end) {
            std::uint32_t obj = 0;
            std::memcpy(&obj, in, sizeof(obj));
            in += sizeof(obj);
            std::uint8_t mask = *in++;
            have.emplace_back(obj, mask);
            for (std::size_t s = 0; s < SPANS.size(); ++s)
                if (mask & (1u << s)) {
                    in += SPANS[s].size;
                }
        }
        std::ranges::sort(have);
        in = newer.data();
        end = in + newer.size();
        while (in < end) {
            std::uint32_t obj = 0;
            std::memcpy(&obj, in, sizeof(obj));
            in += sizeof(obj);
            std::uint8_t mask = *in++;
            auto it = std::ranges::lower_bound(have, std::pair{ obj, std::uint8_t{ 0 } });
            std::uint8_t taken = it != have.end() && it->first == obj ? it->second : 0;
            std::uint8_t add = mask & static_cast<std::uint8_t>(~taken);
            if (add) {
                std::size_t at = older.size();
                older.resize(at + sizeof(obj) + 1);
                std::memcpy(older.data() + at, &obj, sizeof(obj));
                older[at + sizeof(obj)] = add;
            }
            for (std::size_t s = 0; s < SPANS.size(); ++s) {
                if (!(mask & (1u << s))) {
                    continue;
                }
                if (add & (1u << s)) {
                    older.insert(older.end(), in, in + SPANS[s].size);
                }
                in += SPANS[s].size;
            }
        }
        older.shrink_to_fit();
    }
    release_backstep(logic.backsteps[index], "thin");
    logic.backsteps.erase(logic.backsteps.begin() + static_cast<std::ptrdiff_t>(index));
}

void Logic::clear_backstep_objects() {
    auto& line = timeline();
    line = Timeline{};
}

void Logic::load_backstep_objects(int frame) {
    log::debug("load backstep objects");
    auto& line = timeline();
    if (!line.baseline.empty()) {
        line.pending_frame = frame;
    }
    line.full = true;
}

class $modify(BackstepTouched, GJBaseGameLayer) {
    static void touch_all(CCArray* objects) {
        if (!objects || timeline().by_uid.empty() || Logic::get().predicting)
            return;
        for (unsigned n = 0; n < objects->count(); ++n) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
            if (!obj)
                break; // rob stops at the first null
            touch_object(obj);
        }
    }

    void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
        touch_all(objects);
        GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
    }

    void rotateObjects(CCArray* objects, float rotation, CCPoint position, CCPoint offset, bool finished, bool unused) {
        touch_all(objects);
        GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
    }

    void updateObjectSection(GameObject* object) {
        touch_object(object);
        GJBaseGameLayer::updateObjectSection(object);
    }
};
