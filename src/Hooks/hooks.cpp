#include "Physics/object_counters.hpp"
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
#include "Hack/orbs.hpp"
#include "GUI/gui.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/level_forecast.hpp"
#include "Physics/collisions.hpp"
#include "Physics/gjbasegamelayer.hpp"
#include "Physics/gravity.hpp"
#include "Physics/player.hpp"
#include "Physics/prediction_art.hpp"
#include "Rendering/dash_end.hpp"
#include "Rendering/dash_bursts.hpp"
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <vector>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PointNode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <cmath>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RandTriggerGameObject.hpp>
#include <Geode/binding/RandTriggerGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/Geode.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#ifdef GEODE_IS_WINDOWS
#include <safetyhook.hpp>
#include <windows.h>
#endif
#include "Logic/input.hpp"
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/binding/PointNode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <imgui-cocos.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/AppDelegate.hpp>





using namespace geode::prelude;

namespace {
// Hooks that only act while the trajectory predicts; otherwise they call straight through.
// Every call still pays Geode's hook dispatch, and the visibility pass makes thousands of
// them each update, so they are installed only while Show Trajectory is enabled.
// Do not recreate Geode/Tulip handlers every displayed frame.
std::vector<Hook*>& predictHooks() {
	static std::vector<Hook*> hooks;
	return hooks;
}
bool predictHooksOn = false;

void holdForPredict(auto& self, char const* name) {
	auto hook = self.getHook(name);
	if (!hook) {
		log::warn("Echo trajectory hook {} not found", name);
		return;
	}
	hook.unwrap()->setAutoEnable(false);
	predictHooks().push_back(hook.unwrap());
}
}

void Logic::syncPredictHooks() {
	bool want = Logic::get().showTrajectory;
	if (want == predictHooksOn) return;
	for (auto* hook : predictHooks()) {
		auto result = want ? hook->enable() : hook->disable();
		if (!result) log::warn("Echo trajectory hook toggle failed: {}", result.unwrapErr());
	}
	predictHooksOn = want;
}

namespace {
constexpr int kDashSpinTag = 12;
// handleButton is on the stack. A press that arrives some other way, such as Maintain Gravity, still has to swift.
bool inOrbButton = false;
// The release and the re-press of a swift. They must not start another one.
bool orbSwift = false;

// The dash icon spins with a frame-time action. stopDashing copies that angle onto the player.
// This copy is advanced once per physics step, and that angle is what the player keeps.
class TickActionManager : public CCActionManager {
public:
	void tick(float dt) { this->update(dt); }
};

TickActionManager* g_dashSpin = nullptr;
bool g_tickPending = true;
int g_halfPasses = 0;

bool dashSpinEnabled() {
	auto& value = Logic::get();
	return !value.predicting && !value.deferPhysics();
}

float dashTickDelta() {
	float tps = static_cast<float>(Logic::get().sampleRate());
	if (!std::isfinite(tps) || tps <= 0.f) tps = 240.f;
	float warp = 1.f;
	if (auto* layer = GJBaseGameLayer::get()) warp = layer->m_gameState.m_timeWarp;
	if (!std::isfinite(warp)) warp = 1.f;
	return 1.f / tps / std::max(warp, 1.f);
}

TickActionManager* dashSpinManager() {
	if (!g_dashSpin) g_dashSpin = new TickActionManager();
	return g_dashSpin;
}

void dashSpinPre() {
	if (!g_tickPending) return;
	g_tickPending = false;
	if (g_dashSpin && dashSpinEnabled()) g_dashSpin->tick(dashTickDelta());
}

void dashSpinPost(bool isHalfTick) {
	if (!isHalfTick) {
		g_halfPasses = 0;
		g_tickPending = true;
		return;
	}
	// Both passes of a half step report the half flag. The second pass does not spin,
	// and the following step still does.
	if (++g_halfPasses >= 2) {
		g_halfPasses = 0;
		g_tickPending = true;
	}
}

void dashSpinReset() {
	g_tickPending = true;
	g_halfPasses = 0;
}

#ifdef GEODE_IS_WINDOWS
// MSVC's delete aborts (c0000417) when a section vector's buffer is not its allocation.
// Leaving a level then dies inside ~GJBaseGameLayer. Drop those buffers first.
struct SectionVec {
	uintptr_t* begin;
	uintptr_t* end;
	uintptr_t* cap;
};

bool canTouch(void const* pointer, size_t bytes, bool write) {
	if (!pointer || bytes == 0) return false;
	auto address = reinterpret_cast<uintptr_t>(pointer);
	if ((address & 7) != 0 || address < 0x10000 || address > 0x00007fffffffffffull) return false;
	MEMORY_BASIC_INFORMATION info{};
	if (VirtualQuery(pointer, &info, sizeof(info)) == 0) return false;
	if (info.State != MEM_COMMIT) return false;
	if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
	auto start = reinterpret_cast<uintptr_t>(info.BaseAddress);
	if (address < start || address + bytes > start + info.RegionSize) return false;
	if (!write) return true;
	auto protect = info.Protect & 0xff;
	return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

bool heapBlockOk(void* begin, void* cap) {
	if (!begin) return true;
	if (!canTouch(begin, sizeof(void*), false)) return false;
	auto start = reinterpret_cast<uintptr_t>(begin);
	auto stop = reinterpret_cast<uintptr_t>(cap);
	if (stop < start) return false;
	auto bytes = (stop - start) & ~uintptr_t{7};
	if (bytes >= 0x40000000ull) return false;
	if (bytes < 0x1000) return true;
	auto* headerSlot = reinterpret_cast<uintptr_t*>(start - 8);
	if (!canTouch(headerSlot, sizeof(uintptr_t), false)) return false;
	auto header = *headerSlot;
	if (header == 0 || (header & 7) != 0 || header >= start) return false;
	auto gap = start - header;
	return gap >= 8 && gap - 8 <= 0x1f;
}

void zeroVec(SectionVec* vec) {
	vec->begin = nullptr;
	vec->end = nullptr;
	vec->cap = nullptr;
}

ptrdiff_t vecCount(SectionVec* vec) {
	if (!vec->begin || !vec->cap || vec->end < vec->begin || vec->cap < vec->begin) return 0;
	auto capacity = vec->cap - vec->begin;
	auto count = vec->end - vec->begin;
	if (count > capacity) count = capacity;
	if (count > 0x100000) return -1;
	return count;
}

void dropGrid(SectionVec* outer) {
	if (!canTouch(outer, sizeof(SectionVec), true)) return;
	if (!outer->begin || !heapBlockOk(outer->begin, outer->cap) || !canTouch(outer->begin, sizeof(uintptr_t), true)) {
		zeroVec(outer);
		return;
	}
	auto columns = vecCount(outer);
	if (columns < 0) {
		zeroVec(outer);
		return;
	}
	for (ptrdiff_t columnIndex = 0; columnIndex < columns; ++columnIndex) {
		auto* column = reinterpret_cast<SectionVec*>(outer->begin[columnIndex]);
		if (!column) continue;
		if (!canTouch(column, sizeof(SectionVec), true) || !column->begin || !heapBlockOk(column->begin, column->cap) || !canTouch(column->begin, sizeof(uintptr_t), true)) {
			outer->begin[columnIndex] = 0;
			continue;
		}
		auto rows = vecCount(column);
		if (rows < 0) {
			outer->begin[columnIndex] = 0;
			continue;
		}
		for (ptrdiff_t rowIndex = 0; rowIndex < rows; ++rowIndex) {
			auto* bucket = reinterpret_cast<SectionVec*>(column->begin[rowIndex]);
			if (!bucket) continue;
			if (!canTouch(bucket, sizeof(SectionVec), true) || !heapBlockOk(bucket->begin, bucket->cap)) {
				column->begin[rowIndex] = 0;
			}
		}
	}
}

void dropBrokenSections(GJBaseGameLayer* layer) {
	auto* base = reinterpret_cast<uint8_t*>(layer);
	dropGrid(reinterpret_cast<SectionVec*>(base + 0x3598));
	dropGrid(reinterpret_cast<SectionVec*>(base + 0x35b0));
	dropGrid(reinterpret_cast<SectionVec*>(base + 0x35c8));
}
#endif
}

// In a level the game locks the cursor while the mouse button is held, so the position ImGui
// reads never moves. Clicks still land. Drags, which the speedhack slider, scrollbars, and the
// window move all are, do not. While the menu is using the mouse, keep the real cursor position.
static bool menuHoldsMouse() {
	auto& imgui = ImGuiCocos::get();
	if (!imgui.isInitialized() || !imgui.isVisible()) return false;
	return ImGui::GetIO().WantCaptureMouse || ImGui::IsAnyItemActive();
}
static void keepMenuCursor() {
#ifdef GEODE_IS_WINDOWS
	if (!menuHoldsMouse()) return;
	auto* view = CCEGLView::get();
	if (!view) return;
	if (view->m_bCursorLocked) view->toggleLockCursor(false);
	if (!view->m_pMainWindow) return;
	auto hwnd = *reinterpret_cast<HWND*>(reinterpret_cast<uintptr_t>(view->m_pMainWindow) + 0x370);
	POINT point;
	if (!hwnd || !GetCursorPos(&point) || !ScreenToClient(hwnd, &point)) return;
	float x = static_cast<float>(point.x);
	float y = static_cast<float>(point.y);
	if (view->m_fFrameZoomFactor != 0.f) {
		x /= view->m_fFrameZoomFactor;
		y /= view->m_fFrameZoomFactor;
	}
	if (view->m_bInRetinaMonitor && view->m_nRetinaFactor == 1) {
		x *= 2.f;
		y *= 2.f;
	}
	view->m_fMouseX = x;
	view->m_fMouseY = y;
#endif
}

// Visual tilt for ship, UFO, wave, and swing. Physics and hitboxes run on the game's rotation.
// Between the last tick of a frame and the next frame the player node holds the visual angle,
// so the icon, its glow, and everything else the game turns with the player draw at that angle.
std::unordered_map<PlayerObject*, float>& tiltFix() {
	static std::unordered_map<PlayerObject*, float> shown;
	return shown;
}
std::unordered_map<PlayerObject*, float>& tiltHeld() {
	static std::unordered_map<PlayerObject*, float> held;
	return held;
}
std::unordered_map<PlayerObject*, float>& tiltLeft() {
	static std::unordered_map<PlayerObject*, float> left;
	return left;
}
bool tiltMode(PlayerObject* player) {
	return player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;
}
bool tiltApplies(PlayerObject* player) {
	return Logic::get().fixTpsRotation && tiltMode(player) && !Trajectory::get().isFake(player);
}
// Respawns, checkpoints, and portals set the rotation outside the turn functions. The visual angle
// starts over from the game's whenever the rotation is not the one the last turn left behind.
float visualTilt(PlayerObject* player) {
	float now = player->getRotation();
	auto left = tiltLeft().find(player);
	auto shown = tiltFix().find(player);
	bool fresh = left == tiltLeft().end() || left->second != now || shown == tiltFix().end();
	return fresh ? now : shown->second;
}
// Runs one of the game's turn functions on the hitbox angle, then again on the visual angle.
// The turn functions only read and set the rotation, so the second run changes nothing else.
template <class Gameplay, class Visual>
void turnBoth(PlayerObject* player, Gameplay&& gameplayTurn, Visual&& visualTurn) {
	float visual = visualTilt(player);
	gameplayTurn();
	float gameplay = player->getRotation();
	player->setRotation(visual);
	visualTurn();
	tiltFix()[player] = player->getRotation();
	tiltLeft()[player] = gameplay;
	player->setRotation(gameplay);
}
void restoreTilt(PlayerObject* player) {
	if (!player) return;
	auto held = tiltHeld().find(player);
	if (held == tiltHeld().end()) return;
	player->setRotation(held->second);
	tiltHeld().erase(held);
}
void presentTilt(PlayerObject* player) {
	if (!player || !tiltApplies(player) || !tiltFix().contains(player)) return;
	float visual = visualTilt(player);
	tiltHeld()[player] = player->getRotation();
	player->setRotation(visual);
}
void clearTilt() {
	tiltFix().clear();
	tiltHeld().clear();
	tiltLeft().clear();
}


namespace echo::rotation {
float gameplay(PlayerObject* player) {
	if (!player) return 0.f;
	auto held = tiltHeld().find(player);
	return held == tiltHeld().end() ? player->getRotation() : held->second;
}
float visual(PlayerObject* player) {
	if (!player) return 0.f;
	return tiltApplies(player) ? visualTilt(player) : gameplay(player);
}
void restore(PlayerObject* player, float gameplayAngle, float visualAngle) {
	if (!player) return;
	// A pending restoreTilt must never put the failed attempt's angle back.
	tiltHeld().erase(player);
	tiltFix().erase(player);
	tiltLeft().erase(player);
	player->setRotation(gameplayAngle);
	if (tiltApplies(player)) {
		tiltFix()[player] = visualAngle;
		tiltLeft()[player] = gameplayAngle;
	}
}
}

class $modify(EchoPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		Logic::get().clearRandTriggers();
		clearTilt();
		if (Trajectory::get().attached()) Trajectory::get().detach();
		Logic::get().resetSafeMode();
		Logic::get().clearBacksteps();
		echo::objects::reseedVariance();
		Logic::get().clearLayoutHidden();
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
		Logic::get().prepareLevelView(this);
		Trajectory::get().attach(this);
		Logic::get().onPlayInit(this);
		// Batched steps use the physics-only player setter only when it is verified and unhooked.
		phys::preparePredictionArt();
		return true;
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		Logic::get().afterFrame(this, dt);
		Logic::get().updateSafeMode(this);
	}

