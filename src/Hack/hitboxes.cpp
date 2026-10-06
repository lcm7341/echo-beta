// draws hitboxes for players & objects, the trail of where the player has been & marks where clicks happened
// it reads the same collision lists gd checks, so only what can actually be hit is shown
// outline only collects every shape & hides the edges that fall inside another shape
#include <cstring>
#include "Logic/logic.hpp"
#include "Hack/color.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/object.hpp"
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/ShaderLayer.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <span>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

class Hitboxes {
public:
    static Hitboxes& get() {
        static Hitboxes value;
        return value;
    }

    void draw_player_hitbox(CCDrawNode* node, GJBaseGameLayer* play, PlayerObject* player, float width, bool fill, float const* tint);
    void capture_click(GJBaseGameLayer* play, PlayerObject* player, bool down);
    void draw_clicks(CCDrawNode* node, GJBaseGameLayer* play, float width, bool fill);
    void clear_clicks();
    void draw_object_hitbox(CCDrawNode* node, GJBaseGameLayer* play, GameObject* object, float width, bool fill);
    void push_trail(std::deque<HitboxTrailPoint>& trail, HitboxTrailPoint point, GJBaseGameLayer* play);
    int trail_limit();
    int trail_now();
    void trim_trail(std::deque<HitboxTrailPoint>& trail, GJBaseGameLayer* play);
};

constexpr int HITBOX_TAG = 0xEC06;

CCDrawNode* find_hitbox_node(GJBaseGameLayer* play) {
    auto find = [](CCNode* parent) -> CCDrawNode* {
        if (!parent)
            return nullptr;
        return typeinfo_cast<CCDrawNode*>(parent->getChildByTag(HITBOX_TAG));
    };
    if (auto* node = find(play))
        return node;
    if (auto* node = find(play->m_objectLayer))
        return node;
    if (auto* node = find(play->m_uiLayer))
        return node;
    return nullptr;
}

namespace {
    void sync_to_object_layer(CCNode* node, CCNode* object_layer) {
        if (!node || !object_layer || !node->getParent())
            return;
        node->setAdditionalTransform(CCAffineTransformConcat(
            object_layer->nodeToWorldTransform(),
            node->getParent()->worldToNodeTransform()));
    }

    int overlay_z_order(GJBaseGameLayer* play) {
        if (auto* editor = typeinfo_cast<LevelEditorLayer*>(play); editor && editor->m_editorUI) {
            return editor->m_editorUI->getZOrder() - 1;
        }
        return (play->m_uiLayer ? play->m_uiLayer->getZOrder() : 101) + 1;
    }

    class EchoHitboxNode : public CCDrawNode {
    public:
        static EchoHitboxNode* create() {
            auto* node = new EchoHitboxNode();
            if (node && node->init()) {
                node->autorelease();
                return node;
            }
            CC_SAFE_DELETE(node);
            return nullptr;
        }

        void visit() override {
            if (auto* play = GJBaseGameLayer::get(); play && play->m_objectLayer) {
                sync_to_object_layer(this, play->m_objectLayer);
            }
            CCDrawNode::visit();
        }
    };
}

void Logic::seat_overlay(CCNode* node, GJBaseGameLayer* play) {
    if (!node || !play || !play->m_objectLayer)
        return;
    // goes on the game layer NOT shaderlayer oops
    int z = overlay_z_order(play);
    if (node->getParent() != play) {
        node->removeFromParentAndCleanup(false);
        play->addChild(node, z, node->getTag());
    }
    else if (node->getZOrder() != z)
        node->setZOrder(z);
    node->setPosition({ 0.f, 0.f });
    node->setAnchorPoint({ 0.f, 0.f });
    node->setScale(1.f);
    node->setRotation(0.f);
    node->setSkewX(0.f);
    node->setSkewY(0.f);
    sync_to_object_layer(node, play->m_objectLayer);
}

void Logic::sync_overlay_seats(GJBaseGameLayer* play) {
    if (!play || !play->m_objectLayer)
        return;
    if (auto* node = find_hitbox_node(play))
        seat_overlay(node, play);
    if (auto* live = Logic::as_play(play))
        Trajectory::get().sync_seat(live);
}

struct ClickBox {
    CCRect outer;
    CCRect inner;
    bool rotated = false;
    bool down = true;
    std::array<CCPoint, 4> corners{};
    CCPoint center{};
    float radius = 0.f;
    int frame = 0;
};

std::deque<ClickBox> g_clicks;

CCRect visible_layer_rect(CCNode* layer);

struct PendingClick {
    bool player2 = false;
    bool down = true;
};

std::vector<PendingClick> g_pendingClicks;

float collision_radius(GameObject* object) {
    if (!object || object->m_objectRadius <= 0.f)
        return 0.f;
    float radius = object->m_objectRadius;
    float sx = std::abs(object->m_scaleX);
    float sy = std::abs(object->m_scaleY);
    if (sx != 1.f || sy != 1.f)
        radius *= std::max(sx, sy);
    return radius;
}

float hazard_radius(GameObject* object) {
    return collision_radius(object);
}

