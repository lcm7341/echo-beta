// moving objects in the trajectory
//
// the levels move / rotate / follow / scale triggers are run AHEAD of time on a shadow copy of the effect manager
// that gives where every object will be on each future step
// a prediction step puts the nearby objects where they will be, runs, then puts them back
// nothing real is toggled or spawned while forecasting
// the forecast is built a bit per frame (BUILD_STEPS_PER_FRAME) so it doesnt hitch
#include "Hack/level_forecast.hpp"
#include "Logic/game_state_copy.hpp"
#include "Logic/logic.hpp"
#include "Physics/moving_objects.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EffectManagerState.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/CCMoveCNode.hpp>
#include <Geode/binding/GroupCommandObject2.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace geode::prelude;

void trace_note(std::string const& line);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

struct ForecastSpan {
    unsigned short begin;
    unsigned short size;
};

#define OBJECT_FIELD_SPAN(first, last)                                                                                              \
ForecastSpan {                                                                                                                  \
    (unsigned short)offsetof(GameObject, first),                                                            \
        (unsigned short)(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)) \
}
static const std::array SAVED_FIELD_SPANS = {
    OBJECT_FIELD_SPAN(m_positionXOffset, m_scaleYOffset),
    OBJECT_FIELD_SPAN(m_isDirty, m_isOrientedBoxDirty),
    OBJECT_FIELD_SPAN(m_unmodifiedPositionX, m_positionY),
    OBJECT_FIELD_SPAN(m_startRotationX, m_customScaleY),
    ForecastSpan{ (unsigned short)offsetof(GameObject, m_startFlipX), 4 },
    OBJECT_FIELD_SPAN(m_scaleX, m_scaleY),
    OBJECT_FIELD_SPAN(m_lastPosition, m_unk4CC),
    ForecastSpan{ (unsigned short)offsetof(GameObject, m_useTextureRectForSelection), 4 },
};
#undef OBJECT_FIELD_SPAN
#pragma clang diagnostic pop

static const std::size_t RECORD_SIZE = [] {
    std::size_t total = 0;
    for (auto const& span : SAVED_FIELD_SPANS)
        total += span.size;
    return total;
}();

static const std::size_t RECORD_POSITION_OFFSET = [] {
    std::size_t at = 0;
    for (auto const& span : SAVED_FIELD_SPANS) {
        if (span.begin <= offsetof(GameObject, m_positionX) && offsetof(GameObject, m_positionX) < span.begin + span.size)
            return at + (offsetof(GameObject, m_positionX) - span.begin);
        at += span.size;
    }
    return std::size_t{ 0 };
}();

static void save_object_fields(GameObject const* object, unsigned char* out) {
    auto const* raw = reinterpret_cast<unsigned char const*>(object);
    for (auto const& span : SAVED_FIELD_SPANS) {
        std::memcpy(out, raw + span.begin, span.size);
        out += span.size;
    }
}

static void load_object_fields(GameObject* object, unsigned char const* in) {
    auto* raw = reinterpret_cast<unsigned char*>(object);
    for (auto const& span : SAVED_FIELD_SPANS) {
        std::memcpy(raw + span.begin, in, span.size);
        in += span.size;
    }
}

struct NodeLook {
    float rotationX, rotationY, scaleX, scaleY;
    GLubyte opacity;
};

static NodeLook node_look_of(GameObject* object) {
    return { object->getRotationX(), object->getRotationY(), object->getScaleX(), object->getScaleY(), object->getOpacity() };
}

using SectionBucket = gd::vector<GameObject*>;
using SectionColumn = gd::vector<SectionBucket*>;
using SectionGrid = gd::vector<SectionColumn*>;
using SectionCounts = gd::vector<gd::vector<int>*>;

static SectionBucket* bucket_at(SectionGrid const& grid, int x, int y) {
    if (x < 0 || y < 0 || (std::size_t)x >= grid.size() || !grid[x]) return nullptr;
    auto& column = *grid[x];
    return (std::size_t)y < column.size() ? column[y] : nullptr;
}

static int* count_at(SectionCounts const& counts, int x, int y) {
    if (x < 0 || y < 0 || (std::size_t)x >= counts.size() || !counts[x]) return nullptr;
    auto& column = *counts[x];
    return (std::size_t)y < column.size() ? &column[y] : nullptr;
}

struct SectionListCopy {
    std::vector<GameObject*> items;
    int count = 0;
    bool present = false;
};

static void save_section_list(SectionListCopy& out, SectionBucket const* bucket, int const* count) {
    out.present = bucket && count;
    if (!out.present) return;
    out.count = *count;
    int used = std::clamp(*count, 0, (int)bucket->size());
    out.items.assign(bucket->begin(), bucket->begin() + used);
}

template <class FixIndexes>
static void restore_section_list(SectionListCopy const& copy, SectionBucket* bucket, int* count, FixIndexes fix_indexes) {
    if (!copy.present || !bucket || !count) return;
    if (bucket->size() < copy.items.size()) bucket->resize(copy.items.size());
    std::copy(copy.items.begin(), copy.items.end(), bucket->begin());
    *count = copy.count;
    for (std::size_t i = 0; i < copy.items.size(); ++i) {
        if (copy.items[i]) fix_indexes(copy.items[i], (int)i);
    }
}

struct SectionCell {
    int x = 0, y = 0;
    SectionListCopy main, non_effect, blocks;
    bool flag = false, flag_present = false;
};

struct SavedSections {
    bool active = false;
    GJBaseGameLayer* layer = nullptr;
    std::unordered_map<unsigned long long, SectionCell> cells;
    SectionListCopy calc_non_effect, calc_blocks;
    std::vector<int> deactivated_ids;
    int moved_count = 0;
    GameObject* updating_object = nullptr;
    bool updating_object_removed = false;
    bool updating_object_was_queued = false;
    struct SectionIndexes {
        int other, inner, outer, middle;
    };
    std::unordered_map<GameObject*, SectionIndexes> saved_indexes;

    void save_indexes(GameObject* object) {
        if (active && object)
            saved_indexes.try_emplace(object, SectionIndexes{ object->m_someOtherIndex, object->m_innerSectionIndex, object->m_outerSectionIndex, object->m_middleSectionIndex });
    }

    static unsigned long long cell_key(int x, int y) {
        return ((unsigned long long)((unsigned)x) << 32) | (unsigned)y;
    }

    SectionGrid& section_grid() {
        return *reinterpret_cast<SectionGrid*>(&layer->m_sections);
    }

    SectionGrid& non_effect_grid() {
        return *reinterpret_cast<SectionGrid*>(&layer->m_nonEffectObjects);
    }

    SectionGrid& block_grid() {
        return *reinterpret_cast<SectionGrid*>(&layer->m_collisionBlockSections);
    }

    SectionCounts& section_counts() {
        return *reinterpret_cast<SectionCounts*>(&layer->m_sectionSizes);
    }

    SectionCounts& non_effect_counts() {
        return *reinterpret_cast<SectionCounts*>(&layer->m_nonEffectObjectsSizes);
    }

    SectionCounts& block_counts() {
        return *reinterpret_cast<SectionCounts*>(&layer->m_collisionBlockSectionSizes);
    }

    void start_saving(GJBaseGameLayer* play) {
        active = true;
        layer = play;
        cells.clear();
        deactivated_ids.clear();
        saved_indexes.clear();
        save_section_list(calc_non_effect, &play->m_calcNonEffectObjects, &play->m_calcNonEffectObjectsSize);
        save_section_list(calc_blocks, &play->m_calcCollisionBlockObjects, &play->m_calcCollisionBlockObjectsSize);
        moved_count = play->m_movedCount;
    }

    bool flag_at(int x, int y, bool& present) {
        auto& flags = layer->m_nonEffectObjectsFlags;
        present = x >= 0 && y >= 0 && (std::size_t)x < flags.size() && flags[x] && (std::size_t)y < flags[x]->size();
        return present && (*flags[x])[y];
    }

