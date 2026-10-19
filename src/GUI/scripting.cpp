#include "GUI/gui.hpp"
#include "GUI/theme_script.hpp"
#include "Logic/macro_script.hpp"
#include "Script/host.hpp"
#include "Script/host_helpers.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#define LIVE_RUN_DELAY 0.4
#define FILE_CHECK_SECONDS 0.5
#define EDITOR_LINES 16.f
#define LIST_LINES 9.f
#define CONSOLE_LINES 5.f
#define MAX_LINE_DIGITS 9

struct ScriptEditor {
    int host = 0;
    fs::path file;
    std::string text, saved;
    fs::file_time_type written;
    double checked_at = 0.0;
    double edited_at = 0.0;
    bool waiting_live = false;
    bool live = false;
    bool ask_unsaved = false;
    int generation = 0;
    size_t output_seen = 0;
    std::function<void()> after_saving;
    fs::path opened_current;
    EchoUI::CodeEditorState view;
    bool open_big = false;
    bool big_shown = false;
    fs::path deleting;
    bool ask_delete = false;
};

static ScriptEditor g_editor;
static Script::Host* const SCRIPT_HOSTS[] = { &ThemeScript::host(), &MacroScript::host() };

static Script::Host& current_host() {
    g_editor.host = std::clamp(g_editor.host, 0, (int)std::size(SCRIPT_HOSTS) - 1);
    return *SCRIPT_HOSTS[g_editor.host];
}

static void tip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
}

static bool dirty() {
    return !g_editor.file.empty() && g_editor.text != g_editor.saved;
}

static void close_file() {
    g_editor.file.clear();
    g_editor.text.clear();
    g_editor.saved.clear();
    g_editor.waiting_live = false;
    g_editor.view = {};
    ++g_editor.generation;
}

static bool open_file(const fs::path& file) {
    std::string text;
    if (!Script::read_text(file, text)) return false;
    std::erase(text, '\r');
    std::error_code error;
    g_editor.file = file;
    g_editor.text = text;
    g_editor.saved = text;
    g_editor.written = fs::last_write_time(file, error);
    g_editor.checked_at = ImGui::GetTime();
    g_editor.waiting_live = false;
    g_editor.view = {};
    ++g_editor.generation;
    return true;
}

