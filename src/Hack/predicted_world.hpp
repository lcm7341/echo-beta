#pragma once

#include <Geode/Geode.hpp>
#include "Logic/game_state_copy.hpp"
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace cocos2d;

class PlayLayer;
class GJEffectManager;
class EffectGameObject;

class PredictedWorld {
public:
	static PredictedWorld& get() {
		static PredictedWorld value;
		return value;
	}

	bool usable(PlayLayer* play, int now, float step_delta) const;
	bool matches_live(PlayLayer* play, int now);
	std::string near_report(CCPoint at, float reach) const;
	std::string follow_report(PlayLayer* play) const;
	int watch_uid = -1;
	double part_ms[8] = {};
	long long moved_objects = 0, section_changes = 0, cell_saves = 0, toggles = 0, live_misses = 0;
	std::string timing_report() const;
	void reset_timing();
	std::string mismatch;
	void rebuild(PlayLayer* play, float step_delta, int now);
	void trim(int now);
	void invalidate();

	int horizon() const {
		return horizon_step;
	}

	int expected_step() const {
		return start_step;
	}

	bool is_tracked(GameObject* object) {
		return slot(object) != UINT_MAX;
	}

	size_t tracked_count() const {
		return tracked.size();
	}

	bool full() const;

	void begin_session(PlayLayer* play);
	void resume_horizon(PlayLayer* play);
	void pause_horizon(PlayLayer* play);
	void step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2, bool last_step);
	void fire_spawn_triggers(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2);
	void after_players(PlayLayer* play);
	void apply_records(PlayLayer* play, int step, const CCRect& window);
	void reapply_records(PlayLayer* play);
	void reset_applied();
	void pause_session(PlayLayer* play);
	void end_session(PlayLayer* play);

	bool running() const {
		return session_open;
	}

	bool firing() const {
		return inside_step;
	}

	void note_trigger(EffectGameObject* object);
	void note_random_roll(unsigned long long* state);
	void note_moved(GJBaseGameLayer* layer, CCArray* objects);
	void note_toggle(GJBaseGameLayer* layer, int group);
	void log_toggle(GJBaseGameLayer* layer, int group, bool activate);
	void note_area(GJBaseGameLayer* layer, CCArray* objects, int outer_min, int outer_max, int middle_min, int middle_max, bool target_groups);
	void before_section_change(GJBaseGameLayer* layer, GameObject* object);
	void before_section_add(GJBaseGameLayer* layer, GameObject* object);
	void note_deactivated(GJBaseGameLayer* layer, GameObject* object, bool was_queued);
	void enable_hooks(bool on);

private:
	struct Look {
		float rotation_x = 0.f, rotation_y = 0.f, scale_x = 1.f, scale_y = 1.f;
		unsigned char opacity = 255;
		CCPoint node_position, detail_position;
	};

	struct Tracked {
		GameObject* object = nullptr;
		unsigned session = 0;
		Look look;
		bool indexes_saved = false;
		int some_other_index = 0, inner_index = 0, outer_index = 0, middle_index = 0;
		int live_counter = 0, horizon_counter = 0;
		bool live_group_disabled = false, live_group_disabled_temp = false, horizon_group_disabled = false, horizon_group_disabled_temp = false;
		int trigger = -1;
		unsigned applied_offset = UINT_MAX, pending_offset = UINT_MAX;
	};

	struct StepRecords {
		int step = 0;
		unsigned begin = 0, count = 0;
	};

	struct SavedList {
		std::vector<GameObject*> objects;
		int count = 0;
		bool present = false;
	};

	struct SavedCell {
		int x = 0, y = 0;
		SavedList main, non_effect, blocks;
		bool flag = false, has_flag = false;
	};

	struct FiredTrigger {
		EffectGameObject* object = nullptr;
		std::vector<unsigned char> live_bytes, horizon_bytes;
	};

	struct RandomRoll {
		unsigned long long* state = nullptr;
		unsigned long long live = 0, horizon = 0;
	};

	bool valid = false, session_open = false, horizon_loaded = false, inside_step = false, hooks_on = false, live_unknown = false;
	PlayLayer* play = nullptr;
	GJEffectManager* shadow = nullptr;
	float step_delta = 0.f;
	int start_step = 0, horizon_step = 0;
	std::unique_ptr<SavedGameState> horizon_state;
	unsigned long long horizon_random = 0, horizon_teleport = 0, horizon_shake = 0;
	gd::set<std::tuple<int, int, int>> horizon_spawn_tuples, live_spawn_tuples;
	CCPoint camera_start, camera2_start, driver_start;
	bool driver_known = false;

	std::vector<unsigned> slot_of_uid, slot_round, saved_groups;
	PlayLayer* slots_play = nullptr;
	unsigned slots_count = 0;
	unsigned round = 1, session_round = 1;
	std::vector<Tracked> tracked;
	std::vector<unsigned> changed_slots, changed_round;
	unsigned change_round = 1;
	void mark_changed(unsigned at);
	void mark_group_changed(GJBaseGameLayer* layer, int group);
	void mark_live_dirty(unsigned at);
	void mark_group_live_dirty(GJBaseGameLayer* layer, int group);
	std::vector<unsigned char> live_dirty;
	std::vector<unsigned> live_dirty_slots;
	std::vector<unsigned> session_list, candidates, applied, pending;
	std::vector<unsigned char> live_fields, horizon_fields, latest_fields, expected_fields;
	std::vector<unsigned char> record_bytes;
	unsigned record_front = 0;
	std::vector<StepRecords> step_records;

	std::map<long long, SavedCell> cells;
	SavedList calc_non_effect, calc_blocks;
	int moved_count = 0;
	std::vector<int> deactivated_ids;
	std::vector<FiredTrigger> fired;
	std::vector<RandomRoll> rolls;
	std::vector<std::pair<GameObject*, CCPoint>> grouped_positions;

	unsigned slot(GameObject* object);
	unsigned touch(GameObject* object);
	void touch_session(unsigned at);
	void save_group(GJBaseGameLayer* layer, int group);
	void save_command_groups(GJBaseGameLayer* layer);
	void record_moved();
	void apply_record(PlayLayer* play, unsigned at, unsigned offset);
	SavedCell* save_cell(GJBaseGameLayer* layer, int x, int y);
	void restore_live(PlayLayer* play);
	void rebuild_slots(PlayLayer* play);
	void drop_shadow();
};