    SectionCell* save_cell(int x, int y) {
        if (!active || x < 0 || y < 0) return nullptr;
        auto [it, fresh] = cells.try_emplace(cell_key(x, y));
        if (!fresh) return nullptr;
        auto& cell = it->second;
        cell.x = x;
        cell.y = y;
        save_section_list(cell.main, bucket_at(section_grid(), x, y), count_at(section_counts(), x, y));
        save_section_list(cell.non_effect, bucket_at(non_effect_grid(), x, y), count_at(non_effect_counts(), x, y));
        save_section_list(cell.blocks, bucket_at(block_grid(), x, y), count_at(block_counts(), x, y));
        cell.flag = flag_at(x, y, cell.flag_present);
        return &cell;
    }

    void save_cell_before_append(GameObject* object) {
        if (!active) return;
        int x = object->m_outerSectionIndex, y = object->m_middleSectionIndex;
        if (cells.contains(cell_key(x, y))) return;
        auto* cell = save_cell(x, y);
        if (!cell) return;
        auto drop = [object](SectionListCopy& list) {
            if (list.present && !list.items.empty() && list.items.back() == object) {
                list.items.pop_back();
                --list.count;
            }
        };
        drop(cell->main);
        drop(cell->non_effect);
        drop(cell->blocks);
        if (cell->non_effect.present && cell->non_effect.items.empty()) cell->flag = false;
    }

    void restore_saved() {
        if (!active) return;
        active = false;
        for (auto const& [object, was] : saved_indexes) {
            object->m_someOtherIndex = was.other;
            object->m_innerSectionIndex = was.inner;
            object->m_outerSectionIndex = was.outer;
            object->m_middleSectionIndex = was.middle;
        }
        saved_indexes.clear();
        for (auto& [id, cell] : cells) {
            int x = cell.x, y = cell.y;
            restore_section_list(cell.main, bucket_at(section_grid(), x, y), count_at(section_counts(), x, y), [x, y](GameObject* object, int slot) {
                object->m_someOtherIndex = slot;
                object->m_outerSectionIndex = x;
                object->m_middleSectionIndex = y;
            });
            auto side = [](GameObject* object, int slot) {
                object->m_innerSectionIndex = slot;
            };
            restore_section_list(cell.non_effect, bucket_at(non_effect_grid(), x, y), count_at(non_effect_counts(), x, y), side);
            restore_section_list(cell.blocks, bucket_at(block_grid(), x, y), count_at(block_counts(), x, y), side);
            bool present = false;
            flag_at(x, y, present);
            if (present && cell.flag_present)
                (*layer->m_nonEffectObjectsFlags[x])[y] = cell.flag;
        }
        auto side = [](GameObject* object, int slot) {
            object->m_innerSectionIndex = slot;
        };
        restore_section_list(calc_non_effect, &layer->m_calcNonEffectObjects, &layer->m_calcNonEffectObjectsSize, side);
        restore_section_list(calc_blocks, &layer->m_calcCollisionBlockObjects, &layer->m_calcCollisionBlockObjectsSize, side);
        if (auto* queued = layer->m_objectsToDeactivate) {
            for (int id : deactivated_ids)
                queued->removeObjectForKey(id);
        }
        layer->m_movedCount = moved_count;
        cells.clear();
        deactivated_ids.clear();
    }
};

static const double POSITION_TOLERANCE = 0.3;
static const int BUILD_STEPS_PER_FRAME = 60;

struct Forecast {
    PlayLayer* play = nullptr;
    CCArray* objects_array = nullptr;
    unsigned objects_array_count = 0;
    unsigned long long group_version = 0;
    std::vector<GameObject*> objects;
    std::unordered_map<GameObject*, unsigned> object_index;

    GJEffectManager* shadow_manager = nullptr;
    bool valid = false;
    unsigned long long origin_step = 0;
    float dt = 0.f;
    int built_steps = 0;
    std::vector<std::vector<std::pair<unsigned, unsigned>>> step_records;
    std::vector<CCPoint> assumed_player_pos;
    unsigned build_number = 0;
    std::vector<unsigned char> record_bytes;
    std::vector<unsigned> latest_record;
    std::vector<unsigned char> decoration_bytes, has_decoration_bytes;

    std::vector<unsigned char> live_fields;
    std::vector<NodeLook> live_looks;
    std::vector<unsigned> live_fields_saved;

    // objects that go up & down exactly w/ the player (lock to player y, a 1:1 follow). lock_y: 0 = dont know yet,
    // 1 = every step so far it moved by the players own dy, 2 = no. pre_y = where it was before the steps move
    std::vector<unsigned char> lock_y;
    std::vector<double> pre_y;

    std::vector<CCPoint> live_node_pos;
    std::vector<unsigned> live_node_pos_stamp;

    // follow player y. the forecast can only guess 1 path for the icon, but every line of the trajectory has its own,
    // & a follower goes where THAT lines icon goes. so each line works its followers out itself, step by step, the
    // way gd does (processPlayerFollowActions): y += clamp(speed * (icon y + offset - y), max speed)
    struct FollowAction {
        int group;
        float speed, max, delay;
        int offset;
    };
    std::vector<std::vector<FollowAction>> follow_actions_per_step;
    std::unordered_map<int, std::vector<unsigned>> follow_group_members;
    std::vector<double> follower_y;
    std::vector<unsigned> follower_y_stamp;
    unsigned follower_y_round = 1;
    // gd keeps the icons last 200 y's, 1 every 0.01s (PlayerObject::updateSpecial), & a follow w/ a delay reads back
    // into that (getOldPosition). ts is the line being drawns own copy of it, carried on from the real icons
    std::array<float, 200> y_history{};
    int y_history_at = 0;
    float y_history_time = 0.f;
    bool y_history_ready = false;

    SavedSections sections;
    std::vector<unsigned> touched_objects;
    std::vector<unsigned> node_stamp, touch_stamp;
    unsigned touch_round = 1;
    bool running_ahead = false;
    std::vector<unsigned> moved, moved_stamp;
    std::vector<unsigned> elapsed_offset, elapsed_stamp;
    unsigned elapsed_round = 1;
    std::vector<unsigned> elapsed_objects;
    unsigned step_round = 1;

    int spawn_channel = 0;
    int spawn_cursor = 0;
    bool spawn_going_back = false;
    std::vector<CCPoint> player_path;
    CCPoint player_path_before;
    unsigned long long player_path_start = 0;
    CCPoint origin_player_pos, player_step;
    int steps_run = 0;

    struct FiredTrigger {
        EffectGameObject* object;
        std::vector<unsigned char> bytes;
    };

    std::vector<FiredTrigger> fired_triggers;

    std::unordered_set<GameObject*> live_moved_objects;

    bool applying = false;
    std::vector<unsigned> pending_objects, pending_offset;
    std::vector<unsigned> applied, applied_stamp;
    unsigned apply_round = 1;
};

static Forecast& forecast_state() {
    static Forecast value;
    return value;
}

static std::unordered_set<int> g_needed_groups;

static void note_needed_groups(GJEffectManager* manager) {
    auto& groups = g_needed_groups;
    groups.clear();
    for (auto const& command : manager->m_unkVector560)
        if (command.m_centerGroupID > 0) groups.insert(command.m_centerGroupID);
}

static CCArray* drop_unneeded_decorations(CCArray* objects) {
    if (!forecast_state().running_ahead || !objects) return objects;
    auto const& needed = g_needed_groups;
    auto keep = [&](GameObject* object) {
        if (object->m_objectType != GameObjectType::Decoration) return true;
        if (needed.empty() || !object->m_groups) return false;
        for (int g = 0; g < object->m_groupCount; ++g)
            if (needed.contains((*object->m_groups)[g])) return true;
        return false;
    };
    unsigned object_count = objects->count();
    unsigned first = 0;
    for (; first < object_count; ++first) {
        auto* obj = static_cast<GameObject*>(objects->objectAtIndex(first));
        if (!obj) return objects;
        if (!keep(obj)) break;
    }
    if (first == object_count) return objects;
    static CCArray* filtered = [] {
        auto* array = CCArray::create();
        array->retain();
        return array;
    }();
    filtered->removeAllObjects();
    for (unsigned n = 0; n < object_count; ++n) {
        auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
        if (!obj) break;
        if (n < first || (n > first && keep(obj))) filtered->addObject(obj);
    }
    return filtered;
}

