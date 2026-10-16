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
#include <thread>

using namespace geode::prelude;

static Hook* g_touch_hook = nullptr;
static bool g_touch_tracking = true;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

struct Span {
    unsigned short begin;
    unsigned short size;
    bool moves;
};

#define ECHO_SPAN(first, last, moves)                                                                                        \
Span {                                                                                                                          \
    (unsigned short)offsetof(GameObject, first),                                                             \
        (unsigned short)(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)), \
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
    for (const auto& span : SPANS)
        total += span.size;
    return total;
}();

static void gather(const GameObject* object, unsigned char* out) {
    const auto* raw = reinterpret_cast<const unsigned char*>(object);
    for (const auto& span : SPANS) {
        std::memcpy(out, raw + span.begin, span.size);
        out += span.size;
    }
}

static unsigned char put(GJBaseGameLayer* play, GameObject* object, const unsigned char* record) {
    auto* raw = reinterpret_cast<unsigned char*>(object);
    unsigned char written = 0;
    bool moved = false;
    for (std::size_t i = 0; i < SPANS.size(); ++i) {
        const auto& span = SPANS[i];
        if (std::memcmp(raw + span.begin, record, span.size) != 0) {
            std::memcpy(raw + span.begin, record, span.size);
            written |= (unsigned char)(1u << i);
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

static std::vector<GameObject*> grouped_objects(PlayLayer* play) {
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

static std::shared_ptr<ObjectBase const> shared_base(PlayLayer* play) {
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
    base->bytes.resize((std::size_t)amt * STRIDE);
    for (unsigned i = 0; i < amt; ++i) {
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        base->objects.push_back(obj);
        if (obj)
            gather(obj, base->bytes.data() + (std::size_t)i * STRIDE);
    }
    shared = base;
    return base;
}

// a full copy was 31mb per checkpoint on big levels :/
// so 1 full copy (base) & each checkpoint only keeps what is different
static std::shared_ptr<ObjectStates> capture(PlayLayer* play, int frame) {
    auto states = std::make_shared<ObjectStates>();
    states->frame = frame;
    if (!play->m_objects)
        return states;
    states->base = shared_base(play);
    const auto& base = *states->base;
    auto& diff = states->diff;
    std::array<unsigned char, STRIDE> cur;
    for (unsigned i = 0; i < base.objects.size(); ++i) {
        auto* obj = base.objects[i];
        if (!obj)
            continue;
        const auto* slot = base.bytes.data() + (std::size_t)i * STRIDE;
        gather(obj, cur.data());
        if (std::memcmp(slot, cur.data(), STRIDE) == 0)
            continue;
        unsigned idx = i;
        std::size_t at = diff.size();
        diff.resize(at + sizeof(idx) + 1);
        std::memcpy(diff.data() + at, &idx, sizeof(idx));
        unsigned char mask = 0;
        std::size_t offset = 0;
        for (std::size_t s = 0; s < SPANS.size(); ++s) {
            auto size = SPANS[s].size;
            if (std::memcmp(slot + offset, cur.data() + offset, size) != 0) {
                mask |= (unsigned char)(1u << s);
                diff.insert(diff.end(), cur.data() + offset, cur.data() + offset + size);
            }
            offset += size;
        }
        diff[at + sizeof(idx)] = mask;
    }
    diff.shrink_to_fit();
    return states;
}

static void restore(PlayLayer* play, const ObjectStates& states) {
    if (!states.base || !play->m_objects || play->m_objects->count() != states.base->objects.size()) {
        return;
    }
    const auto& base = *states.base;
    const auto* in = states.diff.data();
    const auto* end = in + states.diff.size();
    std::array<unsigned char, STRIDE> cur;
    for (unsigned i = 0; i < base.objects.size(); ++i) {
        const auto* record = base.bytes.data() + (std::size_t)i * STRIDE;
        unsigned idx = 0;
        if (in < end)
            std::memcpy(&idx, in, sizeof(idx));
        if (in < end && idx == i) {
            in += sizeof(idx);
            unsigned char mask = *in++;
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
    std::vector<unsigned char> baseline;
    std::vector<unsigned char> scratch;
    int pending_frame = -1;
    std::vector<unsigned> by_uid;
    std::vector<unsigned> touched_stamp, touched;
    unsigned epoch = 1;
    bool full = true, always_full = false;
    int saves = 0;
};

static Timeline& timeline() {
    static Timeline value;
    return value;
}

static void touch_object(GameObject* object) {
    auto& line = timeline();
    if (!object || line.by_uid.empty() || Logic::get().predicting)
        return;
    auto uid = (std::size_t)object->m_uniqueID;
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

static bool untracked_motion(PlayLayer* play) {
    const auto& game = play->m_gameState;
    if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty())
        return true;
    auto* manager = play->m_effectManager;
    if (!manager)
        return false;
    for (const auto& command : manager->m_unkVector560) {
        auto* trigger = command.m_gameObject;
        int id = trigger ? trigger->m_objectID : 0;
        if (id != 901 && id != 1346)
            return true;
    }
    return false;
}

static bool lvl_has_untracked_movers(PlayLayer* play) {
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

static void rewind(Timeline& line, const std::vector<unsigned char>& undo) {
    const auto* in = undo.data();
    const auto* end = in + undo.size();
    while (in < end) {
        unsigned idx = 0;
        std::memcpy(&idx, in, sizeof(idx));
        in += sizeof(idx);
        unsigned char mask = *in++;
        auto* slot = line.baseline.data() + (std::size_t)idx * STRIDE;
        for (std::size_t s = 0; s < SPANS.size(); ++s) {
            if (mask & (1u << s)) {
                std::memcpy(slot, in, SPANS[s].size);
                in += SPANS[s].size;
            }
            slot += SPANS[s].size;
        }
    }
}

void Logic::step_object_states(PlayLayer* play) {
    if (!play || predicting || backstepping)
        return;
    int frame = step_frame;
    auto& line = timeline();
    if (!line.by_uid.empty() && !line.full && untracked_motion(play))
        line.full = true;
    if (line.pending_frame >= 0) {
        int saved = std::exchange(line.pending_frame, -1);
        if (saved == frame && line.play == play && line.array == play->m_objects) {
            for (std::size_t i = 0; i < line.objects.size(); ++i)
                put(play, line.objects[i], line.baseline.data() + i * STRIDE);
        }
    }
    if (auto* obj = std::exchange(object_restore, nullptr)) {
        object_capture = nullptr;
        auto it = std::ranges::find(checkpoints, obj, &Checkpoint::object);
        if (it == checkpoints.end() || !it->objects)
            return;
        if (it->objects->frame != frame) {
            return;
        }
        restore(play, *it->objects);
        return;
    }
    if (auto* obj = std::exchange(object_capture, nullptr)) {
        auto it = std::ranges::find(checkpoints, obj, &Checkpoint::object);
        if (it == checkpoints.end())
            return;
        if (it->frame != frame)
            return;
        it->objects = capture(play, frame);
    }
}

void Logic::note_backstep_objects(PlayLayer* play) {
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
        line.by_uid.assign((std::size_t)std::max(max_uid, 0) + 1, 0);
        for (std::size_t i = 0; i < line.objects.size(); ++i) {
            auto uid = line.objects[i]->m_uniqueID;
            if (uid >= 0)
                line.by_uid[(std::size_t)uid] = (unsigned)(i + 1);
        }
        line.touched_stamp.assign(line.objects.size(), 0);
        line.touched.clear();
        ++line.epoch;
        line.always_full = lvl_has_untracked_movers(play);
        line.full = line.always_full || untracked_motion(play);
        line.saves = 0;
        while (backsteps.size() > 1) {
            release_backstep(backsteps.front());
            backsteps.pop_front();
        }
        return;
    }
    if (backsteps.size() < 2)
        return;
    auto& undo = backsteps[backsteps.size() - 2].object_undo;
    undo.clear();
    bool verify = !line.full && !line.always_full && (++line.saves % 32) == 0;
    bool everything = line.full || line.always_full || verify || !g_touch_tracking;
    auto diff_object_into_undo = [&](std::size_t i, std::vector<unsigned char>& out, unsigned char* scratch, int& missed) {
        auto* slot = line.baseline.data() + i * STRIDE;
        const auto* raw = reinterpret_cast<const unsigned char*>(line.objects[i]);
        bool same = true;
        std::size_t offset = 0;
        for (const auto& span : SPANS) {
            if (std::memcmp(raw + span.begin, slot + offset, span.size) != 0) {
                same = false;
                break;
            }
            offset += span.size;
        }
        if (same)
            return;
        gather(line.objects[i], scratch);
        unsigned idx = (unsigned)i;
        std::size_t at = out.size();
        out.resize(at + sizeof(idx) + 1);
        std::memcpy(out.data() + at, &idx, sizeof(idx));
        unsigned char mask = 0;
        offset = 0;
        for (std::size_t s = 0; s < SPANS.size(); ++s) {
            auto size = SPANS[s].size;
            if (std::memcmp(slot + offset, scratch + offset, size) != 0) {
                mask |= (unsigned char)(1u << s);
                out.insert(out.end(), slot + offset, slot + offset + size);
            }
            offset += size;
        }
        out[at + sizeof(idx)] = mask;
        std::memcpy(slot, scratch, STRIDE);
        if (verify && (mask & 31) && line.touched_stamp[i] != line.epoch)
            ++missed;
    };
    int missed = 0;
    std::array<unsigned char, STRIDE> scratch;
    std::size_t total = line.objects.size();
    unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    if (everything && total >= 16384 && workers > 1) {
        std::size_t per = (total + workers - 1) / workers;
        std::vector<std::vector<unsigned char>> worker_undo(workers);
        std::vector<int> worker_misses(workers, 0);
        std::vector<std::thread> threads;
        for (unsigned w = 1; w < workers; ++w) {
            threads.emplace_back([&, w] {
                std::array<unsigned char, STRIDE> local;
                for (std::size_t i = w * per; i < std::min(total, (w + 1) * per); ++i)
                    diff_object_into_undo(i, worker_undo[w], local.data(), worker_misses[w]);
            });
        }
        for (std::size_t i = 0; i < std::min(total, per); ++i)
            diff_object_into_undo(i, undo, scratch.data(), worker_misses[0]);
        for (auto& thread : threads)
            thread.join();
        for (unsigned w = 0; w < workers; ++w) {
            missed += worker_misses[w];
            if (w > 0)
                undo.insert(undo.end(), worker_undo[w].begin(), worker_undo[w].end());
        }
    }
    else if (everything) {
        for (std::size_t i = 0; i < total; ++i)
            diff_object_into_undo(i, undo, scratch.data(), missed);
    }
    else {
        for (auto i : line.touched)
            diff_object_into_undo(i, undo, scratch.data(), missed);
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
    if (backsteps.empty())
        return;
    release_backstep(backsteps.back());
    backsteps.pop_back();
    if (backsteps.empty()) {
        clear_backstep_objects();
        return;
    }
    auto& line = timeline();
    line.full = true;
    auto& undo = backsteps.back().object_undo;
    if (!line.baseline.empty())
        rewind(line, undo);
    undo.clear();
    undo.shrink_to_fit();
}

void Logic::drop_backstep(std::size_t index) {
    if (index == 0 || index + 1 >= backsteps.size())
        return;
    auto& older = backsteps[index - 1].object_undo;
    const auto& newer = backsteps[index].object_undo;
    if (!newer.empty()) {
        std::vector<unsigned char> masks;
        const auto* in = older.data();
        const auto* end = in + older.size();
        std::vector<std::pair<unsigned, unsigned char>> have;
        while (in < end) {
            unsigned obj = 0;
            std::memcpy(&obj, in, sizeof(obj));
            in += sizeof(obj);
            unsigned char mask = *in++;
            have.emplace_back(obj, mask);
            for (std::size_t s = 0; s < SPANS.size(); ++s)
                if (mask & (1u << s))
                    in += SPANS[s].size;
        }
        std::ranges::sort(have);
        in = newer.data();
        end = in + newer.size();
        while (in < end) {
            unsigned obj = 0;
            std::memcpy(&obj, in, sizeof(obj));
            in += sizeof(obj);
            unsigned char mask = *in++;
            auto it = std::ranges::lower_bound(have, std::pair{ obj, (unsigned char)0 });
            unsigned char taken = it != have.end() && it->first == obj ? it->second : 0;
            unsigned char add = mask & (unsigned char)(~taken);
            if (add) {
                std::size_t at = older.size();
                older.resize(at + sizeof(obj) + 1);
                std::memcpy(older.data() + at, &obj, sizeof(obj));
                older[at + sizeof(obj)] = add;
            }
            for (std::size_t s = 0; s < SPANS.size(); ++s) {
                if (!(mask & (1u << s)))
                    continue;
                if (add & (1u << s))
                    older.insert(older.end(), in, in + SPANS[s].size);
                in += SPANS[s].size;
            }
        }
        older.shrink_to_fit();
    }
    release_backstep(backsteps[index]);
    backsteps.erase(backsteps.begin() + (std::ptrdiff_t)index);
}

void Logic::clear_backstep_objects() {
    auto& line = timeline();
    line = Timeline{};
}

void Logic::load_backstep_objects(int frame) {
    auto& line = timeline();
    if (!line.baseline.empty())
        line.pending_frame = frame;
    line.full = true;
}

void echo_touch_hook(bool on) {
    if (!g_touch_hook || on == g_touch_tracking)
        return;
    if (on ? g_touch_hook->enable().isOk() : g_touch_hook->disable().isOk()) {
        g_touch_tracking = on;
        if (on)
            timeline().full = true;
    }
}

class $modify(BackstepTouched, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (auto hook = self.getHook("GJBaseGameLayer::updateObjectSection"))
            g_touch_hook = hook.unwrap();
    }

    static void touch_all(CCArray* objects) {
        if (!objects || timeline().by_uid.empty() || Logic::get().predicting)
            return;
        for (unsigned n = 0; n < objects->count(); ++n) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
            if (!obj)
                break;
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
