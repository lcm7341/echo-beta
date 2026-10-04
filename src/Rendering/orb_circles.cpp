// cocos works out every point of a circle w/ sin & cos each time 1 is drawn. the points for a segment count are worked out
//   once (circle_geometry.hpp), after that a circle is just those points scaled & moved
// they are made w/ gds own sinf / cosf so they are exactly what cocos would have drawn
#include <Geode/Geode.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include "circle_geometry.hpp"
#include "circle_signatures.hpp"
#include "wave_merge.hpp"
#include <algorithm>
#include <bit>

using namespace geode::prelude;
namespace {
    thread_local unsigned drawing_wave = 0;
    thread_local float wave_count = 1.f;
    using Circle = void (*)(CCPoint const&, float, float, unsigned, bool, float, float);
    using FilledCircle = void (*)(CCPoint const&, float, float, unsigned);
    Circle native_circle = nullptr;
    FilledCircle native_filled_circle = nullptr;
    CCGLProgram** shader = nullptr;
    GLint* color_uniform = nullptr;
    float* draw_color = nullptr;
    echo::circles::Trig native_cosine = nullptr, native_sine = nullptr;
    constexpr bool fused = false;

    bool draw_cached(CCPoint const& center, float radius, float angle, unsigned segments, bool filled) {
        if (!drawing_wave || std::bit_cast<uint32_t>(angle) != 0 ||
            !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius) ||
            !shader || !*shader)
            return false;
        int slot = echo::circles::Geometry::index(segments);
        if (slot < 0)
            return false;
        if (!filled && draw_color[3] <= 0.f)
            return true;
        static thread_local echo::circles::Geometry geometry;
        std::array<echo::circles::Point, echo::circles::max_vertices> vertices;
        geometry.vertices<fused>(slot, { center.x, center.y }, radius, filled,
            native_cosine, native_sine, vertices);
        (*shader)->use();
        (*shader)->setUniformsForBuiltins();
        float stacked[4];
        float const* color = draw_color;
        bool merged = wave_count > 1.f;
        if (merged) {
            float alpha = draw_color[3];
            for (int i = 0; i < 3; ++i)
                stacked[i] = std::min(draw_color[i] * alpha * wave_count, 1.f);
            stacked[3] = std::min(alpha * wave_count, 1.f);
            color = stacked;
            ccGLBlendFunc(GL_ONE, GL_ONE);
        }
        (*shader)->setUniformLocationWith4fv(*color_uniform, const_cast<float*>(color), 1);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
        glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
        glDrawArrays(filled ? GL_TRIANGLE_FAN : GL_LINE_STRIP, 0, segments + 1);
        CC_INCREMENT_GL_DRAWS(1);
        if (merged)
            ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE);
        return true;
    }

    void cached_circle(CCPoint const& center, float radius, float angle, unsigned segments,
        bool line_to_center, float scaleX, float scaleY) {
        if (line_to_center || scaleX != 1.f || scaleY != 1.f ||
            !draw_cached(center, radius, angle, segments, false))
            native_circle(center, radius, angle, segments, line_to_center, scaleX, scaleY);
    }

    void cached_filled_circle(CCPoint const& center, float radius, float angle, unsigned segments) {
        if (!draw_cached(center, radius, angle, segments, true))
            native_filled_circle(center, radius, angle, segments);
    }
}

class $modify(EchoCircleWave, CCCircleWave) {
    void draw() {
        ++drawing_wave;
        wave_count = echo::waves::count_of(this);
        CCCircleWave::draw();
        wave_count = 1.f;
        --drawing_wave;
    }
};

$execute {
    uintptr_t module = geode::base::getCocos();
    constexpr uintptr_t circle_offset = 0x10a40, filled_offset = 0x11230;
    constexpr uintptr_t shader_offset = 0x1a8338, uniform_offset = 0x1a2b58, color_offset = 0x1a2b60;
    constexpr auto circle_signature = echo::circles::win_circle;
    constexpr auto filled_signature = echo::circles::win_filled;
    auto matches = [module](echo::circles::Signature const& signature) {
        return echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(module + signature.offset),
                   signature.size) == signature.hash;
    };
    if (!matches(circle_signature) || !matches(filled_signature)) {
        return;
    }
    native_sine = *reinterpret_cast<echo::circles::Trig*>(module + 0xe4ad0);   // sinf
    shader = reinterpret_cast<CCGLProgram**>(module + shader_offset);
    color_uniform = reinterpret_cast<GLint*>(module + uniform_offset);
    draw_color = reinterpret_cast<float*>(module + color_offset);
    native_circle = reinterpret_cast<Circle>(module + circle_offset);
    native_filled_circle = reinterpret_cast<FilledCircle>(module + filled_offset);
    (void)Mod::get()->hook(reinterpret_cast<void*>(native_circle), &cached_circle, "Echo circle mesh cache");
    (void)Mod::get()->hook(reinterpret_cast<void*>(native_filled_circle), &cached_filled_circle, "Echo filled circle mesh cache");
}
