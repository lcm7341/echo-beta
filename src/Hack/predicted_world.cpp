#include "Hack/predicted_world.hpp"
#include "Logic/logic.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/moving_objects.hpp"
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EffectManagerState.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/CCMoveCNode.hpp>
#include <Geode/binding/GroupCommandObject2.hpp>
#include <Geode/binding/DynamicObjectAction.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/SequenceTriggerState.hpp>
#include <Geode/binding/SequenceTriggerGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/loader/Priority.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#elif defined(GEODE_IS_ANDROID)
#include <fstream>
#elif defined(GEODE_IS_MACOS)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#include <algorithm>
#include <chrono>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <utility>

using namespace geode::prelude;

#define POSITION_TOLERANCE 0.3
#define TIMELINE_SECONDS 30.0
#define VARINT_BASE (128)
#define ENTRY_FLAG_VALUES (2)
#define STEPS_PER_SECOND (60.f)
#define MAX_RECORD_BYTES 512
#define TRIGGERED_IDS_OFFSET 0x558
#define GAME_EVENT_LINKS_OFFSET 0x588
#define GAME_EVENT_ROUNDS_OFFSET 0x598

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

struct FieldSpan {
	unsigned short begin;
	unsigned short size;
};

#define OBJECT_FIELD_SPAN(first, last) \
	FieldSpan { (unsigned short)offsetof(GameObject, first), (unsigned short)(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)) }

static const std::array SAVED_FIELD_SPANS = {
	OBJECT_FIELD_SPAN(m_positionXOffset, m_scaleYOffset),
	OBJECT_FIELD_SPAN(m_isDirty, m_isOrientedBoxDirty),
	OBJECT_FIELD_SPAN(m_unmodifiedPositionX, m_positionY),
	OBJECT_FIELD_SPAN(m_startRotationX, m_customScaleY),
	FieldSpan{ (unsigned short)offsetof(GameObject, m_startFlipX), 4 },
	OBJECT_FIELD_SPAN(m_scaleX, m_scaleY),
	OBJECT_FIELD_SPAN(m_lastPosition, m_unk4CC),
	FieldSpan{ (unsigned short)offsetof(GameObject, m_useTextureRectForSelection), 4 },
	FieldSpan{ (unsigned short)offsetof(GameObject, m_isDisabled2), 1 },
	FieldSpan{ (unsigned short)offsetof(GameObject, m_isDisabled), 1 },
	FieldSpan{ (unsigned short)offsetof(GameObject, m_isGroupDisabled), 2 },
	FieldSpan{ (unsigned short)offsetof(GameObject, m_enabledGroupsCounter), 4 },
};
#undef OBJECT_FIELD_SPAN

#define COMPACT_FIRST_SPAN 0
#define POSITION_SPAN 2
#define NODE_FLOATS 4

static const std::size_t COMPACT_POSITION_OFFSET = [] {
	std::size_t before = 0;
	for (std::size_t i = COMPACT_FIRST_SPAN; i < POSITION_SPAN; ++i)
		before += SAVED_FIELD_SPANS[i].size;
	return before + offsetof(GameObject, m_positionX) - SAVED_FIELD_SPANS[POSITION_SPAN].begin;
}();
#pragma clang diagnostic pop

static const std::size_t RECORD_SIZE = [] {
	std::size_t total = NODE_FLOATS * sizeof(float);
	for (const auto& span : SAVED_FIELD_SPANS)
		total += span.size;
	return total;
}();

static const std::size_t COMPACT_SIZE = [] {
	std::size_t total = NODE_FLOATS * sizeof(float);
	for (std::size_t i = COMPACT_FIRST_SPAN; i < SAVED_FIELD_SPANS.size(); ++i)
		total += SAVED_FIELD_SPANS[i].size;
	return total;
}();

static void save_node_fields(const GameObject* object, unsigned char* out) {
	auto* node = const_cast<GameObject*>(object);
	float values[NODE_FLOATS] = { node->getRotationX(), node->getRotationY(), node->getScaleX(), node->getScaleY() };
	std::memcpy(out, values, sizeof(values));
}

static void load_node_fields(GameObject* object, const unsigned char* in) {
	float values[NODE_FLOATS];
	std::memcpy(values, in, sizeof(values));
	if (object->getRotationX() != values[0]) object->setRotationX(values[0]);
	if (object->getRotationY() != values[1]) object->setRotationY(values[1]);
	if (object->getScaleX() != values[2]) object->setScaleX(values[2]);
	if (object->getScaleY() != values[3]) object->setScaleY(values[3]);
}

static void save_sequence(SequenceTriggerGameObject* sequence, SequenceProgress& out) {
	if (!sequence) return;
	out.times.clear();
	out.indices.clear();
	for (const auto& [key, time] : sequence->m_sequenceState.m_sequenceTimes)
		out.times.push_back({ key, time });
	for (const auto& [key, index] : sequence->m_sequenceState.m_sequenceIndices)
		out.indices.push_back({ key, index });
}

static void load_sequence(SequenceTriggerGameObject* sequence, const SequenceProgress& saved) {
	if (!sequence) return;
	SequenceProgress now;
	save_sequence(sequence, now);
	if (now.times == saved.times && now.indices == saved.indices) return;
	auto& state = sequence->m_sequenceState;
	state.m_sequenceTimes.clear();
	for (const auto& [key, time] : saved.times)
		state.m_sequenceTimes[key] = time;
	state.m_sequenceIndices.clear();
	for (const auto& [key, index] : saved.indices)
		state.m_sequenceIndices[key] = index;
}

static const std::size_t COMPACT_WORDS = (COMPACT_SIZE + sizeof(unsigned) - 1) / sizeof(unsigned);
static const std::size_t COMPACT_STRIDE = COMPACT_WORDS * sizeof(unsigned);

static unsigned long long changed_words(const unsigned char* now, const unsigned char* before) {
	unsigned long long mask = 0;
	for (std::size_t word = 0; word < COMPACT_WORDS; ++word)
		if (std::memcmp(now + word * sizeof(unsigned), before + word * sizeof(unsigned), sizeof(unsigned)) != 0)
			mask |= 1ull << word;
	return mask;
}

static void write_entry_start(std::vector<unsigned char>& out, unsigned slot_step, bool new_mask) {
	unsigned long long value = (unsigned long long)slot_step * ENTRY_FLAG_VALUES + (new_mask ? 1 : 0);
	while (value >= VARINT_BASE) {
		out.push_back((unsigned char)(value % VARINT_BASE + VARINT_BASE));
		value /= VARINT_BASE;
	}
	out.push_back((unsigned char)value);
}

static const unsigned char* read_entry_start(const unsigned char* in, unsigned& slot, bool& new_mask) {
	unsigned long long value = 0, scale = 1;
	while (*in >= VARINT_BASE) {
		value += (unsigned long long)(*in++ - VARINT_BASE) * scale;
		scale *= VARINT_BASE;
	}
	value += (unsigned long long)*in++ * scale;
	slot += (unsigned)(value / ENTRY_FLAG_VALUES);
	new_mask = value % ENTRY_FLAG_VALUES == 1;
	return in;
}

static const unsigned char* read_entry_fields(const unsigned char* in, bool new_mask, unsigned long long& mask, unsigned char* compact) {
	if (new_mask) {
		std::memcpy(&mask, in, sizeof(mask));
		in += sizeof(mask);
	}
	for (unsigned long long words = mask; words; words &= words - 1) {
		std::memcpy(compact + (std::size_t)std::countr_zero(words) * sizeof(unsigned), in, sizeof(unsigned));
		in += sizeof(unsigned);
	}
	return in;
}

static void save_object_fields(const GameObject* object, unsigned char* out) {
	const auto* raw = reinterpret_cast<const unsigned char*>(object);
	for (const auto& span : SAVED_FIELD_SPANS) {
		std::memcpy(out, raw + span.begin, span.size);
		out += span.size;
	}
	save_node_fields(object, out);
}

static bool node_fields_differ(GameObject* object, const unsigned char* in) {
	float values[NODE_FLOATS];
	std::memcpy(values, in, sizeof(values));
	return object->getRotationX() != values[0] || object->getRotationY() != values[1] || object->getScaleX() != values[2] || object->getScaleY() != values[3];
}

static const unsigned char* load_span_fields(GameObject* object, const unsigned char* in) {
	auto* raw = reinterpret_cast<unsigned char*>(object);
	for (const auto& span : SAVED_FIELD_SPANS) {
		std::memcpy(raw + span.begin, in, span.size);
		in += span.size;
	}
	return in;
}

static void load_object_fields(GameObject* object, const unsigned char* in) {
	load_node_fields(object, load_span_fields(object, in));
	object->m_isObjectRectDirty = true;
	object->m_isOrientedBoxDirty = true;
}

#ifdef ECHO_THREADED_MOVE
#define WORLD_PARTS MovingObjects::MoveThreads::PARTS
#else
#define WORLD_PARTS 1
#endif

