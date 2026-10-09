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

// Neither a practice respawn nor a backstep load brings every object's move state back.
// GJBaseGameLayer::moveObjects refreshes an object's m_lastPosition (which
// PlayerObject::postCollision and the collidedWith* functions use to carry the player on moving
// blocks) only when the object's m_unk4C4 differs from m_gameState.m_commandIndex, then stores
// that index there. Loading a checkpoint rewinds m_commandIndex but leaves each object's index and
// last position from the run that went further, so the steps run again skip refreshes the first
// pass made. It also resets the area move offsets. Recordings made with respawns then met moving
// blocks elsewhere than a continuous playback.
//
// The move state is saved at the start of the step a checkpoint or backstep save holds, and put
// back at the start of the first step after loading it: the same point in the same step, so it is
// exactly what the first pass had.
namespace {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
struct Span {
	std::uint16_t begin;
	std::uint16_t size;
	bool moves; // Changing it can move the object to another section.
};
#define ECHO_SPAN(first, last, moves)                                                                         \
	Span{static_cast<std::uint16_t>(offsetof(GameObject, first)),                                                \
		static_cast<std::uint16_t>(offsetof(GameObject, last) + sizeof(GameObject::last) - offsetof(GameObject, first)), \
		moves}
constexpr std::array kSpans = {
	ECHO_SPAN(m_positionXOffset, m_scaleYOffset, true),
	ECHO_SPAN(m_unmodifiedPositionX, m_positionY, true),
	ECHO_SPAN(m_startRotationX, m_customScaleY, false),
	ECHO_SPAN(m_scaleX, m_scaleY, false),
	ECHO_SPAN(m_lastPosition, m_unk4CC, false),
	// Caches of the fields above with their dirty flags. The oriented box itself is rebuilt.
	ECHO_SPAN(m_isDirty, m_isOrientedBoxDirty, false),
};
#undef ECHO_SPAN
#pragma clang diagnostic pop
static_assert(kSpans.size() <= 8, "a record's span mask is one byte");

constexpr std::size_t kStride = [] {
	std::size_t total = 0;
	for (auto const& span : kSpans) total += span.size;
	return total;
}();

void gather(GameObject const* object, std::uint8_t* out) {
	auto const* raw = reinterpret_cast<std::uint8_t const*>(object);
	for (auto const& span : kSpans) {
		std::memcpy(out, raw + span.begin, span.size);
		out += span.size;
	}
}

// Writes the spans of `record` that differ from the object. Returns the spans written.
std::uint8_t put(GJBaseGameLayer* play, GameObject* object, std::uint8_t const* record) {
	auto* raw = reinterpret_cast<std::uint8_t*>(object);
	std::uint8_t written = 0;
	bool moved = false;
	for (std::size_t i = 0; i < kSpans.size(); ++i) {
		auto const& span = kSpans[i];
		if (std::memcmp(raw + span.begin, record, span.size) != 0) {
			std::memcpy(raw + span.begin, record, span.size);
			written |= static_cast<std::uint8_t>(1u << i);
			moved = moved || span.moves;
		}
		record += span.size;
	}
	if (!written) return 0;
	object->m_isOrientedBoxDirty = true;
	if (moved) play->updateObjectSection(object);
	return written;
}

// Only objects in a group can be moved, rotated, scaled or offset by a trigger.
std::vector<GameObject*> groupedObjects(PlayLayer* play) {
	std::vector<GameObject*> out;
	if (!play->m_objects) return out;
	for (unsigned i = 0; i < play->m_objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
		if (object && object->m_groupCount > 0) out.push_back(object);
	}
	return out;
}

// Practice checkpoints are few, so they keep every object and check the grouped-only shortcut
// the backstep timeline takes.
std::shared_ptr<ObjectStates> capture(PlayLayer* play, int frame) {
	auto states = std::make_shared<ObjectStates>();
	states->frame = frame;
	if (!play->m_objects) return states;
	unsigned count = play->m_objects->count();
	states->objects.reserve(count);
	states->bytes.resize(static_cast<std::size_t>(count) * kStride);
	for (unsigned i = 0; i < count; ++i) {
		auto* object = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
		states->objects.push_back(object);
		if (object) gather(object, states->bytes.data() + static_cast<std::size_t>(i) * kStride);
	}
	return states;
}

void restore(PlayLayer* play, ObjectStates const& states) {
	if (!play->m_objects || play->m_objects->count() != states.objects.size()) {
		log::warn("Echo practice objects: {} objects now, {} saved; not restored",
			play->m_objects ? play->m_objects->count() : 0, states.objects.size());
		return;
	}
	for (unsigned i = 0; i < states.objects.size(); ++i) {
		auto* object = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
		if (object != states.objects[i]) {
			log::warn("Echo practice objects: object {} changed since the save; not restored", i);
			return;
		}
		if (object) put(play, object, states.bytes.data() + static_cast<std::size_t>(i) * kStride);
	}
}

// The backstep timeline: every grouped object's state at the newest save, and on each save an
// undo record of the objects that changed before the next one (index, span mask, old spans).
//
// Comparing every grouped object on each save cost about 8 ms on Amethyst, 60 times a second. Only
// the objects GD's move code touched since the last save (moveObjects, rotateObjects,
// updateObjectSection, seen live) are compared now. Anything that moves objects past those
// (follow, scale, area actions, keyframes, advanced follow) makes the next save compare everything,
// and every 32nd save compares everything anyway: an object it finds moved that the touched list
// missed puts the level on full compares for good, with a log line.
struct Timeline {
	PlayLayer* play = nullptr;
	cocos2d::CCArray* array = nullptr;
	std::vector<GameObject*> objects;
	std::vector<std::uint8_t> baseline;
	std::vector<std::uint8_t> scratch;
	int pendingFrame = -1;
	// Timeline index + 1 by object uid (0: not in the timeline).
	std::vector<std::uint32_t> byUid;
	std::vector<std::uint32_t> touchedStamp;
	std::vector<std::uint32_t> touched;
	std::uint32_t epoch = 1;
	bool full = true;          // The next save compares everything.
	bool alwaysFull = false;   // The level moves objects in ways the touched list cannot see.
	int saves = 0;
};
Timeline& timeline() {
	static Timeline value;
	return value;
}

void touchObject(GameObject* object) {
	auto& line = timeline();
	if (!object || line.byUid.empty() || Logic::get().predicting) return;
	auto uid = static_cast<std::size_t>(object->m_uniqueID);
	if (uid >= line.byUid.size()) return;
	auto at = line.byUid[uid];
	if (!at || line.objects[at - 1] != object) return;
	auto i = at - 1;
	if (line.touchedStamp[i] == line.epoch) return;
	line.touchedStamp[i] = line.epoch;
	line.touched.push_back(i);
}

// Whether something is moving objects in a way the touched list does not see.
bool untrackedMotion(PlayLayer* play) {
	auto const& game = play->m_gameState;
	if (!game.m_moveEffectInstances.empty() || !game.m_rotateEffectInstances.empty() || !game.m_scaleEffectInstances.empty()) return true;
	auto* manager = play->m_effectManager;
	if (!manager) return false;
	for (auto const& command : manager->m_unkVector560) {
		auto* trigger = command.m_gameObject;
		int id = trigger ? trigger->m_objectID : 0;
		if (id != 901 && id != 1346) return true; // Move and Rotate go through moveObjects / rotateObjects.
	}
	return false;
}

// Objects whose motion never goes through moveObjects or rotateObjects at all.
bool levelHasUntrackedMovers(PlayLayer* play) {
	if (!play->m_objects) return false;
	for (unsigned i = 0; i < play->m_objects->count(); ++i) {
		auto* object = static_cast<GameObject*>(play->m_objects->objectAtIndex(i));
		if (!object) continue;
		switch (object->m_objectID) {
			case 3016: // Advanced Follow
			case 3660: // Edit Advanced Follow
			case 3661: // Re-target Advanced Follow
			case 3032: // Keyframe
			case 3033: // Keyframe Animation
				return true;
			default: break;
		}
	}
	return false;
}

void rewind(Timeline& line, std::vector<std::uint8_t> const& undo) {
	auto const* in = undo.data();
	auto const* end = in + undo.size();
	while (in < end) {
		std::uint32_t index = 0;
		std::memcpy(&index, in, sizeof(index));
		in += sizeof(index);
		std::uint8_t mask = *in++;
		auto* slot = line.baseline.data() + static_cast<std::size_t>(index) * kStride;
		for (std::size_t s = 0; s < kSpans.size(); ++s) {
			if (mask & (1u << s)) {
				std::memcpy(slot, in, kSpans[s].size);
				in += kSpans[s].size;
			}
			slot += kSpans[s].size;
		}
	}
}
}

