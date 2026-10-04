// the dash end effect w/out cocos actions. windows
// gd starts 6 actions on the 2 sprites of every effect. here an effect is 1 record & a single node works out what every
//   sprite looks like at the current time (state_at), then draws them all as quads
// effects that started on the same frame share a Cohort, so their state is only worked out once
// the transform & color math is cocos's own (node_transform, sprite_color) w/ gds sinf / cosf, so the pixels match
#include <Geode/Geode.hpp>
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

namespace chrono = std::chrono;

using namespace geode::prelude;

volatile bool echo_dash_compare = false, echo_dash_show_gpu = true;
volatile std::uint64_t echo_dash_compared = 0, echoDashMismatches = 0, echoDashLifeMismatches = 0;

namespace {
    using Trig = float (*)(float);
    bool software_renderer = false;
    Trig native_cos = nullptr, native_sin = nullptr;
    float degrees_to_radians = 0.f;

    struct Affine {
        float a, b, c, d, tx, ty;
    };

    struct QuadTemplate {
        std::array<ccVertex3F, 4> position{};
        std::array<ccTex2F, 4> uv{};
        GLuint texture = 0;
        bool premultiplied = false;
        bool source_alpha = false, destination_one = false;
    };

    Affine node_transform(float x, float y, float scaleX, float scaleY, float cx, float sx, float cy, float sy, CCPoint anchor) {
        if (!(anchor.x == 0.f && anchor.y == 0.f)) {
            float ax = anchor.x, ay = -anchor.y;
            float dx = (-ax * cy) * scaleX, dy = (ay * sx) * scaleY;
            float ex = (ax * sy) * scaleX;
            float ey = (ay * cx) * scaleY;
            x = x + (dx - dy);
            y = y + (ey - ex);
        }
        return { scaleX * cy, scaleX * sy, -sx * scaleY, scaleY * cx, x, y };
    }

    kmMat4 to_matrix(Affine const& t) {
        kmMat4 m{};
        m.mat[0] = t.a;
        m.mat[1] = t.b;
        m.mat[4] = t.c;
        m.mat[5] = t.d;
        m.mat[12] = t.tx;
        m.mat[13] = t.ty;
        m.mat[10] = 1.f;
        m.mat[15] = 1.f;
        return m;
    }

    ccColor4B sprite_color(ccColor3B color, GLubyte opacity, bool premultiplied) {
        ccColor4B out{ color.r, color.g, color.b, opacity };
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
        float x = 0.f, y = 0.f, scaleX = 0.f, scaleY = 0.f, end_x = 0.f, end_y = 0.f;
        float cx = 1.f, sx = 0.f, cy = 1.f, sy = 0.f;
        ccColor3B color{};
        float count = 1.f;
        CCSprite* native_root = nullptr;
        CCSprite* native_outline = nullptr;
    };

    struct State {
        Affine root, outline;
        ccColor4B root_color, outline_color;
    };

    struct Instance {
        float root[16];
        float outline[16];
        ccColor4B root_color, outline_color;
    };

    struct Renderer {
        bool tried = false, ready = false;
        QuadTemplate root, outline;
        CCPoint root_anchor, outline_anchor;
        Affine outline_local{};
        CCGLProgram* program = nullptr;
        GLuint vao = 0, buffer = 0;
        GLint quad_positions = -1, quad_uvs = -1, blend_modes = -1, sampler_root = -1, sampler_outline = -1;
        std::vector<Instance> instances;
    };

    Renderer renderer;

    class DashEffectNode;
    DashEffectNode* currentNode = nullptr;
    std::size_t pending_pair = SIZE_MAX;
    bool merging = false;