template <class Work>
static void run_in_parts(std::size_t count, Work&& work) {
#ifdef ECHO_THREADED_MOVE
	if (count >= MovingObjects::threaded_minimum) {
		auto job = [&](unsigned part) {
			work(count * part / WORLD_PARTS, count * (part + 1) / WORLD_PARTS, part);
		};
		MovingObjects::MoveThreads::get().run(job);
		return;
	}
#endif
	work(0, count, 0);
}

static void save_compact_fields(const GameObject* object, unsigned char* out) {
	const auto* raw = reinterpret_cast<const unsigned char*>(object);
	for (std::size_t i = COMPACT_FIRST_SPAN; i < SAVED_FIELD_SPANS.size(); ++i) {
		std::memcpy(out, raw + SAVED_FIELD_SPANS[i].begin, SAVED_FIELD_SPANS[i].size);
		out += SAVED_FIELD_SPANS[i].size;
	}
	save_node_fields(object, out);
}

static void load_compact_fields(GameObject* object, const unsigned char* in) {
	auto* raw = reinterpret_cast<unsigned char*>(object);
	for (std::size_t i = COMPACT_FIRST_SPAN; i < SAVED_FIELD_SPANS.size(); ++i) {
		std::memcpy(raw + SAVED_FIELD_SPANS[i].begin, in, SAVED_FIELD_SPANS[i].size);
		in += SAVED_FIELD_SPANS[i].size;
	}
	load_node_fields(object, in);
	object->m_isObjectRectDirty = true;
	object->m_isOrientedBoxDirty = true;
}

static CCPoint compact_position(const unsigned char* compact) {
	double x = 0, y = 0;
	std::memcpy(&x, compact + COMPACT_POSITION_OFFSET, sizeof(double));
	std::memcpy(&y, compact + COMPACT_POSITION_OFFSET + sizeof(double), sizeof(double));
	return { (float)x, (float)y };
}

using SectionBucket = gd::vector<GameObject*>;
using SectionColumn = gd::vector<SectionBucket*>;
using SectionGrid = gd::vector<SectionColumn*>;
using SectionCounts = gd::vector<gd::vector<int>*>;

static SectionBucket* bucket_at(SectionGrid& grid, int x, int y) {
	if (x < 0 || y < 0 || (std::size_t)x >= grid.size() || !grid[(std::size_t)x]) return nullptr;
	auto& column = *grid[(std::size_t)x];
	return (std::size_t)y < column.size() ? column[(std::size_t)y] : nullptr;
}

static int* count_at(SectionCounts& counts, int x, int y) {
	if (x < 0 || y < 0 || (std::size_t)x >= counts.size() || !counts[(std::size_t)x]) return nullptr;
	auto& column = *counts[(std::size_t)x];
	return (std::size_t)y < column.size() ? &column[(std::size_t)y] : nullptr;
}

static SectionGrid& main_grid(GJBaseGameLayer* layer) {
	return *reinterpret_cast<SectionGrid*>(&layer->m_sections);
}

static SectionGrid& non_effect_grid(GJBaseGameLayer* layer) {
	return *reinterpret_cast<SectionGrid*>(&layer->m_nonEffectObjects);
}

static SectionGrid& block_grid(GJBaseGameLayer* layer) {
	return *reinterpret_cast<SectionGrid*>(&layer->m_collisionBlockSections);
}

static SectionCounts& main_counts(GJBaseGameLayer* layer) {
	return *reinterpret_cast<SectionCounts*>(&layer->m_sectionSizes);
}

static SectionCounts& non_effect_counts(GJBaseGameLayer* layer) {
	return *reinterpret_cast<SectionCounts*>(&layer->m_nonEffectObjectsSizes);
}

static SectionCounts& block_counts(GJBaseGameLayer* layer) {
	return *reinterpret_cast<SectionCounts*>(&layer->m_collisionBlockSectionSizes);
}

static long long cell_key(int x, int y) {
	return ((long long)x << 32) | (unsigned)y;
}

static bool far_away(GameObject* object) {
	const double LIMIT = 10000000.;
	return !std::isfinite(object->m_positionX) || !std::isfinite(object->m_positionY) || std::fabs(object->m_positionX) >= LIMIT || std::fabs(object->m_positionY) >= LIMIT;
}

static bool section_of(GJBaseGameLayer* layer, GameObject* object, int& x, int& y) {
	return MovingObjects::section_index(object->m_positionX, layer->m_sectionXFactor, x) &&
		MovingObjects::section_index(object->m_positionY, layer->m_sectionYFactor, y);
}

template <class List>
static void save_list(List& out, const SectionBucket* bucket, const int* count) {
	out.present = bucket && count;
	if (!out.present) return;
	out.count = *count;
	int used = std::clamp(*count, 0, (int)bucket->size());
	out.objects.assign(bucket->begin(), bucket->begin() + used);
}

template <class List, class FixIndex>
static void restore_list(const List& saved, SectionBucket* bucket, int* count, FixIndex fix_index) {
	if (!bucket || !count) return;
	if (!saved.present) {
		*count = 0;
		return;
	}
	if (bucket->size() < saved.objects.size()) bucket->resize(saved.objects.size());
	std::copy(saved.objects.begin(), saved.objects.end(), bucket->begin());
	*count = saved.count;
	for (std::size_t i = 0; i < saved.objects.size(); ++i)
		if (saved.objects[i]) fix_index(saved.objects[i], (int)i);
}

static void set_inner_index(GameObject* object, int index) {
	object->m_innerSectionIndex = index;
}

static std::vector<Hook*> g_world_hooks;

void PredictedWorld::rebuild_slots(PlayLayer* play) {
	unsigned amount = play->m_objects ? play->m_objects->count() : 0;
	if (play == slots_play && amount == slots_count) return;
	slots_play = play;
	slots_count = amount;
	int max_uid = 0;
	auto* objects = play->m_objects;
	for (unsigned i = 0; objects && i < objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
		if (object) max_uid = std::max(max_uid, object->m_uniqueID);
	}
	if (slot_of_uid.size() <= (std::size_t)max_uid) {
		slot_of_uid.resize((std::size_t)max_uid + 1, 0);
		slot_round.resize((std::size_t)max_uid + 1, 0);
	}
}

unsigned PredictedWorld::slot(GameObject* object) {
	auto uid = (std::size_t)std::max(object->m_uniqueID, 0);
	if (uid >= slot_of_uid.size()) {
		slot_of_uid.resize(uid + 1, 0);
		slot_round.resize(uid + 1, 0);
	}
	if (slot_round[uid] == round) return slot_of_uid[uid];
	return UINT_MAX;
}

unsigned PredictedWorld::touch(GameObject* object) {
	unsigned at = slot(object);
	if (at == UINT_MAX) {
		auto uid = (std::size_t)std::max(object->m_uniqueID, 0);
		at = (unsigned)tracked.size();
		slot_of_uid[uid] = at;
		slot_round[uid] = round;
		auto& entry = tracked.emplace_back();
		entry.object = object;
		live_fields.resize(live_fields.size() + RECORD_SIZE);
		horizon_fields.resize(horizon_fields.size() + RECORD_SIZE);
		latest_fields.resize(latest_fields.size() + COMPACT_STRIDE);
		expected_fields.resize(expected_fields.size() + COMPACT_STRIDE);
		latest_masks.push_back(0);
		expected_masks.push_back(0);
		save_object_fields(object, horizon_fields.data() + (std::size_t)at * RECORD_SIZE);
		save_compact_fields(object, latest_fields.data() + (std::size_t)at * COMPACT_STRIDE);
		save_compact_fields(object, expected_fields.data() + (std::size_t)at * COMPACT_STRIDE);
	}
	touch_session(at);
	mark_changed(at);
	return at;
}

void PredictedWorld::touch_session(unsigned at) {
	if (join_session(at)) session_list.push_back(at);
}

bool PredictedWorld::join_session(unsigned at) {
	auto& entry = tracked[at];
	entry.dormant = false;
	if (entry.session == session_round) return false;
	entry.session = session_round;
	auto* object = entry.object;
	save_object_fields(object, live_fields.data() + (std::size_t)at * RECORD_SIZE);
	auto& look = entry.look;
	look.rotation_x = object->getRotationX();
	look.rotation_y = object->getRotationY();
	look.scale_x = object->getScaleX();
	look.scale_y = object->getScaleY();
	look.opacity = object->getOpacity();
	look.node_position = object->CCNode::getPosition();
	if (object->m_colorSprite) look.detail_position = object->m_colorSprite->getPosition();
	entry.indexes_saved = false;
	entry.applied = false;
	entry.pending = false;
	return true;
}

void PredictedWorld::save_group(GJBaseGameLayer* layer, int group) {
	if (group <= 0 || (std::size_t)group >= layer->m_groups.size()) return;
	if (saved_groups.size() < layer->m_groups.size()) saved_groups.resize(layer->m_groups.size(), 0);
	if (saved_groups[(std::size_t)group] == round) return;
	saved_groups[(std::size_t)group] = round;
	auto* members = layer->m_groups[(std::size_t)group];
	for (unsigned i = 0; members && i < members->count(); ++i) {
		auto* object = static_cast<GameObject*>(members->objectAtIndex(i));
		if (object) touch(object);
	}
}

