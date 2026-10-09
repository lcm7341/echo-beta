#include "Logic/logic.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/label_kinds.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <unordered_set>




using namespace geode::prelude;


class Labels {
public:
	static Labels& get() {
		static Labels value;
		return value;
	}
	void syncLabel(PlayLayer* play, int tag, bool visible, LabelPlacement const& place, std::string const& text, ccColor3B color);
	// Auto Position: stacks the shown labels from the chosen corner, in the chosen order.
	void layout(PlayLayer* play);
	std::string formatClock(double seconds);
	std::string font;
	std::unordered_map<int, CCTextAlignment> alignments;
};

constexpr int kFrameTag = 0xEC01;
constexpr int kCpsTag = 0xEC02;
constexpr int kPercentTag = 0xEC03;
constexpr int kTimeTag = 0xEC04;
constexpr int kRecordTag = 0xEC05;
constexpr int kDebugTag = 0xEC07;
constexpr int kPositionTag = 0xEC08;
constexpr int kVelocityTag = 0xEC09;
constexpr int kAliveTag = 0xEC0A;
constexpr int kMaxCpsTag = 0xEC0B;
constexpr int kGravityTag = 0xEC0C;
constexpr int kDeathTag = 0xEC0D;
constexpr int kUtilityTag = 0xEC0E;
constexpr int kSafeModeTag = 0xEC0F;

namespace {
LabelInfo const kLabels[] = {
	{"Show Recording Label", "recording", &Session::showRecording, &Session::recordingLabel, kRecordTag},
	{"Show CPS", "cps", &Session::showCps, &Session::cpsLabel, kCpsTag},
	{"Show Frame", "frame", &Session::showFrame, &Session::frameLabel, kFrameTag},
	{"Show Time", "time", &Session::showTime, &Session::timeLabel, kTimeTag},
	{"Show Position", "position", &Session::showPosition, &Session::positionLabel, kPositionTag},
	{"Show Velocity", "velocity", &Session::showVelocity, &Session::velocityLabel, kVelocityTag},
	{"Show Alive", "alive", &Session::showAlive, &Session::aliveLabel, kAliveTag},
	{"Show Max CPS", "max_cps", &Session::showMaxCps, &Session::maxCpsLabel, kMaxCpsTag},
	{"Show Gravity", "gravity", &Session::showGravity, &Session::gravityLabel, kGravityTag},
	{"Utility Labels", "utilities", &Session::showUtilities, &Session::utilityLabel, kUtilityTag},
	{"Safe Mode Cheats", "safe_mode", &Session::showSafeMode, &Session::safeModeLabel, kSafeModeTag},
	{"Death Next Frame", "death_next", &Session::showDeathNext, &Session::deathLabel, kDeathTag},
	{"Show Percent", "percent", &Session::showPercent, &Session::percentLabel, kPercentTag},
};

std::vector<std::string> buildFontNames() {
	std::vector<std::string> names{"Pusab", "Gold Pusab", "Chat"};
	for (int i = 1; i <= 59; ++i) names.push_back(fmt::format("Font {:02}", i));
	return names;
}
}

std::span<LabelInfo const> labelInfos() {
	return kLabels;
}

LabelInfo const* labelInfo(std::string const& key) {
	auto found = std::ranges::find(kLabels, key, [](LabelInfo const& info) { return std::string_view(info.key); });
	return found == std::end(kLabels) ? nullptr : &*found;
}

std::vector<LabelInfo const*> orderedLabels(Session const& value) {
	std::vector<LabelInfo const*> out;
	for (auto const& key : value.labelOrder) {
		auto* info = labelInfo(key);
		if (info && std::ranges::find(out, info) == out.end()) out.push_back(info);
	}
	for (auto const& info : kLabels) {
		if (std::ranges::find(out, &info) == out.end()) out.push_back(&info);
	}
	return out;
}