	// updateCamera calls this on every update. It sets the attempt label's X scale, and a
	// CCLabelBMFont rebuilds every glyph sprite on any scale change, even to the same value.
	// No Mirror Portals keeps the label unmirrored (Rendering/no_mirror.cpp).
	void flipArt(bool flip) {
		flip = flip && !Logic::get().noMirrorPortals;
		if (m_attemptLabel && m_attemptLabel->getScaleX() == (flip ? -1.f : 1.f)) return;
		PlayLayer::flipArt(flip);
	}

	void updateVisibility(float dt) {
		PlayLayer::updateVisibility(dt);
		Logic::get().paintLayoutColors(this);
		Logic::get().drawLayoutArt(this);
	}

	void addObject(GameObject* object) {
		if (!Logic::get().layoutActive()) {
			PlayLayer::addObject(object);
			return;
		}
		Logic::get().styleLayoutObject(object);
		PlayLayer::addObject(object);
	}

	void setupHasCompleted() {
		PlayLayer::setupHasCompleted();
		if (Logic::get().renderQueued) {
			Logic::get().renderQueued = false;
			Logic::get().startRender();
		}
	}

	void updateColor(cocos2d::ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, cocos2d::ccHSVValue& copyHSV, int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
		if (Logic::get().layoutActive()) return;
		PlayLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
	}

	void resetLevel() {
		restoreTilt(m_player1);
		restoreTilt(m_player2);
		clearTilt();
		auto& value = Logic::get();
		// A reset during a backstep's replay ends it there, back in the recording state.
		if (!value.backstepping) Logic::get().finishBackstep(this);
		value.carry = 0.f;
		value.respawnFrames = 0;
		value.resetting = true;
		value.restoredCheckpoint = nullptr;
		Logic::get().syncCheckpoints(this);
		Logic::get().captureRestartHolds(this);
		Logic::get().keepLevelSeed = Logic::get().savedCheckpoint(this);
		bool rollSeed = !Logic::get().keepLevelSeed && !value.seedOverride && !value.isPlaying();
		if (!rollSeed) Logic::get().plantLevelSeed(this);
		Logic::get().endSafeModeAttempt(this);
		forecast::invalidate();
		// Every object gets the same variance index on every reset (object_counters.cpp).
		echo::objects::reseedVariance();
		PlayLayer::resetLevel();
		Logic::get().updateSafeMode(this);
		if (Logic::get().keepLevelSeed) Logic::get().restoreCheckpointRandom(this);
		else if (!rollSeed) Logic::get().plantLevelSeed(this);
#ifndef GEODE_IS_WINDOWS
		else Logic::get().rollLiveSeed(this);
#endif
		Logic::get().keepLevelSeed = false;
		value.resetting = false;
		value.present = true;
		Logic::get().onReset(this);
		if (!value.backstepping) Logic::get().trimBacksteps(this);
		dashSpinReset();
		Logic::get().cancelSteps();
		// postUpdate already ran for the death pose this frame. Redraw at the spawn pose now. Inside the
		// scheduler or a step back, the redraw after the frame does it.
		if (!value.inScheduler && !value.backstepping) Logic::get().drawHitboxes(this);
		Logic::get().syncTimeScale();
	}

	void loadFromCheckpoint(CheckpointObject* checkpoint) {
		Logic::get().restoredCheckpoint = checkpoint;
		PlayLayer::loadFromCheckpoint(checkpoint);
		Logic::get().restoreCheckpointRandom(this);
	}

	void storeCheckpoint(CheckpointObject* checkpoint) {
		// Native and Echo checkpoints must both capture the gameplay angle.
		restoreTilt(m_player1);
		restoreTilt(m_player2);
		PlayLayer::storeCheckpoint(checkpoint);
		// Capture only accepted checkpoints, after oldest-checkpoint eviction completes.
		Logic::get().syncCheckpoints(this);
		if (checkpoint && m_checkpointArray && m_checkpointArray->containsObject(checkpoint))
			Logic::get().onCheckpointCreated(this, checkpoint);
	}

	void removeCheckpoint(bool first) {
		PlayLayer::removeCheckpoint(first);
		Logic::get().syncCheckpoints(this);
	}

	void removeAllCheckpoints() {
		PlayLayer::removeAllCheckpoints();
		Logic::get().syncCheckpoints(this);
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (object == m_anticheatSpike) {
			PlayLayer::destroyPlayer(player, object);
			return;
		}
		// Predicting must never kill the live attempt. Solids/slopes sometimes pass the live
		// body into destroyPlayer while checking a fake — count that as the fake dying.
		if (Logic::get().predicting) {
			Trajectory::get().markPredictDeath(player);
			return;
		}
		// Several hazards can hit in one step; only the first counts.
		if (Trajectory::get().markDead(player) || m_player1->m_isDead || m_player2->m_isDead) return;
		// Noclip gets this call on every step the player touches a hazard.
		if (Logic::get().shouldNoclip(this, player)) {
			if (player == m_player1) Logic::get().noclipDeadP1 = true;
			else if (player == m_player2) Logic::get().noclipDeadP2 = true;
			return;
		}
		PlayLayer::destroyPlayer(player, object);
		if (m_playerDied) Logic::get().syncTimeScale();
	}

	void delayedResetLevel() {
		PlayLayer::delayedResetLevel();
		m_resumeTimer = 0;
		Logic::get().clearStuckDeath(this);
	}

	void fullReset() {
		PlayLayer::fullReset();
		m_resumeTimer = 0;
		Logic::get().clearStuckDeath(this);
	}

	void resetLevelFromStart() {
		PlayLayer::resetLevelFromStart();
		m_resumeTimer = 0;
		Logic::get().clearStuckDeath(this);
	}

	void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
		auto& value = Logic::get();
		if (value.predicting || value.showedNewBest) return;
		value.showedNewBest = true;
		PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
	}

	void checkForEnd() {
		// Native checkForEnd latches m_hasCompletedLevel before starting the
		// animation. Blocking only the animation is already too late.
		if (Logic::get().predicting) return;
		PlayLayer::checkForEnd();
	}

	void levelComplete() {
		if (Logic::get().predicting) return;
		// A hack used on the last step still counts toward Safe Mode.
		Logic::get().updateSafeMode(this);
		Logic::get().holdSafeModeTestMode(this);
		Logic::get().onComplete(this);
		PlayLayer::levelComplete();
	}

	void showCompleteEffect() {
		if (Logic::get().renderCapturing()) Logic::get().holdRenderEffects();
		PlayLayer::showCompleteEffect();
		if (Logic::get().renderCapturing()) Logic::get().holdRenderEffects();
	}

	void playEndAnimationToPos(CCPoint position) {
		if (Logic::get().predicting) return;
		PlayLayer::playEndAnimationToPos(position);
	}

	void playPlatformerEndAnimationToPos(CCPoint position, bool instant) {
		if (Logic::get().predicting) return;
		PlayLayer::playPlatformerEndAnimationToPos(position, instant);
	}

	void pauseGame(bool unfocused) {
		PlayLayer::pauseGame(unfocused);
		if (unfocused) Logic::get().onFocusChange();
	}

	void resume() {
		PlayLayer::resume();
		Logic::get().resumeAttempt(this);
	}

	void resumeAndRestart(bool fromStart) {
		PlayLayer::resumeAndRestart(fromStart);
		Logic::get().resumeAttempt(this);
	}

	void onQuit() {
		restoreTilt(m_player1);
		restoreTilt(m_player2);
		clearTilt();
		Logic::get().onQuit(this);
		Trajectory::get().detach();
		PlayLayer::onQuit();
	}
};

class $modify(EchoEditor, LevelEditorLayer) {
	void onPlaytest() {
		Logic::get().beginEditorPlaytest();
		LevelEditorLayer::onPlaytest();
	}

	void onStopPlaytest() {
		LevelEditorLayer::onStopPlaytest();
		Logic::get().endEditorPlaytest(this);
	}

	void updateEditor(float dt) {
		LevelEditorLayer::updateEditor(dt);
		if (Logic::get().showHitboxes && !Logic::get().editorTesting) {
			Logic::get().drawHitboxes(this);
		}
	}

	void postUpdate(float dt) {
		LevelEditorLayer::postUpdate(dt);
		if (Logic::get().hideP1 && m_player1) m_player1->setVisible(false);
		if (Logic::get().hideP2 && m_player2) m_player2->setVisible(false);
		Logic::get().flushClickHitboxes(this);
		Logic::get().drawHitboxes(this);
	}

