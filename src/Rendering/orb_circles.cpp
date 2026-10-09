#include <Geode/Geode.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include "circle_geometry.hpp"
#include "circle_signatures.hpp"
#include "wave_merge.hpp"
#include <algorithm>
#include <bit>

using namespace geode::prelude;
#if defined(GEODE_IS_WINDOWS) || defined(GEODE_IS_ANDROID64)
namespace {
thread_local unsigned drawingWave = 0;
// Identical rings the ring being drawn stands for (merged rings, Windows only).
thread_local float waveCount = 1.f;
using Circle = void (*)(CCPoint const&, float, float, unsigned, bool, float, float);
using FilledCircle = void (*)(CCPoint const&, float, float, unsigned);
Circle nativeCircle = nullptr;
FilledCircle nativeFilledCircle = nullptr;
CCGLProgram** shader = nullptr;
GLint* colorUniform = nullptr;
float* drawColor = nullptr;
echo::circles::Trig nativeCosine = nullptr, nativeSine = nullptr;
#ifdef GEODE_IS_ANDROID64
constexpr bool fused = true;
#else
constexpr bool fused = false;
#endif
bool drawCached(CCPoint const& center, float radius, float angle, unsigned segments, bool filled) {
    // Only standard wave meshes. Unusual primitives and other callers keep
    // the original renderer; preserve NaN/infinity and signed-zero behavior.
    if (!drawingWave || std::bit_cast<uint32_t>(angle) != 0 ||
        !std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(radius) ||
        !shader || !*shader) return false;
    int slot = echo::circles::Geometry::index(segments);
    if (slot < 0) return false;
    if (!filled && drawColor[3] <= 0.f) return true;
    static thread_local echo::circles::Geometry geometry;
    std::array<echo::circles::Point, echo::circles::maxVertices> vertices;
    geometry.vertices<fused>(slot, {center.x, center.y}, radius, filled,
                              nativeCosine, nativeSine, vertices);
    // Same shader, uniforms, transform, GL mode, vertex count, blend state
    // and draw order as the native primitives.
    (*shader)->use();
    (*shader)->setUniformsForBuiltins();
    float stacked[4];
    float const* color = drawColor;
    // A merged ring stands for N identical additive rings (SRC_ALPHA, ONE). They add
    // N * color * alpha, so draw that premultiplied with (ONE, ONE). Where N copies would
    // saturate, the clamped single draw saturates too.
    bool merged = waveCount > 1.f;
    if (merged) {
        float alpha = drawColor[3];
        for (int i = 0; i < 3; ++i) stacked[i] = std::min(drawColor[i] * alpha * waveCount, 1.f);
        stacked[3] = std::min(alpha * waveCount, 1.f);
        color = stacked;
        ccGLBlendFunc(GL_ONE, GL_ONE);
    }
    (*shader)->setUniformLocationWith4fv(*colorUniform, const_cast<float*>(color), 1);
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_Position);
    glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, 0, vertices.data());
    glDrawArrays(filled ? GL_TRIANGLE_FAN : GL_LINE_STRIP, 0, segments + 1);
    CC_INCREMENT_GL_DRAWS(1);
    if (merged) ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE);
    return true;
}
void cachedCircle(CCPoint const& center, float radius, float angle, unsigned segments,
                  bool lineToCenter, float scaleX, float scaleY) {
    if (lineToCenter || scaleX != 1.f || scaleY != 1.f ||
        !drawCached(center, radius, angle, segments, false))
        nativeCircle(center, radius, angle, segments, lineToCenter, scaleX, scaleY);
}
void cachedFilledCircle(CCPoint const& center, float radius, float angle, unsigned segments) {
    if (!drawCached(center, radius, angle, segments, true))
        nativeFilledCircle(center, radius, angle, segments);
}
}
class $modify(EchoCircleWave, CCCircleWave) {
    void draw() {
        ++drawingWave;
#ifdef GEODE_IS_WINDOWS
        waveCount = echo::waves::countOf(this);
#endif
        CCCircleWave::draw();
        waveCount = 1.f;
        --drawingWave;
    }
};

$execute {
#ifdef GEODE_IS_WINDOWS
    uintptr_t module = geode::base::getCocos();
    constexpr uintptr_t circleOffset = 0x10a40, filledOffset = 0x11230;
    constexpr uintptr_t shaderOffset = 0x1a8338, uniformOffset = 0x1a2b58, colorOffset = 0x1a2b60;
    constexpr auto circleSignature = echo::circles::winCircle;
    constexpr auto filledSignature = echo::circles::winFilled;
#else
    uintptr_t module = geode::base::get();
    constexpr uintptr_t circleOffset = 0xad83a0, filledOffset = 0xad8714;
    constexpr uintptr_t shaderOffset = 0x1245198, uniformOffset = 0x1230cc8, colorOffset = 0x1230cd0;
    constexpr auto circleSignature = echo::circles::armCircle;
    constexpr auto filledSignature = echo::circles::armFilled;
#endif
    auto matches = [module](echo::circles::Signature const& signature) {
        return echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(module + signature.offset),
                                          signature.size) == signature.hash;
    };
    if (!matches(circleSignature) || !matches(filledSignature)) {
        log::warn("Echo circle renderer instructions differ; optimization skipped");
        return;
    }
#ifdef GEODE_IS_WINDOWS
    // Use the game's own CRT trig functions, not the mod's potentially different CRT.
    nativeCosine = *reinterpret_cast<echo::circles::Trig*>(module + 0xe4ac8);
    nativeSine = *reinterpret_cast<echo::circles::Trig*>(module + 0xe4ad0);
#else
    nativeCosine = &::cosf;
    nativeSine = &::sinf;
#endif
    shader = reinterpret_cast<CCGLProgram**>(module + shaderOffset);
    colorUniform = reinterpret_cast<GLint*>(module + uniformOffset);
    drawColor = reinterpret_cast<float*>(module + colorOffset);
    nativeCircle = reinterpret_cast<Circle>(module + circleOffset);
    nativeFilledCircle = reinterpret_cast<FilledCircle>(module + filledOffset);
    if (!Mod::get()->hook(reinterpret_cast<void*>(nativeCircle), &cachedCircle, "Echo circle mesh cache"))
        log::warn("Echo circle mesh cache hook failed");
    if (!Mod::get()->hook(reinterpret_cast<void*>(nativeFilledCircle), &cachedFilledCircle, "Echo filled circle mesh cache"))
        log::warn("Echo filled circle mesh cache hook failed");
}
#endif
