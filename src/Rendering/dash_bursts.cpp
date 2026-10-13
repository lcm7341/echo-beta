#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/binding/GameManager.hpp>
#include <Geode/modify/CCSprite.hpp>
#include "dash_bursts.hpp"
#include "Hooks/opt.hpp"
#include "circle_signatures.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <initializer_list>
#include <bit>
#include <typeinfo>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {
    struct Fresh {
        CCSprite* sprite;
        int orb;
        float extra = 0.f;
    };

    std::vector<Fresh> fresh;

    struct BurstKey {
        CCNode* parent;
        int orb;
        std::uint32_t x, y, rotationX, rotationY;
        bool operator==(BurstKey const&) const = default;
    };

    struct BurstHash {
        std::size_t operator()(BurstKey const& key) const noexcept {
            std::uint64_t hash = 14695981039346656037ull;
            auto mix = [&](std::uint64_t value) {
                hash = (hash ^ value) * 1099511628211ull;
            };
            mix(reinterpret_cast<std::uintptr_t>(key.parent));
            mix(static_cast<std::uint32_t>(key.orb));
            mix(key.x);
            mix(key.y);
            mix(key.rotationX);
            mix(key.rotationY);
            return static_cast<std::size_t>(hash);
        }
    };

    std::unordered_map<BurstKey, std::size_t, BurstHash> fresh_index;

    BurstKey burst_key(CCNode* parent, int orb, CCPoint position, float rotationX, float rotationY) {
        return { parent, orb, std::bit_cast<std::uint32_t>(position.x),
            std::bit_cast<std::uint32_t>(position.y), std::bit_cast<std::uint32_t>(rotationX),
            std::bit_cast<std::uint32_t>(rotationY) };
    }

    std::unordered_map<CCSprite*, float> counts;
    CCGLProgram* stack_program = nullptr;
    GLint count_uniform = -1;
    bool program_tried = false;
    Patch* skip_patch = nullptr;

    bool same(float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    bool identical(Fresh const& a, Fresh const& b) {
        auto* x = a.sprite;
        auto* y = b.sprite;
        if (a.orb != b.orb || x->getParent() != y->getParent() || x->getZOrder() != y->getZOrder())
            return false;
        auto point_same = [](CCPoint p, CCPoint q) {
            return same(p.x, q.x) && same(p.y, q.y);
        };
        auto rect_same = [&](CCRect const& p, CCRect const& q) {
            return point_same(p.origin, q.origin) && same(p.size.width, q.size.width) && same(p.size.height, q.size.height);
        };
        auto bx = x->getBlendFunc(), by = y->getBlendFunc();
        auto cx = x->getColor(), cy = y->getColor();
        auto qx = x->getQuad(), qy = y->getQuad();
        return point_same(x->getPosition(), y->getPosition()) && point_same(x->getAnchorPoint(), y->getAnchorPoint()) &&
               same(x->getRotationX(), y->getRotationX()) && same(x->getRotationY(), y->getRotationY()) &&
               same(x->getScaleX(), y->getScaleX()) && same(x->getScaleY(), y->getScaleY()) &&
               same(x->getSkewX(), y->getSkewX()) && same(x->getSkewY(), y->getSkewY()) &&
               x->isVisible() == y->isVisible() && x->isFlipX() == y->isFlipX() && x->isFlipY() == y->isFlipY() &&
               x->getTexture() == y->getTexture() && rect_same(x->getTextureRect(), y->getTextureRect()) &&
               x->getOpacity() == y->getOpacity() && cx.r == cy.r && cx.g == cy.g && cx.b == cy.b &&
               bx.src == by.src && bx.dst == by.dst && x->getShaderProgram() == y->getShaderProgram() &&
               std::memcmp(&qx, &qy, sizeof(qx)) == 0 && x->getChildrenCount() == 0 && y->getChildrenCount() == 0;
    }

    std::uint64_t signature(Fresh const& item) {
        auto* s = item.sprite;
        std::uint64_t hash = 14695981039346656037ull;
        auto mix = [&](std::uint64_t value) {
            hash = (hash ^ value) * 1099511628211ull;
        };
        mix(static_cast<std::uint32_t>(item.orb));
        mix(reinterpret_cast<std::uintptr_t>(s->getParent()));
        mix(std::bit_cast<std::uint32_t>(s->getPositionX()));
        mix(std::bit_cast<std::uint32_t>(s->getPositionY()));
        mix(std::bit_cast<std::uint32_t>(s->getRotation()));
        mix(reinterpret_cast<std::uintptr_t>(s->getTexture()));
        mix(std::bit_cast<std::uint32_t>(s->getTextureRect().origin.x));
        mix(std::bit_cast<std::uint32_t>(s->getTextureRect().origin.y));
        return hash;
    }

    constexpr auto stack_vertex = R"(#version 120
uniform mat4 CC_MVPMatrix;
attribute vec4 a_position;
attribute vec2 a_texCoord;
attribute vec4 a_color;
varying vec4 v_fragmentColor;
varying vec2 v_texCoord;
void main() {
    gl_Position = CC_MVPMatrix * a_position;
    v_fragmentColor = a_color;
    v_texCoord = a_texCoord;
})";
    constexpr auto stack_fragment = R"(#version 120