static unsigned long long g_live_steps = 0;

static bool g_forecast_stale = true;

static int g_last_rebuild_step = 0;

static std::unordered_map<int, unsigned> g_fired_build_number;

static void refresh_objects(Forecast& f, PlayLayer* play) {
    // hopefully fixes crash
    if (f.play == play && f.objects_array == play->m_objects && f.group_version == MovingObjects::group_version &&
        play->m_objects && f.objects_array_count == play->m_objects->count())
        return;
    f.play = play;
    f.objects_array = play->m_objects;
    f.objects_array_count = play->m_objects ? play->m_objects->count() : 0;
    f.group_version = MovingObjects::group_version;
    for (auto* obj : f.objects)
        obj->release();
    f.objects.clear();
    f.object_index.clear();
    f.valid = false;
    // these hold places in the object list that was just thrown away (crash: a new lvl, the old followers numbers
    // pointed at whatever sat there now)
    f.follow_group_members.clear();
    f.follow_actions_per_step.clear();
    if (!play->m_objects) return;
    for (unsigned i = 0; i < play->m_objects->count(); ++i) {
        auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
        if (!obj || obj->m_groupCount <= 0) continue;
        f.object_index.emplace(obj, (unsigned)f.objects.size());
        obj->retain();
        f.objects.push_back(obj);
    }
    std::size_t amt = f.objects.size();
    f.live_fields.assign(amt * RECORD_SIZE, 0);
    f.live_looks.assign(amt, {});
    f.live_fields_saved.assign(amt, 0);
    f.lock_y.assign(amt, 0);
    f.pre_y.assign(amt, 0.0);
    f.live_node_pos.assign(amt, {});
    f.live_node_pos_stamp.assign(amt, 0);
    f.follower_y.assign(amt, 0.0);
    f.follower_y_stamp.assign(amt, 0);
    f.moved_stamp.assign(amt, 0);
    f.applied_stamp.assign(amt, 0);
    f.touch_stamp.assign(amt, 0);
    f.node_stamp.assign(amt, 0);
    f.pending_offset.assign(amt, UINT_MAX);
    f.latest_record.assign(amt, UINT_MAX);
    f.has_decoration_bytes.assign(amt, 0);
    f.decoration_bytes.clear();
}

static unsigned ensure_index(Forecast& f, GameObject* object) {
    auto [it, fresh] = f.object_index.try_emplace(object, (unsigned)f.objects.size());
    if (!fresh) return it->second;
    object->retain();
    f.objects.push_back(object);
    f.live_fields.resize(f.objects.size() * RECORD_SIZE);
    f.live_looks.emplace_back();
    f.live_fields_saved.push_back(0);
    f.lock_y.push_back(0);
    f.pre_y.push_back(0.0);
    f.live_node_pos.emplace_back();
    f.live_node_pos_stamp.push_back(0);
    f.follower_y.push_back(0.0);
    f.follower_y_stamp.push_back(0);
    f.moved_stamp.push_back(0);
    f.applied_stamp.push_back(0);
    f.touch_stamp.push_back(0);
    f.node_stamp.push_back(0);
    f.pending_offset.push_back(UINT_MAX);
    f.latest_record.push_back(UINT_MAX);
    f.has_decoration_bytes.push_back(0);
    return it->second;
}

static void save_live_fields(Forecast& f, unsigned i) {
    f.live_fields_saved[i] = 1;
    save_object_fields(f.objects[i], f.live_fields.data() + (std::size_t)i * RECORD_SIZE);
    f.live_looks[i] = node_look_of(f.objects[i]);
}

static void save_live_fields_once(Forecast& f, unsigned i) {
    if (!f.live_fields_saved[i]) save_live_fields(f, i);
}

static void save_node_position(Forecast& f, unsigned i) {
    if (f.live_node_pos_stamp[i] == f.touch_round) return;
    f.live_node_pos_stamp[i] = f.touch_round;
    f.live_node_pos[i] = f.objects[i]->CCNode::getPosition();
}

static void restore_live_fields(Forecast& f, unsigned i, bool restore_look) {
    auto* obj = f.objects[i];
    if (restore_look && f.node_stamp[i] == f.touch_round) {
        auto const& saved = f.live_looks[i];
        if (obj->getRotationX() != saved.rotationX) obj->setRotationX(saved.rotationX);
        if (obj->getRotationY() != saved.rotationY) obj->setRotationY(saved.rotationY);
        if (obj->getScaleX() != saved.scaleX) obj->setScaleX(saved.scaleX);
        if (obj->getScaleY() != saved.scaleY) obj->setScaleY(saved.scaleY);
        if (obj->getOpacity() != saved.opacity) obj->setOpacity(saved.opacity);
    }
    load_object_fields(obj, f.live_fields.data() + (std::size_t)i * RECORD_SIZE);
    // the fields are back, but the hitbox the look ahead worked out for the place IT had the object is still sitting
    // in the objects rect & oriented box (the box is its own thing, not in the spans). the dirty flags just went back
    // to whatever they were, which can be "clean". so they are always set here: the next thing that asks gets a
    // hitbox made from where the object really is. w/out ts a portal could go off where a line had put it
    obj->m_isObjectRectDirty = true;
    obj->m_isOrientedBoxDirty = true;
    // but some objects get quickUpdatePosition at the end of the move step, which moves the sprite right away AND the color sprite (the detail part) with it
    // after a forecast they sat wherever the forecast last had them until the next live step: jittering, not where the hitbox was
    // the detail was the problem
    // fuck gd
    if (restore_look && f.live_node_pos_stamp[i] == f.touch_round) {
        auto const& saved_pos = f.live_node_pos[i];
        if (!obj->CCNode::getPosition().equals(saved_pos)) obj->CCNode::setPosition(saved_pos);
        if (auto* detail = obj->m_colorSprite; detail && !obj->m_colorSpriteLocked && !detail->getPosition().equals(saved_pos))
            detail->setPosition(saved_pos);
    }
}

static void mark_touched(Forecast& f, unsigned i) {
    if (f.touch_stamp[i] == f.touch_round) return;
    f.touch_stamp[i] = f.touch_round;
    f.touched_objects.push_back(i);
    save_node_position(f, i);
}

static unsigned record_object(Forecast& f, unsigned i) {
    auto offset = (unsigned)f.record_bytes.size();
    f.record_bytes.resize(f.record_bytes.size() + RECORD_SIZE);
    save_object_fields(f.objects[i], f.record_bytes.data() + offset);
    f.latest_record[i] = offset;
    return offset;
}

static bool is_motion_trigger(int id) {
    switch (id) {
    case 901:
    case 1346:
    case 1347:
    case 1814:
    case 2067:
        return true;
    default:
        return false;
    }
}

static void save_trigger_bytes(Forecast& f, EffectGameObject* object) {
    g_fired_build_number[object->m_uniqueID] = f.build_number;
    if (std::ranges::any_of(f.fired_triggers, [object](auto const& fired) {
            return fired.object == object;
        }))
        return;
    auto& fired = f.fired_triggers.emplace_back();
    fired.object = object;
    auto const* raw = reinterpret_cast<unsigned char const*>(object);
    fired.bytes.assign(raw + sizeof(CCSprite), raw + sizeof(EffectGameObject));
}

