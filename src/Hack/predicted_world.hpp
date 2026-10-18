#pragma once

#include <Geode/Geode.hpp>
#include "Logic/game_state_copy.hpp"
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cocos2d;

class PlayLayer;
class GJEffectManager;
class EffectGameObject;

struct EventLink {
	int group;
	int unique_id;
	int control_id;
	bool disabled;
	gd::vector<int> keys;
};

class PredictedWorld {
public:
	static PredictedWorld& get() {
		static PredictedWorld value;
		return value;
	}

	bool usable(PlayLayer* play, int now, float step_delta) const;
	bool matches_live(PlayLayer* play, int now, const CCRect& region);
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

	bool changes_after(int step) const {
		return last_change_step > step;
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
	void begin_step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2);
	void move_step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2, bool last_step);
	void end_step(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2);
	void enter(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2);
	void leave(PlayLayer* play);
	void fire_spawn_triggers(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2);
	void collect(PlayLayer* play, EffectGameObject* object, PlayerObject* body);
	void player_button(PlayLayer* play, PlayerObject* driver1, PlayerObject* driver2, bool down, bool player1, bool handled, int button);
	void note_event(PlayLayer* play, int event, int material, int player_id);
	void apply_records(PlayLayer* play, int step, const CCRect& window);
	void reapply_records(PlayLayer* play);
	void reset_applied();
	void pause_session(PlayLayer* play);
	void end_session(PlayLayer* play);
	void suspend_session(PlayLayer* play);
	void resume_session(PlayLayer* play);

	bool running() const {
		return session_open;
	}

	bool firing() const {
		return inside_step;
	}
	bool fires_events() const {
#ifdef GEODE_IS_WINDOWS
		return inside_step;
#else
		return false;
#endif
	}
	PlayerObject* live_player(bool second) const {
		if (inside_step) return second ? entered_player2 : entered_player1;
		return play ? (second ? play->m_player2 : play->m_player1) : nullptr;
	}

	void note_trigger(EffectGameObject* object);
	void note_random_roll(unsigned long long* state);
	void note_moved(GJBaseGameLayer* layer, CCArray* objects);
	void note_moved_one(GJBaseGameLayer* layer, GameObject* object);
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
		int trigger = -1;
		bool applied = false, pending = false;
		CCPoint placed;
		unsigned placed_round = 0;
		int changed_step = 0;
		bool dormant = false;
	};

	struct StepRecords {
		int step = 0;
		unsigned count = 0;
		const unsigned char* bytes = nullptr;
	};

	struct RecordBlock {
		std::unique_ptr<unsigned char[]> bytes;
		std::size_t used = 0, size = 0;
		int last_step = 0;
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
	GJEffectManager* entered_manager = nullptr;
	PlayerObject* entered_player1 = nullptr;
	PlayerObject* entered_player2 = nullptr;
	int enter_depth = 0;
	float last_driver_dy = 0.f;
	float step_delta = 0.f;
	int start_step = 0, horizon_step = 0, last_change_step = -1;
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
	unsigned change_round = 1, placed_round = 1;
	std::vector<unsigned char> horizon_differs;
	struct EncodedPart {
		std::vector<unsigned char> bytes;
		unsigned count = 0, first_slot = 0, last_slot = 0;
		std::size_t first_size = 0;
		bool first_new_mask = false;
	};
	std::vector<EncodedPart> encoded_parts;
	CCPoint placed_position(unsigned at);
	void mark_changed(unsigned at);
	void mark_group_changed(GJBaseGameLayer* layer, int group);
	void mark_live_dirty(unsigned at);
	void mark_group_live_dirty(GJBaseGameLayer* layer, int group);
	std::vector<unsigned char> live_dirty;
	std::vector<unsigned> live_dirty_slots;
	std::vector<unsigned> session_list, candidates, applied, pending;
	CCRect pending_window;
	bool pending_scanned = false;
	bool suspended = false;
	std::vector<unsigned> suspended_applied, suspended_pending;
	std::vector<unsigned char> live_fields, horizon_fields, latest_fields, expected_fields, pass_fields;
	std::vector<unsigned long long> latest_masks, expected_masks, pass_masks;
	std::vector<unsigned> pass_field_round;
	unsigned pass_round = 1, suspended_pass_round = 0;
	int pass_step = 0, suspended_pass_step = 0;
	unsigned char* pass_compact(unsigned at);
	void begin_pass();
	std::deque<RecordBlock> record_blocks;
	std::size_t record_bytes_held = 0, record_bytes_limit = 0;
	std::vector<unsigned char> step_bytes;
	std::deque<StepRecords> step_records;
	void drop_records();
	void refresh_record_limit();

	std::map<long long, SavedCell> cells;
	SavedList calc_non_effect, calc_blocks;
	int moved_count = 0;
	std::vector<int> deactivated_ids;
	std::vector<FiredTrigger> fired;
	struct EventRound {
		std::pair<int, int> key;
		int live_round;
	};
	std::vector<EventRound> event_rounds;
	std::map<std::pair<int, int>, std::vector<EventLink>> live_event_links;
	std::vector<RandomRoll> rolls;
	std::vector<std::pair<GameObject*, CCPoint>> grouped_positions;

	unsigned slot(GameObject* object);
	unsigned touch(GameObject* object);
	void touch_session(unsigned at);
	bool join_session(unsigned at);
	std::vector<std::vector<unsigned>> joined_parts;
	std::vector<unsigned char> needs_setters;
	void save_group(GJBaseGameLayer* layer, int group);
	void save_command_groups(GJBaseGameLayer* layer);
	void record_moved();
	void apply_pass_fields(PlayLayer* play, unsigned at);
	SavedCell* save_cell(GJBaseGameLayer* layer, int x, int y);
	void restore_live(PlayLayer* play);
	void rebuild_slots(PlayLayer* play);
	void drop_shadow();
};
