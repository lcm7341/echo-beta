#pragma once

#include <Geode/Geode.hpp>

using namespace cocos2d;

class PlayLayer;

// moves the lvl ahead for predictions
// before ts moving objects just stayed put
namespace forecast {
	void prepare(PlayLayer* play, int steps, float dt);
	void apply(PlayLayer* play, int step, CCRect const& area, float player_y = NAN);
	void rewind(PlayLayer* play);
	bool firing();
	void invalidate();
	void note_live_step();
	void note_player_path(std::vector<CCPoint> path, CCPoint before);
    // TEMP: the trajectory debug file (logs/traj.txt), followers get written into it
    void dump_to(std::string* text);
}
