#pragma once
#include <cmath>

namespace echo::waves {
	using Power = float (*)(float, float);

	// NOT from + delta * t
	// (to - delta * (1 - t))
	template <bool fused>
	float tween(float from, float to, float time) {
		float delta = to - from;
		float left = 1.f - time;
		if constexpr (fused)
			return std::fma(-delta, left, to);
		else {
			volatile float product = delta * left;
			return to - product;
		}
	}

	class Timeline {
		float from_radius = 0.f, to_radius = 0.f;
		bool fadeIn = false, easeOut = false;
		int opacity_phase = -1;
		bool finished = false;

	public:
		void reset(float from, float to, bool fade, bool ease) {
			from_radius = from;
			to_radius = to;
			fadeIn = fade;
			easeOut = ease;
			opacity_phase = -1;
			finished = false;
		}

		template <bool fused, class Target>
		void update(float time, Power power, Target& target) {
			if (finished)
				return;
			float radius_time = !fadeIn && easeOut ? power(time, 0.5f) : time;
			target.radius(tween<fused>(from_radius, to_radius, radius_time));
			if (!fadeIn)
				target.opacity(tween<fused>(255.f, 0.f, radius_time));
			else {
				int phase = time < 0.5f ? 0 : 1;
				if (phase == 1 && opacity_phase != 1)
					target.opacity(tween<fused>(0.f, 255.f, 1.f));
				else if (phase == 0 && opacity_phase == 1)
					target.opacity(tween<fused>(255.f, 0.f, 0.f));
				float local = phase ? (time - 0.5f) / 0.5f : time / 0.5f;
				target.opacity(phase ? tween<fused>(255.f, 0.f, local)
									 : tween<fused>(0.f, 255.f, local));
				opacity_phase = phase;
			}
			if (time >= 1.f) {
				finished = true;
				target.remove();
			}
		}
	};
}
