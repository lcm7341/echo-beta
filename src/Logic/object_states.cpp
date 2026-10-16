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

struct ObjectSpan {
    unsigned short begin;
    unsigned short size;
    bool moves_object;
};

#define ECHO_SPAN(first, last, moves_object)                                                                                        \
ObjectSpan {                                                                                                                          \
    (unsigned short)offsetof(GameObject, first),                                                             \
        (unsigned short)(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)), \
        moves_object                                                                                                            \
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

constexpr std::size_t OBJECT_BYTES = [] {
    std::size_t total = 0;
    for (const auto& span : SPANS)
        total += span.size;
    return total;
}();

static void read_object_bytes(const GameObject* object, unsigned char* out) {
    const auto* raw = reinterpret_cast<const unsigned char*>(object);
    for (const auto& span : SPANS) {
        std::memcpy(out, raw + span.begin, span.size);
        out += span.size;
    }
}

static unsigned char write_object_bytes(GJBaseGameLayer* play, GameObject* object, const unsigned char* record) {
    auto* raw = reinterpret_cast<unsigned char*>(object);
    unsigned char written = 0;
    bool moved = false;
    for (std::size_t i = 0; i < SPANS.size(); ++i) {
        const auto& span = SPANS[i];
        if (std::memcmp(raw + span.begin, record, span.size) != 0) {
            std::memcpy(raw + span.begin, record, span.size);
            written |= (unsigned char)(1u << i);
            moved = moved || span.moves_object;
        }
        record += span.size;
    }
    if (!written) return 0;
    object->m_isOrientedBoxDirty = true;
    if (moved) play->updateObjectSection(object);
    return written;
}

#define SPANS_EXCEPT_DIRTY_FLAGS 31

static unsigned char append_record(std::vector<unsigned char>& out, unsigned index, const unsigned char* before, const unsigned char* now, const unsigned char* keep) {
    std::size_t at = out.size();
    out.resize(at + sizeof(index) + 1);
    std::memcpy(out.data() + at, &index, sizeof(index));
    unsigned char mask = 0;
    std::size_t offset = 0;
    for (std::size_t s = 0; s < SPANS.size(); ++s) {
        auto size = SPANS[s].size;
        if (std::memcmp(before + offset, now + offset, size) != 0) {
            mask |= (unsigned char)(1u << s);
            out.insert(out.end(), keep + offset, keep + offset + size);
        }
        offset += size;
    }
    out[at + sizeof(index)] = mask;
    return mask;
}

static unsigned record_index(const unsigned char* in) {
    unsigned index = 0;
    std::memcpy(&index, in, sizeof(index));
    return index;
}

static void apply_record(const unsigned char*& in, unsigned char* bytes) {
    in += sizeof(unsigned);
    unsigned char mask = *in++;
    for (std::size_t s = 0; s < SPANS.size(); ++s) {
        if (mask & (1u << s)) {
            std::memcpy(bytes, in, SPANS[s].size);
            in += SPANS[s].size;
        }
        bytes += SPANS[s].size;
    }
}

static std::vector<GameObject*> grouped_objects(PlayLayer* play) {
    std::vector<GameObject*> out;
    if (!play->m_objects) return out;
    for (unsigned i = 0; i < play->m_objects->count(); ++i) {
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        if (obj && obj->m_groupCount > 0) out.push_back(obj);
    }
    return out;
}

static std::shared_ptr<ObjectsFullCopy const> shared_full_copy(PlayLayer* play) {
    static std::weak_ptr<ObjectsFullCopy const> shared;
    unsigned amt = play->m_objects->count();
    if (auto base = shared.lock(); base && base->objects.size() == amt) {
        bool same = true;
        for (unsigned i = 0; i < amt && same; ++i)
            same = play->m_objects->objectAtIndex(i) == base->objects[i];
        if (same) return base;
    }
    auto base = std::make_shared<ObjectsFullCopy>();
    base->objects.reserve(amt);
    base->bytes.resize((std::size_t)amt * OBJECT_BYTES);
    for (unsigned i = 0; i < amt; ++i) {
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        base->objects.push_back(obj);
        if (obj) read_object_bytes(obj, base->bytes.data() + (std::size_t)i * OBJECT_BYTES);
    }
    shared = base;
    return base;
}