void moveLabel(Session& value, LabelInfo const& label, int delta) {
	auto order = orderedLabels(value);
	auto at = std::ranges::find(order, &label) - order.begin();
	auto to = at + delta;
	if (to < 0 || to >= static_cast<std::ptrdiff_t>(order.size())) return;
	std::swap(order[static_cast<size_t>(at)], order[static_cast<size_t>(to)]);
	value.labelOrder.clear();
	for (auto* info : order) value.labelOrder.emplace_back(info->key);
}

std::vector<std::string> const& labelFontNames() {
	static auto const names = buildFontNames();
	return names;
}

std::string labelFontFile(int index) {
	if (index <= 0) return "bigFont.fnt";
	if (index == 1) return "goldFont.fnt";
	if (index == 2) return "chatFont.fnt";
	return fmt::format("gjFont{:02}.fnt", std::clamp(index - 2, 1, 59));
}

int labelFontIndex(std::string const& file) {
	for (int i = 0; i < static_cast<int>(labelFontNames().size()); ++i) {
		if (labelFontFile(i) == file) return i;
	}
	return 0;
}

void Labels::syncLabel(PlayLayer* play, int tag, bool visible, LabelPlacement const& place, std::string const& text, ccColor3B color) {
	auto* label = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(tag));
	if (!visible) {
		if (label) label->removeFromParent();
		return;
	}
	auto const& value = Logic::get();
	if (!label) {
		label = CCLabelBMFont::create(text.c_str(), font.c_str());
		if (!label) return;
		label->setAnchorPoint({0.f, 0.5f});
		alignments[tag] = kCCTextAlignmentLeft;
		play->addChild(label, 999, tag);
	}
	// Cocos rebuilds the font's glyph sprites even when the text is unchanged.
	// These labels often keep the same time/CPS/percent across many display frames.
	if (text != label->getString()) label->setString(text.c_str());
	if (!value.labelsAuto) {
		if (label->getAnchorPoint() != CCPoint{0.f, 0.5f}) label->setAnchorPoint({0.f, 0.5f});
		auto& alignment = alignments[tag];
		if (alignment != kCCTextAlignmentLeft) {
			alignment = kCCTextAlignmentLeft;
			label->setAlignment(alignment);
		}
		auto position = label->getPosition();
		if (position.x != place.x || position.y != place.y) label->setPosition({place.x, place.y});
	}
	float scale = place.scale * value.labelScale;
	if (label->getScaleX() != scale || label->getScaleY() != scale) label->setScale(scale);
	// Opacity is a percentage, as the menus show it.
	auto opacity = static_cast<GLubyte>(std::lround(std::clamp(place.opacity, 0, 100) * 2.55f));
	if (label->getOpacity() != opacity) label->setOpacity(opacity);
	auto current = label->getColor();
	if (current.r != color.r || current.g != color.g || current.b != color.b) label->setColor(color);
}
void Labels::layout(PlayLayer* play) {
	auto const& value = Logic::get();
	if (!value.labelsAuto || !play) return;
	auto win = CCDirector::get()->getWinSize();
	bool right = value.labelCorner == 1 || value.labelCorner == 3;
	bool bottom = value.labelCorner >= 2;
	constexpr float kMargin = 3.f;
	constexpr float kGap = 2.f;
	auto order = orderedLabels(value);
	// From the bottom the stack grows up, so it runs through the order backwards.
	if (bottom) std::ranges::reverse(order);
	float used = 0.f;
	for (auto* info : order) {
		auto* label = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(info->tag));
		if (!label || !label->isVisible()) continue;
		CCPoint anchor{right ? 1.f : 0.f, bottom ? 0.f : 1.f};
		if (label->getAnchorPoint() != anchor) label->setAnchorPoint(anchor);
		auto& alignment = alignments[info->tag];
		auto wanted = right ? kCCTextAlignmentRight : kCCTextAlignmentLeft;
		if (alignment != wanted) {
			alignment = wanted;
			label->setAlignment(wanted);
		}
		float height = label->getScaledContentSize().height;
		CCPoint position{right ? win.width - kMargin : kMargin, bottom ? kMargin + used : win.height - kMargin - used};
		if (label->getPosition() != position) label->setPosition(position);
		used += height + kGap;
	}
}

