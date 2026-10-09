#include "Hack/level_forecast.hpp"
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
#ifdef GEODE_IS_WINDOWS
#include <windows.h>
#endif

using namespace geode::prelude;

// How a forecast step reaches the objects, on Windows 2.2081 (GJBaseGameLayer::update+0xb10):
// GJEffectManager::prepareMoveActions(dt, !visible) steps every group command, processMoveActionsStep
// moves, rotates, scales and offsets the target groups (moveObjects, rotateObjects, the area and
// follow actions), and postMoveActions finishes them. dt is the step in seconds (not the 60-based
// delta the players get).
// Every one of those calls updateObjectSection on each object it changes, which is how a forecast
// step learns what moved.
//
// What that changes, and how it comes back:
// - each object's gameplay fields (position and area offsets, rotation, scale, last position and the
//   step stamps moveObjects keys on, the hitbox caches): saved spans, written back;
// - the nodes rotateObjects and the transform actions set (rotation, scale, opacity): the setters;
// - the section grid. addToSection appends to the object's bucket (slot = old count) and
//   removeObjectFromSection swaps the last entry into the hole, both in the main grid and in one
//   side list (non-effect objects, collision blocks, or the two "calc" lists). Each bucket is
//   copied the first time anything touches it, and restoring a bucket rewrites every object's
//   slot indices from its place in it;
// - updateObjectSection queues objects in m_objectsToDeactivate; the added entries are removed;
// - m_movedCount and the game state (m_commandIndex advances once per step).
// The effect manager is never touched: the forecast runs on a shadow loaded with saveToState /
// loadFromState, and the layer's pointer is swapped to it only while a step runs.
namespace {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
struct Span {
	std::uint16_t begin;
	std::uint16_t size;
};
#define ECHO_SPAN(first, last)                                                    \
	Span{static_cast<std::uint16_t>(offsetof(GameObject, first)),                \
		static_cast<std::uint16_t>(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first))}
constexpr std::array kSpans = {
	ECHO_SPAN(m_positionXOffset, m_scaleYOffset),
	ECHO_SPAN(m_isDirty, m_isOrientedBoxDirty),
	ECHO_SPAN(m_unmodifiedPositionX, m_positionY),
	ECHO_SPAN(m_startRotationX, m_customScaleY),
	// m_startFlipX, m_startFlipY and the "queued to deactivate" byte updateObjectSection sets.
	Span{static_cast<std::uint16_t>(offsetof(GameObject, m_startFlipX)), 4},
	ECHO_SPAN(m_scaleX, m_scaleY),
	ECHO_SPAN(m_lastPosition, m_unk4CC),
	// The decoration flags and the byte after them, which the move pipeline sets.
	Span{static_cast<std::uint16_t>(offsetof(GameObject, m_useTextureRectForSelection)), 4},
};
#undef ECHO_SPAN
#pragma clang diagnostic pop

constexpr std::size_t kStride = [] {
	std::size_t total = 0;
	for (auto const& span : kSpans) total += span.size;
	return total;
}();

// Where m_positionX sits in a record.
constexpr std::size_t kRecordPosition = [] {
	std::size_t at = 0;
	for (auto const& span : kSpans) {
		if (span.begin <= offsetof(GameObject, m_positionX) && offsetof(GameObject, m_positionX) < span.begin + span.size)
			return at + (offsetof(GameObject, m_positionX) - span.begin);
		at += span.size;
	}
	return std::size_t{0};
}();

void gather(GameObject const* object, std::uint8_t* out) {
	auto const* raw = reinterpret_cast<std::uint8_t const*>(object);
	for (auto const& span : kSpans) {
		std::memcpy(out, raw + span.begin, span.size);
		out += span.size;
	}
}

void scatter(GameObject* object, std::uint8_t const* in) {
	auto* raw = reinterpret_cast<std::uint8_t*>(object);
	for (auto const& span : kSpans) {
		std::memcpy(raw + span.begin, in, span.size);
		in += span.size;
	}
}

struct Node {
	float rotationX, rotationY, scaleX, scaleY;
	GLubyte opacity;
};

Node nodeOf(GameObject* object) {
	return {object->getRotationX(), object->getRotationY(), object->getScaleX(), object->getScaleY(), object->getOpacity()};
}

// The section grid, as GD indexes it (addToSection / removeObjectFromSection).
using Bucket = gd::vector<GameObject*>;
using Column = gd::vector<Bucket*>;
using Grid = gd::vector<Column*>;
using Counts = gd::vector<gd::vector<int>*>;

Bucket* bucketAt(Grid const& grid, int x, int y) {
	if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= grid.size() || !grid[x]) return nullptr;
	auto& column = *grid[x];
	return static_cast<std::size_t>(y) < column.size() ? column[y] : nullptr;
}

int* countAt(Counts const& counts, int x, int y) {
	if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= counts.size() || !counts[x]) return nullptr;
	auto& column = *counts[x];
	return static_cast<std::size_t>(y) < column.size() ? &column[y] : nullptr;
}

struct ListCopy {
	std::vector<GameObject*> items;
	int count = 0;
	bool present = false;
};

void copyList(ListCopy& out, Bucket const* bucket, int const* count) {
	out.present = bucket && count;
	if (!out.present) return;
	out.count = *count;
	int used = std::clamp(*count, 0, static_cast<int>(bucket->size()));
	out.items.assign(bucket->begin(), bucket->begin() + used);
}

// Writes a copy back and returns how many entries it holds. `slot` names the index field to fix.
template <class Fix>
void putList(ListCopy const& copy, Bucket* bucket, int* count, Fix fix) {
	if (!copy.present || !bucket || !count) return;
	if (bucket->size() < copy.items.size()) bucket->resize(copy.items.size());
	std::copy(copy.items.begin(), copy.items.end(), bucket->begin());
	*count = copy.count;
	for (std::size_t i = 0; i < copy.items.size(); ++i) {
		if (copy.items[i]) fix(copy.items[i], static_cast<int>(i));
	}
}

struct Cell {
	int x = 0, y = 0;
	ListCopy main, nonEffect, blocks;
	bool flag = false;
	bool flagPresent = false;
};

struct Sections {
	bool active = false;
	GJBaseGameLayer* layer = nullptr;
	std::unordered_map<std::uint64_t, Cell> cells;
	ListCopy calcNonEffect, calcBlocks;
	std::vector<int> deactivated;
	int movedCount = 0;
	// The object updateObjectSection is moving: whether it was queued to deactivate when its old
	// section let it go (only an object that changes section can be queued).
	GameObject* probe = nullptr;
	bool probed = false;
	bool probeHad = false;

	static std::uint64_t key(int x, int y) {
		return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
	}

	Grid& grid() { return *reinterpret_cast<Grid*>(&layer->m_sections); }
	Grid& nonEffectGrid() { return *reinterpret_cast<Grid*>(&layer->m_nonEffectObjects); }
	// m_collisionBlockSections is declared one level shallower than addToSection uses it.
	Grid& blockGrid() { return *reinterpret_cast<Grid*>(&layer->m_collisionBlockSections); }
	Counts& counts() { return *reinterpret_cast<Counts*>(&layer->m_sectionSizes); }
	Counts& nonEffectCounts() { return *reinterpret_cast<Counts*>(&layer->m_nonEffectObjectsSizes); }
	Counts& blockCounts() { return *reinterpret_cast<Counts*>(&layer->m_collisionBlockSectionSizes); }