	void updateDebugDraw() {
		LevelEditorLayer::updateDebugDraw();
		Logic::get().drawHitboxes(this);
	}
};

class $modify(EchoPauseLayer, PauseLayer) {
#ifdef GEODE_IS_MOBILE
	// Touch devices have no key for open-menu.
	void customSetup() {
		PauseLayer::customSetup();
		auto* label = ButtonSprite::create("Echo", "goldFont.fnt", "GJ_button_01.png", .8f);
		label->setScale(.7f);
		auto* button = CCMenuItemExt::createSpriteExtra(label, [](CCMenuItemSpriteExtra*) { GUI::get().openPopup(); });
		auto* menu = CCMenu::createWithItem(button);
		auto const size = CCDirector::sharedDirector()->getWinSize();
		menu->setPosition({size.width - 40.f, size.height - 25.f});
		menu->setID("echo-menu"_spr);
		addChild(menu, 10);
	}
#endif

	void onRestart(CCObject* sender) {
		PauseLayer::onRestart(sender);
		if (auto* play = PlayLayer::get()) Logic::get().resumeAttempt(play);
	}

	void onRestartFull(CCObject* sender) {
		PauseLayer::onRestartFull(sender);
		if (auto* play = PlayLayer::get()) Logic::get().resumeAttempt(play);
	}
};





int vanillaPhysicsSteps(float dt, float timeWarp);

#ifndef GEODE_IS_WINDOWS
// Android heap pointers carry a tag in the top byte.
bool plausibleObject(GameObject const* object) {
	auto bits = reinterpret_cast<uintptr_t>(object) & 0x00ffffffffffffffull;
	return bits >= 0x10000 && bits < 0x0000800000000000ull && (bits & 7) == 0;
}

// The game swap-removes from the slot the object names, using the bucket's separate count.
bool sectionHolds(GJBaseGameLayer* layer, GameObject* object) {
	auto inRange = [](auto const& list, int index) { return index >= 0 && static_cast<size_t>(index) < list.size(); };
	int x = object->m_outerSectionIndex;
	int y = object->m_middleSectionIndex;
	int slot = object->m_someOtherIndex;
	if (!inRange(layer->m_sections, x) || !inRange(layer->m_sectionSizes, x)) return false;
	auto* column = layer->m_sections[x];
	auto* sizes = layer->m_sectionSizes[x];
	if (!column || !sizes || !inRange(*column, y) || !inRange(*sizes, y) || !(*column)[y]) return false;
	auto const& bucket = *(*column)[y];
	int count = (*sizes)[y];
	return count > 0 && static_cast<size_t>(count) <= bucket.size() && slot >= 0 && slot < count && bucket[slot] == object && plausibleObject(bucket[count - 1]);
}
#endif

class $modify(EchoGameLayer, GJBaseGameLayer) {
	static void onModify(auto& self) {
		holdForPredict(self, "GJBaseGameLayer::gameEventTriggered");
		// Move triggers call this for every moved object on every step. Its only guard of its own
		// keeps trajectory players out of sections, so it is installed only while predicting is available.
		// Non-finite positions stay guarded by addToSection and removeObjectFromSection, the only
		// places native updateObjectSection indexes the section grid.
		holdForPredict(self, "GJBaseGameLayer::updateObjectSection");
		// These only redirect trajectory players or skip work while predicting; for the real
		// players they call straight through, so the dispatch is only paid while predicting is possible.
		holdForPredict(self, "GJBaseGameLayer::collisionCheckObjects");
		holdForPredict(self, "GJBaseGameLayer::flipGravity");
		holdForPredict(self, "GJBaseGameLayer::teleportPlayer");
		holdForPredict(self, "GJBaseGameLayer::destroyObject");
		holdForPredict(self, "GJBaseGameLayer::pickupItem");
		(void) self.setHookPriority("GJBaseGameLayer::update", Priority::First);
		// After other mods' destructor hooks, immediately before the game frees the grids.
		(void) self.setHookPriority("GJBaseGameLayer::GJBaseGameLayer", Priority::Last);
		(void) self.setHookPriorityAfter("GJBaseGameLayer::GJBaseGameLayer", "smjs.object-collab");
	}

#ifdef GEODE_IS_WINDOWS
	void destructor() {
		dropBrokenSections(this);
		reinterpret_cast<void (*)(GJBaseGameLayer*)>(geode::base::get() + 0x2057a0)(this);
	}
#endif

	void update(float dt) {
		auto& value = Logic::get();
		if (value.skipPhysics) return;
		// Death has to reach the original update. Stopping here freezes auto-retry.
		if (auto* play = Logic::asPlay(this); play && play->m_playerDied) {
			value.stepsThisTick = static_cast<uint32_t>(vanillaPhysicsSteps(dt, m_gameState.m_timeWarp));
			GJBaseGameLayer::update(dt);
			return;
		}
		auto* play = Logic::asPlay(this);
		auto* editor = Logic::asEditor(this);
		GJBaseGameLayer* layer = play;
		if (!layer && editor && value.editorTesting) layer = editor;
		bool locked = Logic::get().lockDeltaActive();
		if (value.refreshOnly || !layer || Logic::get().deferPhysics()) {
			value.stepsThisTick = static_cast<uint32_t>(vanillaPhysicsSteps(dt, m_gameState.m_timeWarp));
			GJBaseGameLayer::update(dt);
			return;
		}
		if (value.disableShakes || value.layoutMode) m_skipCameraShake = true;
		if (!locked && value.stepsLeft == 0) {
			value.stepsThisTick = static_cast<uint32_t>(std::max(Logic::get().countUnlockedSteps(layer, dt), 0));
			if (value.stepsThisTick == 0) return;
		} else if (!locked) {
			value.stepsThisTick = static_cast<uint32_t>(std::max(value.stepsLeft, 0));
		} else {
			value.stepsThisTick = value.stepsLeft > 0 ? 1 : 0;
		}
		GJBaseGameLayer::update(dt);
	}

	double getModifiedDelta(float dt) {
		double original = GJBaseGameLayer::getModifiedDelta(dt);
		if (auto* play = Logic::asPlay(this); play && play->m_playerDied) return original;
		bool gameplay = Logic::asPlay(this) || (Logic::get().editorTesting && Logic::asEditor(this));
		if (!gameplay) return original;
		if (auto* editor = Logic::asEditor(this); editor && editor->m_playbackActive) return original;
		auto const& value = Logic::get();
		if (value.refreshOnly) return 0.0;
		if (Logic::get().deferPhysics()) return original;
		float warp = std::min(m_gameState.m_timeWarp, 1.f);
		float step = value.physicsStep();
		if (!std::isfinite(warp) || warp < 0.f || !std::isfinite(step) || !(step > 0.f)) return 0.0;
		return static_cast<double>(step) * static_cast<double>(warp) * static_cast<double>(value.stepsThisTick);
	}

	// Sections are 100 units wide and indexed by int. A position far outside any level (an advanced
	// follow pushed to 1e12 at high TPS) made GD size a section vector past its limit and throw.
	template <class Point>
	static bool sectionable(Point const& pos) {
		constexpr float kLimit = 1e7f;
		return std::isfinite(pos.x) && std::isfinite(pos.y) && std::fabs(pos.x) < kLimit && std::fabs(pos.y) < kLimit;
	}

	void addToSection(GameObject* object) {
		if (!object) return;
		auto const pos = object->getPosition();
		if (!sectionable(pos)) return;
		if (Logic::get().isTrajectoryPlayer(object)) return;
		GJBaseGameLayer::addToSection(object);
	}