void Logic::stepObjectStates(PlayLayer* play) {
	auto& value = Logic::get();
	if (!play || value.predicting || value.backstepping) return;
	int frame = value.stepFrame;
	auto& line = timeline();
	if (!line.byUid.empty() && !line.full && untrackedMotion(play)) line.full = true;
	if (line.pendingFrame >= 0) {
		int saved = std::exchange(line.pendingFrame, -1);
		if (saved != frame) log::warn("Echo backstep objects saved at frame {}, loaded at {}; not restored", saved, frame);
		else if (line.play == play && line.array == play->m_objects) {
			for (std::size_t i = 0; i < line.objects.size(); ++i)
				put(play, line.objects[i], line.baseline.data() + i * kStride);
		}
	}
	if (auto* object = std::exchange(value.objectRestore, nullptr)) {
		value.objectCapture = nullptr;
		auto found = std::ranges::find(value.checkpoints, object, &Checkpoint::object);
		if (found == value.checkpoints.end() || !found->objects) return;
		if (found->objects->frame != frame) {
			log::warn("Echo practice objects saved at frame {}, respawned at {}; not restored", found->objects->frame, frame);
			return;
		}
		restore(play, *found->objects);
		return;
	}
	if (auto* object = std::exchange(value.objectCapture, nullptr)) {
		auto found = std::ranges::find(value.checkpoints, object, &Checkpoint::object);
		if (found == value.checkpoints.end()) return;
		if (found->frame != frame) log::warn("Echo checkpoint frame {} saved objects at step {}", found->frame, frame);
		found->objects = capture(play, frame);
	}
}

