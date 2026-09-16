#pragma once

#include <array>
#include <chrono>

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

	bool held_next(PlayLayer* play, bool player1) const {
		return next_click(play, player1) == Click::Hold;
	}

	void invalidate() {
		cached = false;
	}

	bool survives(PlayLayer* play, bool player1, bool press, int delay);

	bool isFake(PlayerObject* player) const {
		return player && (player == fake1 || player == fake2);
	}

	PlayerObject* otherFake(PlayerObject* player) const {
		return player == fake1 ? fake2 : fake1;
	}

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
	int measured[2] = {}, measured_full = 0;
	std::vector<CCPoint>* path_sink = nullptr;
	std::unordered_set<EnhancedGameObject*> used_p1, used_p2;

	Click next_click(PlayLayer* play, bool player1) const;
	std::vector<CCPoint> paths[2];
};
