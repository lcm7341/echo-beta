#pragma once
#include <cstdint>
#include <cstring>

namespace OBB {
	using TrigFunction = float (*)(float);

	const std::size_t BOX_CORNERS = 0x140, BOX_EDGES = 0x180, BOX_PROJECTIONS = 0x1a0, BOX_CENTER = 0x1b0;

	struct Point {
		float x, y;
	};

	// write straight into gds OBB2D, at the offset it keeps that value
	inline void write_point(unsigned char* box, std::size_t offset, Point p) {
		std::memcpy(box + offset, &p, sizeof(p));
	}

	inline void write_double(unsigned char* box, std::size_t offset, double value) {
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

	inline float dot(Point corner, Point edge) {
		return add(mul(corner.y, edge.y), mul(corner.x, edge.x));
	}

	inline void calculate(unsigned char* box, Point center, float width, float height, float angle,
		TrigFunction cosine, TrigFunction sine, float half, float one) {
		write_point(box, BOX_CENTER, center);
		Point axis_x{ cosine(angle), sine(angle) }, axis_y{ -sine(angle), cosine(angle) };
		axis_x = scale(axis_x, mul(width, half));
		axis_y = scale(axis_y, mul(height, half));
		Point corner0 = sub(sub(center, axis_x), axis_y), corner1 = sub(add(center, axis_x), axis_y), corner2 = add(add(center, axis_x), axis_y);
		Point corner3 = add(sub(center, axis_x), axis_y);
		write_point(box, BOX_CORNERS, corner0);
		write_point(box, BOX_CORNERS + 8, corner1);
		write_point(box, BOX_CORNERS + 16, corner2);
		write_point(box, BOX_CORNERS + 24, corner3);
		Point edge0 = sub(corner1, corner0), edge1 = sub(corner3, corner0);
		edge0 = scale(edge0, div(one, length_squared(edge0)));
		write_point(box, BOX_EDGES, edge0);
		write_double(box, BOX_PROJECTIONS, (double)dot(corner0, edge0));
		edge1 = scale(edge1, div(one, length_squared(edge1)));
		write_point(box, BOX_EDGES + 8, edge1);
		write_double(box, BOX_PROJECTIONS + 8, (double)dot(corner0, edge1));
	}
}
