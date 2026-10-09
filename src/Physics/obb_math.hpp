#pragma once
#include <cstdint>
#include <cstring>

// OBB2D::calculateWithCenter (GD 2.2081 Windows) without its ~22 calls into libcocos2d's CCPoint
// operators. Each of those is one scalar float add, subtract or multiply per component; this
// does the same operations in the same order, so every stored bit matches.
namespace echo::obb {
using Trig = float (*)(float);
struct Point { float x, y; };

inline void store(std::uint8_t* object, std::size_t offset, Point p) { std::memcpy(object + offset, &p, sizeof(p)); }
inline void storeDouble(std::uint8_t* object, std::size_t offset, double value) { std::memcpy(object + offset, &value, sizeof(value)); }
// Scalar SSE with the native operand order. Values are the same either way, but when both
// operands are NaN the result keeps the first (destination) operand's NaN, and a compiler may
// swap the operands of an add or multiply. One instruction each keeps the order fixed.
inline float addss(float destination, float source) { asm("addss %1, %0" : "+x"(destination) : "x"(source)); return destination; }
inline float subss(float destination, float source) { asm("subss %1, %0" : "+x"(destination) : "x"(source)); return destination; }
inline float mulss(float destination, float source) { asm("mulss %1, %0" : "+x"(destination) : "x"(source)); return destination; }
inline float divss(float destination, float source) { asm("divss %1, %0" : "+x"(destination) : "x"(source)); return destination; }
inline Point sub(Point a, Point b) { return {subss(a.x, b.x), subss(a.y, b.y)}; }
// CCPoint::operator+ loads the other point and adds this one: other + this, per component.
inline Point add(Point a, Point b) { return {addss(b.x, a.x), addss(b.y, a.y)}; }
// CCPoint::operator*(float) loads the factor and multiplies each component into it.
inline Point scale(Point a, float s) { return {mulss(s, a.x), mulss(s, a.y)}; }
// Squared length as the routine sums it: x*x + y*y.
inline float lengthSquared(Point a) { return addss(mulss(a.x, a.x), mulss(a.y, a.y)); }
// Projection onto corner 0 as summed: c.y*e.y + c.x*e.x.
inline float project(Point c, Point e) { return addss(mulss(c.y, e.y), mulss(c.x, e.x)); }

// Everything before the native corner-ordering helper (0x6dda0), which the caller still runs.
// half and one are the game's own 0.5f and 1.0f constants; cosine and sine its CRT imports.
inline void calculate(std::uint8_t* obb, Point center, float width, float height, float angle,
                      Trig cosine, Trig sine, float half, float one) {
    store(obb, 0x1b0, center); // m_center
    Point x{cosine(angle), sine(angle)};
    Point y{-sine(angle), cosine(angle)};
    x = scale(x, mulss(width, half));
    y = scale(y, mulss(height, half));
    Point c0 = sub(sub(center, x), y);
    Point c1 = sub(add(center, x), y);
    Point c2 = add(add(center, x), y);
    Point c3 = add(sub(center, x), y);
    store(obb, 0x140, c0); store(obb, 0x148, c1); store(obb, 0x150, c2); store(obb, 0x158, c3);
    Point e0 = sub(c1, c0), e1 = sub(c3, c0);
    // Each edge is divided by its squared length (x*x + y*y) and projected onto corner 0,
    // summed as y*y' + x*x' and widened to double.
    e0 = scale(e0, divss(one, lengthSquared(e0)));
    store(obb, 0x180, e0);
    storeDouble(obb, 0x1a0, static_cast<double>(project(c0, e0)));
    e1 = scale(e1, divss(one, lengthSquared(e1)));
    store(obb, 0x188, e1);
    storeDouble(obb, 0x1a8, static_cast<double>(project(c0, e1)));
}
}
