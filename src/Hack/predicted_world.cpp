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
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

using namespace geode::prelude;

#define POSITION_TOLERANCE 0.3
#define TIMELINE_SECONDS 30.0
#define RECORD_BYTES_LIMIT (96u << 20)

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
};
#undef OBJECT_FIELD_SPAN

#define COMPACT_FIRST_SPAN 2

static const std::size_t COMPACT_POSITION_OFFSET = offsetof(GameObject, m_positionX) - SAVED_FIELD_SPANS[COMPACT_FIRST_SPAN].begin;
#pragma clang diagnostic pop

static const std::size_t RECORD_SIZE = [] {
	std::size_t total = 0;
	for (const auto& span : SAVED_FIELD_SPANS)
		total += span.size;
	return total;
}();

static const std::size_t COMPACT_SIZE = [] {
	std::size_t total = 0;
	for (std::size_t i = COMPACT_FIRST_SPAN; i < SAVED_FIELD_SPANS.size(); ++i)
		total += SAVED_FIELD_SPANS[i].size;
	return total;
}();

static const std::size_t ENTRY_SIZE = sizeof(unsigned) + 2 * sizeof(float) + COMPACT_SIZE;

static void save_object_fields(const GameObject* object, unsigned char* out) {
	const auto* raw = reinterpret_cast<const unsigned char*>(object);
	for (const auto& span : SAVED_FIELD_SPANS) {
		std::memcpy(out, raw + span.begin, span.size);
		out += span.size;
	}
}

static void load_object_fields(GameObject* object, const unsigned char* in) {
	auto* raw = reinterpret_cast<unsigned char*>(object);
	for (const auto& span : SAVED_FIELD_SPANS) {
		std::memcpy(raw + span.begin, in, span.size);
		in += span.size;
	}
	object->m_isObjectRectDirty = true;
	object->m_isOrientedBoxDirty = true;
}

static void save_compact_fields(const GameObject* object, unsigned char* out) {
	const auto* raw = reinterpret_cast<const unsigned char*>(object);
	for (std::size_t i = COMPACT_FIRST_SPAN; i < SAVED_FIELD_SPANS.size(); ++i) {
		std::memcpy(out, raw + SAVED_FIELD_SPANS[i].begin, SAVED_FIELD_SPANS[i].size);
		out += SAVED_FIELD_SPANS[i].size;
	}
}

static void load_compact_fields(GameObject* object, const unsigned char* in) {
	auto* raw = reinterpret_cast<unsigned char*>(object);
	for (std::size_t i = COMPACT_FIRST_SPAN; i < SAVED_FIELD_SPANS.size(); ++i) {
		std::memcpy(raw + SAVED_FIELD_SPANS[i].begin, in, SAVED_FIELD_SPANS[i].size);
		in += SAVED_FIELD_SPANS[i].size;
	}
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

static void place_node(GameObject* object, CCPoint position) {
	if (!object->CCNode::getPosition().equals(position)) object->CCNode::setPosition(position);
	if (auto* detail = object->m_colorSprite; detail && !object->m_colorSpriteLocked && !detail->getPosition().equals(position))
		detail->setPosition(position);
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
		entry.horizon_counter = object->m_enabledGroupsCounter;
		entry.horizon_group_disabled = object->m_isGroupDisabled;
		entry.horizon_group_disabled_temp = object->m_isGroupDisabledTemp;
		live_fields.resize(live_fields.size() + RECORD_SIZE);
		horizon_fields.resize(horizon_fields.size() + RECORD_SIZE);
		latest_fields.resize(latest_fields.size() + COMPACT_SIZE);
		expected_fields.resize(expected_fields.size() + COMPACT_SIZE);
		save_object_fields(object, horizon_fields.data() + (std::size_t)at * RECORD_SIZE);
		save_compact_fields(object, latest_fields.data() + (std::size_t)at * COMPACT_SIZE);
		save_compact_fields(object, expected_fields.data() + (std::size_t)at * COMPACT_SIZE);
	}
	touch_session(at);
	return at;
}