	void updateObjectSection(GameObject* object) {
		if (!object) return;
		auto const pos = object->getPosition();
		if (!sectionable(pos)) return;
		if (Logic::get().isTrajectoryPlayer(object)) return;
		GJBaseGameLayer::updateObjectSection(object);
	}

#ifdef GEODE_IS_WINDOWS
	// A checkpoint restore moves an object without updating its section. The next physics step
	// then removes it from a slot that no longer holds it, and that slot is -1.
	// Object 1816 also unlinks a collision grid by swapping with the last entry. That entry is
	// tracked by a separate count, and a bad count makes the swap read 0xFFFFFFFFFFFFFFFF.
	void removeObjectFromSection(GameObject* object) {
		if (!object) return;
		auto* raw = reinterpret_cast<uint8_t*>(object);
		auto sectionX = reinterpret_cast<int*>(raw + 0x278);
		auto sectionY = reinterpret_cast<int*>(raw + 0x27c);
		auto slot = reinterpret_cast<int*>(raw + 0x270);
		auto* objectId = reinterpret_cast<int*>(raw + 0x40c);
		auto* blockSlot = reinterpret_cast<int*>(raw + 0x274);
		auto forget = [&] {
			*sectionX = -1;
			*sectionY = -1;
			*slot = -1;
		};
		auto pointerOk = [](void* pointer) {
			auto bits = reinterpret_cast<uintptr_t>(pointer);
			return bits > 0x10000 && (bits & 7) == 0;
		};
		// Native removal reads only this object's bucket, its count, its slot and the last entry,
		// which it moves into the slot. When all of those are sound the repair below changes
		// nothing, and scanning the whole bucket on every removal made levels that move many
		// objects several times slower. Object 1816's block grid always takes the full path.
		if (*objectId != 0x718 && *sectionX >= 0 && *sectionY >= 0 && *slot >= 0) {
			auto* base = reinterpret_cast<uint8_t*>(this);
			auto* columns = *reinterpret_cast<uint8_t***>(base + 0x3598);
			auto* columnsEnd = *reinterpret_cast<uint8_t***>(base + 0x3598 + sizeof(void*));
			auto* sizeColumns = *reinterpret_cast<uint8_t***>(base + 0x3640);
			auto* sizeColumnsEnd = *reinterpret_cast<uint8_t***>(base + 0x3640 + sizeof(void*));
			if (columns && sizeColumns && *sectionX < columnsEnd - columns && *sectionX < sizeColumnsEnd - sizeColumns &&
				columns[*sectionX] && sizeColumns[*sectionX]) {
				auto* column = columns[*sectionX];
				auto* rows = *reinterpret_cast<uint8_t***>(column);
				auto* rowsEnd = *reinterpret_cast<uint8_t***>(column + sizeof(void*));
				auto* sizeColumn = sizeColumns[*sectionX];
				auto* sizeRows = *reinterpret_cast<int**>(sizeColumn);
				auto* sizeRowsEnd = *reinterpret_cast<int**>(sizeColumn + sizeof(void*));
				if (rows && sizeRows && *sectionY < rowsEnd - rows && *sectionY < sizeRowsEnd - sizeRows && rows[*sectionY]) {
					auto* bucket = rows[*sectionY];
					auto* objects = *reinterpret_cast<GameObject***>(bucket);
					auto* objectsEnd = *reinterpret_cast<GameObject***>(bucket + sizeof(void*));
					int stored = objects && objectsEnd > objects ? static_cast<int>(objectsEnd - objects) : 0;
					int used = sizeRows[*sectionY];
					if (used >= 1 && used <= stored && *slot < used && objects[*slot] == object && pointerOk(objects[used - 1])) {
						GJBaseGameLayer::removeObjectFromSection(object);
						return;
					}
				}
			}
		}
		auto compact = [&](GameObject*** beginField, GameObject*** endField, int* count, bool keep) {
			if (!beginField || !endField) {
				*blockSlot = -1;
				return;
			}
			auto* begin = *beginField;
			auto* end = *endField;
			int size = begin && end > begin ? static_cast<int>(end - begin) : 0;
			int used = count ? *count : size;
			if (used < 0) used = 0;
			if (used > size) used = size;
			int write = 0;
			int found = -1;
			bool kept = false;
			if (begin) {
				for (int i = 0; i < used; ++i) {
					auto* item = begin[i];
					if (!pointerOk(item)) continue;
					if (item == object) {
						if (!keep || kept) continue;
						kept = true;
					}
					if (write != i) begin[write] = item;
					auto* index = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(item) + 0x274);
					if (*index != write) *index = write;
					if (item == object) found = write;
					++write;
				}
			}
			if (begin) *endField = begin + write;
			if (count) *count = write;
			*blockSlot = found;
		};
		auto* base = reinterpret_cast<uint8_t*>(this);
		auto repairBlocks = [&](bool keep) {
			if (*objectId != 0x718) return;
			if (raw[0x280] != 0) {
				compact(reinterpret_cast<GameObject***>(base + 0x3600), reinterpret_cast<GameObject***>(base + 0x3608), reinterpret_cast<int*>(base + 0x3618), keep);
				return;
			}
			GameObject*** beginField = nullptr;
			GameObject*** endField = nullptr;
			int* count = nullptr;
			if (*sectionX >= 0 && *sectionY >= 0) {
				auto* columns = *reinterpret_cast<uint8_t***>(base + 0x35c8);
				auto* columnsEnd = *reinterpret_cast<uint8_t***>(base + 0x35c8 + sizeof(void*));
				auto* sizeColumns = *reinterpret_cast<uint8_t***>(base + 0x3670);
				auto* sizeColumnsEnd = *reinterpret_cast<uint8_t***>(base + 0x3670 + sizeof(void*));
				if (columns && sizeColumns && *sectionX < columnsEnd - columns && *sectionX < sizeColumnsEnd - sizeColumns && columns[*sectionX] && sizeColumns[*sectionX]) {
					auto* column = columns[*sectionX];
					auto* rows = *reinterpret_cast<uint8_t***>(column);
					auto* rowsEnd = *reinterpret_cast<uint8_t***>(column + sizeof(void*));
					auto* sizeColumn = sizeColumns[*sectionX];
					auto* sizeRows = *reinterpret_cast<int**>(sizeColumn);
					auto* sizeRowsEnd = *reinterpret_cast<int**>(sizeColumn + sizeof(void*));
					if (rows && sizeRows && *sectionY < rowsEnd - rows && *sectionY < sizeRowsEnd - sizeRows && rows[*sectionY]) {
						auto* bucket = rows[*sectionY];
						beginField = reinterpret_cast<GameObject***>(bucket);
						endField = reinterpret_cast<GameObject***>(bucket + sizeof(void*));
						count = &sizeRows[*sectionY];
					}
				}
			}
			compact(beginField, endField, count, keep);
		};
		bool mainOk = *sectionX >= 0 && *sectionY >= 0 && *slot >= 0;
		if (mainOk) {
			auto* columns = *reinterpret_cast<uint8_t***>(base + 0x3598);
			auto* columnsEnd = *reinterpret_cast<uint8_t***>(base + 0x3598 + sizeof(void*));
			auto* sizeColumns = *reinterpret_cast<uint8_t***>(base + 0x3640);
			auto* sizeColumnsEnd = *reinterpret_cast<uint8_t***>(base + 0x3640 + sizeof(void*));
			mainOk = columns && sizeColumns && *sectionX < columnsEnd - columns && *sectionX < sizeColumnsEnd - sizeColumns && columns[*sectionX] && sizeColumns[*sectionX];
			if (mainOk) {
				auto* column = columns[*sectionX];
				auto* rows = *reinterpret_cast<uint8_t***>(column);
				auto* rowsEnd = *reinterpret_cast<uint8_t***>(column + sizeof(void*));
				auto* sizeColumn = sizeColumns[*sectionX];
				auto* sizeRows = *reinterpret_cast<int**>(sizeColumn);
				auto* sizeRowsEnd = *reinterpret_cast<int**>(sizeColumn + sizeof(void*));
				mainOk = rows && sizeRows && *sectionY < rowsEnd - rows && *sectionY < sizeRowsEnd - sizeRows && rows[*sectionY];
				if (mainOk) {
					auto* bucket = rows[*sectionY];
					auto* objects = *reinterpret_cast<GameObject***>(bucket);
					auto* objectsEnd = *reinterpret_cast<GameObject***>(bucket + sizeof(void*));
					auto* count = &sizeRows[*sectionY];
					int stored = objects && objectsEnd > objects ? static_cast<int>(objectsEnd - objects) : 0;
					int used = *count;
					if (used < 0) used = 0;
					if (used > stored) used = stored;
					// The game swap-removes with this count. A -1 in that slot is read as an object.
					int write = 0;
					for (int i = 0; i < used; ++i) {
						auto* item = objects[i];
						if (!pointerOk(item)) continue;
						if (write != i) objects[write] = item;
						auto* index = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(item) + 0x270);
						if (*index != write) *index = write;
						++write;
					}
					*count = write;
					mainOk = write > 0 && *slot >= 0 && *slot < write && objects[*slot] == object && pointerOk(objects[write - 1]);
				}
			}
		}
		repairBlocks(mainOk);
		if (!mainOk) {
			forget();
			return;
		}
		int savedId = *objectId;
		bool spoof = savedId == 0x718 && *blockSlot < 0;
		if (spoof) *objectId = 0;
		GJBaseGameLayer::removeObjectFromSection(object);
		if (spoof) *objectId = savedId;
	}
#else
	// Parking a trajectory player removes an object that was never added, and a checkpoint restore
	// can leave a stale slot. Either one swap-removes some other object and corrupts the bucket.
	void removeObjectFromSection(GameObject* object) {
		if (!object) return;
		if (sectionHolds(this, object)) {
			GJBaseGameLayer::removeObjectFromSection(object);
			return;
		}
		object->m_someOtherIndex = -1;
		object->m_outerSectionIndex = -1;
		object->m_middleSectionIndex = -1;
	}