static void fire_passed_triggers(Forecast& f, PlayLayer* play, CCPoint at) {
    auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.spawn_channel) : nullptr);
    if (!list) return;
    auto* body = play->m_player1;
    bool sideways = body && body->m_isSideways;
    while ((unsigned)f.spawn_cursor < list->count()) {
        auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex((unsigned)f.spawn_cursor));
        if (!obj) {
            ++f.spawn_cursor;
            continue;
        }
        float key = sideways ? obj->m_speedStart.y : obj->m_speedStart.x;
        float player = sideways ? at.y : at.x;
        if (f.spawn_going_back ? key < player : key > player) break;
        ++f.spawn_cursor;
        bool spawn = obj->m_objectID == 1268;
        if (obj->m_isGroupDisabled || obj->m_isTouchTriggered || !(is_motion_trigger(obj->m_objectID) || spawn))
            continue;
        save_trigger_bytes(f, obj);
        obj->triggerObject(play, 0, nullptr);
    }
}

static void compact_records(Forecast& f) {
    std::size_t used = 0;
    for (auto const& frame : f.step_records)
        used += frame.size();
    if (f.record_bytes.size() < (used + f.objects.size()) * RECORD_SIZE * 2 + (1u << 20)) return;
    std::vector<unsigned char> kept;
    kept.reserve((used + f.objects.size()) * RECORD_SIZE);
    std::unordered_map<unsigned, unsigned> moved;
    auto keep = [&](unsigned offset) {
        auto [it, fresh] = moved.try_emplace(offset, (unsigned)kept.size());
        if (fresh)
            kept.insert(kept.end(), f.record_bytes.begin() + offset, f.record_bytes.begin() + offset + RECORD_SIZE);
        return it->second;
    };
    for (auto& frame : f.step_records) {
        for (auto& entry : frame)
            entry.second = keep(entry.second);
    }
    for (auto& offset : f.latest_record) {
        if (offset != UINT_MAX) offset = keep(offset);
    }
    f.record_bytes = std::move(kept);
}

static const std::size_t RECORD_BYTES_LIMIT = std::size_t{ 192 } << 20;

static CCPoint player_pos_at_step(Forecast const& f, unsigned long long absolute, CCPoint live_pos, CCPoint live_step, unsigned long long live_now) {
    if (!f.player_path.empty()) {
        if (absolute + 1 == f.player_path_start) return f.player_path_before;
        if (absolute >= f.player_path_start) {
            auto idx = absolute - f.player_path_start;
            if (idx < f.player_path.size()) return f.player_path[(std::size_t)idx];
            auto last = f.player_path.back();
            auto step = f.player_path.size() >= 2 ? last - f.player_path[f.player_path.size() - 2] : last - f.player_path_before;
            return last + step * (float)(idx - (f.player_path.size() - 1));
        }
    }
    return live_pos + live_step * ((float)absolute - (float)live_now);
}

static void build_ahead(Forecast& f, PlayLayer* play, int count) {
    auto& logic = Logic::get();
    auto* live_manager = play->m_effectManager;
    SavedGameState saved_state(play->m_gameState);
    auto random_state = GameToolbox::getfast_srand();
    bool predicting = logic.predicting;
    logic.predicting = true;
    ++f.touch_round;
    f.touched_objects.clear();
    for (unsigned i = 0; i < f.objects.size(); ++i)
        save_live_fields_once(f, i);
    // safety net: keep objs pos before run
    // anything the run moved w/out being seen is discovered & put back
    static std::vector<std::pair<double, double>> positions_before;
    bool check_unseen_moves = f.objects.size() <= 60000;
    if (check_unseen_moves) {
        positions_before.resize(f.objects.size());
        for (unsigned i = 0; i < f.objects.size(); ++i)
            positions_before[i] = { f.objects[i]->m_positionX, f.objects[i]->m_positionY };
    }
    f.sections.start_saving(play);
    for (unsigned i = 0; i < f.objects.size(); ++i) {
        bool use_decoration_bytes = f.has_decoration_bytes[i] && f.decoration_bytes.size() >= ((std::size_t)i + 1) * RECORD_SIZE;
        if (f.latest_record[i] == UINT_MAX && !use_decoration_bytes) continue;
        mark_touched(f, i);
        load_object_fields(f.objects[i], use_decoration_bytes ? f.decoration_bytes.data() + (std::size_t)i * RECORD_SIZE : f.record_bytes.data() + f.latest_record[i]);
        play->updateObjectSection(f.objects[i]);
    }
    auto saved_spawn_tuples = play->m_spawnTuples;
    auto* p1 = play->m_player1;
    auto* p2 = play->m_player2;
    auto p1_pos = p1 ? p1->getPosition() : CCPoint{};
    auto p2_pos = p2 ? p2->getPosition() : CCPoint{};
    auto camera = play->m_gameState.m_cameraPosition;
    auto camera2 = play->m_gameState.m_cameraPosition2;
    bool sideways = p1 && p1->m_isSideways;
    auto live_now = g_live_steps;
    auto live_step = p1 ? p1_pos - p1->m_lastPosition : CCPoint{};
    play->m_effectManager = f.shadow_manager;
    // mega hack hooks updateSpawnTriggers & reads the managers PARENT as the level, no null check
    // my copy has no parent so it crashed (megahack + 0x4f7ffc). pretend the level is its parent while i use it
    f.shadow_manager->setParent(play);
    f.running_ahead = true;
    CCPoint fire_from;
    double player_dy = 0.0;
    for (int step = 0; step < count && f.record_bytes.size() < RECORD_BYTES_LIMIT; ++step) {
        ++f.step_round;
        f.moved.clear();
        play->m_gameState.m_commandIndex += 1;
        ++f.steps_run;
        {
            unsigned long long absolute = f.origin_step + (unsigned long long)f.steps_run;
            auto at = player_pos_at_step(f, absolute - 1, p1_pos, live_step, live_now);
            auto previous_at = player_pos_at_step(f, absolute - 2, p1_pos, live_step, live_now);
            fire_from = at;
            f.assumed_player_pos.push_back(at);
            if (p1) p1->CCNode::setPosition(at);
            if (p2) p2->CCNode::setPosition(p2_pos + (at - p1_pos));
            f.shadow_manager->m_unk780 = at.x - previous_at.x;
            f.shadow_manager->m_unk784 = at.y - previous_at.y;
            player_dy = (double)at.y - (double)previous_at.y;
            auto ahead = at - p1_pos;
            auto shift = sideways ? CCPoint{ 0.f, ahead.y } : CCPoint{ ahead.x, 0.f };
            play->m_gameState.m_cameraPosition = camera + shift;
            play->m_gameState.m_cameraPosition2 = camera2 + shift;
        }
        fire_passed_triggers(f, play, fire_from);
        auto& follows = f.follow_actions_per_step.emplace_back();
        // m_unkVector6f0 is the list gd walks in processPlayerFollowActions (the effect manager + 0x608 on mac).
        // each 1 is a CCMoveCNode: its tag is the group, 0a8 the speed, 0b0 the delay, 0b8 the offset, 0c0 the max speed.
        for (auto* follow : f.shadow_manager->m_unkVector6f0) {
            if (!follow) continue;
            auto id = (std::size_t)std::clamp(follow->getTag(), 0, 9999);
            auto* members = id < play->m_groups.size() ? play->m_groups[id] : nullptr;
            if (!members) continue;
            follows.push_back({ (int)id, (float)follow->m_unk0a8, (float)follow->m_unk0c0,
                (float)follow->m_unk0b0, follow->m_unk0b8 });
            auto [listed, fresh] = f.follow_group_members.try_emplace((int)id);
            for (unsigned n = 0; n < members->count(); ++n) {
                auto* obj = static_cast<GameObject*>(members->objectAtIndex(n));
                if (!obj) break;
                auto i = ensure_index(f, obj);
                save_live_fields_once(f, i);
                // put back after the run & thats ALL. theyre not recorded: every line of the trajectory works them out
                // itself (apply). & no updateObjectSection for them, gd doesnt call it for a follow either (these sit
                // in the collision grid, not the normal sections)
                mark_touched(f, i);
                f.lock_y[i] = 3;
                if (fresh) listed->second.push_back(i);
            }
        }
        f.shadow_manager->updateSpawnTriggers(f.dt);
        note_needed_groups(f.shadow_manager);
        f.shadow_manager->prepareMoveActions(f.dt, false);
        play->processMoveActionsStep(f.dt, true);
        f.shadow_manager->postMoveActions();
        auto& frame = f.step_records.emplace_back();
        frame.reserve(f.moved.size());
        for (auto i : f.moved) {
            mark_touched(f, i);
            if (!sideways && f.lock_y[i] < 2) {
                double dy = f.objects[i]->m_positionY - f.pre_y[i];
                if (std::abs(player_dy) > 0.0001 || std::abs(dy) > 0.0001)
                    f.lock_y[i] = std::abs(dy - player_dy) <= 0.001 ? 1 : 2;
            }
            if (f.objects[i]->m_objectType == GameObjectType::Decoration) {
                if (f.decoration_bytes.size() < f.objects.size() * RECORD_SIZE)
                    f.decoration_bytes.resize(f.objects.size() * RECORD_SIZE);
                save_object_fields(f.objects[i], f.decoration_bytes.data() + (std::size_t)i * RECORD_SIZE);
                f.has_decoration_bytes[i] = 1;
                continue;
            }
            frame.emplace_back(i, record_object(f, i));
        }
    }
    f.running_ahead = false;
    f.shadow_manager->setParent(nullptr);
    play->m_effectManager = live_manager;
    play->m_spawnTuples = saved_spawn_tuples;
    if (p1) p1->CCNode::setPosition(p1_pos);
    if (p2) p2->CCNode::setPosition(p2_pos);
    for (auto it = f.fired_triggers.rbegin(); it != f.fired_triggers.rend(); ++it)
        std::memcpy(reinterpret_cast<unsigned char*>(it->object) + sizeof(CCSprite), it->bytes.data(), it->bytes.size());
    f.fired_triggers.clear();
    for (auto i : f.touched_objects)
        restore_live_fields(f, i, true);
    if (check_unseen_moves) {
        for (unsigned i = 0; i < positions_before.size() && i < f.objects.size(); ++i) {
            auto* obj = f.objects[i];
            if (obj->m_positionX == positions_before[i].first && obj->m_positionY == positions_before[i].second)
                continue;
            obj->m_positionX = positions_before[i].first;
            obj->m_positionY = positions_before[i].second;
            obj->dirtifyObjectPos();
            obj->dirtifyObjectRect();
            save_live_fields(f, i);
        }
    }
    f.sections.restore_saved();
    saved_state.restore(play->m_gameState);
    GameToolbox::fast_srand(random_state);
    logic.predicting = predicting;
    f.built_steps = (int)f.step_records.size();
}