static bool save_file() {
    std::ofstream stream(g_editor.file, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << g_editor.text;
    stream.close();
    if (!stream) return false;
    std::error_code error;
    g_editor.saved = g_editor.text;
    g_editor.written = fs::last_write_time(g_editor.file, error);
    g_editor.waiting_live = false;
    if (current_host().reruns()) current_host().run_file(g_editor.file);
    return true;
}

static void after_unsaved_check(std::function<void()> action) {
    if (!dirty()) {
        action();
        return;
    }
    g_editor.after_saving = std::move(action);
    g_editor.ask_unsaved = true;
}

static int error_line(const std::string& error, const fs::path& file) {
    std::string marker = geode::utils::string::pathToString(file.filename()) + ":";
    size_t at = error.find(marker);
    if (at == std::string::npos) return 0;
    size_t digits = at + marker.size(), end = digits;
    while (end < error.size() && std::isdigit((unsigned char)error[end])) ++end;
    if (end == digits || end - digits > MAX_LINE_DIGITS || end >= error.size() || error[end] != ':') return 0;
    return std::stoi(error.substr(digits, end - digits));
}

static void watch_open_file() {
    double now = ImGui::GetTime();
    if (g_editor.file.empty() || now - g_editor.checked_at < FILE_CHECK_SECONDS) return;
    g_editor.checked_at = now;
    std::error_code error;
    auto written = fs::last_write_time(g_editor.file, error);
    if (error || written == g_editor.written) return;
    g_editor.written = written;
    if (!dirty()) open_file(g_editor.file);
}

static void draw_unsaved_modal() {
    if (g_editor.ask_unsaved) ImGui::OpenPopup("Unsaved Changes");
    g_editor.ask_unsaved = false;
    if (!EchoUI::BeginModal("Unsaved Changes", 20.f)) return;
    ImGui::PushTextWrapPos(0.f);
    ImGui::Text("%s has changes that are not saved.", current_host().label(g_editor.file).c_str());
    ImGui::PopTextWrapPos();
    float third = EchoUI::RowButton(3);
    int picked = 0;
    if (EchoUI::Button("Save", ImVec2(third, 0.f))) picked = 1;
    ImGui::SameLine();
    if (EchoUI::Button("Discard", ImVec2(third, 0.f))) picked = 2;
    ImGui::SameLine();
    if (EchoUI::Button("Cancel", ImVec2(third, 0.f))) picked = 3;
    if (picked) {
        auto action = std::move(g_editor.after_saving);
        g_editor.after_saving = nullptr;
        ImGui::CloseCurrentPopup();
        if (picked == 1 && !save_file()) Logic::get().notify("Could not save the script", false);
        else if (picked != 3 && action) action();
    }
    EchoUI::EndModal();
}

static void draw_delete_modal() {
    if (g_editor.ask_delete) ImGui::OpenPopup("Delete Script");
    g_editor.ask_delete = false;
    if (!EchoUI::BeginModal("Delete Script", 20.f)) return;
    Script::Host& host = current_host();
    std::string name = host.label(g_editor.deleting);
    ImGui::PushTextWrapPos(0.f);
    if (g_editor.deleting.filename() == "theme.lua") ImGui::Text("Delete %s and everything in its folder?", name.c_str());
    else ImGui::Text("Delete %s?", name.c_str());
    ImGui::PopTextWrapPos();
    float half = EchoUI::RowButton(2);
    bool remove = EchoUI::Button("Delete", ImVec2(half, 0.f));
    ImGui::SameLine();
    bool cancel = EchoUI::Button("Cancel", ImVec2(half, 0.f));
    if (remove) {
        if (g_editor.deleting == g_editor.file) close_file();
        if (!host.remove_file(g_editor.deleting)) Logic::get().notify("Could not delete the script", false);
    }
    if (remove || cancel) {
        g_editor.deleting.clear();
        ImGui::CloseCurrentPopup();
    }
    EchoUI::EndModal();
}

static void draw_status_line(Script::Host& host) {
    const ImGuiStyle& style = ImGui::GetStyle();
    std::string place = fmt::format("Ln {}, Col {}", g_editor.view.line, g_editor.view.column);
    float place_width = ImGui::CalcTextSize(place.c_str()).x;
    float left_width = std::max(0.f, ImGui::GetContentRegionAvail().x - place_width - style.ItemSpacing.x);
    bool failed = !host.error().empty();
    std::string shown = failed ? host.error() : host.label(g_editor.file) + (dirty() ? " *" : "");
    ImVec2 at = ImGui::GetCursorScreenPos();
    ImVec2 end(at.x + left_width, at.y + ImGui::GetTextLineHeight());
    ImGui::Dummy(ImVec2(left_width, ImGui::GetTextLineHeight()));
    if (failed) ImGui::PushStyleColor(ImGuiCol_Text, EchoUI::ERROR_RED);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), at, end, end.x, shown.c_str(), nullptr, nullptr);
    if (failed) ImGui::PopStyleColor();
    if (failed && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", g_editor.view.error_line > 0 ? (host.error() + "\n\nClick to jump to the line").c_str() : host.error().c_str());
        if (ImGui::IsItemClicked() && g_editor.view.error_line > 0) g_editor.view.jump_to_line = g_editor.view.error_line;
    }
    ImGui::SameLine(0.f, style.ItemSpacing.x);
    ImGui::TextDisabled("%s", place.c_str());
}