std::string Labels::formatClock(double seconds) {
	int total = std::max(0, static_cast<int>(seconds));
	int hours = total / 3600;
	int minutes = (total % 3600) / 60;
	int secs = total % 60;
	return fmt::format("{:02}:{:02}:{:02}", hours, minutes, secs);
}
void Logic::drawOverlay(PlayLayer* play) {
	auto const& value = Logic::get();
	auto& labels = Labels::get();
	// A font change rebuilds every label in the new font.
	auto font = labelFontFile(labelFontIndex(value.labelFont));
	if (labels.font != font) {
		labels.font = font;
		for (auto const& info : labelInfos()) {
			if (auto* old = play->getChildByTag(info.tag)) old->removeFromParent();
		}
	}
	bool showLabels = !(value.renderHideLabels && Logic::get().renderCapturing());
	Labels::get().syncLabel(play, kFrameTag, showLabels && value.showFrame, value.frameLabel, fmt::format("Frame: {}", Logic::get().getFrame(play) + 1), {255, 255, 255});

	unsigned p1 = Logic::get().countPressesInLastSecond(false);
	bool twoDual = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode && play->m_gameState.m_isDualMode;
	unsigned p2 = twoDual ? Logic::get().countPressesInLastSecond(true) : 0;
	ccColor3B cpsColor{255, 255, 255};
	if (value.currentlyPressing) cpsColor = {0, 255, 77};
	else if (value.cpsBreaking) cpsColor = {255, 0, 0};
	else if (value.overMaxCps) cpsColor = {255, 72, 0};
	if (twoDual)
		Labels::get().syncLabel(play, kCpsTag, showLabels && value.showCps, value.cpsLabel, fmt::format("CPS: {}/{}", p1, p2), cpsColor);
	else
		Labels::get().syncLabel(play, kCpsTag, showLabels && value.showCps, value.cpsLabel, fmt::format("CPS: {}", p1), cpsColor);

	// Max CPS: the macro's highest (checked when it loads or changes), or the highest the CPS counter
	// has shown in this level when that is higher. Only the macro's counted before, so it read 0
	// while recording or playing without one.
	static PlayLayer* peakLevel = nullptr;
	static unsigned peakP1 = 0;
	static unsigned peakP2 = 0;
	if (peakLevel != play) {
		peakLevel = play;
		peakP1 = peakP2 = 0;
	}
	peakP1 = std::max(peakP1, p1);
	peakP2 = std::max(peakP2, p2);
	std::string maxCpsText = value.highestCps;
	{
		auto colon = maxCpsText.find(": ");
		bool counted = colon != std::string::npos;
		int macroBest = counted ? std::atoi(maxCpsText.c_str() + colon + 2) : 0;
		unsigned live = std::max(peakP1, peakP2);
		if ((counted || maxCpsText.empty()) && static_cast<int>(live) > macroBest) maxCpsText = fmt::format("{}: {}", peakP2 > peakP1 ? "P2" : "P1", live);
	}

	std::string percent = "0%";
	if (value.showPercent && play->m_player1 && play->m_endPortal) {
		float endX = play->m_endPortal->getPositionX();
		float amount = endX == 0.f ? 0.f : std::min(100.f, play->m_player1->getPositionX() / endX * 100.f);
		if (value.percentAccuracy <= 0) percent = fmt::format("{}%", static_cast<int>(amount));
		else percent = fmt::format("{:.{}f}%", amount, value.percentAccuracy);
	}
	Labels::get().syncLabel(play, kPercentTag, showLabels && value.showPercent, value.percentLabel, percent, {255, 255, 255});
	Labels::get().syncLabel(
		play,
		kTimeTag,
		showLabels && value.showTime,
		value.timeLabel,
		Labels::get().formatClock(play->m_gameState.m_levelTime),
		{255, 255, 255}
	);

	// A backstep replays in playback state; it is still the recording on screen.
	bool recordingShown = showLabels && ((value).isRecording() || (value.backstepTarget >= 0 && value.backstepState != RunState::Playing && value.backstepState != RunState::Idle)) && value.showRecording;
	auto [holds, releases] = recordingShown ? Logic::get().countHolds(value) : std::pair<int, int>{0, 0};
	Labels::get().syncLabel(
		play,
		kRecordTag,
		recordingShown,
		value.recordingLabel,
		fmt::format("Recording: {}/{}", holds, releases),
		{255, 255, 255}
	);

	if (auto* debug = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(kDebugTag))) debug->removeFromParent();

	bool dual = play->m_gameState.m_isDualMode && play->m_player2;
	auto bodyText = [&](auto&& textFor) {
		std::string text = textFor(play->m_player1, false);
		if (dual) text += "   " + textFor(play->m_player2, true);
		return text;
	};
	auto named = [&](bool second, std::string text) {
		if (!dual) return text;
		return std::string(second ? "P2 " : "P1 ") + text;
	};
	Labels::get().syncLabel(play, kPositionTag, showLabels && value.showPosition, value.positionLabel, bodyText([&](PlayerObject* body, bool second) {
		if (!body) return named(second, "--");
		auto pos = body->getPosition();
		return named(second, fmt::format("{:.2f}, {:.2f}", pos.x, pos.y));
	}), {255, 255, 255});
	Labels::get().syncLabel(play, kVelocityTag, showLabels && value.showVelocity, value.velocityLabel, bodyText([&](PlayerObject* body, bool second) {
		if (!body) return named(second, "--");
		float forward = body->m_playerSpeed * static_cast<float>(body->m_speedMultiplier) * 5.77f;
		if (body->m_isGoingLeft) forward = -forward;
		if (body->m_isPlatformer) forward = static_cast<float>(body->m_platformerXVelocity);
		float vx = body->m_isSideways ? static_cast<float>(body->m_yVelocity) : forward;
		float vy = body->m_isSideways ? forward : static_cast<float>(body->m_yVelocity);
		return named(second, fmt::format("{:.2f}, {:.2f}", vx, vy));
	}), {255, 255, 255});
	auto clipped = [&](bool second) {
		return second ? value.noclipP2 && value.noclipDeadP2 : value.noclipP1 && value.noclipDeadP1;
	};
	bool dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (dual && play->m_player2 && play->m_player2->m_isDead) || clipped(false) || (dual && clipped(true));
	Labels::get().syncLabel(play, kAliveTag, showLabels && value.showAlive, value.aliveLabel, bodyText([&](PlayerObject* body, bool second) {
		bool gone = play->m_playerDied || (body && body->m_isDead) || clipped(second);
		return named(second, gone ? "Dead" : "Alive");
	}), dead ? ccColor3B{255, 60, 60} : ccColor3B{80, 255, 120});
	Labels::get().syncLabel(play, kMaxCpsTag, showLabels && value.showMaxCps, value.maxCpsLabel, fmt::format("Max CPS: {}", maxCpsText), {255, 255, 255});
	Labels::get().syncLabel(play, kGravityTag, showLabels && value.showGravity, value.gravityLabel, bodyText([&](PlayerObject* body, bool second) {
		bool flipped = body && body->m_isUpsideDown;
		return named(second, flipped ? "Flipped" : "Normal");
	}), {255, 255, 255});
	// Utility Labels: every aid that is on, one per line, so none stays on by accident.
	std::string utilities;
	if (showLabels && value.showUtilities) {
		auto add = [&](bool on, std::string_view name) {
			if (!on) return;
			if (!utilities.empty()) utilities += '\n';
			utilities += name;
		};
		auto pair = [](bool p1, bool p2, std::string_view name) {
			if (p1 && p2) return std::string(name);
			return std::string(name) + (p1 ? " P1" : " P2");
		};
		if (value.noclipP1 || value.noclipP2) add(true, pair(value.noclipP1, value.noclipP2, "Noclip"));
		add(value.maintainGravity, "Maintain Gravity");
		add(value.sameGravity, "Same Gravity");
		add(value.pauseBeforeKill, "Prevent Death");
		add(value.pauseAtTrajectoryLength, "Pause At Trajectory Length");
		add(value.autoclicker, "Autoclicker");
		add(value.clickBoth, "Click Both");
		add(value.followMode != 0, "Follow Player");
		add(value.swapPlayers, "Swap Player Inputs");
		if (value.inverseP1 || value.inverseP2) add(true, pair(value.inverseP1, value.inverseP2, "Inverse Inputs"));
		add(value.clickDashOrbs, "Click Dash Orbs");
		add(value.clickBlackOrbs, "Click Black Orbs");
		add(value.padBlackOrb, "Auto Black Orb on Pad");
		add(value.padBlueOrb, "Auto Blue Orb on Pad");
		add(value.speedhack != 1.f, fmt::format("Speedhack {:g}x", value.speedhack));
		add(value.sampleRate() != static_cast<double>(kVanillaTps), fmt::format("{:g} TPS", value.sampleRate()));
		add(value.frameAdvance, "Frame Advance");
		add(value.backstep, "Backstep");
		add(value.unroundedPhysics(), "Velocity Unrounding");
		add(value.layoutMode, "Layout");
		if (value.hideP1 || value.hideP2) add(true, pair(value.hideP1, value.hideP2, "Hide Player"));
		add(value.noMirrorPortals, "No Mirror Portals");
		add(value.seedOverride, "Seed Override");
		add(value.safeMode, "Safe Mode");
	}
	Labels::get().syncLabel(play, kUtilityTag, !utilities.empty(), value.utilityLabel, utilities, {255, 200, 80});
	// Safe Mode Cheats: while this attempt makes no progress, the hacks that caused it.
	std::string blocked;
	if (showLabels && value.showSafeMode && value.safeMode) {
		auto names = Logic::get().attemptCheats();
		for (auto name : Logic::get().activeCheats()) {
			if (std::ranges::find(names, name) == names.end()) names.push_back(name);
		}
		for (auto name : names) {
			blocked += blocked.empty() ? "Cheats: " : ", ";
			blocked += name;
		}
	}
	Labels::get().syncLabel(play, kSafeModeTag, !blocked.empty(), value.safeModeLabel, blocked, {255, 60, 60});
	for (int tag = 0xEC10; tag <= 0xEC1F; ++tag) {
		if (auto* leftover = play->getChildByTag(tag)) leftover->removeFromParent();
	}
	Logic::get().syncDeathLabel(play);
}

void Logic::syncDeathLabel(PlayLayer* play) {
	if (!play) return;
	auto const& value = Logic::get();
	bool dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
	bool show = !dead && value.showDeathNext && (value.deathNextP1 || value.deathNextP2);
	if (value.renderHideLabels && Logic::get().renderCapturing()) show = false;
	std::string text = "Death next frame";
	if (value.deathNextP1 && value.deathNextP2) text = "Death next frame";
	else if (play->m_gameState.m_isDualMode && value.deathNextP1) text = "P1 dies next frame";
	else if (play->m_gameState.m_isDualMode && value.deathNextP2) text = "P2 dies next frame";
	Labels::get().syncLabel(play, kDeathTag, show, value.deathLabel, text, {255, 40, 40});
	Labels::get().layout(play);
}