void PredictedWorld::touch_session(unsigned at) {
	auto& entry = tracked[at];
	if (entry.session == session_round) return;
	entry.session = session_round;
	session_list.push_back(at);
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
	entry.live_counter = object->m_enabledGroupsCounter;
	entry.live_group_disabled = object->m_isGroupDisabled;
	entry.live_group_disabled_temp = object->m_isGroupDisabledTemp;
	entry.applied_offset = UINT_MAX;
	entry.pending_offset = UINT_MAX;
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

void PredictedWorld::save_command_groups(GJBaseGameLayer* layer) {
	for (const auto& command : shadow->m_unkVector560) {
		save_group(layer, command.m_targetGroupID);
		save_group(layer, command.m_centerGroupID);
	}
	for (auto* follow : shadow->m_unkVector6f0)
		if (follow) save_group(layer, follow->getTag());
	const auto& state = layer->m_gameState;
	for (const auto& action : state.m_dynamicMoveActions) {
		save_group(layer, action.m_targetGroupID);
		save_group(layer, action.m_centerGroupID);
	}
	for (const auto& action : state.m_dynamicRotateActions) {
		save_group(layer, action.m_targetGroupID);
		save_group(layer, action.m_centerGroupID);
	}
}

PredictedWorld::SavedCell* PredictedWorld::save_cell(GJBaseGameLayer* layer, int x, int y) {
	if (x < 0 || y < 0) return nullptr;
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
	for (unsigned at : session_list) {
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
		object->m_enabledGroupsCounter = entry.live_counter;
		object->m_isGroupDisabled = entry.live_group_disabled;
		object->m_isGroupDisabledTemp = entry.live_group_disabled_temp;
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
	for (auto& trigger : fired)
		std::memcpy(reinterpret_cast<unsigned char*>(trigger.object) + sizeof(GameObject), trigger.live_bytes.data(), trigger.live_bytes.size());
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
	if (!session_open) drop_shadow();
}

bool PredictedWorld::usable(PlayLayer* play, int now, float delta) const {
	if (!valid || play != this->play || delta != step_delta || now < start_step || now > horizon_step) return false;
	return (double)(now - start_step) < Logic::get().sample_rate() * TIMELINE_SECONDS;
}

bool PredictedWorld::full() const {
	return record_bytes.size() - record_front >= RECORD_BYTES_LIMIT;
}

bool PredictedWorld::matches_live(PlayLayer* play, int now) {
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
		if (object->m_isDecoration2) continue;
		CCPoint expected = compact_position(expected_fields.data() + at * COMPACT_SIZE);
		if (std::fabs((double)expected.x - object->m_positionX) > POSITION_TOLERANCE || std::fabs((double)expected.y - object->m_positionY) > POSITION_TOLERANCE) {
			watch_uid = object->m_uniqueID;
			mismatch = fmt::format("object uid={} id={} expected=({:.3f},{:.3f}) live=({:.3f},{:.3f})", object->m_uniqueID, object->m_objectID, expected.x, expected.y,
				object->m_positionX, object->m_positionY);
			return false;
		}
	}
	return true;
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
			out += fmt::format(" cmd(type={} target={} center={} lockY={} elapsed={:.3f} dur={:.3f})", command.m_commandType, command.m_targetGroupID, command.m_centerGroupID, command.m_lockToPlayerY, command.m_deltaTime, command.m_duration);
	};
	describe("live", play->m_effectManager);
	describe("shadow", shadow);
	return out;
}

std::string PredictedWorld::near_report(CCPoint at, float reach) const {
	std::string out;
	for (std::size_t slot = 0; slot < tracked.size(); ++slot) {
		auto* object = tracked[slot].object;
		CCPoint expected = compact_position(expected_fields.data() + slot * COMPACT_SIZE);
		float live_x = (float)object->m_positionX, live_y = (float)object->m_positionY;
		bool close = std::fabs(expected.x - at.x) <= reach && std::fabs(expected.y - at.y) <= reach;
		if (!close) close = std::fabs(live_x - at.x) <= reach && std::fabs(live_y - at.y) <= reach;
		if (!close) continue;
		out += fmt::format(" [uid={} id={} exp=({:.2f},{:.2f}) live=({:.2f},{:.2f})]", object->m_uniqueID, object->m_objectID, expected.x, expected.y, live_x, live_y);
		if (out.size() > 1500) break;
	}
	return out;
}

