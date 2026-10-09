#include "Logic/logic.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

// Safe Mode: an attempt that uses any of Echo's hacks makes no level progress. GD's own test mode
// (what start positions use) makes levelComplete and destroyPlayer's new-best check save nothing.
// It is only set for those: test mode also lifts the first attempt's camera hold (updateCamera keeps
// the camera's left edge at 15 while m_unk322a is set and m_isTestMode is not), so holding it the
// whole attempt cut the camera to a later attempt's on the first step. From levelComplete on it stays
// set for the end screen; the original value comes back at the next reset.
namespace {
bool g_cheated = false;
bool g_applied = false;
bool g_savedTestMode = false;
// The hacks used so far this attempt, in first-use order.
std::vector<std::string_view> g_used;
}

// Every hack Safe Mode counts, by the name the menus give it.
std::vector<std::string_view> Logic::activeCheats() const {
	auto const& value = *this;
	std::vector<std::string_view> out;
	auto add = [&](bool on, std::string_view name) {
		if (on) out.push_back(name);
	};
	add(value.noclipP1 || value.noclipP2, "Noclip");
	add(value.speedhack != 1.f, "Speedhack");
	add(value.sampleRate() != static_cast<double>(kVanillaTps), "TPS");
	add(value.frameAdvance, "Frame Advance");
	add(value.pauseBeforeKill, "Prevent Death");
	add(value.showDeathNext, "Death Next Frame");
	add(value.backstep, "Backstep");
	add(value.unroundedPhysics(), "Velocity Unrounding");
	add(value.pauseAtTrajectoryLength, "Pause At Trajectory Length");
	add(value.swapPlayers, "Swap Player Inputs");
	add(value.inverseP1 || value.inverseP2, "Inverse Inputs");
	add(value.maintainGravity, "Maintain Gravity");
	add(value.sameGravity, "Same Gravity");
	add(value.noMirrorPortals, "No Mirror Portals");
	add(value.seedOverride, "Seed Override");
	add(value.isPlaying(), "Macro Playback");
	add(value.autoclicker, "Autoclicker");
	add(value.clickBoth, "Click Both");
	add(value.followMode != 0, "Follow Player");
	add(value.clickDashOrbs, "Click Dash Orbs");
	add(value.clickBlackOrbs, "Click Black Orbs");
	add(value.padBlackOrb, "Auto Black Orb on Pad");
	add(value.padBlueOrb, "Auto Blue Orb on Pad");
	add(value.showTrajectory, "Trajectory");
	// Hitbox Trail is part of Hitboxes and draws nothing without it (drawHitboxes).
	add(value.showHitboxes, "Hitboxes");
	add(value.layoutMode, "Layout");
	add(value.hideP1 || value.hideP2, "Hide Player");
	return out;
}

std::vector<std::string_view> const& Logic::attemptCheats() const {
	return g_used;
}

bool Logic::attemptCheated() const {
	return g_cheated;
}

void Logic::updateSafeMode(PlayLayer* play) {
	if (!play) return;
	if (!safeMode) return;
	for (auto name : activeCheats()) {
		g_cheated = true;
		if (std::ranges::find(g_used, name) == g_used.end()) g_used.push_back(name);
	}
}

// From levelComplete to the next reset.
void Logic::holdSafeModeTestMode(PlayLayer* play) {
	if (!play || !g_cheated || g_applied) return;
	g_savedTestMode = play->m_isTestMode;
	play->m_isTestMode = true;
	g_applied = true;
}

// Before the native reset: the attempt's own test-mode value, so start positions behave as usual.
void Logic::endSafeModeAttempt(PlayLayer* play) {
	g_cheated = false;
	g_used.clear();
	if (g_applied && play) play->m_isTestMode = g_savedTestMode;
	g_applied = false;
}

void Logic::resetSafeMode() {
	g_cheated = false;
	g_used.clear();
	g_applied = false;
}

class $modify(SafeModePlayLayer, PlayLayer) {
	void destroyPlayer(PlayerObject* player, GameObject* object) {
		Logic::get().updateSafeMode(this);
		if (!Logic::get().attemptCheated() || m_isTestMode) {
			PlayLayer::destroyPlayer(player, object);
			return;
		}
		m_isTestMode = true;
		PlayLayer::destroyPlayer(player, object);
		m_isTestMode = false;
	}
};

