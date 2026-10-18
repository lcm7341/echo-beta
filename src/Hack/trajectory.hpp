#pragma once

#include <Geode/Geode.hpp>
#include "Hack/orbs.hpp"
#include "Logic/game_state_copy.hpp"
#include "Logic/logic.hpp"
#include "Physics/checkpoint.hpp"

#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace cocos2d;

#define FAKE_PAIRS 4
#define KEPT_PAIR 3
#define MEASURE_LIVE 1
#define MEASURE_HOLD 2
#define MEASURE_RELEASE 4
#define MEASURE_STEPS 8

class DashSpinManager : public cocos2d::CCActionManager {
public:
	void tick(float dt) {
		update(dt);
	}
};

enum class ClickType {
	Hold,
	Swift,
	Release,
	Replay
};

class Trajectory {
public:
	struct Sample {
		float x = 0.f, y = 0.f, velocity = 0.f;
		bool dashing = false, ground = false, upside_down = false;
	};

	static void log_line(const std::string& line) {
		get().traj_log(line);
	}

	static Trajectory& get() {
		static Trajectory value;
		return value;
	}

	void attach(PlayLayer* play);
	void detach();
	void sync_seat(PlayLayer* play);

	bool attached() const {
		return draw_node != nullptr;
	}

	bool drawing() const {
		return simulating;
	}

	void set_delta(float dt) {
		real_step_delta = dt;
	}

	void refresh(PlayLayer* play);
	void note_live_step(PlayLayer* play);
	void check_live_step(PlayLayer* play);
	void dash_spin_tick(PlayerObject* fake);
	void dash_spin_start(PlayerObject* fake, float angle);
	void dash_spin_stop(PlayerObject* fake);
	void log_ring_jump(PlayerObject* player, RingObject* ring, bool fake, bool before_set, bool after_set, bool was_dashing, bool was_buffered, bool was_ring_jump, bool was_touched, bool was_custom);
	void lookahead(PlayLayer* play);
	void autoclick(PlayLayer* play);
	int flip_survival(PlayLayer* play, bool player1, int& full, std::vector<CCPoint>* path = nullptr, int limit = 0);

	bool held_next(PlayLayer* play, bool player1) const {
		return next_click(play, player1) == ClickType::Hold;
	}

	void invalidate();

	bool job_pending() const {
		return job.active && job.sliced;
	}

	void cancel_job();

	bool isFake(PlayerObject* player) const {
		return fake_index(player) >= 0;
	}

	int live_player_id(int unique_id) const;
	bool is_second_fake(PlayerObject* player) const {
		int index = fake_index(player);
		return index >= 0 && (index & 1) == 1;
	}

	PlayerObject* otherFake(PlayerObject* player) const {
		int index = fake_index(player);
		return index >= 0 ? fakes[index ^ 1] : nullptr;
	}

	bool mark_dead(PlayerObject* player);
	bool mark_predict_death(PlayerObject* player, GameObject* killer = nullptr);
	bool fakeActivated(PlayerObject* player, EnhancedGameObject* object) const;
	bool realActivated(PlayerObject* player, EnhancedGameObject* object) const;
	void rememberActivation(EnhancedGameObject* object, PlayerObject* player);
	void dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on);

