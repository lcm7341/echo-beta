#pragma once

#include <Geode/Geode.hpp>

#include <string>
#include <vector>

namespace Script {
	class Host;
}

namespace MacroScript {
	extern bool g_running;
	extern bool g_before_step;

	void after_step(PlayLayer* play);
	void before_step(PlayLayer* play);
	void attempt_started(PlayLayer* play);
	void player_died(PlayLayer* play, PlayerObject* player);
	void level_finished(PlayLayer* play);
	void autorun();
	Script::Host& host();
}