// a full copy was 31mb per checkpoint on big levels :/
// so 1 full copy (base) & each checkpoint only keeps what is different
static std::shared_ptr<CheckpointObjects> capture_objects(PlayLayer* play, int frame) {
    auto states = std::make_shared<CheckpointObjects>();
    states->frame = frame;
    if (!play->m_objects) return states;
    states->full_copy = shared_full_copy(play);
    const auto& base = *states->full_copy;
    auto& diff = states->changes;
    std::array<unsigned char, OBJECT_BYTES> now;
    for (unsigned i = 0; i < base.objects.size(); ++i) {
        auto* obj = base.objects[i];
        if (!obj) continue;
        const auto* before = base.bytes.data() + (std::size_t)i * OBJECT_BYTES;
        read_object_bytes(obj, now.data());
        if (std::memcmp(before, now.data(), OBJECT_BYTES) == 0) continue;
        append_record(diff, i, before, now.data(), now.data());
    }
    diff.shrink_to_fit();
    return states;
}

static void restore_objects(PlayLayer* play, const CheckpointObjects& states) {
    if (!states.full_copy || !play->m_objects || play->m_objects->count() != states.full_copy->objects.size()) {
        return;
    }
    const auto& base = *states.full_copy;
    const auto* in = states.changes.data();
    const auto* end = in + states.changes.size();
    std::array<unsigned char, OBJECT_BYTES> changed;
    for (unsigned i = 0; i < base.objects.size(); ++i) {
        const auto* record = base.bytes.data() + (std::size_t)i * OBJECT_BYTES;
        if (in < end && record_index(in) == i) {
            std::memcpy(changed.data(), record, OBJECT_BYTES);
            apply_record(in, changed.data());
            record = changed.data();
        }
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        if (obj != base.objects[i]) {
            return;
        }
        if (obj) write_object_bytes(play, obj, record);
    }
}

struct BackstepObjects {
    PlayLayer* play = nullptr;
    CCArray* array = nullptr;
    std::vector<GameObject*> objects;
    std::vector<unsigned char> saved_bytes;
    int restore_at_frame = -1;
    std::vector<unsigned> index_by_uid;
    std::vector<unsigned> touched_round, touched_objects;
    unsigned round = 1;
    bool check_all = true, always_check_all = false;
    int save_count = 0;
};

static BackstepObjects g_backstep_objects;

static void note_touched(GameObject* object) {
    auto& tracked = g_backstep_objects;
    if (!object || tracked.index_by_uid.empty() || Logic::get().predicting) return;
    auto uid = (std::size_t)object->m_uniqueID;
    if (uid >= tracked.index_by_uid.size()) return;
    auto at = tracked.index_by_uid[uid];
    if (!at || tracked.objects[at - 1] != object) return;
    auto i = at - 1;
    if (tracked.touched_round[i] == tracked.round) return;
    tracked.touched_round[i] = tracked.round;
    tracked.touched_objects.push_back(i);
}

#define MOVE_TRIGGER 901
#define ROTATE_TRIGGER 1346
#define ADVANCED_FOLLOW 3016
#define EDIT_ADVANCED_FOLLOW 3660
#define RETARGET_ADVANCED_FOLLOW 3661
#define KEYFRAME 3032
#define KEYFRAME_ANIMATION 3033

static bool has_untracked_motion(PlayLayer* play) {
    const auto& game = play->m_gameState;
    if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty())
        return true;
    for (const auto& command : play->m_effectManager->m_unkVector560) {
        auto* trigger = command.m_gameObject;
        int id = trigger ? trigger->m_objectID : 0;
        if (id != MOVE_TRIGGER && id != ROTATE_TRIGGER) return true;
    }
    return false;
}

static bool lvl_has_untracked_movers(PlayLayer* play) {
    if (!play->m_objects) return false;
    for (unsigned i = 0; i < play->m_objects->count(); ++i) {
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        if (!obj) continue;
        switch (obj->m_objectID) {
        case ADVANCED_FOLLOW:
        case EDIT_ADVANCED_FOLLOW:
        case RETARGET_ADVANCED_FOLLOW:
        case KEYFRAME:
        case KEYFRAME_ANIMATION:
            return true;
        default:
            break;
        }
    }
    return false;
}

static void rewind(BackstepObjects& tracked, const std::vector<unsigned char>& undo) {
    const auto* in = undo.data();
    const auto* end = in + undo.size();
    while (in < end)
        apply_record(in, tracked.saved_bytes.data() + (std::size_t)record_index(in) * OBJECT_BYTES);
}