void PredictedWorld::mark_changed(unsigned at) {
	tracked[at].changed_step = horizon_step + 1;
	if (changed_round.size() < tracked.size()) changed_round.resize(tracked.size(), 0);
	if (changed_round[at] == change_round) return;
	changed_round[at] = change_round;
	changed_slots.push_back(at);
}

void PredictedWorld::mark_group_changed(GJBaseGameLayer* layer, int group) {
	if (group <= 0 || (std::size_t)group >= layer->m_groups.size()) return;
	save_group(layer, group);
	auto* members = layer->m_groups[(std::size_t)group];
	for (unsigned i = 0; members && i < members->count(); ++i) {
		auto* object = static_cast<GameObject*>(members->objectAtIndex(i));
		if (!object) continue;
		unsigned at = slot(object);
		if (at == UINT_MAX) continue;
		touch_session(at);
		mark_changed(at);
	}
}

void PredictedWorld::mark_live_dirty(unsigned at) {
	if (live_dirty.size() < tracked.size()) live_dirty.resize(tracked.size(), 0);
	if (live_dirty[at]) return;
	live_dirty[at] = 1;
	tracked[at].dormant = false;
	live_dirty_slots.push_back(at);
}

void PredictedWorld::mark_group_live_dirty(GJBaseGameLayer* layer, int group) {
	if (group <= 0 || (std::size_t)group >= layer->m_groups.size()) return;
	auto* members = layer->m_groups[(std::size_t)group];
	for (unsigned i = 0; members && i < members->count(); ++i) {
		auto* object = static_cast<GameObject*>(members->objectAtIndex(i));
		if (!object) continue;
		unsigned at = slot(object);
		if (at != UINT_MAX) mark_live_dirty(at);
	}
}

void PredictedWorld::save_command_groups(GJBaseGameLayer* layer) {
	for (const auto& command : shadow->m_unkVector560) {
		if (command.m_commandType == 0) continue;
		mark_group_changed(layer, command.m_targetGroupID);
		mark_group_changed(layer, command.m_centerGroupID);
	}
	for (auto* follow : shadow->m_unkVector6d8)
		if (follow) mark_group_changed(layer, follow->getTag());
	for (auto* follow : shadow->m_unkVector6f0)
		if (follow) mark_group_changed(layer, follow->getTag());
	const auto& state = layer->m_gameState;
	for (const auto& action : state.m_dynamicMoveActions) {
		mark_group_changed(layer, action.m_targetGroupID);
		mark_group_changed(layer, action.m_centerGroupID);
	}
	for (const auto& action : state.m_dynamicRotateActions) {
		mark_group_changed(layer, action.m_targetGroupID);
		mark_group_changed(layer, action.m_centerGroupID);
	}
}

PredictedWorld::SavedCell* PredictedWorld::save_cell(GJBaseGameLayer* layer, int x, int y) {
	if (x < 0 || y < 0) return nullptr;
	++cell_saves;
	auto [it, fresh] = cells.try_emplace(cell_key(x, y));
	if (!fresh) return nullptr;
	auto& cell = it->second;
	cell.x = x;
	cell.y = y;
	save_list(cell.main, bucket_at(main_grid(layer), x, y), count_at(main_counts(layer), x, y));
	save_list(cell.non_effect, bucket_at(non_effect_grid(layer), x, y), count_at(non_effect_counts(layer), x, y));
	save_list(cell.blocks, bucket_at(block_grid(layer), x, y), count_at(block_counts(layer), x, y));
	auto& flags = layer->m_nonEffectObjectsFlags;
	cell.has_flag = (std::size_t)x < flags.size() && flags[(std::size_t)x] && (std::size_t)y < flags[(std::size_t)x]->size();
	cell.flag = cell.has_flag && (*flags[(std::size_t)x])[(std::size_t)y];
	return &cell;
}

void PredictedWorld::restore_live(PlayLayer* play) {
	needs_setters.assign(session_list.size(), 0);
	run_in_parts(session_list.size(), [&](std::size_t from, std::size_t to, unsigned) {
		for (std::size_t i = from; i < to; ++i) {
			unsigned at = session_list[i];
			auto& entry = tracked[at];
			auto* object = entry.object;
			const auto* node_fields = load_span_fields(object, live_fields.data() + (std::size_t)at * RECORD_SIZE);
			const auto& look = entry.look;
			auto* detail = object->m_colorSprite;
			bool setters = node_fields_differ(object, node_fields) || object->getRotationX() != look.rotation_x || object->getRotationY() != look.rotation_y || object->getScaleX() != look.scale_x ||
				object->getScaleY() != look.scale_y || object->getOpacity() != look.opacity || !object->CCNode::getPosition().equals(look.node_position) ||
				(detail && !object->m_colorSpriteLocked && !detail->getPosition().equals(look.detail_position));
			if (setters) needs_setters[i] = 1;
			else {
				object->m_isObjectRectDirty = true;
				object->m_isOrientedBoxDirty = true;
			}
		}
	});
	for (std::size_t i = 0; i < session_list.size(); ++i) {
		if (!needs_setters[i]) continue;
		unsigned at = session_list[i];
		auto& entry = tracked[at];
		auto* object = entry.object;
		load_object_fields(object, live_fields.data() + (std::size_t)at * RECORD_SIZE);
		const auto& look = entry.look;
		if (object->getRotationX() != look.rotation_x) object->setRotationX(look.rotation_x);
		if (object->getRotationY() != look.rotation_y) object->setRotationY(look.rotation_y);
		if (object->getScaleX() != look.scale_x) object->setScaleX(look.scale_x);
		if (object->getScaleY() != look.scale_y) object->setScaleY(look.scale_y);
		if (object->getOpacity() != look.opacity) object->setOpacity(look.opacity);
		if (!object->CCNode::getPosition().equals(look.node_position)) object->CCNode::setPosition(look.node_position);
		if (auto* detail = object->m_colorSprite; detail && !object->m_colorSpriteLocked && !detail->getPosition().equals(look.detail_position))
			detail->setPosition(look.detail_position);
	}
	for (auto& [key, cell] : cells) {
		int x = cell.x, y = cell.y;
		restore_list(cell.main, bucket_at(main_grid(play), x, y), count_at(main_counts(play), x, y), [x, y](GameObject* object, int index) {
			object->m_someOtherIndex = index;
			object->m_outerSectionIndex = x;
			object->m_middleSectionIndex = y;
		});
		restore_list(cell.non_effect, bucket_at(non_effect_grid(play), x, y), count_at(non_effect_counts(play), x, y), set_inner_index);
		restore_list(cell.blocks, bucket_at(block_grid(play), x, y), count_at(block_counts(play), x, y), set_inner_index);
		auto& flags = play->m_nonEffectObjectsFlags;
		if (cell.has_flag && (std::size_t)x < flags.size() && flags[(std::size_t)x] && (std::size_t)y < flags[(std::size_t)x]->size())
			(*flags[(std::size_t)x])[(std::size_t)y] = cell.flag;
	}
	for (unsigned at : session_list) {
		auto& entry = tracked[at];
		if (!entry.indexes_saved) continue;
		entry.object->m_someOtherIndex = entry.some_other_index;
		entry.object->m_innerSectionIndex = entry.inner_index;
		entry.object->m_outerSectionIndex = entry.outer_index;
		entry.object->m_middleSectionIndex = entry.middle_index;
	}
	restore_list(calc_non_effect, &play->m_calcNonEffectObjects, &play->m_calcNonEffectObjectsSize, set_inner_index);
	restore_list(calc_blocks, &play->m_calcCollisionBlockObjects, &play->m_calcCollisionBlockObjectsSize, set_inner_index);
	if (auto* queued = play->m_objectsToDeactivate)
		for (int id : deactivated_ids)
			queued->removeObjectForKey(id);
	deactivated_ids.clear();
	play->m_movedCount = moved_count;
	for (auto& trigger : fired) {
		std::memcpy(reinterpret_cast<unsigned char*>(trigger.object) + sizeof(GameObject), trigger.live_bytes.data(), trigger.live_bytes.size());
		load_sequence(trigger.sequence, trigger.live_sequence);
	}
	for (auto& roll : rolls)
		*roll.state = roll.live;
	play->m_spawnTuples = live_spawn_tuples;
}

void PredictedWorld::drop_shadow() {
	if (!shadow) return;
	shadow->setParent(nullptr);
	shadow->release();
	shadow = nullptr;
}

void PredictedWorld::invalidate() {
	valid = false;
	if (session_open) return;
	drop_shadow();
	drop_records();
}

void PredictedWorld::drop_records() {
	record_blocks.clear();
	record_bytes_held = 0;
	step_records.clear();
}