	void begin(GJBaseGameLayer* play) {
		active = true;
		layer = play;
		cells.clear();
		deactivated.clear();
		copyList(calcNonEffect, &play->m_calcNonEffectObjects, &play->m_calcNonEffectObjectsSize);
		copyList(calcBlocks, &play->m_calcCollisionBlockObjects, &play->m_calcCollisionBlockObjectsSize);
		movedCount = play->m_movedCount;
	}

	bool flagAt(int x, int y, bool& present) {
		auto& flags = layer->m_nonEffectObjectsFlags;
		present = x >= 0 && y >= 0 && static_cast<std::size_t>(x) < flags.size() && flags[x] && static_cast<std::size_t>(y) < flags[x]->size();
		return present && (*flags[x])[y];
	}

	// Copies a bucket before anything in this session changes it.
	Cell* note(int x, int y) {
		if (!active || x < 0 || y < 0) return nullptr;
		auto [it, fresh] = cells.try_emplace(key(x, y));
		if (!fresh) return nullptr;
		auto& cell = it->second;
		cell.x = x;
		cell.y = y;
		copyList(cell.main, bucketAt(grid(), x, y), countAt(counts(), x, y));
		copyList(cell.nonEffect, bucketAt(nonEffectGrid(), x, y), countAt(nonEffectCounts(), x, y));
		copyList(cell.blocks, bucketAt(blockGrid(), x, y), countAt(blockCounts(), x, y));
		cell.flag = flagAt(x, y, cell.flagPresent);
		return &cell;
	}

	// After an append to a bucket nobody had copied yet: the copy is the bucket without its last entry.
	void noteAppended(GameObject* object) {
		if (!active) return;
		int x = object->m_outerSectionIndex;
		int y = object->m_middleSectionIndex;
		if (cells.contains(key(x, y))) return;
		auto* cell = note(x, y);
		if (!cell) return;
		auto drop = [object](ListCopy& list) {
			if (list.present && !list.items.empty() && list.items.back() == object) {
				list.items.pop_back();
				--list.count;
			}
		};
		drop(cell->main);
		drop(cell->nonEffect);
		drop(cell->blocks);
		// addToSection sets the non-effect flag; a bucket that was empty had it clear.
		if (cell->nonEffect.present && cell->nonEffect.items.empty()) cell->flag = false;
	}

	void end() {
		if (!active) return;
		active = false;
		for (auto& [id, cell] : cells) {
			int x = cell.x;
			int y = cell.y;
			putList(cell.main, bucketAt(grid(), x, y), countAt(counts(), x, y), [x, y](GameObject* object, int slot) {
				object->m_someOtherIndex = slot;
				object->m_outerSectionIndex = x;
				object->m_middleSectionIndex = y;
			});
			auto side = [](GameObject* object, int slot) { object->m_innerSectionIndex = slot; };
			putList(cell.nonEffect, bucketAt(nonEffectGrid(), x, y), countAt(nonEffectCounts(), x, y), side);
			putList(cell.blocks, bucketAt(blockGrid(), x, y), countAt(blockCounts(), x, y), side);
			bool present = false;
			flagAt(x, y, present);
			if (present && cell.flagPresent) (*layer->m_nonEffectObjectsFlags[x])[y] = cell.flag;
		}
		auto side = [](GameObject* object, int slot) { object->m_innerSectionIndex = slot; };
		putList(calcNonEffect, &layer->m_calcNonEffectObjects, &layer->m_calcNonEffectObjectsSize, side);
		putList(calcBlocks, &layer->m_calcCollisionBlockObjects, &layer->m_calcCollisionBlockObjectsSize, side);
		if (auto* queued = layer->m_objectsToDeactivate) {
			for (int id : deactivated) queued->removeObjectForKey(id);
		}
		layer->m_movedCount = movedCount;
		cells.clear();
		deactivated.clear();
	}
};

// How far a live object may stand from its forecast before the forecast is built again. Objects
// locked to the player follow its predicted path, which drifts by a few hundredths of a unit.
constexpr double kTolerance = 0.3;
// At most this many forecast steps are built in one frame (a whole build at 240 TPS took ~40 ms).
constexpr int kBuildPerFrame = 60;

struct Forecast {
	PlayLayer* play = nullptr;
	cocos2d::CCArray* array = nullptr;
	unsigned arrayCount = 0;
	std::uint64_t epoch = 0;
	std::vector<GameObject*> objects; // Every object in a group: only those can be moved.
	std::unordered_map<GameObject*, std::uint32_t> index;

	GJEffectManager* shadow = nullptr;
	bool valid = false;
	std::uint64_t origin = 0; // Live step count the forecast starts from.
	float dt = 0.f;
	int horizon = 0;
	// frames[k - 1]: (object index, record offset) for every object that moved on step k.
	std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> frames;
	// frames[k - 1] fired triggers from (and moved the player node to) firedFrom[k - 1].
	std::vector<cocos2d::CCPoint> firedFrom;
	std::uint32_t generation = 0; // Counts rebuilds: triggers fired by an older build do not count.
	std::vector<std::uint8_t> records;
	std::vector<std::uint32_t> latest; // Per object: its newest record, or UINT32_MAX.
	// Objects the player cannot touch (decoration: rotation centres, followed leaders) keep only
	// their newest state, so the next pass continues from it, and no per-step history.
	std::vector<std::uint8_t> quiet;
	std::vector<std::uint8_t> hasQuiet;

	// Live state of the objects a forecast or prediction touches.
	std::vector<std::uint8_t> base;
	std::vector<Node> nodes;
	std::vector<std::uint32_t> baseStamp;
	std::uint32_t baseEpoch = 1;

	Sections sections;
	std::vector<std::uint32_t> touched; // Objects a forecast pass changed.
	std::vector<std::uint32_t> nodeStamp; // Per object: the pass whose node it saved (touchEpoch).
	std::vector<std::uint32_t> touchStamp;
	std::uint32_t touchEpoch = 1;
	bool recording = false;
	std::vector<std::uint32_t> moved; // This step.
	std::vector<std::uint32_t> movedStamp;
	// prepare's check: per object, its newest record among the steps the live game ran (stamped).
	std::vector<std::uint32_t> lastOffset;
	std::vector<std::uint32_t> lastStamp;
	std::uint32_t lastEpoch = 1;
	std::vector<std::uint32_t> lastList;
	std::uint32_t stepEpoch = 1;