    State state_at(Effect const& effect, Cohort const& cohort) {
        auto& r = renderer;
        float scaleX = effect.scaleX, scaleY = effect.scaleY, x = effect.x, y = effect.y;
        GLubyte root_opacity = 255, outline_opacity = 150;
        if (cohort.ticked) {
            float t = cohort.time;
            scaleX = effect.scaleX + (effect.end_x - effect.scaleX) * t;
            scaleY = effect.scaleY + (effect.end_y - effect.scaleY) * t;
            x = (effect.x + 0.f) + -20.f * t;
            y = (effect.y + 0.f) + 0.f * t;
            root_opacity = static_cast<GLubyte>(255.f + (0.f - 255.f) * t);
            outline_opacity = static_cast<GLubyte>(static_cast<int>(150.f + static_cast<float>(0 - 150) * t));
        }
        State state;
        state.root = node_transform(x, y, scaleX, scaleY, effect.cx, effect.sx, effect.cy, effect.sy, r.root_anchor);
        state.outline = r.outline_local;
        state.root_color = sprite_color(effect.color, root_opacity, r.root.premultiplied);
        state.outline_color = sprite_color({ 255, 255, 255 }, outline_opacity, r.outline.premultiplied);
        return state;
    }

    class DashEffectNode final : public CCNode {
    public:
        std::vector<Effect> effects;
        std::size_t head = 0;
        std::deque<Cohort> cohorts;
        std::uint64_t first_cohort = 0;
        std::vector<Effect> retired;

        static DashEffectNode* create();

        ~DashEffectNode() override {
            if (currentNode == this)
                currentNode = nullptr;
            for (std::size_t i = head; i < effects.size(); ++i)
                release_natives(effects[i]);
            for (auto& effect : retired)
                release_natives(effect);
        }

        static void release_natives(Effect& effect) {
            if (effect.native_root)
                effect.native_root->release();
            if (effect.native_outline)
                effect.native_outline->release();
            effect.native_root = effect.native_outline = nullptr;
        }

        Effect& add() {
            if (cohorts.empty() || cohorts.back().ticked)
                cohorts.emplace_back();
            auto& effect = effects.emplace_back();
            effect.cohort = first_cohort + cohorts.size() - 1;
            return effect;
        }

        void tick(float dt) {
            for (auto& cohort : cohorts) {
                if (!cohort.ticked) {
                    cohort.ticked = true;
                    cohort.elapsed = 0.f;
                }
                else {
                    cohort.elapsed = dt + cohort.elapsed;
                }
                float t = cohort.elapsed / std::max(0.2f, FLT_EPSILON);
                cohort.time = t > 1.f ? 1.f : (0.f > t ? 0.f : t);
            }
            while (!cohorts.empty() && cohorts.front().ticked && cohorts.front().time >= 1.f) {
                while (head < effects.size() && effects[head].cohort == first_cohort) {
                    if (effects[head].native_root)
                        retired.push_back(effects[head]);
                    ++head;
                }
                cohorts.pop_front();
                ++first_cohort;
            }
            if (head > 4096 && head * 2 > effects.size()) {
                pending_pair = SIZE_MAX;
                effects.erase(effects.begin(), effects.begin() + static_cast<std::ptrdiff_t>(head));
                head = 0;
            }
        }

        Cohort const& cohort_of(Effect const& effect) const {
            return cohorts[static_cast<std::size_t>(effect.cohort - first_cohort)];
        }

        void compare();
        void draw_effects();

        void visit() override {
            if (!m_bVisible)
                return;
            setOrderOfArrival(0);
            if (echo_dash_compare)
                compare();
            if (echo_dash_compare && !echo_dash_show_gpu)
                return;
            draw_effects();
        }
    };

    class TickAction final : public CCAction {
    public:
        bool isDone() override {
            return false;
        }

        void step(float dt) override {
            if (auto* node = static_cast<DashEffectNode*>(m_pTarget))
                node->tick(dt);
        }

        void update(float) override {
        }
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