#endif

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		auto& value = Logic::get();
		if (value.refreshOnly) return;
		if (value.predicting) return;
		if (!value.predicting) Orbs::beginStep();
		dashSpinPre();
		auto* play = Logic::asPlay(this);
		auto* editor = Logic::asEditor(this);
		GJBaseGameLayer* layer = play;
		if (!layer && editor && value.editorTesting) layer = editor;
		if (layer && !value.predicting) Logic::get().noteStep();
		if (play && !value.predicting) {
			Logic::get().keepVariance(play);
			value.noclipDeadP1 = false;
			value.noclipDeadP2 = false;
		}
		// Respawn arms a few empty steps after resetLevel returns. Those read as a delay on retry.
		if (m_resumeTimer > 0 && m_resumeTimer <= 8) m_resumeTimer = 0;
		if (play && !m_playerDied && m_resumeTimer <= 0) {
			Logic::get().stepObjectStates(play);
			Logic::get().saveBackstep(play);
		}
		// Follow Player runs in levels and in editor playtests.
		if (layer && !m_playerDied && m_resumeTimer <= 0) Logic::get().runFollow(layer);

		value.inStep = true;
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		value.inStep = false;
		if (play) forecast::noteLiveStep();

		if (!layer) {
			dashSpinPost(isHalfTick);
			return;
		}
		if (m_resumeTimer > 0) {
			dashSpinPost(isHalfTick);
			return;
		}

		if (!m_playerDied) {
			value.stepFrame++;
			if (play && !value.progressBaseSet) {
				value.progressBase = static_cast<int>(m_gameState.m_currentProgress) - value.stepFrame;
				value.progressBaseSet = true;
			}
			Logic::get().applyRate(value.stepFrame);
			if (!value.hitboxTrailVisual) Logic::get().sampleHitboxTrail(layer);
		}
		if (value.ignoreRecordFrames > 0) --value.ignoreRecordFrames;

		value.buttonsPrepared = false;
		if (play) Logic::get().tickAutoclicker(play);
		dashSpinPost(isHalfTick);
	}

	void resetLevelVariables() {
		GJBaseGameLayer::resetLevelVariables();
		m_queuedButtons.clear();
		if (Logic::get().editorTesting && Logic::asEditor(this)) {
			auto& value = Logic::get();
			value.stepFrame = 0;
			value.carry = 0.f;
			value.buttonsPrepared = false;
			value.followLog.clear();
			if (value.isPlaying()) Logic::get().setReplayPos(nullptr);
		}
	}

	void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int length, float dt) {
		if (!Trajectory::get().isFake(player)) {
			GJBaseGameLayer::collisionCheckObjects(player, objects, length, dt);
			return;
		}
		phys::collisionCheckObjects(this, player, objects, length, dt);
	}

	void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
		if (Trajectory::get().isFake(player)) {
			phys::flipGravity(player, flip, true);
			return;
		}
		GJBaseGameLayer::flipGravity(player, flip, noEffects);
	}

	void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
		if (Trajectory::get().isFake(player)) {
			phys::teleportPlayer(this, object, player);
			return;
		}
		GJBaseGameLayer::teleportPlayer(object, player);
	}

	void destroyObject(GameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::destroyObject(object);
	}

	void pickupItem(EffectGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::pickupItem(object);
	}

	void processQueuedButtons(float dt, bool clearInputQueue) {
		if (Logic::get().predicting) return;
		if (auto* play = Logic::asPlay(this)) {
			Logic::get().pumpButtons(play, this, dt, clearInputQueue);
			return;
		}
		if (Logic::get().editorTesting && Logic::asEditor(this)) {
			Logic::get().pumpButtons(this, this, dt, clearInputQueue);
			return;
		}
		GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
	}

	void handleButton(bool down, int button, bool player1) {
		if (Logic::get().predicting) return;
		auto& value = Logic::get();
		// Opening the pause menu releases the hold. Playback keeps the hold it already has.
		auto* live = Logic::asPlay(this);
		if (!down && ((live && live->m_isPaused) || Logic::get().pauseMenuOpen()) && (value.isPlaying() || value.isRecording())) return;
		int jumpEarly = static_cast<int>(PlayerButton::Jump);
		bool jumpButtonEarly = button == 0 || button == jumpEarly;
		// A still-held click/key re-fires after resetLevel. Swallow those downs until a real release.
		if (value.suppressAutoHold && jumpButtonEarly && !value.applyingPlayback && !value.restoringHolds && !value.maintainingGravity) {
			if (!down) {
				value.suppressAutoHold = false;
			} else {
				if (auto* ui = m_uiLayer) {
					if (player1) {
						ui->m_p1Jumping = false;
						ui->m_p1TouchId = -1;
					} else {
						ui->m_p2Jumping = false;
						ui->m_p2TouchId = -1;
					}
				}
				return;
			}
		}
		// A touch-up from the former second dual icon must close the same P1
		// hold as recordInput, before native handling can leave the primary held.
		if (!down && !player1 && value.isRecording() && !value.applyingPlayback &&
			!value.restoringHolds && !value.maintainingGravity &&
			!m_gameState.m_isDualMode && !(m_levelSettings && m_levelSettings->m_twoPlayerMode)) {
			int key = button == 0 ? static_cast<int>(PlayerButton::Jump) : button;
			if (!Logic::get().heldAt(value, true, value.stepFrame + 1, key) &&
				Logic::get().heldAt(value, false, value.stepFrame + 1, key)) player1 = true;
		}
		// Follow Player: either player's button drives the leader; runFollow gives the follower the copy.
		{
			int jumpKey = static_cast<int>(PlayerButton::Jump);
			bool moveButton = button == 0 || button == jumpKey || button == static_cast<int>(PlayerButton::Left) || button == static_cast<int>(PlayerButton::Right);
			if (moveButton && !value.followInjecting && !value.restoringHolds && !value.maintainingGravity && !value.injecting && Logic::get().followActive(this)) {
				bool leader1 = value.followMode == 1;
				if (!value.applyingPlayback) {
					player1 = leader1;
					value.noteFollowPress(this, down, button == 0 ? jumpKey : button);
				} else if (value.backstepTarget >= 0 && player1 == leader1) {
					// A backstep's replay brings the leader's presses back for the copies after it.
					value.noteFollowPress(this, down, button == 0 ? jumpKey : button);
				}
			}
		}
		bool openedButton = !inOrbButton;
		inOrbButton = true;
		bool steered = false;
		if (!value.applyingPlayback && !value.routingInput && !value.restoringHolds && !value.maintainingGravity) {
			steered = Logic::get().steerPlayer(player1);
			bool otherPlayed = value.playsPlayer(player1);
			if (!steered && value.clickBoth && !otherPlayed && !value.playsPlayer(!player1) && (button == 0 || button == static_cast<int>(PlayerButton::Jump) || button == static_cast<int>(PlayerButton::Left) || button == static_cast<int>(PlayerButton::Right))) {
				value.routingInput = true;
				handleButton(down, button, !player1);
				value.routingInput = false;
			}
		}
		int jump = static_cast<int>(PlayerButton::Jump);
		int left = static_cast<int>(PlayerButton::Left);
		int right = static_cast<int>(PlayerButton::Right);
		bool jumpButton = button == 0 || button == jump;
		int swifts = 0;
		if (down && jumpButton && !orbSwift && !value.applyingPlayback && !value.injecting && !value.restoringHolds && !value.maintainingGravity) {
			PlayerObject* body = player1 ? m_player1 : m_player2;
			swifts = Orbs::claimClick(this, body, !player1);
		}
		if (swifts > 0) Orbs::armTool(true);
		if ((button == 0 || button == jump) && !orbSwift && !value.applyingPlayback && !value.maintainingGravity && !value.restoringHolds && !value.resetting && !down) {
			value.restartHoldCaptured = false;
			value.deathJumpP1 = value.deathJumpP2 = false;
		}
		if (button == jump && !value.suppressClickHitbox && !value.maintainingGravity && !value.restoringHolds) {
			Trajectory::get().noteButton(player1, down);
			if (auto* play = Logic::asPlay(this)) {
				// A one-player level has no player 2: a dual there moves both icons with player 1's
				// click, and the second player's key presses player 1 too. Click Both's mirrored
				// press is the same click again.
				bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
				if (twoPlayer) Logic::get().playClick(play, down, player1);
				else if (!Logic::get().routingInput) Logic::get().playClick(play, down, true);
			}
		}
		if (auto* play = Logic::asPlay(this)) {
			auto& value = Logic::get();
			// Routed mirror clicks are not a second player's press. Count only the real button.
			if (!value.injecting && !orbSwift && !value.routingInput) Logic::get().noteClick(play, down, player1, button);
			if (!value.applyingPlayback && value.isRecording() && !value.injecting && !value.restoringHolds) {
				Logic::get().recordInput(play, down, player1, button);
			}
		} else if (Logic::get().editorTesting && Logic::asEditor(this)) {
			auto& value = Logic::get();
			if (!value.applyingPlayback && value.isRecording() && !value.injecting && !value.restoringHolds) {
				Logic::get().recordInput(this, down, player1, button);
			}
		}
		bool oneReplay = value.applyingPlayback && value.playP1 != value.playP2;
		bool icon2 = !player1;
		if (value.maintainingGravity || steered || oneReplay) {
			if (steered) Logic::get().setRoutedHold(icon2, button, down);
			Logic::get().pressIcon(this, icon2, down, button);
		} else {
			GJBaseGameLayer::handleButton(down, button, player1);
		}
		// The orb swift's release and re-press are not the player's inputs. Drawing them shows a second release.
		if ((button == 0 || button == jump || button == left || button == right) && !orbSwift && !value.maintainingGravity && !value.suppressClickHitbox && !value.restoringHolds) {
			Logic::get().noteClickHitbox(this, player1, down);
		}
		if (swifts > 0) {
			Orbs::armTool(false);
			orbSwift = true;
			bool wasRouting = value.routingInput;
			value.routingInput = true;
			// One release+press per stacked orb. Each is recorded so playback can take the same stack.
			for (int i = 0; i < swifts; ++i) {
				handleButton(false, jump, player1);
				handleButton(true, jump, player1);
			}
			value.routingInput = wasRouting;
			orbSwift = false;
		}
		if (openedButton) inOrbButton = false;
	}

	void activateSFXTrigger(SFXTriggerGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::activateSFXTrigger(object);
	}

	void activateSFXEditTrigger(SFXTriggerGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::activateSFXEditTrigger(object);
	}

	void activateSongTrigger(SongTriggerGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::activateSongTrigger(object);
	}

	void activateSongEditTrigger(SongTriggerGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::activateSongEditTrigger(object);
	}

	void activatedAudioTrigger(SFXTriggerGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::activatedAudioTrigger(object);
	}

	void shakeCamera(float duration, float strength, float interval) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::shakeCamera(duration, strength, interval);
	}

	void gameEventTriggered(GJGameEvent event, int material, int playerID) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::gameEventTriggered(event, material, playerID);
	}

	void checkpointActivated(CheckpointGameObject* object) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::checkpointActivated(object);
	}

	CCParticleSystemQuad* spawnParticle(char const* plist, int zOrder, tCCPositionType positionType, CCPoint position) {
		if (Logic::get().predicting) return nullptr;
		return GJBaseGameLayer::spawnParticle(plist, zOrder, positionType, position);
	}

	void spawnParticleTrigger(int particleID, CCPoint position, float rotation, float scale) {
		if (Logic::get().predicting) return;
		GJBaseGameLayer::spawnParticleTrigger(particleID, position, rotation, scale);
	}

	void createBackground(int background) {
		if (Logic::get().layoutActive() && !Logic::get().layoutUseRegularBg) background = 13;
		GJBaseGameLayer::createBackground(background);
	}

	void createMiddleground(int middleground) {
		if (!Logic::get().layoutActive() || !Logic::get().layoutUseRegularBg) {
			GJBaseGameLayer::createMiddleground(middleground);
			return;
		}
		if (m_middleground) {
			m_middleground->removeFromParent();
			m_middleground = nullptr;
		}
	}

	void createGroundLayer(int ground, int line) {
		if (!Logic::get().layoutActive() || !Logic::get().layoutUseRegularBg) {
			GJBaseGameLayer::createGroundLayer(ground, line);
			return;
		}
		GJBaseGameLayer::createGroundLayer(18, 2);
	}

	void updateColor(cocos2d::ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, cocos2d::ccHSVValue& copyHSV, int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
		if (Logic::get().layoutActive()) return;
		GJBaseGameLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
	}

	void triggerGradientCommand(GradientTriggerObject* object) {
		if (Logic::get().layoutActive()) return;
		GJBaseGameLayer::triggerGradientCommand(object);
	}
};

class $modify(EchoUILayer, UILayer) {
	bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) {
		if (Logic::get().pauseMenuOpen()) return false;
		if (Logic::get().suppressAutoHold) return false;
		return UILayer::ccTouchBegan(touch, event);
	}

	void handleKeypress(cocos2d::enumKeyCodes key, bool down, double timestamp) {
		Input::noteKey(key, down);
		if (Logic::get().steerKey(m_gameLayer, key, down, timestamp)) return;
		UILayer::handleKeypress(key, down, timestamp);
	}

	void visit() {
		UILayer::visit();
	}
};




class $modify(EchoPlayerObject, PlayerObject) {
	struct Fields {
		Ref<CCNode> tickSpin;
		// Step of the last dash end effect this player built. Only read above 240 TPS.
		int64_t dashEffectStep = std::numeric_limits<int64_t>::min();
	};

	void stopTickSpin() {
		if (auto* node = m_fields->tickSpin.data()) dashSpinManager()->removeAllActionsFromTarget(node);
	}

	void updateDashArt() {
		PlayerObject::updateDashArt();
		stopTickSpin();
		if (Trajectory::get().isFake(this) || !dashSpinEnabled() || !m_isDashing || !m_iconSprite) return;
		auto* spin = m_iconSprite->getActionByTag(kDashSpinTag);
		if (!spin) return;
		if (!m_fields->tickSpin) m_fields->tickSpin = CCNode::create();
		auto* node = m_fields->tickSpin.data();
		node->setRotation(m_iconSprite->getRotation());
		auto* copy = static_cast<CCAction*>(spin->copy());
		copy->autorelease();
		dashSpinManager()->addAction(copy, node, false);
	}

	void update(float dt) {
		if (Trajectory::get().isFake(this) && !Logic::get().predicting) return;
		if (!Trajectory::get().isFake(this)) Logic::get().capturePendingClick(this);
		if (Logic::get().suppressAutoHold && !Trajectory::get().isFake(this)) {
			auto& value = Logic::get();
			auto found = m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
			if (found != m_holdingButtons.end() && found->second) PlayerObject::releaseButton(PlayerButton::Jump);
			m_jumpBuffered = false;
			// Latch clears once every jump source is up, including mouse (touch ids may already be cleared).
			bool keyDown = Input::jumpKeyDown();
			bool mouseDown = Input::mouseDown();
			bool uiDown = false;
			if (auto* layer = GJBaseGameLayer::get()) {
				if (auto* ui = layer->m_uiLayer) {
					uiDown = ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
				}
			}
			if (!keyDown && !mouseDown && !uiDown) value.suppressAutoHold = false;
		}
		Logic::get().reinforceRoutedHold(this);
		PlayerObject::update(dt);
		if (!Trajectory::get().isFake(this)) Trajectory::get().setDelta(dt);
		// The swing fire bursts are aimed opposite the rotation while this runs.
		if (m_isSwing && tiltApplies(this) && tiltFix().contains(this) && !tiltHeld().contains(this)) {
			float turn = visualTilt(this) - getRotation();
			for (auto* burst : std::array{m_swingBurstParticles1, m_swingBurstParticles2}) {
				if (burst) burst->setAngle(burst->getAngle() - turn);
			}
		}
	}