static void rebuild(Forecast& f, PlayLayer* play, int steps) {
    f.live_moved_objects.clear();
    f.step_records.clear();
    f.follow_actions_per_step.clear();
    f.follow_group_members.clear();
    f.record_bytes.clear();
    std::fill(f.latest_record.begin(), f.latest_record.end(), UINT_MAX);
    std::fill(f.has_decoration_bytes.begin(), f.has_decoration_bytes.end(), 0);
    std::fill(f.live_fields_saved.begin(), f.live_fields_saved.end(), 0);
    std::fill(f.lock_y.begin(), f.lock_y.end(), 0);
    f.built_steps = 0;
    f.valid = false;
    f.assumed_player_pos.clear();
    ++f.build_number;
    auto* live_manager = play->m_effectManager;
    if (!live_manager) return;
    EffectManagerState saved_manager_state;
    live_manager->saveToState(saved_manager_state);
    f.origin_step = g_live_steps;
    f.valid = true;
    g_forecast_stale = false;
    g_last_rebuild_step = Logic::get().step_frame;
    auto const& game = play->m_gameState;
    f.spawn_channel = game.m_currentChannel;
    auto saved_cursor = game.m_spawnChannelRelated0.find(f.spawn_channel);
    f.spawn_cursor = saved_cursor == game.m_spawnChannelRelated0.end() ? 0 : saved_cursor->second;
    auto saved_going_back = game.m_spawnChannelRelated1.find(f.spawn_channel);
    f.spawn_going_back = saved_going_back != game.m_spawnChannelRelated1.end() && saved_going_back->second;
    f.steps_run = 0;
    if (auto* body = play->m_player1) {
        f.origin_player_pos = body->getPosition();
        f.player_step = body->getPosition() - body->m_lastPosition;
    }
    else {
        f.origin_player_pos = CCPoint{ 0.f, 0.f };
        f.player_step = CCPoint{ 0.f, 0.f };
    }
    bool motion_trigger_ahead = false;
    if (auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.spawn_channel) : nullptr)) {
        for (unsigned i = (unsigned)std::max(f.spawn_cursor, 0); i < list->count() && !motion_trigger_ahead; ++i) {
            auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(i));
            motion_trigger_ahead = obj && (is_motion_trigger(obj->m_objectID) || obj->m_objectID == 1268);
        }
    }
    if (saved_manager_state.m_vectorGroupCommandObject2.empty() && !motion_trigger_ahead) {
        if (f.shadow_manager) f.shadow_manager->release();
        f.shadow_manager = nullptr;
        f.step_records.resize((std::size_t)steps);
        f.follow_actions_per_step.resize(f.step_records.size());
        f.assumed_player_pos.resize(f.step_records.size(), CCPoint{ NAN, NAN });
        f.built_steps = steps;
        return;
    }
    if (f.shadow_manager) f.shadow_manager->release();
    f.shadow_manager = GJEffectManager::create();
    if (!f.shadow_manager) {
        f.valid = false;
        return;
    }
    f.shadow_manager->retain();
    f.shadow_manager->m_triggerEffectDelegate = live_manager->m_triggerEffectDelegate;
    f.shadow_manager->loadFromState(saved_manager_state);
    f.shadow_manager->m_unk780 = live_manager->m_unk780;
    f.shadow_manager->m_unk784 = live_manager->m_unk784;
    f.shadow_manager->m_unk788 = live_manager->m_unk788;
    f.shadow_manager->m_unk78C = live_manager->m_unk78C;
    f.shadow_manager->m_unk790 = live_manager->m_unk790;
    f.shadow_manager->m_unk794 = live_manager->m_unk794;
    f.shadow_manager->m_unk798 = live_manager->m_unk798;
    build_ahead(f, play, std::min(steps, BUILD_STEPS_PER_FRAME));
}

namespace forecast {
    bool firing() {
        return forecast_state().running_ahead;
    }

    void invalidate() {
        g_forecast_stale = true;
        forecast_state().player_path.clear();
    }

    void note_live_step() {
        ++g_live_steps;
    }

    void note_player_path(std::vector<CCPoint> path, CCPoint before) {
        auto& f = forecast_state();
        f.player_path = std::move(path);
        f.player_path_before = before;
        f.player_path_start = g_live_steps;
    }