// Called right after a backstep save is added: records what changed since the previous save.
void Logic::noteBackstepObjects(PlayLayer* play) {
	auto& value = Logic::get();
	auto& line = timeline();
	if (line.play != play || line.array != play->m_objects) {
		line.play = play;
		line.array = play->m_objects;
		line.objects = groupedObjects(play);
		line.baseline.resize(line.objects.size() * kStride);
		for (std::size_t i = 0; i < line.objects.size(); ++i) gather(line.objects[i], line.baseline.data() + i * kStride);
		int maxUid = 0;
		for (auto* object : line.objects) maxUid = std::max(maxUid, object->m_uniqueID);
		line.byUid.assign(static_cast<std::size_t>(std::max(maxUid, 0)) + 1, 0);
		for (std::size_t i = 0; i < line.objects.size(); ++i) {
			auto uid = line.objects[i]->m_uniqueID;
			if (uid >= 0) line.byUid[static_cast<std::size_t>(uid)] = static_cast<std::uint32_t>(i + 1);
		}
		line.touchedStamp.assign(line.objects.size(), 0);
		line.touched.clear();
		++line.epoch;
		line.alwaysFull = levelHasUntrackedMovers(play);
		line.full = line.alwaysFull || untrackedMotion(play);
		line.saves = 0;
		if (line.alwaysFull) log::info("Echo backstep: this level has advanced follow or keyframes; every save compares every object");
		// Older saves have no undo records to come back to.
		while (value.backsteps.size() > 1) {
			releaseBackstep(value.backsteps.front(), "new timeline");
			value.backsteps.pop_front();
		}
		return;
	}
	if (value.backsteps.size() < 2) return;
	auto& undo = value.backsteps[value.backsteps.size() - 2].objectUndo;
	undo.clear();
	line.scratch.resize(kStride);
	// Every 32nd save compares everything and checks the touched list missed nothing.
	bool verify = !line.full && !line.alwaysFull && (++line.saves % 32) == 0;
	bool everything = line.full || line.alwaysFull || verify;
	int missed = 0;
	std::string missedSample;
	auto visit = [&](std::size_t i) {
		auto* slot = line.baseline.data() + i * kStride;
		gather(line.objects[i], line.scratch.data());
		if (std::memcmp(slot, line.scratch.data(), kStride) == 0) return;
		std::uint32_t index = static_cast<std::uint32_t>(i);
		std::size_t at = undo.size();
		undo.resize(at + sizeof(index) + 1);
		std::memcpy(undo.data() + at, &index, sizeof(index));
		std::uint8_t mask = 0;
		std::size_t offset = 0;
		for (std::size_t s = 0; s < kSpans.size(); ++s) {
			auto size = kSpans[s].size;
			if (std::memcmp(slot + offset, line.scratch.data() + offset, size) != 0) {
				mask |= static_cast<std::uint8_t>(1u << s);
				undo.insert(undo.end(), slot + offset, slot + offset + size);
			}
			offset += size;
		}
		undo[at + sizeof(index)] = mask;
		std::memcpy(slot, line.scratch.data(), kStride);
		// Spans 0-4 are the object's own state; the last is caches its readers rebuild.
		if (verify && (mask & 0x1f) && line.touchedStamp[i] != line.epoch) {
			if (missed++ < 4) missedSample += fmt::format(" id {} uid {} spans {:#x}", line.objects[i]->m_objectID, line.objects[i]->m_uniqueID, mask);
		}
	};
	if (everything) {
		for (std::size_t i = 0; i < line.objects.size(); ++i) visit(i);
	} else {
		for (auto i : line.touched) visit(i);
	}
	if (missed) {
		line.alwaysFull = true;
		log::warn("Echo backstep: {} moved objects were not on the touched list ({}); every save compares every object on this level now", missed, missedSample);
	}
	line.touched.clear();
	++line.epoch;
	line.full = line.alwaysFull || untrackedMotion(play);
	undo.shrink_to_fit();
}