	// The game only turns toward the flight direction when the step moved at least sqrt(1.2 * dt)
	// since the last one, which a high TPS step never does. The visual turn looks back far enough to pass.
	void updateShipRotation(float dt) {
		restoreTilt(this);
		if (!tiltApplies(this)) {
			tiltFix().erase(this);
			PlayerObject::updateShipRotation(dt);
			return;
		}
		turnBoth(this, [this, dt] { PlayerObject::updateShipRotation(dt); }, [this, dt] {
			auto pos = getPosition();
			auto back = pos - m_shipRotation;
			float moved = back.x * back.x + back.y * back.y;
			float need = dt * 1.25f + 0.01f;
			auto saved = m_shipRotation;
			if (moved > 1e-8f && moved < need) m_shipRotation = pos - back * std::sqrt(need / moved);
			PlayerObject::updateShipRotation(dt);
			m_shipRotation = saved;
		});
	}

	// Slopes turn the player through here, both from the collision pass and from updateRotation,
	// which skips updateShipRotation while on a slope.
	void updateRotation(float dt, float rotation) {
		restoreTilt(this);
		if (!tiltApplies(this)) {
			PlayerObject::updateRotation(dt, rotation);
			return;
		}
		auto turn = [this, dt, rotation] { PlayerObject::updateRotation(dt, rotation); };
		turnBoth(this, turn, turn);
	}

	// The trail, ship fire, and particles are placed here from the main layer's world transform,
	// so they follow whatever rotation the node has while this runs.
	void setPosition(CCPoint const& position) {
		if (Trajectory::get().isFake(this) && phys::fastPredictionPosition()) {
			phys::positionWithoutArt(this, position);
			return;
		}
		// Inside a multi-step update only the last step's art placement is drawn; runFrame
		// places it once after the batch from the final position.
		if (auto& value = Logic::get(); value.deferPlayerArt && !Trajectory::get().isFake(this)) {
			phys::positionWithoutArt(this, position);
			value.playerArtDeferred = true;
			return;
		}
		if (!tiltApplies(this) || !tiltFix().contains(this) || tiltHeld().contains(this)) {
			PlayerObject::setPosition(position);
			return;
		}
		float gameplay = getRotation();
		setRotation(visualTilt(this));
		PlayerObject::setPosition(position);
		setRotation(gameplay);
	}

	void setVisible(bool visible) {
		auto* layer = GJBaseGameLayer::get();
		if (layer && !Logic::get().isTrajectoryPlayer(this)) {
			if (this == layer->m_player1 && Logic::get().hideP1) visible = false;
			if (this == layer->m_player2 && Logic::get().hideP2) visible = false;
		}
		PlayerObject::setVisible(visible);
	}

	void ringJump(RingObject* ring, bool skipCheck) {
		if (Orbs::blockHoldRing(this, ring)) return;
		if (Trajectory::get().isFake(this)) {
			phys::ringJump(this, ring);
			return;
		}
		if (!Logic::get().playerHidden(this)) {
			PlayerObject::ringJump(ring, skipCheck);
			return;
		}
		bool effects = m_playEffects;
		m_playEffects = false;
		PlayerObject::ringJump(ring, skipCheck);
		m_playEffects = effects;
	}

	void bumpPlayer(float force, int objectType, bool playEffect, GameObject* object) {
		if (Trajectory::get().isFake(this)) {
			if (Logic::get().predicting) {
				if (auto* play = PlayLayer::get()) Orbs::notePredictionPad(play, this, object);
			}
			phys::bumpPlayer(this, force, objectType, playEffect, object);
			return;
		}
		if (auto* play = PlayLayer::get()) Orbs::clickPads(play, this, objectType, object);
		PlayerObject::bumpPlayer(force, objectType, playEffect, object);
	}

	void propellPlayer(float force, bool noEffects, int objectType) {
		if (Trajectory::get().isFake(this)) {
			if (Logic::get().predicting) {
				if (auto* play = PlayLayer::get()) Orbs::notePredictionPad(play, this, m_lastActivatedPortal);
			}
			phys::propellPlayer(this, force, noEffects, objectType);
			return;
		}
		if (auto* play = PlayLayer::get()) Orbs::clickPads(play, this, objectType, m_lastActivatedPortal);
		PlayerObject::propellPlayer(force, noEffects, objectType);
	}

	void startDashing(DashRingObject* ring) {
		if (Trajectory::get().isFake(this)) {
			phys::startDashing(this, ring);
			return;
		}
#ifdef GEODE_IS_WINDOWS
		// The burst sprite this press makes can stand in for identical ones (dash_bursts.cpp).
		CCNode* objectLayer = nullptr;
		if (auto* play = PlayLayer::get()) objectLayer = play->m_objectLayer;
		unsigned before = objectLayer && objectLayer->getChildren() ? objectLayer->getChildren()->count() : 0;
		PlayerObject::startDashing(ring);
		if (objectLayer && ring) echo::bursts::noteStart(objectLayer, before, ring->m_objectID);
#else
		PlayerObject::startDashing(ring);
#endif
	}

	void stopDashing() {
		if (Trajectory::get().isFake(this)) {
			phys::stopDashing(this);
			return;
		}
		bool wasDashing = m_isDashing;
		auto* node = m_fields->tickSpin.data();
		bool useTick = dashSpinEnabled() && wasDashing && m_iconSprite && node && dashSpinManager()->numberOfRunningActionsInTarget(node) > 0;
		if (useTick) m_iconSprite->setRotation(node->getRotation());
		stopTickSpin();
#ifdef GEODE_IS_WINDOWS
		// Each release on a dash orb builds an end effect, so at high TPS a macro can stack
		// tens of thousands of them in the object layer. As sprites, drawing and fading them was
		// most of the frame. Echo can draw the same effects on the GPU instead; the limit
		// shares CPU state for identical effects while preserving every native blend.
		using echo::dash::EndEffect;
		auto mode = EndEffect::Native;
		if (wasDashing && echo::dash::endEffectSkippable(this)) {
			auto& value = Logic::get();
			double tps = value.sampleRate();
			// Comparison always records every effect. If the GPU path is unavailable, retain
			// native effects rather than dropping them to satisfy the requested limit.
			// Echo picks sprites or the GPU path itself, from how often effects come (preferGpu).
			bool gpu = echo::dash::preferGpu();
			// Identical effects share CPU state; the draw is bit-identical, so it is always on.
			bool merge = tps > kVanillaTps && gpu && !echoDashCompare && echo::dash::mergeReady();
			if (merge) echo::dash::setMerge(true);
			if (echoDashCompare && echo::dash::gpuEffectsReady()) mode = EndEffect::Record;
			else if (gpu) mode = EndEffect::RecordOnly;
		}
		echo::dash::setEndEffect(mode);
		PlayerObject::stopDashing();
		echo::dash::setEndEffect(EndEffect::Native);
		echo::dash::setMerge(false);
#else
		PlayerObject::stopDashing();
#endif
	}

	void incrementJumps() {
		if (Trajectory::get().drawing()) return;
		PlayerObject::incrementJumps();
	}

	void notePartner() {
		auto* layer = GJBaseGameLayer::get();
		if (layer && this == layer->m_player2 && !Trajectory::get().isFake(this)) ++Logic::get().partnerPresses;
	}

	void releaseButton(PlayerButton button) {
		auto* play = PlayLayer::get();
		auto& value = Logic::get();
		if (play && !Trajectory::get().isFake(this) && (value.isPlaying() || value.isRecording()) && (play->m_isPaused || Logic::get().pauseMenuOpen())) return;
		notePartner();
		PlayerObject::releaseButton(button);
	}

	bool pushButton(PlayerButton button) {
		auto& value = Logic::get();
		if (value.suppressAutoHold && button == PlayerButton::Jump && !Trajectory::get().isFake(this) && !value.maintainingGravity && !value.applyingPlayback && !value.injecting) {
			return false;
		}
		notePartner();
		bool track = button == PlayerButton::Jump && !Trajectory::get().isFake(this);
		if (track) Orbs::beginPush(this);
		auto finish = [&](bool pressed) {
			if (track) Orbs::endPush(this);
			return pressed;
		};
		// Maintain Gravity applies the click here, never through handleButton, so the swift has to be recorded from this press.
		GJBaseGameLayer* layer = nullptr;
		bool player1 = true;
		int swifts = 0;
		if (track && !inOrbButton && !orbSwift && value.maintainingGravity && !value.applyingPlayback && !value.injecting && !value.restoringHolds && !value.predicting && !value.suppressRings) {
			layer = GJBaseGameLayer::get();
			if (layer) {
				player1 = this != layer->m_player2;
				swifts = Orbs::claimClick(layer, this, !player1);
			}
		}
		if (swifts > 0) Orbs::armTool(true);
		bool stash = value.suppressRings && button == PlayerButton::Jump && m_touchingRings && m_touchingRings->count() > 0;
		bool pressed = false;
		if (!stash) {
			pressed = finish(PlayerObject::pushButton(button));
		} else {
			std::vector<cocos2d::CCObject*> rings;
			rings.reserve(m_touchingRings->count());
			for (unsigned int i = 0; i < m_touchingRings->count(); ++i) {
				auto* ring = m_touchingRings->objectAtIndex(i);
				ring->retain();
				rings.push_back(ring);
			}
			m_touchingRings->removeAllObjects();
			pressed = PlayerObject::pushButton(button);
			for (auto* ring : rings) {
				m_touchingRings->addObject(ring);
				ring->release();
			}
			pressed = finish(pressed);
		}
		if (swifts <= 0) return pressed;
		Orbs::armTool(false);
		orbSwift = true;
		int jump = static_cast<int>(PlayerButton::Jump);
		for (int i = 0; i < swifts; ++i) {
			releaseButton(PlayerButton::Jump);
			if (layer && value.isRecording()) Logic::get().recordInput(layer, false, player1, jump);
			pushButton(PlayerButton::Jump);
			if (layer && value.isRecording()) Logic::get().recordInput(layer, true, player1, jump);
		}
		orbSwift = false;
		return pressed;
	}

	void releaseAllButtons() {
		auto* layer = GJBaseGameLayer::get();
		auto* play = Logic::asPlay(layer);
		auto& value = Logic::get();
		if (value.resetting || value.suppressAutoHold || !play) {
			PlayerObject::releaseAllButtons();
			return;
		}
		if ((value.isPlaying() || value.isRecording()) && (play->m_isPaused || Logic::get().pauseMenuOpen())) return;
		bool second = this == play->m_player2;
		bool only2 = value.recordsPlayer(true) && !value.recordsPlayer(false);
		bool only1 = value.recordsPlayer(false) && !value.recordsPlayer(true);
		if ((second && only2) || (!second && only1 && this == play->m_player1)) return;
		PlayerObject::releaseAllButtons();
	}

