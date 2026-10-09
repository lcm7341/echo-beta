#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "circle_signatures.hpp"
#include "dash_end.hpp"
#include "Logic/logic.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <cctype>
#include <chrono>
#include <string>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

using namespace geode::prelude;

volatile bool echoDashCompare = false;
volatile bool echoDashShowGpu = true;
volatile std::uint64_t echoDashCompared = 0, echoDashMismatches = 0, echoDashLifeMismatches = 0;

// Dash end effects drawn without a sprite per effect.
//
// Native stopDashing builds two sprites per effect: an additive root at z 40 in the object layer
// and a 150-opacity outline child, then fades, shrinks and moves them over 0.2 s of action time.
// A high TPS macro can keep tens of thousands alive, and as sprites each one costs its own
// creation, two actions, a scene-graph visit and a draw call. Here each effect is a record: it
// is read from the player where the game's own block would read it, advanced by one action on
// one node with the action manager's dt, and every record is drawn in one instanced call.
//
// The arithmetic follows the game's libcocos2d (fingerprinted below): CCActionInterval::step,
// Echo's DashAnimation timeline for the root, CCFadeTo for the outline, CCSprite::updateColor,
// CCNode::nodeToParentTransform with the game's cosf/sinf, and CCNode::transform's
// kmMat4Multiply chain. The GPU only applies each sprite's MVP to its quad, as the native shader
// does. Compare mode builds both and checks every value against the native sprites.
namespace {
using Trig = float (*)(float);
bool softwareRenderer = false;
Trig nativeCos = nullptr, nativeSin = nullptr;
float degreesToRadians = 0.f;

struct Affine { float a, b, c, d, tx, ty; };
struct QuadTemplate {
    std::array<ccVertex3F, 4> position{};
    std::array<ccTex2F, 4> uv{};
    GLuint texture = 0;
    bool premultiplied = false;
    bool sourceAlpha = false, destinationOne = false;
};

// CCNode::nodeToParentTransform, no skew, no additional transform, anchor not ignored.
Affine nodeTransform(float x, float y, float scaleX, float scaleY, float cx, float sx, float cy, float sy, CCPoint anchor) {
    if (!(anchor.x == 0.f && anchor.y == 0.f)) {
        float ax = anchor.x, ay = -anchor.y;
        float dx = (-ax * cy) * scaleX;
        float dy = (ay * sx) * scaleY;
        float ex = (ax * sy) * scaleX;
        float ey = (ay * cx) * scaleY;
        x = x + (dx - dy);
        y = y + (ey - ex);
    }
    return {scaleX * cy, scaleX * sy, -sx * scaleY, scaleY * cx, x, y};
}

// CCNode::transform: CGAffineToGL with vertexZ in m[14].
kmMat4 toMatrix(Affine const& t) {
    kmMat4 m{};
    m.mat[0] = t.a; m.mat[1] = t.b; m.mat[4] = t.c; m.mat[5] = t.d;
    m.mat[12] = t.tx; m.mat[13] = t.ty; m.mat[10] = 1.f; m.mat[15] = 1.f;
    return m;
}

// CCSprite::updateColor.
ccColor4B spriteColor(ccColor3B color, GLubyte opacity, bool premultiplied) {
    ccColor4B out{color.r, color.g, color.b, opacity};
    if (premultiplied) {
        float scale = static_cast<float>(opacity) / 255.f;
        out.r = static_cast<GLubyte>(static_cast<int>(static_cast<float>(color.r) * scale));
        out.g = static_cast<GLubyte>(static_cast<int>(static_cast<float>(color.g) * scale));
        out.b = static_cast<GLubyte>(static_cast<int>(static_cast<float>(color.b) * scale));
    }
    return out;
}

struct Cohort {
    float elapsed = 0.f, time = 0.f;
    bool ticked = false;
};

struct Effect {
    std::uint64_t cohort = 0;
    float x = 0.f, y = 0.f, scaleX = 0.f, scaleY = 0.f, endX = 0.f, endY = 0.f;
    float cx = 1.f, sx = 0.f, cy = 1.f, sy = 0.f;
    ccColor3B color{};
    // Consecutive bit-identical, same-update effects this record stands for. Drawing expands
    // them in order so every native framebuffer blend (including rounding/clamping) survives.
    float count = 1.f;
    CCSprite* nativeRoot = nullptr;
    CCSprite* nativeOutline = nullptr;
};

// The native sprites' state at the effect's current action time.
struct State {
    Affine root, outline;
    ccColor4B rootColor, outlineColor;
};

struct Instance {
    float root[16];
    float outline[16];
    ccColor4B rootColor, outlineColor;
};

struct Renderer {
    bool tried = false, ready = false;
    QuadTemplate root, outline;
    CCPoint rootAnchor, outlineAnchor;
    Affine outlineLocal{};
    CCGLProgram* program = nullptr;
    GLuint vao = 0, buffer = 0;
    GLint quadPositions = -1, quadUVs = -1, blendModes = -1, samplerRoot = -1, samplerOutline = -1;
    std::vector<Instance> instances;
};
Renderer renderer;

class DashEffectNode;
DashEffectNode* currentNode = nullptr;
// The record made in the current stopDashing, for compare mode's pairing.
std::size_t pendingPair = SIZE_MAX;
// Set around stopDashing when identical effects merge instead of each keeping its own state.
bool merging = false;

State stateAt(Effect const& effect, Cohort const& cohort) {
    auto& r = renderer;
    float scaleX = effect.scaleX, scaleY = effect.scaleY, x = effect.x, y = effect.y;
    GLubyte rootOpacity = 255, outlineOpacity = 150;
    if (cohort.ticked) {
        // DashAnimation's timeline (scale, stackable move, fade) and CCFadeTo, in native order.
        float t = cohort.time;
        scaleX = effect.scaleX + (effect.endX - effect.scaleX) * t;
        scaleY = effect.scaleY + (effect.endY - effect.scaleY) * t;
        // The stackable move folds the node's zero drift into its start: start + (x - x).
        x = (effect.x + 0.f) + -20.f * t;
        y = (effect.y + 0.f) + 0.f * t;
        rootOpacity = static_cast<GLubyte>(255.f + (0.f - 255.f) * t);
        outlineOpacity = static_cast<GLubyte>(static_cast<int>(150.f + static_cast<float>(0 - 150) * t));
    }
    State state;
    state.root = nodeTransform(x, y, scaleX, scaleY, effect.cx, effect.sx, effect.cy, effect.sy, r.rootAnchor);
    state.outline = r.outlineLocal;
    state.rootColor = spriteColor(effect.color, rootOpacity, r.root.premultiplied);
    state.outlineColor = spriteColor({255, 255, 255}, outlineOpacity, r.outline.premultiplied);
    return state;
}

class DashEffectNode final : public CCNode {
public:
    std::vector<Effect> effects;
    std::size_t head = 0;
    std::deque<Cohort> cohorts;
    std::uint64_t firstCohort = 0;
    // Effects the tick removed while their native twins were being compared.
    std::vector<Effect> retired;

