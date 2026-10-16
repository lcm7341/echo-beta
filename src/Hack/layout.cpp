// layout mode: a level stripped down to its gameplay
// hides decoration & triggers (the id lists are silicates), paints the color channels plain,
//   & w/ default art draws every block, slope, spike & saw as the standard one sized to its real hitbox
#include "Logic/logic.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJSpriteColor.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/ColorAction.hpp>
#include <Geode/binding/ColorActionSprite.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <algorithm>
#include <optional>
#include <map>
#include <chrono>
#include <array>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

class LevelLayout {
public:
    static LevelLayout& get() {
        static LevelLayout value;
        return value;
    }

    std::unordered_set<int> const& trigger_hide_ids();
    std::unordered_set<int> const& decoration_hide_ids();
    bool hide_as_trigger(GameObject* object);
    bool hide_as_decoration(GameObject* object);
    void remember_hidden(GameObject* object);
    void hide_object(GameObject* object);
    void style_solid_object(GameObject* object);
    void style_passable_glow(GameObject* object);
    ccColor3B to_color3b(float const* color);
};

// W silicate skid
std::unordered_set<int> const& LevelLayout::trigger_hide_ids() {
    static std::unordered_set<int> const ids{
        29,
        30,
        32,
        33,
        104,
        105,
        221,
        717,
        718,
        743,
        744,
        899,
        915,
        1006,
        1007,
        2903,
        2904,
        2905,
        2907,
        2909,
        2910,
        2911,
        2912,
        2913,
        2914,
        2915,
        2916,
        2917,
        2919,
        2920,
        2921,
        2922,
        2923,
        2924,
        3029,
        3030,
        3031,
        3606,
        3612,
    };
    return ids;
}

// thanks nat
std::unordered_set<int> const& LevelLayout::decoration_hide_ids() {
    static std::unordered_set<int> const ids{
        902,
        943,
        944,
        945,
        946,
        947,
        948,
        949,
        950,
        951,
        980,
        981,
        982,
        983,
        984,
        985,
        986,
        987,
        988,
        1023,
        1024,
        1025,
        1026,
        1027,
        1028,
        1029,
        1030,
        1031,
        1032,
        1063,
        1064,
        1065,
        1066,
        1067,
        1068,
        1069,
        1070,
        1071,
    };
    return ids;
}

bool LevelLayout::hide_as_trigger(GameObject* object) {
    return object && LevelLayout::get().trigger_hide_ids().contains(object->m_objectID);
}

bool LevelLayout::hide_as_decoration(GameObject* object) {
    if (!object) return false;
    if (object->m_objectType == GameObjectType::Decoration || object->m_isNoTouch) return true;
    int id = object->m_objectID;
    if (id >= 506 && id <= 640) return true;
    return LevelLayout::get().decoration_hide_ids().contains(id);
}

void LevelLayout::remember_hidden(GameObject* object) {
    auto& hidden = Logic::get().hidden_decorations;
    if (std::find(hidden.begin(), hidden.end(), object) == hidden.end()) hidden.push_back(object);
}

void LevelLayout::hide_object(GameObject* object) {
    object->m_isHide = true;
    LevelLayout::get().remember_hidden(object);
}

bool replaced_by_default_art(GameObject* object) {
    if (!object || !Logic::get().layout_default_art) return false;
    switch (object->m_objectType) {
    case GameObjectType::Solid:
    case GameObjectType::Slope:
        return true;
    // TODO cant figure out saws
    case GameObjectType::Hazard:
    case GameObjectType::AnimatedHazard:
        return object->m_objectRadius <= 0.f;
    default:
        return false;
    }
}

static std::unordered_set<GameObject*> g_hidden_by_level;

static std::unordered_map<GameObject*, std::pair<int, int>> g_color_channels;

static std::vector<float> g_channel_opacity;

bool invisible_by_color(GameObject* object) {
    auto it = g_color_channels.find(object);
    if (it == g_color_channels.end()) return false;
    auto const& opacity = g_channel_opacity;
    auto channel_invisible = [&](int channel) {
        return channel > 0 && (std::size_t)channel < opacity.size() && opacity[(std::size_t)channel] <= 0.001f;
    };
    auto [base, detail] = it->second;
    return channel_invisible(base) && (detail <= 0 || channel_invisible(detail));
}

static std::unordered_set<GameObject*> g_hidden_for_default_art;

void Logic::refresh_default_art() {
    for (auto* object : g_hidden_for_default_art) {
        if (!object) continue;
        object->m_isHide = false;
        object->setOpacity(255);
        if (object->m_colorSprite) object->m_colorSprite->setVisible(true);
    }
    g_hidden_for_default_art.clear();
    Logic::get().layout_styled = false;
}