	void playDeathEffect() {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this) || Logic::get().disableDeathEffect) return;
		PlayerObject::playDeathEffect();
	}

	void playerDestroyed(bool noEffects) {
		auto* play = PlayLayer::get();
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this)) {
			m_isDead = true;
			return;
		}
		if (play && Logic::get().shouldNoclip(play, this)) {
			m_isDead = false;
			bool otherDead = (play->m_player1 && play->m_player1 != this && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2 != this && play->m_player2->m_isDead);
			if (!otherDead) play->m_playerDied = false;
			return;
		}
		PlayerObject::playerDestroyed(noEffects);
	}

	void playBumpEffect(int objectType, GameObject* object) {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::playBumpEffect(objectType, object);
	}

	void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::playSpiderDashEffect(from, to);
	}

	void playSpawnEffect() {
		if (Logic::get().predicting || Logic::get().backstepping || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::playSpawnEffect();
	}

	void spawnCircle() {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::spawnCircle();
	}

	void spawnDualCircle() {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::spawnDualCircle();
	}

	void spawnPortalCircle(cocos2d::ccColor3B color, float startRadius) {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::spawnPortalCircle(color, startRadius);
	}

	void spawnScaleCircle() {
		if (Logic::get().predicting || Logic::get().isTrajectoryPlayer(this) || Logic::get().playerHidden(this)) return;
		PlayerObject::spawnScaleCircle();
	}

	void activateStreak() {
		if (Logic::get().playerHidden(this)) return;
		PlayerObject::activateStreak();
	}

	void placeStreakPoint() {
		if (Trajectory::get().isFake(this)) {
			// Native placeStreakPoint clears this latch even when no trail is drawn.
			// Keep that state transition without appending invisible wave points.
			m_justPlacedStreak = false;
			return;
		}
		if (Logic::get().playerHidden(this)) return;
		PlayerObject::placeStreakPoint();
	}
};






class $modify(EchoScheduler, CCScheduler) {
	void update(float dt) {
		Logic::get().syncPredictHooks();
		keepMenuCursor();
		auto& value = Logic::get();
		if (value.refreshOnly) {
			CCScheduler::update(dt);
			return;
		}
		auto* play = PlayLayer::get();
		if (value.editorTesting && !LevelEditorLayer::get()) {
			value.editorTesting = false;
			value.editorPaused = false;
		}
		GJBaseGameLayer* layer = play;
		if (!layer && value.editorTesting) layer = LevelEditorLayer::get();
		if (layer) {
			restoreTilt(layer->m_player1);
			restoreTilt(layer->m_player2);
		}
		bool pauseMenu = Logic::get().pauseMenuOpen();
		// Releasing every frame made clicks land and drags impossible: the speedhack slider,
		// scrollbars, and moving the menu all need the button to stay down.
		static bool wasPaused = false;
		if (pauseMenu && play) {
			play->m_isPaused = true;
			if (!wasPaused) GUI::get().releaseStuckMouse();
		}
		wasPaused = pauseMenu && play;
		if (value.resumePending && play && !pauseMenu) {
			value.resumePending = false;
			play->m_isPaused = false;
			play->m_resumeTimer = 0;
			play->m_extraDelta = 0.0;
			value.carry = 0.f;
			if (auto* director = CCDirector::sharedDirector()) director->setNextDeltaTimeZero(false);
			Logic::get().retainPlaybackHold(play);
		}
		if (Logic::get().renderCapturing() && value.frameAdvance) {
			value.frameAdvance = false;
			value.holdingAdvance = false;
			value.advanceQueued = false;
		}
		Logic::get().pollAdvanceHold();
		Logic::get().pollBackHold();
		Logic::get().syncUnrounding();
		Logic::get().syncTimeScale();
		Logic::get().syncPlaybackFps();
		Logic::get().syncMusicPitch();

		// GD places a queued practice checkpoint at the end of the next physics step, one frame after
		// the one on screen when it was asked for. Place it before that step runs instead.
		if (play && !pauseMenu && play->m_tryPlaceCheckpoint && play->m_started && play->m_player1 && !play->m_playerDied) {
			play->m_tryPlaceCheckpoint = false;
			play->markCheckpoint();
		}

		value.saveShownStep = value.frameAdvance && value.backstep;
		int framesBefore = value.stepFrame;
		value.inScheduler = true;
		Logic::get().runFrame(layer, dt, [this](float delta) { CCScheduler::update(delta); });
		value.inScheduler = false;
		value.saveShownStep = false;
		if (layer) {
			bool paused = value.editorPaused;
			bool dead = false;
			if (auto* live = Logic::asPlay(layer)) {
				paused = paused || live->m_isPaused;
				dead = live->m_playerDied;
			}
			if (!paused) {
				// A frame that ran no step (frame advance waiting, a held pause) is not a new trail
				// sample: counting it aged the trail out while the player stood still.
				if (value.hitboxTrailVisual && !dead && value.stepFrame != framesBefore) {
					++value.visualFrame;
					Logic::get().sampleHitboxTrail(layer);
				}
			}
			if (auto* live = Logic::asPlay(layer); live && !paused && !dead) Logic::get().noteShownCamera(live, value.stepFrame > framesBefore);
			// The one hitbox redraw per displayed frame (steps and resets inside the frame skip theirs).
			// It also hides them once Show Hitboxes is off.
			Logic::get().drawHitboxes(layer);
		} else if (auto* editor = LevelEditorLayer::get(); editor && value.showHitboxes) {
			Logic::get().drawHitboxes(editor);
		}
		if (value.refreshQueued) Logic::get().flushVisualRefresh();
		Logic::get().flushClicks();
		if (layer) {
			presentTilt(layer->m_player1);
			presentTilt(layer->m_player2);
		}
	}
};



class $modify(EchoGameObject, GameObject) {
	static void onModify(auto& self) {
		holdForPredict(self, "GameObject::activateObject");
		holdForPredict(self, "GameObject::deactivateObject");
		holdForPredict(self, "GameObject::playShineEffect");
	}

	void activateObject() {
		if (Logic::get().predicting) return;
		GameObject::activateObject();
	}

	void deactivateObject(bool deactivate) {
		if (Logic::get().predicting) return;
		GameObject::deactivateObject(deactivate);
	}

	void playShineEffect() {
		if (Logic::get().predicting) return;
		GameObject::playShineEffect();
	}

	void addGlow(gd::string frame) {
		GameObject::addGlow(std::move(frame));
		Logic::get().styleLayoutGlow(this);
	}
};

class $modify(EchoEffectObject, EffectGameObject) {
	void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
		// A forecast pass (Hack/level_forecast.cpp) fires the motion triggers ahead of the player on
		// its shadow effect manager. Dropping those left it with only the moves already running when
		// it was built: spikes stood still in predictions until they moved live (Keres).
		if (Logic::get().predicting && !forecast::firing()) return;
		EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
	}

	void triggerActivated(float xPosition) {
		if (Logic::get().predicting) return;
		EffectGameObject::triggerActivated(xPosition);
	}
};

class $modify(EchoRandTrigger, RandTriggerGameObject) {
	struct Fields {
		uint64_t state = std::numeric_limits<uint64_t>::max();
	};

	void customObjectSetup(gd::vector<gd::string>& values, gd::vector<void*>& exists) {
		RandTriggerGameObject::customObjectSetup(values, exists);
		Logic::get().noteRandTrigger(m_uniqueID, &m_fields->state);
	}

	void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
		if (m_objectID != 2068) {
			RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
			return;
		}
		if (m_fields->state == std::numeric_limits<uint64_t>::max()) {
			m_fields->state = Logic::get().levelSeed ^ (static_cast<uint64_t>(m_uniqueID) * 2137ull);
		}
		uint64_t roll = m_fields->state;
		m_fields->state = 214013ull * roll + 2531011ull;
		int sum = 0;
		for (auto const& chance : m_chanceObjects) sum += chance.m_chance;
		int threshold = static_cast<int>(std::round((static_cast<float>((roll >> 16) & 0x7FFF) / 32767.f) * static_cast<float>(sum)));
		int running = 0;
		int group = 0;
		for (size_t i = 0; i < m_chanceObjects.size(); ++i) {
			running += m_chanceObjects[i].m_chance;
			group = m_chanceObjects[i].m_groupID;
			if (running >= threshold) break;
		}
		gd::vector<int> keys;
		if (remapKeys) keys = *remapKeys;
		layer->spawnGroup(group, false, 0.0, keys, m_uniqueID, m_controlID);
	}
};




class $modify(EchoEnhancedObject, EnhancedGameObject) {
	void activatedByPlayer(PlayerObject* player) {
		if (Trajectory::get().isFake(player)) {
			phys::activateForTrajectory(static_cast<EffectGameObject*>(static_cast<GameObject*>(this)), player);
			return;
		}
		if (!player || !Logic::get().playerHidden(player)) {
			EnhancedGameObject::activatedByPlayer(player);
			return;
		}
		bool effects = player->m_playEffects;
		player->m_playEffects = false;
		EnhancedGameObject::activatedByPlayer(player);
		player->m_playEffects = effects;
	}
};




class $modify(EchoRingObject, RingObject) {
	void spawnCircle() {
		if (Logic::get().predicting) return;
		auto* play = PlayLayer::get();
		if (play) {
			auto caused = [&](PlayerObject* player) {
				return player && Logic::get().playerHidden(player) && player->m_touchingRings && player->m_touchingRings->containsObject(this);
			};
			if (caused(play->m_player1) || caused(play->m_player2)) return;
		}
		RingObject::spawnCircle();
	}
};





#ifdef GEODE_IS_WINDOWS
// GJBaseGameLayer::update derives its physics iteration count from dt. Silicate replaces that
// calculation with a direct load of the step count (GPL-3.0, GD 2.2081). A midhook here saves
// and restores every register, which changes the ship delta and costs a full frame.
void installPhysicsStepPatch() {
	std::vector<uint8_t> bytes = {
		0x49, 0xBB,
		0, 0, 0, 0, 0, 0, 0, 0,
		0x45, 0x8B, 0x1B,
		0xE9, 0x31, 0x00, 0x00, 0x00
	};
	auto* count = &Logic::get().stepsThisTick;
	std::memcpy(bytes.data() + 2, &count, sizeof(count));
	auto address = reinterpret_cast<void*>(geode::base::get() + 0x237A5B);
	if (!geode::Mod::get()->patch(address, bytes)) log::error("Echo physics steps patch failed");
}
#endif

int vanillaPhysicsSteps(float dt, float timeWarp) {
	float warp = std::min(timeWarp, 1.f);
	if (!std::isfinite(warp) || !(warp > 0.f)) warp = 1.f;
	if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
	int steps = static_cast<int>(std::lround(static_cast<double>(dt) * 60.0 / static_cast<double>(warp) * 4.0));
	return std::max(steps, 1);
}

