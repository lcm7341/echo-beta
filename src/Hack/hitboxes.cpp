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
	CCRect scaledRect(CCRect rect, float scale);
	void scalePoints(CCPoint* points, int count, float scale);
	void drawPlayerHitbox(CCDrawNode* node, GJBaseGameLayer* play, PlayerObject* player, float width, bool fill, float const* tint);
	void captureClick(GJBaseGameLayer* play, PlayerObject* player, bool down);
	void drawClicks(CCDrawNode* node, GJBaseGameLayer* play, float width, bool fill);
	void clearClicks();
	void drawObjectHitbox(CCDrawNode* node, GJBaseGameLayer* play, GameObject* object, float width, bool fill);
	void pushTrail(std::deque<HitboxTrailPoint>& trail, HitboxTrailPoint point, GJBaseGameLayer* play);
	int trailLimit();
	int trailNow();
	void trimTrail(std::deque<HitboxTrailPoint>& trail, GJBaseGameLayer* play);
};

CCRect Hitboxes::scaledRect(CCRect rect, float scale) {
	if (scale == 1.f) return rect;
	CCPoint mid{rect.getMidX(), rect.getMidY()};
	rect.size.width *= scale;
	rect.size.height *= scale;
	rect.origin.x = mid.x - rect.size.width * 0.5f;
	rect.origin.y = mid.y - rect.size.height * 0.5f;
	return rect;
}
void Hitboxes::scalePoints(CCPoint* points, int count, float scale) {
	if (!points || count <= 0 || scale == 1.f) return;
	float cx = 0.f;
	float cy = 0.f;
	for (int i = 0; i < count; ++i) {
		cx += points[i].x;
		cy += points[i].y;
	}
	cx /= static_cast<float>(count);
	cy /= static_cast<float>(count);
	for (int i = 0; i < count; ++i) {
		points[i].x = cx + (points[i].x - cx) * scale;
		points[i].y = cy + (points[i].y - cy) * scale;
	}
}



constexpr int kHitboxTag = 0xEC06;

CCDrawNode* findHitboxNode(GJBaseGameLayer* play) {
	auto find = [](CCNode* parent) -> CCDrawNode* {
		if (!parent) return nullptr;
		return typeinfo_cast<CCDrawNode*>(parent->getChildByTag(kHitboxTag));
	};
	if (auto* node = find(play)) return node;
	if (auto* node = find(play->m_objectLayer)) return node;
	if (auto* node = find(play->m_uiLayer)) return node;
	return nullptr;
}

namespace {
// Same sync Silicate uses: object-local coords, parented outside ShaderLayer on the
// game layer, additionalTransform = object→world then world→parent.
void syncToObjectLayer(CCNode* node, CCNode* objectLayer) {
	if (!node || !objectLayer || !node->getParent()) return;
	node->setAdditionalTransform(CCAffineTransformConcat(
		objectLayer->nodeToWorldTransform(),
		node->getParent()->worldToNodeTransform()));
}

int overlayZOrder(GJBaseGameLayer* play) {
	if (auto* editor = typeinfo_cast<LevelEditorLayer*>(play); editor && editor->m_editorUI) {
		return editor->m_editorUI->getZOrder() - 1;
	}
	return (play->m_uiLayer ? play->m_uiLayer->getZOrder() : 101) + 1;
}

// Resync object-layer transform at visit time (when Silicate does it) so camera/movers match.
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
			syncToObjectLayer(this, play->m_objectLayer);
		}
		CCDrawNode::visit();
	}
};
}

void Logic::seatOverlay(CCNode* node, GJBaseGameLayer* play) {
	if (!node || !play || !play->m_objectLayer) return;
	// Parent to the game layer (outside ShaderLayer FBO) — Silicate's approach.
	int z = overlayZOrder(play);
	if (node->getParent() != play) {
		node->removeFromParentAndCleanup(false);
		play->addChild(node, z, node->getTag());
	} else if (node->getZOrder() != z) node->setZOrder(z);
	node->setPosition({0.f, 0.f});
	node->setAnchorPoint({0.f, 0.f});
	node->setScale(1.f);
	node->setRotation(0.f);
	node->setSkewX(0.f);
	node->setSkewY(0.f);
	syncToObjectLayer(node, play->m_objectLayer);
}