    bool same(float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    bool same(Affine const& a, CCAffineTransform const& b) {
        return same(a.a, b.a) && same(a.b, b.b) && same(a.c, b.c) && same(a.d, b.d) && same(a.tx, b.tx) && same(a.ty, b.ty);
    }

    bool same(ccColor4B a, ccColor4B b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    void DashEffectNode::compare() {
        for (auto& effect : retired) {
            if (effect.native_root->getParent())
                ++echoDashLifeMismatches;
            release_natives(effect);
        }
        retired.clear();
        for (std::size_t i = head; i < effects.size(); ++i) {
            auto& effect = effects[i];
            if (!effect.native_root)
                continue;
            if (!effect.native_root->getParent()) {
                ++echoDashLifeMismatches;
                continue;
            }
            effect.native_root->setVisible(!echo_dash_show_gpu);
            auto state = state_at(effect, cohort_of(effect));
            auto root_transform = effect.native_root->nodeToParentTransform();
            auto outline_transform = effect.native_outline->nodeToParentTransform();
            auto root_color = effect.native_root->getQuad().bl.colors;
            auto outline_color = effect.native_outline->getQuad().bl.colors;
            ++echo_dash_compared;
            bool ok = same(state.root, root_transform) && same(state.outline, outline_transform) &&
                      same(state.root_color, root_color) && same(state.outline_color, outline_color);
            if (ok)
                continue;
            ++echoDashMismatches;
        }
    }

    void DashEffectNode::draw_effects() {
        auto& r = renderer;
        if (!r.ready || head >= effects.size())
            return;
        kmMat4 projection, modelView;
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
        auto outline_matrix = to_matrix(r.outline_local);
        r.instances.clear();
        r.instances.reserve(effects.size() - head);
        for (std::size_t i = head; i < effects.size(); ++i) {
            auto const& effect = effects[i];
            if (echo_dash_compare && !effect.native_root)
                continue;
            auto state = state_at(effect, cohort_of(effect));
            kmMat4 root_local = to_matrix(state.root), root_view, outline_view;
            kmMat4Multiply(&root_view, &modelView, &root_local);
            kmMat4Multiply(&outline_view, &root_view, &outline_matrix);
            Instance instance{};
            kmMat4 mvp;
            kmMat4Multiply(&mvp, &projection, &root_view);
            std::memcpy(instance.root, mvp.mat, sizeof(instance.root));
            kmMat4Multiply(&mvp, &projection, &outline_view);
            std::memcpy(instance.outline, mvp.mat, sizeof(instance.outline));
            instance.root_color = state.root_color;
            instance.outline_color = state.outline_color;
            // DONT simplify ts the rounding changes
            r.instances.insert(r.instances.end(), static_cast<std::size_t>(effect.count), instance);
        }
        if (r.instances.empty())
            return;

        r.program->use();
        std::array<GLfloat, 24> positions{};
        std::array<GLfloat, 16> uvs{};
        for (int corner = 0; corner < 4; ++corner) {
            auto const& rp = r.root.position[corner];
            auto const& op = r.outline.position[corner];
            positions[corner * 3 + 0] = rp.x;
            positions[corner * 3 + 1] = rp.y;
            positions[corner * 3 + 2] = rp.z;
            positions[12 + corner * 3 + 0] = op.x;
            positions[12 + corner * 3 + 1] = op.y;
            positions[12 + corner * 3 + 2] = op.z;
            uvs[corner * 2 + 0] = r.root.uv[corner].u;
            uvs[corner * 2 + 1] = r.root.uv[corner].v;
            uvs[8 + corner * 2 + 0] = r.outline.uv[corner].u;
            uvs[8 + corner * 2 + 1] = r.outline.uv[corner].v;
        }
        glUniform3fv(r.quad_positions, 8, positions.data());
        glUniform2fv(r.quad_uvs, 8, uvs.data());
        GLfloat modes[4] = { r.root.source_alpha ? 1.f : 0.f, r.root.destination_one ? 1.f : 0.f,
            r.outline.source_alpha ? 1.f : 0.f, r.outline.destination_one ? 1.f : 0.f };
        glUniform4fv(r.blend_modes, 1, modes);
        glUniform1i(r.sampler_root, 0);
        glUniform1i(r.sampler_outline, 1);
        ccGLBindTexture2DN(1, r.outline.texture);
        ccGLBindTexture2DN(0, r.root.texture);
        glActiveTexture(GL_TEXTURE0);
        ccGLBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA);
        glBlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA, GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);
        // needs its own vao or cocos breaks
        GLint previous_array = 0, previous_buffer = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_array);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
        glBindVertexArray(r.vao);
        glBindBuffer(GL_ARRAY_BUFFER, r.buffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(r.instances.size() * sizeof(Instance)), r.instances.data(), GL_STREAM_DRAW);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 12, static_cast<GLsizei>(r.instances.size()));
        CC_INCREMENT_GL_DRAWS(1);
        glBindVertexArray(static_cast<GLuint>(previous_array));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous_buffer));
        glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA);
    }

    constexpr auto vertex_source = R"GLSL(#version 130