	// Motion triggers the player reaches while the forecast runs (checkSpawnObjects' own walk).
	int channel = 0;
	int cursor = 0;
	bool goingBack = false;
	// The trajectory's newest prediction of player 1: path[j] is where it is after live step
	// pathStart + j, and pathBefore where it was the step before path[0].
	std::vector<cocos2d::CCPoint> path;
	cocos2d::CCPoint pathBefore;
	std::uint64_t pathStart = 0;
	cocos2d::CCPoint playerAt;   // Player 1 at the forecast's origin.
	cocos2d::CCPoint playerStep; // Its movement per step.
	int stepsRun = 0;            // Steps since playerAt.
	struct Fired {
		EffectGameObject* object;
		std::vector<std::uint8_t> bytes;
	};
	std::vector<Fired> fired;

	// Objects the live game moved since the last prepare (updateObjectSection outside a forecast).
	std::unordered_set<GameObject*> liveMoved;

	bool applying = false;
	std::vector<std::uint32_t> pending;       // Objects with a state not yet applied (away from the prediction).
	std::vector<std::uint32_t> pendingRecord; // Per object: that state, or UINT32_MAX.
	std::vector<std::uint32_t> applied; // Objects a prediction moved.
	std::vector<std::uint32_t> appliedStamp;
	std::uint32_t applyEpoch = 1;
};

Forecast& state() {
	static Forecast value;
	return value;
}

// Groups the forecast must move even where they are decoration: the centres of rotations and the
// leaders of follows (a command's centre group), whose positions other moves read.
std::unordered_set<int>& neededGroups() {
	static std::unordered_set<int> groups;
	return groups;
}

void noteNeededGroups(GJEffectManager* manager) {
	auto& groups = neededGroups();
	groups.clear();
	if (!manager) return;
	for (auto const& command : manager->m_unkVector560)
		if (command.m_centerGroupID > 0) groups.insert(command.m_centerGroupID);
}

// Decoration nothing else reads cannot touch the player, so the forecast leaves it where it is. On
// Amethyst GD's move pipeline shifted about 12,000 objects per forecast step, nearly all decoration,
// and a forecast pass took 85-113 ms. Returns the array to move: `objects` itself when every member is
// kept, otherwise a reused array of the kept ones.
cocos2d::CCArray* forecastMembers(cocos2d::CCArray* objects) {
	if (!state().recording || !objects) return objects;
	auto const& needed = neededGroups();
	auto keep = [&](GameObject* object) {
		if (object->m_objectType != GameObjectType::Decoration) return true;
		if (needed.empty() || !object->m_groups) return false;
		for (int g = 0; g < object->m_groupCount; ++g)
			if (needed.contains((*object->m_groups)[g])) return true;
		return false;
	};
	unsigned count = objects->count();
	unsigned first = 0;
	for (; first < count; ++first) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(first));
		if (!object) return objects; // The native loops stop at the first null.
		if (!keep(object)) break;
	}
	if (first == count) return objects;
	static cocos2d::CCArray* kept = [] {
		auto* array = cocos2d::CCArray::create();
		array->retain();
		return array;
	}();
	kept->removeAllObjects();
	for (unsigned n = 0; n < count; ++n) {
		auto* object = static_cast<GameObject*>(objects->objectAtIndex(n));
		if (!object) break;
		if (n < first || (n > first && keep(object))) kept->addObject(object);
	}
	return kept;
}


std::uint64_t& liveSteps() {
	static std::uint64_t steps = 0;
	return steps;
}

bool& stale() {
	static bool flag = true;
	return flag;
}

int lastRebuildStep = 0;

// Trigger uid -> the build (generation) that fired it.
std::unordered_map<int, std::uint32_t> forecastFired;

void refreshObjects(Forecast& f, PlayLayer* play) {
	// Any change to the level's object list builds the index again: an object GD removed and freed
	// was still indexed, and the next pass read it (a crash in a respawn's trajectory refresh).
	if (f.play == play && f.array == play->m_objects && f.epoch == echo::movement::groupEpoch &&
		play->m_objects && f.arrayCount == play->m_objects->count())
		return;
	f.play = play;
	f.array = play->m_objects;
	f.arrayCount = play->m_objects ? play->m_objects->count() : 0;
	f.epoch = echo::movement::groupEpoch;
	// Indexed objects are retained, so one removed between refreshes is never freed under the index.
	for (auto* object : f.objects) object->release();
	f.objects.clear();
	f.index.clear();
	f.valid = false;
	if (!play->m_objects) return;
	for (unsigned i = 0; i < play->m_objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
		if (!object || object->m_groupCount <= 0) continue;
		f.index.emplace(object, static_cast<std::uint32_t>(f.objects.size()));
		object->retain();
		f.objects.push_back(object);
	}
	std::size_t count = f.objects.size();
	f.base.assign(count * kStride, 0);
	f.nodes.assign(count, {});
	f.baseStamp.assign(count, 0);
	f.movedStamp.assign(count, 0);
	f.appliedStamp.assign(count, 0);
	f.touchStamp.assign(count, 0);
	f.nodeStamp.assign(count, 0);
	f.pendingRecord.assign(count, UINT32_MAX);
	f.latest.assign(count, UINT32_MAX);
	f.hasQuiet.assign(count, 0);
	f.quiet.clear();
}

// Objects outside every group join the index when an area action reaches them.
std::uint32_t ensureIndex(Forecast& f, GameObject* object) {
	auto [it, fresh] = f.index.try_emplace(object, static_cast<std::uint32_t>(f.objects.size()));
	if (!fresh) return it->second;
	object->retain();
	f.objects.push_back(object);
	f.base.resize(f.objects.size() * kStride);
	f.nodes.emplace_back();
	f.baseStamp.push_back(0);
	f.movedStamp.push_back(0);
	f.appliedStamp.push_back(0);
	f.touchStamp.push_back(0);
	f.nodeStamp.push_back(0);
	f.pendingRecord.push_back(UINT32_MAX);
	f.latest.push_back(UINT32_MAX);
	f.hasQuiet.push_back(0);
	return it->second;
}

// `base` is a running copy of every indexed object's live state. It is taken once, and after that
// only the objects the live game moves (updateObjectSection outside a session) are copied again.
void cacheLive(Forecast& f, std::uint32_t i) {
	f.baseStamp[i] = 1;
	gather(f.objects[i], f.base.data() + static_cast<std::size_t>(i) * kStride);
	f.nodes[i] = nodeOf(f.objects[i]);
}

void keepBase(Forecast& f, std::uint32_t i) {
	if (!f.baseStamp[i]) cacheLive(f, i);
}

void putBase(Forecast& f, std::uint32_t i, bool node) {
	auto* object = f.objects[i];
	// Only a node this pass changed comes back, from the copy taken just before (ForecastNodes):
	// rotating decorations turn their nodes every frame, so the running copy can be behind.
	if (node && f.nodeStamp[i] == f.touchEpoch) {
		auto const& saved = f.nodes[i];
		if (object->getRotationX() != saved.rotationX) object->setRotationX(saved.rotationX);
		if (object->getRotationY() != saved.rotationY) object->setRotationY(saved.rotationY);
		if (object->getScaleX() != saved.scaleX) object->setScaleX(saved.scaleX);
		if (object->getScaleY() != saved.scaleY) object->setScaleY(saved.scaleY);
		if (object->getOpacity() != saved.opacity) object->setOpacity(saved.opacity);
	}
	scatter(object, f.base.data() + static_cast<std::size_t>(i) * kStride);
}

