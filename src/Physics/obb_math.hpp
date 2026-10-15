// the box math for obb_fast.cpp (the rotated hitbox of an object: its 4 corners & its 2 sides)
// add / sub / mul / div each do exactly 1 float op ON PURPOSE. the compiler must NOT merge or reorder them,
//   the result has to match gds own function bit for bit or macros stop syncing
// so dont "clean up" the order of anything in here, a + b & b + a are not the same thing to ts file
#pragma once
#include <cstdint>
#include <cstring>

namespace echo::obb {
	using Trig = float (*)(float);

	struct Point {
		float x, y;
	};

	// write straight into gds OBB2D, at the offset it keeps that value
	inline void put(std::uint8_t* box, std::size_t offset, Point p) {
		std::memcpy(box + offset, &p, sizeof(p));
	}

	inline void put_double(std::uint8_t* box, std::size_t offset, double value) {
		std::memcpy(box + offset, &value, sizeof(value));
	}

	inline float add(float a, float b) {
		asm("addss %1, %0" : "+x"(a) : "x"(b));
		return a;
	}

	inline float sub(float a, float b) {
		asm("subss %1, %0" : "+x"(a) : "x"(b));
		return a;
	}

	inline float mul(float a, float b) {
		asm("mulss %1, %0" : "+x"(a) : "x"(b));
		return a;
	}

	inline float div(float a, float b) {
		asm("divss %1, %0" : "+x"(a) : "x"(b));
		return a;
	}

	inline Point sub(Point a, Point b) {
		return { sub(a.x, b.x), sub(a.y, b.y) };
	}

	// b + a, NOT a + b. thats the order gd does it in
	inline Point add(Point a, Point b) {
		return { add(b.x, a.x), add(b.y, a.y) };
	}

	inline Point scale(Point a, float s) {
		return { mul(s, a.x), mul(s, a.y) };
	}

	inline float length_squared(Point a) {
		return add(mul(a.x, a.x), mul(a.y, a.y));
	}

	inline float along(Point corner, Point side) {
		return add(mul(corner.y, side.y), mul(corner.x, side.x));
	}

	inline void calculate(std::uint8_t* box, Point center, float width, float height, float angle,
		Trig cosine, Trig sine, float half, float one) {
		put(box, 0x1b0, center);
		Point x{ cosine(angle), sine(angle) }, y{ -sine(angle), cosine(angle) };
		x = scale(x, mul(width, half));
		y = scale(y, mul(height, half));
		Point c0 = sub(sub(center, x), y), c1 = sub(add(center, x), y), c2 = add(add(center, x), y);
		Point c3 = add(sub(center, x), y);
		put(box, 0x140, c0);
		put(box, 0x148, c1);
		put(box, 0x150, c2);
		put(box, 0x158, c3);
		Point side0 = sub(c1, c0), side1 = sub(c3, c0);
		side0 = scale(side0, div(one, length_squared(side0)));
		put(box, 0x180, side0);
		put_double(box, 0x1a0, static_cast<double>(along(c0, side0)));
		side1 = scale(side1, div(one, length_squared(side1)));
		put(box, 0x188, side1);
		put_double(box, 0x1a8, static_cast<double>(along(c0, side1)));
	}
}
