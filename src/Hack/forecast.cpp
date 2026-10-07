// moving objects in the trajectory
//
// the levels move / rotate / follow / scale triggers are run AHEAD of time on a shadow copy of the effect manager
// that gives where every object will be on each future step
// a prediction step puts the nearby objects where they will be, runs, then puts them back
// nothing real is toggled or spawned while forecasting
// the forecast is built a bit per frame (BUILD_PER_FRAME) so it doesnt hitch
#include "Hack/forecast.hpp"
#include "Logic/logic.hpp"
#include "Physics/moving_objects.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EffectManagerState.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/GroupCommandObject2.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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
#include <windows.h>

using namespace geode::prelude;

namespace {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

    struct Span {
        std::uint16_t begin;
        std::uint16_t size;
    };

#define ECHO_SPAN(first, last)                                                                                              \
    Span {                                                                                                                  \
        static_cast<std::uint16_t>(offsetof(GameObject, first)),                                                            \
            static_cast<std::uint16_t>(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)) \
    }
    constexpr std::array SPANS = {
        ECHO_SPAN(m_positionXOffset, m_scaleYOffset),
        ECHO_SPAN(m_isDirty, m_isOrientedBoxDirty),
        ECHO_SPAN(m_unmodifiedPositionX, m_positionY),
        ECHO_SPAN(m_startRotationX, m_customScaleY),
        Span{ static_cast<std::uint16_t>(offsetof(GameObject, m_startFlipX)), 4 },
        ECHO_SPAN(m_scaleX, m_scaleY),
        ECHO_SPAN(m_lastPosition, m_unk4CC),
        Span{ static_cast<std::uint16_t>(offsetof(GameObject, m_useTextureRectForSelection)), 4 },
    };
