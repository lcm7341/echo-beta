#pragma once

#include <array>
#include <chrono>
#include <functional>

#include <Geode/Geode.hpp>

#include <unordered_set>
#include <vector>

using namespace cocos2d;

class Trajectory {
public:
	PlayerObject* fake1 = nullptr;
	PlayerObject* fake2 = nullptr;

	static Trajectory& get() {
		static Trajectory value;
		return value;
	}

	void attach(PlayLayer* play);
	void detach();
	void sync_seat(PlayLayer* play);

	CCDrawNode* overlay_node() const {
		return node;
	}

	bool attached() const {
		return node != nullptr;
	}

	bool drawing() const {
		return busy;
	}

	void set_delta(float dt) {
		delta = dt;
	}

	void note_button(bool player1, bool down);
	void refresh(PlayLayer* play);
	void lookahead(PlayLayer* play);
	void autoclick(PlayLayer* play);
	int batchable_steps() const { return safe_steps; }
	int flip_survival(PlayLayer* play, bool player1, int& full, std::vector<CCPoint>* path = nullptr, int limit = 0);

	bool held_next(PlayLayer* play, bool player1) const {
		return next_click(play, player1) == Click::Hold;
	}

	void invalidate();

	// lock fps: the trajectory is worked on across frames & the lvl waits for it
	bool job_pending() const {
		return job_running;
	}
	// only safe while the lvl hasnt moved since it began
	void cancel_job();

	bool survives(PlayLayer* play, bool player1, bool press, int delay);

	bool isFake(PlayerObject* player) const {
		return player && (player == fake1 || player == fake2);
	}

	PlayerObject* otherFake(PlayerObject* player) const {
		return player == fake1 ? fake2 : fake1;
	}

	bool mark_dead(PlayerObject* player);
	bool mark_predict_death(PlayerObject* player);
	bool fakeActivated(PlayerObject* player, EnhancedGameObject* object) const;
	bool realActivated(PlayerObject* player, EnhancedGameObject* object) const;
	void rememberActivation(EnhancedGameObject* object, PlayerObject* player);
	void dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on);

private:
	enum class Click {
		Hold,
		Swift,
		Release,
		Replay
	};

	struct Key {
		int frame = -1;
		float p1[6] = {}, p2[6] = {};
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
		uint32_t orbs = 0;
		float width = 0.f;
		float zoom = 0.f;
		float color[4] = {}, swift_color[4] = {}, release_color[4] = {};
		bool operator==(Key const&) const = default;
	};

	CCDrawNode* node = nullptr;
	// a sliced trajectory draws here, swapped in when done
	CCDrawNode* back_node = nullptr;
	bool busy = false;
	bool cached = false;
	Key last_key;
	float delta = 0.f;
	PlayerObject* colliding_fake = nullptr;
	bool dead_p1 = false, dead_p2 = false;
	PlayerObject* join_real = nullptr;
	bool joinable = false;
	bool pairing = false;
	bool joined = false;
	bool parted = false;
	bool held_p1 = false, held_p2 = false;
	int clicked_frame = -1;
	// steps from now the autoclicker has nothing to decide on, so they can run batched (the 1 it just checked counts)
	int safe_steps = 1;
	int measured[2] = {}, measured_full = 0;
	std::vector<CCPoint>* path_sink = nullptr;
	// death next label only, no moving object forecast
	bool label_only = false;
	std::unordered_set<EnhancedGameObject*> used_p1, used_p2;

	// sliced trajectory, on its own fiber (windows)
	struct SliceHook {
		// every save runs before any pause
		std::function<void()> save, pause, resume;
	};
	std::vector<SliceHook*> slice_hooks;
	void* job_fiber = nullptr;
	void* main_fiber = nullptr;
	bool job_running = false, job_done = false, job_cancel = false, in_job = false, job_swapped = false;
	PlayLayer* job_play = nullptr;
	int job_frame = -1;
	unsigned job_drawn = 0;
	std::chrono::steady_clock::time_point slice_began{};
	double slice_budget = 0.0;
	unsigned slice_counter = 0;

	static void job_entry(void* self);
	bool want_slicing(PlayLayer* play) const;
	void run_slice();
	void finish_job();
	void abandon_job();
	void slice_point();
	void refresh_now(PlayLayer* play);

	void park(PlayerObject* player);
	PlayerObject* spawn_fake(PlayLayer* play, char const* id);
	Key make_key(PlayLayer* play) const;
	void seed(PlayerObject* fake, PlayerObject* real);
	// how many steps the icon lives w/ ts click made right now
	int click_survival(PlayLayer* play, bool player1, Click click, int& full, int limit = 0);
	void press(PlayLayer* play, PlayerObject* fake, Click click);
	Click next_click(PlayLayer* play, bool player1) const;
	template <class F>
	void predicting(PlayLayer* play, F&& predict);
	void simulate(PlayLayer* play, bool player1, Click click, bool both, bool draw, int limit, int measure);
	bool advance(PlayLayer* play, PlayerObject* fake, float step_delta, bool draw, Click click);
	std::vector<CCPoint> paths[2];
	void flush_path(PlayLayer* play, PlayerObject* fake, Click click);
	void draw_death(PlayerObject* fake);
	ccColor4F color_for(PlayerObject* fake, Click click) const;
	float lineWidth(PlayLayer* play) const;
};