// drawing a hitbox fucks with cached
class HitboxStateGuard {
    static constexpr size_t SPAN_FROM = 0x2e0, SPAN_TO = 0x370, BOX_FROM = 0x140, BOX_TO = 0x1b8;
    GameObject* m_object;
    std::array<uint8_t, SPAN_TO - SPAN_FROM> m_fields{};
    OBB2D* m_box;
    std::array<uint8_t, BOX_TO - BOX_FROM> m_boxData{};
    // sits before the span. gd works the offset out ONCE & keeps it (the deadlocked teleport portal thing)
    bool m_offsetCalculated;
    CCPoint m_offset;

public:
    explicit HitboxStateGuard(GameObject* object) : m_object(object), m_box(object->m_orientedBox),
        m_offsetCalculated(object->m_boxOffsetCalculated), m_offset(object->m_boxOffset) {
        auto* bytes = reinterpret_cast<uint8_t*>(object);
        std::memcpy(m_fields.data(), bytes + SPAN_FROM, m_fields.size());
        if (m_box)
            std::memcpy(m_boxData.data(), reinterpret_cast<uint8_t*>(m_box) + BOX_FROM, m_boxData.size());
    }

    ~HitboxStateGuard() {
        auto* created = m_object->m_orientedBox;
        if (created && created != m_box)
            created->release();
        std::memcpy(reinterpret_cast<uint8_t*>(m_object) + SPAN_FROM, m_fields.data(), m_fields.size());
        m_object->m_boxOffsetCalculated = m_offsetCalculated;
        m_object->m_boxOffset = m_offset;
        if (m_box)
            std::memcpy(reinterpret_cast<uint8_t*>(m_box) + BOX_FROM, m_boxData.data(), m_boxData.size());
    }

    HitboxStateGuard(HitboxStateGuard const&) = delete;
    HitboxStateGuard& operator=(HitboxStateGuard const&) = delete;
};

OBB2D* live_oriented_box(GameObject* object) {
    if (!object)
        return nullptr;
    bool editor = LevelEditorLayer::get() && !PlayLayer::get();
    float saved_start = object->m_startRotationX;
    if (editor)
        object->m_startRotationX = object->getRotation() - object->m_rotationXOffset;
    object->m_isOrientedBoxDirty = true;
    object->m_isObjectRectDirty = true;
    object->updateOrientedBox();
    if (editor)
        object->m_startRotationX = saved_start;
    if (auto* box = object->m_orientedBox)
        return box;
    return object->getOrientedBox();
}

void refresh_object_hitbox(GameObject* object) {
    if (!object)
        return;
    // objs with a box offset (teleport portals) keep the box gd already has, a redo moves it & that is NOT what the player hits
    if (object->m_customBoxOffset.x != 0.f || object->m_customBoxOffset.y != 0.f)
        return;
    // TODO hitboxes lag behind move triggers
    object->m_isObjectRectDirty = true;
}

// obj hitbox without touching its caches
CCRect Logic::object_hitbox(GameObject* object) {
    if (!object)
        return {};
    HitboxStateGuard preserve(object);
    refresh_object_hitbox(object);
    return object->getObjectRect();
}

void Logic::note_click_hitbox(GJBaseGameLayer* play, bool player1, bool down) {
    if (!play || Logic::get().predicting)
        return;
    if (!Logic::get().show_hitboxes || !Logic::get().hitbox_click)
        return;
    g_pendingClicks.push_back({ !player1, down });
    if (!play->m_gameState.m_isDualMode)
        return;
    g_pendingClicks.push_back({ player1, down });
}

void Logic::capture_pending_click(PlayerObject* player) {
    auto* play = GJBaseGameLayer::get();
    if (!play || !player || Logic::get().is_traj_player(player))
        return;
    bool player2 = player == play->m_player2;
    std::erase_if(g_pendingClicks, [&](PendingClick const& click) {
        if (click.player2 != player2)
            return false;
        Hitboxes::get().capture_click(play, player, click.down);
        return true;
    });
}

void Logic::flush_click_hitboxes(GJBaseGameLayer* play) {
    if (!play) {
        g_pendingClicks.clear();
        return;
    }
    auto const& logic = Logic::get();
    size_t from = g_pendingClicks.size() > 400 ? g_pendingClicks.size() - 400 : 0;
    for (auto const& click : std::span(g_pendingClicks).subspan(from)) {
        auto* player = click.player2 ? play->m_player2 : play->m_player1;
        if ((click.player2 && Logic::get().hitbox_player_hidden(true)) || (!click.player2 && Logic::get().hitbox_player_hidden(false)))
            continue;
        Hitboxes::get().capture_click(play, player, click.down);
    }
    g_pendingClicks.clear();
}

void Logic::clear_click_hitbox() {
    g_pendingClicks.clear();
    Hitboxes::get().clear_clicks();
}

void Hitboxes::capture_click(GJBaseGameLayer*, PlayerObject* player, bool down) {
    auto const& logic = Logic::get();
    if (!player || !logic.show_hitboxes || !logic.hitbox_click || Logic::get().is_traj_player(player))
        return;
    ClickBox box;
    box.down = down;
    box.frame = Hitboxes::get().trail_now();
    box.outer = player->getObjectRect();
    box.inner = player->getObjectRect(0.3f, 0.3f);
    box.center = CCPoint{ box.outer.getMidX(), box.outer.getMidY() };
    box.radius = std::abs(box.outer.size.width) * 0.5f;
    if (auto* oriented = player->getOrientedBox()) {
        box.corners = oriented->m_corners;
        box.rotated = true;
    }
    g_clicks.push_back(box);
    while (g_clicks.size() > 4096)
        g_clicks.pop_front();
}

void Hitboxes::clear_clicks() {
    g_clicks.clear();
}