#undef ECHO_SPAN
#pragma clang diagnostic pop

    constexpr std::size_t STRIDE = [] {
        std::size_t total = 0;
        for (auto const& span : SPANS)
            total += span.size;
        return total;
    }();

    constexpr std::size_t RECORD_POS = [] {
        std::size_t at = 0;
        for (auto const& span : SPANS) {
            if (span.begin <= offsetof(GameObject, m_positionX) && offsetof(GameObject, m_positionX) < span.begin + span.size)
                return at + (offsetof(GameObject, m_positionX) - span.begin);
            at += span.size;
        }
        return std::size_t{ 0 };
    }();

    void gather(GameObject const* object, std::uint8_t* out) {
        auto const* raw = reinterpret_cast<std::uint8_t const*>(object);
        for (auto const& span : SPANS) {
            std::memcpy(out, raw + span.begin, span.size);
            out += span.size;
        }
    }

    void scatter(GameObject* object, std::uint8_t const* in) {
        auto* raw = reinterpret_cast<std::uint8_t*>(object);
        for (auto const& span : SPANS) {
            std::memcpy(raw + span.begin, in, span.size);
            in += span.size;
        }
    }

    struct Node {
        float rotationX, rotationY, scaleX, scaleY;
        GLubyte opacity;
    };

    Node node_of(GameObject* object) {
        return { object->getRotationX(), object->getRotationY(), object->getScaleX(), object->getScaleY(), object->getOpacity() };
    }

    using Bucket = gd::vector<GameObject*>;
    using Column = gd::vector<Bucket*>;
    using Grid = gd::vector<Column*>;
    using Counts = gd::vector<gd::vector<int>*>;

    Bucket* bucket_at(Grid const& grid, int x, int y) {
        if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= grid.size() || !grid[x])
            return nullptr;
        auto& column = *grid[x];
        return static_cast<std::size_t>(y) < column.size() ? column[y] : nullptr;
    }

    int* count_at(Counts const& counts, int x, int y) {
        if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= counts.size() || !counts[x])
            return nullptr;
        auto& column = *counts[x];
        return static_cast<std::size_t>(y) < column.size() ? &column[y] : nullptr;
    }

    struct ListCopy {
        std::vector<GameObject*> items;
        int count = 0;
        bool present = false;
    };

    void copy_list(ListCopy& out, Bucket const* bucket, int const* count) {
        out.present = bucket && count;
        if (!out.present)
            return;
        out.count = *count;
        int used = std::clamp(*count, 0, static_cast<int>(bucket->size()));
        out.items.assign(bucket->begin(), bucket->begin() + used);
    }

    template <class Fix>
    void put_list(ListCopy const& copy, Bucket* bucket, int* count, Fix fix) {
        if (!copy.present || !bucket || !count)
            return;
        if (bucket->size() < copy.items.size())
            bucket->resize(copy.items.size());
        std::copy(copy.items.begin(), copy.items.end(), bucket->begin());
        *count = copy.count;
        for (std::size_t i = 0; i < copy.items.size(); ++i) {
            if (copy.items[i])
                fix(copy.items[i], static_cast<int>(i));
        }
    }

    struct Cell {
        int x = 0, y = 0;
        ListCopy main, non_effect, blocks;
        bool flag = false, flag_present = false;
    };

    struct Sections {
        bool active = false;
        GJBaseGameLayer* layer = nullptr;
        std::unordered_map<std::uint64_t, Cell> cells;
        ListCopy calc_non_effect, calc_blocks;
        std::vector<int> deactivated;
        int moved_count = 0;
        GameObject* probe = nullptr;
        bool probed = false;
        bool probe_had = false;

        static std::uint64_t key(int x, int y) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
        }

        Grid& grid() {
            return *reinterpret_cast<Grid*>(&layer->m_sections);
        }

        Grid& non_effect_grid() {
            return *reinterpret_cast<Grid*>(&layer->m_nonEffectObjects);
        }

        Grid& block_grid() {
            return *reinterpret_cast<Grid*>(&layer->m_collisionBlockSections);
        }

        Counts& counts() {
            return *reinterpret_cast<Counts*>(&layer->m_sectionSizes);
        }

        Counts& non_effect_counts() {
            return *reinterpret_cast<Counts*>(&layer->m_nonEffectObjectsSizes);
        }

        Counts& block_counts() {
            return *reinterpret_cast<Counts*>(&layer->m_collisionBlockSectionSizes);
        }

        void begin(GJBaseGameLayer* play) {
            active = true;
            layer = play;
            cells.clear();
            deactivated.clear();
            copy_list(calc_non_effect, &play->m_calcNonEffectObjects, &play->m_calcNonEffectObjectsSize);
            copy_list(calc_blocks, &play->m_calcCollisionBlockObjects, &play->m_calcCollisionBlockObjectsSize);
            moved_count = play->m_movedCount;
        }

        bool flag_at(int x, int y, bool& present) {
            auto& flags = layer->m_nonEffectObjectsFlags;
            present = x >= 0 && y >= 0 && static_cast<std::size_t>(x) < flags.size() && flags[x] && static_cast<std::size_t>(y) < flags[x]->size();
            return present && (*flags[x])[y];
        }

        Cell* note(int x, int y) {
            if (!active || x < 0 || y < 0)
                return nullptr;
            auto [it, fresh] = cells.try_emplace(key(x, y));
            if (!fresh)
                return nullptr;
            auto& cell = it->second;
            cell.x = x;
            cell.y = y;
            copy_list(cell.main, bucket_at(grid(), x, y), count_at(counts(), x, y));
            copy_list(cell.non_effect, bucket_at(non_effect_grid(), x, y), count_at(non_effect_counts(), x, y));
            copy_list(cell.blocks, bucket_at(block_grid(), x, y), count_at(block_counts(), x, y));
            cell.flag = flag_at(x, y, cell.flag_present);
            return &cell;
        }

        void note_appended(GameObject* object) {
            if (!active)
                return;
            int x = object->m_outerSectionIndex, y = object->m_middleSectionIndex;
            if (cells.contains(key(x, y)))
                return;
            auto* cell = note(x, y);
            if (!cell)
                return;
            auto drop = [object](ListCopy& list) {
                if (list.present && !list.items.empty() && list.items.back() == object) {
                    list.items.pop_back();
                    --list.count;
                }
            };
            drop(cell->main);
            drop(cell->non_effect);
            drop(cell->blocks);
            if (cell->non_effect.present && cell->non_effect.items.empty())
                cell->flag = false;
        }

        void end() {
            if (!active)
                return;
            active = false;
            for (auto& [id, cell] : cells) {
                int x = cell.x, y = cell.y;
                put_list(cell.main, bucket_at(grid(), x, y), count_at(counts(), x, y), [x, y](GameObject* object, int slot) {
                    object->m_someOtherIndex = slot;
                    object->m_outerSectionIndex = x;
                    object->m_middleSectionIndex = y;
                });
                auto side = [](GameObject* object, int slot) {
                    object->m_innerSectionIndex = slot;
                };
                put_list(cell.non_effect, bucket_at(non_effect_grid(), x, y), count_at(non_effect_counts(), x, y), side);
                put_list(cell.blocks, bucket_at(block_grid(), x, y), count_at(block_counts(), x, y), side);
                bool present = false;
                flag_at(x, y, present);
                if (present && cell.flag_present)
                    (*layer->m_nonEffectObjectsFlags[x])[y] = cell.flag;
            }
            auto side = [](GameObject* object, int slot) {
                object->m_innerSectionIndex = slot;
            };
            put_list(calc_non_effect, &layer->m_calcNonEffectObjects, &layer->m_calcNonEffectObjectsSize, side);
            put_list(calc_blocks, &layer->m_calcCollisionBlockObjects, &layer->m_calcCollisionBlockObjectsSize, side);
            if (auto* queued = layer->m_objectsToDeactivate) {
                for (int id : deactivated)
                    queued->removeObjectForKey(id);
            }
            layer->m_movedCount = moved_count;
            cells.clear();
            deactivated.clear();
        }
    };

    constexpr double TOLERANCE = 0.5;
    constexpr int BUILD_PER_FRAME = 30;

    struct Forecast {
        PlayLayer* play = nullptr;
        CCArray* array = nullptr;
        unsigned array_count = 0;
        std::uint64_t epoch = 0;
        std::vector<GameObject*> objects;
        std::unordered_map<GameObject*, std::uint32_t> index;

        GJEffectManager* shadow = nullptr;
        bool valid = false;
        std::uint64_t origin = 0;
        float dt = 0.f;
        int horizon = 0;
        std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> frames;
        std::vector<CCPoint> fired_from;
        std::uint32_t generation = 0;
        std::vector<std::uint8_t> records;
        std::vector<std::uint32_t> latest;
        std::vector<std::uint8_t> quiet, has_quiet;

        std::vector<std::uint8_t> base;
        std::vector<Node> nodes;
        std::vector<std::uint32_t> base_stamp;
        std::uint32_t base_epoch = 1;

        Sections sections;
        std::vector<std::uint32_t> touched;
        std::vector<std::uint32_t> node_stamp, touch_stamp;
        std::uint32_t touch_epoch = 1;
        bool recording = false;
        std::vector<std::uint32_t> moved, moved_stamp;
        std::vector<std::uint32_t> last_offset, last_stamp;
        std::uint32_t last_epoch = 1;
        std::vector<std::uint32_t> last_list;
        std::uint32_t step_epoch = 1;

        int channel = 0;
        int cursor = 0;
        bool goingBack = false;
        std::vector<CCPoint> path;
        CCPoint path_before;
        std::uint64_t path_start = 0;
        CCPoint player_at, player_step;
        int steps_run = 0;

        struct Fired {
            EffectGameObject* object;
            std::vector<std::uint8_t> bytes;
        };

        std::vector<Fired> fired;

        std::unordered_set<GameObject*> live_moved;

        bool applying = false;
        std::vector<std::uint32_t> pending, pending_record;
        std::vector<std::uint32_t> applied, applied_stamp;
        std::uint32_t apply_epoch = 1;
    };

    Forecast& state() {
        static Forecast value;
        return value;
    }

    std::unordered_set<int>& needed_groups() {
        static std::unordered_set<int> groups;
        return groups;
    }

    void note_needed_groups(GJEffectManager* manager) {
        auto& groups = needed_groups();
        groups.clear();
        if (!manager)
            return;
        for (auto const& command : manager->m_unkVector560)
            if (command.m_centerGroupID > 0)
                groups.insert(command.m_centerGroupID);
    }

    CCArray* forecast_members(CCArray* objects) {
        if (!state().recording || !objects)
            return objects;
        auto const& needed = needed_groups();
        auto keep = [&](GameObject* object) {
            if (object->m_objectType != GameObjectType::Decoration)
                return true;
            if (needed.empty() || !object->m_groups)
                return false;
            for (int g = 0; g < object->m_groupCount; ++g)
                if (needed.contains((*object->m_groups)[g]))
                    return true;
            return false;
        };
        unsigned amt = objects->count();
        unsigned first = 0;
        for (; first < amt; ++first) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(first));
            if (!obj)
                return objects;
            if (!keep(obj))
                break;
        }
        if (first == amt)
            return objects;
        static CCArray* kept = [] {
            auto* array = CCArray::create();
            array->retain();
            return array;
        }();
        kept->removeAllObjects();
        for (unsigned n = 0; n < amt; ++n) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
            if (!obj)
                break;
            if (n < first || (n > first && keep(obj)))
                kept->addObject(obj);
        }
        return kept;
    }

    std::uint64_t& live_steps() {
        static std::uint64_t steps = 0;
        return steps;
    }

    bool& stale() {
        static bool flag = true;
        return flag;
    }

    int last_rebuild_step = 0;

    std::unordered_map<int, std::uint32_t> forecast_fired;

    void refresh_objects(Forecast& f, PlayLayer* play) {
        // hopefully fixes crash
        if (f.play == play && f.array == play->m_objects && f.epoch == echo::movement::group_epoch &&
            play->m_objects && f.array_count == play->m_objects->count())
            return;
        f.play = play;
        f.array = play->m_objects;
        f.array_count = play->m_objects ? play->m_objects->count() : 0;
        f.epoch = echo::movement::group_epoch;
        for (auto* obj : f.objects)
            obj->release();
        f.objects.clear();
        f.index.clear();
        f.valid = false;
        if (!play->m_objects)
            return;
        for (unsigned i = 0; i < play->m_objects->count(); ++i) {
            auto* obj = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
            if (!obj || obj->m_groupCount <= 0)
                continue;
            f.index.emplace(obj, static_cast<std::uint32_t>(f.objects.size()));
            obj->retain();
            f.objects.push_back(obj);
        }
        std::size_t amt = f.objects.size();
        f.base.assign(amt * STRIDE, 0);
        f.nodes.assign(amt, {});
        f.base_stamp.assign(amt, 0);
        f.moved_stamp.assign(amt, 0);
        f.applied_stamp.assign(amt, 0);
        f.touch_stamp.assign(amt, 0);
        f.node_stamp.assign(amt, 0);
        f.pending_record.assign(amt, UINT32_MAX);
        f.latest.assign(amt, UINT32_MAX);
        f.has_quiet.assign(amt, 0);
        f.quiet.clear();
    }

    std::uint32_t ensure_index(Forecast& f, GameObject* object) {
        auto [it, fresh] = f.index.try_emplace(object, static_cast<std::uint32_t>(f.objects.size()));
        if (!fresh)
            return it->second;
        object->retain();
        f.objects.push_back(object);
        f.base.resize(f.objects.size() * STRIDE);
        f.nodes.emplace_back();
        f.base_stamp.push_back(0);
        f.moved_stamp.push_back(0);
        f.applied_stamp.push_back(0);
        f.touch_stamp.push_back(0);
        f.node_stamp.push_back(0);
        f.pending_record.push_back(UINT32_MAX);
        f.latest.push_back(UINT32_MAX);
        f.has_quiet.push_back(0);
        return it->second;
    }

    void cache_live(Forecast& f, std::uint32_t i) {
        f.base_stamp[i] = 1;
        gather(f.objects[i], f.base.data() + static_cast<std::size_t>(i) * STRIDE);
        f.nodes[i] = node_of(f.objects[i]);
    }

    void keep_base(Forecast& f, std::uint32_t i) {
        if (!f.base_stamp[i])
            cache_live(f, i);
    }

    void put_base(Forecast& f, std::uint32_t i, bool node) {
        auto* obj = f.objects[i];
        if (node && f.node_stamp[i] == f.touch_epoch) {
            auto const& saved = f.nodes[i];
            if (obj->getRotationX() != saved.rotationX)
                obj->setRotationX(saved.rotationX);
            if (obj->getRotationY() != saved.rotationY)
                obj->setRotationY(saved.rotationY);
            if (obj->getScaleX() != saved.scaleX)
                obj->setScaleX(saved.scaleX);
            if (obj->getScaleY() != saved.scaleY)
                obj->setScaleY(saved.scaleY);
            if (obj->getOpacity() != saved.opacity)
                obj->setOpacity(saved.opacity);
        }
        scatter(obj, f.base.data() + static_cast<std::size_t>(i) * STRIDE);
    }

    void touch(Forecast& f, std::uint32_t i) {
        if (f.touch_stamp[i] == f.touch_epoch)
            return;
        f.touch_stamp[i] = f.touch_epoch;
        f.touched.push_back(i);
    }

    std::uint32_t record(Forecast& f, std::uint32_t i) {
        auto offset = static_cast<std::uint32_t>(f.records.size());
        f.records.resize(f.records.size() + STRIDE);
        gather(f.objects[i], f.records.data() + offset);
        f.latest[i] = offset;
        return offset;
    }

    bool motion_trigger(int id) {
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

    void keep_trigger(Forecast& f, EffectGameObject* object) {
        forecast_fired[object->m_uniqueID] = f.generation;
        if (std::ranges::any_of(f.fired, [object](auto const& fired) {
                return fired.object == object;
            }))
            return;
        auto& fired = f.fired.emplace_back();
        fired.object = object;
        auto const* raw = reinterpret_cast<std::uint8_t const*>(object);
        fired.bytes.assign(raw + sizeof(CCSprite), raw + sizeof(EffectGameObject));
    }

    void fire_ahead(Forecast& f, PlayLayer* play, CCPoint at) {
        auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.channel) : nullptr);
        if (!list)
            return;
        auto* body = play->m_player1;
        bool sideways = body && body->m_isSideways;
        while (static_cast<unsigned>(f.cursor) < list->count()) {
            auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(static_cast<unsigned>(f.cursor)));
            if (!obj) {
                ++f.cursor;
                continue;
            }
            float key = sideways ? obj->m_speedStart.y : obj->m_speedStart.x;
            float player = sideways ? at.y : at.x;
            if (f.goingBack ? key < player : key > player)
                break;
            ++f.cursor;
            bool spawn = obj->m_objectID == 1268;
            if (obj->m_isGroupDisabled || obj->m_isTouchTriggered || !(motion_trigger(obj->m_objectID) || spawn))
                continue;
            keep_trigger(f, obj);
            obj->triggerObject(play, 0, nullptr);
        }
    }

    void compact(Forecast& f) {
        std::size_t used = 0;
        for (auto const& frame : f.frames)
            used += frame.size();
        if (f.records.size() < (used + f.objects.size()) * STRIDE * 2 + (1u << 20))
            return;
        std::vector<std::uint8_t> kept;
        kept.reserve((used + f.objects.size()) * STRIDE);
        std::unordered_map<std::uint32_t, std::uint32_t> moved;
        auto keep = [&](std::uint32_t offset) {
            auto [it, fresh] = moved.try_emplace(offset, static_cast<std::uint32_t>(kept.size()));
            if (fresh)
                kept.insert(kept.end(), f.records.begin() + offset, f.records.begin() + offset + STRIDE);
            return it->second;
        };
        for (auto& frame : f.frames) {
            for (auto& entry : frame)
                entry.second = keep(entry.second);
        }
        for (auto& offset : f.latest) {
            if (offset != UINT32_MAX)
                offset = keep(offset);
        }
        f.records = std::move(kept);
    }

    constexpr std::size_t RECORD_LIMIT = std::size_t{ 192 } << 20;

    CCPoint player_after(Forecast const& f, std::uint64_t absolute, CCPoint node, CCPoint live_step, std::uint64_t live_now) {
        if (!f.path.empty()) {
            if (absolute + 1 == f.path_start)
                return f.path_before;
            if (absolute >= f.path_start) {
                auto idx = absolute - f.path_start;
                if (idx < f.path.size())
                    return f.path[static_cast<std::size_t>(idx)];
                auto last = f.path.back();
                auto step = f.path.size() >= 2 ? last - f.path[f.path.size() - 2] : last - f.path_before;
                return last + step * static_cast<float>(idx - (f.path.size() - 1));
            }
        }
        return node + live_step * (static_cast<float>(absolute) - static_cast<float>(live_now));
    }

    void extend(Forecast& f, PlayLayer* play, int count) {
        auto& logic = Logic::get();
        auto* live = play->m_effectManager;
        auto random_state = GameToolbox::getfast_srand();
        bool predicting = logic.predicting;
        logic.predicting = true;
        // save EVERYTHING that could move first
        ++f.touch_epoch;
        f.touched.clear();
        for (std::uint32_t i = 0; i < f.objects.size(); ++i)
            keep_base(f, i);
        f.sections.begin(play);
        for (std::uint32_t i = 0; i < f.objects.size(); ++i) {
            bool quiet = f.has_quiet[i] && f.quiet.size() >= (static_cast<std::size_t>(i) + 1) * STRIDE;
            if (f.latest[i] == UINT32_MAX && !quiet)
                continue;
            touch(f, i);
            scatter(f.objects[i], quiet ? f.quiet.data() + static_cast<std::size_t>(i) * STRIDE : f.records.data() + f.latest[i]);
            play->updateObjectSection(f.objects[i]);
        }
        auto tuples = play->m_spawnTuples;
        auto* p1 = play->m_player1;
        auto* p2 = play->m_player2;
        auto node1 = p1 ? p1->getPosition() : CCPoint{};
        auto node2 = p2 ? p2->getPosition() : CCPoint{};
        auto camera = play->m_gameState.m_cameraPosition;
        auto camera2 = play->m_gameState.m_cameraPosition2;
        bool sideways = p1 && p1->m_isSideways;
        auto live_now = live_steps();
        auto live_step = p1 ? node1 - p1->m_lastPosition : CCPoint{};
        play->m_effectManager = f.shadow;
        f.recording = true;
        CCPoint fire_from;
        for (int step = 0; step < count && f.records.size() < RECORD_LIMIT; ++step) {
            ++f.step_epoch;
            f.moved.clear();
            play->m_gameState.m_commandIndex += 1;
            ++f.steps_run;
            {
                std::uint64_t absolute = f.origin + static_cast<std::uint64_t>(f.steps_run);
                auto at = player_after(f, absolute - 1, node1, live_step, live_now);
                auto before = player_after(f, absolute - 2, node1, live_step, live_now);
                fire_from = at;
                f.fired_from.push_back(at);
                if (p1)
                    p1->CCNode::setPosition(at);
                if (p2)
                    p2->CCNode::setPosition(node2 + (at - node1));
                f.shadow->m_unk780 = at.x - before.x;
                f.shadow->m_unk784 = at.y - before.y;
                auto ahead = at - node1;
                auto shift = sideways ? CCPoint{ 0.f, ahead.y } : CCPoint{ ahead.x, 0.f };
                play->m_gameState.m_cameraPosition = camera + shift;
                play->m_gameState.m_cameraPosition2 = camera2 + shift;
            }
            fire_ahead(f, play, fire_from);
            f.shadow->updateSpawnTriggers(f.dt);
            note_needed_groups(f.shadow);
            f.shadow->prepareMoveActions(f.dt, false);
            play->processMoveActionsStep(f.dt, true);
            f.shadow->postMoveActions();
            auto& frame = f.frames.emplace_back();
            frame.reserve(f.moved.size());
            for (auto i : f.moved) {
                touch(f, i);
                if (f.objects[i]->m_objectType == GameObjectType::Decoration) {
                    if (f.quiet.size() < f.objects.size() * STRIDE)
                        f.quiet.resize(f.objects.size() * STRIDE);
                    gather(f.objects[i], f.quiet.data() + static_cast<std::size_t>(i) * STRIDE);
                    f.has_quiet[i] = 1;
                    continue;
                }
                frame.emplace_back(i, record(f, i));
            }
        }
        f.recording = false;
        f.shadow->setParent(nullptr);
        play->m_effectManager = live;
        play->m_spawnTuples = tuples;
        if (p1)
            p1->CCNode::setPosition(node1);
        if (p2)
            p2->CCNode::setPosition(node2);
        for (auto it = f.fired.rbegin(); it != f.fired.rend(); ++it)
            std::memcpy(reinterpret_cast<std::uint8_t*>(it->object) + sizeof(CCSprite), it->bytes.data(), it->bytes.size());
        f.fired.clear();
        for (auto i : f.touched)
            put_base(f, i, true);
        f.sections.end();
        GameToolbox::fast_srand(random_state);
        logic.predicting = predicting;
        f.horizon = static_cast<int>(f.frames.size());
    }

    void rebuild(Forecast& f, PlayLayer* play, int steps) {
        f.live_moved.clear();
        f.frames.clear();
        f.records.clear();
        std::fill(f.latest.begin(), f.latest.end(), UINT32_MAX);
        std::fill(f.has_quiet.begin(), f.has_quiet.end(), 0);
        std::fill(f.base_stamp.begin(), f.base_stamp.end(), 0);
        f.horizon = 0;
        f.valid = false;
        f.fired_from.clear();
        ++f.generation;
        auto* live = play->m_effectManager;
        if (!live)
            return;
        EffectManagerState saved;
        live->saveToState(saved);
        f.origin = live_steps();
        f.valid = true;
        stale() = false;
        last_rebuild_step = Logic::get().step_frame;
        auto const& game = play->m_gameState;
        f.channel = game.m_currentChannel;
        auto cursor = game.m_spawnChannelRelated0.find(f.channel);
        f.cursor = cursor == game.m_spawnChannelRelated0.end() ? 0 : cursor->second;
        auto back = game.m_spawnChannelRelated1.find(f.channel);
        f.goingBack = back != game.m_spawnChannelRelated1.end() && back->second;
        f.steps_run = 0;
        if (auto* body = play->m_player1) {
            f.player_at = body->getPosition();
            f.player_step = body->getPosition() - body->m_lastPosition;
        }
        else {
            f.player_at = CCPoint{ 0.f, 0.f };
            f.player_step = CCPoint{ 0.f, 0.f };
        }
        bool ahead = false;
        if (auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.channel) : nullptr)) {
            for (unsigned i = static_cast<unsigned>(std::max(f.cursor, 0)); i < list->count() && !ahead; ++i) {
                auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(i));
                ahead = obj && (motion_trigger(obj->m_objectID) || obj->m_objectID == 1268);
            }
        }
        if (saved.m_vectorGroupCommandObject2.empty() && !ahead) {
            if (f.shadow)
                f.shadow->release();
            f.shadow = nullptr;
            f.frames.resize(static_cast<std::size_t>(steps));
            f.fired_from.resize(f.frames.size(), CCPoint{ NAN, NAN });
            f.horizon = steps;
            return;
        }
        if (f.shadow)
            f.shadow->release();
        f.shadow = GJEffectManager::create();
        if (!f.shadow) {
            f.valid = false;
            return;
        }
        f.shadow->retain();
        f.shadow->m_triggerEffectDelegate = live->m_triggerEffectDelegate;
        f.shadow->loadFromState(saved);
        f.shadow->m_unk780 = live->m_unk780;
        f.shadow->m_unk784 = live->m_unk784;
        f.shadow->m_unk788 = live->m_unk788;
        f.shadow->m_unk78C = live->m_unk78C;
        f.shadow->m_unk790 = live->m_unk790;
        f.shadow->m_unk794 = live->m_unk794;
        f.shadow->m_unk798 = live->m_unk798;
        extend(f, play, std::min(steps, BUILD_PER_FRAME));
    }
}