    bool trigger_between(Forecast const& f, PlayLayer* play, float lo, float hi, bool sideways) {
        auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.spawn_channel) : nullptr);
        if (!list) return false;
        auto key_of = [&](unsigned i) {
            auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(i));
            return obj ? (sideways ? obj->m_speedStart.y : obj->m_speedStart.x) : -INFINITY;
        };
        unsigned low = 0, high = list->count();
        while (low < high) {
            unsigned mid = low + (high - low) / 2;
            if (key_of(mid) <= lo) low = mid + 1;
            else high = mid;
        }
        for (unsigned i = low; i < list->count() && key_of(i) <= hi; ++i) {
            auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(i));
            if (obj && (is_motion_trigger(obj->m_objectID) || obj->m_objectID == 1268)) return true;
        }
        return false;
    }

    bool depends_on_exact_player(Forecast const& f, PlayLayer* play) {
        auto const& game = play->m_gameState;
        if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty())
            return true;
        if (!f.shadow_manager) return false;
        for (auto const& command : f.shadow_manager->m_unkVector560)
            if (command.m_lockToPlayerX || command.m_lockToPlayerY || command.m_lockToCameraX || command.m_lockToCameraY)
                return true;
        return false;
    }

    void prepare(PlayLayer* play, int steps, float dt) {
        auto& f = forecast_state();
        if (!play || steps <= 0 || f.applying) return;
        steps = std::max(steps, std::max(32, (int)(Logic::get().sample_rate() / 15.0)));
        if (Logic::get().backstep_target >= 0 || Logic::get().backstepping) {
            f.valid = false;
            f.live_moved_objects.clear();
            return;
        }
        refresh_objects(f, play);
        if (f.dt != dt) g_forecast_stale = true;
        f.dt = dt;
        unsigned long long elapsed = g_live_steps - f.origin_step;
        if (f.valid && play->m_player1) {
            auto expected = f.origin_player_pos + f.player_step * (float)elapsed;
            bool on_path = g_live_steps >= f.player_path_start && g_live_steps - f.player_path_start < f.player_path.size();
            if (on_path) expected = f.player_path[(std::size_t)(g_live_steps - f.player_path_start)];
            auto live_pos = play->m_player1->getPosition();
            auto step = live_pos - play->m_player1->m_lastPosition;
            bool sideways = play->m_player1->m_isSideways;
            float position_error = sideways ? std::abs(expected.y - live_pos.y) : std::abs(expected.x - live_pos.x);
            float speed_error = on_path ? 0.f : sideways ? std::abs(step.y - f.player_step.y)
                                                  : std::abs(step.x - f.player_step.x);
            if (play->m_isPlatformer ? position_error > 30.f : position_error > 0.5f || speed_error > 0.001f) g_forecast_stale = true;
        }
        if (f.valid && !g_forecast_stale && elapsed > 0 && elapsed <= (unsigned long long)f.step_records.size()) {
            if (f.elapsed_stamp.size() < f.objects.size()) {
                f.elapsed_stamp.resize(f.objects.size(), 0);
                f.elapsed_offset.resize(f.objects.size(), 0);
            }
            ++f.elapsed_round;
            f.elapsed_objects.clear();
            for (unsigned long long k = 0; k < elapsed; ++k)
                for (auto [i, offset] : f.step_records[k]) {
                    if (f.elapsed_stamp[i] != f.elapsed_round) {
                        f.elapsed_stamp[i] = f.elapsed_round;
                        f.elapsed_objects.push_back(i);
                    }
                    f.elapsed_offset[i] = offset;
                }
            bool mismatch = false;
            for (auto i : f.elapsed_objects) {
                double px = 0, py = 0;
                std::memcpy(&px, f.record_bytes.data() + f.elapsed_offset[i] + RECORD_POSITION_OFFSET, sizeof(double));
                std::memcpy(&py, f.record_bytes.data() + f.elapsed_offset[i] + RECORD_POSITION_OFFSET + sizeof(double), sizeof(double));
                auto* obj = f.objects[i];
                if (f.lock_y[i] == 3) continue;
                if (std::abs(px - obj->m_positionX) > POSITION_TOLERANCE || std::abs(py - obj->m_positionY) > POSITION_TOLERANCE) {
                    mismatch = true;
                    break;
                }
            }
            for (auto* obj : f.live_moved_objects) {
                if (mismatch) break;
                auto it = f.object_index.find(obj);
                if (it != f.object_index.end() && it->second < f.elapsed_stamp.size() && f.elapsed_stamp[it->second] == f.elapsed_round)
                    continue;
                if (obj->m_objectType == GameObjectType::Decoration) continue;
                mismatch = true;
            }
            if (mismatch) g_forecast_stale = true;
        }
        for (auto* obj : f.live_moved_objects) {
            if (auto it = f.object_index.find(obj); it != f.object_index.end() && f.live_fields_saved[it->second])
                save_live_fields(f, it->second);
        }
        f.live_moved_objects.clear();
        for (unsigned i = 0; i < f.latest_record.size(); ++i) {
            if ((f.latest_record[i] != UINT_MAX || f.has_decoration_bytes[i]) && f.live_fields_saved[i]) save_live_fields(f, i);
        }
        // followers too. gd moves them w/out telling anyone, so where they really are has to be read again every time
        // (a stale 1 here = the follower gets thrown back to where it was a while ago after every look ahead)
        for (auto const& [group, members] : f.follow_group_members)
            for (auto i : members)
                if (i < f.objects.size() && f.live_fields_saved[i]) save_live_fields(f, i);
        if (f.valid && !g_forecast_stale && !f.player_path.empty() && play->m_player1) {
            bool sideways = play->m_player1->m_isSideways;
            bool exact = depends_on_exact_player(f, play);
            std::size_t built = std::min(f.assumed_player_pos.size(), f.step_records.size());
            for (std::size_t k = (std::size_t)(std::min<unsigned long long>(elapsed, built)); k < built; ++k) {
                auto was = f.assumed_player_pos[k];
                if (std::isnan(was.x)) continue;
                unsigned long long absolute = f.origin_step + k;
                if (absolute + 1 < f.player_path_start || absolute >= f.player_path_start + f.player_path.size())
                    continue;
                auto now = absolute + 1 == f.player_path_start ? f.player_path_before : f.player_path[(std::size_t)(absolute - f.player_path_start)];
                float travel = sideways ? std::abs(now.y - was.y) : std::abs(now.x - was.x);
                bool differs = exact ? std::max(std::abs(now.x - was.x), std::abs(now.y - was.y)) > 0.01f
                                     : travel > 0.01f && trigger_between(f, play, sideways ? std::min(now.y, was.y) : std::min(now.x, was.x), sideways ? std::max(now.y, was.y) : std::max(now.x, was.x), sideways);
                if (!differs) continue;
                g_forecast_stale = true;
                break;
            }
        }
        bool too_old = Logic::get().step_frame - g_last_rebuild_step > (int)(Logic::get().sample_rate() * 30.0);
        if (g_forecast_stale || !f.valid || too_old || elapsed > (unsigned long long)f.built_steps) {
            rebuild(f, play, steps);
            return;
        }
        if (elapsed > 0) {
            f.step_records.erase(f.step_records.begin(), f.step_records.begin() + (std::ptrdiff_t)elapsed);
            f.follow_actions_per_step.erase(f.follow_actions_per_step.begin(), f.follow_actions_per_step.begin() + (std::ptrdiff_t)(std::min<unsigned long long>(elapsed, f.follow_actions_per_step.size())));
            f.assumed_player_pos.erase(f.assumed_player_pos.begin(), f.assumed_player_pos.begin() + (std::ptrdiff_t)(std::min<unsigned long long>(elapsed, f.assumed_player_pos.size())));
            f.origin_player_pos = f.origin_player_pos + f.player_step * (float)elapsed;
            f.steps_run -= (int)elapsed;
            f.origin_step = g_live_steps;
            f.built_steps = (int)f.step_records.size();
            compact_records(f);
        }
        if (f.built_steps < steps) {
            if (!f.shadow_manager || f.step_records.empty() && f.record_bytes.empty()) {
                f.step_records.resize((std::size_t)steps);
                f.follow_actions_per_step.resize(f.step_records.size());
                f.assumed_player_pos.resize(f.step_records.size(), CCPoint{ NAN, NAN });
                f.built_steps = steps;
                return;
            }
            int target = steps;
            if (!f.player_path.empty() && f.player_path_start + f.player_path.size() > f.origin_step) {
                int covered = (int)(f.player_path_start + f.player_path.size() - f.origin_step);
                int lead = std::max(16, (int)(Logic::get().sample_rate() / 30.0));
                target = std::min(steps, std::max(covered + (depends_on_exact_player(f, play) ? 0 : 8), lead));
            }
            if (f.built_steps < target) build_ahead(f, play, std::min(target - f.built_steps, BUILD_STEPS_PER_FRAME));
        }
    }

    static bool record_inside(Forecast const& f, unsigned offset, CCRect const& area) {
        double px = 0, py = 0;
        std::memcpy(&px, f.record_bytes.data() + offset + RECORD_POSITION_OFFSET, sizeof(double));
        std::memcpy(&py, f.record_bytes.data() + offset + RECORD_POSITION_OFFSET + sizeof(double), sizeof(double));
        return area.containsPoint({ (float)px, (float)py });
    }

    void apply(PlayLayer* play, int step, CCRect const& area, float player_y) {
        auto& f = forecast_state();
        if (!play || play != f.play || !f.valid || step < 1) return;
        auto start_applying = [&] {
            if (f.applying) return;
            f.applying = true;
            ++f.apply_round;
            f.applied.clear();
            f.pending_objects.clear();
            f.sections.start_saving(play);
        };
        // followers (follow player y) 1st. they dont come from the forecast: ts lines icon is at player_y BEFORE the step
        // runs, & gd moves a follower off that at the start of the step, so thats done here the same way
        //   processPlayerFollowActions: y += clamp(clamp(speed * dt * 60, 0, 1) * (icon y + offset - y), +- max * dt * 60)
        //   icon y = where it is now, or w/ a delay the y it had floor(delay / 0.01) slots back in its history
        if (std::isfinite(player_y) && !f.follow_actions_per_step.empty() && play->m_player1) {
            auto* body = play->m_player1;
            auto history_slot = [&] {
                int at = f.y_history_at % 200;
                return at < 0 ? at + 200 : at;
            };
            if (!f.y_history_ready) {
                f.y_history_ready = true;
                auto const& real_history = body->m_playerFollowFloats;
                for (std::size_t n = 0; n < f.y_history.size(); ++n)
                    f.y_history[n] = n < real_history.size() ? real_history[n] : player_y;
                f.y_history_at = body->m_followRelated;
                f.y_history_time = body->m_unk838;
            }
            else {
                f.y_history_time += f.dt;
                if (f.y_history_time >= 0.01f) {
                    f.y_history_time += -0.01f;
                    ++f.y_history_at;
                }
                f.y_history[(std::size_t)history_slot()] = player_y;
            }
            auto const& follows = f.follow_actions_per_step[std::min((std::size_t)(step - 1), f.follow_actions_per_step.size() - 1)];
            float tick = f.dt * 60.f;
            for (auto const& follow : follows) {
                auto members = f.follow_group_members.find(follow.group);
                if (members == f.follow_group_members.end()) continue;
                float pull_fraction = std::clamp(follow.speed * tick, 0.f, 1.f);
                float max_move = follow.max * tick;
                float icon_y = player_y;
                if (follow.delay > 0.f) {
                    int back = std::clamp((int)std::floor(follow.delay / 0.01f), 0, 199);
                    int at = history_slot() - back;
                    icon_y = f.y_history[(std::size_t)(at < 0 ? at + 200 : at)];
                }
                if (icon_y == 0.f) continue;
                float target = icon_y + (float)follow.offset;
                start_applying();
                for (auto i : members->second) {
                    save_live_fields_once(f, i);
                    if (f.follower_y_stamp[i] != f.follower_y_round) {
                        std::memcpy(&f.follower_y[i], f.live_fields.data() + (std::size_t)i * RECORD_SIZE + RECORD_POSITION_OFFSET + sizeof(double), sizeof(double));
                        f.follower_y_stamp[i] = f.follower_y_round;
                    }
                    float move = pull_fraction * (target - (float)f.follower_y[i]);
                    if (move != 0.f) {
                        if (max_move > 0.f) move = std::clamp(move, -max_move, max_move);
                        f.follower_y[i] += (double)move;
                    }
                    if (f.applied_stamp[i] != f.apply_round) {
                        f.applied_stamp[i] = f.apply_round;
                        f.applied.push_back(i);
                    }
                    auto* obj = f.objects[i];
                    if (obj->m_positionY != f.follower_y[i]) {
                        obj->m_positionY = f.follower_y[i];
                        obj->m_isObjectRectDirty = true;
                        obj->m_isOrientedBoxDirty = true;
                    }
                }
            }
        }
        if (step > f.built_steps) return;
        auto const& frame = f.step_records[(std::size_t)(step - 1)];
        double y_shift = 0.0;
        if (std::isfinite(player_y) && (std::size_t)(step - 1) < f.assumed_player_pos.size()) {
            float assumed = f.assumed_player_pos[(std::size_t)(step - 1)].y;
            if (std::isfinite(assumed)) y_shift = (double)player_y - (double)assumed;
        }
        if (!f.applying) {
            if (frame.empty()) return;
            start_applying();
        }
        auto put_record = [&](unsigned i, unsigned offset) {
            save_live_fields_once(f, i);
            if (f.applied_stamp[i] != f.apply_round) {
                f.applied_stamp[i] = f.apply_round;
                f.applied.push_back(i);
            }
            load_object_fields(f.objects[i], f.record_bytes.data() + offset);
            if (y_shift != 0.0 && f.lock_y[i] == 1) {
                f.objects[i]->m_positionY += y_shift;
                f.objects[i]->m_isObjectRectDirty = true;
                f.objects[i]->m_isOrientedBoxDirty = true;
            }
            play->updateObjectSection(f.objects[i]);
        };
        for (auto [i, offset] : frame) {
            auto* obj = f.objects[i];
            if (record_inside(f, offset, area) || area.containsPoint({ (float)obj->m_positionX, (float)obj->m_positionY })) {
                put_record(i, offset);
                f.pending_offset[i] = UINT_MAX;
            }
            else {
                if (f.pending_offset[i] == UINT_MAX) f.pending_objects.push_back(i);
                f.pending_offset[i] = offset;
            }
        }
        for (std::size_t n = 0; n < f.pending_objects.size();) {
            auto i = f.pending_objects[n];
            auto offset = f.pending_offset[i];
            if (offset != UINT_MAX && !record_inside(f, offset, area)) {
                ++n;
                continue;
            }
            if (offset != UINT_MAX) put_record(i, offset);
            f.pending_offset[i] = UINT_MAX;
            f.pending_objects[n] = f.pending_objects.back();
            f.pending_objects.pop_back();
        }
    }

    void rewind(PlayLayer* play) {
        auto& f = forecast_state();
        ++f.follower_y_round;
        f.y_history_ready = false;
        suspend(play);
    }

    void suspend(PlayLayer* play) {
        auto& f = forecast_state();
        if (!f.applying) return;
        f.applying = false;
        for (auto i : f.pending_objects)
            f.pending_offset[i] = UINT_MAX;
        f.pending_objects.clear();
        for (auto i : f.applied)
            restore_live_fields(f, i, false);
        f.applied.clear();
        f.sections.restore_saved();
    }
}