void touch(Forecast& f, std::uint32_t i) {
	if (f.touchStamp[i] == f.touchEpoch) return;
	f.touchStamp[i] = f.touchEpoch;
	f.touched.push_back(i);
}

std::uint32_t record(Forecast& f, std::uint32_t i) {
	auto offset = static_cast<std::uint32_t>(f.records.size());
	f.records.resize(f.records.size() + kStride);
	gather(f.objects[i], f.records.data() + offset);
	f.latest[i] = offset;
	return offset;
}

bool motionTrigger(int id) {
	switch (id) {
		case 901:  // Move
		case 1346: // Rotate
		case 1347: // Follow
		case 1814: // Follow Player Y
		case 2067: // Scale
			return true;
		default:
			return false;
	}
}

void keepTrigger(Forecast& f, EffectGameObject* object) {
	forecastFired[object->m_uniqueID] = f.generation;
	if (std::ranges::any_of(f.fired, [object](auto const& fired) { return fired.object == object; })) return;
	auto& fired = f.fired.emplace_back();
	fired.object = object;
	auto const* raw = reinterpret_cast<std::uint8_t const*>(object);
	fired.bytes.assign(raw + sizeof(CCSprite), raw + sizeof(EffectGameObject));
}

// GJBaseGameLayer::checkSpawnObjects (0x21a8f0) at the player's forecast position (`at`): walks the
// channel's x-sorted triggers from its cursor, skips group-disabled and touch-triggered ones, fires
// the rest with triggerObject(layer, 0, nullptr) and moves the cursor on. Only motion triggers fire
// here; the commands they create land on the shadow manager.
void fireAhead(Forecast& f, PlayLayer* play, cocos2d::CCPoint at) {
	auto* list = static_cast<cocos2d::CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.channel) : nullptr);
	if (!list) return;
	auto* body = play->m_player1;
	bool sideways = body && body->m_isSideways;
	while (static_cast<unsigned>(f.cursor) < list->count()) {
		auto* object = static_cast<EffectGameObject*>(list->objectAtIndex(static_cast<unsigned>(f.cursor)));
		if (!object) {
			++f.cursor;
			continue;
		}
		float key = sideways ? object->m_speedStart.y : object->m_speedStart.x;
		float player = sideways ? at.y : at.x;
		if (f.goingBack ? key < player : key > player) break;
		++f.cursor;
		bool spawn = object->m_objectID == 1268;
		if (object->m_isGroupDisabled || object->m_isTouchTriggered || !(motionTrigger(object->m_objectID) || spawn)) continue;
		keepTrigger(f, object);
		object->triggerObject(play, 0, nullptr);
	}
}

// Drops the records no frame (and no object's newest state) still uses.
void compact(Forecast& f) {
	std::size_t used = 0;
	for (auto const& frame : f.frames) used += frame.size();
	if (f.records.size() < (used + f.objects.size()) * kStride * 2 + (1u << 20)) return;
	std::vector<std::uint8_t> kept;
	kept.reserve((used + f.objects.size()) * kStride);
	std::unordered_map<std::uint32_t, std::uint32_t> moved;
	auto keep = [&](std::uint32_t offset) {
		auto [it, fresh] = moved.try_emplace(offset, static_cast<std::uint32_t>(kept.size()));
		if (fresh) kept.insert(kept.end(), f.records.begin() + offset, f.records.begin() + offset + kStride);
		return it->second;
	};
	for (auto& frame : f.frames) {
		for (auto& entry : frame) entry.second = keep(entry.second);
	}
	for (auto& offset : f.latest) {
		if (offset != UINT32_MAX) offset = keep(offset);
	}
	f.records = std::move(kept);
}

// Records past this are not worth it: the forecast stops growing and predictions use what it has.
constexpr std::size_t kRecordLimit = std::size_t{192} << 20;

// Runs `count` more steps on the shadow and records them after the existing frames.
// Player 1 after live step `absolute`: the trajectory's prediction where it has one, past its end
// the prediction's last step carried on, and with no prediction the live player's current step.
// The forecast used to carry on the player's step from its last rebuild, which a speed change
// since made wrong by hundreds of steps: triggers fired early and their objects moved too soon.
cocos2d::CCPoint playerAfter(Forecast const& f, std::uint64_t absolute, cocos2d::CCPoint node, cocos2d::CCPoint liveStep, std::uint64_t liveNow) {
	if (!f.path.empty()) {
		if (absolute + 1 == f.pathStart) return f.pathBefore;
		if (absolute >= f.pathStart) {
			auto index = absolute - f.pathStart;
			if (index < f.path.size()) return f.path[static_cast<std::size_t>(index)];
			auto last = f.path.back();
			auto step = f.path.size() >= 2 ? last - f.path[f.path.size() - 2] : last - f.pathBefore;
			return last + step * static_cast<float>(index - (f.path.size() - 1));
		}
	}
	return node + liveStep * (static_cast<float>(absolute) - static_cast<float>(liveNow));
}