namespace forecast {
    bool firing() {
        return state().recording;
    }

    void invalidate() {
        stale() = true;
        state().path.clear();
    }

    void note_live_step() {
        ++live_steps();
    }

    void note_player_path(std::vector<CCPoint> path, CCPoint before) {
        auto& f = state();
        f.path = std::move(path);
        f.path_before = before;
        f.path_start = live_steps();
    }

    bool trigger_between(Forecast const& f, PlayLayer* play, float lo, float hi, bool sideways) {
        auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.channel) : nullptr);
        if (!list)
            return false;
        auto key_of = [&](unsigned i) {
            auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(i));
            return obj ? (sideways ? obj->m_speedStart.y : obj->m_speedStart.x) : -INFINITY;
        };
        unsigned low = 0, high = list->count();
        while (low < high) {
            unsigned mid = low + (high - low) / 2;
            if (key_of(mid) <= lo)
                low = mid + 1;
            else
                high = mid;
        }
        for (unsigned i = low; i < list->count() && key_of(i) <= hi; ++i) {
            auto* obj = static_cast<EffectGameObject*>(list->objectAtIndex(i));
            if (obj && (motion_trigger(obj->m_objectID) || obj->m_objectID == 1268))
                return true;
        }
        return false;
    }

    bool player_exact(Forecast const& f, PlayLayer* play) {
        auto const& game = play->m_gameState;
        if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty())
            return true;
        if (!f.shadow)
            return false;
        for (auto const& command : f.shadow->m_unkVector560)
            if (command.m_lockToPlayerX || command.m_lockToPlayerY || command.m_lockToCameraX || command.m_lockToCameraY)
                return true;
        return false;
    }

    void prepare(PlayLayer* play, int steps, float dt) {
        auto& f = state();
        if (!play || steps <= 0 || f.applying)
            return;
        steps = std::max(steps, std::max(32, static_cast<int>(Logic::get().sample_rate() / 15.0)));
        if (Logic::get().backstep_target >= 0 || Logic::get().backstepping) {
            f.valid = false;
            f.live_moved.clear();
            return;
        }
        refresh_objects(f, play);
        if (f.dt != dt)
            stale() = true;
        f.dt = dt;
        std::uint64_t elapsed = live_steps() - f.origin;
        if (f.valid && play->m_player1) {
            auto expected = f.player_at + f.player_step * static_cast<float>(elapsed);
            bool onPath = live_steps() >= f.path_start && live_steps() - f.path_start < f.path.size();
            if (onPath)
                expected = f.path[static_cast<std::size_t>(live_steps() - f.path_start)];
            auto live = play->m_player1->getPosition();
            auto step = live - play->m_player1->m_lastPosition;
            bool sideways = play->m_player1->m_isSideways;
            float off = sideways ? std::abs(expected.y - live.y) : std::abs(expected.x - live.x);
            float speed = onPath ? 0.f : sideways ? std::abs(step.y - f.player_step.y)
                                                  : std::abs(step.x - f.player_step.x);
            if (play->m_isPlatformer ? off > 30.f : off > 0.5f || speed > 1e-3f)
                stale() = true;
        }
        if (f.valid && !stale() && elapsed > 0 && elapsed <= static_cast<std::uint64_t>(f.frames.size())) {
            if (f.last_stamp.size() < f.objects.size()) {
                f.last_stamp.resize(f.objects.size(), 0);
                f.last_offset.resize(f.objects.size(), 0);
            }
            ++f.last_epoch;
            f.last_list.clear();
            for (std::uint64_t k = 0; k < elapsed; ++k)
                for (auto [i, offset] : f.frames[k]) {
                    if (f.last_stamp[i] != f.last_epoch) {
                        f.last_stamp[i] = f.last_epoch;
                        f.last_list.push_back(i);
                    }
                    f.last_offset[i] = offset;
                }
            bool wrong = false;
            for (auto i : f.last_list) {
                double px = 0, py = 0;
                std::memcpy(&px, f.records.data() + f.last_offset[i] + RECORD_POS, sizeof(double));
                std::memcpy(&py, f.records.data() + f.last_offset[i] + RECORD_POS + sizeof(double), sizeof(double));
                auto* obj = f.objects[i];
                if (std::abs(px - obj->m_positionX) > TOLERANCE || std::abs(py - obj->m_positionY) > TOLERANCE) {
                    wrong = true;
                    break;
                }
            }
            for (auto* obj : f.live_moved) {
                if (wrong)
                    break;
                auto it = f.index.find(obj);
                if (it != f.index.end() && it->second < f.last_stamp.size() && f.last_stamp[it->second] == f.last_epoch)
                    continue;
                if (obj->m_objectType == GameObjectType::Decoration)
                    continue;
                wrong = true;
            }
            if (wrong)
                stale() = true;
        }
        for (auto* obj : f.live_moved) {
            if (auto it = f.index.find(obj); it != f.index.end() && f.base_stamp[it->second])
                cache_live(f, it->second);
        }
        f.live_moved.clear();
        for (std::uint32_t i = 0; i < f.latest.size(); ++i) {
            if ((f.latest[i] != UINT32_MAX || f.has_quiet[i]) && f.base_stamp[i])
                cache_live(f, i);
        }
        if (f.valid && !stale() && !f.path.empty() && play->m_player1) {
            bool sideways = play->m_player1->m_isSideways;
            bool exact = player_exact(f, play);
            std::size_t built = std::min(f.fired_from.size(), f.frames.size());
            for (std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(elapsed, built)); k < built; ++k) {
                auto was = f.fired_from[k];
                if (std::isnan(was.x))
                    continue;
                std::uint64_t absolute = f.origin + k;
                if (absolute + 1 < f.path_start || absolute >= f.path_start + f.path.size())
                    continue;
                auto now = absolute + 1 == f.path_start ? f.path_before : f.path[static_cast<std::size_t>(absolute - f.path_start)];
                float travel = sideways ? std::abs(now.y - was.y) : std::abs(now.x - was.x);
                bool differs = exact ? std::max(std::abs(now.x - was.x), std::abs(now.y - was.y)) > 0.01f
                                     : travel > 0.01f && trigger_between(f, play, sideways ? std::min(now.y, was.y) : std::min(now.x, was.x), sideways ? std::max(now.y, was.y) : std::max(now.x, was.x), sideways);
                if (!differs)
                    continue;
                stale() = true;
                break;
            }
        }
        bool old = Logic::get().step_frame - last_rebuild_step > static_cast<int>(Logic::get().sample_rate() * 30.0);
        if (stale() || !f.valid || old || elapsed > static_cast<std::uint64_t>(f.horizon)) {
            rebuild(f, play, steps);
            return;
        }
        if (elapsed > 0) {
            f.frames.erase(f.frames.begin(), f.frames.begin() + static_cast<std::ptrdiff_t>(elapsed));
            f.fired_from.erase(f.fired_from.begin(), f.fired_from.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(elapsed, f.fired_from.size())));
            f.player_at = f.player_at + f.player_step * static_cast<float>(elapsed);
            f.steps_run -= static_cast<int>(elapsed);
            f.origin = live_steps();
            f.horizon = static_cast<int>(f.frames.size());
            compact(f);
        }
        if (f.horizon < steps) {
            if (!f.shadow || f.frames.empty() && f.records.empty()) {
                f.frames.resize(static_cast<std::size_t>(steps));
                f.fired_from.resize(f.frames.size(), CCPoint{ NAN, NAN });
                f.horizon = steps;
                return;
            }
            int target = steps;
            if (!f.path.empty() && f.path_start + f.path.size() > f.origin) {
                int covered = static_cast<int>(f.path_start + f.path.size() - f.origin);
                int lead = std::max(16, static_cast<int>(Logic::get().sample_rate() / 30.0));
                target = std::min(steps, std::max(covered + (player_exact(f, play) ? 0 : 8), lead));
            }
            if (f.horizon < target)
                extend(f, play, std::min(target - f.horizon, BUILD_PER_FRAME));
        }
    }

    namespace {
        bool record_inside(Forecast const& f, std::uint32_t offset, CCRect const& area) {
            double px = 0, py = 0;
            std::memcpy(&px, f.records.data() + offset + RECORD_POS, sizeof(double));
            std::memcpy(&py, f.records.data() + offset + RECORD_POS + sizeof(double), sizeof(double));
            return area.containsPoint({ static_cast<float>(px), static_cast<float>(py) });
        }
    }

    void apply(PlayLayer* play, int step, CCRect const& area) {
        auto& f = state();
        if (!play || play != f.play || !f.valid || step < 1 || step > f.horizon)
            return;
        auto const& frame = f.frames[static_cast<std::size_t>(step - 1)];
        if (!f.applying) {
            if (frame.empty())
                return;
            f.applying = true;
            ++f.apply_epoch;
            f.applied.clear();
            f.pending.clear();
            f.sections.begin(play);
        }
        auto put = [&](std::uint32_t i, std::uint32_t offset) {
            keep_base(f, i);
            if (f.applied_stamp[i] != f.apply_epoch) {
                f.applied_stamp[i] = f.apply_epoch;
                f.applied.push_back(i);
            }
            scatter(f.objects[i], f.records.data() + offset);
            play->updateObjectSection(f.objects[i]);
        };
        for (auto [i, offset] : frame) {
            auto* obj = f.objects[i];
            if (record_inside(f, offset, area) || area.containsPoint({ static_cast<float>(obj->m_positionX), static_cast<float>(obj->m_positionY) })) {
                put(i, offset);
                f.pending_record[i] = UINT32_MAX;
            }
            else {
                if (f.pending_record[i] == UINT32_MAX)
                    f.pending.push_back(i);
                f.pending_record[i] = offset;
            }
        }
        for (std::size_t n = 0; n < f.pending.size();) {
            auto i = f.pending[n];
            auto offset = f.pending_record[i];
            if (offset != UINT32_MAX && !record_inside(f, offset, area)) {
                ++n;
                continue;
            }
            if (offset != UINT32_MAX)
                put(i, offset);
            f.pending_record[i] = UINT32_MAX;
            f.pending[n] = f.pending.back();
            f.pending.pop_back();
        }
    }

    void rewind(PlayLayer* play) {
        auto& f = state();
        if (!f.applying)
            return;
        f.applying = false;
        for (auto i : f.pending)
            f.pending_record[i] = UINT32_MAX;
        f.pending.clear();
        for (auto i : f.applied)
            put_base(f, i, false);
        f.applied.clear();
        f.sections.end();
    }
}