    static DashEffectNode* create();
    ~DashEffectNode() override {
        if (currentNode == this) currentNode = nullptr;
        for (std::size_t i = head; i < effects.size(); ++i) releaseNatives(effects[i]);
        for (auto& effect : retired) releaseNatives(effect);
    }
    static void releaseNatives(Effect& effect) {
        if (effect.nativeRoot) effect.nativeRoot->release();
        if (effect.nativeOutline) effect.nativeOutline->release();
        effect.nativeRoot = effect.nativeOutline = nullptr;
    }

    Effect& add() {
        if (cohorts.empty() || cohorts.back().ticked) cohorts.emplace_back();
        auto& effect = effects.emplace_back();
        effect.cohort = firstCohort + cohorts.size() - 1;
        return effect;
    }

    // CCActionInterval::step for every effect: the first tick starts at zero and ignores dt.
    void tick(float dt) {
        for (auto& cohort : cohorts) {
            if (!cohort.ticked) {
                cohort.ticked = true;
                cohort.elapsed = 0.f;
            } else {
                cohort.elapsed = dt + cohort.elapsed;
            }
            float t = cohort.elapsed / std::max(0.2f, FLT_EPSILON);
            cohort.time = t > 1.f ? 1.f : (0.f > t ? 0.f : t);
        }
        // Older cohorts have seen every dt the newer ones have, so they finish first.
        while (!cohorts.empty() && cohorts.front().ticked && cohorts.front().time >= 1.f) {
            while (head < effects.size() && effects[head].cohort == firstCohort) {
                if (effects[head].nativeRoot) retired.push_back(effects[head]);
                ++head;
            }
            cohorts.pop_front();
            ++firstCohort;
        }
        if (head > 4096 && head * 2 > effects.size()) {
            pendingPair = SIZE_MAX;
            effects.erase(effects.begin(), effects.begin() + static_cast<std::ptrdiff_t>(head));
            head = 0;
        }
    }

