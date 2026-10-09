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
	std::unordered_set<int> const& triggerHideIds();
	std::unordered_set<int> const& decoHideIds();
	bool hideAsTrigger(GameObject* object);
	bool hideAsDecoration(GameObject* object);
	void rememberHidden(GameObject* object);
	void hideObject(GameObject* object);
	void styleSolidObject(GameObject* object);
	void stylePassableGlow(GameObject* object);
	ccColor3B rgb(float const* color);
};

// Trigger / effect objects hidden in PlayLayer::addObject (Silicate OTHER_OBJECT_IDS).
std::unordered_set<int> const& LevelLayout::triggerHideIds() {
	static std::unordered_set<int> const ids{
		29, 30, 32, 33, 104, 105, 221, 717, 718, 743, 744, 899, 915, 1006, 1007,
		2903, 2904, 2905, 2907, 2909, 2910, 2911, 2912, 2913, 2914, 2915, 2916,
		2917, 2919, 2920, 2921, 2922, 2923, 2924, 3029, 3030, 3031, 3606, 3612,
	};
	return ids;
}

// Decoration hidden in GameObject::addGlow (Silicate OTHER_DECO_IDS).
std::unordered_set<int> const& LevelLayout::decoHideIds() {
	static std::unordered_set<int> const ids{
		902, 943, 944, 945, 946, 947, 948, 949, 950, 951,
		980, 981, 982, 983, 984, 985, 986, 987, 988,
		1023, 1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032,
		1063, 1064, 1065, 1066, 1067, 1068, 1069, 1070, 1071,
	};
	return ids;
}

bool LevelLayout::hideAsTrigger(GameObject* object) {
	return object && LevelLayout::get().triggerHideIds().contains(object->m_objectID);
}

bool LevelLayout::hideAsDecoration(GameObject* object) {
	if (!object) return false;
	if (object->m_objectType == GameObjectType::Decoration || object->m_isNoTouch) return true;
	int id = object->m_objectID;
	if (id >= 506 && id <= 640) return true;
	return LevelLayout::get().decoHideIds().contains(id);
}

void LevelLayout::rememberHidden(GameObject* object) {
	auto& hidden = Logic::get().hiddenDecorations;
	if (std::find(hidden.begin(), hidden.end(), object) == hidden.end()) hidden.push_back(object);
}

void LevelLayout::hideObject(GameObject* object) {
	object->m_isHide = true;
	LevelLayout::get().rememberHidden(object);
}

// Objects Default Art draws in their place (drawLayoutArt): solids, slopes and hazards other than saws.
bool replacedByDefaultArt(GameObject* object) {
	if (!object || !Logic::get().layoutDefaultArt) return false;
	switch (object->m_objectType) {
		case GameObjectType::Solid:
		case GameObjectType::Slope:
			return true;
		// Saws (round hazards) keep their own art.
		case GameObjectType::Hazard:
		case GameObjectType::AnimatedHazard:
			return object->m_objectRadius <= 0.f;
		default:
			return false;
	}
}

// Solids the level itself hides: Default Art leaves them invisible, as the level has them.
std::unordered_set<GameObject*>& levelHidden() {
	static std::unordered_set<GameObject*> objects;
	return objects;
}

// Each block's colour channels as the level gave them (styleSolidObject recolours some), and each
// channel's opacity as the level has it before layout mode makes every channel opaque: a block on
// a channel the level keeps fully transparent is invisible in the level, and Default Art keeps it so.
std::unordered_map<GameObject*, std::pair<int, int>>& levelChannels() {
	static std::unordered_map<GameObject*, std::pair<int, int>> channels;
	return channels;
}

std::vector<float>& levelOpacity() {
	static std::vector<float> opacity;
	return opacity;
}

bool levelInvisible(GameObject* object) {
	auto found = levelChannels().find(object);
	if (found == levelChannels().end()) return false;
	auto const& opacity = levelOpacity();
	auto clear = [&](int channel) { return channel > 0 && static_cast<std::size_t>(channel) < opacity.size() && opacity[static_cast<std::size_t>(channel)] <= 0.001f; };
	auto [base, detail] = found->second;
	return clear(base) && (detail <= 0 || clear(detail));
}