bool system_memory(unsigned long long& total, unsigned long long& available) {
	const unsigned long long bytes_per_kilobyte = 1024;
#ifdef GEODE_IS_WINDOWS
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (!GlobalMemoryStatusEx(&status)) return false;
	total = status.ullTotalPhys;
	available = status.ullAvailPhys;
	return true;
#elif defined(GEODE_IS_ANDROID)
	std::ifstream in("/proc/meminfo");
	std::string name;
	unsigned long long kilobytes = 0;
	total = available = 0;
	while (in >> name >> kilobytes) {
		if (name == "MemTotal:") total = kilobytes * bytes_per_kilobyte;
		else if (name == "MemAvailable:") available = kilobytes * bytes_per_kilobyte;
		in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	}
	return total > 0 && available > 0;
#elif defined(GEODE_IS_MACOS)
	std::size_t size = sizeof(total);
	if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) != 0) return false;
	vm_statistics64_data_t pages{};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&pages, &count) != KERN_SUCCESS)
		return false;
	available = ((unsigned long long)pages.free_count + pages.inactive_count + pages.purgeable_count) * vm_page_size;
	return true;
#else
	return false;
#endif
}

void PredictedWorld::refresh_record_limit() {
	const unsigned long long min_record_bytes = 96ull << 20;
	const unsigned long long keep_free_bytes = 2ull << 30;
	const int keep_free_share = 8;
	const int most_record_share = 4;
	const int free_record_share = 2;
	record_bytes_limit = min_record_bytes;
	unsigned long long total = 0, available = 0;
	if (!system_memory(total, available)) return;
	unsigned long long keep_free = std::max(total / keep_free_share, keep_free_bytes);
	unsigned long long usable = available + record_bytes_held;
	if (usable <= keep_free) return;
	unsigned long long most = std::min(total / most_record_share, (unsigned long long)std::numeric_limits<std::size_t>::max() / most_record_share);
	record_bytes_limit = (std::size_t)std::clamp((usable - keep_free) / free_record_share, min_record_bytes, std::max(most, min_record_bytes));
}

bool PredictedWorld::usable(PlayLayer* play, int now, float delta) const {
	if (!valid || play != this->play || delta != step_delta || now < start_step || now > horizon_step) return false;
	return (double)(now - start_step) < Logic::get().sample_rate() * TIMELINE_SECONDS;
}

bool PredictedWorld::full() const {
	return record_bytes_held >= record_bytes_limit;
}

bool PredictedWorld::matches_live(PlayLayer* play, int now, const CCRect& region) {
	mismatch.clear();
	if (!usable(play, now, step_delta)) {
		mismatch = fmt::format("timeline unusable valid={} horizon={} start={}", valid, horizon_step, start_step);
		return false;
	}
	if (live_unknown) {
		mismatch = "an object the world never saw moved live";
		return false;
	}
	for (std::size_t at = 0; at < tracked.size(); ++at) {
		auto* object = tracked[at].object;
		if (object->m_isDecoration2 || object->m_objectType == GameObjectType::Decoration || object == play->m_player1CollisionBlock || object == play->m_player2CollisionBlock) continue;
		CCPoint expected = compact_position(expected_fields.data() + at * COMPACT_STRIDE);
		if (!region.containsPoint(expected) && !region.containsPoint({ (float)object->m_positionX, (float)object->m_positionY }))
			continue;
		if (std::fabs((double)expected.x - object->m_positionX) > POSITION_TOLERANCE || std::fabs((double)expected.y - object->m_positionY) > POSITION_TOLERANCE) {
			watch_uid = object->m_uniqueID;
			mismatch = fmt::format("object uid={} id={} expected=({:.3f},{:.3f}) live=({:.3f},{:.3f})", object->m_uniqueID, object->m_objectID, expected.x, expected.y,
				object->m_positionX, object->m_positionY);
			return false;
		}
	}
	return true;
}

void PredictedWorld::reset_timing() {
	for (auto& value : part_ms) value = 0.0;
	moved_objects = section_changes = cell_saves = toggles = live_misses = 0;
}

std::string PredictedWorld::timing_report() const {
	return fmt::format("save_groups={:.2f} spawn_timers={:.2f} prepare={:.2f} process={:.2f} post_blocks={:.2f} record={:.2f} spawn_check={:.2f} apply={:.2f} moved={} section={} cells={} toggles={} xcheck_misses={}",
		part_ms[0], part_ms[1], part_ms[2], part_ms[3], part_ms[4], part_ms[5], part_ms[6], part_ms[7], moved_objects, section_changes, cell_saves, toggles, live_misses);
}

std::string PredictedWorld::follow_report(PlayLayer* play) const {
	std::string out;
	auto describe = [&](const char* who, GJEffectManager* manager) {
		if (!manager) return;
		out += fmt::format(" {}: commands={} follows=[", who, manager->m_unkVector560.size());
		for (auto* follow : manager->m_unkVector6f0) {
			if (!follow) continue;
			out += fmt::format("(group={} speed={} delay={} offset={} max={})", follow->getTag(), follow->m_unk0a8, follow->m_unk0b0, follow->m_unk0b8, follow->m_unk0c0);
		}
		out += "]";
		for (const auto& command : manager->m_unkVector560)
			out += fmt::format(" cmd(type={} target={} center={} lockX={} lockY={} camX={} camY={} modX={} modY={} player={} elapsed={:.3f} dur={:.3f} offset=({:.3f},{:.3f}))", command.m_commandType, command.m_targetGroupID, command.m_centerGroupID,
				command.m_lockToPlayerX, command.m_lockToPlayerY, command.m_lockToCameraX, command.m_lockToCameraY, command.m_moveModX, command.m_moveModY, command.m_targetPlayer, command.m_deltaTime, command.m_duration, command.m_currentXOffset, command.m_currentYOffset);
	};
	describe("live", play->m_effectManager);
	describe("shadow", shadow);
	return out;
}

std::string PredictedWorld::near_report(CCPoint at, float reach) const {
	std::string out;
	if (!valid) return out;
	for (std::size_t slot = 0; slot < tracked.size(); ++slot) {
		auto* object = tracked[slot].object;
		CCPoint expected = compact_position(expected_fields.data() + slot * COMPACT_STRIDE);
		float live_x = (float)object->m_positionX, live_y = (float)object->m_positionY;
		bool close = std::fabs(expected.x - at.x) <= reach && std::fabs(expected.y - at.y) <= reach;
		if (!close) close = std::fabs(live_x - at.x) <= reach && std::fabs(live_y - at.y) <= reach;
		if (!close) continue;
		std::string groups;
		for (int g = 0; object->m_groups && g < object->m_groupCount && g < 10; ++g) groups += fmt::format("{}{}", g ? "," : "", (*object->m_groups)[(std::size_t)g]);
		out += fmt::format(" [uid={} id={} groups={} exp=({:.2f},{:.2f}) live=({:.2f},{:.2f})]", object->m_uniqueID, object->m_objectID, groups, expected.x, expected.y, live_x, live_y);
		if (out.size() > 1500) break;
	}
	return out;
}

void PredictedWorld::trim(int now) {
	if (!valid) return;
	std::size_t done = 0;
	while (done < step_records.size() && step_records[done].step <= now) {
		const auto& first = step_records[done];
		const auto* in = first.bytes;
		unsigned slot = 0;
		for (unsigned i = 0; i < first.count; ++i) {
			bool new_mask = false;
			in = read_entry_start(in, slot, new_mask);
			in = read_entry_fields(in, new_mask, expected_masks[slot], expected_fields.data() + (std::size_t)slot * COMPACT_STRIDE);
		}
		++done;
	}
	step_records.erase(step_records.begin(), step_records.begin() + (std::ptrdiff_t)done);
	while (!record_blocks.empty() && (step_records.empty() || record_blocks.front().last_step < step_records.front().step)) {
		record_bytes_held -= record_blocks.front().size;
		record_blocks.pop_front();
	}
	start_step = std::max(start_step, now);
}

void PredictedWorld::rebuild(PlayLayer* play, float delta, int now) {
	drop_shadow();
	enable_hooks(true);
	valid = true;
	live_unknown = false;
	this->play = play;
	step_delta = delta;
	start_step = horizon_step = now;
	last_change_step = -1;
	++round;
	tracked.clear();
	live_dirty.clear();
	live_dirty_slots.clear();
	changed_slots.clear();
	changed_round.clear();
	live_fields.clear();
	horizon_fields.clear();
	latest_fields.clear();
	expected_fields.clear();
	pass_fields.clear();
	pass_field_round.clear();
	latest_masks.clear();
	expected_masks.clear();
	pass_masks.clear();
	drop_records();
	fired.clear();
	rolls.clear();
	rebuild_slots(play);
	auto* live = play->m_effectManager;
	EffectManagerState state;
	live->saveToState(state);
	shadow = GJEffectManager::create();
	shadow->retain();
	shadow->m_triggerEffectDelegate = live->m_triggerEffectDelegate;
	shadow->loadFromState(state);
	shadow->m_unk780 = live->m_unk780;
	shadow->m_unk784 = live->m_unk784;
	shadow->m_unk788 = live->m_unk788;
	shadow->m_unk78C = live->m_unk78C;
	shadow->m_unk790 = live->m_unk790;
	shadow->m_unk794 = live->m_unk794;
	shadow->m_unk798 = live->m_unk798;
	shadow->setParent(play);
	horizon_state = std::make_unique<SavedGameState>(play->m_gameState);
	horizon_random = GameToolbox::getfast_srand();
	horizon_teleport = Logic::get().teleport_random;
	horizon_shake = Logic::get().shake_random;
	horizon_spawn_tuples = play->m_spawnTuples;
	camera_start = play->m_gameState.m_cameraPosition;
	camera2_start = play->m_gameState.m_cameraPosition2;
	driver_known = false;
}