    Cohort const& cohortOf(Effect const& effect) const {
        return cohorts[static_cast<std::size_t>(effect.cohort - firstCohort)];
    }

    void compare();
    void drawEffects();

    void visit() override {
        // No transform of its own: the effects use the object layer's matrix, as native children do.
        if (!m_bVisible) return;
        // CCNode::visit resets this after every visit; equal-z siblings sort on it.
        setOrderOfArrival(0);
        if (echoDashCompare) compare();
        if (echoDashCompare && !echoDashShowGpu) return;
        drawEffects();
    }
};

class TickAction final : public CCAction {
public:
    bool isDone() override { return false; }
    void step(float dt) override {
        if (auto* node = static_cast<DashEffectNode*>(m_pTarget)) node->tick(dt);
    }
    void update(float) override {}
};

DashEffectNode* DashEffectNode::create() {
    auto* node = new DashEffectNode();
    if (!node->init()) {
        delete node;
        return nullptr;
    }
    node->autorelease();
    auto* action = new TickAction();
    action->autorelease();
    node->runAction(action);
    return node;
}

bool same(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
bool same(Affine const& a, CCAffineTransform const& b) {
    return same(a.a, b.a) && same(a.b, b.b) && same(a.c, b.c) && same(a.d, b.d) && same(a.tx, b.tx) && same(a.ty, b.ty);
}
bool same(ccColor4B a, ccColor4B b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

void DashEffectNode::compare() {
    for (auto& effect : retired) {
        // The native root must have been removed in the same tick.
        if (effect.nativeRoot->getParent()) ++echoDashLifeMismatches;
        releaseNatives(effect);
    }
    retired.clear();
    static int logged = 0;
    for (std::size_t i = head; i < effects.size(); ++i) {
        auto& effect = effects[i];
        if (!effect.nativeRoot) continue;
        if (!effect.nativeRoot->getParent()) {
            ++echoDashLifeMismatches;
            continue;
        }
        effect.nativeRoot->setVisible(!echoDashShowGpu);
        auto state = stateAt(effect, cohortOf(effect));
        auto rootTransform = effect.nativeRoot->nodeToParentTransform();
        auto outlineTransform = effect.nativeOutline->nodeToParentTransform();
        auto rootColor = effect.nativeRoot->getQuad().bl.colors;
        auto outlineColor = effect.nativeOutline->getQuad().bl.colors;
        ++echoDashCompared;
        bool ok = same(state.root, rootTransform) && same(state.outline, outlineTransform) &&
                  same(state.rootColor, rootColor) && same(state.outlineColor, outlineColor);
        if (ok) continue;
        ++echoDashMismatches;
        if (logged++ < 8) {
            log::warn("Echo GPU dash effect differs: root {} {} {} {} {} {} vs {} {} {} {} {} {}; color {} {} {} {} vs {} {} {} {}; "
                      "outline {} {} vs {} {}; color {} vs {}",
                      state.root.a, state.root.b, state.root.c, state.root.d, state.root.tx, state.root.ty,
                      rootTransform.a, rootTransform.b, rootTransform.c, rootTransform.d, rootTransform.tx, rootTransform.ty,
                      state.rootColor.r, state.rootColor.g, state.rootColor.b, state.rootColor.a,
                      rootColor.r, rootColor.g, rootColor.b, rootColor.a,
                      state.outline.tx, state.outline.ty, outlineTransform.tx, outlineTransform.ty,
                      state.outlineColor.a, outlineColor.a);
        }
    }
}

void DashEffectNode::drawEffects() {
    auto& r = renderer;
    if (!r.ready || head >= effects.size()) return;
    kmMat4 projection, modelView;
    kmGLGetMatrix(KM_GL_PROJECTION, &projection);
    kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
    auto outlineMatrix = toMatrix(r.outlineLocal);
    r.instances.clear();
    r.instances.reserve(effects.size() - head);
    for (std::size_t i = head; i < effects.size(); ++i) {
        auto const& effect = effects[i];
        if (echoDashCompare && !effect.nativeRoot) continue;
        auto state = stateAt(effect, cohortOf(effect));
        // CCNode::transform for the root and its outline child, then setUniformsForBuiltins.
        kmMat4 rootLocal = toMatrix(state.root), rootView, outlineView;
        kmMat4Multiply(&rootView, &modelView, &rootLocal);
        kmMat4Multiply(&outlineView, &rootView, &outlineMatrix);
        Instance instance{};
        kmMat4 mvp;
        kmMat4Multiply(&mvp, &projection, &rootView);
        std::memcpy(instance.root, mvp.mat, sizeof(instance.root));
        kmMat4Multiply(&mvp, &projection, &outlineView);
        std::memcpy(instance.outline, mvp.mat, sizeof(instance.outline));
        instance.rootColor = state.rootColor;
        instance.outlineColor = state.outlineColor;
        // A closed-form blend rounds only once and can clamp at a different point from the
        // native root/outline sequence. Keep the original sequence even for shared records.
        r.instances.insert(r.instances.end(), static_cast<std::size_t>(effect.count), instance);
    }
    if (r.instances.empty()) return;

    r.program->use();
    std::array<GLfloat, 24> positions{};
    std::array<GLfloat, 16> uvs{};
    for (int corner = 0; corner < 4; ++corner) {
        auto const& rp = r.root.position[corner];
        auto const& op = r.outline.position[corner];
        positions[corner * 3 + 0] = rp.x; positions[corner * 3 + 1] = rp.y; positions[corner * 3 + 2] = rp.z;
        positions[12 + corner * 3 + 0] = op.x; positions[12 + corner * 3 + 1] = op.y; positions[12 + corner * 3 + 2] = op.z;
        uvs[corner * 2 + 0] = r.root.uv[corner].u; uvs[corner * 2 + 1] = r.root.uv[corner].v;
        uvs[8 + corner * 2 + 0] = r.outline.uv[corner].u; uvs[8 + corner * 2 + 1] = r.outline.uv[corner].v;
    }
    glUniform3fv(r.quadPositions, 8, positions.data());
    glUniform2fv(r.quadUVs, 8, uvs.data());
    GLfloat modes[4] = {r.root.sourceAlpha ? 1.f : 0.f, r.root.destinationOne ? 1.f : 0.f,
                        r.outline.sourceAlpha ? 1.f : 0.f, r.outline.destinationOne ? 1.f : 0.f};
    glUniform4fv(r.blendModes, 1, modes);
    glUniform1i(r.samplerRoot, 0);
    glUniform1i(r.samplerOutline, 1);
    // Through Cocos' texture cache, which does not track the active unit; leave unit 0 active.
    ccGLBindTexture2DN(1, r.outline.texture);
    ccGLBindTexture2DN(0, r.root.texture);
    glActiveTexture(GL_TEXTURE0);
    // Each primitive keeps its own native blend through the second fragment output, so additive
    // roots and alpha outlines stay interleaved in their original order in one draw.
    ccGLBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA);
    glBlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA, GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);
    // A vertex array of our own. Cocos draws with client-side arrays on whatever array is bound
    // (ccGLBindVAO is a no-op in this build), so it must never see our attributes or divisors.
    GLint previousArray = 0, previousBuffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
    glBindVertexArray(r.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r.buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(r.instances.size() * sizeof(Instance)), r.instances.data(), GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 12, static_cast<GLsizei>(r.instances.size()));
    CC_INCREMENT_GL_DRAWS(1);
    glBindVertexArray(static_cast<GLuint>(previousArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
    // Match the real blend state to Cocos' cached two-factor value.
    glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA);
}

constexpr auto vertexSource = R"GLSL(#version 130
in vec4 i_root0; in vec4 i_root1; in vec4 i_root2; in vec4 i_root3;
in vec4 i_outline0; in vec4 i_outline1; in vec4 i_outline2; in vec4 i_outline3;
in vec4 i_rootColor; in vec4 i_outlineColor;
uniform vec3 u_position[8];
uniform vec2 u_uv[8];
out vec4 v_color;
out vec2 v_uv;
flat out int v_quad;
void main() {
    // Two triangles per quad from the native strip bl, br, tl, tr.
    const int corners[6] = int[6](0, 1, 2, 2, 1, 3);
    int quad = gl_VertexID / 6;
    int corner = corners[gl_VertexID - quad * 6];
    int slot = quad * 4 + corner;
    vec4 position = vec4(u_position[slot], 1.0);
    if (quad == 0) {
        gl_Position = mat4(i_root0, i_root1, i_root2, i_root3) * position;
        v_color = i_rootColor;
    } else {
        gl_Position = mat4(i_outline0, i_outline1, i_outline2, i_outline3) * position;
        v_color = i_outlineColor;
    }
    v_uv = u_uv[slot];
    v_quad = quad;
}
)GLSL";

constexpr auto fragmentSource = R"GLSL(#version 130
uniform sampler2D u_root;
uniform sampler2D u_outline;
uniform vec4 u_modes;
in vec4 v_color;
in vec2 v_uv;
flat in int v_quad;
out vec4 echo_color;
out vec4 echo_secondary;
void main() {
    vec4 color = v_color * (v_quad == 0 ? texture(u_root, v_uv) : texture(u_outline, v_uv));
    vec2 mode = v_quad == 0 ? u_modes.xy : u_modes.zw;
    // Source factor SRC_ALPHA or ONE, destination ONE or ONE_MINUS_SRC_ALPHA, for RGB and alpha.
    float source = mode.x > 0.5 ? color.a : 1.0;
    echo_color = vec4(color.rgb, color.a * source);
    echo_secondary = vec4(vec3(source), mode.y > 0.5 ? 0.0 : color.a);
}
)GLSL";

