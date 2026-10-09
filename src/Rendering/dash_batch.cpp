#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCNode.hpp>
#include <Geode/modify/CCSprite.hpp>
#include "dash_batch.hpp"
#include <unordered_map>
#include <vector>
#include <cstddef>

using namespace geode::prelude;
// Diagnostic switch/counters permit a same-process comparison on a paused scene.
// The draw hooks below are only installed when this is set before load.
// Off by default: a frozen-frame image comparison against native drawing on Windows
// still showed pixel differences.
volatile bool echoDashBatchingEnabled = false;
volatile std::uint64_t echoDashRenderedSprites = 0;
volatile std::uint64_t echoDashDrawCalls = 0;

namespace {
struct Effect {
    CCSprite* outline;
    unsigned rootID, outlineID;
    std::uint64_t serial = 0;
    unsigned index = 0;
};
struct SpriteOwner { CCSprite* root; unsigned id; };
std::unordered_map<CCSprite*, Effect> effects;
std::unordered_map<CCSprite*, SpriteOwner> owners;
struct Vertex {
    ccV3F_C4B_T2F sprite;
    float matrix[16];
    float additive;
};
struct Renderer {
    CCGLProgram* program = nullptr;
    CCGLProgram* nativeProgram = nullptr;
    GLuint verticesBuffer = 0, indicesBuffer = 0;
    bool initialized = false, available = false;
    bool active = false, enabled = false;
    CCNode* layer = nullptr;
    std::uint64_t serial = 0;
    std::uint64_t frameSprites = 0, frameDraws = 0;
    GLuint texture = 0;
    std::vector<Vertex> vertices;
};
Renderer renderer;
constexpr unsigned maxSprites = 2048;

bool initialize() {
    auto& r = renderer;
    if (r.initialized) return r.available;
    r.initialized = true;
    if (!GLEW_ARB_blend_func_extended || !glBindFragDataLocationIndexed) {
        log::info("Echo dash draw batching unavailable: dual-source blending unsupported");
        return false;
    }
    GLint buffers = 0, attributes = 0;
    glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &buffers);
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
    if (buffers < 1 || attributes < 8) return false;
    // Each primitive retains its native MVP and RGBA/UV values. A secondary
    // fragment output selects the native destination factor per sprite, so
    // additive roots and alpha outlines remain interleaved in their old order.
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
    // Compile directly: Cocos prepends its built-in uniform declarations, which
    // would put the required GLSL version directive after non-preprocessor text.
    auto compile=[](GLenum type, char const* source) {
        auto shader=glCreateShader(type);
        glShaderSource(shader,1,&source,nullptr);
        glCompileShader(shader);
        GLint compiled=0;
        glGetShaderiv(shader,GL_COMPILE_STATUS,&compiled);
        if (!compiled) {
            char message[2048]{};
            glGetShaderInfoLog(shader,sizeof(message),nullptr,message);
            log::warn("Echo dash batch shader failed: {}",message);
            glDeleteShader(shader); return GLuint{0};
        }
        return shader;
    };
    program->m_uProgram=glCreateProgram();
    program->m_uVertShader=compile(GL_VERTEX_SHADER,vertex);
    program->m_uFragShader=compile(GL_FRAGMENT_SHADER,fragment);
    if (!program->m_uVertShader || !program->m_uFragShader) { program->release(); return false; }
    glAttachShader(program->m_uProgram,program->m_uVertShader);
    glAttachShader(program->m_uProgram,program->m_uFragShader);
    program->addAttribute("a_position", kCCVertexAttrib_Position);
    program->addAttribute("a_color", kCCVertexAttrib_Color);
    program->addAttribute("a_texCoord", kCCVertexAttrib_TexCoords);
    program->addAttribute("a_transform", 3);
    program->addAttribute("a_additive", 7);
    glBindFragDataLocationIndexed(program->getProgram(), 0, 0, "echo_color");
    glBindFragDataLocationIndexed(program->getProgram(), 0, 1, "echo_secondary");
    if (!program->link()) { program->release(); return false; }
    program->updateUniforms();
    glGenBuffers(1, &r.verticesBuffer);
    glGenBuffers(1, &r.indicesBuffer);
    std::vector<GLuint> indices;
    indices.reserve(maxSprites*6);
    for (unsigned i=0; i<maxSprites; ++i) {
        auto v=i*4;
        indices.insert(indices.end(),{v,v+1,v+2,v+2,v+1,v+3});
    }
    ccGLBindVAO(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r.indicesBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,indices.size()*sizeof(GLuint),indices.data(),GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    r.vertices.reserve(maxSprites*4);
    r.program = program;
    r.available = true;
    log::info("Echo ordered dash draw batching initialized (dual-source blending; sprites and actions preserved)");
    return true;
}

void flush() {
    auto& r=renderer;
    if (r.vertices.empty()) return;
    ccGLBindVAO(0);
    r.program->use();
    ccGLBlendFunc(GL_SRC1_COLOR,GL_ONE_MINUS_SRC1_ALPHA);
    // RGB multiplication stays in the blending hardware, as in the native
    // renderer. Only additive alpha's source product is computed in the shader.
    glBlendFuncSeparate(GL_SRC1_COLOR,GL_ONE_MINUS_SRC1_ALPHA,GL_ONE,GL_ONE_MINUS_SRC1_ALPHA);
    ccGLBindTexture2D(r.texture);
    ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
    glBindBuffer(GL_ARRAY_BUFFER,r.verticesBuffer);
    glBufferData(GL_ARRAY_BUFFER,r.vertices.size()*sizeof(Vertex),r.vertices.data(),GL_STREAM_DRAW);
    auto pointer=[](std::size_t offset){return reinterpret_cast<void const*>(offset);};
    glVertexAttribPointer(kCCVertexAttrib_Position,3,GL_FLOAT,GL_FALSE,sizeof(Vertex),pointer(offsetof(Vertex,sprite)+offsetof(ccV3F_C4B_T2F,vertices)));
    glVertexAttribPointer(kCCVertexAttrib_Color,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(Vertex),pointer(offsetof(Vertex,sprite)+offsetof(ccV3F_C4B_T2F,colors)));
    glVertexAttribPointer(kCCVertexAttrib_TexCoords,2,GL_FLOAT,GL_FALSE,sizeof(Vertex),pointer(offsetof(Vertex,sprite)+offsetof(ccV3F_C4B_T2F,texCoords)));
    for (GLuint i=0;i<4;++i) {
        glEnableVertexAttribArray(3+i);
        glVertexAttribPointer(3+i,4,GL_FLOAT,GL_FALSE,sizeof(Vertex),pointer(offsetof(Vertex,matrix)+i*4*sizeof(float)));
    }
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7,1,GL_FLOAT,GL_FALSE,sizeof(Vertex),pointer(offsetof(Vertex,additive)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,r.indicesBuffer);
    glDrawElements(GL_TRIANGLES,static_cast<GLsizei>(r.vertices.size()/4*6),GL_UNSIGNED_INT,nullptr);
    CC_INCREMENT_GL_DRAWS(1);
    r.frameDraws += 1;
    // Keep the actual state consistent with Cocos' two-factor blend cache.
    glBlendFunc(GL_SRC1_COLOR,GL_ONE_MINUS_SRC1_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER,0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
    for (GLuint i=3;i<8;++i) glDisableVertexAttribArray(i);
    r.vertices.clear();
}

bool valid(CCSprite* root, Effect const& effect) {
    if (root->m_uID!=effect.rootID || root->getParent()!=renderer.layer) return false;
    auto* children=root->getChildren();
    if (!children || children->count()!=1 || children->objectAtIndex(0)!=effect.outline) return false;
    auto* outline=effect.outline;
    if (outline->m_uID!=effect.outlineID) return false;
    for (auto* sprite:{root,outline}) {
        auto blend=sprite->getBlendFunc();
        if (sprite->m_pCamera || sprite->m_pGrid || sprite->m_pobBatchNode || sprite->m_pShaderProgram!=renderer.nativeProgram) return false;
        if (!((blend.src==GL_SRC_ALPHA && blend.dst==GL_ONE) || (blend.src==GL_ONE && blend.dst==GL_ONE_MINUS_SRC_ALPHA))) return false;
    }
    return true;
}

bool capture(CCSprite* sprite) {
    auto& r=renderer;
    auto found=owners.find(sprite);
    if (found==owners.end() || found->second.id!=sprite->m_uID) return false;
    auto effect=effects.find(found->second.root);
    if (effect==effects.end() || effect->second.serial!=r.serial) return false;
    auto blend=sprite->getBlendFunc();
    if (sprite->m_pCamera || sprite->m_pGrid || sprite->m_pobBatchNode || sprite->m_pShaderProgram!=r.nativeProgram ||
        !((blend.src==GL_SRC_ALPHA && blend.dst==GL_ONE) || (blend.src==GL_ONE && blend.dst==GL_ONE_MINUS_SRC_ALPHA))) return false;
    r.frameSprites += 1;
    if (!r.enabled || !sprite->m_pobTexture) return false;
    auto texture=sprite->m_pobTexture->getName();
    if ((!r.vertices.empty() && r.texture!=texture) || r.vertices.size()==maxSprites*4) flush();
    r.texture=texture;
    kmMat4 projection,modelView,mvp;
    kmGLGetMatrix(KM_GL_PROJECTION,&projection);
    kmGLGetMatrix(KM_GL_MODELVIEW,&modelView);
    kmMat4Multiply(&mvp,&projection,&modelView);
    auto quad=sprite->getQuad();
    float additive=blend.src==GL_SRC_ALPHA ? 1.f : 0.f;
    for (auto const& value:{quad.bl,quad.br,quad.tl,quad.tr}) {
        Vertex vertex{value,{},additive};
        std::memcpy(vertex.matrix,mvp.mat,sizeof(vertex.matrix));
        r.vertices.push_back(vertex);
    }
    return true;
}
}

namespace echo::dash {
void trackDraw(CCSprite* root,CCSprite* outline) {
    effects[root]={outline,root->m_uID,outline->m_uID};
    owners[root]={root,root->m_uID};
    owners[outline]={root,outline->m_uID};
}
void untrackDraw(CCSprite* root) {
    auto found=effects.find(root);
    if (found==effects.end()) return;
    auto* outline=found->second.outline;
    auto owner=owners.find(outline);
    if (owner!=owners.end() && owner->second.root==root) owners.erase(owner);
    owners.erase(root);
    effects.erase(found);
}
}

// Every node visit and sprite draw in the game passes through these two hooks, so they are
// installed only when batching is on at load. Echo is otherwise the only mod on CCNode::visit.
class $modify(EchoDashLayerVisit,CCNode) {
    static void onModify(auto& self) {
        if (auto hook=self.getHook("cocos2d::CCNode::visit")) hook.unwrap()->setAutoEnable(echoDashBatchingEnabled);
    }
    void visit() {
        auto& r=renderer;
        // Batching off: every node in the scene comes through here, so do no lookups.
        if (!r.active && !echoDashBatchingEnabled) { CCNode::visit(); return; }
        auto* play=r.active ? nullptr : PlayLayer::get();
        if (!r.active && play && static_cast<CCNode*>(this)==static_cast<CCNode*>(play->m_objectLayer) && !m_pCamera && !m_pGrid && initialize()) {
            r.layer=this; r.active=true; r.enabled=echoDashBatchingEnabled;
            r.nativeProgram=CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTextureColor);
            ++r.serial;
            r.frameSprites=0; r.frameDraws=0;
            sortAllChildren();
            auto* children=getChildren();
            if (children) for (unsigned i=0;i<children->count();++i) {
                auto* node=static_cast<CCSprite*>(children->objectAtIndex(i));
                auto found=effects.find(node);
                if (found!=effects.end() && valid(node,found->second)) {
                    found->second.serial=r.serial; found->second.index=i;
                }
            }
            CCNode::visit();
            flush();
            echoDashRenderedSprites=r.frameSprites; echoDashDrawCalls=r.frameDraws;
            r.active=false; r.layer=nullptr;
            return;
        }
        auto found=effects.find(reinterpret_cast<CCSprite*>(this));
        bool effect=r.active && found!=effects.end() && found->second.serial==r.serial;
        if (r.active && !effect && owners.find(reinterpret_cast<CCSprite*>(this))==owners.end()) flush();
        CCNode::visit();
        if (effect) {
            found=effects.find(reinterpret_cast<CCSprite*>(this));
            if (found==effects.end() || found->second.serial!=r.serial) { flush(); return; }
            // Finish each consecutive run before a different child can draw,
            // including nodes whose overridden visit bypasses CCNode::visit.
            auto* children=r.layer->getChildren();
            auto index=found->second.index;
            if (!children || index>=children->count() || children->objectAtIndex(index)!=this || index+1>=children->count()) {
                flush(); return;
            }
            auto next=effects.find(static_cast<CCSprite*>(children->objectAtIndex(index+1)));
            if (next==effects.end() || next->second.serial!=r.serial ||
                (getZOrder()<0 && static_cast<CCNode*>(children->objectAtIndex(index+1))->getZOrder()>=0)) flush();
        }
    }
};
class $modify(EchoDashSpriteDraw,CCSprite) {
    static void onModify(auto& self) {
        if (auto hook=self.getHook("cocos2d::CCSprite::draw")) hook.unwrap()->setAutoEnable(echoDashBatchingEnabled);
    }
    void draw() {
        if (renderer.active) {
            if (capture(this)) return;
            flush();
            renderer.frameDraws += 1;
        }
        CCSprite::draw();
    }
};
#endif