// The objects this file hid for Default Art: styling can run twice on an object, and the second
// pass must not take that Hide for the level's own.
std::unordered_set<GameObject*>& artHidden() {
	static std::unordered_set<GameObject*> objects;
	return objects;
}

// Before a level loads its objects (EchoPlayLayer::init).
void Logic::clearLayoutHidden() {
	levelHidden().clear();
	artHidden().clear();
	levelChannels().clear();
	levelOpacity().clear();
}

void LevelLayout::styleSolidObject(GameObject* object) {
	if (object->m_isHide && !artHidden().contains(object)) levelHidden().insert(object);
	if (!levelChannels().contains(object)) {
		auto channelOf = [](GJSpriteColor* sprite) { return sprite ? (sprite->m_colorID ? sprite->m_colorID : sprite->m_defaultColorID) : 0; };
		levelChannels()[object] = {channelOf(object->m_baseColor), channelOf(object->m_detailColor)};
	}
	object->setOpacity(255);
	object->m_isHide = false;
	// GD's own Hide (the editor option): invisible, still solid. Drawn by drawLayoutArt instead.
	if (replacedByDefaultArt(object)) {
		object->m_isHide = true;
		artHidden().insert(object);
		LevelLayout::get().rememberHidden(object);
	}
	object->m_hasNoGlow = true;
	object->m_activeMainColorID = -1;
	object->m_activeDetailColorID = -1;
	object->m_baseUsesHSV = false;
	object->m_detailUsesHSV = false;
	object->m_isDontFade = true;
	object->m_isDontEnter = true;
	// Hazards are black (GD's black channel 1010, which layout leaves black), whatever the level
	// coloured them: hazards made to blend into the background stayed invisible otherwise.
	// Anything else on the background (1000) or middleground (1013/1014) channels takes the block
	// colour (1004).
	bool hazard = object->m_objectType == GameObjectType::Hazard || object->m_objectType == GameObjectType::AnimatedHazard;
	for (auto* sprite : {object->m_baseColor, object->m_detailColor}) {
		if (!sprite) continue;
		int channel = sprite->m_colorID ? sprite->m_colorID : sprite->m_defaultColorID;
		if (hazard) sprite->m_colorID = 1010;
		else if (channel == 1000 || channel == 1013 || channel == 1014) sprite->m_colorID = 1004;
	}
	auto color = hazard ? ccColor3B{0, 0, 0} : LevelLayout::rgb(Logic::get().blocksColor);
	object->setColor(color);
	object->setChildColor(color);
}

