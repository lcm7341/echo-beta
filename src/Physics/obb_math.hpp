// the oriented box math for obb_fast.cpp
// addss / subss / mulss / divss each do exactly 1 float op ON PURPOSE. the compiler must not merge or reorder them
//   bc the result has to match gds own function bit for bit
#pragma once
#include <cstdint>
#include <cstring>

namespace echo::obb {
	using Trig = float (*)(float);

	struct Point {
		float x, y;
	};

	inline void store(std::uint8_t* object, std::size_t offset, Point p) {
		std::memcpy(object + offset, &p, sizeof(p));
	}

	inline void store_double(std::uint8_t* object, std::size_t offset, double value) {
		std::memcpy(object + offset, &value, sizeof(value));
	}

	inline float addss(float destination, float source) {
		asm("addss %1, %0" : "+x"(destination) : "x"(source));
		return destination;
	}

	inline float subss(float destination, float source) {
		asm("subss %1, %0" : "+x"(destination) : "x"(source));
		return destination;
	}

	inline float mulss(float destination, float source) {
		asm("mulss %1, %0" : "+x"(destination) : "x"(source));
		return destination;
	}

	inline float divss(float destination, float source) {
		asm("divss %1, %0" : "+x"(destination) : "x"(source));
		return destination;
	}

	inline Point sub(Point a, Point b) {
		return { subss(a.x, b.x), subss(a.y, b.y) };
	}

	inline Point add(Point a, Point b) {
		return { addss(b.x, a.x), addss(b.y, a.y) };
	}

	inline Point scale(Point a, float s) {
		return { mulss(s, a.x), mulss(s, a.y) };
	}

	inline float length_squared(Point a) {
		return addss(mulss(a.x, a.x), mulss(a.y, a.y));
	}

	inline float project(Point c, Point e) {
		return addss(mulss(c.y, e.y), mulss(c.x, e.x));
	}

	inline void calculate(std::uint8_t* obb, Point center, float width, float height, float angle,
		Trig cosine, Trig sine, float half, float one) {
		store(obb, 0x1b0, center);
		Point x{ cosine(angle), sine(angle) }, y{ -sine(angle), cosine(angle) };
		x = scale(x, mulss(width, half));
		y = scale(y, mulss(height, half));
		Point c0 = sub(sub(center, x), y), c1 = sub(add(center, x), y), c2 = add(add(center, x), y);
		Point c3 = add(sub(center, x), y);
		store(obb, 0x140, c0);
		store(obb, 0x148, c1);
		store(obb, 0x150, c2);
		store(obb, 0x158, c3);
		Point e0 = sub(c1, c0), e1 = sub(c3, c0);
		e0 = scale(e0, divss(one, length_squared(e0)));
		store(obb, 0x180, e0);
		store_double(obb, 0x1a0, static_cast<double>(project(c0, e0)));
		e1 = scale(e1, divss(one, length_squared(e1)));
		store(obb, 0x188, e1);
		store_double(obb, 0x1a8, static_cast<double>(project(c0, e1)));
	}
}