uniform sampler2D CC_Texture0;
uniform float u_count;
varying vec4 v_fragmentColor;
varying vec2 v_texCoord;
void main() {
    vec4 color = v_fragmentColor * texture2D(CC_Texture0, v_texCoord);
    gl_FragColor = vec4(color.rgb * color.a, color.a * color.a) * u_count;
})";

    GLuint compile(GLenum type, char const* source) {
        auto shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char message[1024]{};
            glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    bool prepare_program() {
        if (program_tried)
            return stack_program != nullptr;
        program_tried = true;
        auto* program = new CCGLProgram();
        program->m_uProgram = glCreateProgram();
        program->m_uVertShader = compile(GL_VERTEX_SHADER, stack_vertex);
        program->m_uFragShader = compile(GL_FRAGMENT_SHADER, stack_fragment);
        if (!program->m_uVertShader || !program->m_uFragShader) {
            program->release();
            return false;
        }
        glAttachShader(program->m_uProgram, program->m_uVertShader);
        glAttachShader(program->m_uProgram, program->m_uFragShader);
        program->addAttribute(kCCAttributeNamePosition, kCCVertexAttrib_Position);
        program->addAttribute(kCCAttributeNameColor, kCCVertexAttrib_Color);
        program->addAttribute(kCCAttributeNameTexCoord, kCCVertexAttrib_TexCoords);
        if (!program->link()) {
            program->release();
            return false;
        }
        program->updateUniforms();
        count_uniform = glGetUniformLocation(program->getProgram(), "u_count");
        if (count_uniform < 0) {
            program->release();
            return false;
        }
        stack_program = program;
        return true;
    }

    void forget(CCSprite* sprite) {
        auto it = counts.find(sprite);
        if (it == counts.end())
            return;
        counts.erase(it);
        sprite->release();
    }
}

