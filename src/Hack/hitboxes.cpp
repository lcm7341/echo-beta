// draws hitboxes for players & objects, the trail of where the player has been & marks where clicks happened
// it reads the same collision lists gd checks, so only what can actually be hit is shown
// outline only collects every shape & hides the edges that fall inside another shape
#include <set>
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
    int trail_length_frames();
    int current_trail_frame();
    void trim_trail(std::deque<HitboxTrailPoint>& trail, GJBaseGameLayer* play);
};

#define HITBOX_NODE_TAG 60422
#define FROZEN_TRAIL_NODE_TAG 60423
std::vector<float> g_frozen_trail_key;

static CCDrawNode* g_hitbox_node = nullptr;

CCDrawNode* find_hitbox_node(GJBaseGameLayer* play) {
    auto* parent = g_hitbox_node ? g_hitbox_node->getParent() : nullptr;
    if (!parent || !play) return nullptr;
    if (parent == play || parent == play->m_objectLayer || parent == play->m_uiLayer)
        return g_hitbox_node;
    return nullptr;
}

static void sync_to_object_layer(CCNode* node, CCNode* object_layer) {
    if (!node || !object_layer || !node->getParent()) return;
    node->setAdditionalTransform(CCAffineTransformConcat(
        object_layer->nodeToWorldTransform(),
        node->getParent()->worldToNodeTransform()));
}

static int overlay_z_order(GJBaseGameLayer* play) {
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
            g_hitbox_node = node;
            return node;
        }
        CC_SAFE_DELETE(node);
        return nullptr;
    }

    ~EchoHitboxNode() override {
        if (g_hitbox_node == this) g_hitbox_node = nullptr;
    }

    void visit() override {
        if (auto* play = GJBaseGameLayer::get(); play && play->m_objectLayer) {
            sync_to_object_layer(this, play->m_objectLayer);
        }
        CCDrawNode::visit();
    }
};

void Logic::attach_overlay(CCNode* node, GJBaseGameLayer* play) {
    if (!node || !play || !play->m_objectLayer) return;
    // goes on the game layer NOT shaderlayer oops
    int z = overlay_z_order(play);
    if (node->getParent() != play) {
        node->removeFromParentAndCleanup(false);
        play->addChild(node, z, node->getTag());
    }
    else if (node->getZOrder() != z) node->setZOrder(z);
    node->setPosition({ 0.f, 0.f });
    node->setAnchorPoint({ 0.f, 0.f });
    node->setScale(1.f);
    node->setRotation(0.f);
    node->setSkewX(0.f);
    node->setSkewY(0.f);
    sync_to_object_layer(node, play->m_objectLayer);
}

void Logic::reattach_overlays(GJBaseGameLayer* play) {
    if (!play || !play->m_objectLayer) return;
    if (auto* node = find_hitbox_node(play)) attach_overlay(node, play);
    if (auto* live = Logic::as_play(play)) Trajectory::get().sync_seat(live);
}

struct ClickHitbox {
    CCRect outer;
    CCRect inner;
    bool rotated = false;
    bool down = true;
    std::array<CCPoint, 4> corners{};
    CCPoint center{};
    float radius = 0.f;
    int frame = 0;
};

std::deque<ClickHitbox> g_click_hitboxes;

CCRect visible_layer_rect(CCNode* layer);

struct PendingClick {
    bool player2 = false;
    bool down = true;
};

std::vector<PendingClick> g_pending_clicks;

float collision_radius(GameObject* object) {
    if (!object || object->m_objectRadius <= 0.f) return 0.f;
    float radius = object->m_objectRadius;
    float sx = std::abs(object->m_scaleX);
    float sy = std::abs(object->m_scaleY);
    if (sx != 1.f || sy != 1.f) radius *= std::max(sx, sy);
    return radius;
}

// drawing a hitbox fucks with cached
class HitboxStateGuard {
    static const size_t OBJECT_BYTES_FROM = 0x2e0, OBJECT_BYTES_TO = 0x370, BOX_BYTES_FROM = 0x140, BOX_BYTES_TO = 0x1b8;
    GameObject* m_object;
    std::array<unsigned char, OBJECT_BYTES_TO - OBJECT_BYTES_FROM> m_objectBytes{};
    OBB2D* m_originalBox;
    std::array<unsigned char, BOX_BYTES_TO - BOX_BYTES_FROM> m_boxBytes{};
    // sits before the span. gd works the offset out ONCE & keeps it (the deadlocked teleport portal thing)
    bool m_offsetCalculated;
    CCPoint m_offset;

public:
    explicit HitboxStateGuard(GameObject* object) : m_object(object), m_originalBox(object->m_orientedBox),
        m_offsetCalculated(object->m_boxOffsetCalculated), m_offset(object->m_boxOffset) {
        auto* bytes = reinterpret_cast<unsigned char*>(object);
        std::memcpy(m_objectBytes.data(), bytes + OBJECT_BYTES_FROM, m_objectBytes.size());
        if (m_originalBox)
            std::memcpy(m_boxBytes.data(), reinterpret_cast<unsigned char*>(m_originalBox) + BOX_BYTES_FROM, m_boxBytes.size());
    }

    ~HitboxStateGuard() {
        auto* created = m_object->m_orientedBox;
        if (created && created != m_originalBox) created->release();
        std::memcpy(reinterpret_cast<unsigned char*>(m_object) + OBJECT_BYTES_FROM, m_objectBytes.data(), m_objectBytes.size());
        m_object->m_boxOffsetCalculated = m_offsetCalculated;
        m_object->m_boxOffset = m_offset;
        if (m_originalBox)
            std::memcpy(reinterpret_cast<unsigned char*>(m_originalBox) + BOX_BYTES_FROM, m_boxBytes.data(), m_boxBytes.size());
    }

    HitboxStateGuard(HitboxStateGuard const&) = delete;
    HitboxStateGuard& operator=(HitboxStateGuard const&) = delete;
};

OBB2D* live_oriented_box(GameObject* object) {
    if (!object) return nullptr;
    bool editor = LevelEditorLayer::get() && !PlayLayer::get();
    float saved_start = object->m_startRotationX;
    if (editor) object->m_startRotationX = object->getRotation() - object->m_rotationXOffset;
    object->m_isOrientedBoxDirty = true;
    object->m_isObjectRectDirty = true;
    object->updateOrientedBox();
    if (editor) object->m_startRotationX = saved_start;
    if (auto* box = object->m_orientedBox) return box;
    return object->getOrientedBox();
}