bool loadTemplate(char const* frame, QuadTemplate& quad, CCPoint& anchor, CCSize* size, bool fixedAdditive) {
    auto* sprite = CCSprite::createWithSpriteFrameName(frame);
    if (!sprite || !sprite->getTexture()) return false;
    auto q = sprite->getQuad();
    quad.position = {q.bl.vertices, q.br.vertices, q.tl.vertices, q.tr.vertices};
    quad.uv = {q.bl.texCoords, q.br.texCoords, q.tl.texCoords, q.tr.texCoords};
    quad.texture = sprite->getTexture()->getName();
    quad.premultiplied = sprite->isOpacityModifyRGB();
    anchor = sprite->getAnchorPointInPoints();
    if (size) *size = sprite->getContentSize();
    auto blend = fixedAdditive ? ccBlendFunc{GL_SRC_ALPHA, GL_ONE} : sprite->getBlendFunc();
    if ((blend.src != GL_ONE && blend.src != GL_SRC_ALPHA) || (blend.dst != GL_ONE && blend.dst != GL_ONE_MINUS_SRC_ALPHA)) return false;
    quad.sourceAlpha = blend.src == GL_SRC_ALPHA;
    quad.destinationOne = blend.dst == GL_ONE;
    // The native sprite is drawn with the position-texture-color shader, no grid or camera.
    return sprite->getShaderProgram() == CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTextureColor);
}

