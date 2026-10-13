#pragma once

namespace echo::obb {
	template <class Point>
	inline float dot(Point const& a, Point const& b) {
#pragma clang fp contract(off)
		// arm's fused dot moves pad contacts by a step at high tps
		return a.x * b.x + a.y * b.y;
	}

	template <class Corners, class Edges, class Projections>
	inline void compute_axes(Corners const& corners, Edges& edges, Projections& projections) {
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
	inline bool overlaps_one_way(Edges const& edges, Projections const& projections, Corners const& other) {
		for (int axis = 0; axis < 2; ++axis) {
			auto const& edge = edges[axis];
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
