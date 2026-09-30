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
    std::unordered_set<int> const& deco_hide_ids();
    bool hide_as_trigger(GameObject* object);
    bool hide_as_decoration(GameObject* object);
    void remember_hidden(GameObject* object);
    void hide_object(GameObject* object);
    void style_solid_object(GameObject* object);
    void style_passable_glow(GameObject* object);
    ccColor3B rgb(float const* color);
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
std::unordered_set<int> const& LevelLayout::deco_hide_ids() {
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
    if (!object) {
        return false;
    }
    if (object->m_objectType == GameObjectType::Decoration || object->m_isNoTouch) {
        return true;
    }
    int id = object->m_objectID;
    if (id >= 506 && id <= 640) {
        return true;
    }
    return LevelLayout::get().deco_hide_ids().contains(id);
}

void LevelLayout::remember_hidden(GameObject* object) {
    auto& hidden = Logic::get().hidden_decorations;
    if (std::find(hidden.begin(), hidden.end(), object) == hidden.end())
        hidden.push_back(object);
}

void LevelLayout::hide_object(GameObject* object) {
    object->m_isHide = true;
    LevelLayout::get().remember_hidden(object);
}

bool replaced_by_default_art(GameObject* object) {
    if (!object || !Logic::get().layout_default_art) {
        return false;
    }
    switch (object->m_objectType) {
    case GameObjectType::Solid:
    case GameObjectType::Slope:
        return true;
    // TODO cant figure out saws
    case GameObjectType::Hazard:
    case GameObjectType::AnimatedHazard:
        return object->m_objectRadius <= 0.f; // negative scale
    default:
        return false;
    }
}

std::unordered_set<GameObject*>& lvl_hidden() {
    static std::unordered_set<GameObject*> objects;
    return objects;
}

std::unordered_map<GameObject*, std::pair<int, int>>& lvl_channels() {
    static std::unordered_map<GameObject*, std::pair<int, int>> channels;
    return channels;
}

std::vector<float>& lvl_opacity() {
    static std::vector<float> opacity;
    return opacity;
}

bool lvl_invisible(GameObject* object) {
    auto it = lvl_channels().find(object);
    if (it == lvl_channels().end()) {
        return false;
    }
    auto const& opacity = lvl_opacity();
    auto clear = [&](int channel) {
        return channel > 0 && static_cast<std::size_t>(channel) < opacity.size() && opacity[static_cast<std::size_t>(channel)] <= 0.001f;
    };
    auto [base, detail] = it->second;
    return clear(base) && (detail <= 0 || clear(detail));
}

std::unordered_set<GameObject*>& art_hidden() {
    static std::unordered_set<GameObject*> objects;
    return objects;
}

void Logic::clear_layout_hidden() {
    lvl_hidden().clear();
    art_hidden().clear();
    lvl_channels().clear();
    lvl_opacity().clear();
}