void Logic::step_object_states(PlayLayer* play) {
    if (!play || predicting || backstepping) return;
    int frame = step_frame;
    auto& tracked = g_backstep_objects;
    if (!tracked.index_by_uid.empty() && !tracked.check_all && has_untracked_motion(play)) tracked.check_all = true;
    if (tracked.restore_at_frame >= 0) {
        int saved = std::exchange(tracked.restore_at_frame, -1);
        if (saved == frame && tracked.play == play && tracked.array == play->m_objects) {
            for (std::size_t i = 0; i < tracked.objects.size(); ++i)
                write_object_bytes(play, tracked.objects[i], tracked.saved_bytes.data() + i * OBJECT_BYTES);
        }
    }
    if (auto* obj = std::exchange(object_restore, nullptr)) {
        object_capture = nullptr;
        auto it = std::ranges::find(checkpoints, obj, &Checkpoint::object);
        if (it == checkpoints.end() || !it->objects) return;
        if (it->objects->frame != frame) {
            return;
        }
        restore_objects(play, *it->objects);
        return;
    }
    if (auto* obj = std::exchange(object_capture, nullptr)) {
        auto it = std::ranges::find(checkpoints, obj, &Checkpoint::object);
        if (it == checkpoints.end()) return;
        if (it->frame != frame) return;
        it->objects = capture_objects(play, frame);
    }
}

void Logic::note_backstep_objects(PlayLayer* play) {
    auto& tracked = g_backstep_objects;
    if (tracked.play != play || tracked.array != play->m_objects) {
        tracked.play = play;
        tracked.array = play->m_objects;
        tracked.objects = grouped_objects(play);
        tracked.saved_bytes.resize(tracked.objects.size() * OBJECT_BYTES);
        for (std::size_t i = 0; i < tracked.objects.size(); ++i)
            read_object_bytes(tracked.objects[i], tracked.saved_bytes.data() + i * OBJECT_BYTES);
        int max_uid = 0;
        for (auto* obj : tracked.objects)
            max_uid = std::max(max_uid, obj->m_uniqueID);
        tracked.index_by_uid.assign((std::size_t)std::max(max_uid, 0) + 1, 0);
        for (std::size_t i = 0; i < tracked.objects.size(); ++i) {
            auto uid = tracked.objects[i]->m_uniqueID;
            if (uid >= 0) tracked.index_by_uid[(std::size_t)uid] = (unsigned)(i + 1);
        }
        tracked.touched_round.assign(tracked.objects.size(), 0);
        tracked.touched_objects.clear();
        ++tracked.round;
        tracked.always_check_all = lvl_has_untracked_movers(play);
        tracked.check_all = tracked.always_check_all || has_untracked_motion(play);
        tracked.save_count = 0;
        while (backsteps.size() > 1) {
            release_backstep(backsteps.front());
            backsteps.pop_front();
        }
        return;
    }
    if (backsteps.size() < 2) return;
    auto& undo = backsteps[backsteps.size() - 2].object_undo;
    undo.clear();
    bool spot_check = !tracked.check_all && !tracked.always_check_all && (++tracked.save_count % 32) == 0;
    bool check_everything = tracked.check_all || tracked.always_check_all || spot_check || !g_touch_tracking;
    auto save_undo = [&](std::size_t i, std::vector<unsigned char>& out, unsigned char* scratch, int& missed_touches) {
        auto* saved = tracked.saved_bytes.data() + i * OBJECT_BYTES;
        const auto* raw = reinterpret_cast<const unsigned char*>(tracked.objects[i]);
        bool same = true;
        std::size_t offset = 0;
        for (const auto& span : SPANS) {
            if (std::memcmp(raw + span.begin, saved + offset, span.size) != 0) {
                same = false;
                break;
            }
            offset += span.size;
        }
        if (same) return;
        read_object_bytes(tracked.objects[i], scratch);
        unsigned char mask = append_record(out, (unsigned)i, saved, scratch, saved);
        std::memcpy(saved, scratch, OBJECT_BYTES);
        if (spot_check && (mask & SPANS_EXCEPT_DIRTY_FLAGS) && tracked.touched_round[i] != tracked.round) ++missed_touches;
    };
    int missed_touches = 0;
    std::array<unsigned char, OBJECT_BYTES> scratch;
    std::size_t total = tracked.objects.size();
    unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    if (check_everything && total >= 16384 && workers > 1) {
        std::size_t per_worker = (total + workers - 1) / workers;
        std::vector<std::vector<unsigned char>> worker_undo(workers);
        std::vector<int> worker_misses(workers, 0);
        std::vector<std::thread> threads;
        for (unsigned w = 1; w < workers; ++w) {
            threads.emplace_back([&, w] {
                std::array<unsigned char, OBJECT_BYTES> local;
                for (std::size_t i = w * per_worker; i < std::min(total, (w + 1) * per_worker); ++i)
                    save_undo(i, worker_undo[w], local.data(), worker_misses[w]);
            });
        }
        for (std::size_t i = 0; i < std::min(total, per_worker); ++i)
            save_undo(i, undo, scratch.data(), worker_misses[0]);
        for (auto& thread : threads)
            thread.join();
        for (unsigned w = 0; w < workers; ++w) {
            missed_touches += worker_misses[w];
            if (w > 0) undo.insert(undo.end(), worker_undo[w].begin(), worker_undo[w].end());
        }
    }
    else if (check_everything) {
        for (std::size_t i = 0; i < total; ++i)
            save_undo(i, undo, scratch.data(), missed_touches);
    }
    else {
        for (auto i : tracked.touched_objects)
            save_undo(i, undo, scratch.data(), missed_touches);
    }
    if (missed_touches) {
        tracked.always_check_all = true;
    }
    tracked.touched_objects.clear();
    ++tracked.round;
    tracked.check_all = tracked.always_check_all || has_untracked_motion(play);
    undo.shrink_to_fit();
}