void extend(Forecast& f, PlayLayer* play, int count) {
	auto& value = Logic::get();
	auto* live = play->m_effectManager;
	GJGameState saved = play->m_gameState;
	bool predicting = value.predicting;
	value.predicting = true;
	// The pipeline changes an object before telling updateObjectSection, so every object that could
	// move is saved first.
	++f.touchEpoch;
	f.touched.clear();
	for (std::uint32_t i = 0; i < f.objects.size(); ++i) keepBase(f, i);
	f.sections.begin(play);
	// The objects stand where the forecast so far leaves them.
	for (std::uint32_t i = 0; i < f.objects.size(); ++i) {
		bool quiet = f.hasQuiet[i] && f.quiet.size() >= (static_cast<std::size_t>(i) + 1) * kStride;
		if (f.latest[i] == UINT32_MAX && !quiet) continue;
		touch(f, i);
		scatter(f.objects[i], quiet ? f.quiet.data() + static_cast<std::size_t>(i) * kStride : f.records.data() + f.latest[i]);
		play->updateObjectSection(f.objects[i]);
	}
	auto tuples = play->m_spawnTuples;
	// Area effects measure from the players' nodes and the camera, so those follow the player's
	// forecast path while the steps run (only along the direction it travels; the height of a
	// jump is not forecast). CCNode::setPosition moves only the node.
	auto* p1 = play->m_player1;
	auto* p2 = play->m_player2;
	auto node1 = p1 ? p1->getPosition() : CCPoint{};
	auto node2 = p2 ? p2->getPosition() : CCPoint{};
	auto camera = play->m_gameState.m_cameraPosition;
	auto camera2 = play->m_gameState.m_cameraPosition2;
	bool sideways = p1 && p1->m_isSideways;
	auto liveNow = liveSteps();
	auto liveStep = p1 ? node1 - p1->m_lastPosition : CCPoint{};
	play->m_effectManager = f.shadow;
	f.recording = true;
	CCPoint fireFrom;
	for (int step = 0; step < count && f.records.size() < kRecordLimit; ++step) {
		++f.stepEpoch;
		f.moved.clear();
		play->m_gameState.m_commandIndex += 1;
		++f.stepsRun;
		{
			std::uint64_t absolute = f.origin + static_cast<std::uint64_t>(f.stepsRun);
			// GD fires the triggers the player passed at the end of a step (checkSpawnObjects runs
			// after the collisions), so their moves start on the next step: this step's triggers are
			// the ones at the player's position after the step before. GJBaseGameLayer::update also
			// hands the move actions player 1's movement on the step before (em+0x800, before the
			// players update), so objects locked to the player follow it.
			auto at = playerAfter(f, absolute - 1, node1, liveStep, liveNow);
			auto before = playerAfter(f, absolute - 2, node1, liveStep, liveNow);
			fireFrom = at;
			f.firedFrom.push_back(at);
			if (p1) p1->CCNode::setPosition(at);
			if (p2) p2->CCNode::setPosition(node2 + (at - node1));
			f.shadow->m_unk780 = at.x - before.x;
			f.shadow->m_unk784 = at.y - before.y;
			// Area effects measure from the camera too; it follows the player along its travel.
			auto ahead = at - node1;
			auto shift = sideways ? CCPoint{0.f, ahead.y} : CCPoint{ahead.x, 0.f};
			play->m_gameState.m_cameraPosition = camera + shift;
			play->m_gameState.m_cameraPosition2 = camera2 + shift;
		}
		fireAhead(f, play, fireFrom);
		// Delayed spawns queued on the shadow come due here, as in the live step (update+0x7c1).
		f.shadow->updateSpawnTriggers(f.dt);
		noteNeededGroups(f.shadow);
		f.shadow->prepareMoveActions(f.dt, false);
		play->processMoveActionsStep(f.dt, true);
		f.shadow->postMoveActions();
		auto& frame = f.frames.emplace_back();
		frame.reserve(f.moved.size());
		for (auto i : f.moved) {
			touch(f, i);
			if (f.objects[i]->m_objectType == GameObjectType::Decoration) {
				if (f.quiet.size() < f.objects.size() * kStride) f.quiet.resize(f.objects.size() * kStride);
				gather(f.objects[i], f.quiet.data() + static_cast<std::size_t>(i) * kStride);
				f.hasQuiet[i] = 1;
				continue;
			}
			frame.emplace_back(i, record(f, i));
		}
	}
	f.recording = false;
	play->m_effectManager = live;
	play->m_spawnTuples = tuples;
	if (p1) p1->CCNode::setPosition(node1);
	if (p2) p2->CCNode::setPosition(node2);
	// Firing a trigger can mark it; it fires for real when the player gets there.
	for (auto it = f.fired.rbegin(); it != f.fired.rend(); ++it)
		std::memcpy(reinterpret_cast<std::uint8_t*>(it->object) + sizeof(CCSprite), it->bytes.data(), it->bytes.size());
	f.fired.clear();
	for (auto i : f.touched) putBase(f, i, true);
	f.sections.end();
	play->m_gameState = saved;
	value.predicting = predicting;
	f.horizon = static_cast<int>(f.frames.size());
}

void rebuild(Forecast& f, PlayLayer* play, int steps) {
	f.liveMoved.clear();
	f.frames.clear();
	f.records.clear();
	std::fill(f.latest.begin(), f.latest.end(), UINT32_MAX);
	std::fill(f.hasQuiet.begin(), f.hasQuiet.end(), 0);
	// A rebuild follows a reset, a backstep or a forecast that went wrong: copy everything again.
	std::fill(f.baseStamp.begin(), f.baseStamp.end(), 0);
	f.horizon = 0;
	f.valid = false;
	f.firedFrom.clear();
	++f.generation;
	auto* live = play->m_effectManager;
	if (!live) return;
	EffectManagerState saved;
	live->saveToState(saved);
	f.origin = liveSteps();
	f.valid = true;
	stale() = false;
	lastRebuildStep = Logic::get().stepFrame;
	auto const& game = play->m_gameState;
	f.channel = game.m_currentChannel;
	auto cursor = game.m_spawnChannelRelated0.find(f.channel);
	f.cursor = cursor == game.m_spawnChannelRelated0.end() ? 0 : cursor->second;
	auto back = game.m_spawnChannelRelated1.find(f.channel);
	f.goingBack = back != game.m_spawnChannelRelated1.end() && back->second;
	f.stepsRun = 0;
	if (auto* body = play->m_player1) {
		f.playerAt = body->getPosition();
		f.playerStep = body->getPosition() - body->m_lastPosition;
	} else {
		f.playerAt = cocos2d::CCPoint{0.f, 0.f};
		f.playerStep = cocos2d::CCPoint{0.f, 0.f};
	}
	// Nothing is moving and no motion trigger lies ahead: every step leaves the objects as they are.
	bool ahead = false;
	if (auto* list = static_cast<cocos2d::CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.channel) : nullptr)) {
		for (unsigned i = static_cast<unsigned>(std::max(f.cursor, 0)); i < list->count() && !ahead; ++i) {
			auto* object = static_cast<EffectGameObject*>(list->objectAtIndex(i));
			ahead = object && (motionTrigger(object->m_objectID) || object->m_objectID == 1268);
		}
	}
	if (saved.m_vectorGroupCommandObject2.empty() && !ahead) {
		if (f.shadow) f.shadow->release();
		f.shadow = nullptr;
		f.frames.resize(static_cast<std::size_t>(steps));
		f.firedFrom.resize(f.frames.size(), cocos2d::CCPoint{NAN, NAN});
		f.horizon = steps;
		return;
	}
	// A fresh shadow each time: loading a state is meant for a manager that starts empty.
	if (f.shadow) f.shadow->release();
	f.shadow = GJEffectManager::create();
	if (!f.shadow) {
		f.valid = false;
		return;
	}
	f.shadow->retain();
	f.shadow->m_triggerEffectDelegate = live->m_triggerEffectDelegate;
	f.shadow->loadFromState(saved);
	// The player and camera movement the update hands the manager each step (update+0xa6a).
	f.shadow->m_unk780 = live->m_unk780;
	f.shadow->m_unk784 = live->m_unk784;
	f.shadow->m_unk788 = live->m_unk788;
	f.shadow->m_unk78C = live->m_unk78C;
	f.shadow->m_unk790 = live->m_unk790;
	f.shadow->m_unk794 = live->m_unk794;
	f.shadow->m_unk798 = live->m_unk798;
	// A build is spread over frames: the first steps now, the rest as prepare() extends it.
	extend(f, play, std::min(steps, kBuildPerFrame));
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

void noteLiveStep() {
	++liveSteps();
}

void notePlayerPath(std::vector<cocos2d::CCPoint> path, cocos2d::CCPoint before) {
	auto& f = state();
	f.path = std::move(path);
	f.pathBefore = before;
	f.pathStart = liveSteps();
}

