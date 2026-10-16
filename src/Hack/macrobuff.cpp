#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <array>
#include <cmath>
#include <map>
#include <vector>

using namespace geode::prelude;

OBB2D* live_oriented_box(GameObject* object);
bool rotation_is_right_angle(GameObject* object);

static const int RED_TINT_TAG = 60464;
static const int RECHECK_EVERY_FRAMES = 10;

using BoxCorners = std::array<CCPoint, 4>;

struct PlayerHitbox {
    CCRect outer, inner;
    BoxCorners rotated{};
    bool is_wave = false;
    bool dies_on_head_hit = false;
    bool is_upside_down = false;
};

struct MacrobuffPlacement {
    CCRect hitbox;
    float rotation = 0.f;
};

struct MacrobuffState {
    std::vector<PlayerHitbox> recording;
    std::vector<PlayerHitbox> path;
    std::map<GameObject*, MacrobuffPlacement> placed_during_playtest;
    CCRect path_bounds;
    LevelEditorLayer* editor = nullptr;
    bool player_died = false;
    bool tint_on = false;
    int frames_until_recheck = 0;
};

static MacrobuffState g_macrobuff;

static bool is_hazard(GameObject* object) {
    return object->m_objectType == GameObjectType::Hazard || object->m_objectType == GameObjectType::AnimatedHazard;
}

static bool is_solid_block(GameObject* object) {
    return object->m_objectType == GameObjectType::Solid && !object->m_isPassable;
}

static bool can_kill_player(GameObject* object) {
    if (!object || object->m_isDisabled || object->m_isGroupDisabled || object->m_isNoTouch)
        return false;
    return is_hazard(object) || is_solid_block(object);
}

static BoxCorners corners_of(CCRect const& rect) {
    return { rect.origin, ccp(rect.getMaxX(), rect.getMinY()), ccp(rect.getMaxX(), rect.getMaxY()), ccp(rect.getMinX(), rect.getMaxY()) };
}

static bool rotated_box_touches(BoxCorners const& box, BoxCorners const& player) {
    for (int side = 0; side < 4; ++side) {
        CCPoint edge = box[(side + 1) % 4] - box[side];
        CCPoint normal{ edge.y, -edge.x };
        float box_min = INFINITY, box_max = -INFINITY, player_min = INFINITY, player_max = -INFINITY;
        for (int corner = 0; corner < 4; ++corner) {
            float b = ccpDot(box[corner], normal);
            float p = ccpDot(player[corner], normal);
            box_min = std::min(box_min, b);
            box_max = std::max(box_max, b);
            player_min = std::min(player_min, p);
            player_max = std::max(player_max, p);
        }
        if (box_max < player_min || player_max < box_min) return false;
    }
    return true;
}

static bool circle_touches_rect(CCPoint center, float radius, CCRect const& rect) {
    float dx = center.x - std::clamp(center.x, rect.getMinX(), rect.getMaxX());
    float dy = center.y - std::clamp(center.y, rect.getMinY(), rect.getMaxY());
    return dx * dx + dy * dy <= radius * radius;
}

static bool is_rotated(GameObject* object) {
    return object->m_shouldUseOuterOb || !rotation_is_right_angle(object);
}

static float scaled_radius(GameObject* object) {
    return object->m_objectRadius * std::max(std::abs(object->m_scaleX), std::abs(object->m_scaleY));
}

static bool rotated_box_also_touches(GameObject* object, BoxCorners const& player) {
    if (!is_rotated(object)) return true;
    auto* box = live_oriented_box(object);
    if (!box) return true;
    return rotated_box_touches({ box->m_corners[0], box->m_corners[1], box->m_corners[2], box->m_corners[3] }, player);
}

static bool hazard_touches(GameObject* object, CCRect const& hitbox, PlayerHitbox const& player, bool fix_radius) {
    if (object->m_objectRadius > 0.f) {
        CCPoint center = object->getPosition();
        float radius = scaled_radius(object);
        bool touching = fix_radius ? ccpDistance(center, ccp(player.outer.getMidX(), player.outer.getMidY())) <= radius + std::abs(player.outer.size.width) * 0.5f
                                   : circle_touches_rect(center, radius, player.outer);
        if (!touching) return false;
        return fix_radius || rotated_box_also_touches(object, player.rotated);
    }
    return hitbox.intersectsRect(player.outer) && rotated_box_also_touches(object, player.rotated);
}

static bool block_kills(GameObject* object, CCRect const& hitbox, PlayerHitbox const& player) {
    if (hitbox.intersectsRect(player.inner))
        return rotated_box_also_touches(object, corners_of(player.inner));
    if (!player.dies_on_head_hit || !hitbox.intersectsRect(player.outer)) return false;
    bool above_head = player.is_upside_down ? hitbox.getMaxY() < player.outer.getMidY() : hitbox.getMinY() > player.outer.getMidY();
    return above_head && rotated_box_also_touches(object, player.rotated);
}

static bool would_kill_player(GameObject* object, CCRect const& hitbox, PlayerHitbox const& player, bool fix_radius) {
    if (is_hazard(object) || player.is_wave)
        return hazard_touches(object, hitbox, player, fix_radius);
    return block_kills(object, hitbox, player);
}