static Hook* g_section_hook = nullptr;
static bool g_section_hook_on = true;

// same story as the backstep one: a hook on updateObjectSection is paid for on every moving object every step.
// it is only needed while predictions can run w/ moving objects on. coming back on, the forecast is rebuilt
void enable_forecast_section_hook(bool on) {
    if (!g_section_hook || on == g_section_hook_on) return;
    if (on ? g_section_hook->enable().isOk() : g_section_hook->disable().isOk()) {
        g_section_hook_on = on;
        if (on) g_forecast_stale = true;
    }
}

class $modify(ForecastSections, GJBaseGameLayer) {
    static void onModify(auto& self) {
        if (auto hook = self.getHook("GJBaseGameLayer::updateObjectSection"))
            g_section_hook = hook.unwrap();
    }

    void removeObjectFromSection(GameObject* object) {
        auto& f = forecast_state();
        if (f.sections.active && object && f.sections.layer == this) {
            f.sections.save_indexes(object);
            f.sections.save_cell(object->m_outerSectionIndex, object->m_middleSectionIndex);
            if (object == f.sections.updating_object && !f.sections.updating_object_removed) {
                f.sections.updating_object_removed = true;
                f.sections.updating_object_was_queued = m_objectsToDeactivate && m_objectsToDeactivate->objectForKey(object->m_uniqueID);
            }
        }
        GJBaseGameLayer::removeObjectFromSection(object);
    }