void Logic::clear_layout_hidden() {
    g_hidden_by_level.clear();
    g_hidden_for_default_art.clear();
    g_color_channels.clear();
    g_channel_opacity.clear();
}

void LevelLayout::style_solid_object(GameObject* object) {
    if (object->m_isHide && !g_hidden_for_default_art.contains(object)) g_hidden_by_level.insert(object);
    if (!g_color_channels.contains(object)) {
        auto channel_of = [](GJSpriteColor* sprite) {
            return sprite ? (sprite->m_colorID ? sprite->m_colorID : sprite->m_defaultColorID) : 0;
        };
        g_color_channels[object] = { channel_of(object->m_baseColor), channel_of(object->m_detailColor) };
    }
    object->setOpacity(255);
    object->m_isHide = false;
    if (replaced_by_default_art(object)) {
        object->m_isHide = true;
        g_hidden_for_default_art.insert(object);
        LevelLayout::get().remember_hidden(object);
    }
    object->m_hasNoGlow = true;
    object->m_activeMainColorID = -1;
    object->m_activeDetailColorID = -1;
    object->m_baseUsesHSV = false;
    object->m_detailUsesHSV = false;
    object->m_isDontFade = true;
    object->m_isDontEnter = true;
    bool hazard = object->m_objectType == GameObjectType::Hazard || object->m_objectType == GameObjectType::AnimatedHazard;
    for (auto* sprite : { object->m_baseColor, object->m_detailColor }) {
        if (!sprite) continue;
        int channel = sprite->m_colorID ? sprite->m_colorID : sprite->m_defaultColorID;
        if (hazard) sprite->m_colorID = 1010;
        else if (channel == 1000 || channel == 1013 || channel == 1014) sprite->m_colorID = 1004;
    }
    auto color = hazard ? ccColor3B{ 0, 0, 0 } : LevelLayout::to_color3b(Logic::get().blocks_color);
    object->setColor(color);
    object->setChildColor(color);
}

void LevelLayout::style_passable_glow(GameObject* object) {
    object->m_isHide = false;
    object->m_isDontFade = true;
    object->m_isDontEnter = true;
    if (object->m_isPassable) {
        object->m_opacityMod = 0.5f;
        object->m_opacityMod2 = 0.5f;
    }
}

ccColor3B LevelLayout::to_color3b(float const* color) {
    return {
        (GLubyte)(std::clamp(color[0], 0.f, 1.f) * 255.f),
        (GLubyte)(std::clamp(color[1], 0.f, 1.f) * 255.f),
        (GLubyte)(std::clamp(color[2], 0.f, 1.f) * 255.f),
    };
}

bool Logic::layout_active() {
    return Logic::get().layout_mode && LevelEditorLayer::get() == nullptr;
}

void Logic::paint_layout_colors(PlayLayer* play) {
    if (!play || !play->m_effectManager || !Logic::get().layout_active()) return;

    auto* effects = play->m_effectManager;
    auto const& logic = Logic::get();
    const std::array<int, 5> ids{ 1000, 1001, 1009, 1013, 1014 };

    for (int id : ids) {
        auto* action = effects->getColorAction(id);
        if (!action) continue;
        if (id == 1000) action->m_color = LevelLayout::get().to_color3b(logic.background_color);
        else if (id == 1001 || id == 1009)
            action->m_color = LevelLayout::get().to_color3b(logic.blocks_color);
        else
            action->m_color = { 40, 125, 255 };
    }
}

void Logic::paint_layout_channels(GJEffectManager* effects) {
    if (!effects) return;
    auto const& logic = Logic::get();
    auto background = LevelLayout::get().to_color3b(logic.background_color);
    auto blocks = LevelLayout::get().to_color3b(logic.blocks_color);
    auto& sprites = effects->m_colorActionSpriteVector;
    auto& opacity = g_channel_opacity;
    if (opacity.empty()) {
        opacity.assign(sprites.size(), 1.f);
        for (std::size_t id = 1; id < sprites.size(); ++id)
            if (sprites[id]) opacity[id] = sprites[id]->m_opacity;
    }
    for (std::size_t id = 1; id < sprites.size(); ++id) {
        auto* sprite = sprites[id];
        if (!sprite) continue;
        switch (id) {
        case 1000:
            sprite->m_color = background;
            break;
        case 1002:
            sprite->m_color = { 255, 255, 255 };
            break;
        case 1010:
            sprite->m_color = { 0, 0, 0 };
            break;
        case 1013:
        case 1014:
            sprite->m_color = { 40, 125, 255 };
            break;
        default:
            sprite->m_color = blocks;
            break;
        }
        sprite->m_opacity = 1.f;
    }
}