private:
	struct CacheKey {
		int frame = -1;
		float player1_state[6] = {}, player2_state[6] = {};
		bool dual = false;
		bool held_p1 = false, held_p2 = false;
		bool dead = false;
		bool playing = false;
		bool recording = false;
		bool hold = false;
		bool swift = false;
		bool release = false;
		bool dash_orbs = false, black_orbs = false, pad_black = false, pad_blue = false;
		bool pause_kill = false, pause_length = false;
		unsigned pause_ms = 0;
		bool hide_p1 = false, hide_p2 = false;
		float seconds = 0.f;
		std::vector<Orbs::Spot> orbs;
		float width = 0.f;
		float zoom = 0.f;
		float color[4] = {}, swift_color[4] = {}, release_color[4] = {};
		bool operator==(const CacheKey&) const = default;
	};

	struct Line {
		ClickType click = ClickType::Hold;
		bool player1 = true, with_partner = false, draw = false;
		int measure = 0;
		int pair = 0;
		PlayerObject* fake = nullptr;
		PlayerObject* partner = nullptr;
		PlayerObject* real = nullptr;
		PlayerObject* partner_real = nullptr;
		int start_step = 0, partner_start_step = 0, first_step = 0, start_frame = 0;
		bool done = false, partner_done = true, left_dual = false, partner_left_dual = false;
		int death_step = -1, partner_death_step = -1, steps_after_death = 0;
		std::vector<CCPoint> path, partner_path;
		std::vector<Sample> samples, partner_samples;
		bool has_death = false, partner_has_death = false;
		std::string death_text, partner_death_text;
		CCRect death_outer, death_inner, partner_death_outer, partner_death_inner;
		bool started_held = false, uses_autoclicker = false, macro_plays = false;
		AutoClick autoclick;
		size_t input_cursor = 0;
		bool macro_held[2][3] = {};
		int last_input_frame[2] = { -1, -1 };
		std::unordered_set<int> orb_clicks[2];
		Orbs::PadPrediction pads;
		int spawn_cursor[2] = {};
		bool can_start_dual = false, running_pair = false, dual_started = false, dual_ended = false;
	};

	enum class Phase {
		Catchup,
		Extend,
		Lines
	};

	struct Job {
		bool active = false, sliced = false, paused = false, stepping = false, cancel = false, finished = false, draw = false, use_world = false, use_kept = false;
		Phase phase = Phase::Lines;
		PlayLayer* play = nullptr;
		int frame = -1;
		unsigned last_drawn_frame = 0;
		std::vector<Line> lines;
		int now = 0, step = 0, steps = 0, steps_per_frame = 1, cadence = 1, current_step = -1, live_advance = 0;
		float step_delta = 0.f, step_seconds = 0.f, warp = 1.f;
		bool varying_tps = false, world_last_step = false;
		CacheKey key;
		std::vector<CCPoint>* survival_path = nullptr;
		std::unique_ptr<SavedGameState> live_state, prediction_state;
		unsigned long long live_random = 0, prediction_random = 0, live_teleport = 0, prediction_teleport = 0, live_shake = 0, prediction_shake = 0;
		SavedPlayerCheckpoint live_player1, live_player2;
		bool has_player2 = false, live_died = false;
		gd::vector<PlayerButtonCommand> live_queued, prediction_queued;
		gd::vector<RecordButtonCommand> live_recorded, prediction_recorded;
		int live_recorded_size = 0, prediction_recorded_size = 0;
		CCPoint layer_position;
		float layer_scale_x = 1.f, layer_scale_y = 1.f, layer_rotation = 0.f;
		bool hold_buffered[2] = {};
		gd::map<int, bool> hold_buttons[2];
		bool ui_jumping[2] = {};
		int ui_touch[2] = { -1, -1 };
		bool performance_mode = false;
		std::chrono::steady_clock::time_point started, extend_started, slice_start;
		int horizon_at_start = 0;
		unsigned slice_counter = 0;
		bool extend_only = false;
		double phase_ms[3] = {};
		int phase_steps[3] = {};
	};

	struct DrawnPath {
		std::vector<CCPoint> points;
		ccColor4F color;
		int first_step = 0;
		bool show_death = false;
		CCRect death_outer, death_inner;
	};

	std::vector<DrawnPath> drawn_paths;
	int drawn_step = -1;
	PlayerObject* fakes[FAKE_PAIRS * 2] = {};
	CCNode* dash_spins[FAKE_PAIRS * 2] = {};
	DashSpinManager* dash_spin_managers[FAKE_PAIRS * 2] = {};
	GameObject* killers[FAKE_PAIRS * 2] = {};
	std::vector<EnhancedGameObject*> activated[FAKE_PAIRS * 2];
	bool fake_dead[FAKE_PAIRS * 2] = {};
	CCDrawNode* draw_node = nullptr;
	bool simulating = false;
	bool drawing_valid = false;
	CacheKey last_cache_key;
	float real_step_delta = 0.f;
	PlayerObject* colliding_fake = nullptr;
	int measured_alive_steps[2] = {}, measured_total_steps = 0;
	int clicked_frame = -1;
	int previous_job_now = -1;
	Job job;
	struct Watch {
		bool valid = false, reported = false, player1 = true, kept = false;
		int start_step = 0, made_at = 0, death_step = -1, partner_death_step = -1, partner_start_step = 0;
		std::vector<Sample> samples, partner_samples;
		std::string death_text, partner_death_text;
	};
	int last_live_step = -1;
	Watch watch;
	std::ofstream log_file;
	PlayLayer* log_play = nullptr;
	void traj_log(const std::string& line);
	std::string ring_report(PlayLayer* play, PlayerObject* player);
	Line kept;
	bool kept_valid = false;
	PlayLayer* kept_play = nullptr;

	int fake_index(PlayerObject* player) const {
		if (!player) return -1;
		for (int i = 0; i < FAKE_PAIRS * 2; ++i)
			if (fakes[i] == player) return i;
		return -1;
	}

	Line* line_of(PlayerObject* player);
	void park_fake(PlayerObject* player);
	void park_fakes(bool keep_kept);
	PlayerObject* spawn_fake(PlayLayer* play, const char* id);
	CacheKey make_cache_key(PlayLayer* play) const;
	void copy_real_into_fake(PlayerObject* fake, PlayerObject* real);
	ClickType next_click(PlayLayer* play, bool player1) const;
	ClickType actual_click(PlayLayer* play, bool player1) const;
	Line make_line(PlayLayer* play, bool player1, ClickType click, bool with_partner, bool draw, int measure, int pair);
	void add_click_lines(PlayLayer* play, std::vector<Line>& lines, bool player1, bool with_partner, bool draw, bool rendering);
	bool begin_job(PlayLayer* play, std::vector<Line> lines, int limit, bool draw, bool sliced);
	bool kept_matches(PlayLayer* play, bool linked, std::string& reason, bool& diverged);
	void begin_extend();
	void start_line(Line& line);
	void press_fake(const Line& line, PlayerObject* fake, ClickType click, bool icon2);
	void enter_prediction();
	void leave_prediction(bool finished);
	void switch_to_lines();
	bool want_slicing(PlayLayer* play) const;
	void run_slice();
	void run_steps(double budget_seconds);
	bool step_once();
	bool step_phase();
	void advance_time();
	void step_line(Line& line, int step);
	void press_line(Line& line, int step);
	void move_line(Line& line, int step);
	bool step_fake(Line& line, PlayerObject* body, bool is_partner, int step);
	CCPoint move_fake(PlayerObject* body);
	bool land_fake(Line& line, PlayerObject* body, bool is_partner, int step, CCPoint from);
	bool drives_world(const Line& line) const;
	void touch_input(const Line& line, bool down, bool player1, bool handled, PlayerButton button = PlayerButton::Jump);
	void walk_spawn_triggers(Line& line, PlayerObject* body, bool is_partner);
	void apply_dual_portal_changes(Line& line, int step);
	CCRect lines_window() const;
	CCRect covered_region(PlayLayer* play) const;
	CCRect window_around(PlayerObject* body) const;
	void finish_job();
	void draw_lines();
	void paint_paths(int skip);
	void store_results();
	int click_survival(PlayLayer* play, bool player1, ClickType click, int& full, int limit);
	ccColor4F path_color(bool icon2, ClickType click) const;
	float line_width(PlayLayer* play) const;
};
