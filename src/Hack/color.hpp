#pragma once

#include <Geode/Geode.hpp>

using namespace geode::prelude;

struct HitboxColor {
	static ccColor4F line_color(float const* color) {
		return { color[0], color[1], color[2], color[3] };
	}

	static ccColor4F fill_color(float const* color, bool fill, float opacity) {
		if (!fill)
			return { 0.f, 0.f, 0.f, 0.f };
		return { color[0], color[1], color[2], color[3] * opacity };
	}
};
