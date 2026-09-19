#pragma once
#include <string>

class GJBaseGameLayer;

namespace echo::step {
	GJBaseGameLayer* begin(GJBaseGameLayer* self);
	void end(GJBaseGameLayer* self, GJBaseGameLayer* layer, bool isHalfTick);
	void trace(GJBaseGameLayer* self);
	void trace_button(char what, bool down, int extra);
	std::string pace();
	void trace_plain(class PlayerObject* player);
}