#ifdef GEODE_IS_WINDOWS
static std::map<std::pair<int, int>, int>& event_round_map(PlayLayer* play) {
	return *reinterpret_cast<std::map<std::pair<int, int>, int>*>(reinterpret_cast<unsigned char*>(play) + GAME_EVENT_ROUNDS_OFFSET);
}

static std::map<std::pair<int, int>, std::vector<EventLink>>& event_link_map(PlayLayer* play) {
	return *reinterpret_cast<std::map<std::pair<int, int>, std::vector<EventLink>>*>(reinterpret_cast<unsigned char*>(play) + GAME_EVENT_LINKS_OFFSET);
}
#endif

void PredictedWorld::begin_session(PlayLayer* play) {
	if (session_open) end_session(this->play);
	suspended = false;
	suspended_applied.clear();
	suspended_pending.clear();
	if (!valid || play != this->play) return;
	refresh_record_limit();
	session_open = true;
	horizon_loaded = false;
	++session_round;
	++placed_round;
	session_list.clear();
	applied.clear();
	pending.clear();
	pending_scanned = false;
	begin_pass();
	cells.clear();
	deactivated_ids.clear();
	save_list(calc_non_effect, &play->m_calcNonEffectObjects, &play->m_calcNonEffectObjectsSize);
	save_list(calc_blocks, &play->m_calcCollisionBlockObjects, &play->m_calcCollisionBlockObjectsSize);
	moved_count = play->m_movedCount;
	live_spawn_tuples = play->m_spawnTuples;
#ifdef GEODE_IS_WINDOWS
	live_event_links = event_link_map(play);
#endif
	for (auto& trigger : fired) {
		const auto* raw = reinterpret_cast<const unsigned char*>(trigger.object) + sizeof(GameObject);
		trigger.live_bytes.assign(raw, raw + trigger.live_bytes.size());
		save_sequence(trigger.sequence, trigger.live_sequence);
	}
	for (auto& roll : rolls)
		roll.live = *roll.state;
	grouped_positions.clear();
	auto* objects = Logic::get().trace_logging ? play->m_objects : nullptr;
	for (unsigned i = 0; objects && i < objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
		if (object && object->m_groupCount > 0) grouped_positions.push_back({ object, { (float)object->m_positionX, (float)object->m_positionY } });
	}
}

void PredictedWorld::resume_horizon(PlayLayer* play) {
	const unsigned char resume_differs = 1;
	const unsigned char resume_node = 2;
	const unsigned char resume_section = 4;
	if (!session_open || horizon_loaded || play != this->play) return;
	horizon_loaded = true;
	std::size_t count = tracked.size();
	horizon_differs.resize(count);
	joined_parts.resize(WORLD_PARTS);
	for (auto& joined : joined_parts) joined.clear();
	run_in_parts(count, [&](std::size_t from, std::size_t to, unsigned part) {
		unsigned char current[MAX_RECORD_BYTES];
		for (std::size_t at = from; at < to; ++at) {
			auto& entry = tracked[at];
			horizon_differs[at] = 0;
			if (entry.dormant) continue;
			auto* object = entry.object;
			const auto* horizon = horizon_fields.data() + at * RECORD_SIZE;
			save_object_fields(object, current);
			if (std::memcmp(current, horizon, RECORD_SIZE) == 0) {
				bool live_wrote = at < live_dirty.size() && live_dirty[at];
				if (!live_wrote && entry.changed_step <= start_step) entry.dormant = true;
				continue;
			}
			if (join_session((unsigned)at)) joined_parts[part].push_back((unsigned)at);
			const auto* node_fields = load_span_fields(object, horizon);
			unsigned char work = resume_differs;
			if (node_fields_differ(object, node_fields)) work |= resume_node;
			else {
				object->m_isObjectRectDirty = true;
				object->m_isOrientedBoxDirty = true;
			}
			if (!far_away(object) && !MovingObjects::same_section(play, object))
				work |= resume_section;
			horizon_differs[at] = work;
		}
	});
	for (const auto& joined : joined_parts)
		session_list.insert(session_list.end(), joined.begin(), joined.end());
	int missed_logs = 0;
	for (unsigned at = 0; at < count; ++at) {
		if (!horizon_differs[at]) continue;
		auto& entry = tracked[at];
		const auto* horizon = horizon_fields.data() + (std::size_t)at * RECORD_SIZE;
		if (at >= live_dirty.size() || !live_dirty[at]) {
			++live_misses;
			if (Logic::get().trace_logging && ++missed_logs <= 8)
				Trajectory::log_line(fmt::format("XCHECK frame={} uid={} id={} live=({:.3f},{:.3f}) differs from the horizon with no live write seen by the hooks", Logic::get().step_frame, entry.object->m_uniqueID,
					entry.object->m_objectID, entry.object->m_positionX, entry.object->m_positionY));
		}
		if (horizon_differs[at] & resume_node) load_object_fields(entry.object, horizon);
		if (horizon_differs[at] & resume_section) play->updateObjectSection(entry.object);
	}
	for (unsigned at : live_dirty_slots) live_dirty[at] = 0;
	live_dirty_slots.clear();
	for (auto& trigger : fired) {
		std::memcpy(reinterpret_cast<unsigned char*>(trigger.object) + sizeof(GameObject), trigger.horizon_bytes.data(), trigger.horizon_bytes.size());
		load_sequence(trigger.sequence, trigger.horizon_sequence);
	}
	for (auto& roll : rolls)
		*roll.state = roll.horizon;
	play->m_spawnTuples = horizon_spawn_tuples;
	horizon_state->restore(play->m_gameState);
	GameToolbox::fast_srand(horizon_random);
	Logic::get().teleport_random = horizon_teleport;
	Logic::get().shake_random = horizon_shake;
}

void PredictedWorld::pause_horizon(PlayLayer* play) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	horizon_loaded = false;
	std::size_t count = tracked.size();
	run_in_parts(count, [&](std::size_t from, std::size_t to, unsigned) {
		for (std::size_t at = from; at < to; ++at) {
			auto& entry = tracked[at];
			if (entry.dormant) continue;
			save_object_fields(entry.object, horizon_fields.data() + at * RECORD_SIZE);
		}
	});
	for (auto& trigger : fired) {
		const auto* raw = reinterpret_cast<const unsigned char*>(trigger.object) + sizeof(GameObject);
		trigger.horizon_bytes.assign(raw, raw + trigger.live_bytes.size());
		save_sequence(trigger.sequence, trigger.horizon_sequence);
	}
	for (auto& roll : rolls)
		roll.horizon = *roll.state;
	horizon_spawn_tuples = play->m_spawnTuples;
	horizon_state = std::make_unique<SavedGameState>(play->m_gameState);
	horizon_random = GameToolbox::getfast_srand();
	horizon_teleport = Logic::get().teleport_random;
	horizon_shake = Logic::get().shake_random;
	restore_live(play);
}