void Hitboxes::draw_clicks(CCDrawNode* node, GJBaseGameLayer* play, float width, bool fill) {
    auto const& logic = Logic::get();
    if (!logic.hitbox_click) {
        g_clicks.clear();
        return;
    }
    if (!(play && LevelEditorLayer::get() == play)) {
        int now = Hitboxes::get().trail_now();
        CCRect view = play && play->m_objectLayer ? visible_layer_rect(play->m_objectLayer) : CCRect{};
        bool view_known = std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f;
        int limit = Hitboxes::get().trail_limit();
        std::erase_if(g_clicks, [&](ClickBox const& box) {
            if (box.frame > now)
                return true;
            return view_known ? !view.intersectsRect(box.outer) : now - box.frame >= limit;
        });
    }
    for (auto const& box : g_clicks) {
        float const* color = box.down ? logic.hitbox_click_color : logic.hitbox_release_color;
        auto tint = HitboxColor::with_fill(color, fill, logic.hitbox_fill_opacity);
        auto line = HitboxColor::line_of(color);
        if (logic.hitbox_rotated && box.rotated) {
            auto corners = box.corners;
            node->drawPolygon(corners.data(), 4, tint, width, line, BorderAlignment::Inside);
        }
        if (logic.hitbox_player || (!logic.hitbox_rotated && !logic.hitbox_inner && !logic.hitbox_circle)) {
            node->drawRect(box.outer, tint, width, line, BorderAlignment::Inside);
        }
        if (logic.hitbox_inner)
            node->drawRect(box.inner, tint, width, line, BorderAlignment::Inside);
        if (logic.hitbox_circle && box.radius > 0.f) {
            node->drawCircle(box.center, box.radius, { 0.f, 0.f, 0.f, 0.f }, width, line, 24);
        }
    }
}

