// the points of a unit circle per segment count, worked out once & reused. see orb_circles.cpp
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace echo::circles {
	struct Point {
		float x, y;
	};

	inline constexpr std::array<unsigned, 5> segment_counts{ 10, 15, 20, 30, 50 };
	inline constexpr size_t max_vertices = 52;
	using Trig = float (*)(float);

	template <bool fused>
	float multiply_add(float a, float b, float c) {
		if constexpr (fused)
			return std::fma(a, b, c);
		else {
			volatile float product = a * b;
			return product + c;
		}
	}

	class Geometry {
		std::array<std::array<Point, max_vertices>, 5> directions{};
		std::array<bool, 5> ready{};

	public:
		static int index(unsigned segments) {
			for (size_t i = 0; i < segment_counts.size(); ++i)
				if (segments == segment_counts[i])
					return static_cast<int>(i);
			return -1;
		}

		template <bool fused>
		void vertices(int slot, Point center, float radius, bool filled,
			Trig cosine, Trig sine, std::array<Point, max_vertices>& out) {
			unsigned segments = segment_counts[slot];
			if (!ready[slot]) {
				float step = 0x1.921fb6p+2f / static_cast<float>(segments);
				for (unsigned i = 0; i <= segments; ++i) {
					float angle = multiply_add<fused>(static_cast<float>(i), step, 0.f);
					directions[slot][i] = { cosine(angle), sine(angle) };
				}
				ready[slot] = true;
			}
			for (unsigned i = 0; i <= segments; ++i) {
				auto const p = directions[slot][i];
				if (filled)
					out[i] = { multiply_add<fused>(p.x, radius, center.x),
						multiply_add<fused>(p.y, radius, center.y) };
				else
					out[i] = { multiply_add<fused>(p.x * radius, 1.f, center.x),
						multiply_add<fused>(p.y * radius, 1.f, center.y) };
			}
			out[segments + 1] = center;
		}
	};
}