void PredictedWorld::record_moved() {
	const int sort_changed_share = 16;
	const unsigned record_block_bytes = 4u << 20;
	StepRecords records;
	records.step = horizon_step;
	if (changed_slots.size() * sort_changed_share < tracked.size())
		std::sort(changed_slots.begin(), changed_slots.end());
	else {
		changed_slots.clear();
		for (unsigned at = 0; at < tracked.size(); ++at)
			if (changed_round[at] == change_round) changed_slots.push_back(at);
	}
	std::size_t count = changed_slots.size();
	encoded_parts.resize(WORLD_PARTS);
	for (auto& part : encoded_parts) {
		part.bytes.clear();
		part.count = 0;
	}
	run_in_parts(count, [&](std::size_t from, std::size_t to, unsigned part_index) {
		auto& part = encoded_parts[part_index];
		unsigned char compact[MAX_RECORD_BYTES] = {};
		unsigned previous_slot = 0;
		for (std::size_t i = from; i < to; ++i) {
			unsigned at = changed_slots[i];
			auto* changed = tracked[at].object;
			if (changed->m_isDecoration2 || changed->m_objectType == GameObjectType::Decoration) continue;
			save_compact_fields(changed, compact);
			auto* latest = latest_fields.data() + (std::size_t)at * COMPACT_STRIDE;
			unsigned long long mask = changed_words(compact, latest);
			if (!mask) continue;
			std::memcpy(latest, compact, COMPACT_STRIDE);
			bool new_mask = mask != latest_masks[at];
			latest_masks[at] = mask;
			if (part.count == 0) {
				part.first_slot = at;
				part.first_new_mask = new_mask;
			}
			write_entry_start(part.bytes, at - previous_slot, new_mask);
			if (part.count == 0) part.first_size = part.bytes.size();
			previous_slot = at;
			std::size_t end = part.bytes.size();
			part.bytes.resize(end + (new_mask ? sizeof(mask) : 0) + (std::size_t)std::popcount(mask) * sizeof(unsigned));
			auto* out = part.bytes.data() + end;
			if (new_mask) {
				std::memcpy(out, &mask, sizeof(mask));
				out += sizeof(mask);
			}
			for (; mask; mask &= mask - 1) {
				std::memcpy(out, compact + (std::size_t)std::countr_zero(mask) * sizeof(unsigned), sizeof(unsigned));
				out += sizeof(unsigned);
			}
			part.last_slot = at;
			++part.count;
		}
	});
	unsigned previous_slot = 0;
	step_bytes.clear();
	for (const auto& part : encoded_parts) {
		if (!part.count) continue;
		write_entry_start(step_bytes, part.first_slot - previous_slot, part.first_new_mask);
		step_bytes.insert(step_bytes.end(), part.bytes.begin() + (std::ptrdiff_t)part.first_size, part.bytes.end());
		previous_slot = part.last_slot;
		records.count += part.count;
	}
	if (!step_bytes.empty()) {
		if (record_blocks.empty() || record_blocks.back().size - record_blocks.back().used < step_bytes.size()) {
			RecordBlock block;
			block.size = std::max<std::size_t>(record_block_bytes, step_bytes.size());
			block.bytes.reset(new unsigned char[block.size]);
			record_bytes_held += block.size;
			record_blocks.push_back(std::move(block));
		}
		auto& block = record_blocks.back();
		std::memcpy(block.bytes.get() + block.used, step_bytes.data(), step_bytes.size());
		records.bytes = block.bytes.get() + block.used;
		block.used += step_bytes.size();
		block.last_step = records.step;
	}
	if (records.count > 0) last_change_step = records.step;
	step_records.push_back(records);
	changed_slots.clear();
	++change_round;
}

void PredictedWorld::enter(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2) {
	if (enter_depth++ > 0) return;
	entered_manager = play->m_effectManager;
	entered_player1 = play->m_player1;
	entered_player2 = play->m_player2;
	play->m_effectManager = shadow;
	play->m_player1 = driver1;
	if (driver2) play->m_player2 = driver2;
	inside_step = true;
}

void PredictedWorld::leave(PlayLayer* play) {
	if (--enter_depth > 0) return;
	inside_step = false;
	play->m_player1 = entered_player1;
	play->m_player2 = entered_player2;
	play->m_effectManager = entered_manager;
}

void PredictedWorld::begin_step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	enter(play, driver1, driver2);
	save_command_groups(play);
	auto& state = play->m_gameState;
	CCPoint at = driver1->getPosition();
	if (!driver_known) {
		driver_known = true;
		driver_start = at;
	}
	CCPoint ahead = at - driver_start;
	CCPoint shift = driver1->m_isSideways ? CCPoint{ 0.f, ahead.y } : CCPoint{ ahead.x, 0.f };
	CCPoint camera = camera_start + shift;
	state.m_cameraStepDiff = camera - state.m_cameraPosition;
	state.m_cameraPosition = camera;
	state.m_cameraPosition2 = camera2_start + shift;
	float seconds = step_delta / STEPS_PER_SECOND;
	play->m_spawnTuples.clear();
	shadow->updateSpawnTriggers(seconds);
	state.m_unkUint4 = state.m_commandIndex;
	state.m_commandIndex += 2;
	play->m_spawnTuples.clear();
	for (auto& [group, flag] : shadow->m_unkMap288) flag = false;
	leave(play);
}

void PredictedWorld::move_step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2, bool last_step) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	enter(play, driver1, driver2);
	auto& state = play->m_gameState;
	CCPoint at = driver1->getPosition();
	float dx = at.x - driver1->m_lastPosition.x, dy = at.y - driver1->m_lastPosition.y;
	if (std::abs(dy - driver1->m_yVelocityRelated3) > step_delta * 16.f) dy = 0.f;
	shadow->m_unk780 = dx;
	shadow->m_unk784 = dy;
	shadow->m_unk788 = -state.m_cameraStepDiff.x;
	shadow->m_unk78C = -state.m_cameraStepDiff.y;
	shadow->m_unk790 = driver1->getRotation() - driver1->m_unkUnused3;
	float seconds = step_delta / STEPS_PER_SECOND;
	auto mark = std::chrono::steady_clock::now();
	auto lap = [&](int part) {
		auto now = std::chrono::steady_clock::now();
		part_ms[part] += std::chrono::duration<double, std::milli>(now - mark).count();
		mark = now;
	};
	shadow->updateTimers(seconds, state.m_timeWarp);
	lap(1);
	shadow->prepareMoveActions(seconds, !last_step);
	lap(2);
	play->processMoveActionsStep(seconds, last_step);
	lap(3);
	shadow->postMoveActions();
	play->updateCollisionBlocks();
	lap(4);
	leave(play);
	last_driver_dy = dy;
}

static void save_follow_positions(PlayLayer* play) {
	auto& state = play->m_gameState;
	const int follow_save_gap = 1000;
	if (state.m_unkUint5 != 0 && state.m_unkUint5 + follow_save_gap > state.m_unkUint6) return;
	state.m_unkUint5 = state.m_unkUint6;
	++state.m_unkUint11;
	auto& history = state.m_unkVecFloat1;
	for (const auto& [key, size] : play->m_savePositionValues) {
		const CCPoint* at;
		int base;
		if (key > 0) {
			auto found = play->m_savePositionObjects.find(key);
			if (found == play->m_savePositionObjects.end() || !found->second) continue;
			at = &found->second->m_lastPosition;
			base = found->second->m_unk52C;
		} else if (key == -1) {
			at = &play->m_player1->m_lastPosition;
			base = play->m_player1->m_unk52C;
		} else if (key == -2) {
			at = &play->m_player2->m_lastPosition;
			base = play->m_player2->m_unk52C;
		} else if (key == -3) {
			at = &state.m_cameraPosition2;
			base = state.m_unkUint12;
		} else continue;
		if (size <= 0 || base < 0) continue;
		std::size_t slot = (std::size_t)(state.m_unkUint11 % size + base) * 2;
		if (slot + 1 >= history.size()) continue;
		history[slot] = at->x;
		history[slot + 1] = at->y;
	}
}

void PredictedWorld::end_step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	enter(play, driver1, driver2);
	save_follow_positions(play);
	shadow->postCollisionCheck();
	leave(play);
	++horizon_step;
	auto record_began = std::chrono::steady_clock::now();
	record_moved();
	part_ms[5] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - record_began).count();
	if (Logic::get().trace_logging && watch_uid >= 0 && horizon_step - start_step <= 12 && (std::size_t)watch_uid < slot_of_uid.size() && slot_round[(std::size_t)watch_uid] == round) {
		auto* object = tracked[slot_of_uid[(std::size_t)watch_uid]].object;
		CCPoint at = driver1->getPosition();
		Trajectory::log_line(fmt::format("WSTEP step={} uid={} pos=({:.3f},{:.3f}) driver=({:.3f},{:.3f}) dy={:.4f} commands={} follows={}", horizon_step, watch_uid, object->m_positionX, object->m_positionY,
			at.x, at.y, last_driver_dy, shadow->m_unkVector560.size(), shadow->m_unkVector6f0.size()));
	}
}

void PredictedWorld::fire_spawn_triggers(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	enter(play, driver1, driver2);
	auto spawn_began = std::chrono::steady_clock::now();
	play->checkSpawnObjects();
	play->m_effectManager->postCollisionCheck();
	part_ms[6] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - spawn_began).count();
	leave(play);
}

void PredictedWorld::collect(PlayLayer* play, EffectGameObject* object, PlayerObject* body) {
	if (!inside_step || !session_open || !horizon_loaded || play != this->play) return;
#ifdef GEODE_IS_WINDOWS
	auto* manager = play->m_effectManager;
	auto& triggered = *reinterpret_cast<std::set<std::pair<int, int>>*>(reinterpret_cast<unsigned char*>(manager) + TRIGGERED_IDS_OFFSET);
	if (triggered.count({ object->m_uniqueID, 0 })) return;
	manager->storeTriggeredID(object->m_uniqueID, 0);
	object->triggerObject(play, body->m_uniqueID, nullptr);
#endif
}

void PredictedWorld::player_button(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2, bool down, bool player1, bool handled, int button) {
	const int button_down_event = 69;
	const int button_up_event = 70;
	const int button_event_stride = 2;
	if (!session_open || !horizon_loaded || play != this->play) return;
	enter(play, driver1, driver2);
	if (handled) {
		bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
		int player_id = two_player && play->m_gameState.m_isDualMode && !player1 ? 2 : 1;
		play->gameEventTriggered((GJGameEvent)((down ? button_down_event : button_up_event) + (button - 1) * button_event_stride), 0, player_id);
	}
	if (!play->m_player1->m_isDead) play->m_effectManager->playerButton(down, player1);
	leave(play);
}

