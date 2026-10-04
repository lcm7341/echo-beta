#pragma once
#include <cstdint>
using namespace cocos2d;

class PlayerObject;

namespace cocos2d {
	class CCSprite;
}

namespace echo::dash {
	void prepare();
	bool end_effect_skippable(PlayerObject* player);
	enum class EndEffect : std::uint8_t {
		Native = 0,
		Skip = 1,
		Record = 2,
		RecordOnly = 3,
	};
	void set_end_effect(EndEffect mode);
	bool gpu_effects_ready();
	bool merge_ready();
	bool prefer_gpu();
	void set_merge(bool value);
	void reset_pending_pair();
	void pair_native(CCSprite* root, CCSprite* outline);
}

extern "C" void echo_record_dash_end(PlayerObject* player) noexcept;
extern volatile bool echo_dash_compare;
extern volatile bool echo_dash_show_gpu;
extern volatile std::uint64_t echo_dash_compared, echoDashMismatches, echoDashLifeMismatches;