in vec4 i_root0; in vec4 i_root1; in vec4 i_root2; in vec4 i_root3;
in vec4 i_outline0; in vec4 i_outline1; in vec4 i_outline2; in vec4 i_outline3;
in vec4 i_rootColor; in vec4 i_outlineColor;
uniform vec3 u_position[8];
uniform vec2 u_uv[8];
out vec4 v_color;
out vec2 v_uv;
flat out int v_quad;
void main() {
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

    constexpr auto fragment_source = R"GLSL(#version 130
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
    float source = mode.x > 0.5 ? color.a : 1.0;
    echo_color = vec4(color.rgb, color.a * source);
    echo_secondary = vec4(vec3(source), mode.y > 0.5 ? 0.0 : color.a);
}
)GLSL";

    bool load_template(char const* frame, QuadTemplate& quad, CCPoint& anchor, CCSize* size, bool fixed_additive) {
        auto* sprite = CCSprite::createWithSpriteFrameName(frame);
        if (!sprite || !sprite->getTexture())
            return false;
        auto q = sprite->getQuad();
        quad.position = { q.bl.vertices, q.br.vertices, q.tl.vertices, q.tr.vertices };
        quad.uv = { q.bl.texCoords, q.br.texCoords, q.tl.texCoords, q.tr.texCoords };
        quad.texture = sprite->getTexture()->getName();
        quad.premultiplied = sprite->isOpacityModifyRGB();
        anchor = sprite->getAnchorPointInPoints();
        if (size)
            *size = sprite->getContentSize();
        auto blend = fixed_additive ? ccBlendFunc{ GL_SRC_ALPHA, GL_ONE } : sprite->getBlendFunc();
        if ((blend.src != GL_ONE && blend.src != GL_SRC_ALPHA) || (blend.dst != GL_ONE && blend.dst != GL_ONE_MINUS_SRC_ALPHA))
            return false;
        quad.source_alpha = blend.src == GL_SRC_ALPHA;
        quad.destination_one = blend.dst == GL_ONE;
        return sprite->getShaderProgram() == CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTextureColor);
    }

    bool initialize() {
        auto& r = renderer;
        using echo::circles::Signature;
        auto cocos = geode::base::getCocos();
        constexpr std::array<Signature, 6> native{ {
            // { where, size, hash } of gd code
            { 0x37f0, 0x2fc, 0x384d3766b3d275f2 }, // CCNode::nodeToParentTransform
            { 0x52d0, 0x4f, 0x14c7b67ad2ecf28e },  // CCNode::transform
            { 0x9d4f0, 0x43, 0x6845e7cc794568b5 }, // CGAffineToGL
            { 0x36870, 0x80, 0x3fd9c7f1c4f24e4a }, // CCActionInterval::step
            { 0x36f20, 0x70, 0x299bd24e337466e1 }, // CCFadeTo::update
            { 0x8d450, 0xd6, 0x5446881e79dbc250 }, // CCSprite::updateColor
        } };
        for (auto signature : native) {
            if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(cocos + signature.offset), signature.size) != signature.hash) {
                return false;
            }
        }
        native_cos = *reinterpret_cast<Trig*>(cocos + 0xe4ac8); // cosf
        native_sin = *reinterpret_cast<Trig*>(cocos + 0xe4ad0); // sinf
        degrees_to_radians = *reinterpret_cast<float const*>(cocos + 0xe61a4);
        if (!native_cos || !native_sin || degrees_to_radians != 0.01745329238474369f) // (pi / 180)
            return false;
        if (!GLEW_ARB_blend_func_extended || !glBindFragDataLocationIndexed || !glVertexAttribDivisor ||
            !glDrawArraysInstanced || !glGenVertexArrays || !glBindVertexArray) {
            return false;
        }
        if (auto const* name = reinterpret_cast<char const*>(glGetString(GL_RENDERER))) {
            std::string lower(name);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            for (char const* soft : { "llvmpipe", "softpipe", "swiftshader", "software", "gdi generic", "basic render" }) {
                if (lower.find(soft) != std::string::npos) {
                    software_renderer = true;
                }
            }
        }
        GLint buffers = 0, attributes = 0;
        glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &buffers);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
        if (buffers < 1 || attributes < 10)
            return false;

        CCSize root_size;
        if (!load_template("playerDash2_001.png", r.root, r.root_anchor, &root_size, true) ||
            !load_template("playerDash2_outline_001.png", r.outline, r.outline_anchor, nullptr, false)) {
            return false;
        }
        r.outline_local = node_transform(root_size.width * 0.5f, root_size.height * 0.5f, 1.f, 1.f, 1.f, 0.f, 1.f, 0.f, r.outline_anchor);

        auto compile = [](GLenum type, char const* source) {
            auto shader = glCreateShader(type);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            if (!compiled) {
                char message[2048]{};
                glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
                glDeleteShader(shader);
                return GLuint{ 0 };
            }
            return shader;
        };
        auto* program = new CCGLProgram;
        program->m_uProgram = glCreateProgram();
        program->m_uVertShader = compile(GL_VERTEX_SHADER, vertex_source);
        program->m_uFragShader = compile(GL_FRAGMENT_SHADER, fragment_source);
        if (!program->m_uVertShader || !program->m_uFragShader) {
            program->release();
            return false;
        }
        glAttachShader(program->m_uProgram, program->m_uVertShader);
        glAttachShader(program->m_uProgram, program->m_uFragShader);
        char const* names[10] = { "i_root0", "i_root1", "i_root2", "i_root3", "i_outline0", "i_outline1",
            "i_outline2", "i_outline3", "i_rootColor", "i_outlineColor" };
        for (GLuint i = 0; i < 10; ++i)
            program->addAttribute(names[i], i);
        glBindFragDataLocationIndexed(program->getProgram(), 0, 0, "echo_color");
        glBindFragDataLocationIndexed(program->getProgram(), 0, 1, "echo_secondary");
        if (!program->link()) {
            program->release();
            return false;
        }
        r.quad_positions = glGetUniformLocation(program->getProgram(), "u_position");
        r.quad_uvs = glGetUniformLocation(program->getProgram(), "u_uv");
        r.blend_modes = glGetUniformLocation(program->getProgram(), "u_modes");
        r.sampler_root = glGetUniformLocation(program->getProgram(), "u_root");
        r.sampler_outline = glGetUniformLocation(program->getProgram(), "u_outline");
        if (r.quad_positions < 0 || r.quad_uvs < 0 || r.blend_modes < 0 || r.sampler_root < 0 || r.sampler_outline < 0) {
            program->release();
            return false;
        }
        r.program = program;

        GLint previous_array = 0, previous_buffer = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_array);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
        glGenVertexArrays(1, &r.vao);
        glGenBuffers(1, &r.buffer);
        if (!r.vao || !r.buffer)
            return false;
        glBindVertexArray(r.vao);
        glBindBuffer(GL_ARRAY_BUFFER, r.buffer);
        auto offset = [](std::size_t bytes) {
            return reinterpret_cast<void const*>(bytes);
        };
        for (GLuint i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), offset(offsetof(Instance, root) + i * 16));
            glVertexAttribDivisor(i, 1);
            glEnableVertexAttribArray(4 + i);
            glVertexAttribPointer(4 + i, 4, GL_FLOAT, GL_FALSE, sizeof(Instance), offset(offsetof(Instance, outline) + i * 16));
            glVertexAttribDivisor(4 + i, 1);
        }
        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Instance), offset(offsetof(Instance, root_color)));
        glVertexAttribDivisor(8, 1);
        glEnableVertexAttribArray(9);
        glVertexAttribPointer(9, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Instance), offset(offsetof(Instance, outline_color)));
        glVertexAttribDivisor(9, 1);
        glBindVertexArray(static_cast<GLuint>(previous_array));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous_buffer));
        return true;
    }

    void record(PlayerObject* player) {
        auto* manager = GameManager::get();
        if (!manager || !player)
            return;
        auto* layer = *reinterpret_cast<CCNode**>(reinterpret_cast<std::uint8_t*>(manager) + 0x208); // GameManager -> game layer
        if (!layer)
            return;
        auto* object_layer = *reinterpret_cast<CCNode**>(reinterpret_cast<std::uint8_t*>(layer) + 0xfe8); // object layer
        auto* sprite = *reinterpret_cast<CCSprite**>(reinterpret_cast<std::uint8_t*>(player) + 0x660);    // the players sprite
        if (!object_layer || !sprite)
            return;
        if (!currentNode || currentNode->getParent() != object_layer) {
            if (currentNode)
                currentNode->release();
            currentNode = DashEffectNode::create();
            if (!currentNode)
                return;
            currentNode->retain();
            object_layer->addChild(currentNode, 0x28);
        }
        object_layer->reorderChild(currentNode, 0x28);
        Effect effect;
        CCPoint position = player->getPosition() + sprite->getPosition();
        float size = *reinterpret_cast<float const*>(reinterpret_cast<std::uint8_t*>(player) + 0x9f0); // tf
        effect.x = position.x;
        effect.y = position.y;
        effect.scaleX = sprite->getScaleX() * size;
        effect.scaleY = sprite->getScaleY() * size;
        effect.end_x = effect.scaleX * 0.2f;
        effect.end_y = effect.scaleY * 0.2f;
        effect.color = sprite->getColor();
        float rotation = static_cast<float>(-*reinterpret_cast<double const*>(reinterpret_cast<std::uint8_t*>(player) + 0x580)); // tf
        if (!(rotation == 0.f)) {
            float radians = -(rotation * degrees_to_radians);
            effect.cx = effect.cy = native_cos(radians);
            effect.sx = effect.sy = native_sin(radians);
        }
        else {
            effect.cx = effect.cy = 1.f;
            effect.sx = effect.sy = 0.f;
        }
        // only merge if theyre EXACTLY the same
        auto& node = *currentNode;
        if (merging && node.head < node.effects.size() && !node.cohorts.empty() && !node.cohorts.back().ticked) {
            auto& last = node.effects.back();
            bool same_cohort = last.cohort == node.first_cohort + node.cohorts.size() - 1;
            if (same_cohort && !last.native_root && last.count < 65536.f &&
                same(last.scaleX, effect.scaleX) && same(last.scaleY, effect.scaleY) &&
                same(last.cx, effect.cx) && same(last.sx, effect.sx) &&
                same(last.cy, effect.cy) && same(last.sy, effect.sy) &&
                last.color.r == effect.color.r && last.color.g == effect.color.g && last.color.b == effect.color.b &&
                same(last.x, effect.x) && same(last.y, effect.y)) {
                last.count += 1.f;
                pending_pair = SIZE_MAX;
                return;
            }
        }
        auto& added = node.add();
        auto cohort = added.cohort;
        added = effect;
        added.cohort = cohort;
        pending_pair = currentNode->effects.size() - 1;
    }
}