// Whether a motion or spawn trigger in the forecast's channel sits in (lo, hi] along the travel:
// fireAhead fires a trigger once the player's position passes its key.
bool triggerBetween(Forecast const& f, PlayLayer* play, float lo, float hi, bool sideways) {
	auto* list = static_cast<cocos2d::CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(f.channel) : nullptr);
	if (!list) return false;
	auto keyOf = [&](unsigned i) {
		auto* object = static_cast<EffectGameObject*>(list->objectAtIndex(i));
		return object ? (sideways ? object->m_speedStart.y : object->m_speedStart.x) : -INFINITY;
	};
	// The list is sorted by key (checkSpawnObjects walks it with a cursor).
	unsigned low = 0, high = list->count();
	while (low < high) {
		unsigned mid = low + (high - low) / 2;
		if (keyOf(mid) <= lo) low = mid + 1;
		else high = mid;
	}
	for (unsigned i = low; i < list->count() && keyOf(i) <= hi; ++i) {
		auto* object = static_cast<EffectGameObject*>(list->objectAtIndex(i));
		if (object && (motionTrigger(object->m_objectID) || object->m_objectID == 1268)) return true;
	}
	return false;
}

// Whether the forecast depends on the player's exact place, not just when it passes triggers.
bool playerExact(Forecast const& f, PlayLayer* play) {
	auto const& game = play->m_gameState;
	if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty()) return true;
	if (!f.shadow) return false;
	for (auto const& command : f.shadow->m_unkVector560)
		if (command.m_lockToPlayerX || command.m_lockToPlayerY || command.m_lockToCameraX || command.m_lockToCameraY) return true;
	return false;
}

void prepare(PlayLayer* play, int steps, float dt) {
	auto& f = state();
	if (!play || steps <= 0 || f.applying) return;
	// A short prediction (Death Next Frame's one-step lookahead runs every frame) still keeps a lead:
	// a one-step forecast was out of date after the next frame's steps and rebuilt every frame, about
	// 10 ms each on Slaughterhouse. With the lead, a few live steps only use what is built.
	steps = std::max(steps, std::max(32, static_cast<int>(Logic::get().sampleRate() / 15.0)));
	// A backstep replays its steps to the frame stepped back to; nothing is drawn until it is there.
	if (Logic::get().backstepTarget >= 0 || Logic::get().backstepping) {
		f.valid = false;
		f.liveMoved.clear();
		return;
	}
	refreshObjects(f, play);
	// A step with a different length is a different forecast (time warp, TPS change).
	if (f.dt != dt) stale() = true;
	f.dt = dt;
	std::uint64_t elapsed = liveSteps() - f.origin;
	// The forecast fires triggers where it expects the player; a speed portal, a teleport or a
	// respawn moves the player elsewhere, so check the guess against where it really is.
	if (f.valid && play->m_player1) {
		auto expected = f.playerAt + f.playerStep * static_cast<float>(elapsed);
		// With the trajectory's path the forecast follows the player through speed changes, so
		// only where the player is matters, not whether its speed is the one it started with.
		bool onPath = liveSteps() >= f.pathStart && liveSteps() - f.pathStart < f.path.size();
		if (onPath) expected = f.path[static_cast<std::size_t>(liveSteps() - f.pathStart)];
		auto live = play->m_player1->getPosition();
		auto step = live - play->m_player1->m_lastPosition;
		bool sideways = play->m_player1->m_isSideways;
		float off = sideways ? std::abs(expected.y - live.y) : std::abs(expected.x - live.x);
		float speed = onPath ? 0.f : sideways ? std::abs(step.y - f.playerStep.y) : std::abs(step.x - f.playerStep.x);
		// A platformer's speed changes all the time; only a jump in position counts there.
		if (play->m_isPlatformer ? off > 30.f : off > 0.5f || speed > 1e-3f) stale() = true;
	}
	// The live level has run `elapsed` of the forecast's steps. Every object it moved has to stand
	// where the forecast put it, and every object the forecast moved has to have got there; anything
	// else (a trigger the forecast cannot see: a touch, an orb, a collision block) means rebuilding.
	if (f.valid && !stale() && elapsed > 0 && elapsed <= static_cast<std::uint64_t>(f.frames.size())) {
		if (f.lastStamp.size() < f.objects.size()) {
			f.lastStamp.resize(f.objects.size(), 0);
			f.lastOffset.resize(f.objects.size(), 0);
		}
		++f.lastEpoch;
		f.lastList.clear();
		for (std::uint64_t k = 0; k < elapsed; ++k)
			for (auto [i, offset] : f.frames[k]) {
				if (f.lastStamp[i] != f.lastEpoch) {
					f.lastStamp[i] = f.lastEpoch;
					f.lastList.push_back(i);
				}
				f.lastOffset[i] = offset;
			}
		bool wrong = false;
		for (auto i : f.lastList) {
			double px = 0, py = 0;
			std::memcpy(&px, f.records.data() + f.lastOffset[i] + kRecordPosition, sizeof(double));
			std::memcpy(&py, f.records.data() + f.lastOffset[i] + kRecordPosition + sizeof(double), sizeof(double));
			auto* object = f.objects[i];
			if (std::abs(px - object->m_positionX) > kTolerance || std::abs(py - object->m_positionY) > kTolerance) {
				wrong = true;
				break;
			}
		}
		for (auto* object : f.liveMoved) {
			if (wrong) break;
			auto found = f.index.find(object);
			if (found != f.index.end() && found->second < f.lastStamp.size() && f.lastStamp[found->second] == f.lastEpoch) continue;
			// Decoration keeps only its newest forecast state, never a per-step one.
			if (object->m_objectType == GameObjectType::Decoration) continue;
			wrong = true;
		}
		if (wrong) stale() = true;
	}
	// The running copy of the live objects follows what the live game moved. GD only reports an
	// object (updateObjectSection) when it may change section, so one moving inside its section is
	// not reported: every object the forecast moves is copied again too. A stale copy would be put
	// back on the live object after a prediction and move it back in the real level.
	for (auto* object : f.liveMoved) {
		if (auto found = f.index.find(object); found != f.index.end() && f.baseStamp[found->second]) cacheLive(f, found->second);
	}
	f.liveMoved.clear();
	for (std::uint32_t i = 0; i < f.latest.size(); ++i) {
		if ((f.latest[i] != UINT32_MAX || f.hasQuiet[i]) && f.baseStamp[i]) cacheLive(f, i);
	}
	// The steps built so far fired triggers from where the forecast expected the player. Where the
	// trajectory now predicts the player elsewhere, those steps are wrong if a motion trigger lies
	// between the two places along the travel, or, with area effects or moves locked to the
	// player (which measure from the player's node), at all.
	if (f.valid && !stale() && !f.path.empty() && play->m_player1) {
		bool sideways = play->m_player1->m_isSideways;
		bool exact = playerExact(f, play);
		std::size_t built = std::min(f.firedFrom.size(), f.frames.size());
		for (std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(elapsed, built)); k < built; ++k) {
			auto was = f.firedFrom[k];
			if (std::isnan(was.x)) continue;
			std::uint64_t absolute = f.origin + k; // frames[k] fired from the player after this live step.
			if (absolute + 1 < f.pathStart || absolute >= f.pathStart + f.path.size()) continue;
			auto now = absolute + 1 == f.pathStart ? f.pathBefore : f.path[static_cast<std::size_t>(absolute - f.pathStart)];
			float travel = sideways ? std::abs(now.y - was.y) : std::abs(now.x - was.x);
			bool differs = exact ? std::max(std::abs(now.x - was.x), std::abs(now.y - was.y)) > 0.01f
				: travel > 0.01f && triggerBetween(f, play, sideways ? std::min(now.y, was.y) : std::min(now.x, was.x), sideways ? std::max(now.y, was.y) : std::max(now.x, was.x), sideways);
			if (!differs) continue;
			stale() = true;
			break;
		}
	}
	// A safety net for anything the checks miss (and a bound on the record buffer): rebuild at
	// least every thirty seconds of level time.
	bool old = Logic::get().stepFrame - lastRebuildStep > static_cast<int>(Logic::get().sampleRate() * 30.0);
	if (stale() || !f.valid || old || elapsed > static_cast<std::uint64_t>(f.horizon)) {
		rebuild(f, play, steps);
		return;
	}
	if (elapsed > 0) {
		f.frames.erase(f.frames.begin(), f.frames.begin() + static_cast<std::ptrdiff_t>(elapsed));
		f.firedFrom.erase(f.firedFrom.begin(), f.firedFrom.begin() + static_cast<std::ptrdiff_t>(std::min<std::uint64_t>(elapsed, f.firedFrom.size())));
		f.playerAt = f.playerAt + f.playerStep * static_cast<float>(elapsed);
		f.stepsRun -= static_cast<int>(elapsed);
		f.origin = liveSteps();
		f.horizon = static_cast<int>(f.frames.size());
		compact(f);
	}
	if (f.horizon < steps) {
		if (!f.shadow || f.frames.empty() && f.records.empty()) {
			// A level with nothing moving stays that way until something moves (the check rebuilds).
			f.frames.resize(static_cast<std::size_t>(steps));
			f.firedFrom.resize(f.frames.size(), cocos2d::CCPoint{NAN, NAN});
			f.horizon = steps;
			return;
		}
		// Past the trajectory's prediction the player is a guess. Where the forecast depends on the
		// player's exact place, the next prediction always corrects the guess slightly and that
		// meant a rebuild (about 50 a second on Artemis 13), so nothing is built on a guess there.
		// Elsewhere a guess only matters when a trigger lies between it and the truth.
		// It always stays a little ahead of the live game, though: a motion trigger the live game
		// fires before the forecast does is spent (it fires once), so the forecast never gets its
		// command. On Keres the predictions died early, the path was a few steps long and the
		// forecast fell behind the live triggers, so moving spikes stood still in predictions.
		int target = steps;
		if (!f.path.empty() && f.pathStart + f.path.size() > f.origin) {
			int covered = static_cast<int>(f.pathStart + f.path.size() - f.origin);
			int lead = std::max(16, static_cast<int>(Logic::get().sampleRate() / 30.0));
			target = std::min(steps, std::max(covered + (playerExact(f, play) ? 0 : 8), lead));
		}
		if (f.horizon < target) extend(f, play, std::min(target - f.horizon, kBuildPerFrame));
	}
}