// Drops the newest backstep save; the baseline goes back to the save before it.
void Logic::popBackstep() {
	auto& value = Logic::get();
	if (value.backsteps.empty()) return;
	releaseBackstep(value.backsteps.back(), "pop");
	value.backsteps.pop_back();
	if (value.backsteps.empty()) {
		clearBackstepObjects();
		return;
	}
	auto& line = timeline();
	// Loading a save changes objects outside the touched list.
	line.full = true;
	auto& undo = value.backsteps.back().objectUndo;
	if (!line.baseline.empty()) rewind(line, undo);
	undo.clear();
	undo.shrink_to_fit();
}

void Logic::clearBackstepObjects() {
	auto& line = timeline();
	line = Timeline{};
}

// The newest save was just loaded: put its object state back at the first step.
void Logic::loadBackstepObjects(int frame) {
	auto& line = timeline();
	if (!line.baseline.empty()) line.pendingFrame = frame;
	line.full = true;
}

// The touched list (see Timeline): the live calls that move grouped objects.
class $modify(BackstepTouched, GJBaseGameLayer) {
	static void touchAll(cocos2d::CCArray* objects) {
		if (!objects || timeline().byUid.empty() || Logic::get().predicting) return;
		for (unsigned n = 0; n < objects->count(); ++n) {
			auto* object = static_cast<GameObject*>(objects->objectAtIndex(n));
			if (!object) break; // The native loops stop at the first null.
			touchObject(object);
		}
	}

	void moveObjects(cocos2d::CCArray* objects, double dx, double dy, bool lockPlayerY) {
		touchAll(objects);
		GJBaseGameLayer::moveObjects(objects, dx, dy, lockPlayerY);
	}

	void rotateObjects(cocos2d::CCArray* objects, float rotation, cocos2d::CCPoint position, cocos2d::CCPoint offset, bool finished, bool unused) {
		touchAll(objects);
		GJBaseGameLayer::rotateObjects(objects, rotation, position, offset, finished, unused);
	}

	void updateObjectSection(GameObject* object) {
		touchObject(object);
		GJBaseGameLayer::updateObjectSection(object);
	}
};