static bool kills_somewhere_on_path(GameObject* object, std::vector<PlayerHitbox> const& path, bool fix_radius) {
    CCRect hitbox = Logic::object_hitbox(object);
    float reach = object->m_objectRadius > 0.f ? scaled_radius(object) : 0.f;
    CCRect nearby(hitbox.getMinX() - reach, hitbox.getMinY() - reach, hitbox.size.width + reach * 2.f, hitbox.size.height + reach * 2.f);
    for (auto const& player : path) {
        if (nearby.intersectsRect(player.outer) && would_kill_player(object, hitbox, player, fix_radius))
            return true;
    }
    return false;
}

static bool moved_since_playtest(GameObject* object, MacrobuffState const& state) {
    auto before = state.placed_during_playtest.find(object);
    if (before == state.placed_during_playtest.end()) return true;
    CCRect now = Logic::object_hitbox(object);
    return !now.equals(before->second.hitbox) || object->getRotation() != before->second.rotation;
}

static void show_red_tint(bool on) {
    auto* scene = CCDirector::sharedDirector()->getRunningScene();
    auto* tint = scene ? scene->getChildByTag(RED_TINT_TAG) : nullptr;
    if (!on) {
        if (tint) tint->removeFromParent();
        return;
    }
    if (!scene || tint) return;
    auto win = CCDirector::sharedDirector()->getWinSize();
    auto* layer = CCLayerColor::create({ 255, 0, 0, 70 }, win.width, win.height);
    layer->setTag(RED_TINT_TAG);
    scene->addChild(layer, 1000);
}

void Logic::clear_macrobuff_samples() {
    auto& state = g_macrobuff;
    state.recording.clear();
    state.player_died = false;
}

void Logic::record_macrobuff_player_hitboxes(GJBaseGameLayer* layer, bool died) {
    auto& state = g_macrobuff;
    if (!layer) return;
    if (died) {
        state.player_died = true;
        return;
    }
    if (state.player_died) {
        state.recording.clear();
        state.player_died = false;
    }
    for (auto* player : { layer->m_player1, layer->m_player2 }) {
        if (!player || Logic::get().is_traj_player(player)) continue;
        if (player == layer->m_player2 && !layer->m_gameState.m_isDualMode) continue;
        PlayerHitbox hitbox;
        hitbox.outer = player->getObjectRect();
        hitbox.inner = player->getObjectRect(0.3f, 0.3f);
        hitbox.rotated = corners_of(hitbox.outer);
        if (auto* box = player->getOrientedBox())
            hitbox.rotated = { box->m_corners[0], box->m_corners[1], box->m_corners[2], box->m_corners[3] };
        hitbox.is_wave = player->m_isDart;
        hitbox.dies_on_head_hit = !player->m_isPlatformer && !player->m_isShip && !player->m_isBird && !player->m_isBall && !player->m_isDart && !player->m_isSpider && !player->m_isSwing;
        hitbox.is_upside_down = player->m_isUpsideDown;
        state.recording.push_back(hitbox);
    }
}

void Logic::capture_macrobuff_baseline(GJBaseGameLayer* layer) {
    auto& state = g_macrobuff;
    auto* editor = typeinfo_cast<LevelEditorLayer*>(layer);
    state.tint_on = false;
    state.placed_during_playtest.clear();
    state.path.clear();
    state.editor = nullptr;
    if (!editor || !editor->m_objects) return;
    for (auto* object : CCArrayExt<GameObject*>(editor->m_objects)) {
        if (can_kill_player(object))
            state.placed_during_playtest[object] = { Logic::object_hitbox(object), object->getRotation() };
    }
    state.path = state.recording;
    if (!state.path.empty()) {
        float left = INFINITY, bottom = INFINITY, right = -INFINITY, top = -INFINITY;
        for (auto const& player : state.path) {
            left = std::min(left, player.outer.getMinX());
            bottom = std::min(bottom, player.outer.getMinY());
            right = std::max(right, player.outer.getMaxX());
            top = std::max(top, player.outer.getMaxY());
        }
        state.path_bounds = CCRect(left, bottom, right - left, top - bottom);
    }
    state.editor = editor;
    state.frames_until_recheck = 0;
}

void Logic::update_macrobuff_tint(LevelEditorLayer* editor) {
    auto& state = g_macrobuff;
    auto& logic = Logic::get();
    bool wanted = logic.macrobuff_tint && editor && !logic.editor_testing;
    if (wanted && state.editor != editor && !state.recording.empty())
        Logic::get().capture_macrobuff_baseline(editor);
    if (!wanted || state.editor != editor || state.path.empty()) {
        state.tint_on = false;
        show_red_tint(false);
        return;
    }
    if (--state.frames_until_recheck > 0) {
        show_red_tint(state.tint_on);
        return;
    }
    state.frames_until_recheck = RECHECK_EVERY_FRAMES;
    bool fix_radius = editor->m_levelSettings && editor->m_levelSettings->m_fixRadiusCollision;
    CCRect near_path(state.path_bounds.getMinX() - 300.f, state.path_bounds.getMinY() - 300.f, state.path_bounds.size.width + 600.f, state.path_bounds.size.height + 600.f);
    bool kills = false;
    for (auto* object : CCArrayExt<GameObject*>(editor->m_objects)) {
        if (!can_kill_player(object) || !near_path.containsPoint(object->getPosition())) continue;
        if (moved_since_playtest(object, state) && kills_somewhere_on_path(object, state.path, fix_radius)) {
            kills = true;
            break;
        }
    }
    state.tint_on = kills;
    show_red_tint(kills);
}
