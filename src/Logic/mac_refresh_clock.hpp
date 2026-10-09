#pragma once
#include <cstdint>

namespace echo::mac {
	double refresh_frame_time(double dt);
	uint32_t window_display();
	double window_max_refresh();
	void suggest_sdl_pacing();
}
