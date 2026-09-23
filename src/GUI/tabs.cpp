#include "GUI/gui.hpp"
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
    // TODO
    return {};
}
}

void GUI::draw_tools() {
    // TODO
}

void GUI::draw_visuals() {
    // TODO
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
    // todo
    return {};
}

void GUI::apply_plain(std::string const& text) {
    // TODO
}

void GUI::draw_edit_fields(bool enabled, float column) {
    // todo
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
    // TODO
}

bool GUI::shift_selecting() const {
    // TODO
    return {};
}

void GUI::select_input(int index, bool range) {
    // TODO
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
    // later
}

void GUI::draw_sequence() {
    // TODO
}

void GUI::draw_click_times(bool& softs, float& soft_ms, bool& hards, float& hard_seconds, bool& micros, int& micro_ms, char const* suffix) {
    // TODO
}
void GUI::draw_clickbot() {
    // later
}

void GUI::draw_render() {
    // TODO
}
