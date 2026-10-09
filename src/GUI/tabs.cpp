#include "GUI/gui.hpp"
#include "Hack/label_kinds.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <initializer_list>
#include <sstream>
#include <Geode/utils/web.hpp>
#include <fstream>
#include <Geode/cocos/robtop/keyboard_dispatcher/CCKeyboardDispatcher.h>
#include <Geode/loader/Event.hpp>
#include <optional>
#include <cctype>


using namespace geode::prelude;

ImGuiChildFlags GUI::fitCard() {
	return ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY;
}

float GUI::columnWidth(std::initializer_list<char const*> labels) {
	float width = 0.f;
	for (char const* label : labels) width = std::max(width, GUI::get().labelWidth(label));
	return width;
}



void GUI::drawMain() {
	auto& value = Logic::get();
	bool busy = (value).isRecording() || (value).isPlaying();
	float col = GUI::get().columnWidth({"Stop Recording", "Start Recording", "Stop Playing", "Start Playing", "Set TPS"});

	ImGui::BeginChild("##play-deck", ImVec2(0.f, 0.f), GUI::get().fitCard());
	char const* players[] = {"Both", "Player 1", "Player 2"};
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float comboW = GUI::get().comboBoxWidth(players, 3);
	float section = GUI::get().labelWidth("Record") + spacing + comboW + spacing + GUI::get().labelWidth("Play") + spacing + comboW;
	char const* recordLabel = (value).isRecording() ? "Stop Recording" : "Start Recording";
	char const* playLabel = (value).isPlaying() ? "Stop Playing" : "Start Playing";
	auto checkRow = [](char const* text) {
		return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
	};
	float toggleNeed = std::max(GUI::get().labelWidth("Start Recording"), GUI::get().labelWidth("Stop Recording"));
	toggleNeed = std::max(toggleNeed, std::max(GUI::get().labelWidth("Start Playing"), GUI::get().labelWidth("Stop Playing")));
	float checks = checkRow("Lock TPS") + spacing + checkRow("Lock FPS") + spacing + checkRow("Real Time");
	section = std::max(section, std::max(checks, toggleNeed * 2.f + spacing));
	float buttonHalf = (section - spacing) * 0.5f;
	if (ImGui::Button(recordLabel, ImVec2(buttonHalf, 0.f))) Logic::get().toggleRecording();
	GUI::get().offerRebind("toggle-record");
	ImGui::SameLine();
	if (ImGui::Button(playLabel, ImVec2(buttonHalf, 0.f))) Logic::get().togglePlaying();
	GUI::get().offerRebind("toggle-play");
	if (busy) ImGui::BeginDisabled();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Record");
	ImGui::SameLine();
	int recordSel = GUI::get().pairIndex(value.recordP1, value.recordP2);
	if (GUI::get().comboItems("##dropdown_recording_player", players, 3, recordSel)) {
		GUI::get().applyPair(recordSel, value.recordP1, value.recordP2);
		GUI::get().persist();
	}
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Play");
	ImGui::SameLine();
	int playSel = GUI::get().pairIndex(value.playP1, value.playP2);
	if (GUI::get().comboItems("##dropdown_playing_player", players, 3, playSel)) {
		GUI::get().applyPair(playSel, value.playP1, value.playP2);
		GUI::get().persist();
	}
	if (busy) ImGui::EndDisabled();
	ImGui::Separator();
	static float inputFps = -1.f;
	if (inputFps < 0.f) inputFps = value.fps;
	static float inputVisual = -1.f;
	if (inputVisual < 0.f) inputVisual = value.playbackFps >= 1.f ? value.playbackFps : 240.f;
	float setW = std::max(GUI::get().labelWidth("Set TPS"), GUI::get().labelWidth("Set FPS"));
	float sliderW = std::max(section - setW - spacing, ImGui::GetFontSize() * 5.f);
	ImGui::SetNextItemWidth(sliderW);
	ImGui::DragFloat("###fps", &inputFps, 1.f, 1.f, FLT_MAX, "%.2f TPS");
	ImGui::SameLine();
	bool canSetTps = !(value).isPlaying() || (value).isRecording();
	if (!canSetTps) ImGui::BeginDisabled();
	if (ImGui::Button("Set TPS", ImVec2(setW, 0.f))) {
		inputFps = std::clamp(inputFps, 1.f, FLT_MAX);
		Logic::get().setTps(inputFps);
		inputFps = value.fps;
	}
	if (!canSetTps) ImGui::EndDisabled();
	
	ImGui::SetNextItemWidth(sliderW);
	ImGui::DragFloat("###playback_fps", &inputVisual, 1.f, 1.f, FLT_MAX, "%.2f FPS");
	ImGui::SameLine();
	if (ImGui::Button("Set FPS", ImVec2(setW, 0.f))) {
		inputVisual = std::clamp(inputVisual, 1.f, FLT_MAX);
		GUI::get().setPlaybackFps(inputVisual);
	}
	if (GUI::get().checkboxSetting("Lock TPS", value.lockDelta)) GUI::get().dropCarry();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forces your TPS value to this. Turn this on for maximal accuracy, but it shouldn't be necessary.");
	ImGui::SameLine();
	if (GUI::get().checkboxSetting("Lock FPS", value.lockFps)) {
		GUI::get().dropCarry();
		Logic::get().syncPlaybackFps();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forces your FPS value to this. Essentially smooth fix, instead of lagging it will slow the game down.");
	ImGui::SameLine();
	if (value.lockFps) ImGui::BeginDisabled();
	if (GUI::get().checkboxSetting("Real Time", value.realTimeMode)) GUI::get().dropCarry();
	if (value.lockFps) ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Essentially the hard opposite of Lock FPS.");
	GUI::get().checkboxSetting("Sync Audio", value.syncAudio);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("If the game slows down, music and sound effects jump to the position the level time says they should be at.");
	ImGui::SameLine();
	GUI::get().checkboxSetting("Safe Mode", value.safeMode);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("An attempt that uses any hack (noclip, speedhack, TPS, macros, trajectory, hitboxes...) makes no level progress.");
	ImGui::Separator();
	ImGui::SetNextItemWidth(section);
	float speed = value.speedhack;
	if (ImGui::DragFloat("###speed", &speed, 0.005f, 0.01f, 100.f, "Speed  %.3f")) GUI::get().setSpeed(speed, false);
	GUI::get().offerRebind("key-speedhack");
	if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().setSpeed(value.speedhack, true);
	ImGui::EndChild();
	float deckH = ImGui::GetItemRectSize().y;

	ImGui::SameLine();
	ImGuiChildFlags replayFlags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX;
	if (deckH < 8.f) replayFlags |= ImGuiChildFlags_AutoResizeY;
	ImGui::BeginChild("##play-replay", ImVec2(0.f, deckH > 8.f ? deckH : 0.f), replayFlags);
	std::string replay_txt = "Replay";
	std::string status_msg = replay_txt + Logic::get().message;
	// GUI::get().sectionLabel(status_msg.c_str()); // nah.


	ImGui::Text("TPS  %.2f", value.fps);
	ImGui::SameLine(0.f, 16.f);
	ImGui::Text("Inputs  %i", static_cast<int>(value.inputs.size()));

	float pair = std::max(GUI::get().labelWidth("Save"), GUI::get().labelWidth("Load"));
	pair = std::max(pair, std::max(GUI::get().labelWidth("Reset Level"), GUI::get().labelWidth("Close Menu")));
	float name = std::max(col, ImGui::GetFontSize() * 12.f);
	name = std::max(name, std::max(checkRow("No Overwrite"), checkRow("Ignore Actions")));
	name = std::max(name, pair * 2.f + spacing);
	float half = (name - spacing) * 0.5f;
	ImGui::SetNextItemWidth(name);
	ImGui::InputTextWithHint("###replay_name", "Replay name", &value.macroName);
	if (ImGui::IsItemDeactivatedAfterEdit()) value.macroPath.clear();
	if (ImGui::Button("Save", ImVec2(half, 0.f))) Logic::get().saveReplay();
	GUI::get().offerRebind("key-save");
	ImGui::SameLine();
	if (ImGui::Button("Load", ImVec2(half, 0.f))) Logic::get().pickReplay();
	GUI::get().offerRebind("key-load");
	GUI::get().checkboxSetting("No Overwrite", value.noOverwrite);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Saves a new file using the overwrite name format instead of replacing the current replay.");
	GUI::get().offerRebind("key-no-overwrite");
	GUI::get().checkboxSetting("Ignore Actions", value.ignoreInputs);
	GUI::get().offerRebind("key-ignore");
	ImGui::Separator();
	if (ImGui::Button("Reset Level", ImVec2(half, 0.f))) {
		if (auto* play = PlayLayer::get()) play->resetLevel();
	}
	GUI::get().offerRebind("key-reset");
	ImGui::SameLine();
	if (ImGui::Button("Close Menu", ImVec2(half, 0.f))) GUI::get().beginClose();
	GUI::get().offerRebind("open-menu");

	ImGui::EndChild();
}







namespace {
// Widest row of the Macro Tools and Visuals pages, so both pages line their Settings buttons up.
float toolColumn() {
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	auto checkW = [](char const* text) {
		return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
	};
	auto fieldW = [](char const* text, float widget) {
		return widget + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
	};
	float settingsNeed = GUI::get().labelWidth("Settings");
	float advanceNeed = GUI::get().labelWidth("Advance");
	float tools = checkW("Auto Black Orb on Pad");
	tools = std::max(tools, fieldW("Noclip", GUI::get().px(150.f)));
	tools = std::max(tools, fieldW("Inverse Inputs", GUI::get().px(200.f)));
	tools = std::max(tools, fieldW("Follow Player", GUI::get().px(150.f)));
	tools = std::max(tools, fieldW("Hide Player", GUI::get().px(150.f)));
	tools = std::max(tools, checkW("Click Both Players") + spacing + checkW("Swap Player Input"));
	tools = std::max(tools, checkW("Maintain Gravity"));
	tools = std::max(tools, checkW("Disable Shake Effects"));
	tools = std::max(tools, checkW("Disable Death Effect"));
	tools = std::max(tools, checkW("No Mirror Portals"));
	tools = std::max(tools, checkW("Fix Rotation Bug"));
	tools = std::max(tools, checkW("Autoclicker") + spacing + settingsNeed);
	tools = std::max(tools, checkW("Frame Advance") + spacing + advanceNeed + spacing + settingsNeed);
	tools = std::max(tools, checkW("Layout") + spacing + settingsNeed);
	tools = std::max(tools, checkW("Hitboxes") + spacing + settingsNeed);
	tools = std::max(tools, checkW("Trajectory") + spacing + settingsNeed);
	return tools;
}
}

void GUI::drawTools() {
	auto& value = Logic::get();
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float settingsNeed = GUI::get().labelWidth("Settings");
	float advanceNeed = GUI::get().labelWidth("Advance");
	float tools = toolColumn();
	auto settingsWidth = [&]() {
		float used = ImGui::GetItemRectSize().x + spacing;
		return std::max(settingsNeed, tools - used);
	};
	char const* noclip[] = {"Off", "Player 1", "Player 2", "Both"};
	int noclipSel = GUI::get().quadIndex(value.noclipP1, value.noclipP2);
	ImGui::SetNextItemWidth(GUI::get().px(150.f));
	if (GUI::get().comboItems("Noclip##dropdown_noclip", noclip, 4, noclipSel)) {
		GUI::get().applyQuad(noclipSel, value.noclipP1, value.noclipP2);
		GUI::get().persist();
	}
	GUI::get().checkboxSetting("Click Both Players", value.clickBoth);
	GUI::get().offerRebind("key-click-both");
	ImGui::SameLine();
	GUI::get().checkboxSetting("Swap Player Input", value.swapPlayers);
	{
		char const* follow[] = {"Off", "P2 follows P1", "P1 follows P2"};
		ImGui::SetNextItemWidth(GUI::get().px(150.f));
		if (GUI::get().comboItems("Follow Player##dropdown_follow", follow, 3, value.followMode)) GUI::get().persist();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Two-player levels: your clicks (on either player's button) drive one player, and the other copies them exactly, the delay later. Off during macro playback.");
		if (value.followMode != 0) {
			ImGui::SetNextItemWidth(GUI::get().px(80.f));
			if (ImGui::DragFloat("##follow_delay", &value.followDelay, value.followDelayMs ? 1.f : 0.25f, 0.f, 100000.f, value.followDelayMs ? "%.1f" : "%.0f")) value.followDelay = std::max(value.followDelay, 0.f);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far behind the follower is. %d steps at the current TPS.", Logic::get().followSteps());
			if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().persist();
			ImGui::SameLine();
			char const* units[] = {"frames", "ms"};
			int unit = value.followDelayMs ? 1 : 0;
			if (GUI::get().comboItems("Delay##dropdown_follow_unit", units, 2, unit, GUI::get().px(90.f))) {
				value.followDelayMs = unit == 1;
				GUI::get().persist();
			}
		}
	}
	{
		bool locked = value.isRecording() || value.isPlaying();
		bool unrounded = value.shownUnrounded();
		ImGui::BeginDisabled(locked);
		// One toggle: with a macro loaded it is that macro's setting (saved in it); new recordings take it too.
		if (ImGui::Checkbox("Velocity Unrounding", &unrounded)) {
			value.setUnrounded(unrounded);
			GUI::get().persist();
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Stops GD rounding the player's y velocity to three decimals (chizz's Velocity Unrounding). Changes physics. A loaded macro keeps this in its file; save after changing it.");
		ImGui::EndDisabled();
	}

	
	GUI::get().checkboxSetting("Maintain Gravity", value.maintainGravity);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording, the wave is controlled as if gravity were always normal.");
	if (value.maintainGravity && GUI::get().twoPlayerDual()) {
		ImGui::SameLine();
		GUI::get().checkboxSetting("Same Gravity", value.sameGravity);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Maintain Gravity keeps both players on the same gravity. When one flips, the other is flipped to match.");
	}

	char const* inverse[] = {"Off", "Player 1", "Player 2", "Both"};
	int inverseSel = GUI::get().quadIndex(value.inverseP1, value.inverseP2);
	ImGui::SetNextItemWidth(GUI::get().px(200.f));
	if (GUI::get().comboItems("Inverse Inputs##dropdown_inverse", inverse, 4, inverseSel)) {
		GUI::get().applyQuad(inverseSel, value.inverseP1, value.inverseP2);
		GUI::get().persist();
	}
	

	GUI::get().checkboxSetting("Autoclicker###autoclick_checkbox", value.autoclicker);
	GUI::get().offerRebind("key-autoclick");
	ImGui::SameLine();
	if (ImGui::Button("Settings###autoclick_settings", ImVec2(settingsWidth(), 0.f))) ImGui::OpenPopup("Autoclicker Settings");
	if (ImGui::BeginPopupModal("Autoclicker Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		GUI::get().checkboxSetting("Enable Autoclicker", value.autoclicker);
		char const* autoPlayers[] = {"Player 1", "Player 2", "Both"};
		int autoSel = value.autoclickP1 && value.autoclickP2 ? 2 : value.autoclickP2 ? 1 : 0;
		if (GUI::get().comboItems("Player##dropdown_autoclicker", autoPlayers, 3, autoSel)) {
			value.autoclickP1 = autoSel != 1;
			value.autoclickP2 = autoSel != 0;
			GUI::get().persist();
		}
		int press = value.autoClick.pressInterval;
		int release = value.autoClick.releaseInterval;
		ImGui::SetNextItemWidth(GUI::get().px(90.f));
		if (ImGui::DragInt("Click Length", &press, 1.f, 0, 10000)) value.autoClick.pressInterval = std::max(press, 0);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 makes every click a swift click.");
		if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().persist();
		ImGui::SetNextItemWidth(GUI::get().px(90.f));
		if (ImGui::DragInt("Release Length", &release, 1.f, 1, 10000)) value.autoClick.releaseInterval = std::max(release, 1);
		if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().persist();
		GUI::get().checkboxSetting("Auto Disable", value.autoclickAutoDisable);
		int disableIn = value.autoClick.disableIn;
		if (!value.autoclickAutoDisable) ImGui::BeginDisabled();
		ImGui::SetNextItemWidth(GUI::get().px(90.f));
		if (ImGui::DragInt("Frames", &disableIn, 1.f, 1, 10000)) value.autoClick.disableIn = std::max(disableIn, 1);
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		if (!value.autoclickAutoDisable) ImGui::EndDisabled();
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	bool advancing = value.frameAdvance;
	if (ImGui::Checkbox("Frame Advance###advance_checkbox", &advancing)) GUI::get().setFrameAdvance(advancing);
	GUI::get().offerRebind("toggle-advance");
	ImGui::SameLine();
	float advanceRoom = std::max(advanceNeed + spacing + settingsNeed, tools - ImGui::GetItemRectSize().x - spacing);
	float advanceW = std::max(advanceNeed, (advanceRoom - spacing) * 0.5f);
	float advanceSettingsW = std::max(settingsNeed, advanceRoom - spacing - advanceW);
	if (ImGui::Button("Advance", ImVec2(advanceW, 0.f))) Logic::get().requestStep();
	GUI::get().offerRebind("step-frame");
	ImGui::SameLine();
	if (ImGui::Button("Settings###advance_settings", ImVec2(advanceSettingsW, 0.f))) ImGui::OpenPopup("Frame Advance Settings");
	if (ImGui::BeginPopupModal("Frame Advance Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		bool advancingHere = value.frameAdvance;
		if (ImGui::Checkbox("Frame Advance", &advancingHere)) GUI::get().setFrameAdvance(advancingHere);
		if (ImGui::Button("Advance")) Logic::get().requestStep();
		int hold = static_cast<int>(value.advanceHoldMs);
		int speed = static_cast<int>(std::lround(value.advanceSpeed));
		ImGui::SetNextItemWidth(GUI::get().px(90.f));
		if (ImGui::DragInt("Hold Time (ms)", &hold, 1.f, 0, 5000)) value.advanceHoldMs = static_cast<unsigned>(std::max(hold, 0));
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		ImGui::SetNextItemWidth(GUI::get().px(90.f));
		if (ImGui::DragInt("Speed", &speed, 1.f, 1, 200, "%d%%")) value.advanceSpeed = static_cast<float>(std::clamp(speed, 1, 200));
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Percent of your TPS, scaled by speedhack. 100%% at 1x advances in real time.");
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		ImGui::Separator();
		if (GUI::get().checkboxSetting("Backstep", value.backstep) && !value.backstep) Logic::get().clearBacksteps();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording, keeps saved states to step backwards to. Saves every step at 240 TPS; at higher TPS it saves less often and replays your inputs from the last save.");
		ImGui::SameLine();
		if (ImGui::Button("Back")) Logic::get().stepBack(PlayLayer::get(), 1);
		GUI::get().offerRebind("step-back");
		ImGui::SetNextItemWidth(GUI::get().px(90.f));
		if (ImGui::DragInt("Saved States", &value.backstepSaves, 1.f, 1, 100000)) value.backstepSaves = std::clamp(value.backstepSaves, 1, 100000);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many states to keep. Each covers 1/240 s of level time, so 240 is one second back.");
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		GUI::get().drawKeybindPrompt(true);
		ImGui::EndPopup();
	}

	if (GUI::get().checkboxSetting("Prevent Death", value.pauseBeforeKill)) Logic::get().resetTrajectory();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording, frame advance stops on the frame before a death instead of dying. Works without Show Trajectory.");
	GUI::get().offerRebind("key-pause-before-kill");
	GUI::get().sectionLabel("Orb Assists (while recording)");
	if (GUI::get().checkboxSetting("Click Dash Orbs", value.clickDashOrbs)) Logic::get().resetTrajectory();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swift-clicks each stacked dash orb and stores those clicks. Does not affect playback.");
	GUI::get().offerRebind("key-click-dash-orbs");
	if (GUI::get().checkboxSetting("Click Black Orbs", value.clickBlackOrbs)) Logic::get().resetTrajectory();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Swift-clicks black orbs during UFO sections. Does not affect playback.");
	GUI::get().offerRebind("key-click-black-orbs");
	if (GUI::get().checkboxSetting("Auto Black Orb on Pad", value.padBlackOrb)) Logic::get().resetTrajectory();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the player hits a pad, swift-clicks the black orbs it is touching. Stored in the recording.");
	GUI::get().offerRebind("key-pad-black-orb");
	if (GUI::get().checkboxSetting("Auto Blue Orb on Pad", value.padBlueOrb)) Logic::get().resetTrajectory();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the player hits a pad, swift-clicks a blue orb it is touching. Stored in the recording.");
	GUI::get().offerRebind("key-pad-blue-orb");
}

void GUI::drawVisuals() {
	auto& value = Logic::get();
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	float settingsNeed = GUI::get().labelWidth("Settings");
	float tools = toolColumn();
	auto settingsWidth = [&]() {
		float used = ImGui::GetItemRectSize().x + spacing;
		return std::max(settingsNeed, tools - used);
	};
	char const* hide[] = {"Off", "Player 1", "Player 2", "Both"};
	int hideSel = GUI::get().quadIndex(value.hideP1, value.hideP2);
	ImGui::SetNextItemWidth(GUI::get().px(150.f));
	if (GUI::get().comboItems("Hide Player##dropdown_hideplayer", hide, 4, hideSel)) {
		GUI::get().applyQuad(hideSel, value.hideP1, value.hideP2);
		if (value.trajectoryFollowHidePlayer) Logic::get().resetTrajectory();
		GUI::get().persist();
	}
	GUI::get().checkboxSetting("Disable Shake Effects", value.disableShakes);
	GUI::get().checkboxSetting("Disable Death Effect", value.disableDeathEffect);
	if (GUI::get().checkboxSetting("No Mirror Portals", value.noMirrorPortals)) GUI::get().applyNoMirror();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops mirror portals from flipping the level.");
	GUI::get().checkboxSetting("Fix Rotation Bug", value.fixTpsRotation);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixes gamemode rotation on high TPS without affecting hitboxes.");
	
	GUI::get().offerRebind("key-shakes");
	GUI::get().checkboxSetting("Layout###layout_checkbox", value.layoutMode);
	GUI::get().offerRebind("key-layout");
	ImGui::SameLine();
	if (ImGui::Button("Settings###layout_settings", ImVec2(settingsWidth(), 0.f))) ImGui::OpenPopup("Layout Settings###layoutsettings2");
	if (ImGui::BeginPopupModal("Layout Settings###layoutsettings2", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		GUI::get().checkboxSetting("Layout Mode", value.layoutMode);
		GUI::get().checkboxSetting("Use Regular Background", value.layoutUseRegularBg);
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uses the level's actual background");
		GUI::get().checkboxSetting("Default Art", value.layoutDefaultArt);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blocks, spikes and saws show as the default ones, sized to their hitboxes. Applies when the level loads.");
		if (ImGui::ColorEdit3("Background Color", value.backgroundColor, ImGuiColorEditFlags_NoInputs)) Logic::get().saveSettingsFile();
		if (ImGui::ColorEdit3("Ground Color", value.blocksColor, ImGuiColorEditFlags_NoInputs)) Logic::get().saveSettingsFile();
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	GUI::get().checkboxSetting("Hitboxes###hitbox_checkbox", value.showHitboxes);
	GUI::get().offerRebind("key-hitboxes");
	ImGui::SameLine();
	if (ImGui::Button("Settings###notboxsettings", ImVec2(settingsWidth(), 0.f))) {
		ImGui::OpenPopup("Hitbox Settings###boxsettings");
	}
	if (ImGui::BeginPopupModal("Hitbox Settings###boxsettings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		auto hitboxColor = [](char const* label, bool& enabled, float* color) {
			GUI::get().checkboxSetting(label, enabled);
			ImGui::SameLine();
			std::string id = std::string("##") + label;
			if (ImGui::ColorEdit4(id.c_str(), color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().saveSettingsFile();
		};
		GUI::get().checkboxSetting("Show Hitboxes", value.showHitboxes);
		GUI::get().checkboxSetting("Fill Hitboxes", value.fillHitboxes);
		GUI::get().checkboxSetting("Outline Only", value.hitboxOutlineOnly);
		GUI::get().checkboxSetting("Hide Activated Objects", value.hideActivatedObjects);
		GUI::get().checkboxSetting("Follow Hide Player", value.hitboxesFollowHidePlayer);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Will hide the hitboxes of the hidden player.");
		{
			char const* hideHit[] = {"Off", "Player 1", "Player 2", "Both"};
			int hideHitSel = GUI::get().quadIndex(value.hideHitboxP1, value.hideHitboxP2);
			if (value.hitboxesFollowHidePlayer) ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(GUI::get().px(150.f));
			if (GUI::get().comboItems("Hide Hitboxes##dropdown_hidehitbox", hideHit, 4, hideHitSel)) {
				GUI::get().applyQuad(hideHitSel, value.hideHitboxP1, value.hideHitboxP2);
				Logic::get().saveSettingsFile();
			}
			if (value.hitboxesFollowHidePlayer) ImGui::EndDisabled();
		}
		ImGui::SetNextItemWidth(GUI::get().px(180.f));
		GUI::get().dragFloatSetting("Thickness", value.hitboxThickness, 0.01f, -1000000.f, 1000000.f, "%.3f");
		ImGui::SetNextItemWidth(GUI::get().px(180.f));
		GUI::get().dragFloatSetting("Scale", value.hitboxScale, 0.01f, -1000000.f, 1000000.f, "%.3f");
		ImGui::SetNextItemWidth(GUI::get().px(180.f));
		if (ImGui::DragFloat("Fill Opacity", &value.hitboxFillOpacity, 0.01f, 0.f, 1.f, "%.2f")) {}
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		int trail = value.trailLength;
		ImGui::SetNextItemWidth(GUI::get().px(180.f));
		char const* trailFormat = trail <= 0 ? "TPS" : "%d";
		if (ImGui::DragInt("Trail Length", &trail, 1.f, 0, static_cast<int>(value.fps) * 10, trailFormat)) value.trailLength = std::max(trail, 0);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many frames the trail and the click and release boxes stay on screen");
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		GUI::get().checkboxSetting("Show Trail", value.hitboxTrail);
		ImGui::SetNextItemWidth(GUI::get().px(180.f));
		if (ImGui::DragFloat("Trail Opacity", &value.hitboxTrailOpacity, 0.01f, 0.f, 1.f, "%.2f")) value.hitboxTrailOpacity = std::clamp(value.hitboxTrailOpacity, 0.f, 1.f);
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		GUI::get().checkboxSetting("Trail at visual FPS", value.hitboxTrailVisual);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Displays one trail hitbox per displayed frame");
		ImGui::Separator();
		hitboxColor("Player box", value.hitboxPlayer, value.hitboxOuterColor);
		hitboxColor("Click", value.hitboxClick, value.hitboxClickColor);
		ImGui::SameLine();
		if (ImGui::ColorEdit4("Release", value.hitboxReleaseColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().saveSettingsFile();
		hitboxColor("Rotated box", value.hitboxRotated, value.hitboxRotatedColor);
		hitboxColor("Inner box", value.hitboxInner, value.hitboxInnerColor);
		hitboxColor("Player circle", value.hitboxCircle, value.hitboxCircleColor);
		hitboxColor("Solids", value.hitboxSolid, value.hitboxSolidColor);
		hitboxColor("Slopes", value.hitboxSlope, value.hitboxSlopeColor);
		hitboxColor("Hazards", value.hitboxHazard, value.hitboxHazardColor);
		hitboxColor("Orbs, pads, and portals", value.hitboxInteract, value.hitboxInteractColor);
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	GUI::get().checkboxSetting("Trajectory###trajectory_checkbox", value.showTrajectory);
	GUI::get().offerRebind("key-trajectory");
	ImGui::SameLine();
	if (ImGui::Button("Settings###trajectory_settings", ImVec2(settingsWidth(), 0.f))) ImGui::OpenPopup("Trajectory Settings");
	if (ImGui::BeginPopupModal("Trajectory Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		GUI::get().checkboxSetting("Show Trajectory", value.showTrajectory);
		if (GUI::get().checkboxSetting("Moving Object Support", value.trajectoryMovingObjects)) Logic::get().resetTrajectory();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("The trajectory moves the level's moving objects along with it. Off: objects stand still in the trajectory, which is faster.");
		if (GUI::get().checkboxSetting("Follow Hide Player", value.trajectoryFollowHidePlayer)) Logic::get().resetTrajectory();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Will hide the trajectory of the hidden player.");
		{
			char const* hideTraj[] = {"Off", "Player 1", "Player 2", "Both"};
			int hideTrajSel = GUI::get().quadIndex(value.hideTrajectoryP1, value.hideTrajectoryP2);
			if (value.trajectoryFollowHidePlayer) ImGui::BeginDisabled();
			ImGui::SetNextItemWidth(GUI::get().px(150.f));
			if (GUI::get().comboItems("Hide Trajectory##dropdown_hidetraj", hideTraj, 4, hideTrajSel)) {
				GUI::get().applyQuad(hideTrajSel, value.hideTrajectoryP1, value.hideTrajectoryP2);
				Logic::get().resetTrajectory();
				Logic::get().saveSettingsFile();
			}
			if (value.trajectoryFollowHidePlayer) ImGui::EndDisabled();
		}
		GUI::get().checkboxSetting("Hold", value.trajectoryHold);
		ImGui::SameLine();
		GUI::get().checkboxSetting("Release", value.trajectoryRelease);
		ImGui::SameLine();
		GUI::get().checkboxSetting("Swift", value.trajectorySwift);
		float seconds = value.trajectorySeconds;
		ImGui::SetNextItemWidth(GUI::get().px(160.f));
		if (ImGui::DragFloat("Length (s)", &seconds, 0.05f, 0.1f, 4.f, "%.2f")) value.trajectorySeconds = std::clamp(seconds, 0.1f, 4.f);
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		ImGui::SetNextItemWidth(GUI::get().px(160.f));
		GUI::get().dragFloatSetting("Width", value.trajectoryWidth, 0.05f, 0.1f, 4.f, "%.2f");
		if (ImGui::ColorEdit4("Hold color", value.trajectoryColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().saveSettingsFile();
		ImGui::SameLine();
		if (ImGui::ColorEdit4("Swift color", value.trajectorySwiftColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().saveSettingsFile();
		ImGui::SameLine();
		if (ImGui::ColorEdit4("Release color", value.trajectoryReleaseColor, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().saveSettingsFile();
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Player 1 uses these colors. Player 2 is each color, inverted.");
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}









std::string GUI::ordinal(int number) {
	int teen = number % 100;
	if (teen >= 11 && teen <= 13) return std::to_string(number) + "th";
	switch (number % 10) {
		case 1: return std::to_string(number) + "st";
		case 2: return std::to_string(number) + "nd";
		case 3: return std::to_string(number) + "rd";
		default: return std::to_string(number) + "th";
	}
}

std::string GUI::formatPlain(Session const& value) {
	std::ostringstream out;
	out << value.fps << "\n";
	for (size_t i = 0; i < value.inputs.size(); ++i) {
		auto const& input = value.inputs[i];
		int button = input.button == 0 ? 1 : input.button;
		out << input.number << " " << (input.holding ? 1 : 0) << " " << (input.player2 ? 1 : 0) << " " << button;
		if (i + 1 != value.inputs.size()) out << "\n";
	}
	return out.str();
}

void GUI::applyPlain(std::string const& text) {
	std::istringstream lines(text);
	std::string line;
	if (!std::getline(lines, line)) return;
	float fps = 0.f;
	std::istringstream header(line);
	if (header >> fps && fps >= 1.f && fps <= 360.f) {
		auto& value = Logic::get();
		if (!(value).isRecording() && !(value).isPlaying()) {
			value.fps = fps;
			Logic::get().pushRuntimeSettings();
		}
	}
	std::vector<Frame> next;
	while (std::getline(lines, line)) {
		if (line.empty()) continue;
		std::istringstream row(line);
		Frame input;
		int holding = 0;
		int player2 = 0;
		int button = 1;
		if (!(row >> input.number >> holding >> player2)) continue;
		row >> button;
		input.holding = holding != 0;
		input.player2 = player2 != 0;
		input.button = button == 0 ? 1 : button;
		if (input.number < 0) input.number = 0;
		next.push_back(input);
	}
	Logic::get().inputs = std::move(next);
	Logic::get().poses.clear();
	Logic::get().inputIds.clear();
	Logic::get().touchInputs();
	Logic::get().refreshLiveHold();
	Logic::get().recomputeCps(Logic::get());
	Logic::get().autosaveReplay();
}

void GUI::drawEditFields(bool enabled, float column) {
	auto& inputs = Logic::get().inputs;
	if (!enabled) ImGui::BeginDisabled();
	int count = enabled ? GUI::get().selectedCount() : 0;
	if (count > 1) ImGui::Text("%i Inputs Selected", count);
	else {
		int shown = enabled ? GUI::get().selected + 1 : 0;
		ImGui::Text("%s Input Selected", GUI::get().ordinal(std::max(shown, 0)).c_str());
	}
	float frameLabel = ImGui::CalcTextSize("Frame").x + ImGui::GetStyle().ItemInnerSpacing.x;
	ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 4.f, column - frameLabel));
	int frame = enabled ? GUI::get().draft.number : 0;
	bool down = enabled ? GUI::get().draft.holding : false;
	bool player2 = enabled ? GUI::get().draft.player2 : false;
	int beforeFrame = frame;
	// An active InputInt writes its typed text back every frame, so the field is scoped to the
	// selected inputs or that text would land on whatever gets selected next.
	ImGui::PushID(fmt::format("{}:{}:{}", GUI::get().selectedId, GUI::get().anchorId, GUI::get().selectedId != 0 ? -1 : GUI::get().selected).c_str());
	bool frameEdited = ImGui::InputInt("Frame", &frame);
	ImGui::PopID();
	if (frameEdited && enabled) {
		int delta = std::max(frame, 0) - beforeFrame;
		rewriteSelected([&](Frame& input) { input.number = std::max(input.number + delta, 0); });
	}
	if (ImGui::Checkbox("Down", &down) && enabled) rewriteSelected([&](Frame& input) { input.holding = down; });
	ImGui::SameLine();
	if (ImGui::Checkbox("Player 2", &player2) && enabled) rewriteSelected([&](Frame& input) { input.player2 = player2; });
	char const* buttonNames[] = {"Jump", "Left", "Right"};
	int button = enabled ? GUI::get().draft.button : static_cast<int>(PlayerButton::Jump);
	if (button == 0) button = static_cast<int>(PlayerButton::Jump);
	int buttonSel = button == static_cast<int>(PlayerButton::Left) ? 1 : button == static_cast<int>(PlayerButton::Right) ? 2 : 0;
	float buttonLabel = ImGui::CalcTextSize("Button").x + ImGui::GetStyle().ItemInnerSpacing.x;
	ImGui::SetNextItemWidth(std::max(GUI::get().comboBoxWidth(buttonNames, 3), column - buttonLabel));
	if (ImGui::Combo("Button", &buttonSel, buttonNames, 3) && enabled) {
		int next = buttonSel == 1 ? static_cast<int>(PlayerButton::Left) : buttonSel == 2 ? static_cast<int>(PlayerButton::Right) : static_cast<int>(PlayerButton::Jump);
		rewriteSelected([&](Frame& input) { input.button = next; });
	}
	int first = 0;
	int last = 0;
	bool span = GUI::get().selectionSpan(first, last);
	float moveW = std::max(GUI::get().labelWidth("Move Up"), GUI::get().labelWidth("Move Down"));
	if (ImGui::Button("Move Up", ImVec2(moveW, 0.f)) && enabled && span && first > 0) moveSelected(-1);
	ImGui::SameLine();
	if (ImGui::Button("Move Down", ImVec2(moveW, 0.f)) && enabled && span && last + 1 < static_cast<int>(inputs.size())) moveSelected(1);
	if (!enabled) ImGui::EndDisabled();
}



void GUI::drawMacro() {
	auto& value = Logic::get();
	GUI::get().sectionLabel("Files");
	static int convertSel = 0;
	char const* formats[] = {"JSON", "Plain Text", "Silicate", "TCBot", "GDR"};
	float fileSpacing = ImGui::GetStyle().ItemSpacing.x;
	float exportLabel = ImGui::CalcTextSize("Export As").x + ImGui::GetStyle().ItemInnerSpacing.x;
	float filePair = GUI::get().labelWidth("Convert Folder");
	filePair = std::max(filePair, GUI::get().labelWidth("Open Folder"));
	filePair = std::max(filePair, std::max(GUI::get().labelWidth("Export"), GUI::get().labelWidth("Import")));
	filePair = std::max(filePair, std::max(GUI::get().labelWidth("Labels"), GUI::get().labelWidth("Metadata")));
	filePair = std::max(filePair, std::max(GUI::get().labelWidth("Overwrite Name"), GUI::get().labelWidth("Reset Macro")));
	float files = filePair * 2.f + fileSpacing;
	float comboNeed = GUI::get().comboBoxWidth(formats, 5);
	if (exportLabel + comboNeed > files) files = exportLabel + comboNeed;
	float comboW = std::max(comboNeed, files - exportLabel);
	float fileHalf = (files - fileSpacing) * 0.5f;
	GUI::get().comboItems("Export As", formats, 5, convertSel, comboW);
	if (ImGui::Button("Export", ImVec2(fileHalf, 0.f))) Logic::get().exportKind(formats[convertSel]);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Converts the current replay to the converted folder");
	ImGui::SameLine();
	if (ImGui::Button("Import", ImVec2(fileHalf, 0.f))) Logic::get().importReplays();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import a replay of the selected type to convert to Echo");
	if (ImGui::Button("Convert Folder", ImVec2(fileHalf, 0.f))) Logic::get().convertLegacyEchoes();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rewrites all other replay types in the replays folder into .echo files. Old Echo replays are renamed to .old.echo.");
	ImGui::SameLine();
	if (ImGui::Button("Open Folder", ImVec2(fileHalf, 0.f))) geode::utils::file::openFolder(Logic::get().saveRoot() / "replays");
	if (ImGui::Button("Labels", ImVec2(fileHalf, 0.f))) ImGui::OpenPopup("Label Settings");
	ImGui::SameLine();
	if (ImGui::Button("Metadata", ImVec2(fileHalf, 0.f))) ImGui::OpenPopup("Replay Metadata");

	GUI::get().sectionLabel("Seed");
	if (value.levelSeedKnown && PlayLayer::get()) ImGui::Text("Level seed: %llu", static_cast<unsigned long long>(value.levelSeed));
	else ImGui::TextUnformatted("Level seed: not set");
	if (GUI::get().checkboxSetting("Override", value.seedOverride)) GUI::get().applySeedOverride();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overrides the game seed to this value");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(GUI::get().px(140.f));
	ImGui::InputScalar("##seed_override", ImGuiDataType_U64, &value.seedOverrideValue);
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		GUI::get().applySeedOverride();
		Logic::get().saveSettingsFile();
	}

	GUI::get().sectionLabel("Display");
	float cpsDrag = ImGui::GetFontSize() * 6.f;
	float cpsLabel = ImGui::CalcTextSize("Max CPS").x + ImGui::GetStyle().ItemInnerSpacing.x;
	float cpsW = std::max(GUI::get().labelWidth("CPS"), files - cpsDrag - cpsLabel - fileSpacing);
	ImGui::SetNextItemWidth(cpsDrag);
	ImGui::DragInt("Max CPS", &value.maxCps, 0.4f, 1, 100);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is based on ILL CPS rules.");
	ImGui::SameLine();
	if (ImGui::Button("CPS", ImVec2(cpsW, 0.f))) ImGui::OpenPopup("CPS Breaks###cps_breaks");
	if (ImGui::BeginPopupModal("CPS Breaks###cps_breaks", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("Highest CPS: %s", value.highestCps.c_str());
		ImGui::Text("Current CPS: %.2f", value.currentCps);
		ImGui::Separator();
		int p1 = static_cast<int>(value.cpsNotesP1.size());
		int total = p1 + static_cast<int>(value.cpsNotesP2.size());
		if (total == 0) {
			ImGui::TextUnformatted("No CPS breaks");
		} else {
			float lineH = ImGui::GetTextLineHeightWithSpacing();
			ImGui::BeginChild("##cps-notes", ImVec2(ImGui::GetFontSize() * 22.f, lineH * std::min(total, 12) + ImGui::GetStyle().WindowPadding.y * 2.f), ImGuiChildFlags_Borders);
			ImGuiListClipper clipper;
			clipper.Begin(total, lineH);
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					bool second = i >= p1;
					auto const& note = second ? value.cpsNotesP2[i - p1] : value.cpsNotesP1[i];
					std::string span = note.first == note.last
						? std::to_string(note.first)
						: std::to_string(note.first) + "-" + std::to_string(note.last);
					std::string label = std::string(second ? "P2  " : "P1  ") + span + "  " + note.text + "##cps-" + std::to_string(i);
					if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0.f, lineH))) {
						GUI::get().showCpsRange(note.first, note.last, second);
						ImGui::CloseCurrentPopup();
					}
					if (ImGui::IsItemHovered()) {
						char const* tip = "Over 16 inputs in a second";
						if (note.text == "Rule 2") tip = "Over 3 inputs in a frame";
						else if (note.text == "Rule 3") tip = "5+ clicks faster than 48/s";
						ImGui::SetTooltip("%s", tip);
					}
				}
			}
			ImGui::EndChild();
		}
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopupModal("Label Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		float gridH = ImGui::GetFrameHeightWithSpacing() * 16.f;
		float gridW = ImGui::GetFontSize() * 33.f;
		ImGui::BeginChild("##label-grid", ImVec2(gridW, gridH), ImGuiChildFlags_None);
		auto saveLabels = [] { Logic::get().saveSettingsFile(); };
		if (GUI::get().checkboxSetting("Auto Position", value.labelsAuto)) saveLabels();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stacks the shown labels from a corner in the order below, so turning one off closes the gap.");
		if (value.labelsAuto) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.f);
			if (ImGui::Combo("Corner", &value.labelCorner, kLabelCorners, IM_ARRAYSIZE(kLabelCorners))) saveLabels();
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.f);
		ImGui::DragFloat("Size", &value.labelScale, 0.01f, 0.05f, 10.f, "%.2fx");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplies every label's own size.");
		if (ImGui::IsItemDeactivatedAfterEdit()) saveLabels();
		ImGui::SameLine();
		int font = labelFontIndex(value.labelFont);
		auto const& fonts = labelFontNames();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.f);
		if (ImGui::BeginCombo("Font", fonts[static_cast<size_t>(font)].c_str())) {
			for (int i = 0; i < static_cast<int>(fonts.size()); ++i) {
				if (ImGui::Selectable(fonts[static_cast<size_t>(i)].c_str(), i == font)) {
					value.labelFont = labelFontFile(i);
					saveLabels();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::Spacing();
		ImGui::TextDisabled(value.labelsAuto ? "Drag a label by its grip to reorder it." : "Auto Position stacks the labels in this order.");
		auto order = orderedLabels(value);
		float em = ImGui::GetFontSize();
		float titleX = em * 3.6f;
		float fieldsX = em * 12.5f;
		float fieldW = em * 3.f;
		float gap = ImGui::GetStyle().ItemSpacing.x;
		// Column headings over the fields.
		ImGui::TextDisabled("Label");
		float column = fieldsX;
		auto heading = [&](char const* text) {
			ImGui::SameLine(column);
			ImGui::TextDisabled("%s", text);
			column += fieldW + gap;
		};
		if (!value.labelsAuto) {
			heading("X");
			heading("Y");
		}
		heading("Size");
		heading("Opacity");
		for (std::size_t i = 0; i < order.size(); ++i) {
			auto const& label = *order[i];
			auto& place = value.*label.place;
			ImGui::PushID(label.key);
			// Dragging the grip past a neighbour swaps the two; the grip keeps its ID, so the drag
			// carries on with the same label.
			ImGui::Selectable("=##grip", false, ImGuiSelectableFlags_None, ImVec2(em * .9f, 0.f));
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
			// One place per row height dragged, so the label stays under the pointer.
			if (ImGui::IsItemActive()) {
				float dy = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y;
				float rowH = ImGui::GetFrameHeightWithSpacing();
				if (std::abs(dy) >= rowH) {
					int step = dy < 0.f ? -1 : 1;
					std::ptrdiff_t next = static_cast<std::ptrdiff_t>(i) + step;
					if (next >= 0 && next < static_cast<std::ptrdiff_t>(order.size())) {
						moveLabel(value, label, step);
						ImGui::ResetMouseDragDelta();
					}
				}
			}
			if (ImGui::IsItemDeactivated()) saveLabels();
			ImGui::SameLine();
			if (ImGui::Checkbox("##shown", &(value.*label.shown))) saveLabels();
			ImGui::SameLine(titleX);
			std::string title = label.title;
			if (title.starts_with("Show ")) title = title.substr(5);
			ImGui::TextUnformatted(title.c_str());
			ImGui::SameLine(fieldsX);
			ImGui::PushItemWidth(fieldW);
			if (!value.labelsAuto) {
				ImGui::DragFloat("##x", &place.x, 1.f, 0.f, 0.f, "%.0f");
				if (ImGui::IsItemDeactivatedAfterEdit()) saveLabels();
				ImGui::SameLine();
				ImGui::DragFloat("##y", &place.y, 1.f, 0.f, 0.f, "%.0f");
				if (ImGui::IsItemDeactivatedAfterEdit()) saveLabels();
				ImGui::SameLine();
			}
			ImGui::DragFloat("##scale", &place.scale, 0.01f, 0.05f, 10.f, "%.2fx");
			if (ImGui::IsItemDeactivatedAfterEdit()) saveLabels();
			ImGui::SameLine();
			int opacity = place.opacity;
			if (ImGui::DragInt("##opacity", &opacity, 1.f, 0, 100, "%d%%")) place.opacity = std::clamp(opacity, 0, 100);
			if (ImGui::IsItemDeactivatedAfterEdit()) saveLabels();
			ImGui::PopItemWidth();
			ImGui::PopID();
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
		ImGui::InputInt("Percent Accuracy", &value.percentAccuracy);
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		ImGui::EndChild();
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::Button("Overwrite Name", ImVec2(files, 0.f))) ImGui::OpenPopup("Overwrite Name Format");
	if (ImGui::BeginPopupModal("Overwrite Name Format", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
		ImGui::InputText("Renaming Format", &value.renameFormat);
		if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::Button("Reset Macro", ImVec2(files, 0.f)) && !value.inputs.empty()) ImGui::OpenPopup("Confirm Reset");
	if (ImGui::BeginPopupModal("Confirm Reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::TextUnformatted("Reset the current macro?");
		float confirmW = GUI::get().sharedButtonWidth({"Yes", "No"});
		if (ImGui::Button("Yes", ImVec2(confirmW, 0.f))) {
			GUI::get().resetMacro();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("No", ImVec2(confirmW, 0.f))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopupModal("Replay Metadata", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		int total = static_cast<int>(value.recordingSeconds);
		int clicks = 0;
		for (auto const& input : value.inputs) {
			if (input.holding) ++clicks;
		}
		ImGui::Text("Time: %02d:%02d:%02d", total / 3600, (total % 3600) / 60, total % 60);
		ImGui::Text("Attempts: %i", value.attempts);
		ImGui::Text("Clicks: %i", clicks);
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::Checkbox("Save Debug Samples", &value.saveDebug)) {
		value.format = value.saveDebug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
		GUI::get().persist();
	}
}

void GUI::showCpsRange(int first, int last, bool player2) {
	if (last < first) std::swap(first, last);
	markFirst = first;
	markLast = last;
	markPlayer = player2 ? 1 : 0;
	showEditorP1 = !player2;
	showEditorP2 = player2;
	autoScroll = false;
	plain = false;
	page = "Editor";
	scrollToMark = true;
	Logic::get().identifyInputs();
	selectInput(-1);
	auto const& inputs = Logic::get().inputs;
	for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
		if (!inCpsMark(inputs[i])) continue;
		selectInput(i);
		break;
	}
}

bool GUI::shiftSelecting() const {
	if (ImGui::GetIO().KeyShift) return true;
	if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) return true;
	if (auto* keys = CCKeyboardDispatcher::get()) return keys->getShiftKeyPressed();
	return false;
}

void GUI::selectInput(int index, bool range) {
	auto& inputs = Logic::get().inputs;
	if (index < 0 || index >= static_cast<int>(inputs.size())) {
		selected = -1;
		selectedId = 0;
		anchor = -1;
		anchorId = 0;
		return;
	}
	if (Logic::get().inputId(static_cast<size_t>(index)) == 0) Logic::get().identifyInputs(true);
	selected = index;
	selectedId = Logic::get().inputId(static_cast<size_t>(index));
	draft = inputs[index];
	if (!range || anchor < 0 || anchor >= static_cast<int>(inputs.size())) {
		anchor = selected;
		anchorId = selectedId;
	}
}
void GUI::followSelection() {
	auto& value = Logic::get();
	auto& inputs = value.inputs;
	auto find = [&](uint64_t id, int fallback) {
		if (id != 0) {
#ifdef GEODE_IS_WINDOWS
			// IDs stay attached to their input. Appending recorded clicks does
			// not move an existing selection; edits still fall back to a search.
			if (fallback >= 0 && fallback < static_cast<int>(inputs.size()) &&
				value.inputId(static_cast<size_t>(fallback)) == id) return fallback;
#endif
			for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
				if (value.inputId(static_cast<size_t>(i)) == id) return i;
			}
			return -1;
		}
		if (fallback >= 0 && fallback < static_cast<int>(inputs.size())) return fallback;
		return -1;
	};
	selected = find(selectedId, selected);
	if (selected >= 0) {
		if (selectedId == 0) selectedId = value.inputId(static_cast<size_t>(selected));
		draft = inputs[selected];
	} else {
		selectedId = 0;
	}
	anchor = find(anchorId, anchorId != 0 ? anchor : selected);
	if (anchor >= 0) {
		if (anchorId == 0) anchorId = value.inputId(static_cast<size_t>(anchor));
	} else {
		anchor = selected;
		anchorId = selectedId;
	}
}
void strokeSelectedButton() {
	ImVec2 min = ImGui::GetItemRectMin();
	ImVec2 max = ImGui::GetItemRectMax();
	float rounding = ImGui::GetStyle().FrameRounding;
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRect(min, max, IM_COL32(0, 0, 0, 230), rounding, 0, 3.f);
	draw->AddRect(ImVec2(min.x + 1.5f, min.y + 1.5f), ImVec2(max.x - 1.5f, max.y - 1.5f), IM_COL32(255, 255, 255, 245), rounding, 0, 1.5f);
}
void GUI::drawEditor() {
	auto& value = Logic::get();
	auto& inputs = value.inputs;
	Logic::get().identifyInputs();
	GUI::get().followSelection();
	if (GUI::get().selected >= 0 && !GUI::get().editorShows(inputs[GUI::get().selected])) GUI::get().selectInput(-1);

	ImGuiStyle const& editStyle = ImGui::GetStyle();
	float listW = GUI::get().labelWidth("Release Right at 0000000") + editStyle.ScrollbarSize + editStyle.WindowPadding.x * 2.f + editStyle.ChildBorderSize * 2.f;
	float editPad = editStyle.WindowPadding.x * 2.f + editStyle.ChildBorderSize * 2.f;
	float pairW = std::max(GUI::get().labelWidth("Add Input"), GUI::get().labelWidth("Delete Inputs"));
	pairW = std::max(pairW, std::max(GUI::get().labelWidth("Move Up"), GUI::get().labelWidth("Move Down")));
	float editRow = pairW * 2.f + editStyle.ItemSpacing.x;
	float checkW = ImGui::GetFrameHeight() + editStyle.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto Scroll To Frame").x;
	float sortW = ImGui::GetFrameHeight() + editStyle.ItemInnerSpacing.x + ImGui::CalcTextSize("Sort Automatically").x + editStyle.ItemSpacing.x + ImGui::CalcTextSize("(?)").x;
	float editSingle = std::max(GUI::get().labelWidth("Set to Current Frame"), std::max(checkW, sortW));
	// Width follows the widest control. AutoResizeX on the child grows with labels at any Global Scale;
	// a fixed width + equal-share math clips the second button when padding stays absolute.
	float editInner = std::max(editSingle, editRow);
	float editW = editInner + editPad;
	static float editorPanelH = 0.f;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Show");
	ImGui::SameLine();
	float used = ImGui::GetItemRectSize().x + editStyle.ItemSpacing.x;
	char const* players[] = {"Both", "Player 1", "Player 2"};
	int showSel = GUI::get().pairIndex(GUI::get().showEditorP1, GUI::get().showEditorP2);
	std::string tpsLabel = value.rates.empty() ? "TPS Changes" : fmt::format("TPS Changes - {}", value.rates.size());
	float comboNeed = GUI::get().comboBoxWidth(players, 3);
	float buttonNeed = GUI::get().labelWidth(tpsLabel.c_str());
	float comboW = std::max(comboNeed, listW - used);
	float buttonW = std::max(buttonNeed, editW);
	if (GUI::get().comboItems("##editor_show", players, 3, showSel, comboW)) {
		GUI::get().applyPair(showSel, GUI::get().showEditorP1, GUI::get().showEditorP2);
	}
	ImGui::SameLine();
	if (ImGui::Button(tpsLabel.c_str(), ImVec2(buttonW, 0.f))) ImGui::OpenPopup("TPS Changes###editor_tps");
	if (ImGui::BeginPopupModal("TPS Changes###editor_tps", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		if (value.rates.empty()) {
			ImGui::TextUnformatted("No TPS changes");
		} else {
			float lineH = ImGui::GetTextLineHeightWithSpacing();
			ImGui::BeginChild("##editor-tps", ImVec2(ImGui::GetFontSize() * 16.f, lineH * std::min(static_cast<int>(value.rates.size()), 12) + ImGui::GetStyle().WindowPadding.y * 2.f), ImGuiChildFlags_Borders);
			for (int i = 0; i < static_cast<int>(value.rates.size()); ++i) {
				auto const& change = value.rates[static_cast<size_t>(i)];
				std::string label = fmt::format("Frame {}  {:.2f}##rate-{}", change.frame, change.fps, i);
				if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0.f, lineH))) {
					GUI::get().markFirst = change.frame;
					GUI::get().markLast = change.frame;
					GUI::get().markPlayer = -1;
					GUI::get().scrollToMark = true;
					GUI::get().autoScroll = false;
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndChild();
		}
		if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// The list shares the editor column, so its height follows that column instead of a fixed row count.
	float listH = editorPanelH > 1.f ? editorPanelH : ImGui::GetFrameHeightWithSpacing() * 10.f;
	ImGui::BeginChild("##echo-inputs", ImVec2(listW, listH), ImGuiChildFlags_Borders);
	if (inputs.empty() && !GUI::get().plain) {
		GUI::get().selectInput(-1);
		GUI::get().scrollToMark = false;
		ImGui::TextUnformatted("No Inputs");
	} else if (GUI::get().plain) {
		GUI::get().selected = -1;
		if (GUI::get().plainText.empty()) GUI::get().plainText = GUI::get().formatPlain(value);
		if (ImGui::InputTextMultiline("##echo-plain", &GUI::get().plainText, ImVec2(-1.f, -1.f))) GUI::get().applyPlain(GUI::get().plainText);
	} else {
		// With both players shown every input is a row, so no row list is built. A filtered list is
		// kept between frames and only grows while recording.
		bool everyRow = GUI::get().showEditorP1 && GUI::get().showEditorP2;
		struct ShownRows {
			std::vector<Frame> const* owner = nullptr;
			Frame const* data = nullptr;
			size_t count = 0;
			uint64_t revision = 0;
			bool p1 = true;
			bool p2 = true;
			std::vector<int> rows;
		};
		static ShownRows filtered;
		if (!everyRow) {
			bool kept = filtered.owner == &inputs && filtered.revision == value.inputsRevision && filtered.count <= inputs.size() &&
				filtered.p1 == GUI::get().showEditorP1 && filtered.p2 == GUI::get().showEditorP2 &&
				(filtered.count < inputs.size() || filtered.data == inputs.data());
			if (!kept) {
				filtered.rows.clear();
				filtered.count = 0;
			}
			for (size_t i = filtered.count; i < inputs.size(); ++i) {
				if (GUI::get().editorShows(inputs[i])) filtered.rows.push_back(static_cast<int>(i));
			}
			filtered = {&inputs, inputs.data(), inputs.size(), value.inputsRevision, GUI::get().showEditorP1, GUI::get().showEditorP2, std::move(filtered.rows)};
		}
		int rows = everyRow ? static_cast<int>(inputs.size()) : static_cast<int>(filtered.rows.size());
		auto inputAt = [&](int row) { return everyRow ? row : filtered.rows[static_cast<size_t>(row)]; };
		auto rowOf = [&](int i) {
			if (everyRow) return i;
			auto at = std::lower_bound(filtered.rows.begin(), filtered.rows.end(), i);
			return at != filtered.rows.end() && *at == i ? static_cast<int>(std::distance(filtered.rows.begin(), at)) : 0;
		};
		if (rows == 0) {
			GUI::get().selectInput(-1);
			GUI::get().scrollToMark = false;
			ImGui::TextUnformatted(GUI::get().showEditorP1 ? "No Player 1 Inputs" : "No Player 2 Inputs");
		} else {
			int frame = PlayLayer::get() ? Logic::get().getFrame(PlayLayer::get()) : (value.editorTesting ? value.stepFrame : 0);
			auto numberAt = [&](int row) { return inputs[static_cast<size_t>(inputAt(row))].number; };
			// The first row nearest the current frame. Sorted inputs find it by binary search.
			auto firstRow = [&](auto&& before) {
				int low = 0;
				int high = rows;
				while (low < high) {
					int mid = low + (high - low) / 2;
					if (before(numberAt(mid))) low = mid + 1;
					else high = mid;
				}
				return low;
			};
			int closest = inputAt(0);
			int best = std::abs(inputs[closest].number - frame);
			if (Logic::get().inputsSorted(value)) {
				int above = firstRow([&](int number) { return number < frame; });
				int pick = above < rows ? above : rows - 1;
				if (above > 0 && (above == rows || numberAt(above) != frame)) {
					int below = numberAt(above - 1);
					if (above == rows || frame - below <= numberAt(above) - frame) pick = firstRow([&](int number) { return number < below; });
				}
				closest = inputAt(pick);
				best = std::abs(inputs[closest].number - frame);
			} else {
				for (int row = 0; row < rows; ++row) {
					int i = inputAt(row);
					int diff = std::abs(inputs[i].number - frame);
					if (diff < best) {
						best = diff;
						closest = i;
					}
				}
			}
			bool follow = GUI::get().autoScroll && PlayLayer::get();
			float rowH = ImGui::GetFrameHeightWithSpacing();
			if (GUI::get().scrollToMark) {
				int firstMark = 0;
				for (int row = 0; row < rows; ++row) {
					if (GUI::get().inCpsMark(inputs[inputAt(row)])) {
						firstMark = row;
						break;
					}
				}
				ImGui::SetScrollY(std::max(0.f, firstMark * rowH - (ImGui::GetWindowHeight() - rowH) * 0.5f));
				GUI::get().scrollToMark = false;
			} else if (follow) {
				int chosen = closest;
				int picked = GUI::get().selected;
				bool pickedShown = GUI::get().selectedId != 0 && picked >= 0 && picked < static_cast<int>(inputs.size()) &&
					Logic::get().inputId(static_cast<size_t>(picked)) == GUI::get().selectedId && GUI::get().editorShows(inputs[picked]);
				if (pickedShown && std::abs(inputs[picked].number - frame) == best) chosen = picked;
				// Reselecting while an edit field is active rewrites every input the playhead passes.
				if (chosen != picked && !ImGui::IsAnyItemActive()) GUI::get().selectInput(chosen);
				int row = rowOf(chosen);
				ImGui::SetScrollY(std::max(0.f, row * rowH - (ImGui::GetWindowHeight() - rowH) * 0.5f));
			}
			ImGuiListClipper clipper;
			clipper.Begin(rows, rowH);
			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
					int i = inputAt(row);
					bool chosen = GUI::get().selected == i;
					bool marked = chosen || GUI::get().inputInRange(i) || GUI::get().inCpsMark(inputs[i]);
					float const* tint = inputs[i].player2 ? value.editorP2 : value.editorP1;
					if (marked) tint = inputs[i].player2 ? value.editorSelP2 : value.editorSelP1;
					ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tint[0], tint[1], tint[2], tint[3]));
					int button = inputs[i].button == 0 ? static_cast<int>(PlayerButton::Jump) : inputs[i].button;
					char const* action = inputs[i].holding ? "Click" : "Release";
					if (button == static_cast<int>(PlayerButton::Left)) action = inputs[i].holding ? "Left" : "Release Left";
					else if (button == static_cast<int>(PlayerButton::Right)) action = inputs[i].holding ? "Right" : "Release Right";
					std::string label = fmt::format("{} at {}##in{}", action, inputs[i].number, i);
					ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.f));
					if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
						GUI::get().selectInput(i, GUI::get().shiftSelecting());
						GUI::get().autoScroll = false;
						GUI::get().clearCpsMark();
					}
					if (GUI::get().inputInRange(i)) strokeSelectedButton();
					ImGui::PopStyleColor();
				}
			}
		}
	}
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginChild("##echo-edit", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
	bool live = GUI::get().selected >= 0 && GUI::get().selected < static_cast<int>(inputs.size()) && !GUI::get().plain;
	GUI::get().drawEditFields(live, editInner);
	ImGui::Separator();
	int frame = (PlayLayer::get() ? Logic::get().getFrame(PlayLayer::get()) : (value.editorTesting ? value.stepFrame : 0)) + 1;
	ImGui::Text("Current Frame: %i", frame);
	if (!live) ImGui::BeginDisabled();
	if (ImGui::Button("Set to Current Frame", ImVec2(editInner, 0.f))) {
		int delta = frame - GUI::get().draft.number;
		rewriteSelected([&](Frame& input) { input.number = std::max(input.number + delta, 0); });
	}
	if (!live) ImGui::EndDisabled();
	ImGui::Checkbox("Auto Scroll To Frame", &GUI::get().autoScroll);
	if (ImGui::Checkbox("Edit Plain Text", &GUI::get().plain)) {
		GUI::get().plainText = GUI::get().plain ? GUI::get().formatPlain(value) : std::string();
	}
	ImGui::Separator();
	if (ImGui::Button("Add Input", ImVec2(pairW, 0.f))) GUI::get().addInput(frame);
	ImGui::SameLine();
	int deleteFirst = 0;
	int deleteLast = 0;
	bool canDelete = GUI::get().selectionSpan(deleteFirst, deleteLast);
	if (!canDelete) ImGui::BeginDisabled();
	if (ImGui::Button(GUI::get().hasRange() ? "Delete Inputs" : "Delete Input", ImVec2(pairW, 0.f)) && canDelete) GUI::get().deleteSelected();
	if (!canDelete) ImGui::EndDisabled();

	ImGui::Separator();
	if (ImGui::Button("Actions", ImVec2(editInner, 0.f))) ImGui::OpenPopup("Editor Actions");
	if (ImGui::BeginPopupModal("Editor Actions", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		float gap = ImGui::GetStyle().ItemSpacing.x;
		float half = std::max(GUI::get().labelWidth("Random Offset"), GUI::get().labelWidth("Remove Player 1"));
		half = std::max(half, GUI::get().labelWidth("-1 Frame"));
		float row = half * 2.f + gap;
		row = std::max(row, GUI::get().labelWidth("Flip Presses and Releases"));
		row = std::max(row, GUI::get().labelWidth("Flip Player 1 and 2"));
		auto randomOffset = [] { GUI::get().randomOffsetChosen(); };
		char const* scope = GUI::get().hasRange() ? "the selected inputs" : "the whole macro";

		GUI::get().sectionLabel("Shift");
		float framesLabel = ImGui::CalcTextSize("Frames").x + ImGui::GetStyle().ItemInnerSpacing.x;
		ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.f, row - framesLabel));
		ImGui::InputInt("Frames###offset_frames", &GUI::get().inputOffset);
		if (ImGui::Button("Offset", ImVec2(half, 0.f))) offsetChosen(GUI::get().inputOffset);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves %s by that many frames", scope);
		ImGui::SameLine();
		if (ImGui::Button("Random Offset", ImVec2(half, 0.f))) randomOffset();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves each input in %s by a random amount from -frames to +frames", scope);
		if (ImGui::Button("-1 Frame", ImVec2(half, 0.f))) offsetChosen(-1);
		ImGui::SameLine();
		if (ImGui::Button("+1 Frame", ImVec2(half, 0.f))) offsetChosen(1);

		GUI::get().sectionLabel("Order");
		if (ImGui::Button("Sort", ImVec2(row, 0.f))) {
			Logic::get().sortInputs();
			GUI::get().syncDraft();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sorts inputs by frame");

		GUI::get().sectionLabel("Players");
		if (ImGui::Button("Remove Player 1", ImVec2(half, 0.f))) {
			Logic::get().removePlayerInputs(false);
			GUI::get().selectInput(-1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove Player 2", ImVec2(half, 0.f))) {
			Logic::get().removePlayerInputs(true);
			GUI::get().selectInput(-1);
		}
		if (ImGui::Button("Flip Player 1 and 2", ImVec2(row, 0.f))) {
			Logic::get().flipInputPlayers();
			GUI::get().syncDraft();
		}

		GUI::get().sectionLabel("Holds");
		if (ImGui::Button("Flip Presses and Releases", ImVec2(row, 0.f))) {
			Logic::get().flipInputHolds();
			GUI::get().syncDraft();
		}

		ImGui::Separator();
		if (ImGui::Button("Close", ImVec2(row, 0.f))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (GUI::get().checkboxSetting("Sort Automatically", value.sortAutomatically) && value.sortAutomatically) {
		Logic::get().sortInputs();
		GUI::get().syncDraft();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording and playing together, new clicks are sorted instead of appended.");
	ImGui::EndChild();
	editorPanelH = ImGui::GetItemRectSize().y;
}

void GUI::drawSequence() {
	auto& value = Logic::get();
	static int selected = -1;
	static int amount = 1;

	GUI::get().sectionLabel("Queue");
	if (GUI::get().checkboxSetting("Enable Sequence", value.sequenceEnabled)) value.replayIndex = 0;
	ImGui::SameLine();
	if (GUI::get().checkboxSetting("Random", value.sequenceRandom)) {
		value.replayIndex = 0;
		value.sequenceOrder.clear();
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Plays the queue in a random order");
	if (ImGui::Button("Remove All")) {
		value.sequence.clear();
		value.sequenceOrder.clear();
		value.replayIndex = 0;
		selected = -1;
		value.message = "Sequence cleared";
	}

	ImGui::BeginChild("##seq_replay_list", ImVec2(GUI::get().px(260.f), GUI::get().px(150.f)), ImGuiChildFlags_Borders);
	for (unsigned i = 0; i < value.sequence.size(); ++i) {
		ImGui::PushID(static_cast<int>(i));
		bool chosen = selected == static_cast<int>(i);
		if (ImGui::Selectable("##replay", chosen, ImGuiSelectableFlags_AllowDoubleClick)) selected = static_cast<int>(i);
		ImGui::SameLine();
		ImGui::TextUnformatted(value.sequence[i].name.c_str());
		ImGui::PopID();
	}
	ImGui::EndChild();

	GUI::get().sectionLabel("Selected");
	if (selected >= 0 && selected < static_cast<int>(value.sequence.size())) {
		auto& clip = value.sequence[static_cast<size_t>(selected)];
		ImGui::Text("Name: %s", clip.name.c_str());
		ImGui::SameLine();
		if (ImGui::Button("Remove")) {
			value.sequence.erase(value.sequence.begin() + selected);
			selected = -1;
		}
	}

	GUI::get().sectionLabel("Add");
	ImGui::SetNextItemWidth(GUI::get().px(120.f));
	ImGui::InputInt("Amount###macros", &amount);
	if (amount < 1) amount = 1;
	ImGui::SameLine();
	if (ImGui::Button("Add")) GUI::get().openMacroPicker(true, amount);
}

void GUI::drawClickTimes(bool& softs, float& softMs, bool& hards, float& hardSeconds, bool& micros, int& microMs, char const* suffix) {
	GUI::get().checkboxSetting((std::string("Soft Clicks###soft_") + suffix).c_str(), softs);
	ImGui::SameLine();
	GUI::get().checkboxSetting((std::string("Hard Clicks###hard_") + suffix).c_str(), hards);
	ImGui::SameLine();
	GUI::get().checkboxSetting((std::string("Micro Clicks###micro_") + suffix).c_str(), micros);
	ImGui::Text("Micro Times:"); ImGui::SameLine();
	ImGui::Text("0 to %ims", microMs);
	ImGui::Text("Soft Times:"); ImGui::SameLine();
	ImGui::Text("%ims to %.3fms", microMs, softMs);
	ImGui::Text("Regular Times:"); ImGui::SameLine();
	ImGui::Text("%.3fms to %.3fs", softMs, hardSeconds);
	ImGui::Text("Hard Times:"); ImGui::SameLine();
	ImGui::Text("%.3fs and up", hardSeconds);
	ImGui::SetNextItemWidth(GUI::get().px(160.f));
	GUI::get().dragFloatSetting((std::string("Soft ms###softms_") + suffix).c_str(), softMs, 1.f, 0.f, 5000.f, "%.0f");
	int micro = microMs;
	ImGui::SetNextItemWidth(GUI::get().px(160.f));
	if (ImGui::DragInt((std::string("Micro ms###micms_") + suffix).c_str(), &micro, 1.f, 0, 1000)) microMs = std::max(micro, 0);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	ImGui::SetNextItemWidth(GUI::get().px(160.f));
	GUI::get().dragFloatSetting((std::string("Hard seconds###hardms_") + suffix).c_str(), hardSeconds, 0.05f, 0.f, 30.f, "%.2f");
}
void GUI::drawClickbot() {
	auto& value = Logic::get();
	GUI::get().sectionLabel("Clickbot");
	GUI::get().checkboxSetting("Enabled###enable_clickbot", value.clickbot);
	ImGui::SameLine();
	static int player = 0;
	char const* players[] = {"Player 1", "Player 2"};
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
	GUI::get().comboItems("##dropdown_clickbot", players, 2, player);
	ImGui::Separator();

	auto& pack = player == 0 ? value.clickpack : value.clickpackP2;
	auto& volume = player == 0 ? value.clickbotVolume : value.clickbotVolumeP2;
	ImGui::SetNextItemWidth(GUI::get().px(100.f));
	GUI::get().dragFloatSetting(player == 0 ? "Volume Multiplier###vol_p1" : "Volume Multiplier###vol_p2", volume, 0.25f, 0.f, 10.f, "%.2f");

	auto foldersRoot = Logic::get().saveRoot() / "clickpacks";
	std::vector<std::string> folders;
	if (std::filesystem::exists(foldersRoot)) {
		for (auto const& entry : std::filesystem::directory_iterator(foldersRoot)) {
			if (entry.is_directory()) folders.push_back(entry.path().filename().string());
		}
	}
	std::string current = std::filesystem::path(pack).filename().string();
	if (current.empty()) current = pack;
	ImGui::SetNextItemWidth(GUI::get().px(220.f));
	if (ImGui::BeginCombo(player == 0 ? "##folder_dropdown_p1" : "##folder_dropdown_p2", current.empty() ? "Click Pack" : current.c_str())) {
		for (auto const& folder : folders) {
			if (ImGui::Selectable(folder.c_str(), folder == current)) {
				pack = (foldersRoot / folder).string();
				GUI::get().persist();
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Pick Clickpack")) {
		GUI::get().clickpacks().player = player;
		GUI::get().openClickpackPicker();
	}
	if (player == 0) {
		GUI::get().drawClickTimes(value.p1Softs, value.p1SoftMs, value.p1Hards, value.p1HardSeconds, value.p1Micros, value.p1MicroMs, "p1");
	} else {
		GUI::get().drawClickTimes(value.p2Softs, value.p2SoftMs, value.p2Hards, value.p2HardSeconds, value.p2Micros, value.p2MicroMs, "p2");
	}
}

void GUI::drawRender() {
	auto& value = Logic::get();
	GUI::get().sectionLabel("Capture");
	if (auto blocker = GUI::get().renderBlocker(); !blocker.empty()) {
		value.ffmpegMissing = true;
		if (ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		ImGui::TextWrapped("%s", blocker.c_str());
#ifdef GEODE_IS_WINDOWS
		if (ImGui::Button("Download FFmpeg")) {
			geode::utils::web::openLinkInBrowser("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip");
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip("Place ffmpeg.exe next to Geometry Dash once downloaded!");
#endif
		return;
	}
	value.ffmpegMissing = false;
	bool recording = Logic::get().renderActive();
	if (recording) {
		if (ImGui::Button("Stop")) Logic::get().stopRender();
	} else if (PlayLayer::get()) {
		if (ImGui::Button("Start Here")) Logic::get().startRender();
	} else if (value.renderQueued) {
		ImGui::TextUnformatted("Waiting to enter a level...");
		if (ImGui::Button("Cancel")) value.renderQueued = false;
	} else if (ImGui::Button("Start")) {
		Logic::get().startRender();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Begins rendering from your current position.");
	if (Logic::get().renderCapturing()) {
		auto status = Logic::get().renderStatus();
		float fraction = status.known ? status.progress : -1.f * static_cast<float>(ImGui::GetTime());
		ImGui::ProgressBar(fraction, ImVec2(GUI::get().px(380.f), 0.f), status.text.c_str());
	}
	ImGui::SameLine();
	if (value.mergeBusy) ImGui::BeginDisabled();
	if (ImGui::Button("Merge Renders")) GUI::get().openRenderPicker();
	if (value.mergeBusy) ImGui::EndDisabled();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Joins two videos back to back with a crossfade");

	ImGui::SetNextItemWidth(GUI::get().px(220.f));
	ImGui::InputTextWithHint("Video Name", value.macroName.empty() ? "Replay" : value.macroName.c_str(), &value.renderName);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave empty to name the video after the macro");
	
	ImGui::SetNextItemWidth(GUI::get().px(80.f));
	ImGui::InputText("Extension", &value.renderExtension);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	GUI::get().checkboxSetting("Auto Exit on Finish", value.renderAutoExit);
	GUI::get().checkboxSetting("Record the Opening Pause", value.renderRecordPause);
	ImGui::SetNextItemWidth(GUI::get().px(160.f));
	ImGui::InputText("Codec", &value.renderCodec);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	ImGui::SetNextItemWidth(GUI::get().px(160.f));
	ImGui::InputText("FFmpeg Args", &value.renderArgs);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output options placed after the codec information. Leave blank if you don't know what this is");
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();

	if (ImGui::Button("Render Settings")) ImGui::OpenPopup("Render Settings"); ImGui::SameLine();
	GUI::get().checkboxSetting("Immediate Restart", value.immediateRestart);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Restarts the level once as soon as you enter it");
	GUI::get().offerRebind("key-reset");
	if (!ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	GUI::get().sectionLabel("Picture");
	int width = value.renderWidth;
	int height = value.renderHeight;
	int fps = value.renderFps;
	float mbps = static_cast<float>(value.renderBitrate) / 1000000.f;
	ImGui::SetNextItemWidth(GUI::get().px(120.f));
	if (ImGui::DragInt("Width", &width, 1.f, 2, 10000)) value.renderWidth = std::max(width, 2);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(GUI::get().px(120.f));
	if (ImGui::DragInt("Height", &height, 1.f, 2, 10000)) value.renderHeight = std::max(height, 2);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	ImGui::SetNextItemWidth(GUI::get().px(120.f));
	if (ImGui::DragInt("FPS", &fps, 1.f, 1, 1000)) value.renderFps = std::max(fps, 1);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(GUI::get().px(120.f));
	char const* bitrateFormat = mbps <= 0.f ? "Lossless" : "%.1f Mbps";
	if (ImGui::DragFloat("Bitrate", &mbps, 0.1f, 0.f, 200.f, bitrateFormat)) value.renderBitrate = mbps <= 0.f ? 0 : std::max(static_cast<int>(mbps * 1000000.f), 1000);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();

	GUI::get().sectionLabel("Audio");
	ImGui::SetNextItemWidth(GUI::get().px(140.f));
	GUI::get().dragFloatSetting("Fade In", value.renderFadeIn, 0.05f, 0.f, 60.f, "%.2f s");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(GUI::get().px(140.f));
	GUI::get().dragFloatSetting("Fade Out", value.renderFadeOut, 0.05f, 0.f, 60.f, "%.2f s");
	ImGui::SetNextItemWidth(GUI::get().px(140.f));
	GUI::get().dragFloatSetting("After End", value.renderAfterEnd, 0.05f, 0.f, 120.f, "%.2f s");
	ImGui::SetNextItemWidth(GUI::get().px(140.f));
	GUI::get().dragFloatSetting("Music Volume", value.renderMusic, 0.01f, 0.f, 1.f, "%.2f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(GUI::get().px(140.f));
	GUI::get().dragFloatSetting("SFX Volume", value.renderSfx, 0.01f, 0.f, 1.f, "%.2f");
	GUI::get().checkboxSetting("Use Speedhack", value.renderSpeedhack);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Renders at your speedhack: the video shows the level that much faster or slower, with music and sounds pitched to match, like playing with speedhack.");
	GUI::get().checkboxSetting("SSB Fix", value.renderSsbFix);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixes audio sync because of the game's scroll speed bug when rendering.");
	ImGui::SameLine();
	GUI::get().checkboxSetting("Color Fix", value.renderColorFix);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixes video colors to be more accurate to the game.");
	GUI::get().checkboxSetting("Hide Labels", value.renderHideLabels);
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hides Echo's overlay labels while a render is capturing.");
	if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