void PredictedWorld::note_event(PlayLayer* play, int event, int material, int player_id) {
	const int material_key_scale = 10000;
	if (!session_open || play != this->play) return;
	std::pair<int, int> key{ event, material * material_key_scale + player_id };
	for (const auto& round : event_rounds)
		if (round.key == key) return;
#ifdef GEODE_IS_WINDOWS
	auto& rounds = event_round_map(play);
	auto found = rounds.find(key);
	event_rounds.push_back({ key, found == rounds.end() ? 0 : found->second });
#endif
}

CCPoint PredictedWorld::placed_position(unsigned at) {
	auto& entry = tracked[at];
	if (entry.placed_round != placed_round) {
		entry.placed_round = placed_round;
		entry.placed = CCPoint((float)entry.object->m_positionX, (float)entry.object->m_positionY);
	}
	return entry.placed;
}

void PredictedWorld::begin_pass() {
	++pass_round;
	pass_step = start_step;
}

unsigned char* PredictedWorld::pass_compact(unsigned at) {
	if (pass_field_round.size() < tracked.size()) {
		pass_field_round.resize(tracked.size(), 0);
		pass_fields.resize(tracked.size() * COMPACT_STRIDE);
		pass_masks.resize(tracked.size(), 0);
	}
	auto* compact = pass_fields.data() + (std::size_t)at * COMPACT_STRIDE;
	if (pass_field_round[at] != pass_round) {
		pass_field_round[at] = pass_round;
		std::memcpy(compact, expected_fields.data() + (std::size_t)at * COMPACT_STRIDE, COMPACT_STRIDE);
		pass_masks[at] = expected_masks[at];
	}
	return compact;
}

void PredictedWorld::apply_pass_fields(PlayLayer* play, unsigned at) {
	touch_session(at);
	auto& entry = tracked[at];
	const auto* compact = pass_compact(at);
	load_compact_fields(entry.object, compact);
	entry.placed = compact_position(compact);
	entry.placed_round = placed_round;
	if (!far_away(entry.object) && !MovingObjects::same_section(play, entry.object))
		play->updateObjectSection(entry.object);
	if (!entry.applied) applied.push_back(at);
	entry.applied = true;
	entry.pending = false;
}

void PredictedWorld::apply_records(PlayLayer* play, int step, const CCRect& window) {
	const float pending_margin = 20.f;
	if (!session_open || horizon_loaded || play != this->play) return;
	auto apply_began = std::chrono::steady_clock::now();
	struct ApplyTimer {
		double& total;
		std::chrono::steady_clock::time_point began;
		~ApplyTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(); }
	} apply_timer{ part_ms[7], apply_began };
	bool rescan = !pending_scanned || window.getMinX() < pending_window.getMinX() || window.getMaxX() > pending_window.getMaxX() || window.getMinY() < pending_window.getMinY() ||
		window.getMaxY() > pending_window.getMaxY();
	if (rescan) {
		pending_window = { window.origin.x - pending_margin, window.origin.y - pending_margin, window.size.width + 2.f * pending_margin, window.size.height + 2.f * pending_margin };
		pending_scanned = true;
	}
	auto close = [&](unsigned at, CCPoint position) {
		return pending_window.containsPoint(position) || pending_window.containsPoint(placed_position(at));
	};
	int first_step = step_records.empty() ? step + 1 : std::max(pass_step + 1, step_records.front().step);
	for (int record_step = first_step; record_step <= step; ++record_step) {
		std::size_t index = (std::size_t)(record_step - step_records.front().step);
		if (index >= step_records.size()) break;
		const auto& records = step_records[index];
		const auto* in = records.bytes;
		unsigned at = 0;
		for (unsigned i = 0; i < records.count; ++i) {
			bool new_mask = false;
			in = read_entry_start(in, at, new_mask);
			auto* compact = pass_compact(at);
			in = read_entry_fields(in, new_mask, pass_masks[at], compact);
			auto& entry = tracked[at];
			if (close(at, compact_position(compact))) {
				apply_pass_fields(play, at);
				continue;
			}
			if (!entry.pending) pending.push_back(at);
			entry.pending = true;
		}
	}
	pass_step = std::max(pass_step, step);
	for (std::size_t i = 0; rescan && i < pending.size();) {
		unsigned at = pending[i];
		if (tracked[at].pending && !close(at, compact_position(pass_compact(at)))) {
			++i;
			continue;
		}
		if (tracked[at].pending) apply_pass_fields(play, at);
		pending[i] = pending.back();
		pending.pop_back();
	}
}

void PredictedWorld::reapply_records(PlayLayer* play) {
	if (!session_open || horizon_loaded || play != this->play) return;
	for (unsigned at : applied)
		if (tracked[at].applied) apply_pass_fields(play, at);
}

void PredictedWorld::reset_applied() {
	++placed_round;
	for (unsigned at : applied) tracked[at].applied = false;
	for (unsigned at : pending) tracked[at].pending = false;
	applied.clear();
	pending.clear();
	pending_scanned = false;
	begin_pass();
}

void PredictedWorld::pause_session(PlayLayer* play) {
	if (!session_open || play != this->play) return;
	if (horizon_loaded) pause_horizon(play);
	else restore_live(play);
}

void PredictedWorld::end_session(PlayLayer* play) {
	if (!session_open) return;
	if (play == this->play) {
		pause_session(play);
		for (const auto& [object, position] : grouped_positions) {
			if ((float)object->m_positionX == position.x && (float)object->m_positionY == position.y) continue;
			unsigned at = slot(object);
			if (at != UINT_MAX && tracked[at].session == session_round) continue;
			if (Logic::get().trace_logging)
				Trajectory::log_line(fmt::format("LEAK frame={} uid={} id={} moved from ({:.3f},{:.3f}) to ({:.3f},{:.3f}) outside the journal", Logic::get().step_frame, object->m_uniqueID, object->m_objectID,
					position.x, position.y, object->m_positionX, object->m_positionY));
		}
	}
#ifdef GEODE_IS_WINDOWS
	if (play == this->play) {
		auto& rounds = event_round_map(play);
		for (const auto& round : event_rounds)
			if (auto found = rounds.find(round.key); found != rounds.end())
				found->second = round.live_round;
		event_link_map(play) = live_event_links;
	}
#endif
	event_rounds.clear();
	session_open = false;
	horizon_loaded = false;
	inside_step = false;
	session_list.clear();
	for (unsigned at : applied)
		if (at < tracked.size()) tracked[at].applied = false;
	for (unsigned at : pending)
		if (at < tracked.size()) tracked[at].pending = false;
	applied.clear();
	pending.clear();
	pending_scanned = false;
	cells.clear();
	grouped_positions.clear();
	if (!valid) drop_shadow();
}

void PredictedWorld::suspend_session(PlayLayer* play) {
	if (!session_open || play != this->play) return;
	suspended_applied.clear();
	suspended_pending.clear();
	for (unsigned at : applied)
		if (tracked[at].applied) suspended_applied.push_back(at);
	for (unsigned at : pending)
		if (tracked[at].pending) suspended_pending.push_back(at);
	suspended_pass_round = pass_round;
	suspended_pass_step = pass_step;
	end_session(play);
	suspended = true;
}

void PredictedWorld::resume_session(PlayLayer* play) {
	if (!suspended) return;
	auto saved_applied = std::move(suspended_applied);
	auto saved_pending = std::move(suspended_pending);
	begin_session(play);
	if (!session_open) return;
	pass_round = suspended_pass_round;
	pass_step = suspended_pass_step;
	for (unsigned at : saved_applied) {
		touch_session(at);
		tracked[at].applied = true;
		applied.push_back(at);
	}
	for (unsigned at : saved_pending) {
		touch_session(at);
		tracked[at].pending = true;
		pending.push_back(at);
	}
	pending_scanned = false;
}

void PredictedWorld::note_trigger(EffectGameObject* object) {
	if (!session_open || !horizon_loaded || !object) return;
	unsigned at = touch(object);
	auto& entry = tracked[at];
	if (entry.trigger >= 0) return;
	entry.trigger = (int)fired.size();
	auto& trigger = fired.emplace_back();
	trigger.object = object;
	const auto* raw = reinterpret_cast<const unsigned char*>(object) + sizeof(GameObject);
	trigger.live_bytes.assign(raw, raw + (sizeof(EffectGameObject) - sizeof(GameObject)));
	trigger.horizon_bytes = trigger.live_bytes;
	trigger.sequence = typeinfo_cast<SequenceTriggerGameObject*>(object);
	save_sequence(trigger.sequence, trigger.live_sequence);
	trigger.horizon_sequence = trigger.live_sequence;
}

void PredictedWorld::note_random_roll(unsigned long long* state) {
	if (!session_open || !horizon_loaded || !state) return;
	for (const auto& roll : rolls)
		if (roll.state == state) return;
	rolls.push_back({ state, *state, *state });
}