bool initialize() {
    auto& r = renderer;
    using echo::circles::Signature;
    auto cocos = geode::base::getCocos();
    constexpr std::array<Signature, 6> native{{
        {0x37f0, 0x2fc, 0x384d3766b3d275f2},  // CCNode::nodeToParentTransform
        {0x52d0, 0x4f, 0x14c7b67ad2ecf28e},   // CCNode::transform
        {0x9d4f0, 0x43, 0x6845e7cc794568b5},  // CGAffineToGL
        {0x36870, 0x80, 0x3fd9c7f1c4f24e4a},  // CCActionInterval::step
        {0x36f20, 0x70, 0x299bd24e337466e1},  // CCFadeTo::update
        {0x8d450, 0xd6, 0x5446881e79dbc250},  // CCSprite::updateColor
    }};
    for (auto signature : native) {
        if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(cocos + signature.offset), signature.size) != signature.hash) {
            log::warn("Echo GPU dash effects: native sprite code differs at {:#x}; using sprites", signature.offset);
            return false;
        }
    }
    nativeCos = *reinterpret_cast<Trig*>(cocos + 0xe4ac8);
    nativeSin = *reinterpret_cast<Trig*>(cocos + 0xe4ad0);
    degreesToRadians = *reinterpret_cast<float const*>(cocos + 0xe61a4);
    if (!nativeCos || !nativeSin || degreesToRadians != 0.01745329238474369f) return false;
    if (!GLEW_ARB_blend_func_extended || !glBindFragDataLocationIndexed || !glVertexAttribDivisor ||
        !glDrawArraysInstanced || !glGenVertexArrays || !glBindVertexArray) {
        log::info("Echo GPU dash effects unavailable: needs instancing and dual-source blending");
        return false;
    }
    // A software renderer runs "GPU" work on the CPU, so it has nothing to hand the work to.
    if (auto const* name = reinterpret_cast<char const*>(glGetString(GL_RENDERER))) {
        std::string lower(name);
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (char const* soft : {"llvmpipe", "softpipe", "swiftshader", "software", "gdi generic", "basic render"}) {
            if (lower.find(soft) != std::string::npos) {
                log::info("Echo GPU dash effects off: {} is a software renderer", name);
                softwareRenderer = true;
            }
        }
    }
    GLint buffers = 0, attributes = 0;
    glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &buffers);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
    if (buffers < 1 || attributes < 10) return false;

    CCSize rootSize;
    if (!loadTemplate("playerDash2_001.png", r.root, r.rootAnchor, &rootSize, true) ||
        !loadTemplate("playerDash2_outline_001.png", r.outline, r.outlineAnchor, nullptr, false)) {
        log::warn("Echo GPU dash effects: effect sprites differ from the native setup; using sprites");
        return false;
    }
    // stopDashing places the outline at half the root's content size.
    r.outlineLocal = nodeTransform(rootSize.width * 0.5f, rootSize.height * 0.5f, 1.f, 1.f, 1.f, 0.f, 1.f, 0.f, r.outlineAnchor);

    auto compile = [](GLenum type, char const* source) {
        auto shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char message[2048]{};
            glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
            log::warn("Echo GPU dash effect shader failed: {}", message);
            glDeleteShader(shader);
            return GLuint{0};
        }
        return shader;
    };
    auto* program = new CCGLProgram;
    program->m_uProgram = glCreateProgram();
    program->m_uVertShader = compile(GL_VERTEX_SHADER, vertexSource);
    program->m_uFragShader = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!program->m_uVertShader || !program->m_uFragShader) { program->release(); return false; }
    glAttachShader(program->m_uProgram, program->m_uVertShader);
    glAttachShader(program->m_uProgram, program->m_uFragShader);
    char const* names[10] = {"i_root0", "i_root1", "i_root2", "i_root3", "i_outline0", "i_outline1",
                             "i_outline2", "i_outline3", "i_rootColor", "i_outlineColor"};
    for (GLuint i = 0; i < 10; ++i) program->addAttribute(names[i], i);
    glBindFragDataLocationIndexed(program->getProgram(), 0, 0, "echo_color");
    glBindFragDataLocationIndexed(program->getProgram(), 0, 1, "echo_secondary");
    if (!program->link()) { program->release(); return false; }
    r.quadPositions = glGetUniformLocation(program->getProgram(), "u_position");
    r.quadUVs = glGetUniformLocation(program->getProgram(), "u_uv");
    r.blendModes = glGetUniformLocation(program->getProgram(), "u_modes");
    r.samplerRoot = glGetUniformLocation(program->getProgram(), "u_root");
    r.samplerOutline = glGetUniformLocation(program->getProgram(), "u_outline");
    if (r.quadPositions < 0 || r.quadUVs < 0 || r.blendModes < 0 || r.samplerRoot < 0 || r.samplerOutline < 0) {
        program->release();
        return false;
    }
    r.program = program;

    GLint previousArray = 0, previousBuffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousArray);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousBuffer);
    glGenVertexArrays(1, &r.vao);
    glGenBuffers(1, &r.buffer);
    if (!r.vao || !r.buffer) return false;
    glBindVertexArray(r.vao);
    glBindBuffer(GL_ARRAY_BUFFER, r.buffer);
    auto offset = [](std::size_t bytes) { return reinterpret_cast<void const*>(bytes); };
    for (GLuint i = 0; i < 4; ++i) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), offset(offsetof(Instance, root) + i * 16));
        glVertexAttribDivisor(i, 1);
        glEnableVertexAttribArray(4 + i);
        glVertexAttribPointer(4 + i, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), offset(offsetof(Instance, outline) + i * 16));
        glVertexAttribDivisor(4 + i, 1);
    }
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Instance), offset(offsetof(Instance, rootColor)));
    glVertexAttribDivisor(8, 1);
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(9, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Instance), offset(offsetof(Instance, outlineColor)));
    glVertexAttribDivisor(9, 1);
    glBindVertexArray(static_cast<GLuint>(previousArray));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousBuffer));
    log::info("Echo GPU dash effects ready (one instanced draw; native transform, color and timing)");
    return true;
}