class $modify(ForecastSections, GJBaseGameLayer) {
    void removeObjectFromSection(GameObject* object) {
        auto& f = state();
        if (f.sections.active && object && f.sections.layer == this) {
            f.sections.note(object->m_outerSectionIndex, object->m_middleSectionIndex);
            if (object == f.sections.probe && !f.sections.probed) {
                f.sections.probed = true;
                f.sections.probe_had = m_objectsToDeactivate && m_objectsToDeactivate->objectForKey(object->m_uniqueID);
            }
        }
        GJBaseGameLayer::removeObjectFromSection(object);
    }

    void addToSection(GameObject* object) {
        GJBaseGameLayer::addToSection(object);
        auto& f = state();
        if (f.sections.active && object && f.sections.layer == this)
            f.sections.note_appended(object);
    }

    void updateObjectSection(GameObject* object) {
        auto& f = state();
        if (!f.sections.active || !object || f.sections.layer != this) {
            if (object && !Logic::get().predicting && static_cast<GJBaseGameLayer*>(f.play) == this && f.live_moved.size() < 100000) {
                f.live_moved.insert(object);
            }
            GJBaseGameLayer::updateObjectSection(object);
            return;
        }
        if (f.recording) {
            if (auto it = f.index.find(object); it != f.index.end()) {
                auto i = it->second;
                if (f.moved_stamp[i] != f.step_epoch) {
                    f.moved_stamp[i] = f.step_epoch;
                    f.moved.push_back(i);
                }
            }
        }
        f.sections.probe = object;
        f.sections.probed = false;
        GJBaseGameLayer::updateObjectSection(object);
        f.sections.probe = nullptr;
        auto* queued = m_objectsToDeactivate;
        if (f.sections.probed && !f.sections.probe_had && queued && queued->objectForKey(object->m_uniqueID))
            f.sections.deactivated.push_back(object->m_uniqueID);
    }
};