void Logic::syncOverlaySeats(GJBaseGameLayer* play) {
	if (!play || !play->m_objectLayer) return;
	if (auto* node = findHitboxNode(play)) seatOverlay(node, play);
	if (auto* live = Logic::asPlay(play)) Trajectory::get().syncSeat(live);
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

CCRect visibleLayerRect(CCNode* layer);

struct PendingClick {
	bool player2 = false;
	bool down = true;
};
std::vector<PendingClick> g_pendingClicks;

float collisionRadius(GameObject* object) {
	if (!object || object->m_objectRadius <= 0.f) return 0.f;
	float radius = object->m_objectRadius;
	float sx = std::abs(object->m_scaleX);
	float sy = std::abs(object->m_scaleY);
	if (sx != 1.f || sy != 1.f) radius *= std::max(sx, sy);
	return radius;
}

float hazardRadius(GameObject* object, float scale) {
	// GD's getObjectRadius uses max(scaleX, scaleY) without abs, so flipped saws
	// (negative scale) report a negative radius and we used to fall back to a square.
	return collisionRadius(object) * std::abs(scale);
}

// Drawing an object's hitbox rebuilds its cached collision rect and oriented box (forcing them
// dirty, since move triggers can leave them clean). Collision reads those caches, and in GD a
// stale cache stays stale until something marks it dirty, so rebuilding it at draw time changed
// collision results with hitboxes on, by frame timing: a macro recorded that way could play back
// differently. The guard puts back everything drawing touched: the box pointer and its flag
// (0x2e0..0x2e8), the cached rect and both dirty flags (0x358..0x369), and the box's contents. A
// box drawing had to create is released again.
class HitboxStateGuard {
	static constexpr size_t kFrom = 0x2e0, kTo = 0x370;
	static constexpr size_t kBoxFrom = 0x140, kBoxTo = 0x1b8;
	GameObject* m_object;
	std::array<uint8_t, kTo - kFrom> m_fields{};
	OBB2D* m_box;
	std::array<uint8_t, kBoxTo - kBoxFrom> m_boxData{};
public:
	explicit HitboxStateGuard(GameObject* object) : m_object(object), m_box(object->m_orientedBox) {
		auto* bytes = reinterpret_cast<uint8_t*>(object);
		std::memcpy(m_fields.data(), bytes + kFrom, m_fields.size());
		if (m_box) std::memcpy(m_boxData.data(), reinterpret_cast<uint8_t*>(m_box) + kBoxFrom, m_boxData.size());
	}
	~HitboxStateGuard() {
		auto* created = m_object->m_orientedBox;
		if (created && created != m_box) created->release();
		std::memcpy(reinterpret_cast<uint8_t*>(m_object) + kFrom, m_fields.data(), m_fields.size());
		if (m_box) std::memcpy(reinterpret_cast<uint8_t*>(m_box) + kBoxFrom, m_boxData.data(), m_boxData.size());
	}
	HitboxStateGuard(HitboxStateGuard const&) = delete;
	HitboxStateGuard& operator=(HitboxStateGuard const&) = delete;
};

OBB2D* liveOrientedBox(GameObject* object) {
	if (!object) return nullptr;
	// updateOrientedBox angles from m_startRotationX + m_rotationXOffset, not the
	// live node rotation. Editor rotate updates the node; move is what normally
	// syncs start rotation — so mirror that while we rebuild the OBB.
	bool editor = LevelEditorLayer::get() && !PlayLayer::get();
	float savedStart = object->m_startRotationX;
	if (editor) object->m_startRotationX = object->getRotation() - object->m_rotationXOffset;
	object->m_isOrientedBoxDirty = true;
	object->m_isObjectRectDirty = true;
	object->updateOrientedBox();
	if (editor) object->m_startRotationX = savedStart;
	if (auto* box = object->m_orientedBox) return box;
	return object->getOrientedBox();
}

void refreshObjectHitbox(GameObject* object) {
	if (!object) return;
	// Move triggers often leave the cached rect/OBB clean while the node has already moved.
	object->m_isOrientedBoxDirty = true;
	object->m_isObjectRectDirty = true;
}

// The object's hitbox box as the hitbox drawing shows it, with its collision caches left exactly
// as they were (layout.cpp's Default Art sizes blocks to it).
CCRect Logic::objectHitbox(GameObject* object) {
	if (!object) return {};
	HitboxStateGuard preserve(object);
	refreshObjectHitbox(object);
	return object->getObjectRect();
}

void Logic::noteClickHitbox(GJBaseGameLayer* play, bool player1, bool down) {
	if (!play || Logic::get().predicting) return;
	if (!Logic::get().showHitboxes || !Logic::get().hitboxClick) return;
	g_pendingClicks.push_back({!player1, down});
	if (!play->m_gameState.m_isDualMode) return;
	g_pendingClicks.push_back({player1, down});
}
void Logic::capturePendingClick(PlayerObject* player) {
	auto* play = GJBaseGameLayer::get();
	if (!play || !player || Logic::get().isTrajectoryPlayer(player)) return;
	bool player2 = player == play->m_player2;
	std::erase_if(g_pendingClicks, [&](PendingClick const& click) {
		if (click.player2 != player2) return false;
		Hitboxes::get().captureClick(play, player, click.down);
		return true;
	});
}
void Logic::flushClickHitboxes(GJBaseGameLayer* play) {
	if (!play) {
		g_pendingClicks.clear();
		return;
	}
	auto const& value = Logic::get();
	size_t from = g_pendingClicks.size() > 400 ? g_pendingClicks.size() - 400 : 0;
	for (auto const& click : std::span(g_pendingClicks).subspan(from)) {
		auto* player = click.player2 ? play->m_player2 : play->m_player1;
		if ((click.player2 && Logic::get().hitboxPlayerHidden(true)) || (!click.player2 && Logic::get().hitboxPlayerHidden(false))) continue;
		Hitboxes::get().captureClick(play, player, click.down);
	}
	g_pendingClicks.clear();
}
void Logic::clearClickHitbox() {
	g_pendingClicks.clear();
	Hitboxes::get().clearClicks();
}

void Hitboxes::captureClick(GJBaseGameLayer*, PlayerObject* player, bool down) {
	auto const& value = Logic::get();
	if (!player || !value.showHitboxes || !value.hitboxClick || Logic::get().isTrajectoryPlayer(player)) return;
	float scale = value.hitboxScale;
	ClickBox box;
	box.down = down;
	box.frame = Hitboxes::get().trailNow();
	box.outer = Hitboxes::get().scaledRect(player->getObjectRect(), scale);
	box.inner = Hitboxes::get().scaledRect(player->getObjectRect(0.3f, 0.3f), scale);
	box.center = CCPoint{box.outer.getMidX(), box.outer.getMidY()};
	box.radius = std::abs(box.outer.size.width) * 0.5f;
	if (auto* oriented = player->getOrientedBox()) {
		box.corners = oriented->m_corners;
		Hitboxes::get().scalePoints(box.corners.data(), 4, scale);
		box.rotated = true;
	}
	g_clicks.push_back(box);
	while (g_clicks.size() > 4096) g_clicks.pop_front();
}
void Hitboxes::clearClicks() {
	g_clicks.clear();
}
void Hitboxes::drawClicks(CCDrawNode* node, GJBaseGameLayer* play, float width, bool fill) {
	auto const& value = Logic::get();
	if (!value.hitboxClick) {
		g_clicks.clear();
		return;
	}
	if (!(play && LevelEditorLayer::get() == play)) {
		if (value.hitboxTrailVisual && value.isPlaying() && play && play->m_objectLayer) {
			CCRect view = visibleLayerRect(play->m_objectLayer);
			if (std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f) {
				std::erase_if(g_clicks, [&](ClickBox const& box) { return !view.intersectsRect(box.outer); });
			}
		} else {
			int now = Hitboxes::get().trailNow();
			int limit = Hitboxes::get().trailLimit();
			std::erase_if(g_clicks, [&](ClickBox const& box) { return box.frame > now || now - box.frame >= limit; });
		}
	}
	for (auto const& box : g_clicks) {
		float const* color = box.down ? value.hitboxClickColor : value.hitboxReleaseColor;
		auto tint = HitboxColor::withFill(color, fill, value.hitboxFillOpacity);
		auto line = HitboxColor::lineOf(color);
		if (value.hitboxRotated && box.rotated) {
			auto corners = box.corners;
			node->drawPolygon(corners.data(), 4, tint, width, line, BorderAlignment::Inside);
		}
		if (value.hitboxPlayer || (!value.hitboxRotated && !value.hitboxInner && !value.hitboxCircle)) {
			node->drawRect(box.outer, tint, width, line, BorderAlignment::Inside);
		}
		if (value.hitboxInner) node->drawRect(box.inner, tint, width, line, BorderAlignment::Inside);
		if (value.hitboxCircle && box.radius > 0.f) {
			node->drawCircle(box.center, box.radius, {0.f, 0.f, 0.f, 0.f}, width, line, 24);
		}
	}
}

void Hitboxes::drawPlayerHitbox(CCDrawNode* node, GJBaseGameLayer* play, PlayerObject* player, float width, bool fill, float const* tint) {
	if (!player || Logic::get().isTrajectoryPlayer(player)) return;
	auto const& value = Logic::get();
	float scale = value.hitboxScale;
	auto rect = Hitboxes::get().scaledRect(player->getObjectRect(), scale);
	auto innerRect = Hitboxes::get().scaledRect(player->getObjectRect(0.3f, 0.3f), scale);
	CCPoint center{rect.getMidX(), rect.getMidY()};
	float const* outerColor = tint ? tint : value.hitboxOuterColor;
	float const* rotatedColor = tint ? tint : value.hitboxRotatedColor;
	float const* innerColor = tint ? tint : value.hitboxInnerColor;
	float const* circleColor = tint ? tint : value.hitboxCircleColor;
	if (value.hitboxRotated) {
		if (auto* box = player->getOrientedBox()) {
			std::array<CCPoint, 4> corners = box->m_corners;
			Hitboxes::get().scalePoints(corners.data(), 4, scale);
			node->drawPolygon(corners.data(), 4, HitboxColor::withFill(rotatedColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(rotatedColor), BorderAlignment::Inside);
		}
	}
	if (value.hitboxPlayer) {
		node->drawRect(rect, HitboxColor::withFill(outerColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(outerColor), BorderAlignment::Inside);
	}
	if (value.hitboxInner) {
		node->drawRect(innerRect, HitboxColor::withFill(innerColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(innerColor), BorderAlignment::Inside);
	}
	float radius = std::abs(rect.size.width) * 0.5f;
	float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
	// With Fix Radius Collision on, GD tests round hazards against this circle, not the box
	// (playerCircleCollision: centre distance against half the box width plus the radius). It is
	// the hitbox that matters there, so it is shown.
	bool fixRadius = play->m_levelSettings && play->m_levelSettings->m_fixRadiusCollision;
	if ((value.hitboxCircle || fixRadius) && radius > 0.f) {
		unsigned segments = static_cast<unsigned>(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
		node->drawCircle(center, radius, {0.f, 0.f, 0.f, 0.f}, width, HitboxColor::lineOf(circleColor), std::max(segments, 8u));
	}
}

// Outline Only: the object hitboxes of one frame are collected as convex shapes instead of drawn,
// then each shape's edges are cut where other shapes cross them, and a piece is drawn only when the
// point just outside it is outside every other shape of its kind. Blocks, slopes and hazards are one
// kind: touching or overlapping ones show one outline around the whole group, each piece in its own
// shape's colour, and a hitbox inside others (blocks and spikes inside a slope) shows nothing.
// Specials (orbs, pads, portals) are the other kind: they keep their outlines over hazards.
namespace {
struct OutlineShape {
	std::vector<CCPoint> points; // Counter-clockwise.
	ccColor4F fill;
	ccColor4F line;
	CCRect bounds;
	int kind = 0; // 0: blocks, slopes and hazards; 1: orbs, pads, portals and other specials.
};

struct OutlineSink {
	bool collecting = false;
	int kind = 0; // The kind the shapes collected next belong to.
	std::vector<OutlineShape> shapes;
};

OutlineSink& outlines() {
	static OutlineSink sink;
	return sink;
}

void collectShape(CCPoint const* points, unsigned count, ccColor4F const& fill, ccColor4F const& line) {
	if (count < 3) return;
	OutlineShape shape;
	shape.points.assign(points, points + count);
	float area = 0.f;
	for (unsigned i = 0; i < count; ++i) {
		auto const& a = shape.points[i];
		auto const& b = shape.points[(i + 1) % count];
		area += a.x * b.y - b.x * a.y;
	}
	if (area < 0.f) std::reverse(shape.points.begin(), shape.points.end());
	float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
	for (auto const& p : shape.points) {
		minX = std::min(minX, p.x);
		minY = std::min(minY, p.y);
		maxX = std::max(maxX, p.x);
		maxY = std::max(maxY, p.y);
	}
	shape.bounds = {minX, minY, maxX - minX, maxY - minY};
	shape.fill = fill;
	shape.line = line;
	shape.kind = outlines().kind;
	outlines().shapes.push_back(std::move(shape));
}

void emitPolygon(CCDrawNode* node, CCPoint* points, unsigned count, ccColor4F const& fill, float width, ccColor4F const& line) {
	if (outlines().collecting) return collectShape(points, count, fill, line);
	node->drawPolygon(points, count, fill, width, line, BorderAlignment::Inside);
}

void emitRect(CCDrawNode* node, CCRect const& rect, ccColor4F const& fill, float width, ccColor4F const& line) {
	if (outlines().collecting) {
		std::array<CCPoint, 4> corners{rect.origin, ccp(rect.getMaxX(), rect.getMinY()), ccp(rect.getMaxX(), rect.getMaxY()), ccp(rect.getMinX(), rect.getMaxY())};
		return collectShape(corners.data(), 4, fill, line);
	}
	node->drawRect(rect, fill, width, line, BorderAlignment::Inside);
}

void emitCircle(CCDrawNode* node, CCPoint const& center, float radius, ccColor4F const& fill, float width, ccColor4F const& line, unsigned segments) {
	if (outlines().collecting) {
		std::vector<CCPoint> ring(std::max(segments, 8u));
		for (std::size_t i = 0; i < ring.size(); ++i) {
			float angle = static_cast<float>(i) * 2.f * static_cast<float>(M_PI) / static_cast<float>(ring.size());
			ring[i] = center + CCPoint{std::cos(angle), std::sin(angle)} * radius;
		}
		return collectShape(ring.data(), static_cast<unsigned>(ring.size()), fill, line);
	}
	node->drawCircle(center, radius, fill, width, line, segments);
}

// Inside or on the edge of a counter-clockwise convex shape.
bool insideShape(OutlineShape const& shape, CCPoint p) {
	if (!shape.bounds.containsPoint(p)) return false;
	auto const& pts = shape.points;
	for (std::size_t i = 0; i < pts.size(); ++i) {
		auto const& a = pts[i];
		auto const& b = pts[(i + 1) % pts.size()];
		if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) < -1e-4f) return false;
	}
	return true;
}

void flushOutlines(CCDrawNode* node, float width) {
	auto& sink = outlines();
	sink.collecting = false;
	auto& shapes = sink.shapes;
	if (shapes.empty()) return;
	// Fills first, without borders, so the outlines draw on top.
	for (auto& shape : shapes) {
		if (shape.fill.a > 0.f) node->drawPolygon(shape.points.data(), static_cast<unsigned>(shape.points.size()), shape.fill, 0.f, {0.f, 0.f, 0.f, 0.f});
	}
	// Shapes bucketed by their bounds, to find the ones near an edge.
	constexpr float kCell = 60.f;
	std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cells;
	auto cellKey = [](int x, int y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y); };
	for (std::uint32_t i = 0; i < shapes.size(); ++i) {
		auto const& b = shapes[i].bounds;
		for (int x = static_cast<int>(std::floor(b.getMinX() / kCell)); x <= static_cast<int>(std::floor(b.getMaxX() / kCell)); ++x)
			for (int y = static_cast<int>(std::floor(b.getMinY() / kCell)); y <= static_cast<int>(std::floor(b.getMaxY() / kCell)); ++y) cells[cellKey(x, y)].push_back(i);
	}
	std::vector<std::uint32_t> nearby;
	std::vector<std::uint32_t> seen(shapes.size(), UINT32_MAX);
	std::vector<float> cuts;
	// A polygon's border is `width` either side of a line, 2 * `width` thick; Inside puts it all
	// inside the edge. A piece is a segment of radius `width` along the line `width` inside its edge.
	// Not a filled band: cocos gives every filled polygon a 1-unit anti-aliasing fringe, about 3 px at
	// normal zoom, much thicker than the border. Each end is pulled in by the radius so the round
	// caps stop at the corner instead of running past it.
	float depth = width;
	// How far outside an edge its probe point sits.
	float probe = std::max(0.05f, width * 0.25f);
	for (std::uint32_t i = 0; i < shapes.size(); ++i) {
		auto const& shape = shapes[i];
		auto const& pts = shape.points;
		for (std::size_t e = 0; e < pts.size(); ++e) {
			auto a = pts[e];
			auto b = pts[(e + 1) % pts.size()];
			auto d = b - a;
			float length = std::hypot(d.x, d.y);
			if (length < 1e-4f) continue;
			CCPoint outward{d.y / length, -d.x / length}; // Right of a counter-clockwise edge.
			// Shapes of this colour whose bounds reach the edge.
			nearby.clear();
			float minX = std::min(a.x, b.x) - probe, maxX = std::max(a.x, b.x) + probe;
			float minY = std::min(a.y, b.y) - probe, maxY = std::max(a.y, b.y) + probe;
			for (int x = static_cast<int>(std::floor(minX / kCell)); x <= static_cast<int>(std::floor(maxX / kCell)); ++x)
				for (int y = static_cast<int>(std::floor(minY / kCell)); y <= static_cast<int>(std::floor(maxY / kCell)); ++y) {
					auto found = cells.find(cellKey(x, y));
					if (found == cells.end()) continue;
					for (auto j : found->second) {
						if (j == i || seen[j] == i * 1024u + e || shapes[j].kind != shape.kind) continue;
						seen[j] = static_cast<std::uint32_t>(i * 1024u + e);
						auto const& o = shapes[j].bounds;
						if (o.getMaxX() < minX || o.getMinX() > maxX || o.getMaxY() < minY || o.getMinY() > maxY) continue;
						nearby.push_back(j);
					}
				}
			// Where the other shapes' edges cross this one.
			cuts.assign({0.f, 1.f});
			for (auto j : nearby) {
				auto const& other = shapes[j].points;
				for (std::size_t k = 0; k < other.size(); ++k) {
					auto c = other[k];
					auto f = other[(k + 1) % other.size()] - c;
					float denom = d.x * f.y - d.y * f.x;
					if (std::abs(denom) < 1e-6f) continue;
					auto ac = c - a;
					float t = (ac.x * f.y - ac.y * f.x) / denom;
					float u = (ac.x * d.y - ac.y * d.x) / denom;
					if (t > 0.f && t < 1.f && u >= 0.f && u <= 1.f) cuts.push_back(t);
				}
			}
			std::sort(cuts.begin(), cuts.end());
			CCPoint inward = outward * -depth;
			for (std::size_t c = 0; c + 1 < cuts.size(); ++c) {
				float t0 = cuts[c], t1 = cuts[c + 1];
				if ((t1 - t0) * length < 0.01f) continue;
				auto mid = a + d * ((t0 + t1) * 0.5f);
				auto test = mid + outward * probe;
				bool covered = std::ranges::any_of(nearby, [&](std::uint32_t j) { return insideShape(shapes[j], test); });
				if (covered) continue;
				// Drawn inside the shape, as the per-object outlines are (BorderAlignment::Inside).
				float trim = std::min(depth / length, (t1 - t0) * 0.5f);
				node->drawSegment(a + d * (t0 + trim) + inward, a + d * (t1 - trim) + inward, depth, shape.line);
			}
		}
	}
	shapes.clear();
}
}

void Hitboxes::drawObjectHitbox(CCDrawNode* node, GJBaseGameLayer* play, GameObject* object, float width, bool fill) {
	if (!object || object->m_isGroupDisabled) return;
	// Don't require m_isActivated — section activation lags the camera on some levels, so
	// on-screen objects (and early parts of long levels) would draw with no hitboxes.
	if (object->m_objectType == GameObjectType::Decoration) return;
	if (object == play->m_player1 || object == play->m_player2 || Logic::get().isTrajectoryPlayer(object)) return;
	if (auto* live = Logic::asPlay(play)) {
		if (object == live->m_player1CollisionBlock || object == live->m_player2CollisionBlock) return;
		if (object == live->m_anticheatSpike) return;
	}
	auto const& value = Logic::get();
	HitboxStateGuard preserve(object);
	refreshObjectHitbox(object);

	float scale = value.hitboxScale;
	switch (object->m_objectType) {
		case GameObjectType::Solid:
		case GameObjectType::Slope:
		case GameObjectType::Hazard:
		case GameObjectType::AnimatedHazard: outlines().kind = 0; break;
		default: outlines().kind = 1; break;
	}
	auto paintRect = [&](float const* color) {
		emitRect(node, Hitboxes::get().scaledRect(object->getObjectRect(), scale), HitboxColor::withFill(color, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(color));
	};

	switch (object->m_objectType) {
		case GameObjectType::Solid: {
			if (!value.hitboxSolid) break;
			// Passable blocks (fake blocks) let the player through: nothing there to hit.
			if (object->m_isPassable) break;
			paintRect(value.hitboxSolidColor);
			break;
		}
		case GameObjectType::Slope: {
			auto rect = Hitboxes::get().scaledRect(object->getObjectRect(), scale);
			std::array<CCPoint, 3> verts{
				rect.origin,
				ccp(rect.getMinX(), rect.getMaxY()),
				ccp(rect.getMaxX(), rect.getMinY()),
			};
			CCPoint maxPoint{rect.getMaxX(), rect.getMaxY()};
			switch (object->m_slopeDirection) {
				case 0:
				case 7: verts[1] = maxPoint; break;
				case 1:
				case 5: verts[0] = maxPoint; break;
				case 3:
				case 6: verts[2] = maxPoint; break;
				default: break;
			}
			// A hazard slope kills on touch, so it shows in the hazard colour (and with the hazard toggle).
			float const* slopeColor = object->m_slopeIsHazard ? value.hitboxHazardColor : value.hitboxSlopeColor;
			if (object->m_slopeIsHazard ? !value.hitboxHazard : !value.hitboxSlope) break;
			if (object->m_slopeIsHazard) {
				// GameObject::slopeYPos (0x1a13b0), which every slope collision measures from, moves a
				// hazard slope's surface 4 units out: down for directions 1, 3, 5 and 6 (the ones with
				// the corner at the top), up for the rest. Drawn as the plain triangle, the wave died
				// before its box reached it. The corner stays; the sloped side moves out by 4.
				int dir = object->m_slopeDirection;
				bool ceiling = dir >= 0 && dir <= 6 && ((0x6a >> dir) & 1);
				float shift = (ceiling ? -4.f : 4.f) * scale;
				// The right-angle corner: the vertex sharing x with one other vertex and y with the other.
				int corner = 0;
				for (int k = 0; k < 3; ++k) {
					auto const& a = verts[(k + 1) % 3];
					auto const& b = verts[(k + 2) % 3];
					auto const& c = verts[k];
					if ((c.x == a.x && c.y == b.y) || (c.x == b.x && c.y == a.y)) corner = k;
				}
				std::array<CCPoint, 4> shape;
				int n = 0;
				for (int k = 0; k < 3; ++k) {
					auto const& p = verts[(corner + k) % 3];
					if (k == 0) {
						shape[n++] = p;
						continue;
					}
					CCPoint moved{p.x, p.y + shift};
					// The end level with the corner keeps its point and gains the moved one; the end
					// above or below the corner just moves (its old point lies on the new edge).
					bool level = p.y == verts[corner].y;
					if (level && k == 1) {
						shape[n++] = p;
						shape[n++] = moved;
					} else if (level) {
						shape[n++] = moved;
						shape[n++] = p;
					} else {
						shape[n++] = moved;
					}
				}
				emitPolygon(node, shape.data(), n, HitboxColor::withFill(slopeColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(slopeColor));
				break;
			}
			emitPolygon(node, verts.data(), 3, HitboxColor::withFill(value.hitboxSlopeColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(value.hitboxSlopeColor));
			break;
		}
		case GameObjectType::Hazard:
		case GameObjectType::AnimatedHazard: {
			if (!value.hitboxHazard) break;
			float radius = hazardRadius(object, scale);
			float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
			if (radius > 0.f) {
				unsigned segments = static_cast<unsigned>(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
				emitCircle(node, object->getPosition(), radius, HitboxColor::withFill(value.hitboxHazardColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(value.hitboxHazardColor), std::max(segments, 8u));
			} else if (auto* box = liveOrientedBox(object)) {
				std::array<CCPoint, 4> corners = box->m_corners;
				Hitboxes::get().scalePoints(corners.data(), 4, scale);
				emitPolygon(node, corners.data(), 4, HitboxColor::withFill(value.hitboxHazardColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(value.hitboxHazardColor));
			} else {
				paintRect(value.hitboxHazardColor);
			}
			break;
		}
		default: {
			if (value.hideActivatedObjects) {
				if (auto* enhanced = typeinfo_cast<EnhancedGameObject*>(object)) {
					bool used = play->m_player1 && phys::hasBeenActivatedByPlayer(play->m_player1, enhanced);
					if (play->m_gameState.m_isDualMode) {
						used = used && play->m_player2 && phys::hasBeenActivatedByPlayer(play->m_player2, enhanced);
					}
					if (used) return;
				}
			}
			if (!value.hitboxInteract) break;
			float radius = collisionRadius(object) * std::abs(scale);
			bool fixRadius = play->m_levelSettings && play->m_levelSettings->m_fixRadiusCollision;
			bool useBox = radius <= 0.f || (object->m_shouldUseOuterOb && !fixRadius);
			if (radius > 0.f) {
				float zoom = std::max(play->m_gameState.m_cameraZoom, 0.05f);
				unsigned segments = static_cast<unsigned>(std::max(radius, 8.f) * 2.f * std::sqrt(zoom));
				emitCircle(node, object->getPosition(), radius, HitboxColor::withFill(value.hitboxInteractColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(value.hitboxInteractColor), std::max(segments, 8u));
			}
			if (useBox) {
				if (auto* box = liveOrientedBox(object)) {
					std::array<CCPoint, 4> corners = box->m_corners;
					Hitboxes::get().scalePoints(corners.data(), 4, scale);
					emitPolygon(node, corners.data(), 4, HitboxColor::withFill(value.hitboxInteractColor, fill, value.hitboxFillOpacity), width, HitboxColor::lineOf(value.hitboxInteractColor));
				} else if (radius <= 0.f) {
					paintRect(value.hitboxInteractColor);
				}
			}
			break;
		}
	}
}
CCRect visibleLayerRect(CCNode* layer) {
	auto win = CCDirector::sharedDirector()->getWinSize();
	CCPoint corners[4] = {{0.f, 0.f}, {win.width, 0.f}, {win.width, win.height}, {0.f, win.height}};
	float minX = 0.f;
	float minY = 0.f;
	float maxX = 0.f;
	float maxY = 0.f;
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
	return {minX, minY, std::max(maxX - minX, 0.f), std::max(maxY - minY, 0.f)};
}

CCRect uprightRect(CCRect rect) {
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
	float width = 0.f;
	float height = 0.f;
};

CCRect trailRect(PlayerObject* player, float scale, TrailFit& fit) {
	CCRect raw = uprightRect(player->getObjectRect());
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
		return Hitboxes::get().scaledRect(raw, scale);
	}
	if (!known) {
		fit.width = std::clamp(finite ? raw.size.width : 30.f, 8.f, 36.f);
		fit.height = std::clamp(finite ? raw.size.height : 30.f, 8.f, 36.f);
	}
	CCPoint pos = player->getPosition();
	if (!std::isfinite(pos.x) || !std::isfinite(pos.y)) pos = CCPoint{raw.getMidX(), raw.getMidY()};
	CCRect held(pos.x - fit.width * 0.5f, pos.y - fit.height * 0.5f, fit.width, fit.height);
	return Hitboxes::get().scaledRect(held, scale);
}

bool sameTrailSpot(CCRect const& a, CCRect const& b) {
	auto close = [](float left, float right) { return std::abs(left - right) < 0.05f; };
	return close(a.origin.x, b.origin.x) && close(a.origin.y, b.origin.y) && close(a.size.width, b.size.width) && close(a.size.height, b.size.height);
}

void Hitboxes::pushTrail(std::deque<HitboxTrailPoint>& trail, HitboxTrailPoint point, GJBaseGameLayer* play) {
	if (!trail.empty() && trail.back().frame == point.frame) {
		trail.back() = point;
		return;
	}
	if (Logic::get().hitboxDedup && !trail.empty() && sameTrailSpot(trail.back().rect, point.rect)) {
		point.frame = trail.back().frame;
		trail.back() = point;
		return;
	}
	trail.push_back(point);
	Hitboxes::get().trimTrail(trail, play);
}

int Hitboxes::trailLimit() {
	auto const& value = Logic::get();
	int length = value.trailLength;
	if (length <= 0) length = std::max(1, static_cast<int>(std::lround(value.fps)));
	return std::max(1, length);
}

int Hitboxes::trailNow() {
	auto const& value = Logic::get();
	return value.hitboxTrailVisual ? value.visualFrame : value.stepFrame;
}

void Hitboxes::trimTrail(std::deque<HitboxTrailPoint>& trail, GJBaseGameLayer* play) {
	auto const& value = Logic::get();
	if (play && LevelEditorLayer::get() == play) return;
	// A replay at visual FPS keeps every sample that is still on screen.
	if (value.hitboxTrailVisual && value.isPlaying() && play && play->m_objectLayer) {
		CCRect view = visibleLayerRect(play->m_objectLayer);
		if (std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f) {
			std::erase_if(trail, [&](HitboxTrailPoint const& point) { return !view.intersectsRect(point.rect); });
			return;
		}
	}
	int limit = Hitboxes::get().trailLimit();
	int now = Hitboxes::get().trailNow();
	while (!trail.empty() && (trail.front().frame > now || now - trail.front().frame >= limit)) trail.pop_front();
}

void Logic::sampleHitboxTrail(GJBaseGameLayer* play) {
	auto& value = Logic::get();
	if (!play || !value.showHitboxes || !value.hitboxTrail) return;
	if (auto* live = Logic::asPlay(play); live && (live->m_levelEndAnimationStarted || live->m_playerDied)) return;
	int frame = Hitboxes::get().trailNow();
	float scale = value.hitboxScale;
	static TrailFit fitP1;
	static TrailFit fitP2;
	if (value.trailP1.empty()) fitP1 = {};
	if (value.trailP2.empty()) fitP2 = {};
	auto capture = [&](PlayerObject* player, TrailFit& fit, std::deque<HitboxTrailPoint>& trail) {
		HitboxTrailPoint point;
		point.frame = frame;
		point.rect = trailRect(player, scale, fit);
		CCPoint mid{point.rect.getMidX(), point.rect.getMidY()};
		point.inner = point.rect;
		point.inner.size.width *= 0.3f;
		point.inner.size.height *= 0.3f;
		point.inner.origin.x = mid.x - point.inner.size.width * 0.5f;
		point.inner.origin.y = mid.y - point.inner.size.height * 0.5f;
		point.center = mid;
		point.radius = std::abs(point.rect.size.width) * 0.5f;
		if (auto* oriented = player->getOrientedBox()) {
			point.corners = oriented->m_corners;
			Hitboxes::get().scalePoints(point.corners.data(), 4, scale);
			float minX = point.corners[0].x;
			float maxX = minX;
			float minY = point.corners[0].y;
			float maxY = minY;
			for (int i = 1; i < 4; ++i) {
				minX = std::min(minX, point.corners[i].x);
				maxX = std::max(maxX, point.corners[i].x);
				minY = std::min(minY, point.corners[i].y);
				maxY = std::max(maxY, point.corners[i].y);
			}
			float spanW = maxX - minX;
			float spanH = maxY - minY;
			bool huge = spanW > 75.f || spanH > 75.f || (spanW > std::abs(point.rect.size.width) * 2.5f && spanW > 45.f) || (spanH > std::abs(point.rect.size.height) * 2.5f && spanH > 45.f);
			point.rotated = !huge;
		}
		Hitboxes::get().pushTrail(trail, point, play);
	};
	if (play->m_player1 && !Logic::get().hitboxPlayerHidden(false) && !Logic::get().isTrajectoryPlayer(play->m_player1)) capture(play->m_player1, fitP1, value.trailP1);
	if (play->m_player2 && !Logic::get().hitboxPlayerHidden(true) && !Logic::get().isTrajectoryPlayer(play->m_player2)) capture(play->m_player2, fitP2, value.trailP2);
}

void Logic::drawHitboxes(GJBaseGameLayer* play) {
	auto& value = Logic::get();
	auto* layer = play->m_objectLayer;
	if (!layer) return;
	auto* node = findHitboxNode(play);
	if (!value.showHitboxes) {
		if (node) node->setVisible(false);
		value.trailP1.clear();
		value.trailP2.clear();
		return;
	}
	if (!node) {
		node = EchoHitboxNode::create();
		node->setTag(kHitboxTag);
	}
	node->setVisible(true);
	node->setBlendFunc({GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA});
	seatOverlay(node, play);
	node->clear();
	if (value.hideP1 && play->m_player1) play->m_player1->setVisible(false);
	if (value.hideP2 && play->m_player2) play->m_player2->setVisible(false);
	auto* player = play->m_player1;
	bool level = Logic::asPlay(play) != nullptr;
	float viewScale = std::max(play->m_gameState.m_cameraZoom, 0.05f);
	if (!level) viewScale = std::max(std::abs(layer->getScaleX()), 0.05f);
	float width = std::max(value.hitboxThickness, 0.f) / viewScale;
	Hitboxes::get().trimTrail(value.trailP1, play);
	Hitboxes::get().trimTrail(value.trailP2, play);
	if (!value.hitboxTrail) {
		value.trailP1.clear();
		value.trailP2.clear();
	}
	auto grid = [](float value) { return static_cast<int>(std::lround(value * 20.f)); };
	struct Spot {
		int x;
		int y;
		int w;
		int h;
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
			for (int value : corners.point) hash = hash * 1315423911u + static_cast<size_t>(value);
			return hash;
		}
	};
	auto quantize = [&](CCRect const& rect) {
		return Spot{grid(rect.origin.x), grid(rect.origin.y), grid(rect.size.width), grid(rect.size.height)};
	};
	auto quantizeCorners = [&](std::array<CCPoint, 4> const& corners) {
		Corners key;
		for (int i = 0; i < 4; ++i) {
			key.point[i * 2] = grid(corners[i].x);
			key.point[i * 2 + 1] = grid(corners[i].y);
		}
		return key;
	};
	auto quantizeCircle = [&](CCPoint center, float radius) {
		return Spot{grid(center.x), grid(center.y), grid(radius), 0};
	};
	std::unordered_set<Spot, SpotHash> drawnBoxes;
	std::unordered_set<Corners, CornersHash> drawnRotated;
	std::unordered_set<Spot, SpotHash> drawnInner;
	std::unordered_set<Spot, SpotHash> drawnCircles;
	auto claimLive = [&](PlayerObject* body) {
		if (!body || Logic::get().isTrajectoryPlayer(body)) return;
		float scale = value.hitboxScale;
		auto rect = Hitboxes::get().scaledRect(body->getObjectRect(), scale);
		auto inner = Hitboxes::get().scaledRect(body->getObjectRect(0.3f, 0.3f), scale);
		drawnBoxes.insert(quantize(rect));
		drawnInner.insert(quantize(inner));
		drawnCircles.insert(quantizeCircle(CCPoint{rect.getMidX(), rect.getMidY()}, std::abs(rect.size.width) * 0.5f));
		if (auto* oriented = body->getOrientedBox()) {
			std::array<CCPoint, 4> corners = oriented->m_corners;
			Hitboxes::get().scalePoints(corners.data(), 4, scale);
			drawnRotated.insert(quantizeCorners(corners));
		}
	};
	if (!Logic::get().hitboxPlayerHidden(false)) claimLive(play->m_player1);
	if (play->m_gameState.m_isDualMode && !Logic::get().hitboxPlayerHidden(true)) claimLive(play->m_player2);
	bool fill = value.fillHitboxes;
	float trailOpacity = std::clamp(value.hitboxTrailOpacity, 0.f, 1.f);
	auto trailLine = [&](float const* color) {
		auto line = HitboxColor::lineOf(color);
		line.a *= trailOpacity;
		return line;
	};
	auto trailFill = [&](float const* color) {
		auto painted = HitboxColor::withFill(color, fill, value.hitboxFillOpacity);
		painted.a *= trailOpacity;
		return painted;
	};
	auto eachTrail = [&](auto&& draw) {
		if (!value.hitboxTrail) return;
		draw(value.trailP1);
		draw(value.trailP2);
	};
	if (value.hitboxPlayer) {
		eachTrail([&](std::deque<HitboxTrailPoint> const& trail) {
			for (auto const& old : trail) {
				if (!drawnBoxes.insert(quantize(old.rect)).second) continue;
				node->drawRect(old.rect, trailFill(value.hitboxOuterColor), width, trailLine(value.hitboxOuterColor), BorderAlignment::Inside);
			}
		});
	}
	if (value.hitboxRotated) {
		eachTrail([&](std::deque<HitboxTrailPoint> const& trail) {
			for (auto const& old : trail) {
				if (!old.rotated || !drawnRotated.insert(quantizeCorners(old.corners)).second) continue;
				auto corners = old.corners;
				node->drawPolygon(corners.data(), 4, trailFill(value.hitboxRotatedColor), width, trailLine(value.hitboxRotatedColor), BorderAlignment::Inside);
			}
		});
	}
	if (value.hitboxCircle) {
		eachTrail([&](std::deque<HitboxTrailPoint> const& trail) {
			for (auto const& old : trail) {
				if (!(old.radius > 0.f) || !drawnCircles.insert(quantizeCircle(old.center, old.radius)).second) continue;
				node->drawCircle(old.center, old.radius, trailFill(value.hitboxCircleColor), width, trailLine(value.hitboxCircleColor), 16);
			}
		});
	}
	if (value.hitboxInner) {
		eachTrail([&](std::deque<HitboxTrailPoint> const& trail) {
			for (auto const& old : trail) {
				if (!drawnInner.insert(quantize(old.inner)).second) continue;
				node->drawRect(old.inner, trailFill(value.hitboxInnerColor), width, trailLine(value.hitboxInnerColor), BorderAlignment::Inside);
			}
		});
	}
	float viewX = player ? player->getPositionX() : 0.f;
	float reach = 500.f;
	CCRect view = visibleLayerRect(layer);
	bool useView = std::isfinite(view.origin.x) && std::isfinite(view.origin.y) && view.size.width > 1.f && view.size.height > 1.f;
	if (useView) {
		// Prefer the camera view over the player X. On levels where the camera leads the
		// player (or spawn is far from the first section), a 500px player window skips
		// everything on screen until you catch up.
		viewX = view.getMidX();
		reach = view.size.width * 0.5f + 200.f;
	} else if (!level) {
		auto win = CCDirector::sharedDirector()->getWinSize();
		viewX = (-layer->getPositionX() + win.width * 0.5f) / viewScale;
		reach = win.width / viewScale;
	}
	outlines().collecting = value.hitboxOutlineOnly;
	outlines().shapes.clear();
	if (Logic::asPlay(play)) {
		// GJBaseGameLayer::checkCollisions tests only the objects in m_nonEffectObjects[x][y] (the
		// section's collision list, up to its size), so those are the only hitboxes, and the grid
		// already sorts them into 100-unit sections. Walking only the sections in view replaced a
		// walk over every object in the level each frame.
		auto& grid = play->m_nonEffectObjects;
		auto& sizes = play->m_nonEffectObjectsSizes;
		float minY = useView ? view.getMinY() - 200.f : -1e9f;
		float maxY = useView ? view.getMaxY() + 200.f : 1e9f;
		// addToSection files an object at column m_positionX * m_sectionXFactor.
		float xFactor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
		int firstX = std::max(static_cast<int>(std::floor((viewX - reach) * xFactor)) - 1, 0);
		int lastX = std::min(static_cast<int>(std::floor((viewX + reach) * xFactor)) + 1, static_cast<int>(std::min(grid.size(), sizes.size())) - 1);
		for (int x = firstX; x <= lastX; ++x) {
			auto* column = grid[x];
			auto* counts = sizes[x];
			if (!column || !counts) continue;
			int rows = static_cast<int>(std::min(column->size(), counts->size()));
			// Every row: the row an object is filed in does not follow its height. On Keres every
			// object sits in row 0, even at y 735, so picking rows from the camera height skipped them
			// all once the view rose above y 200 (about 70%). The height check is per object below.
			for (int y = 0; y < rows; ++y) {
				auto* bucket = (*column)[y];
				if (!bucket) continue;
				int count = std::min<int>((*counts)[y], static_cast<int>(bucket->size()));
				for (int i = 0; i < count; ++i) {
					auto* object = (*bucket)[i];
					// GD's collision check (collisionCheckObjects) skips disabled objects, so they
					// have no hitbox: drawing them showed boxes where nothing can kill the player.
					if (!object || object->m_isDisabled) continue;
					if (std::fabs(object->getPositionX() - viewX) > reach || object->getPositionY() < minY || object->getPositionY() > maxY) continue;
					Hitboxes::get().drawObjectHitbox(node, play, object, width, value.fillHitboxes);
				}
			}
		}
		// Objects GD moves every step (addToSection files them by the byte at +0x280) are not in the
		// grid: checkCollisions tests them from m_calcNonEffectObjects instead.
		auto const& moving = play->m_calcNonEffectObjects;
		int movingCount = std::min<int>(play->m_calcNonEffectObjectsSize, static_cast<int>(moving.size()));
		for (int i = 0; i < movingCount; ++i) {
			auto* object = moving[static_cast<std::size_t>(i)];
			if (!object || object->m_isDisabled) continue;
			if (std::fabs(object->getPositionX() - viewX) > reach) continue;
			Hitboxes::get().drawObjectHitbox(node, play, object, width, value.fillHitboxes);
		}
	} else if (play->m_objects) {
		for (auto* object : CCArrayExt<GameObject*>(play->m_objects)) {
			if (!object) continue;
			// GD's collision check (collisionCheckObjects) skips disabled objects, so they have
			// no hitbox: drawing them showed boxes where nothing can kill the player.
			if (object->m_isDisabled) continue;
			if (std::fabs(object->getPositionX() - viewX) > reach) continue;
			Hitboxes::get().drawObjectHitbox(node, play, object, width, value.fillHitboxes);
		}
	}
	if (outlines().collecting) flushOutlines(node, width);
	if (!Logic::get().hitboxPlayerHidden(false)) Hitboxes::get().drawPlayerHitbox(node, play, play->m_player1, width, value.fillHitboxes, nullptr);
	else value.trailP1.clear();
	if (play->m_gameState.m_isDualMode && !Logic::get().hitboxPlayerHidden(true)) {
		Hitboxes::get().drawPlayerHitbox(node, play, play->m_player2, width, value.fillHitboxes, nullptr);
	} else if (Logic::get().hitboxPlayerHidden(true)) {
		value.trailP2.clear();
	}
	Hitboxes::get().drawClicks(node, play, width, value.fillHitboxes);
}