#ifdef GEODE_IS_ANDROID64
void installAndroidPhysicsStepPatch() {
	// Verified against the phone's ARM64 libcocos2dcpp.so. Replace only the count
	// calculation; keep time warp in s1 and all three delta divisions at 0x89da30.
	constexpr std::array<uint32_t, 10> original = {
		0x1e62100a, 0x1e6a096a, 0x1e624140, 0x1e264009, 0x1e2e1000,
		0x1e202130, 0x5400758d, 0x1e380120, 0x1e620000, 0xb900e3a0
	};
	auto* address = reinterpret_cast<void*>(geode::base::get() + 0x89d9f8);
	if (std::memcmp(address, original.data(), sizeof(original)) != 0) {
		log::error("Echo ARM64 physics instructions differ; keeping vanilla TPS fallback");
		return;
	}
	// ldr x0, literal; ldr w0, [x0]; clamp count to >=1; scvtf d0, w0;
	// str w0, [x29,#0xe0]; b 0x89da20; nop; pointer literal.
	std::array<uint32_t, 10> words = {
		0x58000100, 0xb9400000, 0x7100041f, 0x1a9fa400,
		0x1e620000, 0xb900e3a0, 0x14000004, 0xd503201f, 0, 0
	};
	auto* count = &Logic::get().stepsThisTick;
	static_assert(sizeof(count) == 8);
	std::memcpy(words.data() + 8, &count, sizeof(count));
	std::vector<uint8_t> bytes(sizeof(words));
	std::memcpy(bytes.data(), words.data(), sizeof(words));
	if (!Mod::get()->patch(address, bytes)) {
		log::error("Echo ARM64 physics steps patch failed; keeping vanilla TPS fallback");
		return;
	}
	kStepPatch = true;
}
#endif

#ifdef GEODE_IS_WINDOWS
safetyhook::MidHook g_levelSeed;

// PlayLayer::resetLevel writes the attempt's rand state here. Recording keeps the value the game
// chose. Playback puts the macro's seed back so shake, teleport, and random triggers match.
// The offset is Silicate's for GD 2.2081.
void onLevelSeed(safetyhook::Context&) {
	auto* play = PlayLayer::get();
	if (!play) return;
	auto& value = Logic::get();
	if (value.keepLevelSeed) return;
	if (!value.isPlaying() && !value.seedOverride) {
		Logic::get().rollLiveSeed(play);
		return;
	}
	if (value.seedOverride) {
		value.levelSeed = value.seedOverrideValue;
		value.levelSeedKnown = true;
	}
	if (value.levelSeedKnown) {
		Logic::get().setGameRandomState(value.levelSeed);
		value.shakeRandom = value.levelSeed & 0x7FFF;
		value.teleportRandom = value.levelSeed & 0x7FFF;
		Logic::get().applyAttemptVariance(play, value.levelSeed);
		Logic::get().reseedRandTriggers(value.levelSeed);
		return;
	}
	uint64_t state = Logic::get().gameRandomState();
	value.levelSeed = state;
	value.levelSeedKnown = true;
	value.shakeRandom = state & 0x7FFF;
	value.teleportRandom = state & 0x7FFF;
	Logic::get().reseedRandTriggers(state);
}
#endif

$execute {
#ifdef GEODE_IS_ANDROID64
	installAndroidPhysicsStepPatch();
#endif
#if defined(GEODE_IS_WINDOWS)
	installPhysicsStepPatch();
	g_levelSeed = safetyhook::create_mid(geode::base::get() + 0x3b90fb, onLevelSeed);
	if (!g_levelSeed) log::error("Echo level seed hook failed");

	auto shakeStep = [](safetyhook::Context& ctx) {
		auto& state = Logic::get().shakeRandom;
		state = static_cast<uint64_t>(static_cast<int>((214013ull * state + 2531011ull) >> 16) & 0x7FFF);
		ctx.rax = static_cast<uintptr_t>(state);
		ctx.rip += 6;
	};
	auto teleportStep = [](safetyhook::Context& ctx) {
		auto& state = Logic::get().teleportRandom;
		state = static_cast<uint64_t>(static_cast<int>((214013ull * state + 2531011ull) >> 16) & 0x7FFF);
		ctx.rax = static_cast<uintptr_t>(state);
	};
	static safetyhook::MidHook shakeA;
	static safetyhook::MidHook shakeB;
	static safetyhook::MidHook shakeC;
	static safetyhook::MidHook shakeD;
	static safetyhook::MidHook teleport;
	shakeA = safetyhook::create_mid(geode::base::get() + 0x23E173, shakeStep);
	shakeB = safetyhook::create_mid(geode::base::get() + 0x23E1A1, shakeStep);
	shakeC = safetyhook::create_mid(geode::base::get() + 0x23E1CB, shakeStep);
	shakeD = safetyhook::create_mid(geode::base::get() + 0x23E1E9, shakeStep);
	teleport = safetyhook::create_mid(geode::base::get() + 0x20FEDC, teleportStep);

	auto patch = [](uintptr_t offset, std::vector<uint8_t> bytes, char const* name) {
		if (!Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset), bytes)) log::error("Failed to patch {}", name);
	};
	// Taken directly from Silicate (GPL-3.0): these offsets and bytes are Silicate's findings for GD 2.2081.
	patch(0x23B4EA, std::vector<uint8_t>(24, 0x90), "GJBaseGameLayer::resetLevelVariables");
	patch(0x4CD95C, std::vector<uint8_t>(19, 0x90), "UILayer::handleKeypress");
	patch(0x3B994C, {0xEB, 0x5E}, "PlayLayer::resetLevel");
	patch(0x3BA508, std::vector<uint8_t>(5, 0x90), "PlayLayer::resumeAndRestart");
#endif
}



class $modify(EchoShaderLayer, ShaderLayer) {
	void performCalculations() {
		if (Logic::get().layoutActive()) {
			m_state.m_usesShaders = false;
			return;
		}
		ShaderLayer::performCalculations();
	}
};




class $modify(EchoEffectManager, GJEffectManager) {
	void updateEffects(float dt) {
		if (Logic::get().layoutActive()) {
			m_pulseEffectMap.clear();
			m_pulseEffectVector.clear();
			m_opacityEffectMap.clear();
		}
		GJEffectManager::updateEffects(dt);
		if (Logic::get().layoutActive()) Logic::get().paintLayoutChannels(this);
	}

	// Alpha triggers fade groups through this (preUpdateVisibility reads it per object). Layout
	// hides the triggers but they still ran, so hazards faded to see-through over the background.
	// Opacity only changes drawing.
	float opacityModForGroup(int id) {
		if (Logic::get().layoutActive()) return 1.f;
		return GJEffectManager::opacityModForGroup(id);
	}
};

// Spamming the wave at a high TPS turns every tick into a corner, and the trail redraws every
// corner on screen each frame. A corner closer than about a pixel to the last one cannot be seen,
// so it is left out of the trail.
class $modify(EchoHardStreak, HardStreak) {
	void addPoint(CCPoint point) {
		auto* points = m_pointArray;
		auto* last = points && points->count() > 0 ? static_cast<PointNode*>(points->lastObject()) : nullptr;
		if (!last) {
			HardStreak::addPoint(point);
			return;
		}
		auto* layer = GJBaseGameLayer::get();
		float zoom = layer ? layer->m_gameState.m_cameraZoom : 1.f;
		if (!std::isfinite(zoom) || zoom < 0.05f) zoom = 1.f;
		float reach = 0.25f / zoom;
		if (ccpDistanceSQ(last->m_point, point) < reach * reach) return;
		HardStreak::addPoint(point);
	}
};



class $modify(EchoDirector, CCDirector) {
	void drawScene() {
		auto& logic = Logic::get();
		if (auto* play = PlayLayer::get(); play && (logic.showHitboxes || logic.showTrajectory)) {
			logic.syncOverlaySeats(play);
		}
		if (!logic.renderCapturing()) {
			auto started = std::chrono::steady_clock::now();
			CCDirector::drawScene();
			logic.noteFrameWork(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
			return;
		}
		auto started = std::chrono::steady_clock::now();
		auto advance = [&] {
			bool capture = logic.beginRenderFrame();
			if (capture) logic.renderFrameGameStep = logic.renderGameStep();
			if (!m_bPaused && m_pScheduler) m_pScheduler->update(capture ? logic.renderFrameGameStep : logic.renderPaused() ? 0.f : logic.renderWallDelta());
			if (auto* play = PlayLayer::get(); play && capture && (logic.showHitboxes || logic.showTrajectory)) {
				logic.syncOverlaySeats(play);
			}
			bool switched = m_pNextScene != nullptr;
			if (switched) setNextScene();
			return capture && logic.captureRenderFrame() && !switched;
		};
		while (advance() && logic.renderBatchMore(started)) {}
		logic.showRenderOverlay();
		m_pobOpenGLView->swapBuffers();
	}
};




#ifdef GEODE_IS_WINDOWS
// The window keeps its real size while a render holds the view at the video size.
class $modify(EchoEGLView, CCEGLView) {
	void setFrameSize(float width, float height) {
		if (Logic::get().renderHoldsView()) return;
		CCEGLView::setFrameSize(width, height);
	}

	void onGLFWframebuffersize(GLFWwindow* window, int width, int height) {
		if (Logic::get().renderHoldsView()) return;
		CCEGLView::onGLFWframebuffersize(window, width, height);
	}

	void onGLFWWindowSizeFunCallback(GLFWwindow* window, int width, int height) {
		if (Logic::get().renderHoldsView()) return;
		CCEGLView::onGLFWWindowSizeFunCallback(window, width, height);
	}

	void toggleLockCursor(bool locked) {
		if (locked && menuHoldsMouse()) locked = false;
		CCEGLView::toggleLockCursor(locked);
	}
};
#endif




class $modify(EchoAudioEngine, FMODAudioEngine) {
	void update(float dt) {
		if (Logic::get().renderBlocksEngine()) return;
		FMODAudioEngine::update(dt);
	}
};

// While rendering, FMOD only mixes when the renderer asks for the next frame's audio.
FMOD_RESULT echoMixerUpdate(FMOD::System* self) {
	if (Logic::get().renderBlocksMixer()) return FMOD_OK;
	return self->update();
}

$execute {
	(void)Mod::get()->hook(
		reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::update)),
		&echoMixerUpdate,
		"FMOD::System::update",
		tulip::hook::TulipConvention::Stdcall
	);
}




class $modify(EchoAppDelegate, AppDelegate) {
	void applicationDidEnterBackground() {
		Logic::get().saveSettingsFile();
		Logic::get().onFocusChange();
		AppDelegate::applicationDidEnterBackground();
	}

	void trySaveGame(bool force) {
		Logic::get().saveSettingsFile();
		AppDelegate::trySaveGame(force);
	}

	void applicationWillEnterForeground() {
		AppDelegate::applicationWillEnterForeground();
		Logic::get().onFocusChange();
	}
};