static void draw_console(Script::Host& host, float height) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Console");
    float clear_width = ImGui::CalcTextSize("Clear").x + style.FramePadding.x * 2.f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - clear_width));
    if (EchoUI::Button("Clear", ImVec2(clear_width, 0.f))) host.clear_output();
    const auto& output = host.output();
    ImGui::BeginChild("##script-console", ImVec2(0.f, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    if (output.empty()) ImGui::TextDisabled("print() and errors show up here");
    for (const auto& line : output)
        ImGui::TextUnformatted(line.c_str());
    if (output.size() != g_editor.output_seen) {
        g_editor.output_seen = output.size();
        ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
}

static bool file_running(const std::vector<fs::path>& running, const fs::path& file) {
    return std::find(running.begin(), running.end(), file) != running.end();
}

static float console_height() {
    return ImGui::GetTextLineHeightWithSpacing() * CONSOLE_LINES;
}

static float editor_height_filling(float least_lines, float extra_below) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float line = ImGui::GetTextLineHeight();
    float below = style.ItemSpacing.y * 3.f + line + ImGui::GetFrameHeight() + console_height() + extra_below;
    return std::max(line * least_lines, ImGui::GetContentRegionAvail().y - below);
}

static void draw_editor_buttons(Script::Host& host, bool big) {
    bool reruns = host.reruns();
    bool running = file_running(host.running_files(), g_editor.file);
    int count = 4 + (reruns ? 1 : 0) + (big ? 0 : 1);
    float share = EchoUI::RowButton(count);
    ImGui::BeginDisabled(!dirty());
    if (EchoUI::Button("Save", ImVec2(share, 0.f)) && !save_file()) Logic::get().notify("Could not save the script", false);
    tip(reruns ? "Saves and runs the script (Ctrl+S works too)" : "Saves the script (Ctrl+S works too)");
    ImGui::SameLine();
    if (EchoUI::Button("Revert", ImVec2(share, 0.f))) {
        open_file(g_editor.file);
        if (running && reruns) host.run_file(g_editor.file);
    }
    tip("Undoes every change since the last save");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (running) {
        if (EchoUI::Button("Stop", ImVec2(share, 0.f))) host.stop_file(g_editor.file);
        tip("Stops this script");
    }
    else {
        if (EchoUI::Button("Run", ImVec2(share, 0.f))) host.run_text(g_editor.text, g_editor.file);
        tip(reruns ? "Runs the code in the editor without saving it" : "Runs the code in the editor without saving it. Other scripts keep running");
    }
    ImGui::SameLine();
    bool wrap = Logic::get().script_wrap;
    if (EchoUI::Checkbox("Wrap", &wrap)) {
        Logic::get().script_wrap = wrap;
        Logic::get().save_settings_file();
    }
    tip("Wraps long lines so you don't have to scroll sideways");
    if (reruns) {
        ImGui::SameLine();
        if (EchoUI::Checkbox("Live", &g_editor.live) && g_editor.live) {
            g_editor.waiting_live = true;
            g_editor.edited_at = 0.0;
        }
        tip("Reruns the script as you type, without saving it");
    }
    if (!big) {
        ImGui::SameLine();
        if (EchoUI::Button("Expand", ImVec2(share, 0.f))) g_editor.open_big = true;
        tip("Opens the editor in a bigger window you can resize");
    }
}

static void draw_editor_body(Script::Host& host, float editor_height) {
    bool reruns = host.reruns();
    g_editor.view.wrap = Logic::get().script_wrap;
    g_editor.view.error_line = error_line(host.error(), g_editor.file);
    ImGui::PushID(g_editor.generation);
    bool changed = EchoUI::CodeEditor("##script-code", &g_editor.text, ImVec2(-1.f, editor_height), host.names(), g_editor.view);
    bool typing = ImGui::IsItemActive();
    ImGui::PopID();
    if (changed) {
        g_editor.edited_at = ImGui::GetTime();
        g_editor.waiting_live = true;
    }
    if (typing && dirty() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && !save_file())
        Logic::get().notify("Could not save the script", false);
    if (reruns && g_editor.live && g_editor.waiting_live && ImGui::GetTime() - g_editor.edited_at >= LIVE_RUN_DELAY) {
        g_editor.waiting_live = false;
        host.run_text(g_editor.text, g_editor.file);
    }
    draw_status_line(host);
    draw_console(host, console_height());
}

static void draw_editor_card(Script::Host& host, float width_em, float height) {
    const float least_lines = 5.f;
    if (!EchoUI::BeginCard("Editor", width_em, height)) {
        EchoUI::EndCard();
        return;
    }
    if (g_editor.file.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Pick a script from the list or press New to make one.");
        ImGui::PopTextWrapPos();
        EchoUI::EndCard();
        return;
    }
    draw_editor_buttons(host, false);
    float editor_height = ImGui::GetTextLineHeight() * EDITOR_LINES + ImGui::GetStyle().FramePadding.y * 2.f;
    if (height > 0.f) editor_height = editor_height_filling(least_lines, 0.f);
    if (g_editor.big_shown) {
        ImGui::BeginChild("##script-code-away", ImVec2(-1.f, editor_height), ImGuiChildFlags_Borders);
        ImGui::TextDisabled("The script is open in the big editor.");
        ImGui::EndChild();
        draw_status_line(host);
        draw_console(host, console_height());
    }
    else {
        draw_editor_body(host, editor_height);
    }
    EchoUI::EndCard();
}

static void draw_big_editor(Script::Host& host) {
    const float least_lines = 6.f;
    if (g_editor.open_big) ImGui::OpenPopup("Script Editor");
    g_editor.open_big = false;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    g_editor.big_shown = false;
    if (!EchoUI::BeginBigModal("Script Editor", ImVec2(display.x * .82f, display.y * .86f), ImVec2(EchoUI::em(28.f), EchoUI::em(16.f)))) return;
    g_editor.big_shown = true;
    if (g_editor.file.empty()) {
        ImGui::TextDisabled("Pick a script from the list first.");
        EchoUI::EndModal();
        return;
    }
    draw_editor_buttons(host, true);
    draw_editor_body(host, editor_height_filling(least_lines, ImGui::GetStyle().WindowPadding.y));
    EchoUI::EndModal();
}

static void draw_scripts_card(Script::Host& host, float width_em, float height) {
    if (!EchoUI::BeginCard("Scripts", width_em, height)) {
        EchoUI::EndCard();
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    std::vector<const char*> types;
    for (auto* each : SCRIPT_HOSTS)
        types.push_back(each->name());
    EchoUI::Row("Type", "Themes change how the menu looks, macro scripts play along with you and can edit your macro");
    int picked_type = g_editor.host;
    if (EchoUI::Combo("##script-type", &picked_type, types.data(), (int)types.size()) && picked_type != g_editor.host) {
        after_unsaved_check([picked_type] {
            g_editor.host = picked_type;
            close_file();
        });
    }

    auto running_files = host.running_files();
    bool running = !running_files.empty();
    ImVec4 dot = style.Colors[ImGuiCol_TextDisabled];
    if (running) dot = EchoUI::RUNNING_GREEN;
    else if (!host.error().empty()) dot = EchoUI::ERROR_RED;
    EchoUI::StatusDot(dot);
    ImGui::SameLine();
    if (running && running_files.size() == 1) ImGui::Text("Running %s", host.label(running_files.front()).c_str());
    else if (running) ImGui::Text("%d scripts running", (int)running_files.size());
    else if (!host.error().empty()) ImGui::TextUnformatted("Stopped by an error");
    else ImGui::TextUnformatted("Nothing running");
    if (running && ImGui::IsItemHovered()) {
        std::string names;
        for (const auto& file : running_files)
            names += (names.empty() ? "" : "\n") + host.label(file);
        ImGui::SetTooltip("%s", names.c_str());
    }

    bool can_autorun = host.can_autorun();
    bool macros = &host == &MacroScript::host();
    float under = style.ItemSpacing.y + ImGui::GetFrameHeight();
    if (can_autorun && !g_editor.file.empty()) under += style.ItemSpacing.y + ImGui::GetFrameHeight();
    if (macros) under += style.ItemSpacing.y + ImGui::GetFrameHeight();
    float list_height = ImGui::GetTextLineHeightWithSpacing() * LIST_LINES;
    if (height > 0.f) list_height = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y - under);
    ImGui::BeginChild("##script-files", ImVec2(0.f, list_height), ImGuiChildFlags_Borders);
    auto files = host.files();
    for (const auto& file : files) {
        std::string label = host.label(file);
        bool runs = file_running(running_files, file);
        bool autoruns = can_autorun && host.autoruns(file);
        if (runs) label += " (running)";
        if (autoruns) label += " (autorun)";
        ImGui::PushID(geode::utils::string::pathToString(file).c_str());
        if (runs || autoruns) {
            ImGui::AlignTextToFramePadding();
            EchoUI::StatusDot(runs ? EchoUI::RUNNING_GREEN : style.Colors[ImGuiCol_CheckMark]);
            ImGui::SameLine(0.f, style.ItemInnerSpacing.x);
        }
        float stop_w = runs ? ImGui::CalcTextSize("Stop").x + style.FramePadding.x * 2.f : 0.f;
        float row_w = runs ? std::max(EchoUI::em(3.f), ImGui::GetContentRegionAvail().x - stop_w - style.ItemSpacing.x) : 0.f;
        if (EchoUI::Selectable(label.c_str(), file == g_editor.file, 0, ImVec2(row_w, 0.f)) && file != g_editor.file)
            after_unsaved_check([file] { open_file(file); });
        if (ImGui::BeginPopupContextItem("##script-menu")) {
            if (EchoUI::Selectable("Delete", false)) {
                g_editor.deleting = file;
                g_editor.ask_delete = true;
            }
            ImGui::EndPopup();
        }
        if (runs) {
            ImGui::SameLine();
            if (EchoUI::Button("Stop", ImVec2(stop_w, 0.f))) host.stop_file(file);
            tip("Stops this script");
        }
        ImGui::PopID();
    }
    auto examples = host.examples();
    for (const auto& example : examples) {
        std::string label = host.label(example) + " (example)";
        ImGui::PushID(geode::utils::string::pathToString(example).c_str());
        if (EchoUI::Selectable(label.c_str(), false)) {
            after_unsaved_check([&host, example] {
                fs::path made;
                if (host.install_example(example, made)) open_file(made);
            });
        }
        ImGui::PopID();
    }
    if (files.empty() && examples.empty()) ImGui::TextDisabled("No scripts yet");
    ImGui::EndChild();

    if (can_autorun && !g_editor.file.empty()) {
        bool autorun = host.autoruns(g_editor.file);
        if (EchoUI::Checkbox("Autorun", &autorun)) host.set_autorun(g_editor.file, autorun);
        tip("Starts this script when the game opens and keeps it running until you stop it");
    }
    if (macros) {
        if (EchoUI::Checkbox("Full Game Access", &Logic::get().script_full_access)) Logic::get().save_settings_file();
        tip("Lets macro scripts read and change the PlayLayer, players and level through gd. Changing or calling anything turns on safe mode");
    }
    float third = EchoUI::RowButton(3);
    if (EchoUI::Button("New", ImVec2(third, 0.f))) {
        after_unsaved_check([&host] {
            fs::path made;
            if (!host.make_new(made) || !open_file(made)) return;
            if (host.reruns()) host.run_file(made);
        });
    }
    tip("Makes a new script and opens it");
    ImGui::SameLine();
    if (EchoUI::Button("Folder", ImVec2(third, 0.f))) {
        std::error_code error;
        fs::create_directories(host.folder(), error);
        GUI::get().open_folder(host.folder());
    }
    tip("Opens the scripts folder");
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (EchoUI::Button(can_autorun ? "Stop All" : "Stop", ImVec2(third, 0.f))) host.stop();
    ImGui::EndDisabled();
    tip(can_autorun ? "Stops every running script and removes their labels" : "Stops the running script");
    EchoUI::EndCard();
}

void GUI::edit_script(const fs::path& file) {
    GUI::get().page = "Scripting";
    if (file == g_editor.file) return;
    after_unsaved_check([file] { open_file(file); });
}

void GUI::draw_scripting() {
    const float side_share = .36f, least_side_em = 13.f, most_side_em = 18.f;
    Script::Host& host = current_host();
    if (g_editor.file.empty() && !host.current().empty() && host.current() != g_editor.opened_current) {
        g_editor.opened_current = host.current();
        open_file(host.current());
    }
    watch_open_file();
    ImVec2 room = GUI::get().page_room;
    if (room.x > 0.f && room.y > 0.f) {
        float unit = EchoUI::em(1.f);
        float side = std::clamp(room.x * side_share, unit * least_side_em, unit * most_side_em);
        float editor = std::max(unit, room.x - side - ImGui::GetStyle().ItemSpacing.x);
        draw_editor_card(host, editor / unit, room.y);
        ImGui::SameLine();
        draw_scripts_card(host, side / unit, room.y);
    }
    else {
        draw_editor_card(host, GUI::get().card_width() * 2.f, 0.f);
        draw_scripts_card(host, GUI::get().card_width(), 0.f);
    }
    draw_big_editor(host);
    draw_unsaved_modal();
    draw_delete_modal();
}