void LevelLayout::stylePassableGlow(GameObject* object) {
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



bool Logic::layoutActive() {
	return Logic::get().layoutMode && LevelEditorLayer::get() == nullptr;
}

void Logic::paintLayoutColors(PlayLayer* play) {
	if (!play || !play->m_effectManager || !Logic::get().layoutActive()) return;

	auto* effects = play->m_effectManager;
	auto const& value = Logic::get();
	constexpr std::array<int, 5> ids{1000, 1001, 1009, 1013, 1014};

	for (int id : ids) {
		auto* action = effects->getColorAction(id);
		if (!action) continue;
		if (id == 1000) action->m_color = LevelLayout::get().rgb(value.backgroundColor);
		else if (id == 1001 || id == 1009) action->m_color = LevelLayout::get().rgb(value.blocksColor);
		else action->m_color = {40, 125, 255};
	}
}

void Logic::paintLayoutChannels(GJEffectManager* effects) {
	if (!effects) return;
	auto const& value = Logic::get();
	auto background = LevelLayout::get().rgb(value.backgroundColor);
	auto blocks = LevelLayout::get().rgb(value.blocksColor);
	auto& sprites = effects->m_colorActionSpriteVector;
	// Once per level, before anything here has written them (layout mode blocks colour triggers, so
	// the level's start colours are the ones it keeps).
	auto& opacity = levelOpacity();
	if (opacity.empty()) {
		opacity.assign(sprites.size(), 1.f);
		for (std::size_t id = 1; id < sprites.size(); ++id)
			if (sprites[id]) opacity[id] = sprites[id]->m_opacity;
	}
	for (std::size_t id = 1; id < sprites.size(); ++id) {
		auto* sprite = sprites[id];
		if (!sprite) continue;
		switch (id) {
			case 1000: sprite->m_color = background; break; // Background
			case 1002: sprite->m_color = {255, 255, 255}; break; // Ground line
			case 1010: sprite->m_color = {0, 0, 0}; break; // Black: hazards (styleSolidObject)
			case 1013:
			case 1014: sprite->m_color = {40, 125, 255}; break; // Middleground
			default: sprite->m_color = blocks; break;
		}
		sprite->m_opacity = 1.f;
	}
}

void Logic::styleLayoutObject(GameObject* object) {
	if (!object || !Logic::get().layoutActive()) return;
	if (LevelLayout::get().hideAsTrigger(object)) {
		LevelLayout::get().hideObject(object);
		return;
	}
	LevelLayout::get().styleSolidObject(object);
}

void Logic::styleLayoutGlow(GameObject* object) {
	if (!object || !Logic::get().layoutActive()) return;
	if (LevelLayout::get().hideAsDecoration(object)) {
		LevelLayout::get().hideObject(object);
		return;
	}
	LevelLayout::get().stylePassableGlow(object);
}

void Logic::applyLayout(PlayLayer* play) {
	auto& value = Logic::get();
	if (!play) return;
	if (!Logic::get().layoutActive()) {
		if (!value.layoutStyled) return;
		for (auto* object : value.hiddenDecorations) {
			if (!object) continue;
			object->m_isHide = false;
		}
		value.hiddenDecorations.clear();
		value.layoutStyled = false;
		return;
	}
	Logic::get().paintLayoutColors(play);
	if (value.layoutStyled || !play->m_objects) return;
	value.layoutStyled = true;
	for (auto* object : CCArrayExt<GameObject*>(play->m_objects)) {
		Logic::get().styleLayoutObject(object);
		Logic::get().styleLayoutGlow(object);
	}
}



// Default Art: every block, slope, spike and saw on screen drawn as the default one (object 1, 289
// or 291, 8, 88), in a layer of Echo's own over the level's objects. Each sprite is sized so the
// default object's hitbox lands on the real object's hitbox, and follows the object's position,
// rotation and flips every frame. The objects themselves only go invisible, so collisions do not
// change.
namespace {
constexpr int kLayoutArtTag = 0x4543484c; // "ECHL"

struct DefaultPiece {
	CCSpriteFrame* frame = nullptr;
	// Its hitbox: width and height, or a radius.
	float width = 0.f, height = 0.f, radius = 0.f;
	// Drawn twice, the second turned 180 degrees: GD keeps the left half of the saw and turns it for
	// the right (mirroring it would turn the teeth the wrong way).
	bool halves = false;
	CCSpriteFrame* fill = nullptr; // Drawn under the frame: a slope's frame is only its outline.
	int layer = 0;                 // 0 blocks, 1 slopes, 2 hazards: later layers draw over earlier.
};

struct DefaultArt {
	DefaultPiece block, spike, saw, slope, wideSlope;
	PlayLayer* measured = nullptr; // The level the hitbox sizes were taken from.
	bool loaded = false;
};

// The frame comes from a created object (its main sprite). createWithKey does not set up the
// object's hitbox (a spike reports 30x30, the saw no radius), so the sizes are GD's defaults unless
// the level has the object itself, whose sizes are then used.
DefaultArt& defaultArt(PlayLayer* play) {
	static DefaultArt art;
	if (!art.loaded) {
		art.loaded = true;
		// The frames GD builds objects 1, 8, 88, 289 and 291 from (GJ_GameSheet). A slope is its fill
		// under its outline; the outline alone (the object's main sprite) looked invisible.
		auto frameOf = [](char const* name) -> CCSpriteFrame* {
			auto* frame = CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(name);
			if (frame) frame->retain();
			return frame;
		};
		art.block = {frameOf("square_01_001.png"), 30.f, 30.f, 0.f, false, nullptr, 0};
		art.spike = {frameOf("spike_01_001.png"), 6.f, 12.f, 0.f, false, nullptr, 2};
		art.saw = {frameOf("sawblade_01_001.png"), 0.f, 0.f, 32.f, true, nullptr, 2};
		art.slope = {frameOf("blockOutline_14_001.png"), 30.f, 30.f, 0.f, false, frameOf("triangle_a_02_001.png"), 1};
		art.wideSlope = {frameOf("blockOutline_15_001.png"), 60.f, 30.f, 0.f, false, frameOf("triangle_a_04_001.png"), 1};
	}
	if (play && art.measured != play && play->m_objects) {
		art.measured = play;
		bool spike = false, saw = false;
		for (auto* object : CCArrayExt<GameObject*>(play->m_objects)) {
			if (!object || (spike && saw)) break;
			float sx = std::abs(object->m_scaleX), sy = std::abs(object->m_scaleY);
			if (sx <= 0.f || sy <= 0.f) continue;
			if (!spike && object->m_objectID == 8 && object->m_width > 0.f && object->m_height > 0.f) {
				spike = true;
				art.spike.width = object->m_width;
				art.spike.height = object->m_height;
			}
			if (!saw && object->m_objectID == 88 && object->m_objectRadius > 0.f) {
				saw = true;
				art.saw.radius = object->m_objectRadius;
			}
		}
	}
	return art;
}

struct ArtLayer {
	std::array<CCNode*, 3> nodes{}; // Blocks, slopes, hazards (DefaultPiece::layer), bottom to top.
	PlayLayer* play = nullptr;
	std::array<std::vector<CCSprite*>, 3> pools;
	std::array<std::size_t, 3> used{};
};

ArtLayer& artLayer() {
	static ArtLayer value;
	return value;
}
}

void Logic::drawLayoutArt(PlayLayer* play) {
	auto& layer = artLayer();
	bool on = play && Logic::get().layoutActive() && Logic::get().layoutDefaultArt && Logic::get().layoutStyled && play->m_objectLayer;
	if (layer.play != play || !on) {
		for (auto* node : layer.nodes) {
			if (node && layer.play == play && node->getParent()) node->removeFromParent();
			if (node) node->release();
		}
		layer = ArtLayer{};
		if (!on) return;
	}
	auto& art = defaultArt(play);
	if (!art.block.frame || !art.spike.frame || !art.saw.frame || !art.slope.frame || !art.wideSlope.frame || !art.slope.fill || !art.wideSlope.fill) return;
	if (!layer.nodes[0]) {
		layer.play = play;
		// One texture for all of them makes batches; otherwise plain nodes.
		auto* texture = art.block.frame->getTexture();
		bool shared = texture;
		for (auto* frame : {art.spike.frame, art.saw.frame, art.slope.frame, art.wideSlope.frame, art.slope.fill, art.wideSlope.fill}) shared = shared && frame->getTexture() == texture;
		// Just under the player, over the level's objects.
		int z = play->m_player1 && play->m_player1->getParent() == play->m_objectLayer ? play->m_player1->getZOrder() - 1 : 0;
		for (auto& node : layer.nodes) {
			node = shared ? static_cast<CCNode*>(CCSpriteBatchNode::createWithTexture(texture)) : CCNode::create();
			node->retain();
			node->setTag(kLayoutArtTag);
			play->m_objectLayer->addChild(node, z);
		}
	}
	layer.used = {};
	// The objects in view, as the hitbox drawing finds them: the collision lists of the sections on
	// screen, every row, and the moving objects GD keeps out of the grid.
	auto win = CCDirector::sharedDirector()->getWinSize();
	auto* objectLayer = play->m_objectLayer;
	CCPoint a = objectLayer->convertToNodeSpace({0.f, 0.f});
	CCPoint b = objectLayer->convertToNodeSpace({win.width, win.height});
	float minX = std::min(a.x, b.x) - 120.f, maxX = std::max(a.x, b.x) + 120.f;
	float minY = std::min(a.y, b.y) - 120.f, maxY = std::max(a.y, b.y) + 120.f;
	auto blockColor = LevelLayout::get().rgb(Logic::get().blocksColor);
	auto next = [&](int which, CCSpriteFrame* frame) -> CCSprite* {
		auto& pool = layer.pools[which];
		auto& used = layer.used[which];
		CCSprite* out = nullptr;
		if (used < pool.size()) {
			out = pool[used];
		} else {
			out = CCSprite::createWithSpriteFrame(frame);
			if (!out) return nullptr;
			layer.nodes[which]->addChild(out);
			pool.push_back(out);
		}
		++used;
		if (!out->isFrameDisplayed(frame)) out->setDisplayFrame(frame);
		out->setVisible(true);
		return out;
	};
	auto place = [&](GameObject* object) {
		if (!object || !replacedByDefaultArt(object) || object->m_isDisabled || object->m_isGroupDisabled) return;
		// GD's hidden anti-cheat spike (put on the player at every spawn and load) and the players'
		// collision blocks are not level objects; the hitbox drawing skips them too.
		if (object == play->m_anticheatSpike || object == play->m_player1CollisionBlock || object == play->m_player2CollisionBlock) return;
		// A block the level keeps invisible (hidden, or no-touch) or the player goes through
		// (passable: GD's fake blocks) stays invisible.
		if (object->m_objectType == GameObjectType::Solid && (object->m_isNoTouch || object->m_isPassable || levelHidden().contains(object) || levelInvisible(object))) {
			object->setOpacity(0);
			return;
		}
		auto pos = object->getPosition();
		if (pos.x < minX || pos.x > maxX || pos.y < minY || pos.y > maxY) return;
		float scaleX = std::abs(object->m_scaleX), scaleY = std::abs(object->m_scaleY);
		bool hazard = object->m_objectType == GameObjectType::Hazard || object->m_objectType == GameObjectType::AnimatedHazard ||
			(object->m_objectType == GameObjectType::Slope && object->m_slopeIsHazard);
		DefaultPiece const* piece = nullptr;
		float sx = scaleX, sy = scaleY;
		bool slope = object->m_objectType == GameObjectType::Slope;
		std::optional<CCRect> blockBox;
		bool slopeFlipX = false, slopeFlipY = false;
		if (slope) {
			// The box the slope's hitbox triangle fills: a quarter turn swaps its sides.
			float w = object->m_width * scaleX, h = object->m_height * scaleY;
			float turns = object->getRotation() / 90.f;
			if (std::abs(turns - std::round(turns)) < 0.001f && static_cast<long>(std::round(turns)) % 2 != 0) std::swap(w, h);
			bool wide = w > h * 1.5f;
			piece = wide ? &art.wideSlope : &art.slope;
			sx = w / piece->width;
			sy = h / piece->height;
			// The default slope's art (outline and fill alike), upright, has its right-angle corner
			// bottom right (checked in game). The triangle the hitbox drawing uses (m_slopeDirection)
			// says where this one's corner is; flips put the art's corner there. A custom slope's own
			// rotation and flips describe its own art, not the default's.
			switch (object->m_slopeDirection) {
				case 0: case 7: break;                                      // Bottom right.
				case 1: case 5: slopeFlipY = true; break;                   // Top right.
				case 3: case 6: slopeFlipX = slopeFlipY = true; break;      // Top left.
				default: slopeFlipX = true; break;                          // Bottom left.
			}
		} else if (hazard && object->m_objectRadius > 0.f) {
			piece = &art.saw;
			sx = sy = object->m_objectRadius * std::max(scaleX, scaleY) / piece->radius;
		} else if (hazard) {
			piece = &art.spike;
			if (object->m_width > 0.f) sx = object->m_width * scaleX / piece->width;
			if (object->m_height > 0.f) sy = object->m_height * scaleY / piece->height;
		} else {
			// A block is drawn on its real hitbox box (what the hitbox drawing shows). m_width and
			// m_height are the object's nominal size: thin pieces (edges, quarter blocks) came out
			// as whole 30x30 blocks where the level has none.
			piece = &art.block;
			auto box = Logic::objectHitbox(object);
			if (box.size.width < 0.5f || box.size.height < 0.5f) {
				object->setOpacity(0);
				return;
			}
			blockBox = box;
		}
		// Opacity 0 after GD's visibility pass (updateVisibility sets it each frame); Hide alone did
		// not hide the objects in play. Their other sprites are drawn apart from them.
		object->setOpacity(0);
		if (object->m_colorSprite) object->m_colorSprite->setVisible(false);
		if (object->m_glowSprite) object->m_glowSprite->setVisible(false);
		// GD's own default look: the art untinted, a black body with a white outline. A slope's fill
		// is black under its white outline.
		auto color = ccColor3B{255, 255, 255};
		// The saw's art is white: GD tints it black.
		if (piece == &art.saw) color = {0, 0, 0};
		(void)hazard;
		(void)blockColor;
		float rotationX = object->getRotationX();
		float rotationY = object->getRotationY();
		bool flipX = object->isFlipX();
		bool flipY = object->isFlipY();
		if (slope) {
			rotationX = rotationY = 0.f;
			flipX = slopeFlipX;
			flipY = slopeFlipY;
		}
		if (blockBox) {
			pos = CCPoint{blockBox->getMidX(), blockBox->getMidY()};
			sx = blockBox->size.width / piece->width;
			sy = blockBox->size.height / piece->height;
			rotationX = rotationY = 0.f;
			flipX = flipY = false;
		}
		// A block turned by a right angle covers the same box upright: all default blocks face the
		// same way, with width and height swapped for a quarter turn.
		if (piece == &art.block && rotationX == rotationY) {
			float turns = rotationX / 90.f;
			float nearest = std::round(turns);
			if (std::abs(turns - nearest) < 0.001f) {
				if (static_cast<long>(nearest) % 2 != 0) std::swap(sx, sy);
				rotationX = rotationY = 0.f;
				flipX = flipY = false;
			}
		}
		auto draw = [&](CCSpriteFrame* frame, float turn) {
			auto* out = next(piece->layer, frame);
			if (!out) return;
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
			layer.pools[piece->layer][layer.used[piece->layer] - 1]->setColor({0, 0, 0});
		}
		draw(piece->frame, 0.f);
		if (piece->halves) draw(piece->frame, 180.f);
	};
	auto& grid = play->m_nonEffectObjects;
	auto& sizes = play->m_nonEffectObjectsSizes;
	float xFactor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f;
	int firstX = std::max(static_cast<int>(std::floor(minX * xFactor)) - 1, 0);
	int lastX = std::min(static_cast<int>(std::floor(maxX * xFactor)) + 1, static_cast<int>(std::min(grid.size(), sizes.size())) - 1);
	for (int x = firstX; x <= lastX; ++x) {
		auto* column = grid[x];
		auto* counts = sizes[x];
		if (!column || !counts) continue;
		int rows = static_cast<int>(std::min(column->size(), counts->size()));
		for (int y = 0; y < rows; ++y) {
			auto* bucket = (*column)[y];
			if (!bucket) continue;
			int count = std::min<int>((*counts)[y], static_cast<int>(bucket->size()));
			for (int i = 0; i < count; ++i) place((*bucket)[i]);
		}
	}
	int moving = std::min<int>(play->m_calcNonEffectObjectsSize, static_cast<int>(play->m_calcNonEffectObjects.size()));
	for (int i = 0; i < moving; ++i) place(play->m_calcNonEffectObjects[static_cast<std::size_t>(i)]);
	for (std::size_t which = 0; which < layer.pools.size(); ++which)
		for (std::size_t i = layer.used[which]; i < layer.pools[which].size(); ++i) layer.pools[which][i]->setVisible(false);
}
