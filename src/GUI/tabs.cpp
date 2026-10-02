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

ImGuiChildFlags GUI::fit_card() {
    return ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY;
}

float GUI::column_width(std::initializer_list<char const*> labels) {
    float width = 0.f;
    for (char const* label : labels) width = std::max(width, GUI::get().label_width(label));
    return width;
}

void GUI::draw_main() {
    auto& value = Logic::get();
    bool busy = (value).is_recording() || (value).is_playing();
    float col = GUI::get().column_width({"Stop Recording", "Start Recording", "Stop Playing", "Start Playing", "Set TPS"});

    ImGui::BeginChild("##play-deck", ImVec2(0.f, 0.f), GUI::get().fit_card());
    char const* players[] = {"Both", "Player 1", "Player 2"};
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float comboW = GUI::get().combo_box_width(players, 3);
    float section = GUI::get().label_width("Record") + spacing + comboW + spacing + GUI::get().label_width("Play") + spacing + comboW;
    char const* recordLabel = (value).is_recording() ? "Stop Recording" : "Start Recording";
    char const* playLabel = (value).is_playing() ? "Stop Playing" : "Start Playing";
    auto checkRow = [](char const* text) {
        return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
    };
    float toggleNeed = std::max(GUI::get().label_width("Start Recording"), GUI::get().label_width("Stop Recording"));
    toggleNeed = std::max(toggleNeed, std::max(GUI::get().label_width("Start Playing"), GUI::get().label_width("Stop Playing")));
    float checks = checkRow("Lock TPS") + spacing + checkRow("Lock FPS") + spacing + checkRow("Real Time");
    section = std::max(section, std::max(checks, toggleNeed * 2.f + spacing));
    float buttonHalf = (section - spacing) * 0.5f;
    if (ImGui::Button(recordLabel, ImVec2(buttonHalf, 0.f))) Logic::get().toggle_recording();
    GUI::get().offer_rebind("toggle-record");
    ImGui::SameLine();
    if (ImGui::Button(playLabel, ImVec2(buttonHalf, 0.f))) Logic::get().toggle_playing();
    GUI::get().offer_rebind("toggle-play");
    if (busy) ImGui::BeginDisabled();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Record");
    ImGui::SameLine();
    int recordSel = GUI::get().pair_index(value.record_p1, value.record_p2);
    if (GUI::get().combo_items("##dropdown_recording_player", players, 3, recordSel)) {
        GUI::get().apply_pair(recordSel, value.record_p1, value.record_p2);
        GUI::get().persist();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Play");
    ImGui::SameLine();
    int playSel = GUI::get().pair_index(value.play_p1, value.play_p2);
    if (GUI::get().combo_items("##dropdown_playing_player", players, 3, playSel)) {
        GUI::get().apply_pair(playSel, value.play_p1, value.play_p2);
        GUI::get().persist();
    }
    if (busy) ImGui::EndDisabled();
    ImGui::Separator();
    static float inputFps = -1.f;
    if (inputFps < 0.f) inputFps = value.fps;
    static float inputVisual = -1.f;
    if (inputVisual < 0.f) inputVisual = value.playback_fps >= 1.f ? value.playback_fps : 240.f;
    float setW = std::max(GUI::get().label_width("Set TPS"), GUI::get().label_width("Set FPS"));
    float sliderW = std::max(section - setW - spacing, ImGui::GetFontSize() * 5.f);
    ImGui::SetNextItemWidth(sliderW);
    ImGui::DragFloat("###fps", &inputFps, 1.f, 1.f, FLT_MAX, "%.2f TPS");
    ImGui::SameLine();
    bool canSetTps = !(value).is_playing() || (value).is_recording();
    if (!canSetTps) ImGui::BeginDisabled();
    if (ImGui::Button("Set TPS", ImVec2(setW, 0.f))) {
        inputFps = std::clamp(inputFps, 1.f, FLT_MAX);
        Logic::get().set_tps(inputFps);
        inputFps = value.fps;
    }
    if (!canSetTps) ImGui::EndDisabled();
    
    ImGui::SetNextItemWidth(sliderW);
    ImGui::DragFloat("###playback_fps", &inputVisual, 1.f, 1.f, FLT_MAX, "%.2f FPS");
    ImGui::SameLine();
    if (ImGui::Button("Set FPS", ImVec2(setW, 0.f))) {
        inputVisual = std::clamp(inputVisual, 1.f, FLT_MAX);
        GUI::get().set_playback_fps(inputVisual);
    }
    if (GUI::get().checkbox_setting("Lock TPS", value.lock_delta)) GUI::get().drop_carry();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forces your TPS value to this. Turn this on for maximum accuracy, but it shouldn't be necessary.");
    ImGui::SameLine();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forces your FPS value to this. Essentially smooth fix, instead of lagging it will slow the game down.");
    ImGui::SameLine();
    if (GUI::get().checkbox_setting("Real Time", value.real_time_mode)) GUI::get().drop_carry();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Essentially the hard opposite of Lock FPS.");
    GUI::get().checkbox_setting("Sync Audio", value.sync_audio);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Syncs the audio if the game lags");
    ImGui::SameLine();
    GUI::get().checkbox_setting("Safe Mode", value.safeMode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Removes the ability to make progress when using hacks. You can still make progress if no hacks are on, though.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(section);
    float speed = value.speedhack;
    if (ImGui::DragFloat("###speed", &speed, 0.005f, 0.01f, 100.f, "Speed  %.3f")) GUI::get().set_speed(speed, false);
    GUI::get().offer_rebind("key-speedhack");
    if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().set_speed(value.speedhack, true);
    ImGui::EndChild();
    float deckH = ImGui::GetItemRectSize().y;

    ImGui::SameLine();
    ImGuiChildFlags replayFlags = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX;
    if (deckH < 8.f) replayFlags |= ImGuiChildFlags_AutoResizeY;
    ImGui::BeginChild("##play-replay", ImVec2(0.f, deckH > 8.f ? deckH : 0.f), replayFlags);
    std::string replay_txt = "Replay";
    std::string status_msg = replay_txt + Logic::get().message;

    ImGui::Text("TPS  %.2f", value.fps);
    ImGui::SameLine(0.f, 16.f);
    ImGui::Text("Inputs  %i", static_cast<int>(value.inputs.size()));

    float pair = std::max(GUI::get().label_width("Save"), GUI::get().label_width("Load"));
    pair = std::max(pair, std::max(GUI::get().label_width("Reset Level"), GUI::get().label_width("Close Menu")));
    float name = std::max(col, ImGui::GetFontSize() * 12.f);
    name = std::max(name, std::max(checkRow("No Overwrite"), checkRow("Ignore Actions")));
    name = std::max(name, pair * 2.f + spacing);
    float half = (name - spacing) * 0.5f;
    ImGui::SetNextItemWidth(name);
    ImGui::InputTextWithHint("###replay_name", "Replay name", &value.macro_name);
    if (ImGui::IsItemDeactivatedAfterEdit()) value.macro_path.clear();
    if (ImGui::Button("Save", ImVec2(half, 0.f))) Logic::get().write_file();
    GUI::get().offer_rebind("key-save");
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(half, 0.f))) Logic::get().pick_replay();
    GUI::get().offer_rebind("key-load");
    GUI::get().checkbox_setting("No Overwrite", value.no_overwrite);
    GUI::get().offer_rebind("key-no-overwrite", "Saves a new file using the overwrite name format instead of replacing the current replay.");
    GUI::get().checkbox_setting("Ignore Actions", value.ignore_inputs);
    GUI::get().offer_rebind("key-ignore");
    ImGui::Separator();
    if (ImGui::Button("Reset Level", ImVec2(half, 0.f))) {
        if (auto* play = PlayLayer::get()) play->resetLevel();
    }
    GUI::get().offer_rebind("key-reset");
    ImGui::SameLine();
    if (ImGui::Button("Close Menu", ImVec2(half, 0.f))) GUI::get().begin_close();
    GUI::get().offer_rebind("open-menu");

    ImGui::EndChild();
}