void Logic::style_layout_object(GameObject* object) {
    if (!object || !Logic::get().layout_active()) return;
    if (LevelLayout::get().hide_as_trigger(object)) {
        LevelLayout::get().hide_object(object);
        return;
    }
    if (object->m_objectType == GameObjectType::UserCoin || object->m_objectType == GameObjectType::SecretCoin)
        return;
    LevelLayout::get().style_solid_object(object);
}

void Logic::style_layout_glow(GameObject* object) {
    if (!object || !Logic::get().layout_active()) return;
    if (LevelLayout::get().hide_as_decoration(object)) {
        LevelLayout::get().hide_object(object);
        return;
    }
    LevelLayout::get().style_passable_glow(object);
}

void Logic::apply_layout(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!play) return;
    if (!Logic::get().layout_active()) {
        if (!logic.layout_styled) return;
        for (auto* obj : logic.hidden_decorations) {
            if (!obj) continue;
            obj->m_isHide = false;
        }
        logic.hidden_decorations.clear();
        logic.layout_styled = false;
        return;
    }
    Logic::get().paint_layout_colors(play);
    if (logic.layout_styled || !play->m_objects) return;
    logic.layout_styled = true;
    for (auto* obj : CCArrayExt<GameObject*>(play->m_objects)) {
        Logic::get().style_layout_object(obj);
        Logic::get().style_layout_glow(obj);
    }
}

static const int LAYOUT_ART_TAG = 1162037324; // "ECHL"

struct DefaultArtPiece {
    CCSpriteFrame* frame = nullptr;
    float width = 0.f, height = 0.f, radius = 0.f;
    // saw is 2 halves bc yea
    bool two_halves = false;
    CCSpriteFrame* fill_frame = nullptr;
    int batch = 0;
};

struct DefaultArt {
    DefaultArtPiece block, spike, saw, slope, wide_slope;
    PlayLayer* measured_level = nullptr;
    bool loaded = false;
};

static DefaultArt& default_art(PlayLayer* play) {
    static DefaultArt art;
    if (!art.loaded) {
        art.loaded = true;
        auto frame_of = [](char const* name) -> CCSpriteFrame* {
            auto* frame = CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(name);
            if (frame) frame->retain();
            return frame;
        };
        art.block = { frame_of("square_01_001.png"), 30.f, 30.f, 0.f, false, nullptr, 0 };
        art.spike = { frame_of("spike_01_001.png"), 6.f, 12.f, 0.f, false, nullptr, 2 };
        art.saw = { frame_of("sawblade_01_001.png"), 0.f, 0.f, 32.f, true, nullptr, 2 };
        art.slope = { frame_of("blockOutline_14_001.png"), 30.f, 30.f, 0.f, false, frame_of("triangle_a_02_001.png"), 1 };
        art.wide_slope = { frame_of("blockOutline_15_001.png"), 60.f, 30.f, 0.f, false, frame_of("triangle_a_04_001.png"), 1 };
    }
    if (play && art.measured_level != play && play->m_objects) {
        art.measured_level = play;
        bool spike = false, saw = false;
        for (auto* obj : CCArrayExt<GameObject*>(play->m_objects)) {
            if (!obj || (spike && saw)) break;
            float sx = std::abs(obj->m_scaleX), sy = std::abs(obj->m_scaleY);
            if (sx <= 0.f || sy <= 0.f) continue;
            if (!spike && obj->m_objectID == 8 && obj->m_width > 0.f && obj->m_height > 0.f) {
                spike = true;
                art.spike.width = obj->m_width;
                art.spike.height = obj->m_height;
            }
            if (!saw && obj->m_objectID == 88 && obj->m_objectRadius > 0.f) {
                saw = true;
                art.saw.radius = obj->m_objectRadius;
            }
        }
    }
    return art;
}

struct DefaultArtNodes {
    std::array<CCNode*, 3> nodes{};
    PlayLayer* play = nullptr;
    std::array<std::vector<CCSprite*>, 3> sprite_pools;
    std::array<std::size_t, 3> sprites_used{};
};

static DefaultArtNodes g_default_art_nodes;

