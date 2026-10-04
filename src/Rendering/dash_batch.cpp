// draws all the dash orb end effects in 1 go. windows
// each effect is 2 sprites that cocos visits & draws 1 by 1. w/ orb spam at high tps there are thousands alive at once
// the sprites still exist, but their visit() is skipped: capture() turns each into 4 vertices in a shared buffer
//   & flush() sends the whole buffer to the gpu w/ its own small shader
// a sprite the batch cant copy exactly (valid() says no) is drawn the normal way
#include <Geode/Geode.hpp>
#include <Geode/modify/CCNode.hpp>
#include <Geode/modify/CCSprite.hpp>
#include "dash_batch.hpp"
#include <unordered_map>
#include <vector>
#include <cstddef>

using namespace geode::prelude;
volatile bool echo_dash_batching_enabled = false;
volatile std::uint64_t echo_dash_rendered_sprites = 0, echo_dash_draw_calls = 0;

namespace {
    struct Effect {
        CCSprite* outline;
        unsigned root_id, outline_id;
        std::uint64_t serial = 0;
        unsigned index = 0;
    };

    struct SpriteOwner {
        CCSprite* root;
        unsigned id;
    };

    std::unordered_map<CCSprite*, Effect> effects;
    std::unordered_map<CCSprite*, SpriteOwner> owners;

    struct Vertex {
        ccV3F_C4B_T2F sprite;
        float matrix[16];
        float additive;
    };

    struct Renderer {
        CCGLProgram* program = nullptr;
        CCGLProgram* native_program = nullptr;
        GLuint vertices_buffer = 0, indices_buffer = 0;
        bool initialized = false, available = false;
        bool active = false, enabled = false;
        CCNode* layer = nullptr;
        std::uint64_t serial = 0;
        std::uint64_t frame_sprites = 0, frame_draws = 0;
        GLuint texture = 0;
        std::vector<Vertex> vertices;
    };

    Renderer renderer;
    constexpr unsigned max_sprites = 2048;

    bool initialize() {
        auto& r = renderer;
        if (r.initialized)
            return r.available;
        r.initialized = true;
        if (!GLEW_ARB_blend_func_extended || !glBindFragDataLocationIndexed) {
            return false;
        }
        GLint buffers = 0, attributes = 0;
        glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &buffers);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
        if (buffers < 1 || attributes < 8)
            return false;
        constexpr auto vertex = R"GLSL(#version 130
in vec4 a_position;
in vec4 a_color;
in vec2 a_texCoord;
in mat4 a_transform;
in float a_additive;
out vec4 v_fragmentColor;
out vec2 v_texCoord;
flat out float v_additive;
void main() {
    gl_Position = a_transform * a_position;
    v_fragmentColor = a_color;
    v_texCoord = a_texCoord;
    v_additive = a_additive;
}
)GLSL";
        constexpr auto fragment = R"GLSL(#version 130