namespace {
bool recordInside(Forecast const& f, std::uint32_t offset, cocos2d::CCRect const& area) {
	double px = 0, py = 0;
	std::memcpy(&px, f.records.data() + offset + kRecordPosition, sizeof(double));
	std::memcpy(&py, f.records.data() + offset + kRecordPosition + sizeof(double), sizeof(double));
	return area.containsPoint({static_cast<float>(px), static_cast<float>(py)});
}
}

void apply(PlayLayer* play, int step, cocos2d::CCRect const& area) {
	auto& f = state();
	if (!play || play != f.play || !f.valid || step < 1 || step > f.horizon) return;
	auto const& frame = f.frames[static_cast<std::size_t>(step - 1)];
	if (!f.applying) {
		if (frame.empty()) return;
		f.applying = true;
		++f.applyEpoch;
		f.applied.clear();
		f.pending.clear();
			f.sections.begin(play);
	}
	// Only objects near the prediction can touch it. One that is away keeps its newest state pending
	// until it, or the prediction, comes close; one that starts close always follows.
	auto put = [&](std::uint32_t i, std::uint32_t offset) {
		keepBase(f, i);
		if (f.appliedStamp[i] != f.applyEpoch) {
			f.appliedStamp[i] = f.applyEpoch;
			f.applied.push_back(i);
		}
		scatter(f.objects[i], f.records.data() + offset);
		play->updateObjectSection(f.objects[i]);
	};
	for (auto [i, offset] : frame) {
		auto* object = f.objects[i];
		if (recordInside(f, offset, area) || area.containsPoint({static_cast<float>(object->m_positionX), static_cast<float>(object->m_positionY)})) {
			put(i, offset);
			f.pendingRecord[i] = UINT32_MAX;
		} else {
			if (f.pendingRecord[i] == UINT32_MAX) f.pending.push_back(i);
			f.pendingRecord[i] = offset;
		}
	}
	for (std::size_t n = 0; n < f.pending.size();) {
		auto i = f.pending[n];
		auto offset = f.pendingRecord[i];
		if (offset != UINT32_MAX && !recordInside(f, offset, area)) {
			++n;
			continue;
		}
		if (offset != UINT32_MAX) put(i, offset);
		f.pendingRecord[i] = UINT32_MAX;
		f.pending[n] = f.pending.back();
		f.pending.pop_back();
	}
}

void rewind(PlayLayer* play) {
	auto& f = state();
	if (!f.applying) return;
	f.applying = false;
	for (auto i : f.pending) f.pendingRecord[i] = UINT32_MAX;
	f.pending.clear();
	// Applying only writes the gameplay fields; the nodes were never changed.
	for (auto i : f.applied) putBase(f, i, false);
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
				f.sections.probeHad = m_objectsToDeactivate && m_objectsToDeactivate->objectForKey(object->m_uniqueID);
			}
		}
		GJBaseGameLayer::removeObjectFromSection(object);
	}

	void addToSection(GameObject* object) {
		GJBaseGameLayer::addToSection(object);
		auto& f = state();
		if (f.sections.active && object && f.sections.layer == this) f.sections.noteAppended(object);
	}

	void updateObjectSection(GameObject* object) {
		auto& f = state();
		if (!f.sections.active || !object || f.sections.layer != this) {
			if (object && !Logic::get().predicting && static_cast<GJBaseGameLayer*>(f.play) == this && f.liveMoved.size() < 100000) {
				f.liveMoved.insert(object);
			}
			GJBaseGameLayer::updateObjectSection(object);
			return;
		}
		if (f.recording) {
			if (auto found = f.index.find(object); found != f.index.end()) {
				auto i = found->second;
				if (f.movedStamp[i] != f.stepEpoch) {
					f.movedStamp[i] = f.stepEpoch;
					f.moved.push_back(i);
				}
			}
		}
		// GD returns at once when the object stays in its section, which is nearly always.
		f.sections.probe = object;
		f.sections.probed = false;
		GJBaseGameLayer::updateObjectSection(object);
		f.sections.probe = nullptr;
		auto* queued = m_objectsToDeactivate;
		if (f.sections.probed && !f.sections.probeHad && queued && queued->objectForKey(object->m_uniqueID)) f.sections.deactivated.push_back(object->m_uniqueID);
	}
};