    void addToSection(GameObject* object) {
        auto& f = forecast_state();
        if (f.sections.layer == this) f.sections.save_indexes(object);
        GJBaseGameLayer::addToSection(object);
        if (f.sections.active && object && f.sections.layer == this)
            f.sections.save_cell_before_append(object);
    }

    void updateObjectSection(GameObject* object) {
        auto& f = forecast_state();
        if (!f.sections.active || !object || f.sections.layer != this) {
            if (object && !Logic::get().predicting && static_cast<GJBaseGameLayer*>(f.play) == this && f.live_moved_objects.size() < 100000) {
                f.live_moved_objects.insert(object);
            }
            GJBaseGameLayer::updateObjectSection(object);
            return;
        }
        if (f.running_ahead) {
            if (auto it = f.object_index.find(object); it != f.object_index.end()) {
                auto i = it->second;
                if (f.moved_stamp[i] != f.step_round) {
                    f.moved_stamp[i] = f.step_round;
                    f.moved.push_back(i);
                }
            }
        }
        f.sections.save_indexes(object);
        f.sections.updating_object = object;
        f.sections.updating_object_removed = false;
        GJBaseGameLayer::updateObjectSection(object);
        f.sections.updating_object = nullptr;
        auto* queued = m_objectsToDeactivate;
        if (f.sections.updating_object_removed && !f.sections.updating_object_was_queued && queued && queued->objectForKey(object->m_uniqueID))
            f.sections.deactivated_ids.push_back(object->m_uniqueID);
    }
};

class $modify(ForecastMoves, GJBaseGameLayer) {
    static void note_moving(CCArray* objects) {
        auto& f = forecast_state();
        if (!f.running_ahead || !objects) return;
        for (unsigned n = 0; n < objects->count(); ++n) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
            if (!obj) break;
            auto i = ensure_index(f, obj);
            save_live_fields_once(f, i);
            save_node_position(f, i);
            if (f.moved_stamp[i] != f.step_round) {
                f.moved_stamp[i] = f.step_round;
                f.moved.push_back(i);
                f.pre_y[i] = obj->m_positionY;
            }
        }
    }

    void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
        objects = drop_unneeded_decorations(objects);
        note_moving(objects);
        GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
    }

    void rotateObjects(CCArray* objects, float rotation, CCPoint position, CCPoint offset, bool finished, bool unused) {
        objects = drop_unneeded_decorations(objects);
        note_moving(objects);
        GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
    }
};

class $modify(ForecastToggles, GJBaseGameLayer) {
    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (forecast_state().running_ahead) return;
        if (Logic::get().trace_logging && !Logic::get().predicting)
            trace_note(fmt::format("TG frame={} group={} on={} trigger={} state={}\n", Logic::get().step_frame, group, activate, triggerID, (int)Logic::get().state));
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void spawnObject(GameObject* object, double delay, gd::vector<int> const& remapKeys) {
        auto& f = forecast_state();
        if (f.running_ahead) {
            auto* effect = typeinfo_cast<EffectGameObject*>(object);
            if (!effect || !(is_motion_trigger(effect->m_objectID) || effect->m_objectID == 1268))
                return;
            save_trigger_bytes(f, effect);
        }
        GJBaseGameLayer::spawnObject(object, delay, remapKeys);
    }
};

class $modify(ForecastAreas, GJBaseGameLayer) {
    void save_area_objects(CCArray* objects, int outerMin, int outerMax, int middleMin, int middleMax, bool targetGroups) {
        auto& f = forecast_state();
        auto keep = [&f](GameObject* object) {
            if (!object) return;
            save_live_fields_once(f, ensure_index(f, object));
        };
        if (targetGroups && objects) {
            for (unsigned i = 0; i < objects->count(); ++i)
                keep(static_cast<GameObject*>(objects->objectAtIndex(i)));
            return;
        }
        auto& grid = *reinterpret_cast<SectionGrid*>(&m_sections);
        auto& counts = *reinterpret_cast<SectionCounts*>(&m_sectionSizes);
        int last_x = std::min(outerMax, (int)grid.size() - 1);
        for (int x = std::max(outerMin, 0); x <= last_x; ++x) {
            if (!grid[x]) continue;
            int last_y = std::min(middleMax, (int)grid[x]->size() - 1);
            for (int y = std::max(middleMin, 0); y <= last_y; ++y) {
                auto* bucket = bucket_at(grid, x, y);
                auto* amt = count_at(counts, x, y);
                if (!bucket || !amt) continue;
                int used = std::clamp(*amt, 0, (int)bucket->size());
                for (int i = 0; i < used; ++i)
                    keep((*bucket)[i]);
            }
        }
    }

    void processAreaEffects(gd::vector<EnterEffectInstance>* effects, GJAreaActionType type, float dt, bool visibleFrame) {
        if (forecast_state().running_ahead && (type == GJAreaActionType::Fade || type == GJAreaActionType::Tint))
            return;
        GJBaseGameLayer::processAreaEffects(effects, type, dt, visibleFrame);
    }

    void processAreaMoveGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
        if (forecast_state().running_ahead)
            save_area_objects(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
        GJBaseGameLayer::processAreaMoveGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
    }

    void processAreaRotateGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
        if (forecast_state().running_ahead)
            save_area_objects(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
        GJBaseGameLayer::processAreaRotateGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
    }

    void processAreaTransformGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
        if (forecast_state().running_ahead)
            save_area_objects(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
        GJBaseGameLayer::processAreaTransformGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
    }
};

class $modify(ForecastNodes, GameObject) {
    void save_node_look() {
        auto& f = forecast_state();
        if (!f.running_ahead) return;
        auto it = f.object_index.find(this);
        if (it == f.object_index.end()) return;
        auto i = it->second;
        if (f.node_stamp[i] == f.touch_round) return;
        f.node_stamp[i] = f.touch_round;
        save_node_position(f, i);
        f.live_looks[i] = node_look_of(this);
    }

    void setRotation(float rotation) {
        save_node_look();
        GameObject::setRotation(rotation);
    }

    void setRotationX(float rotation) {
        save_node_look();
        GameObject::setRotationX(rotation);
    }

    void setRotationY(float rotation) {
        save_node_look();
        GameObject::setRotationY(rotation);
    }

    void setScaleX(float scale) {
        save_node_look();
        GameObject::setScaleX(scale);
    }

    void setScaleY(float scale) {
        save_node_look();
        GameObject::setScaleY(scale);
    }

    void setOpacity(GLubyte opacity) {
        save_node_look();
        GameObject::setOpacity(opacity);
    }
};

class $modify(ForecastLiveTriggers, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        auto& f = forecast_state();
        if (!f.running_ahead && !Logic::get().predicting && layer && static_cast<GJBaseGameLayer*>(f.play) == layer && (is_motion_trigger(m_objectID) || m_objectID == 1268)) {
            auto ahead = g_fired_build_number.find(m_uniqueID);
            bool known = ahead != g_fired_build_number.end() && ahead->second == f.build_number;
            if (f.valid && !known) g_forecast_stale = true;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

#ifdef GEODE_IS_MACOS
// the 2 things the follow code leans on, checked against gds own code (processPlayerFollowActions reads these offsets)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(GJEffectManager, m_unkVector6f0) == 0x608);
static_assert(offsetof(CCMoveCNode, m_unk0a8) == 0xa8 && offsetof(CCMoveCNode, m_unk0b0) == 0xb0 && offsetof(CCMoveCNode, m_unk0b8) == 0xb8 && offsetof(CCMoveCNode, m_unk0c0) == 0xc0);
#pragma clang diagnostic pop
#endif