void record(PlayerObject* player) {
    auto* manager = GameManager::get();
    if (!manager || !player) return;
    // The same objects the native block reads: GameManager's game layer, its object layer,
    // and the player's sprite at +0x660.
    auto* layer = *reinterpret_cast<CCNode**>(reinterpret_cast<std::uint8_t*>(manager) + 0x208);
    if (!layer) return;
    auto* objectLayer = *reinterpret_cast<CCNode**>(reinterpret_cast<std::uint8_t*>(layer) + 0xfe8);
    auto* sprite = *reinterpret_cast<CCSprite**>(reinterpret_cast<std::uint8_t*>(player) + 0x660);
    if (!objectLayer || !sprite) return;
    if (!currentNode || currentNode->getParent() != objectLayer) {
        if (currentNode) currentNode->release();
        currentNode = DashEffectNode::create();
        if (!currentNode) return;
        currentNode->retain();
        objectLayer->addChild(currentNode, 0x28);
    }
    // A native effect sprite is added last among the layer's z 40 children, above any object a
    // section added before it. Level objects keep being added as sections come into view, so the
    // node moves to that same place whenever an effect is made; otherwise objects added after
    // the node drew over the whole trail and cut it into pieces.
    objectLayer->reorderChild(currentNode, 0x28);
    Effect effect;
    CCPoint position = player->getPosition() + sprite->getPosition();
    float size = *reinterpret_cast<float const*>(reinterpret_cast<std::uint8_t*>(player) + 0x9f0);
    effect.x = position.x;
    effect.y = position.y;
    effect.scaleX = sprite->getScaleX() * size;
    effect.scaleY = sprite->getScaleY() * size;
    effect.endX = effect.scaleX * 0.2f;
    effect.endY = effect.scaleY * 0.2f;
    effect.color = sprite->getColor();
    float rotation = static_cast<float>(-*reinterpret_cast<double const*>(reinterpret_cast<std::uint8_t*>(player) + 0x580));
    if (!(rotation == 0.f)) {
        // nodeToParentTransform: equal X and Y rotation share one cosf/sinf of -(degrees * k).
        float radians = -(rotation * degreesToRadians);
        effect.cx = effect.cy = nativeCos(radians);
        effect.sx = effect.sy = nativeSin(radians);
    } else {
        effect.cx = effect.cy = 1.f;
        effect.sx = effect.sy = 0.f;
    }
    // Share storage and CPU timeline/transform work only for bit-identical effects in the same
    // update. Even a sub-pixel displacement changes texture sampling and covered edge pixels.
    auto& node = *currentNode;
    if (merging && node.head < node.effects.size() && !node.cohorts.empty() && !node.cohorts.back().ticked) {
        auto& last = node.effects.back();
        bool sameCohort = last.cohort == node.firstCohort + node.cohorts.size() - 1;
        if (sameCohort && !last.nativeRoot && last.count < 65536.f &&
            same(last.scaleX, effect.scaleX) && same(last.scaleY, effect.scaleY) &&
            same(last.cx, effect.cx) && same(last.sx, effect.sx) &&
            same(last.cy, effect.cy) && same(last.sy, effect.sy) &&
            last.color.r == effect.color.r && last.color.g == effect.color.g && last.color.b == effect.color.b &&
            same(last.x, effect.x) && same(last.y, effect.y)) {
            last.count += 1.f;
            pendingPair = SIZE_MAX;
            return;
        }
    }
    auto& added = node.add();
    auto cohort = added.cohort;
    added = effect;
    added.cohort = cohort;
    pendingPair = currentNode->effects.size() - 1;
}
}