void Hitboxes::draw_player_hitbox(CCDrawNode* node, GJBaseGameLayer* play, PlayerObject* player, float width, bool fill, float const* tint) {
    if (!player || Logic::get().is_traj_player(player))
        return;
    auto const& logic = Logic::get();
    auto rect = player->getObjectRect();
    auto inner_rect = player->getObjectRect(0.3f, 0.3f);
    CCPoint center{ rect.getMidX(), rect.getMidY() };
    float const* outerColor = tint ? tint : logic.hitbox_outer_color;
    float const* rotated_color = tint ? tint : logic.hitbox_rotated_color;
    float const* innerColor = tint ? tint : logic.hitbox_inner_color;
    float const* circle_color = tint ? tint : logic.hitbox_circle_color;
    if (logic.hitbox_rotated) {
        if (auto* box = player->getOrientedBox()) {
            std::array<CCPoint, 4> corners = box->m_corners;
            node->drawPolygon(corners.data(), 4, HitboxColor::with_fill(rotated_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(rotated_color), BorderAlignment::Inside);
        }
    }
    if (logic.hitbox_player) {
        node->drawRect(rect, HitboxColor::with_fill(outerColor, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(outerColor), BorderAlignment::Inside);
    }
    if (logic.hitbox_inner) {
        node->drawRect(inner_rect, HitboxColor::with_fill(innerColor, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(innerColor), BorderAlignment::Inside);
    }
    float radius = std::abs(rect.size.width) * 0.5f;
    float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
    // with fix radius collision on gd uses a circle not the box
    bool fix_radius = play->m_levelSettings && play->m_levelSettings->m_fixRadiusCollision;
    if ((logic.hitbox_circle || fix_radius) && radius > 0.f) {
        unsigned segments = static_cast<unsigned>(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
        node->drawCircle(center, radius, { 0.f, 0.f, 0.f, 0.f }, width, HitboxColor::line_of(circle_color), std::max(segments, 8u));
    }
}

// Outline Only --> collects every shape then cuts the edges that are inside another one
namespace {
    struct OutlineShape {
        std::vector<CCPoint> points;
        ccColor4F fill;
        ccColor4F line;
        CCRect bounds;
        int kind = 0;
    };

    struct OutlineSink {
        bool collecting = false;
        int kind = 0;
        std::vector<OutlineShape> shapes;
    };

    OutlineSink& outlines() {
        static OutlineSink sink;
        return sink;
    }

    void collect_shape(CCPoint const* points, unsigned count, ccColor4F const& fill, ccColor4F const& line) {
        if (count < 3)
            return;

        OutlineShape shape;
        shape.points.assign(points, points + count);
        float area = 0.f;
        for (unsigned i = 0; i < count; ++i) {
            auto const& a = shape.points[i];
            auto const& b = shape.points[(i + 1) % count];
            area += a.x * b.y - b.x * a.y;
        }
        if (area < 0.f)
            std::reverse(shape.points.begin(), shape.points.end());
        float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
        for (auto const& p : shape.points) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
        shape.bounds = { minX, minY, maxX - minX, maxY - minY };
        shape.fill = fill;
        shape.line = line;
        shape.kind = outlines().kind;
        outlines().shapes.push_back(std::move(shape));
    }

    void emit_polygon(CCDrawNode* node, CCPoint* points, unsigned count, ccColor4F const& fill, float width, ccColor4F const& line) {
        if (outlines().collecting)
            return collect_shape(points, count, fill, line);
        node->drawPolygon(points, count, fill, width, line, BorderAlignment::Inside);
    }

    void emit_rect(CCDrawNode* node, CCRect const& rect, ccColor4F const& fill, float width, ccColor4F const& line) {
        if (outlines().collecting) {
            std::array<CCPoint, 4> corners{ rect.origin, ccp(rect.getMaxX(), rect.getMinY()), ccp(rect.getMaxX(), rect.getMaxY()), ccp(rect.getMinX(), rect.getMaxY()) };
            return collect_shape(corners.data(), 4, fill, line);
        }
        node->drawRect(rect, fill, width, line, BorderAlignment::Inside);
    }

    void emit_circle(CCDrawNode* node, CCPoint const& center, float radius, ccColor4F const& fill, float width, ccColor4F const& line, unsigned segments) {
        if (outlines().collecting) {
            std::vector<CCPoint> ring(std::max(segments, 8u));
            for (std::size_t i = 0; i < ring.size(); ++i) {
                float angle = (float)(i) * 2.f * (float)(M_PI) / (float)(ring.size());
                ring[i] = center + CCPoint{ std::cos(angle), std::sin(angle) } * radius;
            }
            return collect_shape(ring.data(), static_cast<unsigned>(ring.size()), fill, line);
        }
        node->drawCircle(center, radius, fill, width, line, segments);
    }

    bool inside_shape(OutlineShape const& shape, CCPoint p) {
        if (!shape.bounds.containsPoint(p))
            return false;
        auto const& pts = shape.points;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            auto const& a = pts[i];
            auto const& b = pts[(i + 1) % pts.size()];
            if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) < -1e-4f)
                return false;
        }
        return true;
    }

    void flush_outlines(CCDrawNode* node, float width) {
        auto& sink = outlines();
        sink.collecting = false;
        auto& shapes = sink.shapes;
        if (shapes.empty())
            return;
        for (auto& shape : shapes) {
            if (shape.fill.a > 0.f)
                node->drawPolygon(shape.points.data(), static_cast<unsigned>(shape.points.size()), shape.fill, 0.f, { 0.f, 0.f, 0.f, 0.f });
        }
        constexpr float CELL = 90.f;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells;
        auto cell_key = [](int x, int y) {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
        };
        for (std::uint32_t i = 0; i < shapes.size(); ++i) {
            auto const& b = shapes[i].bounds;
            for (int x = (int)(std::floor(b.getMinX() / CELL)); x <= (int)(std::floor(b.getMaxX() / CELL)); ++x)
                for (int y = (int)(std::floor(b.getMinY() / CELL)); y <= (int)(std::floor(b.getMaxY() / CELL)); ++y)
                    cells[cell_key(x, y)].push_back(i);
        }
        std::vector<std::uint32_t> nearby;
        std::vector<std::uint32_t> seen(shapes.size(), UINT32_MAX);
        std::vector<float> cuts;
        float depth = width;
        float probe = std::max(0.05f, width * 0.25f);
        for (std::uint32_t i = 0; i < shapes.size(); ++i) {
            auto const& shape = shapes[i];
            auto const& pts = shape.points;
            for (std::size_t e = 0; e < pts.size(); ++e) {
                auto a = pts[e];
                auto b = pts[(e + 1) % pts.size()];
                auto d = b - a;
                float len = std::hypot(d.x, d.y);
                if (len < 1e-4f)
                    continue;
                CCPoint outward{ d.y / len, -d.x / len };
                nearby.clear();
                float minX = std::min(a.x, b.x) - probe, maxX = std::max(a.x, b.x) + probe;
                float minY = std::min(a.y, b.y) - probe, maxY = std::max(a.y, b.y) + probe;
                for (int x = (int)(std::floor(minX / CELL)); x <= (int)(std::floor(maxX / CELL)); ++x)
                    for (int y = (int)(std::floor(minY / CELL)); y <= (int)(std::floor(maxY / CELL)); ++y) {
                        auto it = cells.find(cell_key(x, y));
                        if (it == cells.end())
                            continue;
                        for (auto j : it->second) {
                            if (j == i || seen[j] == i * 1024u + e || shapes[j].kind != shape.kind)
                                continue;
                            seen[j] = static_cast<std::uint32_t>(i * 1024u + e);
                            auto const& o = shapes[j].bounds;
                            if (o.getMaxX() < minX || o.getMinX() > maxX || o.getMaxY() < minY || o.getMinY() > maxY)
                                continue;
                            nearby.push_back(j);
                        }
                    }
                cuts.assign({ 0.f, 1.f });
                for (auto j : nearby) {
                    auto const& other = shapes[j].points;
                    for (std::size_t k = 0; k < other.size(); ++k) {
                        auto c = other[k];
                        auto f = other[(k + 1) % other.size()] - c;
                        float denom = d.x * f.y - d.y * f.x;
                        if (std::abs(denom) < 1e-6f)
                            continue;
                        auto ac = c - a;
                        float t = (ac.x * f.y - ac.y * f.x) / denom;
                        float u = (ac.x * d.y - ac.y * d.x) / denom;
                        if (t > 0.f && t < 1.f && u >= 0.f && u <= 1.f)
                            cuts.push_back(t);
                    }
                }
                std::sort(cuts.begin(), cuts.end());
                CCPoint inward = outward * -depth;
                for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
                    float t0 = cuts[c], t1 = cuts[c + 1];
                    if ((t1 - t0) * len < 0.01f)
                        continue;
                    auto mid = a + d * ((t0 + t1) * 0.5f);
                    auto test = mid + outward * probe;
                    bool covered = std::ranges::any_of(nearby, [&](std::uint32_t j) {
                        return inside_shape(shapes[j], test);
                    });
                    if (covered)
                        continue;
                    float trim = std::min(depth / len, (t1 - t0) * 0.5f);
                    node->drawSegment(a + d * (t0 + trim) + inward, a + d * (t1 - trim) + inward, depth, shape.line);
                }
            }
        }
        shapes.clear();
    }
}