namespace echo::bursts {
    void prepare() {
        if (!skip_patch)
            return;
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get())
                continue;
            for (auto* hook : mod->getHooks()) {
                if (!hook->isEnabled() || hook->getAddress() != geode::base::get() + 0x395910)
                    continue; // startDashing
                if (auto res = skip_patch->disable(); res) {
                    skip_patch = nullptr;
                }
                return;
            }
        }
    }

    void note_start(CCNode* layer, unsigned count_before, int orb) {
        if (!layer)
            return;
        auto* children = layer->getChildren();
        if (!children || children->count() != count_before + 1)
            return;
        auto* last = children->lastObject();
        if (!last || typeid(*last) != typeid(CCSprite))
            return;
        auto* sprite = static_cast<CCSprite*>(last);
        if (sprite->getZOrder() != 0x28)
            return;
        auto blend = sprite->getBlendFunc();
        if (blend.src != GL_SRC_ALPHA || blend.dst != GL_ONE)
            return;
        if (CCDirector::sharedDirector()->getActionManager()->numberOfRunningActionsInTarget(sprite) != 3)
            return;
        sprite->retain();
        fresh.push_back({ sprite, orb, 0.f });
        fresh_index[burst_key(sprite->getParent(), orb, sprite->getPosition(),
            sprite->getRotationX(), sprite->getRotationY())] = fresh.size() - 1;
    }

    void merge_fresh() {
        if (!counts.empty()) {
            std::vector<CCSprite*> gone;
            for (auto const& [sprite, count] : counts)
                if (!sprite->getParent())
                    gone.push_back(sprite);
            for (auto* sprite : gone)
                forget(sprite);
        }
        if (fresh.empty())
            return;
        if (!prepare_program()) {
            for (auto const& item : fresh)
                item.sprite->release();
            fresh.clear();
            fresh_index.clear();
            return;
        }
        static std::unordered_map<std::uint64_t, std::vector<size_t>> keepers;
        keepers.clear();
        auto* manager = CCDirector::sharedDirector()->getActionManager();
        for (size_t i = 0; i < fresh.size(); ++i) {
            auto const& item = fresh[i];
            auto* sprite = item.sprite;
            if (!sprite->getParent() || manager->numberOfRunningActionsInTarget(sprite) != 3)
                continue;
            if (item.extra > 0.f) {
                auto [entry, inserted] = counts.try_emplace(sprite, 1.f);
                if (inserted)
                    sprite->retain();
                entry->second += item.extra;
            }
            auto& group = keepers[signature(item)];
            CCSprite* keeper = nullptr;
            for (auto idx : group) {
                if (identical(fresh[idx], item)) {
                    keeper = fresh[idx].sprite;
                    break;
                }
            }
            if (!keeper) {
                group.push_back(i);
                continue;
            }
            float add = 1.f;
            if (auto it = counts.find(sprite); it != counts.end()) {
                add = it->second;
                forget(sprite);
            }
            auto [entry, inserted] = counts.try_emplace(keeper, 1.f);
            if (inserted)
                keeper->retain();
            entry->second += add;
            sprite->removeMeAndCleanup();
        }
        for (auto const& item : fresh)
            item.sprite->release();
        fresh.clear();
        fresh_index.clear();
    }

    void note_removed(CCNode* child) {
        if (counts.empty() || !child)
            return;
        forget(static_cast<CCSprite*>(child));
    }
}

class $modify(EchoBurstDraw, CCSprite) {
    void draw() {
        if (counts.empty())
            return CCSprite::draw();
        auto it = counts.find(this);
        if (it == counts.end() || it->second <= 1.f || !stack_program)
            return CCSprite::draw();
        auto* program = getShaderProgram();
        auto blend = getBlendFunc();
        program->retain();
        setShaderProgram(stack_program);
        setBlendFunc({ GL_ONE, GL_ONE });
        stack_program->use();
        stack_program->setUniformLocationWith1f(count_uniform, it->second);
        CCSprite::draw();
        setBlendFunc(blend);
        setShaderProgram(program);
        program->release();
    }
};

namespace {
    // free page close enough for a jmp
    std::uint8_t* allocate_near(std::uintptr_t target) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        std::uintptr_t const step = info.dwAllocationGranularity;
        std::uintptr_t const reach = 0x7ff00000; // a jmp can only go ~2gb
        auto try_at = [](std::uintptr_t address) {
            return static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        };
        std::uintptr_t floor = target > reach ? target - reach : step;
        std::uintptr_t address = (target & ~(step - 1)) - step;
        while (address >= floor) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
                break;
            if (region.State == MEM_FREE)
                if (auto* page = try_at(address))
                    return page;
            std::uintptr_t next = region.State == MEM_FREE ? address : reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            if (next < floor + step)
                break;
            address = (next - 1) & ~(step - 1);
        }
        return nullptr;
    }
}