extern "C" void echoRecordDashEnd(PlayerObject* player) noexcept {
    try {
        record(player);
    } catch (...) {
    }
}

namespace echo::dash {
bool gpuEffectsReady() {
    auto& r = renderer;
    if (!r.tried) {
        r.tried = true;
        r.ready = initialize();
    }
    return r.ready;
}

bool mergeReady() {
    return gpuEffectsReady();
}

// Each sprite effect costs the main thread its creation, two actions and a visit and draw every
// frame; a GPU record costs a fraction of that, and the GPU draws them all in one call. So the
// GPU wins as soon as effects are frequent, and only an occasional effect is cheaper as a sprite
// (no extra draw call, no buffer upload). The rate is a decaying count of effects per second;
// switching back needs a much lower rate so a steady pattern does not flip back and forth.
bool preferGpu() {
    if (!gpuEffectsReady() || softwareRenderer) return false;
    using Clock = std::chrono::steady_clock;
    constexpr double kWindow = 0.5;     // seconds the rate remembers
    constexpr double kToGpu = 60.0;     // about one effect per frame at 60 FPS
    constexpr double kToSprites = 15.0;
    static Clock::time_point last{};
    static double rate = 0.0;
    static bool gpu = false;
    auto now = Clock::now();
    double since = last == Clock::time_point{} ? kWindow * 10.0 : std::chrono::duration<double>(now - last).count();
    last = now;
    rate = rate * std::exp(-since / kWindow) + 1.0 / kWindow;
    if (!gpu && rate >= kToGpu) gpu = true;
    else if (gpu && rate < kToSprites) gpu = false;
    return gpu;
}

void setMerge(bool value) {
    merging = value;
}

void resetPendingPair() {
    pendingPair = SIZE_MAX;
}

void pairNative(CCSprite* root, CCSprite* outline) {
    if (!currentNode || pendingPair >= currentNode->effects.size() || !root || !outline) return;
    auto& effect = currentNode->effects[pendingPair];
    pendingPair = SIZE_MAX;
    root->retain();
    outline->retain();
    effect.nativeRoot = root;
    effect.nativeOutline = outline;
}
}
#endif