// While a forecast step runs, nothing live may be toggled, and spawns only reach motion triggers
// and further spawn triggers (GJBaseGameLayer::spawnObject queues a delayed object on the effect
// manager, which is the shadow then, and triggers the rest).
// GD's move loop only calls updateObjectSection for an object that changes section, so an object
// moving inside its section (a small circle, a short slide) was never seen as moving: the forecast
// left it standing still. Every object a move or a rotation is about to change is noted here, its
// live state kept first.
class $modify(ForecastMoves, GJBaseGameLayer) {
	static void noteMoving(cocos2d::CCArray* objects) {
		auto& f = state();
		if (!f.recording || !objects) return;
		for (unsigned n = 0; n < objects->count(); ++n) {
			auto* object = static_cast<GameObject*>(objects->objectAtIndex(n));
			if (!object) break; // The native loops stop at the first null.
			auto i = ensureIndex(f, object);
			keepBase(f, i);
			if (f.movedStamp[i] != f.stepEpoch) {
				f.movedStamp[i] = f.stepEpoch;
				f.moved.push_back(i);
			}
		}
	}

	void moveObjects(cocos2d::CCArray* objects, double dx, double dy, bool lockPlayerY) {
		objects = forecastMembers(objects);
		noteMoving(objects);
		GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
	}

	void rotateObjects(cocos2d::CCArray* objects, float rotation, cocos2d::CCPoint position, cocos2d::CCPoint offset, bool finished, bool unused) {
		objects = forecastMembers(objects);
		noteMoving(objects);
		GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
	}
};

class $modify(ForecastToggles, GJBaseGameLayer) {
	void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
		if (state().recording) return;
		GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
	}

	void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
		GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
	}

	void spawnObject(GameObject* object, double delay, gd::vector<int> const& remapKeys) {
		auto& f = state();
		if (f.recording) {
			auto* effect = typeinfo_cast<EffectGameObject*>(object);
			if (!effect || !(motionTrigger(effect->m_objectID) || effect->m_objectID == 1268)) return;
				keepTrigger(f, effect);
		}
		GJBaseGameLayer::spawnObject(object, delay, remapKeys);
	}
};

// Area actions pick their objects from a group or from a range of sections, and only those can move;
// they are saved just before the action changes them. Fade and tint only change how objects look.
class $modify(ForecastAreas, GJBaseGameLayer) {
	void keepArea(cocos2d::CCArray* objects, int outerMin, int outerMax, int middleMin, int middleMax, bool targetGroups) {
		auto& f = state();
		auto keep = [&f](GameObject* object) {
			if (!object) return;
			keepBase(f, ensureIndex(f, object));
		};
		if (targetGroups && objects) {
			for (unsigned i = 0; i < objects->count(); ++i) keep(static_cast<GameObject*>(objects->objectAtIndex(i)));
			return;
		}
		auto& grid = *reinterpret_cast<Grid*>(&m_sections);
		auto& counts = *reinterpret_cast<Counts*>(&m_sectionSizes);
		for (int x = std::max(outerMin, 0); x <= outerMax; ++x) {
			for (int y = std::max(middleMin, 0); y <= middleMax; ++y) {
				auto* bucket = bucketAt(grid, x, y);
				auto* count = countAt(counts, x, y);
				if (!bucket || !count) continue;
				int used = std::clamp(*count, 0, static_cast<int>(bucket->size()));
				for (int i = 0; i < used; ++i) keep((*bucket)[i]);
			}
		}
	}

	void processAreaEffects(gd::vector<EnterEffectInstance>* effects, GJAreaActionType type, float dt, bool visibleFrame) {
		if (state().recording && (type == GJAreaActionType::Fade || type == GJAreaActionType::Tint)) return;
		GJBaseGameLayer::processAreaEffects(effects, type, dt, visibleFrame);
	}

	void processAreaMoveGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance, cocos2d::CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
		if (state().recording) keepArea(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
		GJBaseGameLayer::processAreaMoveGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
	}

	void processAreaRotateGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance, cocos2d::CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
		if (state().recording) keepArea(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
		GJBaseGameLayer::processAreaRotateGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
	}

	void processAreaTransformGroupAction(cocos2d::CCArray* objects, EnterEffectInstance* instance, cocos2d::CCPoint position, int outerMin, int outerMax, int middleMin, int middleMax, int startIndex, bool targetGroups, bool reset) {
		if (state().recording) keepArea(objects, outerMin, outerMax, middleMin, middleMax, targetGroups);
		GJBaseGameLayer::processAreaTransformGroupAction(objects, instance, position, outerMin, outerMax, middleMin, middleMax, startIndex, targetGroups, reset);
	}
};

// The forecast's rotations, scales and fades go through these; each object's node is saved the first
// time a pass changes it.
class $modify(ForecastNodes, GameObject) {
	void keepNode() {
		auto& f = state();
		if (!f.recording) return;
		auto found = f.index.find(this);
		if (found == f.index.end()) return;
		auto i = found->second;
		if (f.nodeStamp[i] == f.touchEpoch) return;
		f.nodeStamp[i] = f.touchEpoch;
		f.nodes[i] = nodeOf(this);
	}

	void setRotation(float rotation) {
		keepNode();
		GameObject::setRotation(rotation);
	}

	void setRotationX(float rotation) {
		keepNode();
		GameObject::setRotationX(rotation);
	}

	void setRotationY(float rotation) {
		keepNode();
		GameObject::setRotationY(rotation);
	}

	void setScaleX(float scale) {
		keepNode();
		GameObject::setScaleX(scale);
	}

	void setScaleY(float scale) {
		keepNode();
		GameObject::setScaleY(scale);
	}

	void setOpacity(GLubyte opacity) {
		keepNode();
		GameObject::setOpacity(opacity);
	}
};

// The forecast copies the live commands only when it is built. A motion trigger the live game fires
// that this build has not fired already (a touch, a collision, or the forecast falling behind) is
// spent once fired, so the forecast never gets its command and the objects stand still in
// predictions. GD reports no move inside a section, so nothing else notices: build again from the
// live commands.
class $modify(ForecastLiveTriggers, EffectGameObject) {
	void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
		auto& f = state();
		if (!f.recording && !Logic::get().predicting && layer && static_cast<GJBaseGameLayer*>(f.play) == layer && (motionTrigger(m_objectID) || m_objectID == 1268)) {
			auto ahead = forecastFired.find(m_uniqueID);
			bool known = ahead != forecastFired.end() && ahead->second == f.generation;
			if (f.valid && !known) stale() = true;
		}
		EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
	}
};