// while forecasting nothing real gets toggled
class $modify(ForecastMoves, GJBaseGameLayer) {
    static void note_moving(CCArray* objects) {
        auto& f = state();
        if (!f.recording || !objects)
            return;
        for (unsigned n = 0; n < objects->count(); ++n) {
            auto* obj = static_cast<GameObject*>(objects->objectAtIndex(n));
            if (!obj)
                break;
            auto i = ensure_index(f, obj);
            keep_base(f, i);
            if (f.moved_stamp[i] != f.step_epoch) {
                f.moved_stamp[i] = f.step_epoch;
                f.moved.push_back(i);
            }
        }
    }

    void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
        objects = forecast_members(objects);
        note_moving(objects);
        GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
    }

    void rotateObjects(CCArray* objects, float rotation, CCPoint position, CCPoint offset, bool finished, bool unused) {
        objects = forecast_members(objects);
        note_moving(objects);
        GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
    }
};

class $modify(ForecastToggles, GJBaseGameLayer) {
    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (state().recording)
            return;
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void spawnObject(GameObject* object, double delay, gd::vector<int> const& remapKeys) {
        auto& f = state();
        if (f.recording) {
            auto* effect = typeinfo_cast<EffectGameObject*>(object);
            if (!effect || !(motion_trigger(effect->m_objectID) || effect->m_objectID == 1268))
                return;
            keep_trigger(f, effect);
        }
        GJBaseGameLayer::spawnObject(object, delay, remapKeys);
    }
};