void Logic::draw_layout_art(PlayLayer* play) {
    auto& art_nodes = g_default_art_nodes;
    bool enabled = play && Logic::get().layout_active() && Logic::get().layout_default_art && Logic::get().layout_styled && play->m_objectLayer;
    if (art_nodes.play != play || !enabled) {
        for (auto* node : art_nodes.nodes) {
            if (node && art_nodes.play == play && node->getParent()) node->removeFromParent();
            if (node) node->release();
        }
        art_nodes = DefaultArtNodes{};
        if (!enabled) return;
    }
    auto& art = default_art(play);
    if (!art.block.frame || !art.spike.frame || !art.saw.frame || !art.slope.frame || !art.wide_slope.frame || !art.slope.fill_frame || !art.wide_slope.fill_frame)
        return;
    if (!art_nodes.nodes[0]) {
        art_nodes.play = play;
        auto* texture = art.block.frame->getTexture();
        bool one_texture = texture;
        for (auto* frame : { art.spike.frame, art.saw.frame, art.slope.frame, art.wide_slope.frame, art.slope.fill_frame, art.wide_slope.fill_frame })
            one_texture = one_texture && frame->getTexture() == texture;
        int z = play->m_player1 && play->m_player1->getParent() == play->m_objectLayer ? play->m_player1->getZOrder() - 1 : 0;
        for (auto& node : art_nodes.nodes) {
            node = one_texture ? static_cast<CCNode*>(CCSpriteBatchNode::createWithTexture(texture)) : CCNode::create();
            node->retain();
            node->setTag(LAYOUT_ART_TAG);
            play->m_objectLayer->addChild(node, z);
        }
    }
    art_nodes.sprites_used = {};
    auto win = CCDirector::sharedDirector()->getWinSize();
    auto* object_layer = play->m_objectLayer;
    CCPoint screen_bottom_left = object_layer->convertToNodeSpace({ 0.f, 0.f });
    CCPoint screen_top_right = object_layer->convertToNodeSpace({ win.width, win.height });
    float min_x = std::min(screen_bottom_left.x, screen_top_right.x) - 120.f, max_x = std::max(screen_bottom_left.x, screen_top_right.x) + 120.f;
    float min_y = std::min(screen_bottom_left.y, screen_top_right.y) - 120.f, max_y = std::max(screen_bottom_left.y, screen_top_right.y) + 120.f;
    auto next_sprite = [&](int batch, CCSpriteFrame* frame) -> CCSprite* {
        auto& pool = art_nodes.sprite_pools[batch];
        auto& used_count = art_nodes.sprites_used[batch];
        CCSprite* out = nullptr;
        if (used_count < pool.size()) {
            out = pool[used_count];
        }
        else {
            out = CCSprite::createWithSpriteFrame(frame);
            if (!out) return nullptr;
            art_nodes.nodes[batch]->addChild(out);
            pool.push_back(out);
        }
        ++used_count;
        if (!out->isFrameDisplayed(frame)) out->setDisplayFrame(frame);
        out->setVisible(true);
        return out;
    };
    auto draw_object_art = [&](GameObject* object) {
        if (!object || !replaced_by_default_art(object) || object->m_isDisabled || object->m_isGroupDisabled)
            return;
        if (object == play->m_anticheatSpike || object == play->m_player1CollisionBlock || object == play->m_player2CollisionBlock)
            return;
        if (object->m_objectType == GameObjectType::Solid && (object->m_isNoTouch || object->m_isPassable || g_hidden_by_level.contains(object) || invisible_by_color(object))) {
            object->setOpacity(0);
            return;
        }
        auto pos = object->getPosition();
        if (pos.x < min_x || pos.x > max_x || pos.y < min_y || pos.y > max_y) return;
        float object_scale_x = std::abs(object->m_scaleX), object_scale_y = std::abs(object->m_scaleY);
        bool hazard = object->m_objectType == GameObjectType::Hazard || object->m_objectType == GameObjectType::AnimatedHazard ||
                      (object->m_objectType == GameObjectType::Slope && object->m_slopeIsHazard);
        DefaultArtPiece const* piece = nullptr;
        float sprite_scale_x = object_scale_x, sprite_scale_y = object_scale_y;
        bool slope = object->m_objectType == GameObjectType::Slope;
        std::optional<CCRect> block_box;
        bool slope_flip_x = false, slope_flip_y = false;
        if (slope) {
            auto box = Logic::object_hitbox(object);
            if (box.size.width < 0.5f || box.size.height < 0.5f) {
                object->setOpacity(0);
                return;
            }
            pos = CCPoint{ box.getMidX(), box.getMidY() };
            float w = box.size.width, h = box.size.height;
            bool wide = w > h * 1.5f;
            piece = wide ? &art.wide_slope : &art.slope;
            sprite_scale_x = w / piece->width;
            sprite_scale_y = h / piece->height;
            switch (object->m_slopeDirection) {
            case 0:
            case 7:
                break;
            case 1:
            case 5:
                slope_flip_y = true;
                break;
            case 3:
            case 6:
                slope_flip_x = slope_flip_y = true;
                break;
            default:
                slope_flip_x = true;
                break;
            }
        }
        else if (hazard && object->m_objectRadius > 0.f) {
            piece = &art.saw;
            sprite_scale_x = sprite_scale_y = object->m_objectRadius * std::max(object_scale_x, object_scale_y) / piece->radius;
        }
        else if (hazard) {
            piece = &art.spike;
            if (object->m_width > 0.f) sprite_scale_x = object->m_width * object_scale_x / piece->width;
            if (object->m_height > 0.f) sprite_scale_y = object->m_height * object_scale_y / piece->height;
        }
        else {
            piece = &art.block;
            auto box = Logic::object_hitbox(object);
            if (box.size.width < 0.5f || box.size.height < 0.5f) {
                object->setOpacity(0);
                return;
            }
            block_box = box;
        }
        object->setOpacity(0);
        if (object->m_colorSprite) object->m_colorSprite->setVisible(false);
        if (object->m_glowSprite) object->m_glowSprite->setVisible(false);
        auto color = ccColor3B{ 255, 255, 255 };
        // saws r white!
        if (piece == &art.saw)
            color = { 0, 0, 0 };
        float rotation_x = object->getRotationX(), rotation_y = object->getRotationY();
        bool flip_x = object->isFlipX(), flip_y = object->isFlipY();
        if (slope) {
            rotation_x = rotation_y = 0.f;
            flip_x = slope_flip_x;
            flip_y = slope_flip_y;
        }
        if (block_box) {
            pos = CCPoint{ block_box->getMidX(), block_box->getMidY() };
            sprite_scale_x = block_box->size.width / piece->width;
            sprite_scale_y = block_box->size.height / piece->height;
            rotation_x = rotation_y = 0.f;
            flip_x = flip_y = false;
        }
        if (piece == &art.block && rotation_x == rotation_y) {
            float turns = rotation_x / 90.f;
            float nearest = std::round(turns);
            if (std::abs(turns - nearest) < 0.001f) {
                if ((long)nearest % 2 != 0) std::swap(sprite_scale_x, sprite_scale_y);
                rotation_x = rotation_y = 0.f;
                flip_x = flip_y = false;
            }
        }
        auto draw_sprite = [&](CCSpriteFrame* frame, float turn) {
            auto* out = next_sprite(piece->batch, frame);
            if (!out) return;
            out->setPosition(pos);
            out->setRotationX(rotation_x + turn);
            out->setRotationY(rotation_y + turn);
            out->setScaleX(sprite_scale_x);
            out->setScaleY(sprite_scale_y);
            out->setFlipX(flip_x);
            out->setFlipY(flip_y);
            out->setColor(color);
        };
        if (piece->fill_frame) {
            draw_sprite(piece->fill_frame, 0.f);
            art_nodes.sprite_pools[piece->batch][art_nodes.sprites_used[piece->batch] - 1]->setColor({ 0, 0, 0 });
        }
        draw_sprite(piece->frame, 0.f);
        if (piece->two_halves) draw_sprite(piece->frame, 180.f);
    };
    auto& sections = play->m_sections;
    auto& sizes = play->m_sectionSizes;
    float x_factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
    int first_x = std::max((int)(std::floor(min_x * x_factor)) - 1, 0);
    int last_x = std::min((int)(std::floor(max_x * x_factor)) + 1, (int)(std::min(sections.size(), sizes.size())) - 1);
    for (int x = first_x; x <= last_x; ++x) {
        auto* column = sections[x];
        auto* counts = sizes[x];
        if (!column || !counts) continue;
        int rows = (int)(std::min(column->size(), counts->size()));
        for (int y = 0; y < rows; ++y) {
            auto* bucket = (*column)[y];
            if (!bucket) continue;
            int object_count = std::min<int>((*counts)[y], (int)(bucket->size()));
            for (int i = 0; i < object_count; ++i)
                draw_object_art((*bucket)[i]);
        }
    }
    int moving = std::min<int>(play->m_calcNonEffectObjectsSize, (int)(play->m_calcNonEffectObjects.size()));
    for (int i = 0; i < moving; ++i)
        draw_object_art(play->m_calcNonEffectObjects[(std::size_t)i]);
    for (std::size_t batch = 0; batch < art_nodes.sprite_pools.size(); ++batch)
        for (std::size_t i = art_nodes.sprites_used[batch]; i < art_nodes.sprite_pools[batch].size(); ++i)
            art_nodes.sprite_pools[batch][i]->setVisible(false);
}