bool rotation_is_right_angle(GameObject* object) {
    return std::fmod(object->getRotationX(), 90.f) == 0.f && std::fmod(object->getRotationY(), 90.f) == 0.f;
}

void refresh_object_hitbox(GameObject* object) {
    if (!object) return;
    // objs with a box offset (teleport portals) keep the box gd already has, a redo moves it & that is NOT what the player hits
    if (object->m_customBoxOffset.x != 0.f || object->m_customBoxOffset.y != 0.f) return;
    // move triggers leave the old box cached :/
    object->m_isOrientedBoxDirty = true;
    object->m_isObjectRectDirty = true;
}

CCRect Logic::object_hitbox(GameObject* object) {
    if (!object)
        return {};
    HitboxStateGuard preserve(object);
    refresh_object_hitbox(object);
    return object->getObjectRect();
}

void Logic::note_click_hitbox(GJBaseGameLayer* play, bool player1, bool down) {
    if (!play || Logic::get().predicting) return;
    if (!Logic::get().show_hitboxes || !Logic::get().hitbox_click) return;
    g_pending_clicks.push_back({ !player1, down });
    if (!play->m_gameState.m_isDualMode) return;
    g_pending_clicks.push_back({ player1, down });
}

void Logic::capture_pending_click(PlayerObject* player) {
    auto* play = GJBaseGameLayer::get();
    if (!play || !player || Logic::get().is_traj_player(player)) return;
    bool player2 = player == play->m_player2;
    std::erase_if(g_pending_clicks, [&](PendingClick const& click) {
        if (click.player2 != player2) return false;
        Hitboxes::get().capture_click(play, player, click.down);
        return true;
    });
}

void Logic::flush_click_hitboxes(GJBaseGameLayer* play) {
    if (!play) {
        g_pending_clicks.clear();
        return;
    }
    auto const& logic = Logic::get();
    size_t from = g_pending_clicks.size() > 400 ? g_pending_clicks.size() - 400 : 0;
    for (auto const& click : std::span(g_pending_clicks).subspan(from)) {
        auto* player = click.player2 ? play->m_player2 : play->m_player1;
        if ((click.player2 && Logic::get().hitbox_player_hidden(true)) || (!click.player2 && Logic::get().hitbox_player_hidden(false)))
            continue;
        Hitboxes::get().capture_click(play, player, click.down);
    }
    g_pending_clicks.clear();
}

void Logic::clear_click_hitbox() {
    g_pending_clicks.clear();
    Hitboxes::get().clear_clicks();
}

void Hitboxes::capture_click(GJBaseGameLayer*, PlayerObject* player, bool down) {
    auto const& logic = Logic::get();
    if (!player || !logic.show_hitboxes || !logic.hitbox_click || Logic::get().is_traj_player(player))
        return;
    ClickHitbox box;
    box.down = down;
    box.frame = Hitboxes::get().current_trail_frame();
    box.outer = player->getObjectRect();
    box.inner = player->getObjectRect(0.3f, 0.3f);
    box.center = CCPoint{ box.outer.getMidX(), box.outer.getMidY() };
    box.radius = std::abs(box.outer.size.width) * 0.5f;
    if (auto* oriented = player->getOrientedBox()) {
        box.corners = oriented->m_corners;
        box.rotated = true;
    }
    g_click_hitboxes.push_back(box);
    while (g_click_hitboxes.size() > 4096)
        g_click_hitboxes.pop_front();
}

void Hitboxes::clear_clicks() {
    g_click_hitboxes.clear();
}