void Logic::pop_backstep() {
    if (backsteps.empty()) return;
    release_backstep(backsteps.back());
    backsteps.pop_back();
    if (backsteps.empty()) {
        clear_backstep_objects();
        return;
    }
    auto& tracked = g_backstep_objects;
    tracked.check_all = true;
    auto& undo = backsteps.back().object_undo;
    if (!tracked.saved_bytes.empty()) rewind(tracked, undo);
    undo.clear();
    undo.shrink_to_fit();
}

void Logic::drop_backstep(std::size_t index) {
    if (index == 0 || index + 1 >= backsteps.size()) return;
    auto& older = backsteps[index - 1].object_undo;
    const auto& newer = backsteps[index].object_undo;
    if (!newer.empty()) {
        std::vector<unsigned char> masks;
        const auto* in = older.data();
        const auto* end = in + older.size();
        std::vector<std::pair<unsigned, unsigned char>> older_masks;
        while (in < end) {
            unsigned obj = 0;
            std::memcpy(&obj, in, sizeof(obj));
            in += sizeof(obj);
            unsigned char mask = *in++;
            older_masks.emplace_back(obj, mask);
            for (std::size_t s = 0; s < SPANS.size(); ++s)
                if (mask & (1u << s)) in += SPANS[s].size;
        }
        std::ranges::sort(older_masks);
        in = newer.data();
        end = in + newer.size();
        while (in < end) {
            unsigned obj = 0;
            std::memcpy(&obj, in, sizeof(obj));
            in += sizeof(obj);
            unsigned char mask = *in++;
            auto it = std::ranges::lower_bound(older_masks, std::pair{ obj, (unsigned char)0 });
            unsigned char older_has = it != older_masks.end() && it->first == obj ? it->second : 0;
            unsigned char missing = mask & (unsigned char)(~older_has);
            if (missing) {
                std::size_t at = older.size();
                older.resize(at + sizeof(obj) + 1);
                std::memcpy(older.data() + at, &obj, sizeof(obj));
                older[at + sizeof(obj)] = missing;
            }
            for (std::size_t s = 0; s < SPANS.size(); ++s) {
                if (!(mask & (1u << s))) continue;
                if (missing & (1u << s)) older.insert(older.end(), in, in + SPANS[s].size);
                in += SPANS[s].size;
            }
        }
        older.shrink_to_fit();
    }
    release_backstep(backsteps[index]);
    backsteps.erase(backsteps.begin() + (std::ptrdiff_t)index);
}

void Logic::clear_backstep_objects() {
    auto& tracked = g_backstep_objects;
    tracked = BackstepObjects{};
}

void Logic::load_backstep_objects(int frame) {
    auto& tracked = g_backstep_objects;
    if (!tracked.saved_bytes.empty()) tracked.restore_at_frame = frame;
    tracked.check_all = true;
}

void enable_touch_hook(bool on) {
    if (!g_touch_hook || on == g_touch_tracking) return;
    if (on ? g_touch_hook->enable().isOk() : g_touch_hook->disable().isOk()) {
        g_touch_tracking = on;
        if (on) g_backstep_objects.check_all = true;
    }
}

class $modify(BackstepTouched, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (auto hook = self.getHook("GJBaseGameLayer::updateObjectSection"))
            g_touch_hook = hook.unwrap();
    }

    static void touch_all(CCArray* objects) {
        if (!objects || g_backstep_objects.index_by_uid.empty() || Logic::get().predicting) return;
        for (unsigned n = 0; n < objects->count(); ++n) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
            if (!obj) break;
            note_touched(obj);
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
        note_touched(object);
        GJBaseGameLayer::updateObjectSection(object);
    }
};
