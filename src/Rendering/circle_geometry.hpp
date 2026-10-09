#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace echo::circles {
struct Point { float x, y; };
inline constexpr std::array<unsigned, 5> segmentCounts{10, 15, 20, 30, 50};
inline constexpr size_t maxVertices = 52;
using Trig = float (*)(float);

// The native ARM64 primitive uses FMA; the Windows primitive rounds each
// multiplication/addition separately. Keep both orders, including the seam.
template<bool fused>
float multiplyAdd(float a, float b, float c) {
    if constexpr (fused) return std::fma(a, b, c);
    else { volatile float product = a * b; return product + c; }
}
class Geometry {
    std::array<std::array<Point, maxVertices>, 5> directions{};
    std::array<bool, 5> ready{};
public:
    static int index(unsigned segments) {
        for (size_t i = 0; i < segmentCounts.size(); ++i)
            if (segments == segmentCounts[i]) return static_cast<int>(i);
        return -1;
    }
    template<bool fused>
    void vertices(int slot, Point center, float radius, bool filled,
                  Trig cosine, Trig sine, std::array<Point, maxVertices>& out) {
        unsigned segments = segmentCounts[slot];
        if (!ready[slot]) {
            float step = 0x1.921fb6p+2f / static_cast<float>(segments);
            for (unsigned i = 0; i <= segments; ++i) {
                float angle = multiplyAdd<fused>(static_cast<float>(i), step, 0.f);
                directions[slot][i] = {cosine(angle), sine(angle)};
            }
            ready[slot] = true;
        }
        for (unsigned i = 0; i <= segments; ++i) {
            auto const p = directions[slot][i];
            // Outlined circles first multiply by radius, then scale (1), then
            // add the center; filled circles use FMA directly on ARM64.
            if (filled) out[i] = {multiplyAdd<fused>(p.x, radius, center.x),
                                  multiplyAdd<fused>(p.y, radius, center.y)};
            else out[i] = {multiplyAdd<fused>(p.x * radius, 1.f, center.x),
                           multiplyAdd<fused>(p.y * radius, 1.f, center.y)};
        }
        out[segments + 1] = center;
    }
};
}