void PredictedWorld::trim(int now) {
	if (!valid) return;
	while (!step_records.empty() && step_records.front().step <= now) {
		const auto& first = step_records.front();
		const auto* in = record_bytes.data() + first.begin;
		for (unsigned i = 0; i < first.count; ++i, in += ENTRY_SIZE) {
			unsigned at = 0;
			std::memcpy(&at, in, sizeof(at));
			std::memcpy(expected_fields.data() + (std::size_t)at * COMPACT_SIZE, in + sizeof(unsigned) + 2 * sizeof(float), COMPACT_SIZE);
		}
		record_front = first.begin + first.count * (unsigned)ENTRY_SIZE;
		step_records.erase(step_records.begin());
	}
	if (step_records.empty()) {
		record_bytes.clear();
		record_front = 0;
	}
	else if (record_front > record_bytes.size() / 2) {
		record_bytes.erase(record_bytes.begin(), record_bytes.begin() + record_front);
		for (auto& records : step_records)
			records.begin -= record_front;
		record_front = 0;
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
	++round;
	tracked.clear();
	live_fields.clear();
	horizon_fields.clear();
	latest_fields.clear();
	expected_fields.clear();
	record_bytes.clear();
	record_front = 0;
	step_records.clear();
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

void PredictedWorld::begin_session(PlayLayer* play) {
	if (session_open) end_session(this->play);
	if (!valid || play != this->play) return;
	session_open = true;
	horizon_loaded = false;
	++session_round;
	session_list.clear();
	applied.clear();
	pending.clear();
	cells.clear();
	deactivated_ids.clear();
	for (auto& entry : tracked)
		entry.applied_offset = entry.pending_offset = UINT_MAX;
	save_list(calc_non_effect, &play->m_calcNonEffectObjects, &play->m_calcNonEffectObjectsSize);
	save_list(calc_blocks, &play->m_calcCollisionBlockObjects, &play->m_calcCollisionBlockObjectsSize);
	moved_count = play->m_movedCount;
	live_spawn_tuples = play->m_spawnTuples;
	for (auto& trigger : fired) {
		const auto* raw = reinterpret_cast<const unsigned char*>(trigger.object) + sizeof(GameObject);
		trigger.live_bytes.assign(raw, raw + trigger.live_bytes.size());
	}
	for (auto& roll : rolls)
		roll.live = *roll.state;
	grouped_positions.clear();
	auto* objects = play->m_objects;
	for (unsigned i = 0; objects && i < objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
		if (object && object->m_groupCount > 0) grouped_positions.push_back({ object, { (float)object->m_positionX, (float)object->m_positionY } });
	}
}

void PredictedWorld::resume_horizon(PlayLayer* play) {
	if (!session_open || horizon_loaded || play != this->play) return;
	horizon_loaded = true;
	for (unsigned at = 0; at < tracked.size(); ++at) {
		touch_session(at);
		auto& entry = tracked[at];
		load_object_fields(entry.object, horizon_fields.data() + (std::size_t)at * RECORD_SIZE);
		entry.object->m_enabledGroupsCounter = entry.horizon_counter;
		entry.object->m_isGroupDisabled = entry.horizon_group_disabled;
		entry.object->m_isGroupDisabledTemp = entry.horizon_group_disabled_temp;
		if (!far_away(entry.object)) play->updateObjectSection(entry.object);
	}
	for (auto& trigger : fired)
		std::memcpy(reinterpret_cast<unsigned char*>(trigger.object) + sizeof(GameObject), trigger.horizon_bytes.data(), trigger.horizon_bytes.size());
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
	for (unsigned at = 0; at < tracked.size(); ++at) {
		auto& entry = tracked[at];
		save_object_fields(entry.object, horizon_fields.data() + (std::size_t)at * RECORD_SIZE);
		entry.horizon_counter = entry.object->m_enabledGroupsCounter;
		entry.horizon_group_disabled = entry.object->m_isGroupDisabled;
		entry.horizon_group_disabled_temp = entry.object->m_isGroupDisabledTemp;
	}
	for (auto& trigger : fired) {
		const auto* raw = reinterpret_cast<const unsigned char*>(trigger.object) + sizeof(GameObject);
		trigger.horizon_bytes.assign(raw, raw + trigger.live_bytes.size());
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
	StepRecords records;
	records.step = horizon_step;
	records.begin = (unsigned)record_bytes.size();
	unsigned char compact[256];
	for (unsigned at = 0; at < tracked.size(); ++at) {
		save_compact_fields(tracked[at].object, compact);
		auto* latest = latest_fields.data() + (std::size_t)at * COMPACT_SIZE;
		if (std::memcmp(compact, latest, COMPACT_SIZE) == 0) continue;
		std::memcpy(latest, compact, COMPACT_SIZE);
		CCPoint position = compact_position(compact);
		std::size_t end = record_bytes.size();
		record_bytes.resize(end + ENTRY_SIZE);
		auto* out = record_bytes.data() + end;
		std::memcpy(out, &at, sizeof(at));
		std::memcpy(out + sizeof(unsigned), &position.x, sizeof(float));
		std::memcpy(out + sizeof(unsigned) + sizeof(float), &position.y, sizeof(float));
		std::memcpy(out + sizeof(unsigned) + 2 * sizeof(float), compact, COMPACT_SIZE);
		++records.count;
	}
	step_records.push_back(records);
}

void PredictedWorld::step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2, bool last_step) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	auto* live_manager = play->m_effectManager;
	auto* real1 = play->m_player1;
	auto* real2 = play->m_player2;
	play->m_effectManager = shadow;
	play->m_player1 = driver1;
	if (driver2) play->m_player2 = driver2;
	inside_step = true;
	save_command_groups(play);
	auto& state = play->m_gameState;
	CCPoint at = driver1->getPosition();
	if (!driver_known) {
		driver_known = true;
		driver_start = at;
	}
	float dx = at.x - driver1->m_lastPosition.x, dy = at.y - driver1->m_lastPosition.y;
	if (std::abs(dy - driver1->m_yVelocityRelated3) > step_delta * 16.f) dy = 0.f;
	shadow->m_unk780 = dx;
	shadow->m_unk784 = dy;
	CCPoint ahead = at - driver_start;
	CCPoint shift = driver1->m_isSideways ? CCPoint{ 0.f, ahead.y } : CCPoint{ ahead.x, 0.f };
	state.m_cameraPosition = camera_start + shift;
	state.m_cameraPosition2 = camera2_start + shift;
	shadow->updateSpawnTriggers(step_delta);
	shadow->updateTimers(step_delta, state.m_timeWarp);
	shadow->prepareMoveActions(step_delta, !last_step);
	play->processMoveActionsStep(step_delta, last_step);
	shadow->postMoveActions();
	play->updateCollisionBlocks();
	inside_step = false;
	play->m_player1 = real1;
	play->m_player2 = real2;
	play->m_effectManager = live_manager;
	++horizon_step;
	record_moved();
	if (Logic::get().trace_logging && watch_uid >= 0 && horizon_step - start_step <= 12 && (std::size_t)watch_uid < slot_of_uid.size() && slot_round[(std::size_t)watch_uid] == round) {
		auto* object = tracked[slot_of_uid[(std::size_t)watch_uid]].object;
		Trajectory::log_line(fmt::format("WSTEP step={} uid={} pos=({:.3f},{:.3f}) driver=({:.3f},{:.3f}) dy={:.4f} commands={} follows={}", horizon_step, watch_uid, object->m_positionX, object->m_positionY,
			at.x, at.y, dy, shadow->m_unkVector560.size(), shadow->m_unkVector6f0.size()));
	}
}

void PredictedWorld::fire_spawn_triggers(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	auto* live_manager = play->m_effectManager;
	auto* real1 = play->m_player1;
	auto* real2 = play->m_player2;
	play->m_effectManager = shadow;
	play->m_player1 = driver1;
	if (driver2) play->m_player2 = driver2;
	inside_step = true;
	play->checkSpawnObjects();
	inside_step = false;
	play->m_player1 = real1;
	play->m_player2 = real2;
	play->m_effectManager = live_manager;
}

void PredictedWorld::after_players(PlayLayer* play) {
	if (!session_open || !horizon_loaded || play != this->play) return;
	auto* live_manager = play->m_effectManager;
	play->m_effectManager = shadow;
	inside_step = true;
	shadow->postCollisionCheck();
	inside_step = false;
	play->m_effectManager = live_manager;
}

void PredictedWorld::apply_record(PlayLayer* play, unsigned at, unsigned offset) {
	touch_session(at);
	auto& entry = tracked[at];
	load_compact_fields(entry.object, record_bytes.data() + offset + sizeof(unsigned) + 2 * sizeof(float));
	if (!far_away(entry.object)) play->updateObjectSection(entry.object);
	if (entry.applied_offset == UINT_MAX) applied.push_back(at);
	entry.applied_offset = offset;
}

void PredictedWorld::apply_records(PlayLayer* play, int step, const CCRect& window) {
	if (!session_open || horizon_loaded || play != this->play) return;
	auto close = [&](unsigned at, unsigned offset) {
		float x = 0.f, y = 0.f;
		std::memcpy(&x, record_bytes.data() + offset + sizeof(unsigned), sizeof(float));
		std::memcpy(&y, record_bytes.data() + offset + sizeof(unsigned) + sizeof(float), sizeof(float));
		if (window.containsPoint({ x, y })) return true;
		auto* object = tracked[at].object;
		return window.containsPoint({ (float)object->m_positionX, (float)object->m_positionY });
	};
	if (!step_records.empty() && step >= step_records.front().step) {
		std::size_t index = (std::size_t)(step - step_records.front().step);
		if (index < step_records.size()) {
			const auto& records = step_records[index];
			for (unsigned i = 0; i < records.count; ++i) {
				unsigned offset = records.begin + i * (unsigned)ENTRY_SIZE;
				unsigned at = 0;
				std::memcpy(&at, record_bytes.data() + offset, sizeof(at));
				if (close(at, offset)) {
					apply_record(play, at, offset);
					tracked[at].pending_offset = UINT_MAX;
					continue;
				}
				if (tracked[at].pending_offset == UINT_MAX) pending.push_back(at);
				tracked[at].pending_offset = offset;
			}
		}
	}
	for (std::size_t i = 0; i < pending.size();) {
		unsigned at = pending[i];
		unsigned offset = tracked[at].pending_offset;
		if (offset != UINT_MAX && !close(at, offset)) {
			++i;
			continue;
		}
		if (offset != UINT_MAX) apply_record(play, at, offset);
		tracked[at].pending_offset = UINT_MAX;
		pending[i] = pending.back();
		pending.pop_back();
	}
}

void PredictedWorld::reapply_records(PlayLayer* play) {
	if (!session_open || horizon_loaded || play != this->play) return;
	for (unsigned at : applied)
		if (tracked[at].applied_offset != UINT_MAX) apply_record(play, at, tracked[at].applied_offset);
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
			object->m_positionX = position.x;
			object->m_positionY = position.y;
			object->m_isObjectRectDirty = true;
			object->m_isOrientedBoxDirty = true;
			place_node(object, position);
		}
	}
	session_open = false;
	horizon_loaded = false;
	inside_step = false;
	session_list.clear();
	applied.clear();
	pending.clear();
	cells.clear();
	grouped_positions.clear();
	if (!valid) drop_shadow();
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
			if (slot(object) == UINT_MAX) {
				live_unknown = true;
				return;
			}
		}
		return;
	}
	for (unsigned i = 0; i < objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(i));
		if (!object) break;
		touch(object);
	}
}