extern "C" void echo_record_dash_end(PlayerObject* player) noexcept {
    try {
        record(player);
    }
    catch (...) {
    }
}

namespace echo::dash {
    bool gpu_effects_ready() {
        auto& r = renderer;
        if (!r.tried) {
            r.tried = true;
            r.ready = initialize();
        }
        return r.ready;
    }

    bool merge_ready() {
        return gpu_effects_ready();
    }

    bool prefer_gpu() {
        if (!gpu_effects_ready() || software_renderer)
            return false;
        using Clock = chrono::steady_clock;
        constexpr double RATE_WINDOW = 0.5;
        constexpr double TO_GPU = 60.0;
        constexpr double TO_SPRITES = 15.0;
        static Clock::time_point last{};
        static double rate = 0.0;
        static bool gpu = false;
        auto now = Clock::now();
        double since = last == Clock::time_point{} ? RATE_WINDOW * 10.0 : chrono::duration<double>(now - last).count();
        last = now;
        rate = rate * std::exp(-since / RATE_WINDOW) + 1.0 / RATE_WINDOW;
        if (!gpu && rate >= TO_GPU)
            gpu = true;
        else if (gpu && rate < TO_SPRITES)
            gpu = false;
        return gpu;
    }

    void set_merge(bool value) {
        merging = value;
    }

    void reset_pending_pair() {
        pending_pair = SIZE_MAX;
    }

    void pair_native(CCSprite* root, CCSprite* outline) {
        if (!currentNode || pending_pair >= currentNode->effects.size() || !root || !outline)
            return;
        auto& effect = currentNode->effects[pending_pair];
        pending_pair = SIZE_MAX;
        root->retain();
        outline->retain();
        effect.native_root = root;
        effect.native_outline = outline;
    }
}