void LevelLayout::style_solid_object(GameObject* object) {
    if (object->m_isHide && !art_hidden().contains(object)) {
        lvl_hidden().insert(object);
    }
    if (!lvl_channels().contains(object)) {
        auto channel_of = [](GJSpriteColor* sprite) {
            return sprite ? (sprite->m_colorID ? sprite->m_colorID : sprite->m_defaultColorID) : 0;
        };
        lvl_channels()[object] = { channel_of(object->m_baseColor), channel_of(object->m_detailColor) };
    }
    object->setOpacity(255);
    object->m_isHide = false;
    if (replaced_by_default_art(object)) {
        object->m_isHide = true;
        art_hidden().insert(object);
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
        if (!sprite) {
            continue;
        }
        int channel = sprite->m_colorID ? sprite->m_colorID : sprite->m_defaultColorID;
        if (hazard)
            sprite->m_colorID = 1010;
        else if (channel == 1000 || channel == 1013 || channel == 1014)
            sprite->m_colorID = 1004;
    }
    auto color = hazard ? ccColor3B{ 0, 0, 0 } : LevelLayout::rgb(Logic::get().blocks_color);
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

ccColor3B LevelLayout::rgb(float const* color) {
    return {
        static_cast<GLubyte>(std::clamp(color[0], 0.f, 1.f) * 255.f),
        static_cast<GLubyte>(std::clamp(color[1], 0.f, 1.f) * 255.f),
        static_cast<GLubyte>(std::clamp(color[2], 0.f, 1.f) * 255.f),
    };
}

bool Logic::layout_active() {
    return Logic::get().layout_mode && LevelEditorLayer::get() == nullptr;
}

void Logic::paint_layout_colors(PlayLayer* play) {
    if (!play || !play->m_effectManager || !Logic::get().layout_active())
        return;

    auto* effects = play->m_effectManager;
    auto const& logic = Logic::get();
    constexpr std::array<int, 5> ids{ 1000, 1001, 1009, 1013, 1014 };

    for (int id : ids) {
        auto* action = effects->getColorAction(id);
        if (!action) {
            continue;
        }
        if (id == 1000)
            action->m_color = LevelLayout::get().rgb(logic.backgroundColor);
        else if (id == 1001 || id == 1009)
            action->m_color = LevelLayout::get().rgb(logic.blocks_color);
        else
            action->m_color = { 40, 125, 255 };
    }
}

void Logic::paint_layout_channels(GJEffectManager* effects) {
    if (!effects)
        return;
    auto const& logic = Logic::get();
    auto background = LevelLayout::get().rgb(logic.backgroundColor);
    auto blocks = LevelLayout::get().rgb(logic.blocks_color);
    auto& sprites = effects->m_colorActionSpriteVector;
    auto& opacity = lvl_opacity();
    if (opacity.empty()) {
        opacity.assign(sprites.size(), 1.f);
        for (std::size_t id = 1; id < sprites.size(); ++id)
            if (sprites[id]) {
                opacity[id] = sprites[id]->m_opacity;
            }
    }
    for (std::size_t id = 1; id < sprites.size(); ++id) {
        auto* sprite = sprites[id];
        if (!sprite) {
            continue;
        }
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
    if (!object || !Logic::get().layout_active())
        return;
    if (LevelLayout::get().hide_as_trigger(object)) {
        LevelLayout::get().hide_object(object);
        return;
    }
    if (object->m_objectType == GameObjectType::UserCoin || object->m_objectType == GameObjectType::SecretCoin)
        return;
    LevelLayout::get().style_solid_object(object);
}

void Logic::style_layout_glow(GameObject* object) {
    if (!object || !Logic::get().layout_active())
        return;
    if (LevelLayout::get().hide_as_decoration(object)) {
        LevelLayout::get().hide_object(object);
        return;
    }
    LevelLayout::get().style_passable_glow(object);
}

void Logic::apply_layout(PlayLayer* play) {
    log::debug("apply layout");
    auto& logic = Logic::get();
    if (!play)
        return;
    if (!Logic::get().layout_active()) {
        if (!logic.layout_styled)
            return;
        for (auto* obj : logic.hidden_decorations) {
            if (!obj) {
                continue;
            }
            obj->m_isHide = false;
        }
        logic.hidden_decorations.clear();
        logic.layout_styled = false;
        return;
    }
    Logic::get().paint_layout_colors(play);
    if (logic.layout_styled || !play->m_objects)
        return;
    logic.layout_styled = true;
    for (auto* obj : CCArrayExt<GameObject*>(play->m_objects)) {
        Logic::get().style_layout_object(obj);
        Logic::get().style_layout_glow(obj);
    }
}

// default art
namespace {
    constexpr int LAYOUT_ART_TAG = 0x4543484c; // "ECHL"

    struct DefaultPiece {
        CCSpriteFrame* frame = nullptr;
        float width = 0.f, height = 0.f, radius = 0.f;
        // saw is 2 halves bc yea
        bool halves = false;
        CCSpriteFrame* fill = nullptr;
        int layer = 0;
    };

    struct DefaultArt {
        DefaultPiece block, spike, saw, slope, wide_slope;
        PlayLayer* measured = nullptr;
        bool loaded = false;
    };

    // createWithKey gives the wrong hitbox
    DefaultArt& default_art(PlayLayer* play) {
        static DefaultArt art;
        if (!art.loaded) {
            art.loaded = true;
            auto frame_of = [](char const* name) -> CCSpriteFrame* {
                auto* frame = CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(name);
                if (frame)
                    frame->retain();
                return frame;
            };
            art.block = { frame_of("square_01_001.png"), 30.f, 30.f, 0.f, false, nullptr, 0 };
            art.spike = { frame_of("spike_01_001.png"), 6.f, 12.f, 0.f, false, nullptr, 2 };
            art.saw = { frame_of("sawblade_01_001.png"), 0.f, 0.f, 32.f, true, nullptr, 2 };
            art.slope = { frame_of("blockOutline_14_001.png"), 30.f, 30.f, 0.f, false, frame_of("triangle_a_02_001.png"), 1 };
            art.wide_slope = { frame_of("blockOutline_15_001.png"), 60.f, 30.f, 0.f, false, frame_of("triangle_a_04_001.png"), 1 };
        }
        if (play && art.measured != play && play->m_objects) {
            art.measured = play;
            bool spike = false, saw = false;
            for (auto* obj : CCArrayExt<GameObject*>(play->m_objects)) {
                if (!obj || (spike && saw))
                    break;
                float sx = std::abs(obj->m_scaleX), sy = std::abs(obj->m_scaleY);
                if (sx <= 0.f || sy <= 0.f)
                    continue;
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

    struct ArtLayer {
        std::array<CCNode*, 3> nodes{};
        PlayLayer* play = nullptr;
        std::array<std::vector<CCSprite*>, 3> pools;
        std::array<std::size_t, 3> used{};
    };

    ArtLayer& art_layer() {
        static ArtLayer value;
        return value;
    }
}

void Logic::draw_layout_art(PlayLayer* play) {
    auto& layer = art_layer();
    bool on = play && Logic::get().layout_active() && Logic::get().layout_default_art && Logic::get().layout_styled && play->m_objectLayer;
    if (layer.play != play || !on) {
        for (auto* node : layer.nodes) {
            if (node && layer.play == play && node->getParent()) {
                node->removeFromParent();
            }
            if (node) {
                node->release();
            }
        }
        layer = ArtLayer{};
        if (!on)
            return;
    }
    auto& art = default_art(play);
    if (!art.block.frame || !art.spike.frame || !art.saw.frame || !art.slope.frame || !art.wide_slope.frame || !art.slope.fill || !art.wide_slope.fill)
        return;
    if (!layer.nodes[0]) {
        layer.play = play;
        auto* texture = art.block.frame->getTexture();
        bool shared = texture;
        for (auto* frame : { art.spike.frame, art.saw.frame, art.slope.frame, art.wide_slope.frame, art.slope.fill, art.wide_slope.fill })
            shared = shared && frame->getTexture() == texture;
        int z = play->m_player1 && play->m_player1->getParent() == play->m_objectLayer ? play->m_player1->getZOrder() - 1 : 0;
        for (auto& node : layer.nodes) {
            node = shared ? static_cast<CCNode*>(CCSpriteBatchNode::createWithTexture(texture)) : CCNode::create();
            node->retain();
            node->setTag(LAYOUT_ART_TAG);
            play->m_objectLayer->addChild(node, z);
        }
    }
    layer.used = {};
    auto win = CCDirector::sharedDirector()->getWinSize();
    auto* object_layer = play->m_objectLayer;
    CCPoint a = object_layer->convertToNodeSpace({ 0.f, 0.f });
    CCPoint b = object_layer->convertToNodeSpace({ win.width, win.height });
    float minX = std::min(a.x, b.x) - 120.f, maxX = std::max(a.x, b.x) + 120.f;
    float minY = std::min(a.y, b.y) - 120.f, maxY = std::max(a.y, b.y) + 120.f;
    auto block_color = LevelLayout::get().rgb(Logic::get().blocks_color);
    auto next = [&](int which, CCSpriteFrame* frame) -> CCSprite* {
        auto& pool = layer.pools[which];
        auto& used = layer.used[which];
        CCSprite* out = nullptr;
        if (used < pool.size()) {
            out = pool[used];
        }
        else {
            out = CCSprite::createWithSpriteFrame(frame);
            if (!out) {
                return nullptr;
            }
            layer.nodes[which]->addChild(out);
            pool.push_back(out);
        }
        ++used;
        if (!out->isFrameDisplayed(frame)) {
            out->setDisplayFrame(frame);
        }
        out->setVisible(true);
        return out;
    };
    auto place = [&](GameObject* object) {
        if (!object || !replaced_by_default_art(object) || object->m_isDisabled || object->m_isGroupDisabled)
            return;
        if (object == play->m_anticheatSpike || object == play->m_player1CollisionBlock || object == play->m_player2CollisionBlock)
            return;
        if (object->m_objectType == GameObjectType::Solid && (object->m_isNoTouch || object->m_isPassable || lvl_hidden().contains(object) || lvl_invisible(object))) {
            object->setOpacity(0);
            return;
        }
        auto pos = object->getPosition();
        if (pos.x < minX || pos.x > maxX || pos.y < minY || pos.y > maxY)
            return;
        float scaleX = std::abs(object->m_scaleX), scaleY = std::abs(object->m_scaleY);
        bool hazard = object->m_objectType == GameObjectType::Hazard || object->m_objectType == GameObjectType::AnimatedHazard ||
                      (object->m_objectType == GameObjectType::Slope && object->m_slopeIsHazard);
        DefaultPiece const* piece = nullptr;
        float sx = scaleX, sy = scaleY;
        bool slope = object->m_objectType == GameObjectType::Slope;
        std::optional<CCRect> block_box;
        bool slope_flip_x = false, slope_flip_y = false;
        if (slope) {
            float w = object->m_width * scaleX, h = object->m_height * scaleY;
            float turns = object->getRotation() / 90.f;
            if (std::abs(turns - std::round(turns)) < 0.001f && static_cast<long>(std::round(turns)) % 2 != 0) {
                std::swap(w, h);
            }
            bool wide = w > h * 1.5f;
            piece = wide ? &art.wide_slope : &art.slope;
            sx = w / piece->width;
            sy = h / piece->height;
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
            sx = sy = object->m_objectRadius * std::max(scaleX, scaleY) / piece->radius;
        }
        else if (hazard) {
            piece = &art.spike;
            if (object->m_width > 0.f) {
                sx = object->m_width * scaleX / piece->width;
            }
            if (object->m_height > 0.f) {
                sy = object->m_height * scaleY / piece->height;
            }
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
        if (object->m_colorSprite) {
            object->m_colorSprite->setVisible(false);
        }
        if (object->m_glowSprite) {
            object->m_glowSprite->setVisible(false);
        }
        auto color = ccColor3B{ 255, 255, 255 };
        if (piece == &art.saw) {
            color = { 0, 0, 0 };
        }
        (void)hazard;
        (void)block_color;
        float rotationX = object->getRotationX(), rotationY = object->getRotationY();
        bool flipX = object->isFlipX(), flipY = object->isFlipY();
        if (slope) {
            rotationX = rotationY = 0.f;
            flipX = slope_flip_x;
            flipY = slope_flip_y;
        }
        if (block_box) {
            pos = CCPoint{ block_box->getMidX(), block_box->getMidY() };
            sx = block_box->size.width / piece->width;
            sy = block_box->size.height / piece->height;
            rotationX = rotationY = 0.f;
            flipX = flipY = false;
        }
        if (piece == &art.block && rotationX == rotationY) {
            float turns = rotationX / 90.f;
            float nearest = std::round(turns);
            if (std::abs(turns - nearest) < 0.001f) {
                if (static_cast<long>(nearest) % 2 != 0) {
                    std::swap(sx, sy);
                }
                rotationX = rotationY = 0.f;
                flipX = flipY = false;
            }
        }
        auto draw = [&](CCSpriteFrame* frame, float turn) {
            auto* out = next(piece->layer, frame);
            if (!out)
                return;
            out->setPosition(pos);
            out->setRotationX(rotationX + turn);
            out->setRotationY(rotationY + turn);
            out->setScaleX(sx);
            out->setScaleY(sy);
            out->setFlipX(flipX);
            out->setFlipY(flipY);
            out->setColor(color);
        };
        if (piece->fill) {
            draw(piece->fill, 0.f);
            layer.pools[piece->layer][layer.used[piece->layer] - 1]->setColor({ 0, 0, 0 });
        }
        draw(piece->frame, 0.f);
        if (piece->halves) {
            draw(piece->frame, 180.f);
        }
    };
    auto& grid = play->m_nonEffectObjects;
    auto& sizes = play->m_nonEffectObjectsSizes;
    float xFactor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
    int first_x = std::max((int)(std::floor(minX * xFactor)) - 1, 0);
    int last_x = std::min((int)(std::floor(maxX * xFactor)) + 1, (int)(std::min(grid.size(), sizes.size())) - 1);
    for (int x = first_x; x <= last_x; ++x) {
        auto* column = grid[x];
        auto* counts = sizes[x];
        if (!column || !counts) {
            continue;
        }
        int rows = (int)(std::min(column->size(), counts->size()));
        for (int y = 0; y < rows; ++y) {
            auto* bucket = (*column)[y];
            if (!bucket) {
                continue;
            }
            int amt = std::min<int>((*counts)[y], (int)(bucket->size()));
            for (int i = 0; i < amt; ++i)
                place((*bucket)[i]);
        }
    }
    int moving = std::min<int>(play->m_calcNonEffectObjectsSize, (int)(play->m_calcNonEffectObjects.size()));
    for (int i = 0; i < moving; ++i)
        place(play->m_calcNonEffectObjects[static_cast<std::size_t>(i)]);
    for (std::size_t which = 0; which < layer.pools.size(); ++which)
        for (std::size_t i = layer.used[which]; i < layer.pools[which].size(); ++i)
            layer.pools[which][i]->setVisible(false);
}
