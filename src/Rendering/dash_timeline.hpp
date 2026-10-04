#pragma once
#include <cstdint>

namespace echo::dash {
	struct Timeline {
		float end_x{}, end_y{}, start_x{}, start_y{}, deltaX{}, deltaY{};
		float start_px{}, start_py{}, previous_x{}, previous_y{};
		std::uint8_t opacity{};
		bool scale_started = false, removed = false;

		template <class Target>
		void start(Target& target, float x, float y) {
			end_x = x;
			end_y = y;
			start_px = previous_x = target.pos_x();
			start_py = previous_y = target.pos_y();
			opacity = target.opacity();
			scale_started = removed = false;
		}

		template <class Target>
		void update(float time, Target& target) {
			if (removed)
				return;
			if (!scale_started) {
				start_x = target.scaleX();
				start_y = target.scaleY();
				deltaX = end_x - start_x;
				deltaY = end_y - start_y;
				scale_started = true;
			}
			target.scale(start_x + deltaX * time, start_y + deltaY * time);
			if (time >= 1.f) {
				removed = true;
				target.remove();
				return;
			}
			float difference_x = target.pos_x() - previous_x, difference_y = target.pos_y() - previous_y;
			start_px = start_px + difference_x;
			start_py = start_py + difference_y;
			float x = start_px + -20.f * time, y = start_py + 0.f * time;
			target.position(x, y);
			previous_x = x;
			previous_y = y;
			target.opacity(static_cast<std::uint8_t>(opacity + (0.f - opacity) * time));
		}
	};
}
