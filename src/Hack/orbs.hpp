#pragma once

#include <Geode/Geode.hpp>

#include <unordered_set>

using namespace cocos2d;

namespace Orbs {
	struct Pose {
		GameObject* object = nullptr;
		CCPoint position;
		float rotation = 0.f, rotation_x = 0.f, rotation_y = 0.f;
	};

	// where each orb near a player is, so the trajectory can tell when one moved
	struct Spot {
		int id = 0;
		float x = 0.f, y = 0.f, rotation = 0.f;
		bool operator==(Spot const&) const = default;
	};

	bool click_orb_assist(bool player2);
	int claim_click(GJBaseGameLayer* layer, PlayerObject* player, bool player2);
	void begin_push(PlayerObject* player);
	void end_push(PlayerObject* player);
	void begin_step();
	void arm_tool(bool on);
	bool blockHoldRing(PlayerObject* player, GameObject* object);
	void click_pads(PlayLayer* play, PlayerObject* player, int object_type, GameObject* pad);
	void flush_pads(PlayLayer* play);
	void note_dash(PlayLayer* play, PlayerObject* player, GameObject* ring);
	void click_fake_dash(PlayerObject* player, bool player2);
	bool has_pending_pads();
	bool take_queued_assist();
	void click_fake(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int>& used, bool fresh_click);
	void reset_prediction_pads();
	void notePredictionPad(GJBaseGameLayer* play, PlayerObject* player, GameObject* pad);
	void apply_prediction_pads(GJBaseGameLayer* play, PlayerObject* player, bool player2);
	void clear();
	std::vector<Pose> capture(GJBaseGameLayer* play, PlayerObject* player);
	void restore(std::vector<Pose> const& poses);
	std::vector<Spot> orb_spots(GJBaseGameLayer* play, PlayerObject* player);
}