void PredictedWorld::note_toggle(GJBaseGameLayer* layer, int group) {
	if (!session_open || layer != play) return;
	if (group <= 0 || (std::size_t)group >= layer->m_groups.size()) return;
	auto* members = layer->m_groups[(std::size_t)group];
	for (unsigned i = 0; members && i < members->count(); ++i) {
		auto* object = static_cast<GameObject*>(members->objectAtIndex(i));
		if (object) touch(object);
	}
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
	g_world_hooks.push_back(hook.unwrap());
}

class $modify(WorldSections, GJBaseGameLayer) {
	static void onModify(auto& self) {
		add_world_hook(self, "GJBaseGameLayer::removeObjectFromSection");
		add_world_hook(self, "GJBaseGameLayer::addToSection");
		add_world_hook(self, "GJBaseGameLayer::updateObjectSection");
		add_world_hook(self, "GJBaseGameLayer::moveObjects");
		add_world_hook(self, "GJBaseGameLayer::rotateObjects");
		add_world_hook(self, "GJBaseGameLayer::toggleGroupTriggered");
		add_world_hook(self, "GJBaseGameLayer::processAdvancedFollowActions");
		add_world_hook(self, "GJBaseGameLayer::processAreaEffects");
		add_world_hook(self, "GJBaseGameLayer::processAreaMoveGroupAction");
		add_world_hook(self, "GJBaseGameLayer::processAreaRotateGroupAction");
		add_world_hook(self, "GJBaseGameLayer::processAreaTransformGroupAction");
	}

	void removeObjectFromSection(GameObject* object) {
		PredictedWorld::get().before_section_change(this, object);
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

	void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
		PredictedWorld::get().note_toggle(this, group);
		GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
	}

	void processAdvancedFollowActions(float dt) {
		if (PredictedWorld::get().running()) return;
		GJBaseGameLayer::processAdvancedFollowActions(dt);
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