class $modify(ForecastAreas, GJBaseGameLayer) {
    void keep_area(CCArray* objects, int outerMin, int outerMax, int middleMin, int middleMax, bool targetGroups) {
        auto& f = state();
        auto keep = [&f](GameObject* object) {
            if (!object)
                return;
            keep_base(f, ensure_index(f, object));
        };
        if (targetGroups && objects) {
            for (unsigned i = 0; i < objects->count(); ++i)
                keep(static_cast<GameObject*>(objects->objectAtIndex(i)));
            return;
        }
        auto& grid = *reinterpret_cast<Grid*>(&m_sections);
        auto& counts = *reinterpret_cast<Counts*>(&m_sectionSizes);
        for (int x = std::max(outerMin, 0); x <= outerMax; ++x) {
            for (int y = std::max(middleMin, 0); y <= middleMax; ++y) {
                auto* bucket = bucket_at(grid, x, y);
                auto* amt = count_at(counts, x, y);
                if (!bucket || !amt)
                    continue;
                int used = std::clamp(*amt, 0, static_cast<int>(bucket->size()));
                for (int i = 0; i < used; ++i)
                    keep((*bucket)[i]);
            }
        }
    }

    void processAreaEffects(gd::vector<EnterEffectInstance>* effects, GJAreaActionType type, float dt, bool visibleFrame) {
        if (state().recording && (type == GJAreaActionType::Fade || type == GJAreaActionType::Tint))
            return;
        GJBaseGameLayer::processAreaEffects(effects, type, dt, visibleFrame);
    }