void PredictedWorld::note_moved(GJBaseGameLayer* layer, CCArray* objects) {
	if (layer != play || !objects) return;
	if (!session_open) {
		if (!valid || live_unknown) return;
		for (unsigned i = 0; i < objects->count(); ++i) {
			auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
			if (!object) break;
			unsigned at = slot(object);
			if (at == UINT_MAX) {
				live_unknown = true;
				return;
			}
			mark_live_dirty(at);
		}
		return;
	}
	moved_objects += objects->count();
	for (unsigned i = 0; i < objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
		if (!object) break;
		touch(object);
	}
}

void PredictedWorld::note_moved_one(GJBaseGameLayer* layer, GameObject* object) {
	if (layer != play || !object) return;
	if (!session_open) {
		if (!valid || live_unknown) return;
		unsigned at = slot(object);
		if (at == UINT_MAX) live_unknown = true;
		else mark_live_dirty(at);
		return;
	}
	touch(object);
}

void PredictedWorld::note_toggle(GJBaseGameLayer* layer, int group) {
	if (layer != play) return;
	if (!session_open) {
		if (valid) mark_group_live_dirty(layer, group);
		return;
	}
	++toggles;
	if (horizon_loaded) last_change_step = std::max(last_change_step, horizon_step + 1);
	if (group <= 0 || (std::size_t)group >= layer->m_groups.size()) return;
	auto* members = layer->m_groups[(std::size_t)group];
	for (unsigned i = 0; members && i < members->count(); ++i) {
		auto* object = static_cast<GameObject*>(members->objectAtIndex(i));
		if (object) touch(object);
	}
}

void PredictedWorld::log_toggle(GJBaseGameLayer* layer, int group, bool activate) {
	auto* manager = layer->m_effectManager;
	int bit = manager && group >= 0 && (std::size_t)group < manager->m_unkVector438.size() ? (int)manager->m_unkVector438[(std::size_t)group] : -1;
	int members = group > 0 && (std::size_t)group < layer->m_groups.size() && layer->m_groups[(std::size_t)group] ? (int)layer->m_groups[(std::size_t)group]->count() : 0;
	int first_counter = 0, first_uid = 0;
	if (members > 0) {
		auto* first = static_cast<GameObject*>(layer->m_groups[(std::size_t)group]->objectAtIndex(0));
		first_counter = first->m_enabledGroupsCounter;
		first_uid = first->m_uniqueID;
	}
	Trajectory::log_line(fmt::format("TOGGLE frame={} step={} group={} on={} bit={} shadow={} predicting={} firing={} session={} members={} first_uid={} first_counter={}", Logic::get().step_frame, horizon_step, group, activate, bit,
		manager == shadow, Logic::get().predicting, inside_step, session_open, members, first_uid, first_counter));
}

void PredictedWorld::note_area(GJBaseGameLayer* layer, CCArray* objects, int outer_min, int outer_max, int middle_min, int middle_max, bool target_groups) {
	if (!session_open || layer != play) return;
	if (target_groups) {
		note_moved(layer, objects);
		return;
	}
	auto& grid = main_grid(layer);
	auto& counts = main_counts(layer);
	int last_x = std::min(outer_max, (int)grid.size() - 1);
	for (int x = std::max(outer_min, 0); x <= last_x; ++x) {
		if (!grid[(std::size_t)x]) continue;
		int last_y = std::min(middle_max, (int)grid[(std::size_t)x]->size() - 1);
		for (int y = std::max(middle_min, 0); y <= last_y; ++y) {
			auto* bucket = bucket_at(grid, x, y);
			auto* amount = count_at(counts, x, y);
			if (!bucket || !amount) continue;
			int used = std::clamp(*amount, 0, (int)bucket->size());
			for (int i = 0; i < used; ++i)
				if ((*bucket)[(std::size_t)i]) touch((*bucket)[(std::size_t)i]);
		}
	}
}

void PredictedWorld::before_section_change(GJBaseGameLayer* layer, GameObject* object) {
	++section_changes;
	if (!session_open || layer != play || !object || Logic::get().is_traj_player(object)) return;
	unsigned at = touch(object);
	auto& entry = tracked[at];
	if (!entry.indexes_saved) {
		entry.indexes_saved = true;
		entry.some_other_index = object->m_someOtherIndex;
		entry.inner_index = object->m_innerSectionIndex;
		entry.outer_index = object->m_outerSectionIndex;
		entry.middle_index = object->m_middleSectionIndex;
	}
	save_cell(layer, object->m_outerSectionIndex, object->m_middleSectionIndex);
}

void PredictedWorld::before_section_add(GJBaseGameLayer* layer, GameObject* object) {
	if (!session_open || layer != play || !object || Logic::get().is_traj_player(object)) return;
	before_section_change(layer, object);
	int x, y;
	if (section_of(layer, object, x, y)) save_cell(layer, x, y);
}

void PredictedWorld::note_deactivated(GJBaseGameLayer* layer, GameObject* object, bool was_queued) {
	if (!session_open || layer != play || !object || was_queued) return;
	auto* queued = layer->m_objectsToDeactivate;
	if (queued && queued->objectForKey(object->m_uniqueID)) deactivated_ids.push_back(object->m_uniqueID);
}

void PredictedWorld::enable_hooks(bool on) {
	if (on == hooks_on) return;
	for (auto* hook : g_world_hooks)
		(void)(on ? hook->enable() : hook->disable());
	hooks_on = on;
	if (!on) invalidate();
}

static void add_world_hook(auto& self, const char* name) {
	auto hook = self.getHook(name);
	if (!hook) return;
	hook.unwrap()->setAutoEnable(false);
	hook.unwrap()->setPriority(Priority::First);
	g_world_hooks.push_back(hook.unwrap());
}

class $modify(WorldSections, GJBaseGameLayer) {
	static void onModify(auto& self) {
		add_world_hook(self, "GJBaseGameLayer::removeObjectFromSection");
		add_world_hook(self, "GJBaseGameLayer::addToSection");
		add_world_hook(self, "GJBaseGameLayer::updateObjectSection");
		add_world_hook(self, "GJBaseGameLayer::moveObjects");
		add_world_hook(self, "GJBaseGameLayer::rotateObjects");
		add_world_hook(self, "GJBaseGameLayer::rotateObject");
		add_world_hook(self, "GJBaseGameLayer::toggleGroup");
		add_world_hook(self, "GJBaseGameLayer::processAreaEffects");
		add_world_hook(self, "GJBaseGameLayer::processAreaMoveGroupAction");
		add_world_hook(self, "GJBaseGameLayer::processAreaRotateGroupAction");
		add_world_hook(self, "GJBaseGameLayer::processAreaTransformGroupAction");
	}

	void removeObjectFromSection(GameObject* object) {
		auto& world = PredictedWorld::get();
		if (world.running() && object && far_away(object)) return;
		world.before_section_change(this, object);
		GJBaseGameLayer::removeObjectFromSection(object);
	}

	void addToSection(GameObject* object) {
		auto& world = PredictedWorld::get();
		if (world.running() && object && far_away(object)) return;
		world.before_section_add(this, object);
		GJBaseGameLayer::addToSection(object);
	}

	void updateObjectSection(GameObject* object) {
		auto& world = PredictedWorld::get();
		if (!world.running()) {
			GJBaseGameLayer::updateObjectSection(object);
			return;
		}
		if (object && far_away(object)) return;
		bool was_queued = object && m_objectsToDeactivate && m_objectsToDeactivate->objectForKey(object->m_uniqueID);
		GJBaseGameLayer::updateObjectSection(object);
		world.note_deactivated(this, object, was_queued);
	}

	void moveObjects(CCArray* objects, double dx, double dy, bool lockPlayerY) {
		PredictedWorld::get().note_moved(this, objects);
		GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
	}

	void rotateObjects(CCArray* objects, float rotation, CCPoint position, CCPoint offset, bool finished, bool unused) {
		PredictedWorld::get().note_moved(this, objects);
		GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
	}

	void rotateObject(GameObject* object, float rotation) {
		PredictedWorld::get().note_moved_one(this, object);
		GJBaseGameLayer::rotateObject(object, rotation);
	}

	void toggleGroup(int group, bool activate) {
		PredictedWorld::get().note_toggle(this, group);
		if (Logic::get().trace_logging) PredictedWorld::get().log_toggle(this, group, activate);
		GJBaseGameLayer::toggleGroup(group, activate);
	}

	void processAreaEffects(gd::vector<EnterEffectInstance>* effects, GJAreaActionType type, float dt, bool visibleFrame) {
		if (PredictedWorld::get().running() && (type == GJAreaActionType::Fade || type == GJAreaActionType::Tint)) return;
		GJBaseGameLayer::processAreaEffects(effects, type, dt, visibleFrame);
	}

	void processAreaMoveGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
		PredictedWorld::get().note_area(this, objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
		GJBaseGameLayer::processAreaMoveGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
	}

	void processAreaRotateGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
		PredictedWorld::get().note_area(this, objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
		GJBaseGameLayer::processAreaRotateGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
	}

	void processAreaTransformGroupAction(CCArray* objects, EnterEffectInstance* instance, CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
		PredictedWorld::get().note_area(this, objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
		GJBaseGameLayer::processAreaTransformGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
	}
};
