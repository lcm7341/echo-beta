#pragma once

namespace OBB {
	template <class Point>
	inline float dot(const Point& a, const Point& b) {
#pragma clang fp contract(off)
		return a.x * b.x + a.y * b.y;
	}

	template <class Corners, class Edges, class Projections>
	inline void compute_axes(const Corners& corners, Edges& edges, Projections& projections) {
		for (int axis = 0; axis < 2; ++axis) {
			auto edge = corners[axis == 0 ? 1 : 3];
			edge.x -= corners[0].x;
			edge.y -= corners[0].y;
			float scale = 1.f / dot(edge, edge);
			edge.x *= scale;
			edge.y *= scale;
			edges[axis] = edge;
			projections[axis] = dot(corners[0], edge);
		}
	}

	template <class Edges, class Projections, class Corners>
	inline bool overlaps_one_way(const Edges& edges, const Projections& projections, const Corners& other) {
		for (int axis = 0; axis < 2; ++axis) {
			const auto& edge = edges[axis];
			double origin = projections[axis];
			double low = dot(other[0], edge), high = low;
			for (int i = 1; i < 4; ++i) {
				double value = dot(other[i], edge);
				if (value < low)
					low = value;
				else if (value > high)
					high = value;
			}
			if (low > origin + 1.0 || high < origin)
				return false;
		}
		return true;
	}
}