uniform sampler2D CC_Texture0;
in vec4 v_fragmentColor;
in vec2 v_texCoord;
flat in float v_additive;
out vec4 echo_color;
out vec4 echo_secondary;
void main() {
    vec4 color = v_fragmentColor * texture2D(CC_Texture0, v_texCoord);
    if (v_additive > 0.5) {
        echo_color = vec4(color.rgb, color.a * color.a);
        echo_secondary = vec4(vec3(color.a), 0.0);
    } else {
        echo_color = color;
        echo_secondary = vec4(1.0, 1.0, 1.0, color.a);
    }
}
)GLSL";
        auto* program = new CCGLProgram;
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
        program->m_uProgram = glCreateProgram();
        program->m_uVertShader = compile(GL_VERTEX_SHADER, vertex);
        program->m_uFragShader = compile(GL_FRAGMENT_SHADER, fragment);
        if (!program->m_uVertShader || !program->m_uFragShader) {
            program->release();
            return false;
        }
        glAttachShader(program->m_uProgram, program->m_uVertShader);
        glAttachShader(program->m_uProgram, program->m_uFragShader);
        program->addAttribute("a_position", kCCVertexAttrib_Position);
        program->addAttribute("a_color", kCCVertexAttrib_Color);
        program->addAttribute("a_texCoord", kCCVertexAttrib_TexCoords);
        program->addAttribute("a_transform", 3);
        program->addAttribute("a_additive", 7);
        glBindFragDataLocationIndexed(program->getProgram(), 0, 0, "echo_color");
        glBindFragDataLocationIndexed(program->getProgram(), 0, 1, "echo_secondary");
        if (!program->link()) {
            program->release();
            return false;
        }
        program->updateUniforms();
        glGenBuffers(1, &r.vertices_buffer);
        glGenBuffers(1, &r.indices_buffer);
        std::vector<GLuint> indices;
        indices.reserve(max_sprites * 6);
        for (unsigned i = 0; i < max_sprites; ++i) {
            auto v = i * 4;
            indices.insert(indices.end(), { v, v + 1, v + 2, v + 2, v + 1, v + 3 });
        }
        ccGLBindVAO(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r.indices_buffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        r.vertices.reserve(max_sprites * 4);
        r.program = program;
        r.available = true;
        return true;
    }

    void flush() {
        auto& r = renderer;
        if (r.vertices.empty())
            return;
        ccGLBindVAO(0);
        r.program->use();
        ccGLBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA);
        glBlendFuncSeparate(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA, GL_ONE, GL_ONE_MINUS_SRC1_ALPHA);
        ccGLBindTexture2D(r.texture);
        ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
        glBindBuffer(GL_ARRAY_BUFFER, r.vertices_buffer);
        glBufferData(GL_ARRAY_BUFFER, r.vertices.size() * sizeof(Vertex), r.vertices.data(), GL_STREAM_DRAW);
        auto pointer = [](std::size_t offset) {
            return reinterpret_cast<void const*>(offset);
        };
        glVertexAttribPointer(kCCVertexAttrib_Position, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), pointer(offsetof(Vertex, sprite) + offsetof(ccV3F_C4B_T2F, vertices)));
        glVertexAttribPointer(kCCVertexAttrib_Color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), pointer(offsetof(Vertex, sprite) + offsetof(ccV3F_C4B_T2F, colors)));
        glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), pointer(offsetof(Vertex, sprite) + offsetof(ccV3F_C4B_T2F, texCoords)));
        for (GLuint i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(3 + i);
            glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), pointer(offsetof(Vertex, matrix) + i * 4 * sizeof(float)));
        }
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), pointer(offsetof(Vertex, additive)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r.indices_buffer);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(r.vertices.size() / 4 * 6), GL_UNSIGNED_INT, nullptr);
        CC_INCREMENT_GL_DRAWS(1);
        r.frame_draws += 1;
        glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_ALPHA);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        for (GLuint i = 3; i < 8; ++i)
            glDisableVertexAttribArray(i);
        r.vertices.clear();
    }

    bool valid(CCSprite* root, Effect const& effect) {
        if (root->m_uID != effect.root_id || root->getParent() != renderer.layer)
            return false;
        auto* children = root->getChildren();
        if (!children || children->count() != 1 || children->objectAtIndex(0) != effect.outline)
            return false;
        auto* outline = effect.outline;
        if (outline->m_uID != effect.outline_id)
            return false;
        for (auto* sprite : { root, outline }) {
            auto blend = sprite->getBlendFunc();
            if (sprite->m_pCamera || sprite->m_pGrid || sprite->m_pobBatchNode || sprite->m_pShaderProgram != renderer.native_program)
                return false;
            if (!((blend.src == GL_SRC_ALPHA && blend.dst == GL_ONE) || (blend.src == GL_ONE && blend.dst == GL_ONE_MINUS_SRC_ALPHA)))
                return false;
        }
        return true;
    }

    bool capture(CCSprite* sprite) {
        auto& r = renderer;
        auto it = owners.find(sprite);
        if (it == owners.end() || it->second.id != sprite->m_uID)
            return false;
        auto effect = effects.find(it->second.root);
        if (effect == effects.end() || effect->second.serial != r.serial)
            return false;
        auto blend = sprite->getBlendFunc();
        if (sprite->m_pCamera || sprite->m_pGrid || sprite->m_pobBatchNode || sprite->m_pShaderProgram != r.native_program ||
            !((blend.src == GL_SRC_ALPHA && blend.dst == GL_ONE) || (blend.src == GL_ONE && blend.dst == GL_ONE_MINUS_SRC_ALPHA)))
            return false;
        r.frame_sprites += 1;
        if (!r.enabled || !sprite->m_pobTexture)
            return false;
        auto texture = sprite->m_pobTexture->getName();
        if ((!r.vertices.empty() && r.texture != texture) || r.vertices.size() == max_sprites * 4)
            flush();
        r.texture = texture;
        kmMat4 projection, modelView, mvp;
        kmGLGetMatrix(KM_GL_PROJECTION, &projection);
        kmGLGetMatrix(KM_GL_MODELVIEW, &modelView);
        kmMat4Multiply(&mvp, &projection, &modelView);
        auto quad = sprite->getQuad();
        float additive = blend.src == GL_SRC_ALPHA ? 1.f : 0.f;
        for (auto const& value : { quad.bl, quad.br, quad.tl, quad.tr }) {
            Vertex vertex{ value, {}, additive };
            std::memcpy(vertex.matrix, mvp.mat, sizeof(vertex.matrix));
            r.vertices.push_back(vertex);
        }
        return true;
    }
}

namespace echo::dash {
    void track_draw(CCSprite* root, CCSprite* outline) {
        effects[root] = { outline, root->m_uID, outline->m_uID };
        owners[root] = { root, root->m_uID };
        owners[outline] = { root, outline->m_uID };
    }

    void untrack_draw(CCSprite* root) {
        auto it = effects.find(root);
        if (it == effects.end())
            return;
        auto* outline = it->second.outline;
        auto owner = owners.find(outline);
        if (owner != owners.end() && owner->second.root == root)
            owners.erase(owner);
        owners.erase(root);
        effects.erase(it);
    }
}

class $modify(EchoDashLayerVisit, CCNode) {
    static void onModify(auto& self) {
        if (auto hook = self.getHook("cocos2d::CCNode::visit"))
            hook.unwrap()->setAutoEnable(echo_dash_batching_enabled);
    }

};

class $modify(EchoDashSpriteDraw, CCSprite) {
    static void onModify(auto& self) {
        if (auto hook = self.getHook("cocos2d::CCSprite::draw"))
            hook.unwrap()->setAutoEnable(echo_dash_batching_enabled);
    }

    void draw() {
        if (renderer.active) {
            if (capture(this))
                return;
            flush();
            renderer.frame_draws += 1;
        }
        CCSprite::draw();
    }
};