void Hitboxes::draw_object_hitbox(CCDrawNode* node, GJBaseGameLayer* play, GameObject* object, float width, bool fill) {
    if (!object || object->m_isGroupDisabled)
        return;
    // DONT check m_isActivated its late on some lvls fsr
    if (object->m_objectType == GameObjectType::Decoration)
        return;
    if (object == play->m_player1 || object == play->m_player2 || Logic::get().is_traj_player(object))
        return;
    if (auto* live = Logic::as_play(play)) {
        if (object == live->m_player1CollisionBlock || object == live->m_player2CollisionBlock)
            return;
        if (object == live->m_anticheatSpike)
            return;
    }
    auto const& logic = Logic::get();
    HitboxStateGuard preserve(object);
    refresh_object_hitbox(object);

    switch (object->m_objectType) {
    case GameObjectType::Solid:
    case GameObjectType::Slope:
    case GameObjectType::Hazard:
    case GameObjectType::AnimatedHazard:
        outlines().kind = 0;
        break;
    default:
        outlines().kind = 1;
        break;
    }
    auto paint_rect = [&](float const* color) {
        emit_rect(node, object->getObjectRect(), HitboxColor::with_fill(color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(color));
    };

    switch (object->m_objectType) {
    case GameObjectType::Solid: {
        if (!logic.hitbox_solid)
            break;
        if (object->m_isPassable)
            break;
        paint_rect(logic.hitbox_solid_color);
        break;
    }
    case GameObjectType::Slope: {
        auto rect = object->getObjectRect();
        std::array<CCPoint, 3> verts{
            rect.origin,
            ccp(rect.getMinX(), rect.getMaxY()),
            ccp(rect.getMaxX(), rect.getMinY()),
        };
        CCPoint max_point{ rect.getMaxX(), rect.getMaxY() };
        switch (object->m_slopeDirection) {
        case 0:
        case 7:
            verts[1] = max_point;
            break;
        case 1:
        case 5:
            verts[0] = max_point;
            break;
        case 3:
        case 6:
            verts[2] = max_point;
            break;
        default:
            break;
        }
        // hazard slope = hazard color, hopefully that is what bro meant
        float const* slope_color = object->m_slopeIsHazard ? logic.hitbox_hazard_color : logic.hitbox_slope_color;
        if (object->m_slopeIsHazard ? !logic.hitbox_hazard : !logic.hitbox_slope)
            break;
        if (object->m_slopeIsHazard) {
            int dir = object->m_slopeDirection;
            bool ceiling = dir >= 0 && dir <= 6 && ((0x6a >> dir) & 1);
            float shift = (ceiling ? -4.f : 4.f);
            int corner = 0;
            for (int k = 0; k < 3; ++k) {
                auto const& a = verts[(k + 1) % 3];
                auto const& b = verts[(k + 2) % 3];
                auto const& c = verts[k];
                if ((c.x == a.x && c.y == b.y) || (c.x == b.x && c.y == a.y))
                    corner = k;
            }
            std::array<CCPoint, 4> shape;
            int n = 0;
            for (int k = 0; k < 3; ++k) {
                auto const& p = verts[(corner + k) % 3];
                if (k == 0) {
                    shape[n++] = p;
                    continue;
                }
                CCPoint moved{ p.x, p.y + shift };
                bool level = p.y == verts[corner].y;
                if (level && k == 1) {
                    shape[n++] = p;
                    shape[n++] = moved;
                }
                else if (level) {
                    shape[n++] = moved;
                    shape[n++] = p;
                }
                else {
                    shape[n++] = moved;
                }
            }
            emit_polygon(node, shape.data(), n, HitboxColor::with_fill(slope_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(slope_color));
            break;
        }
        emit_polygon(node, verts.data(), 3, HitboxColor::with_fill(logic.hitbox_slope_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(logic.hitbox_slope_color));
        break;
    }
    case GameObjectType::Hazard:
    case GameObjectType::AnimatedHazard: {
        if (!logic.hitbox_hazard)
            break;
        float radius = hazard_radius(object);
        float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
        if (radius > 0.f) {
            unsigned segments = static_cast<unsigned>(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
            emit_circle(node, object->getPosition(), radius, HitboxColor::with_fill(logic.hitbox_hazard_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(logic.hitbox_hazard_color), std::max(segments, 8u));
        }
        else if (auto* box = live_oriented_box(object)) {
            std::array<CCPoint, 4> corners = box->m_corners;
            emit_polygon(node, corners.data(), 4, HitboxColor::with_fill(logic.hitbox_hazard_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(logic.hitbox_hazard_color));
        }
        else {
            paint_rect(logic.hitbox_hazard_color);
        }
        break;
    }
    default: {
        if (logic.hide_activated_objects) {
            if (auto* enhanced = typeinfo_cast<EnhancedGameObject*>(object)) {
                bool used = play->m_player1 && phys::hasBeenActivatedByPlayer(play->m_player1, enhanced);
                if (play->m_gameState.m_isDualMode) {
                    used = used && play->m_player2 && phys::hasBeenActivatedByPlayer(play->m_player2, enhanced);
                }
                if (used)
                    return;
            }
        }
        if (!logic.hitbox_interact)
            break;
        float radius = collision_radius(object);
        bool fix_radius = play->m_levelSettings && play->m_levelSettings->m_fixRadiusCollision;
        bool use_box = radius <= 0.f || (object->m_shouldUseOuterOb && !fix_radius);
        if (radius > 0.f) {
            float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
            unsigned segments = static_cast<unsigned>(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
            emit_circle(node, object->getPosition(), radius, HitboxColor::with_fill(logic.hitbox_interact_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(logic.hitbox_interact_color), std::max(segments, 8u));
        }
        if (use_box) {
            if (auto* box = live_oriented_box(object)) {
                std::array<CCPoint, 4> corners = box->m_corners;
                emit_polygon(node, corners.data(), 4, HitboxColor::with_fill(logic.hitbox_interact_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_of(logic.hitbox_interact_color));
            }
            else if (radius <= 0.f) {
                paint_rect(logic.hitbox_interact_color);
            }
        }
        break;
    }
    }
}

CCRect visible_layer_rect(CCNode* layer) {
    auto win = CCDirector::sharedDirector()->getWinSize();
    CCPoint corners[4] = { { 0.f, 0.f }, { win.width, 0.f }, { win.width, win.height }, { 0.f, win.height } };
    float minX = 0.f, minY = 0.f;
    float maxX = 0.f, maxY = 0.f;
    for (int i = 0; i < 4; ++i) {
        CCPoint local = layer->convertToNodeSpace(corners[i]);
        if (i == 0) {
            minX = maxX = local.x;
            minY = maxY = local.y;
            continue;
        }
        minX = std::min(minX, local.x);
        minY = std::min(minY, local.y);
        maxX = std::max(maxX, local.x);
        maxY = std::max(maxY, local.y);
    }
    return { minX, minY, std::max(maxX - minX, 0.f), std::max(maxY - minY, 0.f) };
}

CCRect upright_rect(CCRect rect) {
    if (rect.size.width < 0.f) {
        rect.origin.x += rect.size.width;
        rect.size.width = -rect.size.width;
    }
    if (rect.size.height < 0.f) {
        rect.origin.y += rect.size.height;
        rect.size.height = -rect.size.height;
    }
    return rect;
}

struct TrailFit {
    float width = 0.f, height = 0.f;
};

CCRect trail_rect(PlayerObject* player, TrailFit& fit) {
    CCRect raw = upright_rect(player->getObjectRect());
    bool known = fit.width >= 2.f && fit.height >= 2.f;
    bool finite = std::isfinite(raw.origin.x) && std::isfinite(raw.origin.y) && std::isfinite(raw.size.width) && std::isfinite(raw.size.height);
    bool tiny = !finite || std::abs(player->getScaleX()) < 0.2f || raw.size.width < 2.f || raw.size.height < 2.f;
    bool smear = finite && (raw.size.width > 75.f || raw.size.height > 75.f);
    if (known && finite) {
        smear = smear || (raw.size.width > fit.width * 2.5f && raw.size.width > 45.f) || (raw.size.height > fit.height * 2.5f && raw.size.height > 45.f);
    }
    if (!tiny && !smear) {
        fit.width = raw.size.width;
        fit.height = raw.size.height;
        return raw;
    }
    if (!known) {
        fit.width = std::clamp(finite ? raw.size.width : 30.f, 8.f, 36.f);
        fit.height = std::clamp(finite ? raw.size.height : 30.f, 8.f, 36.f);
    }
    CCPoint pos = player->getPosition();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y))
        pos = CCPoint{ raw.getMidX(), raw.getMidY() };
    CCRect held(pos.x - fit.width * 0.5f, pos.y - fit.height * 0.5f, fit.width, fit.height);
    return held;
}

bool same_trail_spot(CCRect const& a, CCRect const& b) {
    auto close = [](float left, float right) {
        return std::abs(left - right) < 0.05f;
    };
    return close(a.origin.x, b.origin.x) && close(a.origin.y, b.origin.y) && close(a.size.width, b.size.width) && close(a.size.height, b.size.height);
}

void Hitboxes::push_trail(std::deque<HitboxTrailPoint>& trail, HitboxTrailPoint point, GJBaseGameLayer* play) {
    if (!trail.empty() && trail.back().frame == point.frame) {
        trail.back() = point;
        return;
    }
    if (Logic::get().hitbox_dedup && !trail.empty() && same_trail_spot(trail.back().rect, point.rect)) {
        point.frame = trail.back().frame;
        trail.back() = point;
        return;
    }
    trail.push_back(point);
    Hitboxes::get().trim_trail(trail, play);
}

int Hitboxes::trail_limit() {
    auto const& logic = Logic::get();
    int len = logic.trail_length;
    if (len <= 0)
        len = std::max(1, (int)(std::lround(logic.fps)));
    return std::max(1, len);
}

int Hitboxes::trail_now() {
    auto const& logic = Logic::get();
    return logic.hitbox_trail_visual ? logic.visual_frame : logic.step_frame;
}

void Hitboxes::trim_trail(std::deque<HitboxTrailPoint>& trail, GJBaseGameLayer* play) {
    auto const& logic = Logic::get();
    if (play && LevelEditorLayer::get() == play)
        return;
    if (logic.hitbox_trail_visual && logic.is_playing() && play && play->m_objectLayer) {
        CCRect view = visible_layer_rect(play->m_objectLayer);
        if (std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f) {
            std::erase_if(trail, [&](HitboxTrailPoint const& point) {
                return !view.intersectsRect(point.rect);
            });
            return;
        }
    }
    int limit = Hitboxes::get().trail_limit();
    int now = Hitboxes::get().trail_now();
    while (!trail.empty() && (trail.front().frame > now || now - trail.front().frame >= limit))
        trail.pop_front();
}

void Logic::sample_hitbox_trail(GJBaseGameLayer* play) {
    auto& logic = Logic::get();
    if (!play || !logic.show_hitboxes || !logic.hitbox_trail)
        return;
    if (auto* live = Logic::as_play(play); live && (live->m_levelEndAnimationStarted || live->m_playerDied))
        return;
    int frame = Hitboxes::get().trail_now();
    static TrailFit fit_p1, fit_p2;
    if (logic.trail_p1.empty())
        fit_p1 = {};
    if (logic.trail_p2.empty())
        fit_p2 = {};
    auto capture = [&](PlayerObject* player, TrailFit& fit, std::deque<HitboxTrailPoint>& trail) {
        HitboxTrailPoint point;
        point.frame = frame;
        point.rect = trail_rect(player, fit);
        CCPoint mid{ point.rect.getMidX(), point.rect.getMidY() };
        point.inner = point.rect;
        point.inner.size.width *= 0.3f;
        point.inner.size.height *= 0.3f;
        point.inner.origin.x = mid.x - point.inner.size.width * 0.5f;
        point.inner.origin.y = mid.y - point.inner.size.height * 0.5f;
        point.center = mid;
        point.radius = std::abs(point.rect.size.width) * 0.5f;
        if (auto* oriented = player->getOrientedBox()) {
            point.corners = oriented->m_corners;
            float minX = point.corners[0].x, maxX = minX;
            float minY = point.corners[0].y, maxY = minY;
            for (int i = 1; i < 4; ++i) {
                minX = std::min(minX, point.corners[i].x);
                maxX = std::max(maxX, point.corners[i].x);
                minY = std::min(minY, point.corners[i].y);
                maxY = std::max(maxY, point.corners[i].y);
            }
            float span_w = maxX - minX, span_h = maxY - minY;
            bool huge = span_w > 75.f || span_h > 75.f || (span_w > std::abs(point.rect.size.width) * 2.5f && span_w > 45.f) || (span_h > std::abs(point.rect.size.height) * 2.5f && span_h > 45.f);
            point.rotated = !huge;
        }
        Hitboxes::get().push_trail(trail, point, play);
    };
    if (play->m_player1 && !Logic::get().hitbox_player_hidden(false) && !Logic::get().is_traj_player(play->m_player1))
        capture(play->m_player1, fit_p1, logic.trail_p1);
    if (play->m_player2 && !Logic::get().hitbox_player_hidden(true) && !Logic::get().is_traj_player(play->m_player2))
        capture(play->m_player2, fit_p2, logic.trail_p2);
}

void Logic::draw_hitboxes(GJBaseGameLayer* play) {
    auto& value = Logic::get();
    auto* layer = play->m_objectLayer;
    if (!layer)
        return;
    auto* node = find_hitbox_node(play);
    if (!value.show_hitboxes) {
        if (node)
            node->setVisible(false);
        value.trail_p1.clear();
        value.trail_p2.clear();
        return;
    }
    if (!node) {
        node = EchoHitboxNode::create();
        node->setTag(HITBOX_TAG);
    }
    node->setVisible(true);
    node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
    seat_overlay(node, play);
    node->clear();
    if (value.hide_p1 && play->m_player1)
        play->m_player1->setVisible(false);
    if (value.hide_p2 && play->m_player2)
        play->m_player2->setVisible(false);
    auto* player = play->m_player1;
    bool level = Logic::as_play(play) != nullptr;
    float view_scale = std::max(play->m_gameState.m_cameraZoom, 0.05f);
    if (!level)
        view_scale = std::max(std::abs(layer->getScaleX()), 0.05f);
    float width = std::max(value.hitbox_thickness, 0.f) / view_scale;
    Hitboxes::get().trim_trail(value.trail_p1, play);
    Hitboxes::get().trim_trail(value.trail_p2, play);
    if (!value.hitbox_trail) {
        value.trail_p1.clear();
        value.trail_p2.clear();
    }
    auto grid = [](float value) {
        return (int)(std::lround(value * 20.f));
    };

    struct Spot {
        int x, y, w, h;
        bool operator==(Spot const& other) const = default;
    };

    struct SpotHash {
        size_t operator()(Spot const& spot) const {
            size_t hash = static_cast<size_t>(spot.x);
            hash = hash * 1315423911u + static_cast<size_t>(spot.y);
            hash = hash * 1315423911u + static_cast<size_t>(spot.w);
            return hash * 1315423911u + static_cast<size_t>(spot.h);
        }
    };

    struct Corners {
        std::array<int, 8> point{};
        bool operator==(Corners const& other) const = default;
    };

    struct CornersHash {
        size_t operator()(Corners const& corners) const {
            size_t hash = 0;
            for (int value : corners.point)
                hash = hash * 1315423911u + static_cast<size_t>(value);
            return hash;
        }
    };

    auto quantize = [&](CCRect const& rect) {
        return Spot{ grid(rect.origin.x), grid(rect.origin.y), grid(rect.size.width), grid(rect.size.height) };
    };
    auto quantize_corners = [&](std::array<CCPoint, 4> const& corners) {
        Corners key;
        for (int i = 0; i < 4; ++i) {
            key.point[i * 2] = grid(corners[i].x);
            key.point[i * 2 + 1] = grid(corners[i].y);
        }
        return key;
    };
    auto quantize_circle = [&](CCPoint center, float radius) {
        return Spot{ grid(center.x), grid(center.y), grid(radius), 0 };
    };
    std::unordered_set<Spot, SpotHash> drawn_boxes;
    std::unordered_set<Corners, CornersHash> drawn_rotated;
    std::unordered_set<Spot, SpotHash> drawn_inner, drawn_circles;
    auto claim_live = [&](PlayerObject* body) {
        if (!body || Logic::get().is_traj_player(body))
            return;
        auto rect = body->getObjectRect();
        auto inner = body->getObjectRect(0.3f, 0.3f);
        drawn_boxes.insert(quantize(rect));
        drawn_inner.insert(quantize(inner));
        drawn_circles.insert(quantize_circle(CCPoint{ rect.getMidX(), rect.getMidY() }, std::abs(rect.size.width) * 0.5f));
        if (auto* oriented = body->getOrientedBox()) {
            std::array<CCPoint, 4> corners = oriented->m_corners;
            drawn_rotated.insert(quantize_corners(corners));
        }
    };
    if (!Logic::get().hitbox_player_hidden(false))
        claim_live(play->m_player1);
    if (play->m_gameState.m_isDualMode && !Logic::get().hitbox_player_hidden(true))
        claim_live(play->m_player2);
    bool fill = value.fill_hitboxes;
    float trail_opacity = std::clamp(value.hitbox_trail_opacity, 0.f, 1.f);
    auto trail_line = [&](float const* color) {
        auto line = HitboxColor::line_of(color);
        line.a *= trail_opacity;
        return line;
    };
    auto trail_fill = [&](float const* color) {
        auto painted = HitboxColor::with_fill(color, fill, value.hitbox_fill_opacity);
        painted.a *= trail_opacity;
        return painted;
    };
    auto each_trail = [&](auto&& draw) {
        if (!value.hitbox_trail)
            return;
        draw(value.trail_p1);
        draw(value.trail_p2);
    };
    if (value.hitbox_player) {
        each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
            for (auto const& old : trail) {
                if (!drawn_boxes.insert(quantize(old.rect)).second)
                    continue;
                node->drawRect(old.rect, trail_fill(value.hitbox_outer_color), width, trail_line(value.hitbox_outer_color), BorderAlignment::Inside);
            }
        });
    }
    if (value.hitbox_rotated) {
        each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
            for (auto const& old : trail) {
                if (!old.rotated || !drawn_rotated.insert(quantize_corners(old.corners)).second)
                    continue;
                auto corners = old.corners;
                node->drawPolygon(corners.data(), 4, trail_fill(value.hitbox_rotated_color), width, trail_line(value.hitbox_rotated_color), BorderAlignment::Inside);
            }
        });
    }
    if (value.hitbox_circle) {
        each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
            for (auto const& old : trail) {
                if (!(old.radius > 0.f) || !drawn_circles.insert(quantize_circle(old.center, old.radius)).second)
                    continue;
                node->drawCircle(old.center, old.radius, trail_fill(value.hitbox_circle_color), width, trail_line(value.hitbox_circle_color), 16);
            }
        });
    }
    if (value.hitbox_inner) {
        each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
            for (auto const& old : trail) {
                if (!drawn_inner.insert(quantize(old.inner)).second)
                    continue;
                node->drawRect(old.inner, trail_fill(value.hitbox_inner_color), width, trail_line(value.hitbox_inner_color), BorderAlignment::Inside);
            }
        });
    }
    float view_x = player ? player->getPositionX() : 0.f;
    float reach = 500.f;
    CCRect view = visible_layer_rect(layer);
    bool use_view = std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f;
    if (use_view) {
        // TODO use camera x for now might fuck with camera trigger but we ball
        view_x = view.getMidX();
        reach = view.size.width * 0.5f + 200.f;
    }
    else if (!level) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        view_x = (-layer->getPositionX() + win.width * 0.5f) / view_scale;
        reach = win.width / view_scale;
    }
    outlines().collecting = value.hitbox_outline_only;
    outlines().shapes.clear();
    if (Logic::as_play(play)) {
        auto& grid = play->m_nonEffectObjects;
        auto& sizes = play->m_nonEffectObjectsSizes;
        float minY = use_view ? view.getMinY() - 200.f : -1e9f;
        float maxY = use_view ? view.getMaxY() + 200.f : 1e9f;
        float xFactor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
        int first_x = std::max((int)(std::floor((view_x - reach) * xFactor)) - 1, 0);
        int last_x = std::min((int)(std::floor((view_x + reach) * xFactor)) + 1, (int)(std::min(grid.size(), sizes.size())) - 1);
        for (int x = first_x; x <= last_x; ++x) {
            auto* column = grid[x];
            auto* counts = sizes[x];
            if (!column || !counts)
                continue;
            int rows = (int)(std::min(column->size(), counts->size()));
        }
        // moving objects arent actually in the grid
        auto const& moving = play->m_calcNonEffectObjects;
        int moving_count = std::min<int>(play->m_calcNonEffectObjectsSize, (int)(moving.size()));
        for (int i = 0; i < moving_count; ++i) {
            auto* obj = moving[static_cast<std::size_t>(i)];
            if (!obj || obj->m_isDisabled)
                continue;
            if (std::fabs(obj->getPositionX() - view_x) > reach)
                continue;
            Hitboxes::get().draw_object_hitbox(node, play, obj, width, value.fill_hitboxes);
        }
    }
    else if (play->m_objects) {
        for (auto* obj : CCArrayExt<GameObject*>(play->m_objects)) {
            if (!obj)
                continue;
            if (obj->m_isDisabled)
                continue;
            if (std::fabs(obj->getPositionX() - view_x) > reach)
                continue;
            Hitboxes::get().draw_object_hitbox(node, play, obj, width, value.fill_hitboxes);
        }
    }
    if (outlines().collecting)
        flush_outlines(node, width);
    if (!Logic::get().hitbox_player_hidden(false))
        Hitboxes::get().draw_player_hitbox(node, play, play->m_player1, width, value.fill_hitboxes, nullptr);
    else
        value.trail_p1.clear();
    if (play->m_gameState.m_isDualMode && !Logic::get().hitbox_player_hidden(true)) {
        Hitboxes::get().draw_player_hitbox(node, play, play->m_player2, width, value.fill_hitboxes, nullptr);
    }
    else if (Logic::get().hitbox_player_hidden(true)) {
        value.trail_p2.clear();
    }
    Hitboxes::get().draw_clicks(node, play, width, value.fill_hitboxes);
}