void Hitboxes::draw_clicks(CCDrawNode* node, GJBaseGameLayer* play, float width, bool fill) {
    auto const& logic = Logic::get();
    if (!logic.hitbox_click) {
        g_click_hitboxes.clear();
        return;
    }
    if (!(play && LevelEditorLayer::get() == play)) {
        int now = Hitboxes::get().current_trail_frame();
        CCRect view = play && play->m_objectLayer ? visible_layer_rect(play->m_objectLayer) : CCRect{};
        bool view_known = std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f;
        int limit = Hitboxes::get().trail_length_frames();
        std::erase_if(g_click_hitboxes, [&](ClickHitbox const& box) {
            if (box.frame > now) return true;
            return view_known ? !view.intersectsRect(box.outer) : now - box.frame >= limit;
        });
    }
    for (auto const& box : g_click_hitboxes) {
        float const* color = box.down ? logic.hitbox_click_color : logic.hitbox_release_color;
        auto tint = HitboxColor::fill_color(color, fill, logic.hitbox_fill_opacity);
        auto line = HitboxColor::line_color(color);
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
    if (!player || Logic::get().is_traj_player(player)) return;
    auto const& logic = Logic::get();
    auto rect = player->getObjectRect();
    auto inner_rect = player->getObjectRect(0.3f, 0.3f);
    CCPoint center{ rect.getMidX(), rect.getMidY() };
    float const* outer_color = tint ? tint : logic.hitbox_outer_color;
    float const* rotated_color = tint ? tint : logic.hitbox_rotated_color;
    float const* inner_color = tint ? tint : logic.hitbox_inner_color;
    float const* circle_color = tint ? tint : logic.hitbox_circle_color;
    if (logic.hitbox_rotated) {
        if (auto* box = player->getOrientedBox()) {
            std::array<CCPoint, 4> corners = box->m_corners;
            node->drawPolygon(corners.data(), 4, HitboxColor::fill_color(rotated_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(rotated_color), BorderAlignment::Inside);
        }
    }
    if (logic.hitbox_player) {
        node->drawRect(rect, HitboxColor::fill_color(outer_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(outer_color), BorderAlignment::Inside);
    }
    if (logic.hitbox_inner) {
        node->drawRect(inner_rect, HitboxColor::fill_color(inner_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(inner_color), BorderAlignment::Inside);
    }
    float radius = std::abs(rect.size.width) * 0.5f;
    float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
    if (logic.hitbox_circle && radius > 0.f) {
        unsigned segments = (unsigned)(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
        node->drawCircle(center, radius, { 0.f, 0.f, 0.f, 0.f }, width, HitboxColor::line_color(circle_color), std::max(segments, 8u));
    }
}

struct OutlineShape {
    unsigned first_point = 0, point_count = 0;
    bool axis_aligned = false, is_circle = false;
    CCPoint center;
    float radius = 0.f;
    ccColor4F fill;
    ccColor4F line;
    CCRect bounds;
    int outline_group = 0;
};

struct OutlineBuilder {
    bool collecting = false;
    int outline_group = 0;
    std::vector<OutlineShape> shapes;
    std::vector<CCPoint> points;
    std::vector<std::pair<unsigned long long, unsigned>> cells;
    std::vector<unsigned> seen, shapes_near_edge;
    std::vector<float> cuts;
    std::vector<CCPoint> circle_points;
    std::vector<std::pair<float, float>> covered;
    std::vector<unsigned> shapes_around;
};

static OutlineBuilder g_outlines;

static void collect_shape(CCPoint const* points, unsigned count, ccColor4F const& fill, ccColor4F const& line) {
    if (count < 3) return;
    auto& outlines = g_outlines;
    OutlineShape shape;
    shape.first_point = (unsigned)outlines.points.size();
    shape.point_count = count;
    outlines.points.insert(outlines.points.end(), points, points + count);
    CCPoint* shape_points = outlines.points.data() + shape.first_point;
    float area = 0.f;
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (unsigned i = 0; i < count; ++i) {
        auto const& p = shape_points[i];
        auto const& q = shape_points[(i + 1) % count];
        area += p.x * q.y - q.x * p.y;
        min_x = std::min(min_x, p.x);
        min_y = std::min(min_y, p.y);
        max_x = std::max(max_x, p.x);
        max_y = std::max(max_y, p.y);
    }
    if (area < 0.f) std::reverse(shape_points, shape_points + count);
    shape.bounds = { min_x, min_y, max_x - min_x, max_y - min_y };
    if (count == 4) {
        shape.axis_aligned = true;
        for (unsigned i = 0; i < 4 && shape.axis_aligned; ++i) {
            auto const& p = shape_points[i];
            auto const& q = shape_points[(i + 1) % 4];
            shape.axis_aligned = p.x == q.x || p.y == q.y;
        }
    }
    shape.fill = fill;
    shape.line = line;
    shape.outline_group = outlines.outline_group;
    outlines.shapes.push_back(shape);
}

static void emit_polygon(CCDrawNode* node, CCPoint* points, unsigned count, ccColor4F const& fill, float width, ccColor4F const& line) {
    if (g_outlines.collecting) return collect_shape(points, count, fill, line);
    node->drawPolygon(points, count, fill, width, line, BorderAlignment::Inside);
}

static void emit_rect(CCDrawNode* node, CCRect const& rect, ccColor4F const& fill, float width, ccColor4F const& line) {
    if (g_outlines.collecting) {
        std::array<CCPoint, 4> corners{ rect.origin, ccp(rect.getMaxX(), rect.getMinY()), ccp(rect.getMaxX(), rect.getMaxY()), ccp(rect.getMinX(), rect.getMaxY()) };
        return collect_shape(corners.data(), 4, fill, line);
    }
    node->drawRect(rect, fill, width, line, BorderAlignment::Inside);
}

static void emit_circle(CCDrawNode* node, CCPoint const& center, float radius, ccColor4F const& fill, float width, ccColor4F const& line, unsigned segments) {
    if (g_outlines.collecting) {
        auto& circle_points = g_outlines.circle_points;
        circle_points.resize(std::max(segments, 8u));
        for (std::size_t i = 0; i < circle_points.size(); ++i) {
            float angle = (float)(i) * 2.f * (float)(M_PI) / (float)(circle_points.size());
            circle_points[i] = center + CCPoint{ std::cos(angle), std::sin(angle) } * radius;
        }
        collect_shape(circle_points.data(), (unsigned)circle_points.size(), fill, line);
        if (!g_outlines.shapes.empty()) {
            auto& shape = g_outlines.shapes.back();
            shape.is_circle = true;
            shape.center = center;
            shape.radius = radius;
        }
        return;
    }
    node->drawCircle(center, radius, fill, width, line, segments);
}

static bool inside_shape(OutlineShape const& shape, CCPoint p) {
    if (!shape.bounds.containsPoint(p)) return false;
    CCPoint const* shape_points = g_outlines.points.data() + shape.first_point;
    for (unsigned i = 0; i < shape.point_count; ++i) {
        auto const& a = shape_points[i];
        auto const& b = shape_points[(i + 1) % shape.point_count];
        if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) < -1e-4f) return false;
    }
    return true;
}

static void flush_outlines(CCDrawNode* node, float width) {
    auto& outlines = g_outlines;
    outlines.collecting = false;
    auto& shapes = outlines.shapes;
    if (shapes.empty()) {
        outlines.points.clear();
        return;
    }
    static ccColor4F const none{ 0.f, 0.f, 0.f, 0.f };
    for (auto& shape : shapes) {
        if (shape.fill.a > 0.f)
            node->drawPolygon(outlines.points.data() + shape.first_point, shape.point_count, shape.fill, 0.f, none);
    }
    const float CELL_SIZE = 60.f;
    auto outline_cell_key = [](int outline_group, int x, int y) {
        return ((unsigned long long)((unsigned)outline_group & 255) << 56) |
               ((unsigned long long)((unsigned)x & 268435455) << 28) | ((unsigned)y & 268435455);
    };
    auto& cells = outlines.cells;
    cells.clear();
    for (unsigned i = 0; i < shapes.size(); ++i) {
        auto const& b = shapes[i].bounds;
        int x0 = (int)(std::floor(b.getMinX() / CELL_SIZE)), x1 = (int)(std::floor(b.getMaxX() / CELL_SIZE));
        int y0 = (int)(std::floor(b.getMinY() / CELL_SIZE)), y1 = (int)(std::floor(b.getMaxY() / CELL_SIZE));
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                cells.emplace_back(outline_cell_key(shapes[i].outline_group, x, y), i);
    }
    std::sort(cells.begin(), cells.end());
    auto& shapes_near_edge = outlines.shapes_near_edge;
    auto& shapes_around = outlines.shapes_around;
    auto& seen = outlines.seen;
    seen.assign(shapes.size(), UINT_MAX);
    auto& cuts = outlines.cuts;
    float depth = width;
    float probe = std::max(0.05f, width * 0.25f);
    for (unsigned i = 0; i < shapes.size(); ++i) {
        auto const& shape = shapes[i];
        shapes_around.clear();
        {
            auto const& own_bounds = shape.bounds;
            float min_x = own_bounds.getMinX() - probe, max_x = own_bounds.getMaxX() + probe, min_y = own_bounds.getMinY() - probe, max_y = own_bounds.getMaxY() + probe;
            for (int x = (int)(std::floor(min_x / CELL_SIZE)); x <= (int)(std::floor(max_x / CELL_SIZE)); ++x)
                for (int y = (int)(std::floor(min_y / CELL_SIZE)); y <= (int)(std::floor(max_y / CELL_SIZE)); ++y) {
                    auto key = outline_cell_key(shape.outline_group, x, y);
                    auto it = std::lower_bound(cells.begin(), cells.end(), std::pair<unsigned long long, unsigned>{ key, 0 });
                    for (; it != cells.end() && it->first == key; ++it) {
                        auto j = it->second;
                        if (j == i || seen[j] == i) continue;
                        seen[j] = i;
                        auto const& other_bounds = shapes[j].bounds;
                        if (other_bounds.getMaxX() < min_x || other_bounds.getMinX() > max_x || other_bounds.getMaxY() < min_y || other_bounds.getMinY() > max_y)
                            continue;
                        shapes_around.push_back(j);
                    }
                }
        }
        if (shape.is_circle && std::ranges::all_of(shapes_around, [&](unsigned j) { return shapes[j].is_circle; })) {
            const float TAU = 6.28318530718f;
            auto& covered = outlines.covered;
            covered.clear();
            bool hidden = false;
            for (auto j : shapes_around) {
                auto const& other = shapes[j];
                float dx = other.center.x - shape.center.x, dy = other.center.y - shape.center.y;
                float dist = std::hypot(dx, dy);
                if (dist + shape.radius <= other.radius) {
                    hidden = true;
                    break;
                }
                if (dist >= shape.radius + other.radius || dist + other.radius <= shape.radius || dist < 1e-5f)
                    continue;
                float c = (shape.radius * shape.radius + dist * dist - other.radius * other.radius) / (2.f * shape.radius * dist);
                float half = std::acos(std::clamp(c, -1.f, 1.f));
                float mid = std::atan2(dy, dx);
                float from = mid - half, to = mid + half;
                while (from < 0.f) {
                    from += TAU;
                    to += TAU;
                }
                while (from >= TAU) {
                    from -= TAU;
                    to -= TAU;
                }
                if (to > TAU) {
                    covered.emplace_back(from, TAU);
                    covered.emplace_back(0.f, to - TAU);
                }
                else covered.emplace_back(from, to);
            }
            if (hidden) continue;
            std::sort(covered.begin(), covered.end());
            float step = TAU / (float)(std::max<unsigned>(shape.point_count, 8));
            float inner_radius = std::max(shape.radius - depth, 0.f);
            auto draw_uncovered_arc = [&](float from, float to) {
                if (to - from < 1e-4f) return;
                int parts = std::max(1, (int)std::ceil((to - from) / step));
                CCPoint last = shape.center + CCPoint{ std::cos(from), std::sin(from) } * inner_radius;
                for (int k = 1; k <= parts; ++k) {
                    float angle = from + (to - from) * (float)k / (float)parts;
                    CCPoint next = shape.center + CCPoint{ std::cos(angle), std::sin(angle) } * inner_radius;
                    node->drawSegment(last, next, depth, shape.line);
                    last = next;
                }
            };
            float cursor = 0.f;
            for (auto const& [from, to] : covered) {
                if (from > cursor) draw_uncovered_arc(cursor, from);
                cursor = std::max(cursor, to);
                if (cursor >= TAU) break;
            }
            if (cursor < TAU) draw_uncovered_arc(cursor, TAU);
            continue;
        }
        CCPoint const* shape_points = outlines.points.data() + shape.first_point;
        for (unsigned edge = 0; edge < shape.point_count; ++edge) {
            auto a = shape_points[edge];
            auto b = shape_points[(edge + 1) % shape.point_count];
            auto d = b - a;
            float len = std::hypot(d.x, d.y);
            if (len < 1e-4f) continue;
            CCPoint outward{ d.y / len, -d.x / len };
            shapes_near_edge.clear();
            float min_x = std::min(a.x, b.x) - probe, max_x = std::max(a.x, b.x) + probe;
            float min_y = std::min(a.y, b.y) - probe, max_y = std::max(a.y, b.y) + probe;
            for (auto j : shapes_around) {
                auto const& other_bounds = shapes[j].bounds;
                if (other_bounds.getMaxX() < min_x || other_bounds.getMinX() > max_x || other_bounds.getMaxY() < min_y || other_bounds.getMinY() > max_y)
                    continue;
                shapes_near_edge.push_back(j);
            }
            bool axis_edge = shape.axis_aligned && std::ranges::all_of(shapes_near_edge, [&](unsigned j) {
                return shapes[j].axis_aligned;
            });
            if (axis_edge) {
                bool horizontal = a.y == b.y;
                float lo = horizontal ? std::min(a.x, b.x) : std::min(a.y, b.y);
                float hi = horizontal ? std::max(a.x, b.x) : std::max(a.y, b.y);
                CCPoint test = a + outward * probe;
                auto& covered = outlines.covered;
                covered.clear();
                for (auto j : shapes_near_edge) {
                    auto const& other_bounds = shapes[j].bounds;
                    if (horizontal) {
                        if (test.y < other_bounds.getMinY() || test.y > other_bounds.getMaxY()) continue;
                        float from = std::max(lo, other_bounds.getMinX()), to = std::min(hi, other_bounds.getMaxX());
                        if (to > from) covered.emplace_back(from, to);
                    }
                    else {
                        if (test.x < other_bounds.getMinX() || test.x > other_bounds.getMaxX()) continue;
                        float from = std::max(lo, other_bounds.getMinY()), to = std::min(hi, other_bounds.getMaxY());
                        if (to > from) covered.emplace_back(from, to);
                    }
                }
                std::sort(covered.begin(), covered.end());
                float span = hi - lo;
                auto edge_fraction = [&](float v) {
                    float t = (v - lo) / span;
                    return (horizontal ? (b.x >= a.x) : (b.y >= a.y)) ? t : 1.f - t;
                };
                auto draw_uncovered_edge_piece = [&](float from, float to) {
                    if (to - from < 0.01f) return;
                    float t0 = edge_fraction(from), t1 = edge_fraction(to);
                    if (t0 > t1) std::swap(t0, t1);
                    float trim = std::min(depth / len, (t1 - t0) * 0.5f);
                    node->drawSegment(a + d * (t0 + trim) + outward * -depth, a + d * (t1 - trim) + outward * -depth, depth, shape.line);
                };
                float cursor = lo;
                for (auto const& [from, to] : covered) {
                    if (from > cursor) draw_uncovered_edge_piece(cursor, from);
                    cursor = std::max(cursor, to);
                    if (cursor >= hi) break;
                }
                if (cursor < hi) draw_uncovered_edge_piece(cursor, hi);
                continue;
            }
            cuts.clear();
            cuts.push_back(0.f);
            cuts.push_back(1.f);
            for (auto j : shapes_near_edge) {
                CCPoint const* other_points = outlines.points.data() + shapes[j].first_point;
                unsigned other_count = shapes[j].point_count;
                for (unsigned k = 0; k < other_count; ++k) {
                    auto c = other_points[k];
                    auto f = other_points[(k + 1) % other_count] - c;
                    float denom = d.x * f.y - d.y * f.x;
                    if (std::abs(denom) < 1e-6f) continue;
                    auto ac = c - a;
                    float t = (ac.x * f.y - ac.y * f.x) / denom;
                    float u = (ac.x * d.y - ac.y * d.x) / denom;
                    if (t > 0.f && t < 1.f && u >= 0.f && u <= 1.f) cuts.push_back(t);
                }
            }
            if (cuts.size() > 2) std::sort(cuts.begin(), cuts.end());
            CCPoint inward = outward * -depth;
            for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
                float t0 = cuts[c], t1 = cuts[c + 1];
                if ((t1 - t0) * len < 0.01f) continue;
                if (!shapes_near_edge.empty()) {
                    auto test = a + d * ((t0 + t1) * 0.5f) + outward * probe;
                    bool covered = std::ranges::any_of(shapes_near_edge, [&](unsigned j) {
                        return inside_shape(shapes[j], test);
                    });
                    if (covered) continue;
                }
                float trim = std::min(depth / len, (t1 - t0) * 0.5f);
                node->drawSegment(a + d * (t0 + trim) + inward, a + d * (t1 - trim) + inward, depth, shape.line);
            }
        }
    }
    shapes.clear();
    outlines.points.clear();
}

void Hitboxes::draw_object_hitbox(CCDrawNode* node, GJBaseGameLayer* play, GameObject* object, float width, bool fill) {
    if (!object || object->m_isGroupDisabled) return;
    // DONT check m_isActivated its late on some lvls fsr
    if (object->m_objectType == GameObjectType::Decoration) return;
    if (object == play->m_player1 || object == play->m_player2 || Logic::get().is_traj_player(object))
        return;
    if (object == play->m_player1CollisionBlock || object == play->m_player2CollisionBlock) return;
    if (auto* live = Logic::as_play(play); live && object == live->m_anticheatSpike) return;
    auto const& logic = Logic::get();
    HitboxStateGuard preserve(object);
    refresh_object_hitbox(object);
    bool use_plain_rect = !Logic::as_play(play) && rotation_is_right_angle(object);

    switch (object->m_objectType) {
    case GameObjectType::Solid:
    case GameObjectType::Slope:
    case GameObjectType::Hazard:
    case GameObjectType::AnimatedHazard:
        g_outlines.outline_group = 0;
        break;
    default:
        g_outlines.outline_group = 1;
        break;
    }
    if (object->m_objectType == GameObjectType::Solid && object->m_isPassable) g_outlines.outline_group = 2;
    struct RestoreCollecting {
        bool was_collecting = g_outlines.collecting;
        ~RestoreCollecting() { g_outlines.collecting = was_collecting; }
    } restore_collecting;
    if (g_outlines.outline_group == 1) g_outlines.collecting = false;
    auto paint_rect = [&](float const* color) {
        emit_rect(node, object->getObjectRect(), HitboxColor::fill_color(color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(color));
    };

    switch (object->m_objectType) {
    case GameObjectType::Solid: {
        if (object->m_isPassable) {
            if (logic.hitbox_passable) paint_rect(logic.hitbox_passable_color);
            break;
        }
        if (!logic.hitbox_solid) break;
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
        if (object->m_slopeIsHazard ? !logic.hitbox_hazard : !logic.hitbox_slope) break;
        if (object->m_slopeIsHazard) {
            int dir = object->m_slopeDirection;
            bool ceiling = dir >= 0 && dir <= 6 && ((106 >> dir) & 1);
            float shift = (ceiling ? -4.f : 4.f);
            int corner = 0;
            for (int k = 0; k < 3; ++k) {
                auto const& a = verts[(k + 1) % 3];
                auto const& b = verts[(k + 2) % 3];
                auto const& c = verts[k];
                if ((c.x == a.x && c.y == b.y) || (c.x == b.x && c.y == a.y)) corner = k;
            }
            std::array<CCPoint, 4> shape;
            int corner_count = 0;
            for (int k = 0; k < 3; ++k) {
                auto const& p = verts[(corner + k) % 3];
                if (k == 0) {
                    shape[corner_count++] = p;
                    continue;
                }
                CCPoint moved{ p.x, p.y + shift };
                bool level = p.y == verts[corner].y;
                if (level && k == 1) {
                    shape[corner_count++] = p;
                    shape[corner_count++] = moved;
                }
                else if (level) {
                    shape[corner_count++] = moved;
                    shape[corner_count++] = p;
                }
                else {
                    shape[corner_count++] = moved;
                }
            }
            emit_polygon(node, shape.data(), corner_count, HitboxColor::fill_color(slope_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(slope_color));
            break;
        }
        emit_polygon(node, verts.data(), 3, HitboxColor::fill_color(logic.hitbox_slope_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(logic.hitbox_slope_color));
        break;
    }
    case GameObjectType::Hazard:
    case GameObjectType::AnimatedHazard: {
        if (!logic.hitbox_hazard) break;
        float radius = collision_radius(object);
        float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
        if (radius > 0.f) {
            unsigned segments = (unsigned)(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
            emit_circle(node, object->getPosition(), radius, HitboxColor::fill_color(logic.hitbox_hazard_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(logic.hitbox_hazard_color), std::max(segments, 8u));
        }
        else if (auto* box = use_plain_rect ? nullptr : live_oriented_box(object)) {
            std::array<CCPoint, 4> corners = box->m_corners;
            emit_polygon(node, corners.data(), 4, HitboxColor::fill_color(logic.hitbox_hazard_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(logic.hitbox_hazard_color));
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
                if (used) return;
            }
        }
        if (!logic.hitbox_interact) break;
        float radius = collision_radius(object);
        bool fix_radius = play->m_levelSettings && play->m_levelSettings->m_fixRadiusCollision;
        bool use_box = radius <= 0.f || (object->m_shouldUseOuterOb && !fix_radius);
        if (radius > 0.f) {
            float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
            unsigned segments = (unsigned)(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
            emit_circle(node, object->getPosition(), radius, HitboxColor::fill_color(logic.hitbox_interact_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(logic.hitbox_interact_color), std::max(segments, 8u));
        }
        if (use_box) {
            if (auto* box = use_plain_rect ? nullptr : live_oriented_box(object)) {
                std::array<CCPoint, 4> corners = box->m_corners;
                emit_polygon(node, corners.data(), 4, HitboxColor::fill_color(logic.hitbox_interact_color, fill, logic.hitbox_fill_opacity), width, HitboxColor::line_color(logic.hitbox_interact_color));
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
    float min_x = 0.f, min_y = 0.f;
    float max_x = 0.f, max_y = 0.f;
    for (int i = 0; i < 4; ++i) {
        CCPoint local = layer->convertToNodeSpace(corners[i]);
        if (i == 0) {
            min_x = max_x = local.x;
            min_y = max_y = local.y;
            continue;
        }
        min_x = std::min(min_x, local.x);
        min_y = std::min(min_y, local.y);
        max_x = std::max(max_x, local.x);
        max_y = std::max(max_y, local.y);
    }
    return { min_x, min_y, std::max(max_x - min_x, 0.f), std::max(max_y - min_y, 0.f) };
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

struct LastTrailSize {
    float width = 0.f, height = 0.f;
};

CCRect trail_rect(PlayerObject* player, LastTrailSize& last_size) {
    CCRect raw = upright_rect(player->getObjectRect());
    bool size_known = last_size.width >= 2.f && last_size.height >= 2.f;
    bool finite = std::isfinite(raw.origin.x) && std::isfinite(raw.origin.y) && std::isfinite(raw.size.width) && std::isfinite(raw.size.height);
    bool tiny = !finite || std::abs(player->getScaleX()) < 0.2f || raw.size.width < 2.f || raw.size.height < 2.f;
    bool smear = finite && (raw.size.width > 75.f || raw.size.height > 75.f);
    if (size_known && finite) {
        smear = smear || (raw.size.width > last_size.width * 2.5f && raw.size.width > 45.f) || (raw.size.height > last_size.height * 2.5f && raw.size.height > 45.f);
    }
    if (!tiny && !smear) {
        last_size.width = raw.size.width;
        last_size.height = raw.size.height;
        return raw;
    }
    if (!size_known) {
        last_size.width = std::clamp(finite ? raw.size.width : 30.f, 8.f, 36.f);
        last_size.height = std::clamp(finite ? raw.size.height : 30.f, 8.f, 36.f);
    }
    CCPoint pos = player->getPosition();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y))
        pos = CCPoint{ raw.getMidX(), raw.getMidY() };
    CCRect steady_rect(pos.x - last_size.width * 0.5f, pos.y - last_size.height * 0.5f, last_size.width, last_size.height);
    return steady_rect;
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

int Hitboxes::trail_length_frames() {
    auto const& logic = Logic::get();
    int len = logic.trail_length;
    if (len <= 0) len = std::max(1, (int)(std::lround(logic.fps)));
    return std::max(1, len);
}

int Hitboxes::current_trail_frame() {
    auto const& logic = Logic::get();
    return logic.hitbox_trail_visual ? logic.visual_frame : logic.step_frame;
}

void Hitboxes::trim_trail(std::deque<HitboxTrailPoint>& trail, GJBaseGameLayer* play) {
    auto const& logic = Logic::get();
    if (play && LevelEditorLayer::get() == play) return;
    if (logic.hitbox_trail_visual && logic.is_playing() && play && play->m_objectLayer) {
        CCRect view = visible_layer_rect(play->m_objectLayer);
        if (std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f) {
            std::erase_if(trail, [&](HitboxTrailPoint const& point) {
                return !view.intersectsRect(point.rect);
            });
            return;
        }
    }
    int limit = Hitboxes::get().trail_length_frames();
    int now = Hitboxes::get().current_trail_frame();
    while (!trail.empty() && (trail.front().frame > now || now - trail.front().frame >= limit))
        trail.pop_front();
}

void Logic::sample_hitbox_trail(GJBaseGameLayer* play) {
    auto& logic = Logic::get();
    if (!play || !logic.show_hitboxes || !logic.hitbox_trail) return;
    if (auto* live = Logic::as_play(play); live && (live->m_levelEndAnimationStarted || live->m_playerDied))
        return;
    int frame = Hitboxes::get().current_trail_frame();
    static LastTrailSize last_size_p1, last_size_p2;
    if (logic.trail_p1.empty())
        last_size_p1 = {};
    if (logic.trail_p2.empty())
        last_size_p2 = {};
    auto capture = [&](PlayerObject* player, LastTrailSize& last_size, std::deque<HitboxTrailPoint>& trail) {
        HitboxTrailPoint point;
        point.frame = frame;
        point.rect = trail_rect(player, last_size);
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
            float min_x = point.corners[0].x, max_x = min_x;
            float min_y = point.corners[0].y, max_y = min_y;
            for (int i = 1; i < 4; ++i) {
                min_x = std::min(min_x, point.corners[i].x);
                max_x = std::max(max_x, point.corners[i].x);
                min_y = std::min(min_y, point.corners[i].y);
                max_y = std::max(max_y, point.corners[i].y);
            }
            float span_w = max_x - min_x, span_h = max_y - min_y;
            bool huge = span_w > 75.f || span_h > 75.f || (span_w > std::abs(point.rect.size.width) * 2.5f && span_w > 45.f) || (span_h > std::abs(point.rect.size.height) * 2.5f && span_h > 45.f);
            point.rotated = !huge;
        }
        Hitboxes::get().push_trail(trail, point, play);
    };
    if (play->m_player1 && !Logic::get().hitbox_player_hidden(false) && !Logic::get().is_traj_player(play->m_player1))
        capture(play->m_player1, last_size_p1, logic.trail_p1);
    if (play->m_player2 && !Logic::get().hitbox_player_hidden(true) && !Logic::get().is_traj_player(play->m_player2))
        capture(play->m_player2, last_size_p2, logic.trail_p2);
}

void Logic::draw_hitboxes(GJBaseGameLayer* play) {
    auto& logic = Logic::get();
    auto* layer = play->m_objectLayer;
    if (!layer) return;
    auto* node = find_hitbox_node(play);
    if (!logic.show_hitboxes) {
        if (node) node->setVisible(false);
        logic.trail_p1.clear();
        logic.trail_p2.clear();
        return;
    }
    if (!node) {
        node = EchoHitboxNode::create();
        node->setTag(HITBOX_NODE_TAG);
    }
    node->setVisible(true);
    node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
    attach_overlay(node, play);
    node->clear();
    if (logic.hide_p1 && play->m_player1) play->m_player1->setVisible(false);
    if (logic.hide_p2 && play->m_player2) play->m_player2->setVisible(false);
    auto* player = play->m_player1;
    bool level = Logic::as_play(play) != nullptr;
    float view_scale = std::max(play->m_gameState.m_cameraZoom, 0.05f);
    if (!level) view_scale = std::max(std::abs(layer->getScaleX()), 0.05f);
    float width = std::max(logic.hitbox_thickness, 0.f) / view_scale;
    Hitboxes::get().trim_trail(logic.trail_p1, play);
    Hitboxes::get().trim_trail(logic.trail_p2, play);
    if (!logic.hitbox_trail) {
        logic.trail_p1.clear();
        logic.trail_p2.clear();
    }
    auto grid_units = [](float coordinate) {
        return (int)(std::lround(coordinate * 20.f));
    };

    struct DrawnSpot {
        int x, y, w, h;
        auto operator<=>(DrawnSpot const& other) const = default;
    };

    struct DrawnCorners {
        std::array<int, 8> point{};
        auto operator<=>(DrawnCorners const& other) const = default;
    };

    auto quantize = [&](CCRect const& rect) {
        return DrawnSpot{ grid_units(rect.origin.x), grid_units(rect.origin.y), grid_units(rect.size.width), grid_units(rect.size.height) };
    };
    auto quantize_corners = [&](std::array<CCPoint, 4> const& corners) {
        DrawnCorners key;
        for (int i = 0; i < 4; ++i) {
            key.point[i * 2] = grid_units(corners[i].x);
            key.point[i * 2 + 1] = grid_units(corners[i].y);
        }
        return key;
    };
    auto quantize_circle = [&](CCPoint center, float radius) {
        return DrawnSpot{ grid_units(center.x), grid_units(center.y), grid_units(radius), 0 };
    };
    static std::set<DrawnSpot> drawn_boxes;
    static std::set<DrawnCorners> drawn_rotated;
    static std::set<DrawnSpot> drawn_inner, drawn_circles;
    drawn_boxes.clear();
    drawn_rotated.clear();
    drawn_inner.clear();
    drawn_circles.clear();
    auto mark_live_player_drawn = [&](PlayerObject* body) {
        if (!body || Logic::get().is_traj_player(body)) return;
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
    if (!Logic::get().hitbox_player_hidden(false)) mark_live_player_drawn(play->m_player1);
    if (play->m_gameState.m_isDualMode && !Logic::get().hitbox_player_hidden(true))
        mark_live_player_drawn(play->m_player2);
    bool fill = logic.fill_hitboxes;
    float trail_opacity = std::clamp(logic.hitbox_trail_opacity, 0.f, 1.f);
    auto trail_line = [&](float const* color) {
        auto line = HitboxColor::line_color(color);
        line.a *= trail_opacity;
        return line;
    };
    auto trail_fill = [&](float const* color) {
        auto painted = HitboxColor::fill_color(color, fill, logic.hitbox_fill_opacity);
        painted.a *= trail_opacity;
        return painted;
    };
    auto for_each_trail = [&](auto&& draw) {
        if (!logic.hitbox_trail) return;
        draw(logic.trail_p1);
        draw(logic.trail_p2);
    };
    g_outlines.collecting = logic.hitbox_outline_only;
    g_outlines.shapes.clear();
    struct LastTrailShape {
        CCPoint a, b;
        bool has = false;
    } last_shape;
    auto repeats_last_shape = [&](CCPoint a, CCPoint b) {
        if (!g_outlines.collecting) return false;
        bool same = last_shape.has && std::abs(a.x - last_shape.a.x) < width && std::abs(a.y - last_shape.a.y) < width && std::abs(b.x - last_shape.b.x) < width &&
                    std::abs(b.y - last_shape.b.y) < width;
        if (same) return true;
        last_shape = { a, b, true };
        return false;
    };
    CCRect trail_cull_rect = visible_layer_rect(layer);
    bool cull_trail_to_view = !level && std::isfinite(trail_cull_rect.origin.x) && std::isfinite(trail_cull_rect.origin.y) && trail_cull_rect.size.width > 1.f;
    if (cull_trail_to_view) {
        trail_cull_rect.origin.x -= 150.f;
        trail_cull_rect.origin.y -= 150.f;
        trail_cull_rect.size.width += 300.f;
        trail_cull_rect.size.height += 300.f;
    }
    auto add_trail_shapes = [&](bool cull) {
        auto near_screen = [&](CCRect const& rect) {
            return !cull || trail_cull_rect.intersectsRect(rect);
        };
        if (logic.hitbox_player) {
            g_outlines.outline_group = 10;
            for_each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
                last_shape.has = false;
                for (auto const& past_point : trail) {
                    if (!near_screen(past_point.rect)) continue;
                    if (repeats_last_shape(past_point.rect.origin, ccp(past_point.rect.getMaxX(), past_point.rect.getMaxY())) || !drawn_boxes.insert(quantize(past_point.rect)).second)
                        continue;
                    emit_rect(node, past_point.rect, trail_fill(logic.hitbox_outer_color), width, trail_line(logic.hitbox_outer_color));
                }
            });
        }
        if (logic.hitbox_rotated) {
            g_outlines.outline_group = 11;
            for_each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
                last_shape.has = false;
                for (auto const& past_point : trail) {
                    if (!near_screen(past_point.rect)) continue;
                    if (!past_point.rotated || repeats_last_shape(past_point.corners[0], past_point.corners[2]) || !drawn_rotated.insert(quantize_corners(past_point.corners)).second)
                        continue;
                    auto corners = past_point.corners;
                    emit_polygon(node, corners.data(), 4, trail_fill(logic.hitbox_rotated_color), width, trail_line(logic.hitbox_rotated_color));
                }
            });
        }
        if (logic.hitbox_circle) {
            g_outlines.outline_group = 12;
            for_each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
                last_shape.has = false;
                for (auto const& past_point : trail) {
                    if (!near_screen(past_point.rect)) continue;
                    if (!(past_point.radius > 0.f) || repeats_last_shape(past_point.center, ccp(past_point.radius, 0.f)) || !drawn_circles.insert(quantize_circle(past_point.center, past_point.radius)).second)
                        continue;
                    emit_circle(node, past_point.center, past_point.radius, trail_fill(logic.hitbox_circle_color), width, trail_line(logic.hitbox_circle_color), 16);
                }
            });
        }
        if (logic.hitbox_inner) {
            g_outlines.outline_group = 13;
            for_each_trail([&](std::deque<HitboxTrailPoint> const& trail) {
                last_shape.has = false;
                for (auto const& past_point : trail) {
                    if (!near_screen(past_point.rect)) continue;
                    if (repeats_last_shape(past_point.inner.origin, ccp(past_point.inner.getMaxX(), past_point.inner.getMaxY())) || !drawn_inner.insert(quantize(past_point.inner)).second)
                        continue;
                    emit_rect(node, past_point.inner, trail_fill(logic.hitbox_inner_color), width, trail_line(logic.hitbox_inner_color));
                }
            });
        }
    };
    bool trail_is_static = !level && !Logic::get().editor_testing && logic.hitbox_outline_only && logic.hitbox_trail;
    auto* frozen_trail_node = static_cast<CCDrawNode*>(node->getChildByTag(FROZEN_TRAIL_NODE_TAG));
    if (trail_is_static) {
        if (!frozen_trail_node) {
            frozen_trail_node = CCDrawNode::create();
            frozen_trail_node->setTag(FROZEN_TRAIL_NODE_TAG);
            frozen_trail_node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
            node->addChild(frozen_trail_node, -1);
            g_frozen_trail_key.clear();
        }
        frozen_trail_node->setVisible(true);
        std::vector<float> key{ width, (float)(logic.hitbox_player), (float)(logic.hitbox_rotated), (float)(logic.hitbox_circle), (float)(logic.hitbox_inner),
            (float)(logic.fill_hitboxes), logic.hitbox_trail_opacity, logic.hitbox_fill_opacity };
        for (auto const* color : { logic.hitbox_outer_color, logic.hitbox_rotated_color, logic.hitbox_circle_color, logic.hitbox_inner_color })
            key.insert(key.end(), color, color + 4);
        for (auto const* trail : { &logic.trail_p1, &logic.trail_p2 }) {
            key.push_back((float)(trail->size()));
            if (!trail->empty()) {
                key.insert(key.end(), { (float)(trail->front().frame), trail->front().rect.origin.x, trail->front().rect.origin.y });
                key.insert(key.end(), { (float)(trail->back().frame), trail->back().rect.origin.x, trail->back().rect.origin.y });
            }
        }
        if (key != g_frozen_trail_key) {
            g_frozen_trail_key = std::move(key);
            frozen_trail_node->clear();
            g_outlines.shapes.clear();
            g_outlines.collecting = true;
            add_trail_shapes(false);
            flush_outlines(frozen_trail_node, width);
            g_outlines.collecting = logic.hitbox_outline_only;
            g_outlines.shapes.clear();
        }
    }
    else {
        if (frozen_trail_node && frozen_trail_node->isVisible()) {
            frozen_trail_node->setVisible(false);
            frozen_trail_node->clear();
            g_frozen_trail_key.clear();
        }
        add_trail_shapes(cull_trail_to_view);
    }
    float view_x = player ? player->getPositionX() : 0.f;
    float reach = 500.f;
    CCRect view = visible_layer_rect(layer);
    bool view_known = std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f;
    if (view_known) {
        // TODO use camera x for now might fuck with camera trigger but we ball
        view_x = view.getMidX();
        reach = view.size.width * 0.5f + 200.f;
    }
    else if (!level) {
        auto win = CCDirector::sharedDirector()->getWinSize();
        view_x = (-layer->getPositionX() + win.width * 0.5f) / view_scale;
        reach = win.width / view_scale;
    }
    if (Logic::as_play(play)) {
        auto& sections = play->m_nonEffectObjects;
        auto& sizes = play->m_nonEffectObjectsSizes;
        float min_y = view_known ? view.getMinY() - 200.f : -1e9f;
        float max_y = view_known ? view.getMaxY() + 200.f : 1e9f;
        float x_factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
        int first_x = std::max((int)(std::floor((view_x - reach) * x_factor)) - 1, 0);
        int last_x = std::min((int)(std::floor((view_x + reach) * x_factor)) + 1, (int)(std::min(sections.size(), sizes.size())) - 1);
        for (int x = first_x; x <= last_x; ++x) {
            auto* column = sections[x];
            auto* counts = sizes[x];
            if (!column || !counts) continue;
            int rows = (int)(std::min(column->size(), counts->size()));
            // on keres everything is in row 0 xd
            for (int y = 0; y < rows; ++y) {
                auto* bucket = (*column)[y];
                if (!bucket) continue;
                int object_count = std::min<int>((*counts)[y], (int)(bucket->size()));
                for (int i = 0; i < object_count; ++i) {
                    auto* obj = (*bucket)[i];
                    if (!obj || obj->m_isDisabled) continue;
                    if (std::fabs(obj->getPositionX() - view_x) > reach || obj->getPositionY() < min_y || obj->getPositionY() > max_y)
                        continue;
                    Hitboxes::get().draw_object_hitbox(node, play, obj, width, logic.fill_hitboxes);
                }
            }
        }
        auto const& moving = play->m_calcNonEffectObjects;
        int moving_count = std::min<int>(play->m_calcNonEffectObjectsSize, (int)(moving.size()));
        for (int i = 0; i < moving_count; ++i) {
            auto* obj = moving[(std::size_t)i];
            if (!obj || obj->m_isDisabled) continue;
            if (std::fabs(obj->getPositionX() - view_x) > reach) continue;
            Hitboxes::get().draw_object_hitbox(node, play, obj, width, logic.fill_hitboxes);
        }
    }
    else if (play->m_objects) {
        float min_y = view_known ? view.getMinY() - 200.f : -1e9f;
        float max_y = view_known ? view.getMaxY() + 200.f : 1e9f;
        for (auto* obj : CCArrayExt<GameObject*>(play->m_objects)) {
            if (!obj) continue;
            if (obj->m_isDisabled) continue;
            if (std::fabs(obj->getPositionX() - view_x) > reach || obj->getPositionY() < min_y || obj->getPositionY() > max_y)
                continue;
            if (obj->m_isTrigger) {
                auto* effect = typeinfo_cast<EffectGameObject*>(obj);
                if (!effect || !effect->m_isTouchTriggered) continue;
            }
            Hitboxes::get().draw_object_hitbox(node, play, obj, width, logic.fill_hitboxes);
        }
    }
    if (g_outlines.collecting) flush_outlines(node, width);
    if (!Logic::get().hitbox_player_hidden(false))
        Hitboxes::get().draw_player_hitbox(node, play, play->m_player1, width, logic.fill_hitboxes, nullptr);
    else logic.trail_p1.clear();
    if (play->m_gameState.m_isDualMode && !Logic::get().hitbox_player_hidden(true)) {
        Hitboxes::get().draw_player_hitbox(node, play, play->m_player2, width, logic.fill_hitboxes, nullptr);
    }
    else if (Logic::get().hitbox_player_hidden(true)) {
        logic.trail_p2.clear();
    }
    Hitboxes::get().draw_clicks(node, play, width, logic.fill_hitboxes);
}
