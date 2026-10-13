#pragma once

#include <Geode/Geode.hpp>

#include <unordered_set>

using namespace cocos2d;

namespace Orbs {
	struct Pose {
		GameObject* object = nullptr;
		CCPoint pos;
		float rotation = 0.f, rotationX = 0.f, rotationY = 0.f;
	};

	bool click_orb_assist(bool player2);
	int claim_click(GJBaseGameLayer* layer, PlayerObject* player, bool player2);
	void begin_push(PlayerObject* player);
	void end_push(PlayerObject* player);
	void note_jump_presses(PlayLayer* play);
	void begin_step();
	void arm_tool(bool on);
	void commit_tool_hits();
	bool blockHoldRing(PlayerObject* player, GameObject* object);
	void click_pads(PlayLayer* play, PlayerObject* player, int objectType, GameObject* pad);
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
	uint32_t pose_hash(GJBaseGameLayer* play, PlayerObject* player);
}