// startDashing builds the burst inline (a sprite, 9 frame names, an animation & 4 actions)
// spamming an orb built it again just for the merge to delete it, most of the cost of a spammed press
// so ask me first & a duplicate jumps past the block
extern "C" bool echo_burst_duplicate(PlayerObject* player, GameObject* orb, float rotation) noexcept {
    try {
        if (!echo::opt::effects)
            return false;
        if (!player || !orb || fresh.empty() || !stack_program)
            return false;
        auto* play = PlayLayer::get();
        if (!play || !play->m_objectLayer)
            return false;
        CCPoint position = orb->getRealPosition();
        float sprite_rotation = -rotation;
        int type = orb->m_objectID;
        auto key = burst_key(play->m_objectLayer, type, position, sprite_rotation, sprite_rotation);
        auto indexed = fresh_index.find(key);
        if (indexed == fresh_index.end())
            return false;
        auto matches = [&](Fresh const& item) {
            auto* sprite = item.sprite;
            return item.orb == type && sprite->getParent() == play->m_objectLayer &&
                   same(sprite->getPositionX(), position.x) && same(sprite->getPositionY(), position.y) &&
                   same(sprite->getRotationX(), sprite_rotation) && same(sprite->getRotationY(), sprite_rotation);
        };
        auto& candidate = fresh[indexed->second];
        if (matches(candidate)) {
            candidate.extra += 1.f;
            return true;
        }
        for (auto it = fresh.rbegin(); it != fresh.rend(); ++it) {
            if (!matches(*it))
                continue;
            it->extra += 1.f;
            return true;
        }
    }
    catch (...) {
    }
    return false;
}

$execute {
    auto module = geode::base::get();
    // hash = the press check, the burst block & what the patch replaces
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(module + 0x395e4f), 0x231) != 0x690edbac5dce0ad2ull) {
        return;
    }
    auto site = module + 0x395e6e; // in startDashing
    auto* cave = allocate_near(site);
    if (!cave)
        return;
    auto dist = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(cave)) - static_cast<std::int64_t>(site + 5);
    if (dist < INT32_MIN || dist > INT32_MAX) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    // reached by jmp so alignment & shadow space are fine. rdi = player, r14 = orb, xmm10 = angle
    // volatiles are dead, what i replaced ends in a call
    std::vector<std::uint8_t> code;
    auto bytes = [&](std::initializer_list<std::uint8_t> list) {
        code.insert(code.end(), list);
    };
    auto imm64 = [&](std::uint64_t value) {
        for (int i = 0; i < 8; ++i)
            code.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    bytes({ 0x48, 0x89, 0xf9 });       // mov rcx, rdi
    bytes({ 0x4c, 0x89, 0xf2 });       // mov rdx, r14
    bytes({ 0x41, 0x0f, 0x28, 0xd2 }); // movaps xmm2, xmm10
    bytes({ 0x48, 0xb8 });
    imm64(reinterpret_cast<std::uintptr_t>(&echo_burst_duplicate)); // mov rax, fn
    bytes({ 0xff, 0xd0 });                                          // call rax
    bytes({ 0x84, 0xc0 });                                          // test al, al
    bytes({ 0x75, 0x23 });                                          // jne skip (+35)
    bytes({ 0x48, 0xb9 });
    imm64(module + 0x5c7b08); // mov rcx, frame name
    bytes({ 0x48, 0xb8 });
    imm64(module + 0x516c90); // mov rax, &import
    bytes({ 0xff, 0x10 });    // call [rax]  (createWithSpriteFrameName)
    bytes({ 0x49, 0xbb });
    imm64(site + 13);            // mov r11, continue
    bytes({ 0x41, 0xff, 0xe3 }); // jmp r11
    bytes({ 0x49, 0xbb });
    imm64(module + 0x396079);    // skip: mov r11, after the block
    bytes({ 0x41, 0xff, 0xe3 }); // jmp r11
    std::memcpy(cave, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), cave, code.size());
    // jmp cave; nops
    std::vector<std::uint8_t> patch{ 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    auto rel = static_cast<std::int32_t>(dist);
    std::memcpy(patch.data() + 1, &rel, sizeof(rel));
    auto res = Mod::get()->patch(reinterpret_cast<void*>(site), patch);
    if (!res) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    skip_patch = res.unwrap();
}
#endif
