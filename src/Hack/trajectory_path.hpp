#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

namespace echo::trajectory {
	inline int prediction_steps(float seconds, float rate, float warp) {
		double count = std::ceil(seconds * rate / std::clamp(warp, 0.01f, 1.f));
		if (!std::isfinite(count))
			return 1;
		return static_cast<int>(std::clamp(count, 1., double(std::numeric_limits<int>::max() - 1)));
	}

	template <class Point, class Emit>
	void draw_path(std::vector<Point> const& points, Emit emit) {
		if (points.size() < 2)
			return;
		auto start = points.front();
		auto end = start;
		for (size_t i = 1; i < points.size(); ++i) {
			auto next = points[i];
			double ax = double(end.x) - start.x, ay = double(end.y) - start.y;
			double bx = double(next.x) - end.x, by = double(next.y) - end.y;
			if (ax * by == ay * bx && ax * bx + ay * by >= 0.)
				end = next;
			else {
				emit(start, end);
				start = end;
				end = next;
			}
		}
		emit(start, end);
	}
}