namespace {
float tool_column() {
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    auto checkW = [](char const* text) {
        return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
    };
    auto fieldW = [](char const* text, float widget) {
        return widget + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(text).x;
    };
    float settingsNeed = GUI::get().label_width("Settings");
    float advanceNeed = GUI::get().label_width("Advance");
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

void GUI::draw_tools() {
    auto& value = Logic::get();
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float settingsNeed = GUI::get().label_width("Settings");
    float advanceNeed = GUI::get().label_width("Advance");
    float tools = tool_column();
    auto settingsWidth = [&]() {
        float used = ImGui::GetItemRectSize().x + spacing;
        return std::max(settingsNeed, tools - used);
    };
    char const* noclip[] = {"Off", "Player 1", "Player 2", "Both"};
    int noclipSel = GUI::get().quad_index(value.noclip_p1, value.noclip_p2);
    ImGui::SetNextItemWidth(GUI::get().px(150.f));
    if (GUI::get().combo_items("Noclip##dropdown_noclip", noclip, 4, noclipSel)) {
        GUI::get().apply_quad(noclipSel, value.noclip_p1, value.noclip_p2);
        GUI::get().persist();
    }
    GUI::get().checkbox_setting("Click Both Players", value.click_both);
    GUI::get().offer_rebind("key-click-both");
    ImGui::SameLine();
    GUI::get().checkbox_setting("Swap Player Input", value.swap_players);
    {
        char const* follow[] = {"Off", "P2 follows P1", "P1 follows P2"};
        ImGui::SetNextItemWidth(GUI::get().px(150.f));
        if (GUI::get().combo_items("Follow Player##dropdown_follow", follow, 3, value.follow_mode)) GUI::get().persist();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("For two player levels, the following player will copy the followed player after that many frames or milliseconds.");
        if (value.follow_mode != 0) {
            ImGui::SetNextItemWidth(GUI::get().px(80.f));
            if (ImGui::DragFloat("##follow_delay", &value.follow_delay, value.follow_delay_ms ? 1.f : 0.25f, 0.f, 100000.f, value.follow_delay_ms ? "%.1f" : "%.0f")) value.follow_delay = std::max(value.follow_delay, 0.f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("How far behind the follower is. %d steps at the current TPS.", Logic::get().follow_steps());
            if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().persist();
            ImGui::SameLine();
            char const* units[] = {"frames", "ms"};
            int unit = value.follow_delay_ms ? 1 : 0;
            if (GUI::get().combo_items("Delay##dropdown_follow_unit", units, 2, unit, GUI::get().px(90.f))) {
                value.follow_delay_ms = unit == 1;
                GUI::get().persist();
            }
        }
    }
    {
        bool locked = value.is_recording() || value.is_playing();
        bool unrounded = value.shown_unrounded();
        ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("Velocity Unrounding", &unrounded)) {
            value.set_unrounded(unrounded);
            GUI::get().persist();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Stops GD rounding the player's y velocity to three decimals (it's chizz's Velocity Unrounding mod). CHANGES PHYSICS!");
        ImGui::EndDisabled();
    }

    GUI::get().checkbox_setting("Maintain Gravity", value.maintain_gravity);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording, the wave is controlled as if gravity were always normal.");
    if (value.maintain_gravity && GUI::get().two_player_dual()) {
        ImGui::SameLine();
        GUI::get().checkbox_setting("Same Gravity", value.same_gravity);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ensures that Maintain Gravity keeps both players on the same gravity.");
    }

    char const* inverse[] = {"Off", "Player 1", "Player 2", "Both"};
    int inverseSel = GUI::get().quad_index(value.inverse_p1, value.inverse_p2);
    ImGui::SetNextItemWidth(GUI::get().px(200.f));
    if (GUI::get().combo_items("Inverse Inputs##dropdown_inverse", inverse, 4, inverseSel)) {
        GUI::get().apply_quad(inverseSel, value.inverse_p1, value.inverse_p2);
        GUI::get().persist();
    }
    
    GUI::get().checkbox_setting("Autoclicker###autoclick_checkbox", value.autoclicker);
    GUI::get().offer_rebind("key-autoclick");
    ImGui::SameLine();
    if (ImGui::Button("Settings###autoclick_settings", ImVec2(settingsWidth(), 0.f))) ImGui::OpenPopup("Autoclicker Settings");
    if (ImGui::BeginPopupModal("Autoclicker Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        GUI::get().checkbox_setting("Enable Autoclicker", value.autoclicker);
        char const* autoPlayers[] = {"Player 1", "Player 2", "Both"};
        int autoSel = value.autoclick_p1 && value.autoclick_p2 ? 2 : value.autoclick_p2 ? 1 : 0;
        if (GUI::get().combo_items("Player##dropdown_autoclicker", autoPlayers, 3, autoSel)) {
            value.autoclick_p1 = autoSel != 1;
            value.autoclick_p2 = autoSel != 0;
            GUI::get().persist();
        }
        int press = value.auto_click.press_interval;
        int release = value.auto_click.release_interval;
        ImGui::SetNextItemWidth(GUI::get().px(90.f));
        if (ImGui::DragInt("Click Length", &press, 1.f, 0, 10000)) value.auto_click.press_interval = std::max(press, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 makes every click a swift click.");
        if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().persist();
        ImGui::SetNextItemWidth(GUI::get().px(90.f));
        if (ImGui::DragInt("Release Length", &release, 1.f, 1, 10000)) value.auto_click.release_interval = std::max(release, 1);
        if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().persist();
        GUI::get().checkbox_setting("Auto Disable", value.autoclick_auto_disable);
        int disable_in = value.auto_click.disable_in;
        if (!value.autoclick_auto_disable) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(GUI::get().px(90.f));
        if (ImGui::DragInt("Frames", &disable_in, 1.f, 1, 10000)) value.auto_click.disable_in = std::max(disable_in, 1);
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        if (!value.autoclick_auto_disable) ImGui::EndDisabled();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    bool advancing = value.frame_advance;
    if (ImGui::Checkbox("Frame Advance###advance_checkbox", &advancing)) GUI::get().set_frame_advance(advancing);
    GUI::get().offer_rebind("toggle-advance");
    ImGui::SameLine();
    float advanceRoom = std::max(advanceNeed + spacing + settingsNeed, tools - ImGui::GetItemRectSize().x - spacing);
    float advanceW = std::max(advanceNeed, (advanceRoom - spacing) * 0.5f);
    float advanceSettingsW = std::max(settingsNeed, advanceRoom - spacing - advanceW);
    GUI::get().offer_rebind("step-frame");
    ImGui::SameLine();
    if (ImGui::Button("Settings###advance_settings", ImVec2(advanceSettingsW, 0.f))) ImGui::OpenPopup("Frame Advance Settings");
    if (ImGui::BeginPopupModal("Frame Advance Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        bool advancingHere = value.frame_advance;
        if (ImGui::Checkbox("Frame Advance", &advancingHere)) GUI::get().set_frame_advance(advancingHere);
        int hold = static_cast<int>(value.advance_hold_ms);
        int speed = static_cast<int>(std::lround(value.advance_speed));
        ImGui::SetNextItemWidth(GUI::get().px(90.f));
        if (ImGui::DragInt("Hold Time (ms)", &hold, 1.f, 0, 5000)) value.advance_hold_ms = static_cast<unsigned>(std::max(hold, 0));
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        ImGui::SetNextItemWidth(GUI::get().px(90.f));
        if (ImGui::DragInt("Speed", &speed, 1.f, 1, 200, "%d%%")) value.advance_speed = static_cast<float>(std::clamp(speed, 1, 200));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Percent of your TPS, scaled with speedhack. 100%% at 1x will advance you in real time.");
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        ImGui::Separator();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording, keeps saved states to step backwards to. Saves every step at 240 TPS; at higher TPS it saves less often and replays your inputs from the last save.");
        ImGui::SameLine();
        GUI::get().offer_rebind("step-back");
        ImGui::SetNextItemWidth(GUI::get().px(90.f));
        if (ImGui::DragInt("Saved States", &value.backstep_percent, 1.f, 1, 10000, "%d%%")) value.backstep_percent = std::clamp(value.backstep_percent, 1, 10000);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many states to keep, in percent of your TPS. 100%% keeps as many as your TPS (240 at 240 TPS).");
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        GUI::get().draw_keybind_prompt(true);
        ImGui::EndPopup();
    }

    if (GUI::get().checkbox_setting("Prevent Death", value.pause_before_kill)) Logic::get().reset_traj();
    GUI::get().offer_rebind("key-pause-before-kill", "While recording, frame advance stops on the frame before a death instead of dying. Works without Show Trajectory.");
    if (GUI::get().checkbox_setting("Trajectory Autoclicker", value.traj_autoclicker)) Logic::get().reset_traj();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("When the next frame would kill you, starts holding if released or releases if held. Works without Show Trajectory.");
    GUI::get().section_label("Orb Assists (while recording)");
    if (GUI::get().checkbox_setting("Click Dash Orbs", value.click_dash_orbs)) Logic::get().reset_traj();
    GUI::get().offer_rebind("key-click-dash-orbs", "Swift-clicks each stacked dash orb and stores those clicks. Does not affect playback.");
    if (GUI::get().checkbox_setting("Click Black Orbs", value.click_black_orbs)) Logic::get().reset_traj();
    GUI::get().offer_rebind("key-click-black-orbs", "Swift-clicks black orbs during UFO sections. Does not affect playback.");
    if (GUI::get().checkbox_setting("Auto Black Orb on Pad", value.pad_black_orb)) Logic::get().reset_traj();
    GUI::get().offer_rebind("key-pad-black-orb", "When the player hits a pad, swift-clicks the black orbs it is touching. Stored in the recording.");
    if (GUI::get().checkbox_setting("Auto Blue Orb on Pad", value.pad_blue_orb)) Logic::get().reset_traj();
    GUI::get().offer_rebind("key-pad-blue-orb", "When the player hits a pad, swift-clicks a blue orb it is touching. Stored in the recording.");
}

void GUI::draw_visuals() {
    auto& value = Logic::get();
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float settingsNeed = GUI::get().label_width("Settings");
    float tools = tool_column();
    auto settingsWidth = [&]() {
        float used = ImGui::GetItemRectSize().x + spacing;
        return std::max(settingsNeed, tools - used);
    };
    char const* hide[] = {"Off", "Player 1", "Player 2", "Both"};
    int hideSel = GUI::get().quad_index(value.hide_p1, value.hide_p2);
    ImGui::SetNextItemWidth(GUI::get().px(150.f));
    if (GUI::get().combo_items("Hide Player##dropdown_hideplayer", hide, 4, hideSel)) {
        GUI::get().apply_quad(hideSel, value.hide_p1, value.hide_p2);
        if (value.traj_follow_hide_player) Logic::get().reset_traj();
        GUI::get().persist();
    }
    GUI::get().checkbox_setting("Disable Shake Effects", value.disable_shakes);
    GUI::get().offer_rebind("key-shakes");
    GUI::get().checkbox_setting("Disable Death Effect", value.disable_death_effect);
    if (GUI::get().checkbox_setting("No Mirror Portals", value.no_mirror_portals)) GUI::get().apply_no_mirror();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops mirror portals from flipping the level.");
    GUI::get().checkbox_setting("Fix Rotation Bug", value.fix_tps_rotation);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixes gamemode rotation on high TPS without affecting hitboxes.");
    GUI::get().checkbox_setting("Layout###layout_checkbox", value.layout_mode);
    GUI::get().offer_rebind("key-layout");
    ImGui::SameLine();
    if (ImGui::Button("Settings###layout_settings", ImVec2(settingsWidth(), 0.f))) ImGui::OpenPopup("Layout Settings###layoutsettings2");
    if (ImGui::BeginPopupModal("Layout Settings###layoutsettings2", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        GUI::get().checkbox_setting("Layout Mode", value.layout_mode);
        GUI::get().checkbox_setting("Use Regular Background", value.layout_use_regular_bg);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uses the level's actual background");
        if (ImGui::ColorEdit3("Background Color", value.backgroundColor, ImGuiColorEditFlags_NoInputs)) Logic::get().save_settings_file();
        if (ImGui::ColorEdit3("Ground Color", value.blocks_color, ImGuiColorEditFlags_NoInputs)) Logic::get().save_settings_file();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    GUI::get().checkbox_setting("Hitboxes###hitbox_checkbox", value.show_hitboxes);
    GUI::get().offer_rebind("key-hitboxes");
    ImGui::SameLine();
    if (ImGui::Button("Settings###notboxsettings", ImVec2(settingsWidth(), 0.f))) {
        ImGui::OpenPopup("Hitbox Settings###boxsettings");
    }
    if (ImGui::BeginPopupModal("Hitbox Settings###boxsettings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto hitboxColor = [](char const* label, bool& enabled, float* color) {
            GUI::get().checkbox_setting(label, enabled);
            ImGui::SameLine();
            std::string id = std::string("##") + label;
            if (ImGui::ColorEdit4(id.c_str(), color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().save_settings_file();
        };
        GUI::get().checkbox_setting("Show Hitboxes", value.show_hitboxes);
        GUI::get().checkbox_setting("Fill Hitboxes", value.fill_hitboxes);
        GUI::get().checkbox_setting("Outline Only", value.hitbox_outline_only);
        GUI::get().checkbox_setting("Hide Activated Objects", value.hide_activated_objects);
        GUI::get().checkbox_setting("Follow Hide Player", value.hitboxes_follow_hide_player);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Will hide the hitboxes of the hidden player.");
        {
            char const* hideHit[] = {"Off", "Player 1", "Player 2", "Both"};
            int hideHitSel = GUI::get().quad_index(value.hide_hitbox_p1, value.hide_hitbox_p2);
            if (value.hitboxes_follow_hide_player) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(GUI::get().px(150.f));
            if (GUI::get().combo_items("Hide Hitboxes##dropdown_hidehitbox", hideHit, 4, hideHitSel)) {
                GUI::get().apply_quad(hideHitSel, value.hide_hitbox_p1, value.hide_hitbox_p2);
                Logic::get().save_settings_file();
            }
            if (value.hitboxes_follow_hide_player) ImGui::EndDisabled();
        }
        ImGui::SetNextItemWidth(GUI::get().px(180.f));
        GUI::get().drag_float_setting("Thickness", value.hitbox_thickness, 0.01f, -1000000.f, 1000000.f, "%.3f");
        ImGui::SetNextItemWidth(GUI::get().px(180.f));
        ImGui::SetNextItemWidth(GUI::get().px(180.f));
        if (ImGui::DragFloat("Fill Opacity", &value.hitbox_fill_opacity, 0.01f, 0.f, 1.f, "%.2f")) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        int trail = value.trail_length;
        ImGui::SetNextItemWidth(GUI::get().px(180.f));
        char const* trailFormat = trail <= 0 ? "TPS" : "%d";
        if (ImGui::DragInt("Trail Length", &trail, 1.f, 0, static_cast<int>(value.fps) * 10, trailFormat)) value.trail_length = std::max(trail, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many frames the trail and the click and release boxes stay on screen");
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        GUI::get().checkbox_setting("Show Trail", value.hitbox_trail);
        ImGui::SetNextItemWidth(GUI::get().px(180.f));
        if (ImGui::DragFloat("Trail Opacity", &value.hitbox_trail_opacity, 0.01f, 0.f, 1.f, "%.2f")) value.hitbox_trail_opacity = std::clamp(value.hitbox_trail_opacity, 0.f, 1.f);
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        GUI::get().checkbox_setting("Trail at visual FPS", value.hitbox_trail_visual);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Displays one trail hitbox per displayed frame");
        ImGui::Separator();
        hitboxColor("Player box", value.hitbox_player, value.hitbox_outer_color);
        hitboxColor("Click", value.hitbox_click, value.hitbox_click_color);
        ImGui::SameLine();
        if (ImGui::ColorEdit4("Release", value.hitbox_release_color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().save_settings_file();
        hitboxColor("Rotated box", value.hitbox_rotated, value.hitbox_rotated_color);
        hitboxColor("Inner box", value.hitbox_inner, value.hitbox_inner_color);
        hitboxColor("Player circle", value.hitbox_circle, value.hitbox_circle_color);
        hitboxColor("Solids", value.hitbox_solid, value.hitbox_solid_color);
        hitboxColor("Slopes", value.hitbox_slope, value.hitbox_slope_color);
        hitboxColor("Hazards", value.hitbox_hazard, value.hitbox_hazard_color);
        hitboxColor("Orbs, pads, and portals", value.hitbox_interact, value.hitbox_interact_color);
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    GUI::get().checkbox_setting("Trajectory###trajectory_checkbox", value.show_traj);
    GUI::get().offer_rebind("key-trajectory");
    ImGui::SameLine();
    if (ImGui::Button("Settings###trajectory_settings", ImVec2(settingsWidth(), 0.f))) ImGui::OpenPopup("Trajectory Settings");
    if (ImGui::BeginPopupModal("Trajectory Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        GUI::get().checkbox_setting("Show Trajectory", value.show_traj);
        if (GUI::get().checkbox_setting("Moving Object Support", value.traj_moving_objects)) Logic::get().reset_traj();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The trajectory moves the level's moving objects along with it. Off: objects stand still in the trajectory, which is faster.");
        if (GUI::get().checkbox_setting("Follow Hide Player", value.traj_follow_hide_player)) Logic::get().reset_traj();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Will hide the trajectory of the hidden player.");
        {
            char const* hideTraj[] = {"Off", "Player 1", "Player 2", "Both"};
            int hideTrajSel = GUI::get().quad_index(value.hide_traj_p1, value.hide_traj_p2);
            if (value.traj_follow_hide_player) ImGui::BeginDisabled();
            ImGui::SetNextItemWidth(GUI::get().px(150.f));
            if (GUI::get().combo_items("Hide Trajectory##dropdown_hidetraj", hideTraj, 4, hideTrajSel)) {
                GUI::get().apply_quad(hideTrajSel, value.hide_traj_p1, value.hide_traj_p2);
                Logic::get().reset_traj();
                Logic::get().save_settings_file();
            }
            if (value.traj_follow_hide_player) ImGui::EndDisabled();
        }
        GUI::get().checkbox_setting("Hold", value.traj_hold);
        ImGui::SameLine();
        GUI::get().checkbox_setting("Release", value.traj_release);
        ImGui::SameLine();
        GUI::get().checkbox_setting("Swift", value.traj_swift);
        float seconds = value.traj_seconds;
        ImGui::SetNextItemWidth(GUI::get().px(160.f));
        if (ImGui::DragFloat("Length (s)", &seconds, 0.05f, 0.1f, 4.f, "%.2f")) value.traj_seconds = std::clamp(seconds, 0.1f, 4.f);
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        ImGui::SetNextItemWidth(GUI::get().px(160.f));
        GUI::get().drag_float_setting("Width", value.traj_width, 0.05f, 0.1f, 4.f, "%.2f");
        if (ImGui::ColorEdit4("Hold color", value.traj_color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().save_settings_file();
        ImGui::SameLine();
        if (ImGui::ColorEdit4("Swift color", value.traj_swift_color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().save_settings_file();
        ImGui::SameLine();
        if (ImGui::ColorEdit4("Release color", value.traj_release_color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) Logic::get().save_settings_file();
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

std::string GUI::format_plain(Session const& value) {
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

void GUI::apply_plain(std::string const& text) {
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line)) return;
    float fps = 0.f;
    std::istringstream header(line);
    if (header >> fps && std::isfinite(fps) && fps >= 1.f) {
        auto& value = Logic::get();
        if (!(value).is_recording() && !(value).is_playing()) Logic::get().set_tps(fps);
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
    Logic::get().input_ids.clear();
    Logic::get().touch_inputs();
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps(Logic::get());
    Logic::get().autosave_replay();
}

void GUI::draw_edit_fields(bool enabled, float column) {
    auto& inputs = Logic::get().inputs;
    if (!enabled) ImGui::BeginDisabled();
    int count = enabled ? GUI::get().selected_count() : 0;
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
    ImGui::PushID(fmt::format("{}:{}:{}", GUI::get().selected_id, GUI::get().anchor_id, GUI::get().selected_id != 0 ? -1 : GUI::get().selected).c_str());
    bool frameEdited = ImGui::InputInt("Frame", &frame);
    ImGui::PopID();
    if (frameEdited && enabled) {
        int delta = std::max(frame, 0) - beforeFrame;
        rewrite_selected([&](Frame& input) { input.number = std::max(input.number + delta, 0); });
    }
    if (ImGui::Checkbox("Down", &down) && enabled) rewrite_selected([&](Frame& input) { input.holding = down; });
    ImGui::SameLine();
    if (ImGui::Checkbox("Player 2", &player2) && enabled) rewrite_selected([&](Frame& input) { input.player2 = player2; });
    char const* buttonNames[] = {"Jump", "Left", "Right"};
    int button = enabled ? GUI::get().draft.button : static_cast<int>(PlayerButton::Jump);
    if (button == 0) button = static_cast<int>(PlayerButton::Jump);
    int buttonSel = button == static_cast<int>(PlayerButton::Left) ? 1 : button == static_cast<int>(PlayerButton::Right) ? 2 : 0;
    float buttonLabel = ImGui::CalcTextSize("Button").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(GUI::get().combo_box_width(buttonNames, 3), column - buttonLabel));
    if (ImGui::Combo("Button", &buttonSel, buttonNames, 3) && enabled) {
        int next = buttonSel == 1 ? static_cast<int>(PlayerButton::Left) : buttonSel == 2 ? static_cast<int>(PlayerButton::Right) : static_cast<int>(PlayerButton::Jump);
        rewrite_selected([&](Frame& input) { input.button = next; });
    }
    int first = 0;
    int last = 0;
    bool span = GUI::get().selection_span(first, last);
    float moveW = std::max(GUI::get().label_width("Move Up"), GUI::get().label_width("Move Down"));
    if (ImGui::Button("Move Up", ImVec2(moveW, 0.f)) && enabled && span && first > 0) move_selected(-1);
    ImGui::SameLine();
    if (ImGui::Button("Move Down", ImVec2(moveW, 0.f)) && enabled && span && last + 1 < static_cast<int>(inputs.size())) move_selected(1);
    if (!enabled) ImGui::EndDisabled();
}

void GUI::draw_macro() {
    auto& value = Logic::get();
    GUI::get().section_label("Files");
    static int convertSel = 0;
    char const* formats[] = {"JSON", "Plain Text", "Silicate", "TCBot", "GDR"};
    float fileSpacing = ImGui::GetStyle().ItemSpacing.x;
    float exportLabel = ImGui::CalcTextSize("Export As").x + ImGui::GetStyle().ItemInnerSpacing.x;
    float filePair = GUI::get().label_width("Convert Folder");
    filePair = std::max(filePair, GUI::get().label_width("Open Folder"));
    filePair = std::max(filePair, std::max(GUI::get().label_width("Export"), GUI::get().label_width("Import")));
    filePair = std::max(filePair, std::max(GUI::get().label_width("Labels"), GUI::get().label_width("Metadata")));
    filePair = std::max(filePair, std::max(GUI::get().label_width("Overwrite Name"), GUI::get().label_width("Reset Macro")));
    float files = filePair * 2.f + fileSpacing;
    float comboNeed = GUI::get().combo_box_width(formats, 5);
    if (exportLabel + comboNeed > files) files = exportLabel + comboNeed;
    float comboW = std::max(comboNeed, files - exportLabel);
    float fileHalf = (files - fileSpacing) * 0.5f;
    GUI::get().combo_items("Export As", formats, 5, convertSel, comboW);
    if (ImGui::Button("Export", ImVec2(fileHalf, 0.f))) Logic::get().export_kind(formats[convertSel]);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Converts the current replay to the converted folder");
    ImGui::SameLine();
    if (ImGui::Button("Import", ImVec2(fileHalf, 0.f))) Logic::get().import_replays();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Import a replay of the selected type to convert to Echo");
    if (ImGui::Button("Convert Folder", ImVec2(fileHalf, 0.f))) Logic::get().convert_legacy_echoes();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rewrites all other replay types in the replays folder into .echo files. Old Echo replays are renamed to .old.echo.");
    ImGui::SameLine();
    if (ImGui::Button("Open Folder", ImVec2(fileHalf, 0.f))) geode::utils::file::openFolder(Logic::get().save_root() / "replays");
    if (ImGui::Button("Labels", ImVec2(fileHalf, 0.f))) ImGui::OpenPopup("Label Settings");
    ImGui::SameLine();
    if (ImGui::Button("Metadata", ImVec2(fileHalf, 0.f))) ImGui::OpenPopup("Replay Metadata");

    GUI::get().section_label("Seed");
    if (value.lvl_seed_known && PlayLayer::get()) ImGui::Text("Level seed: %llu", static_cast<unsigned long long>(value.lvl_seed));
    else ImGui::TextUnformatted("Level seed: not set");
    if (GUI::get().checkbox_setting("Override", value.seed_override)) GUI::get().apply_seed_override();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Overrides the game seed to this value");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(GUI::get().px(140.f));
    ImGui::InputScalar("##seed_override", ImGuiDataType_U64, &value.seed_override_value);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        GUI::get().apply_seed_override();
        Logic::get().save_settings_file();
    }

    GUI::get().section_label("Display");
    float cpsDrag = ImGui::GetFontSize() * 6.f;
    float cpsLabel = ImGui::CalcTextSize("Max CPS").x + ImGui::GetStyle().ItemInnerSpacing.x;
    float cpsW = std::max(GUI::get().label_width("CPS"), files - cpsDrag - cpsLabel - fileSpacing);
    ImGui::SetNextItemWidth(cpsDrag);
    ImGui::DragInt("Max CPS", &value.max_cps, 0.4f, 1, 100);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is based on ILL CPS rules.");
    ImGui::SameLine();
    if (ImGui::Button("CPS", ImVec2(cpsW, 0.f))) ImGui::OpenPopup("CPS Breaks###cps_breaks");
    if (ImGui::BeginPopupModal("CPS Breaks###cps_breaks", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Highest CPS: %s", value.highest_cps.c_str());
        ImGui::Text("Current CPS: %.2f", value.current_cps);
        ImGui::Separator();
        int p1 = static_cast<int>(value.cps_notes_p1.size());
        int total = p1 + static_cast<int>(value.cps_notes_p2.size());
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
                    auto const& note = second ? value.cps_notes_p2[i - p1] : value.cps_notes_p1[i];
                    std::string span = note.first == note.last
                        ? std::to_string(note.first)
                        : std::to_string(note.first) + "-" + std::to_string(note.last);
                    std::string label = std::string(second ? "P2  " : "P1  ") + span + "  " + note.text + "##cps-" + std::to_string(i);
                    if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0.f, lineH))) {
                        GUI::get().show_cps_range(note.first, note.last, second);
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
    if (ImGui::Button("Overwrite Name", ImVec2(files, 0.f))) ImGui::OpenPopup("Overwrite Name Format");
    if (ImGui::BeginPopupModal("Overwrite Name Format", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.f);
        ImGui::InputText("Renaming Format", &value.rename_format);
        if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::Button("Reset Macro", ImVec2(files, 0.f)) && !value.inputs.empty()) ImGui::OpenPopup("Confirm Reset");
    if (ImGui::BeginPopupModal("Confirm Reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Reset the current macro?");
        float confirmW = GUI::get().shared_button_width({"Yes", "No"});
        if (ImGui::Button("Yes", ImVec2(confirmW, 0.f))) {
            GUI::get().reset_macro();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(confirmW, 0.f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Replay Metadata", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        int total = static_cast<int>(value.recording_seconds);
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
    if (ImGui::Checkbox("Save Debug Samples", &value.save_debug)) {
        value.format = value.save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
        GUI::get().persist();
    }
}

void GUI::show_cps_range(int first, int last, bool player2) {
    if (last < first) std::swap(first, last);
    mark_first = first;
    mark_last = last;
    mark_player = player2 ? 1 : 0;
    show_editor_p1 = !player2;
    show_editor_p2 = player2;
    auto_scroll = false;
    plain = false;
    page = "Editor";
    scroll_to_mark = true;
    Logic::get().identify_inputs();
    select_input(-1);
    auto const& inputs = Logic::get().inputs;
    for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
        if (!in_cps_mark(inputs[i])) continue;
        select_input(i);
        break;
    }
}

bool GUI::shift_selecting() const {
    if (ImGui::GetIO().KeyShift) return true;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) return true;
    if (auto* keys = CCKeyboardDispatcher::get()) return keys->getShiftKeyPressed();
    return false;
}

void GUI::select_input(int index, bool range) {
    auto& inputs = Logic::get().inputs;
    if (index < 0 || index >= static_cast<int>(inputs.size())) {
        selected = -1;
        selected_id = 0;
        anchor = -1;
        anchor_id = 0;
        return;
    }
    if (Logic::get().input_id(static_cast<size_t>(index)) == 0) Logic::get().identify_inputs(true);
    selected = index;
    selected_id = Logic::get().input_id(static_cast<size_t>(index));
    draft = inputs[index];
    if (!range || anchor < 0 || anchor >= static_cast<int>(inputs.size())) {
        anchor = selected;
        anchor_id = selected_id;
    }
}
void GUI::follow_selection() {
    auto& value = Logic::get();
    auto& inputs = value.inputs;
    auto find = [&](uint64_t id, int fallback) {
        if (id != 0) {
            if (fallback >= 0 && fallback < static_cast<int>(inputs.size()) &&
                value.input_id(static_cast<size_t>(fallback)) == id) return fallback;
            for (int i = 0; i < static_cast<int>(inputs.size()); ++i) {
                if (value.input_id(static_cast<size_t>(i)) == id) return i;
            }
            return -1;
        }
        if (fallback >= 0 && fallback < static_cast<int>(inputs.size())) return fallback;
        return -1;
    };
    selected = find(selected_id, selected);
    if (selected >= 0) {
        if (selected_id == 0) selected_id = value.input_id(static_cast<size_t>(selected));
        draft = inputs[selected];
    } else {
        selected_id = 0;
    }
    anchor = find(anchor_id, anchor_id != 0 ? anchor : selected);
    if (anchor >= 0) {
        if (anchor_id == 0) anchor_id = value.input_id(static_cast<size_t>(anchor));
    } else {
        anchor = selected;
        anchor_id = selected_id;
    }
}
void stroke_selected_button() {
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    float rounding = ImGui::GetStyle().FrameRounding;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRect(min, max, IM_COL32(0, 0, 0, 230), rounding, 0, 3.f);
    draw->AddRect(ImVec2(min.x + 1.5f, min.y + 1.5f), ImVec2(max.x - 1.5f, max.y - 1.5f), IM_COL32(255, 255, 255, 245), rounding, 0, 1.5f);
}
void GUI::draw_editor() {
    auto& value = Logic::get();
    auto& inputs = value.inputs;
    Logic::get().identify_inputs();
    GUI::get().follow_selection();
    if (GUI::get().selected >= 0 && !GUI::get().editor_shows(inputs[GUI::get().selected])) GUI::get().select_input(-1);

    ImGuiStyle const& editStyle = ImGui::GetStyle();
    float listW = GUI::get().label_width("Release Right at 0000000") + editStyle.ScrollbarSize + editStyle.WindowPadding.x * 2.f + editStyle.ChildBorderSize * 2.f;
    float editPad = editStyle.WindowPadding.x * 2.f + editStyle.ChildBorderSize * 2.f;
    float pairW = std::max(GUI::get().label_width("Add Input"), GUI::get().label_width("Delete Inputs"));
    pairW = std::max(pairW, std::max(GUI::get().label_width("Move Up"), GUI::get().label_width("Move Down")));
    float editRow = pairW * 2.f + editStyle.ItemSpacing.x;
    float checkW = ImGui::GetFrameHeight() + editStyle.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto Scroll To Frame").x;
    float sortW = ImGui::GetFrameHeight() + editStyle.ItemInnerSpacing.x + ImGui::CalcTextSize("Sort Automatically").x + editStyle.ItemSpacing.x + ImGui::CalcTextSize("(?)").x;
    float editSingle = std::max(GUI::get().label_width("Set to Current Frame"), std::max(checkW, sortW));
    float editInner = std::max(editSingle, editRow);
    float editW = editInner + editPad;
    static float editorPanelH = 0.f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Show");
    ImGui::SameLine();
    float used = ImGui::GetItemRectSize().x + editStyle.ItemSpacing.x;
    char const* players[] = {"Both", "Player 1", "Player 2"};
    int showSel = GUI::get().pair_index(GUI::get().show_editor_p1, GUI::get().show_editor_p2);
    std::string tpsLabel = value.rates.empty() ? "TPS Changes" : fmt::format("TPS Changes - {}", value.rates.size());
    float comboNeed = GUI::get().combo_box_width(players, 3);
    float buttonNeed = GUI::get().label_width(tpsLabel.c_str());
    float comboW = std::max(comboNeed, listW - used);
    float buttonW = std::max(buttonNeed, editW);
    if (GUI::get().combo_items("##editor_show", players, 3, showSel, comboW)) {
        GUI::get().apply_pair(showSel, GUI::get().show_editor_p1, GUI::get().show_editor_p2);
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
                    GUI::get().mark_first = change.frame;
                    GUI::get().mark_last = change.frame;
                    GUI::get().mark_player = -1;
                    GUI::get().scroll_to_mark = true;
                    GUI::get().auto_scroll = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndChild();
        }
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    float listH = editorPanelH > 1.f ? editorPanelH : ImGui::GetFrameHeightWithSpacing() * 10.f;
    ImGui::BeginChild("##echo-inputs", ImVec2(listW, listH), ImGuiChildFlags_Borders);
    if (inputs.empty() && !GUI::get().plain) {
        GUI::get().select_input(-1);
        GUI::get().scroll_to_mark = false;
        ImGui::TextUnformatted("No Inputs");
    } else if (GUI::get().plain) {
        GUI::get().selected = -1;
        if (GUI::get().plain_text.empty()) GUI::get().plain_text = GUI::get().format_plain(value);
        if (ImGui::InputTextMultiline("##echo-plain", &GUI::get().plain_text, ImVec2(-1.f, -1.f))) GUI::get().apply_plain(GUI::get().plain_text);
    } else {
        bool everyRow = GUI::get().show_editor_p1 && GUI::get().show_editor_p2;
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
            bool kept = filtered.owner == &inputs && filtered.revision == value.inputs_revision && filtered.count <= inputs.size() &&
                filtered.p1 == GUI::get().show_editor_p1 && filtered.p2 == GUI::get().show_editor_p2 &&
                (filtered.count < inputs.size() || filtered.data == inputs.data());
            if (!kept) {
                filtered.rows.clear();
                filtered.count = 0;
            }
            for (size_t i = filtered.count; i < inputs.size(); ++i) {
                if (GUI::get().editor_shows(inputs[i])) filtered.rows.push_back(static_cast<int>(i));
            }
            filtered = {&inputs, inputs.data(), inputs.size(), value.inputs_revision, GUI::get().show_editor_p1, GUI::get().show_editor_p2, std::move(filtered.rows)};
        }
        int rows = everyRow ? static_cast<int>(inputs.size()) : static_cast<int>(filtered.rows.size());
        auto inputAt = [&](int row) { return everyRow ? row : filtered.rows[static_cast<size_t>(row)]; };
        auto rowOf = [&](int i) {
            if (everyRow) return i;
            auto at = std::lower_bound(filtered.rows.begin(), filtered.rows.end(), i);
            return at != filtered.rows.end() && *at == i ? static_cast<int>(std::distance(filtered.rows.begin(), at)) : 0;
        };
        if (rows == 0) {
            GUI::get().select_input(-1);
            GUI::get().scroll_to_mark = false;
            ImGui::TextUnformatted(GUI::get().show_editor_p1 ? "No Player 1 Inputs" : "No Player 2 Inputs");
        } else {
            int frame = PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : (value.editor_testing ? value.step_frame : 0);
            auto numberAt = [&](int row) { return inputs[static_cast<size_t>(inputAt(row))].number; };
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
            if (Logic::get().inputs_sorted(value)) {
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
            bool follow = GUI::get().auto_scroll && PlayLayer::get();
            float rowH = ImGui::GetFrameHeightWithSpacing();
            if (GUI::get().scroll_to_mark) {
                int firstMark = 0;
                for (int row = 0; row < rows; ++row) {
                    if (GUI::get().in_cps_mark(inputs[inputAt(row)])) {
                        firstMark = row;
                        break;
                    }
                }
                ImGui::SetScrollY(std::max(0.f, firstMark * rowH - (ImGui::GetWindowHeight() - rowH) * 0.5f));
                GUI::get().scroll_to_mark = false;
            } else if (follow) {
                int chosen = closest;
                int picked = GUI::get().selected;
                bool pickedShown = GUI::get().selected_id != 0 && picked >= 0 && picked < static_cast<int>(inputs.size()) &&
                    Logic::get().input_id(static_cast<size_t>(picked)) == GUI::get().selected_id && GUI::get().editor_shows(inputs[picked]);
                if (pickedShown && std::abs(inputs[picked].number - frame) == best) chosen = picked;
                if (chosen != picked && !ImGui::IsAnyItemActive()) GUI::get().select_input(chosen);
                int row = rowOf(chosen);
                ImGui::SetScrollY(std::max(0.f, row * rowH - (ImGui::GetWindowHeight() - rowH) * 0.5f));
            }
            ImGuiListClipper clipper;
            clipper.Begin(rows, rowH);
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    int i = inputAt(row);
                    bool chosen = GUI::get().selected == i;
                    bool marked = chosen || GUI::get().input_in_range(i) || GUI::get().in_cps_mark(inputs[i]);
                    float const* tint = inputs[i].player2 ? value.editor_p2 : value.editor_p1;
                    if (marked) tint = inputs[i].player2 ? value.editor_sel_p2 : value.editor_sel_p1;
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tint[0], tint[1], tint[2], tint[3]));
                    int button = inputs[i].button == 0 ? static_cast<int>(PlayerButton::Jump) : inputs[i].button;
                    char const* action = inputs[i].holding ? "Click" : "Release";
                    if (button == static_cast<int>(PlayerButton::Left)) action = inputs[i].holding ? "Left" : "Release Left";
                    else if (button == static_cast<int>(PlayerButton::Right)) action = inputs[i].holding ? "Right" : "Release Right";
                    std::string label = fmt::format("{} at {}##in{}", action, inputs[i].number, i);
                    ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.f));
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                        GUI::get().select_input(i, GUI::get().shift_selecting());
                        GUI::get().auto_scroll = false;
                        GUI::get().clear_cps_mark();
                    }
                    if (GUI::get().input_in_range(i)) stroke_selected_button();
                    ImGui::PopStyleColor();
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##echo-edit", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    bool live = GUI::get().selected >= 0 && GUI::get().selected < static_cast<int>(inputs.size()) && !GUI::get().plain;
    GUI::get().draw_edit_fields(live, editInner);
    ImGui::Separator();
    int frame = (PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : (value.editor_testing ? value.step_frame : 0)) + 1;
    ImGui::Text("Current Frame: %i", frame);
    if (!live) ImGui::BeginDisabled();
    if (ImGui::Button("Set to Current Frame", ImVec2(editInner, 0.f))) {
        int delta = frame - GUI::get().draft.number;
        rewrite_selected([&](Frame& input) { input.number = std::max(input.number + delta, 0); });
    }
    if (!live) ImGui::EndDisabled();
    ImGui::Checkbox("Auto Scroll To Frame", &GUI::get().auto_scroll);
    if (ImGui::Checkbox("Edit Plain Text", &GUI::get().plain)) {
        GUI::get().plain_text = GUI::get().plain ? GUI::get().format_plain(value) : std::string();
    }
    ImGui::Separator();
    if (ImGui::Button("Add Input", ImVec2(pairW, 0.f))) GUI::get().add_input(frame);
    ImGui::SameLine();
    int deleteFirst = 0;
    int deleteLast = 0;
    bool canDelete = GUI::get().selection_span(deleteFirst, deleteLast);
    if (!canDelete) ImGui::BeginDisabled();
    if (ImGui::Button(GUI::get().has_range() ? "Delete Inputs" : "Delete Input", ImVec2(pairW, 0.f)) && canDelete) GUI::get().delete_selected();
    if (!canDelete) ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::Button("Actions", ImVec2(editInner, 0.f))) ImGui::OpenPopup("Editor Actions");
    if (ImGui::BeginPopupModal("Editor Actions", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        float gap = ImGui::GetStyle().ItemSpacing.x;
        float half = std::max(GUI::get().label_width("Random Offset"), GUI::get().label_width("Remove Player 1"));
        half = std::max(half, GUI::get().label_width("-1 Frame"));
        float row = half * 2.f + gap;
        row = std::max(row, GUI::get().label_width("Flip Presses and Releases"));
        row = std::max(row, GUI::get().label_width("Flip Player 1 and 2"));
        auto randomOffset = [] { GUI::get().random_offset_chosen(); };
        char const* scope = GUI::get().has_range() ? "the selected inputs" : "the whole macro";

        GUI::get().section_label("Shift");
        float framesLabel = ImGui::CalcTextSize("Frames").x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.f, row - framesLabel));
        ImGui::InputInt("Frames###offset_frames", &GUI::get().input_offset);
        if (ImGui::Button("Offset", ImVec2(half, 0.f))) offset_chosen(GUI::get().input_offset);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves %s by that many frames", scope);
        ImGui::SameLine();
        if (ImGui::Button("Random Offset", ImVec2(half, 0.f))) randomOffset();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves each input in %s by a random amount from -frames to +frames", scope);
        if (ImGui::Button("-1 Frame", ImVec2(half, 0.f))) offset_chosen(-1);
        ImGui::SameLine();
        if (ImGui::Button("+1 Frame", ImVec2(half, 0.f))) offset_chosen(1);

        GUI::get().section_label("Order");
        if (ImGui::Button("Sort", ImVec2(row, 0.f))) {
            Logic::get().sort_inputs();
            GUI::get().sync_draft();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sorts inputs by frame");

        GUI::get().section_label("Players");
        if (ImGui::Button("Remove Player 1", ImVec2(half, 0.f))) {
            Logic::get().remove_player_inputs(false);
            GUI::get().select_input(-1);
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove Player 2", ImVec2(half, 0.f))) {
            Logic::get().remove_player_inputs(true);
            GUI::get().select_input(-1);
        }
        if (ImGui::Button("Flip Player 1 and 2", ImVec2(row, 0.f))) {
            Logic::get().flip_input_players();
            GUI::get().sync_draft();
        }

        GUI::get().section_label("Holds");
        if (ImGui::Button("Flip Presses and Releases", ImVec2(row, 0.f))) {
            Logic::get().flip_input_holds();
            GUI::get().sync_draft();
        }

        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(row, 0.f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (GUI::get().checkbox_setting("Sort Automatically", value.sort_automatically) && value.sort_automatically) {
        Logic::get().sort_inputs();
        GUI::get().sync_draft();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("While recording and playing together, new clicks are sorted instead of appended.");
    ImGui::EndChild();
    editorPanelH = ImGui::GetItemRectSize().y;
}

void GUI::draw_sequence() {
    auto& value = Logic::get();
    static int selected = -1;
    static int amount = 1;

    GUI::get().section_label("Queue");
    if (GUI::get().checkbox_setting("Enable Sequence", value.sequence_enabled)) value.replay_index = 0;
    ImGui::SameLine();
    if (GUI::get().checkbox_setting("Random", value.sequence_random)) {
        value.replay_index = 0;
        value.sequence_order.clear();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Plays the queue in a random order");
    if (ImGui::Button("Remove All")) {
        value.sequence.clear();
        value.sequence_order.clear();
        value.replay_index = 0;
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

    GUI::get().section_label("Selected");
    if (selected >= 0 && selected < static_cast<int>(value.sequence.size())) {
        auto& clip = value.sequence[static_cast<size_t>(selected)];
        ImGui::Text("Name: %s", clip.name.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            value.sequence.erase(value.sequence.begin() + selected);
            selected = -1;
        }
    }

    GUI::get().section_label("Add");
    ImGui::SetNextItemWidth(GUI::get().px(120.f));
    ImGui::InputInt("Amount###macros", &amount);
    if (amount < 1) amount = 1;
    ImGui::SameLine();
    if (ImGui::Button("Add")) GUI::get().open_macro_picker(true, amount);
}

void GUI::draw_click_times(bool& softs, float& soft_ms, bool& hards, float& hard_seconds, bool& micros, int& micro_ms, char const* suffix) {
    GUI::get().checkbox_setting((std::string("Soft Clicks###soft_") + suffix).c_str(), softs);
    ImGui::SameLine();
    GUI::get().checkbox_setting((std::string("Hard Clicks###hard_") + suffix).c_str(), hards);
    ImGui::SameLine();
    GUI::get().checkbox_setting((std::string("Micro Clicks###micro_") + suffix).c_str(), micros);
    ImGui::Text("Micro Times:"); ImGui::SameLine();
    ImGui::Text("0 to %ims", micro_ms);
    ImGui::Text("Soft Times:"); ImGui::SameLine();
    ImGui::Text("%ims to %.3fms", micro_ms, soft_ms);
    ImGui::Text("Regular Times:"); ImGui::SameLine();
    ImGui::Text("%.3fms to %.3fs", soft_ms, hard_seconds);
    ImGui::Text("Hard Times:"); ImGui::SameLine();
    ImGui::Text("%.3fs and up", hard_seconds);
    ImGui::SetNextItemWidth(GUI::get().px(160.f));
    GUI::get().drag_float_setting((std::string("Soft ms###softms_") + suffix).c_str(), soft_ms, 1.f, 0.f, 5000.f, "%.0f");
    int micro = micro_ms;
    ImGui::SetNextItemWidth(GUI::get().px(160.f));
    if (ImGui::DragInt((std::string("Micro ms###micms_") + suffix).c_str(), &micro, 1.f, 0, 1000)) micro_ms = std::max(micro, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    ImGui::SetNextItemWidth(GUI::get().px(160.f));
    GUI::get().drag_float_setting((std::string("Hard seconds###hardms_") + suffix).c_str(), hard_seconds, 0.05f, 0.f, 30.f, "%.2f");
}
void GUI::draw_clickbot() {
    auto& value = Logic::get();
    GUI::get().section_label("Clickbot");
    GUI::get().checkbox_setting("Enabled###enable_clickbot", value.clickbot);
    ImGui::SameLine();
    static int player = 0;
    char const* players[] = {"Player 1", "Player 2"};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
    GUI::get().combo_items("##dropdown_clickbot", players, 2, player);
    ImGui::Separator();

    auto& pack = player == 0 ? value.clickpack : value.clickpack_p2;
    auto& volume = player == 0 ? value.clickbot_volume : value.clickbot_volume_p2;
    ImGui::SetNextItemWidth(GUI::get().px(100.f));
    GUI::get().drag_float_setting(player == 0 ? "Volume Multiplier###vol_p1" : "Volume Multiplier###vol_p2", volume, 0.25f, 0.f, 10.f, "%.2f");

    auto foldersRoot = Logic::get().save_root() / "clickpacks";
    std::vector<std::string> folders;
    std::error_code folderError;
    for (auto const& entry : std::filesystem::directory_iterator(foldersRoot, folderError)) {
        if (!entry.is_directory(folderError)) continue;
        try {
            folders.push_back(entry.path().filename().string());
        } catch (...) {}
    }
    std::string current;
    try {
        current = std::filesystem::path(pack).filename().string();
    } catch (...) {}
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
        GUI::get().open_clickpack_picker();
    }
    if (player == 0) {
        GUI::get().draw_click_times(value.p1_softs, value.p1_soft_ms, value.p1_hards, value.p1_hard_seconds, value.p1_micros, value.p1_micro_ms, "p1");
    } else {
        GUI::get().draw_click_times(value.p2_softs, value.p2_soft_ms, value.p2_hards, value.p2_hard_seconds, value.p2_micros, value.p2_micro_ms, "p2");
    }
}

void GUI::draw_render() {
    auto& value = Logic::get();
    GUI::get().section_label("Capture");
    if (auto blocker = GUI::get().render_blocker(); !blocker.empty()) {
        value.ffmpeg_missing = true;
        if (ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::TextWrapped("%s", blocker.c_str());
        if (ImGui::Button("Download FFmpeg")) {
            geode::utils::web::openLinkInBrowser("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Place ffmpeg.exe next to Geometry Dash once downloaded!");
        return;
    }
    value.ffmpeg_missing = false;
    bool recording = Logic::get().render_active();
    if (recording) {
        if (ImGui::Button("Stop")) Logic::get().stop_render();
    } else if (PlayLayer::get()) {
        if (ImGui::Button("Start Here")) Logic::get().start_render();
    } else if (value.render_queued) {
        ImGui::TextUnformatted("Waiting to enter a level...");
        if (ImGui::Button("Cancel")) value.render_queued = false;
    } else if (ImGui::Button("Start")) {
        Logic::get().start_render();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Begins rendering from your current position.");
    if (Logic::get().render_capturing()) {
        auto status = Logic::get().render_status();
        float fraction = status.known ? status.progress : -1.f * static_cast<float>(ImGui::GetTime());
        ImGui::ProgressBar(fraction, ImVec2(GUI::get().px(380.f), 0.f), status.text.c_str());
    }
    ImGui::SameLine();
    if (value.merge_busy) ImGui::BeginDisabled();
    if (ImGui::Button("Merge Renders")) GUI::get().open_render_picker();
    if (value.merge_busy) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Joins two videos back to back with a crossfade. Needs ffprobe.exe next to ffmpeg.exe.");

    ImGui::SetNextItemWidth(GUI::get().px(220.f));
    ImGui::InputTextWithHint("Video Name", value.macro_name.empty() ? "Replay" : value.macro_name.c_str(), &value.render_name);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave empty to name the video after the macro");
    
    ImGui::SetNextItemWidth(GUI::get().px(80.f));
    ImGui::InputText("Extension", &value.render_extension);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    GUI::get().checkbox_setting("Auto Exit on Finish", value.render_auto_exit);
    GUI::get().checkbox_setting("Record the Opening Pause", value.render_record_pause);
    ImGui::SetNextItemWidth(GUI::get().px(160.f));
    ImGui::InputText("Codec", &value.render_codec);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    ImGui::SetNextItemWidth(GUI::get().px(160.f));
    ImGui::InputText("FFmpeg Args", &value.render_args);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output options placed after the codec information. Leave blank if you don't know what this is");
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();

    if (ImGui::Button("Render Settings")) ImGui::OpenPopup("Render Settings"); ImGui::SameLine();
    GUI::get().checkbox_setting("Immediate Restart", value.immediate_restart);
    GUI::get().offer_rebind("key-reset", "Restarts the level once as soon as you enter it");
    if (!ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    GUI::get().section_label("Picture");
    int width = value.render_width;
    int height = value.render_height;
    int fps = value.render_fps;
    float mbps = static_cast<float>(value.render_bitrate) / 1000000.f;
    ImGui::SetNextItemWidth(GUI::get().px(120.f));
    if (ImGui::DragInt("Width", &width, 1.f, 2, 7680)) value.render_width = std::clamp(width, 2, 7680);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(GUI::get().px(120.f));
    if (ImGui::DragInt("Height", &height, 1.f, 2, 4320)) value.render_height = std::clamp(height, 2, 4320);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    ImGui::SetNextItemWidth(GUI::get().px(120.f));
    if (ImGui::DragInt("FPS", &fps, 1.f, 1, 1000)) value.render_fps = std::max(fps, 1);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(GUI::get().px(120.f));
    char const* bitrateFormat = mbps <= 0.f ? "Lossless" : "%.1f Mbps";
    if (ImGui::DragFloat("Bitrate", &mbps, 0.1f, 0.f, 200.f, bitrateFormat)) value.render_bitrate = mbps <= 0.f ? 0 : std::max(static_cast<int>(mbps * 1000000.f), 1000);
    if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();

    GUI::get().section_label("Audio");
    ImGui::SetNextItemWidth(GUI::get().px(140.f));
    GUI::get().drag_float_setting("Fade In", value.render_fade_in, 0.05f, 0.f, 60.f, "%.2f s");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(GUI::get().px(140.f));
    GUI::get().drag_float_setting("Fade Out", value.render_fade_out, 0.05f, 0.f, 60.f, "%.2f s");
    ImGui::SetNextItemWidth(GUI::get().px(140.f));
    GUI::get().drag_float_setting("After End", value.render_after_end, 0.05f, 0.f, 120.f, "%.2f s");
    ImGui::SetNextItemWidth(GUI::get().px(140.f));
    GUI::get().drag_float_setting("Music Volume", value.render_music, 0.01f, 0.f, 1.f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(GUI::get().px(140.f));
    GUI::get().drag_float_setting("SFX Volume", value.render_sfx, 0.01f, 0.f, 1.f, "%.2f");
    GUI::get().checkbox_setting("Use Speedhack", value.render_speedhack);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Renders at your speedhack value");
    GUI::get().checkbox_setting("SSB Fix", value.render_ssb_fix);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixes audio sync because of the game's scroll speed bug when rendering.");
    ImGui::SameLine();
    GUI::get().checkbox_setting("Color Fix", value.render_color_fix);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fixes video colors to be more accurate to the game.");
    GUI::get().checkbox_setting("Hide Labels", value.render_hide_labels);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hides Echo's overlay labels while a render is capturing.");
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