    void processAreaMoveGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
        if (state().recording)
            keep_area(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
        GJBaseGameLayer::processAreaMoveGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
    }

    void processAreaRotateGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
        if (state().recording)
            keep_area(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
        GJBaseGameLayer::processAreaRotateGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
    }

    void processAreaTransformGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
        if (state().recording)
            keep_area(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
        GJBaseGameLayer::processAreaTransformGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
    }
};

class $modify(ForecastNodes, GameObject) {
    void keep_node() {
        auto& f = state();
        if (!f.recording)
            return;
        auto it = f.index.find(this);
        if (it == f.index.end())
            return;
        auto i = it->second;
        if (f.node_stamp[i] == f.touch_epoch)
            return;
        f.node_stamp[i] = f.touch_epoch;
        f.nodes[i] = node_of(this);
    }

    void setRotation(float rotation) {
        keep_node();
        GameObject::setRotation(rotation);
    }

    void setRotationX(float rotation) {
        keep_node();
        GameObject::setRotationX(rotation);
    }

    void setRotationY(float rotation) {
        keep_node();
        GameObject::setRotationY(rotation);
    }

    void setScaleX(float scale) {
        keep_node();
        GameObject::setScaleX(scale);
    }

    void setScaleY(float scale) {
        keep_node();
        GameObject::setScaleY(scale);
    }

    void setOpacity(GLubyte opacity) {
        keep_node();
        GameObject::setOpacity(opacity);
    }
};

class $modify(ForecastLiveTriggers, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        auto& f = state();
        if (!f.recording && !Logic::get().predicting && layer && static_cast<GJBaseGameLayer*>(f.play) == layer && (motion_trigger(m_objectID) || m_objectID == 1268)) {
            auto ahead = forecast_fired.find(m_uniqueID);
            bool known = ahead != forecast_fired.end() && ahead->second == f.generation;
            if (f.valid && !known)
                stale() = true;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};
