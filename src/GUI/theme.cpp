#include <imgui_internal.h>
#include "GUI/gui.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
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
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <optional>
#include <cctype>

using namespace geode::prelude;

void GUI::help_marker(char const* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

matjson::Value GUI::color_json(ImVec4 const& color) {
    matjson::Value value = matjson::Value::array();
    value.push(color.x);
    value.push(color.y);
    value.push(color.z);
    value.push(color.w);
    return value;
}

matjson::Value GUI::pair_json(ImVec2 const& value) {
    matjson::Value json = matjson::Value::array();
    json.push(value.x);
    json.push(value.y);
    return json;
}

void GUI::read_pair(matjson::Value const& json, char const* key, ImVec2& value) {
    if (!json[key].isArray() || json[key].size() < 2) return;
    if (auto x = json[key][0].asDouble()) value.x = static_cast<float>(x.unwrap());
    if (auto y = json[key][1].asDouble()) value.y = static_cast<float>(y.unwrap());
}

void GUI::read_color(matjson::Value const& json, char const* key, float* color) {
    if (!json[key].isArray() || json[key].size() < 4) return;
    for (int i = 0; i < 4; ++i) {
        if (auto channel = json[key][i].asDouble()) color[i] = static_cast<float>(channel.unwrap());
    }
}

float GUI::unscaled(float value) {
    float scale = GUI::get().applied_scale;
    return scale > 0.f ? value / scale : value;
}

ImVec2 GUI::unscaled(ImVec2 value) {
    return ImVec2(GUI::get().unscaled(value.x), GUI::get().unscaled(value.y));
}

matjson::Value GUI::style_json() {
    auto& style = ImGui::GetStyle();
    auto& value = Logic::get();
    matjson::Value json;
    json["name"] = value.theme_name;
    json["scale"] = value.menu_scale;
    json["metrics_absolute"] = true;
    json["WindowPadding"] = GUI::get().pair_json(style.WindowPadding);
    json["FramePadding"] = GUI::get().pair_json(style.FramePadding);
    json["CellPadding"] = GUI::get().pair_json(style.CellPadding);
    json["ItemSpacing"] = GUI::get().pair_json(style.ItemSpacing);
    json["ItemInnerSpacing"] = GUI::get().pair_json(style.ItemInnerSpacing);
    json["TouchExtraPadding"] = GUI::get().pair_json(style.TouchExtraPadding);
    json["IndentSpacing"] = style.IndentSpacing;
    json["ScrollbarSize"] = style.ScrollbarSize;
    json["GrabMinSize"] = style.GrabMinSize;
    json["WindowBorderSize"] = style.WindowBorderSize;
    json["ChildBorderSize"] = style.ChildBorderSize;
    json["PopupBorderSize"] = style.PopupBorderSize;
    json["FrameBorderSize"] = style.FrameBorderSize;
    json["TabBorderSize"] = style.TabBorderSize;
    json["WindowRounding"] = style.WindowRounding;
    json["ChildRounding"] = style.ChildRounding;
    json["FrameRounding"] = style.FrameRounding;
    json["PopupRounding"] = style.PopupRounding;
    json["ScrollbarRounding"] = style.ScrollbarRounding;
    json["GrabRounding"] = style.GrabRounding;
    json["TabRounding"] = style.TabRounding;
    json["WindowTitleAlign"] = GUI::get().pair_json(style.WindowTitleAlign);
    json["ButtonTextAlign"] = GUI::get().pair_json(style.ButtonTextAlign);
    json["SelectableTextAlign"] = GUI::get().pair_json(style.SelectableTextAlign);
    json["DisplaySafeAreaPadding"] = GUI::get().pair_json(style.DisplaySafeAreaPadding);
    json["AntiAliasedLines"] = true;
    json["AntiAliasedLinesUseTex"] = true;
    json["AntiAliasedFill"] = true;
    json["CurveTessellationTol"] = style.CurveTessellationTol;
    json["CircleTessellationMaxError"] = style.CircleTessellationMaxError;
    json["SharpEdges"] = style.CircleTessellationMaxError >= 2.5f;
    json["colors"] = matjson::Value::array();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) json["colors"].push(GUI::get().color_json(style.Colors[i]));
    json["editor_p1"] = matjson::Value::array();
    json["editor_p2"] = matjson::Value::array();
    json["editor_sel_p1"] = matjson::Value::array();
    json["editor_sel_p2"] = matjson::Value::array();
    for (int i = 0; i < 4; ++i) {
        json["editor_p1"].push(value.editor_p1[i]);
        json["editor_p2"].push(value.editor_p2[i]);
        json["editor_sel_p1"].push(value.editor_sel_p1[i]);
        json["editor_sel_p2"].push(value.editor_sel_p2[i]);
    }
    return json;
}

void GUI::apply_style_json(matjson::Value const& json) {
    auto& style = ImGui::GetStyle();
    auto& value = Logic::get();
    if (auto text = json["name"].asString()) value.theme_name = text.unwrap();
    if (auto number = json["scale"].asDouble()) value.menu_scale = std::clamp(static_cast<float>(number.unwrap()), 0.35f, 3.f);
    GUI::get().read_pair(json, "WindowPadding", style.WindowPadding);
    GUI::get().read_pair(json, "FramePadding", style.FramePadding);
    GUI::get().read_pair(json, "CellPadding", style.CellPadding);
    GUI::get().read_pair(json, "ItemSpacing", style.ItemSpacing);
    GUI::get().read_pair(json, "ItemInnerSpacing", style.ItemInnerSpacing);
    GUI::get().read_pair(json, "TouchExtraPadding", style.TouchExtraPadding);
    GUI::get().read_pair(json, "WindowTitleAlign", style.WindowTitleAlign);
    GUI::get().read_pair(json, "ButtonTextAlign", style.ButtonTextAlign);
    GUI::get().read_pair(json, "SelectableTextAlign", style.SelectableTextAlign);
    GUI::get().read_pair(json, "DisplaySafeAreaPadding", style.DisplaySafeAreaPadding);
    auto number = [&](char const* key, float& field) {
        if (auto parsed = json[key].asDouble()) field = static_cast<float>(parsed.unwrap());
    };
    number("IndentSpacing", style.IndentSpacing);
    number("ScrollbarSize", style.ScrollbarSize);
    number("GrabMinSize", style.GrabMinSize);
    number("WindowBorderSize", style.WindowBorderSize);
    number("ChildBorderSize", style.ChildBorderSize);
    number("PopupBorderSize", style.PopupBorderSize);
    number("FrameBorderSize", style.FrameBorderSize);
    number("TabBorderSize", style.TabBorderSize);
    number("WindowRounding", style.WindowRounding);
    number("ChildRounding", style.ChildRounding);
    number("FrameRounding", style.FrameRounding);
    number("PopupRounding", style.PopupRounding);
    number("ScrollbarRounding", style.ScrollbarRounding);
    number("GrabRounding", style.GrabRounding);
    number("TabRounding", style.TabRounding);
    number("CurveTessellationTol", style.CurveTessellationTol);
    number("CircleTessellationMaxError", style.CircleTessellationMaxError);
    if (auto flag = json["SharpEdges"].asBool()) {
        style.CircleTessellationMaxError = flag.unwrap() ? 5.f : 0.1f;
    } else if (style.CircleTessellationMaxError >= 2.5f) {
        style.CircleTessellationMaxError = 5.f;
    } else if (style.CircleTessellationMaxError > 0.f) {
        style.CircleTessellationMaxError = 0.1f;
    }
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    if (json["colors"].isArray()) {
        int count = std::min(static_cast<int>(json["colors"].size()), static_cast<int>(ImGuiCol_COUNT));
        for (int i = 0; i < count; ++i) {
            if (!json["colors"][i].isArray() || json["colors"][i].size() < 4) continue;
            ImVec4 color = style.Colors[i];
            if (auto channel = json["colors"][i][0].asDouble()) color.x = static_cast<float>(channel.unwrap());
            if (auto channel = json["colors"][i][1].asDouble()) color.y = static_cast<float>(channel.unwrap());
            if (auto channel = json["colors"][i][2].asDouble()) color.z = static_cast<float>(channel.unwrap());
            if (auto channel = json["colors"][i][3].asDouble()) color.w = static_cast<float>(channel.unwrap());
            style.Colors[i] = color;
        }
    }
    GUI::get().read_color(json, "editor_p1", value.editor_p1);
    GUI::get().read_color(json, "editor_p2", value.editor_p2);
    GUI::get().read_color(json, "editor_sel_p1", value.editor_sel_p1);
    GUI::get().read_color(json, "editor_sel_p2", value.editor_sel_p2);
    if (style.CurveTessellationTol < 0.10f) style.CurveTessellationTol = 0.10f;
    bool absolute = false;
    if (auto flag = json["metrics_absolute"].asBool()) absolute = flag.unwrap();
    if (!absolute && value.menu_scale != 1.f) {
        float bake = value.menu_scale;
        auto grow = [&](ImVec2& pair) { pair.x *= bake; pair.y *= bake; };
        grow(style.WindowPadding);
        grow(style.FramePadding);
        grow(style.CellPadding);
        grow(style.ItemSpacing);
        grow(style.ItemInnerSpacing);
        grow(style.TouchExtraPadding);
        grow(style.DisplaySafeAreaPadding);
        style.IndentSpacing *= bake;
        style.ScrollbarSize *= bake;
        style.GrabMinSize *= bake;
        style.WindowRounding *= bake;
        style.ChildRounding *= bake;
        style.FrameRounding *= bake;
        style.PopupRounding *= bake;
        style.ScrollbarRounding *= bake;
        style.GrabRounding *= bake;
        style.TabRounding *= bake;
    }
    GUI::get().on_style_replaced();
}

std::filesystem::path GUI::theme_path(std::string const& name) {
    auto safe = name.empty() ? std::string("Default") : name;
    for (char& glyph : safe) {
        if (glyph == '/' || glyph == '\\' || glyph == ':') glyph = '_';
    }
    return Logic::get().save_root() / "themes" / (safe + ".ui.json");
}

void GUI::write_theme(std::filesystem::path const& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file(path);
    if (!file) return;
    file << GUI::get().style_json().dump(2);
}

void GUI::note_style_edit() {
    if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().write_theme(GUI::get().theme_path("current"));
}

bool edit_style_vec(char const* id, ImVec2& live, float min, float max) {
    struct Draft {
        ImVec2 value;
        bool drag = false;
    };
    static std::unordered_map<std::string, Draft> drafts;
    auto& draft = drafts[id];
    if (!draft.drag) draft.value = live;
    ImGui::SliderFloat2(id, &draft.value.x, min, max, "%.0f");
    if (ImGui::IsItemActivated()) draft.drag = true;
    if (!ImGui::IsItemDeactivated()) return false;
    draft.drag = false;
    live = draft.value;
    GUI::get().note_style_edit();
    return true;
}

bool edit_style_float(char const* id, float& live, float min, float max) {
    struct Draft {
        float value = 0.f;
        bool drag = false;
    };
    static std::unordered_map<std::string, Draft> drafts;
    auto& draft = drafts[id];
    if (!draft.drag) draft.value = live;
    ImGui::SliderFloat(id, &draft.value, min, max, "%.0f");
    if (ImGui::IsItemActivated()) draft.drag = true;
    if (!ImGui::IsItemDeactivated()) return false;
    draft.drag = false;
    live = draft.value;
    GUI::get().note_style_edit();
    return true;
}

void GUI::paint_editor(float* dest, ImVec4 color) {
    dest[0] = color.x;
    dest[1] = color.y;
    dest[2] = color.z;
    dest[3] = color.w;
}

void GUI::randomize_theme() {
    // TODO
}

void GUI::color_row(char const* label, float* color, int id) {
    ImGui::PushID(id);
    if (ImGui::ColorEdit4("##editor-color", color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
        Logic::get().save_settings_file();
        GUI::get().write_theme(GUI::get().theme_path("current"));
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
}

void GUI::apply_saved_theme() {
    auto path = GUI::get().theme_path("current");
    if (!std::filesystem::exists(path)) return;
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed) return;
    GUI::get().apply_style_json(parsed.unwrap());
}
void GUI::apply_theme_json(matjson::Value const& json) {
    GUI::get().apply_style_json(json);
    GUI::get().write_theme(GUI::get().theme_path("current"));
    Logic::get().save_settings_file();
}

void GUI::draw_theme() {
    // TODO
}

BindAction const kListed[] = {
    {"open-menu", "Open menu"},
    {"toggle-record", "Toggle recording"},
    {"toggle-play", "Toggle playback"},
    {"toggle-advance", "Toggle frame advance"},
    {"step-frame", "Step one frame"},
    {"step-back", "Step back one frame"},
    {"key-click-both", "Click both players"},
    {"key-swap", "Swap player input"},
    {"key-shakes", "Disable shake"},
    {"key-layout", "Layout mode"},
    {"key-hitboxes", "Show hitboxes"},
    {"key-trajectory", "Show trajectory"},
    {"key-autoclick", "Autoclicker"},
    {"key-no-overwrite", "No overwrite"},
    {"key-ignore", "Ignore actions"},
    {"key-show-recording", "Show recording label"},
    {"key-show-cps", "Show CPS"},
    {"key-show-frame", "Show frame"},
    {"key-show-time", "Show time"},
    {"key-show-percent", "Show percent"},
    {"key-save", "Save replay"},
    {"key-load", "Load replay"},
    {"key-reset", "Reset level"},
    {"key-speedhack", "Speedhack"},
    {"key-click-dash-orbs", "Click dash orbs"},
    {"key-click-black-orbs", "Click black orbs"},
    {"key-pad-black-orb", "Auto black orb on pad"},
    {"key-pad-blue-orb", "Auto blue orb on pad"},
    {"key-pause-before-kill", "Prevent death"},
};

std::optional<cocos2d::enumKeyCodes> GUI::cocos_key(ImGuiKey key) {
    using Code = cocos2d::enumKeyCodes;
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return static_cast<Code>(cocos2d::KEY_A + (key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return static_cast<Code>(cocos2d::KEY_Zero + (key - ImGuiKey_0));
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) return static_cast<Code>(cocos2d::KEY_F1 + (key - ImGuiKey_F1));
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) return static_cast<Code>(cocos2d::KEY_NumPad0 + (key - ImGuiKey_Keypad0));
    switch (key) {
        case ImGuiKey_Tab: return cocos2d::KEY_Tab;
        case ImGuiKey_LeftArrow: return cocos2d::KEY_Left;
        case ImGuiKey_RightArrow: return cocos2d::KEY_Right;
        case ImGuiKey_UpArrow: return cocos2d::KEY_Up;
        case ImGuiKey_DownArrow: return cocos2d::KEY_Down;
        case ImGuiKey_PageUp: return cocos2d::KEY_PageUp;
        case ImGuiKey_PageDown: return cocos2d::KEY_PageDown;
        case ImGuiKey_Home: return cocos2d::KEY_Home;
        case ImGuiKey_End: return cocos2d::KEY_End;
        case ImGuiKey_Insert: return cocos2d::KEY_Insert;
        case ImGuiKey_Delete: return cocos2d::KEY_Delete;
        case ImGuiKey_Backspace: return cocos2d::KEY_Backspace;
        case ImGuiKey_Space: return cocos2d::KEY_Space;
        case ImGuiKey_Enter: return cocos2d::KEY_Enter;
        case ImGuiKey_Escape: return cocos2d::KEY_Escape;
        case ImGuiKey_Apostrophe: return cocos2d::KEY_OEM7;
        case ImGuiKey_Comma: return cocos2d::KEY_OEMComma;
        case ImGuiKey_Minus: return cocos2d::KEY_OEMMinus;
        case ImGuiKey_Period: return cocos2d::KEY_OEMPeriod;
        case ImGuiKey_Slash: return cocos2d::KEY_OEM2;
        case ImGuiKey_Semicolon: return cocos2d::KEY_OEM1;
        case ImGuiKey_Equal: return cocos2d::KEY_OEMPlus;
        case ImGuiKey_LeftBracket: return cocos2d::KEY_OEM4;
        case ImGuiKey_Backslash: return cocos2d::KEY_OEM5;
        case ImGuiKey_RightBracket: return cocos2d::KEY_OEM6;
        case ImGuiKey_GraveAccent: return cocos2d::KEY_OEM3;
        case ImGuiKey_CapsLock: return cocos2d::KEY_CapsLock;
        case ImGuiKey_LeftCtrl: return cocos2d::KEY_LeftControl;
        case ImGuiKey_RightCtrl: return cocos2d::KEY_RightControl;
        case ImGuiKey_LeftShift: return cocos2d::KEY_LeftShift;
        case ImGuiKey_RightShift: return cocos2d::KEY_RightShift;
        case ImGuiKey_LeftAlt: return cocos2d::KEY_LeftMenu;
        case ImGuiKey_RightAlt: return cocos2d::KEY_RightMenu;
        case ImGuiKey_LeftSuper: return cocos2d::KEY_LeftWindowsKey;
        case ImGuiKey_RightSuper: return cocos2d::KEY_RightWindowsKey;
        case ImGuiKey_Menu: return cocos2d::KEY_ApplicationsKey;
        case ImGuiKey_ScrollLock: return cocos2d::KEY_ScrollLock;
        case ImGuiKey_NumLock: return cocos2d::KEY_Numlock;
        case ImGuiKey_PrintScreen: return cocos2d::KEY_PrintScreen;
        case ImGuiKey_Pause: return cocos2d::KEY_Pause;
        case ImGuiKey_KeypadDecimal: return cocos2d::KEY_Decimal;
        case ImGuiKey_KeypadDivide: return cocos2d::KEY_Divide;
        case ImGuiKey_KeypadMultiply: return cocos2d::KEY_Multiply;
        case ImGuiKey_KeypadSubtract: return cocos2d::KEY_Subtract;
        case ImGuiKey_KeypadAdd: return cocos2d::KEY_Add;
        case ImGuiKey_KeypadEnter: return cocos2d::KEY_Enter;
        default: return std::nullopt;
    }
}

bool GUI::is_modifier(ImGuiKey key) {
    switch (key) {
        case ImGuiKey_LeftCtrl:
        case ImGuiKey_RightCtrl:
        case ImGuiKey_LeftShift:
        case ImGuiKey_RightShift:
        case ImGuiKey_LeftAlt:
        case ImGuiKey_RightAlt:
        case ImGuiKey_LeftSuper:
        case ImGuiKey_RightSuper:
        case ImGuiKey_ReservedForModCtrl:
        case ImGuiKey_ReservedForModShift:
        case ImGuiKey_ReservedForModAlt:
        case ImGuiKey_ReservedForModSuper:
            return true;
        default: return false;
    }
}

std::string GUI::key_name(cocos2d::enumKeyCodes key) {
    if (key == cocos2d::KEY_None) return "None";
    char const* name = CCKeyboardDispatcher::get()->keyToString(key);
    if (!name || name[0] == '\0') return "Unknown";
    return name;
}

KeyboardModifier GUI::modifier_for_key(cocos2d::enumKeyCodes key) {
    switch (key) {
        case cocos2d::KEY_LeftControl:
        case cocos2d::KEY_RightControl:
        case cocos2d::KEY_Control:
            return KeyboardModifier::Control;
        case cocos2d::KEY_LeftShift:
        case cocos2d::KEY_RightShift:
        case cocos2d::KEY_Shift:
            return KeyboardModifier::Shift;
        case cocos2d::KEY_LeftMenu:
        case cocos2d::KEY_RightMenu:
        case cocos2d::KEY_Alt:
            return KeyboardModifier::Alt;
        case cocos2d::KEY_LeftWindowsKey:
        case cocos2d::KEY_RightWindowsKey:
            return KeyboardModifier::Super;
        default:
            return KeyboardModifier::None;
    }
}

std::string GUI::stored_bind(char const* setting) {
    auto binds = Mod::get()->getSettingValue<std::vector<Keybind>>(setting);
    if (binds.empty() || binds[0].key == cocos2d::KEY_None) return "None";
    return binds[0].toString();
}

void GUI::store_bind(char const* setting, cocos2d::enumKeyCodes key, bool ctrl, bool shift, bool alt) {
    KeyboardModifier mods = GUI::get().modifier_for_key(key);
    if (ctrl) mods |= KeyboardModifier::Control;
    if (shift) mods |= KeyboardModifier::Shift;
    if (alt) mods |= KeyboardModifier::Alt;
    Mod::get()->setSettingValue<std::vector<Keybind>>(setting, {Keybind(key, mods)});
    Logic::get().save_settings_file();
}

void GUI::flip_setting(bool& field, char const* setting) {
    field = !field;
    Logic::write_bool(setting, field);
    Logic::get().save_settings_file();
}

static void flip_utility(bool& field) {
    field = !field;
    Logic::get().reset_traj();
    Logic::get().save_settings_file();
}

bool Logic::dispatch_setting_key(std::string_view key, bool down) {
    if (!down) return false;
    auto& value = Logic::get();
    if (key == "toggle-record") Logic::get().toggle_recording();
    else if (key == "toggle-play") Logic::get().toggle_playing();
    else if (key == "toggle-advance") Logic::get().toggle_frame_advance();
    else if (key == "key-click-both") GUI::get().flip_setting(value.click_both, "click-both");
    else if (key == "key-swap") GUI::get().flip_setting(value.swap_players, "swap-players");
    else if (key == "key-shakes") GUI::get().flip_setting(value.disable_shakes, "disable-shakes");
    else if (key == "key-layout") GUI::get().flip_setting(value.layout_mode, "layout-mode");
    else if (key == "key-hitboxes") GUI::get().flip_setting(value.show_hitboxes, "show-hitboxes");
    else if (key == "key-trajectory") GUI::get().flip_setting(value.show_traj, "show-trajectory");
    else if (key == "key-autoclick") GUI::get().flip_setting(value.autoclicker, "autoclicker");
    else if (key == "key-no-overwrite") GUI::get().flip_setting(value.no_overwrite, "no-overwrite");
    else if (key == "key-ignore") GUI::get().flip_setting(value.ignore_inputs, "ignore-inputs");
    else if (key == "key-show-recording") GUI::get().flip_setting(value.show_recording, "show-recording");
    else if (key == "key-show-cps") GUI::get().flip_setting(value.show_cps, "show-cps");
    else if (key == "key-show-frame") GUI::get().flip_setting(value.show_frame, "show-frame");
    else if (key == "key-show-time") GUI::get().flip_setting(value.show_time, "show-time");
    else if (key == "key-show-percent") GUI::get().flip_setting(value.show_percent, "show-percent");
    else if (key == "key-click-dash-orbs") flip_utility(value.click_dash_orbs);
    else if (key == "key-click-black-orbs") flip_utility(value.click_black_orbs);
    else if (key == "key-pad-black-orb") flip_utility(value.pad_black_orb);
    else if (key == "key-pad-blue-orb") flip_utility(value.pad_blue_orb);
    else if (key == "key-pause-before-kill") flip_utility(value.pause_before_kill);
    else if (key == "key-save") Logic::get().write_file();
    else if (key == "key-load") Logic::get().pick_replay();
    else if (key == "open-menu") {
        GUI::get().open_popup();
    }
    else if (key == "key-reset") {
        if (auto* play = PlayLayer::get()) play->resetLevel();
    } else if (key == "key-speedhack") {
        if (std::abs(value.speedhack - 1.f) > 0.05f) {
            value.speedhack_memory = value.speedhack;
            value.speedhack = 1.f;
        } else {
            float back = value.speedhack_memory;
            if (!(back >= 0.01f) || std::abs(back - 1.f) <= 0.05f) return false;
            value.speedhack = back;
        }
        Logic::get().sync_time_scale();
        Logic::get().sync_playback_fps();
        Logic::get().save_settings_file();
    } else {
        return false;
    }
    return true;
}

void GUI::offer_rebind(char const* setting_id, char const* tip) {
    if (ImGui::IsItemHovered()) {
        auto current = GUI::get().stored_bind(setting_id);
        if (tip) ImGui::SetTooltip("%s\nRight-click to rebind (%s)", tip, current.c_str());
        else ImGui::SetTooltip("Right-click to rebind (%s)", current.c_str());
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        GUI::get().prompt = setting_id;
        GUI::get().prompt_nested = ImGui::GetCurrentContext()->BeginPopupStack.Size > 0;
    }
}

void GUI::draw_keybind_prompt(bool nested) {
    if (GUI::get().prompt.empty() || GUI::get().prompt_nested != nested) return;
    if (GUI::get().prompt_shown != GUI::get().prompt) {
        GUI::get().prompt_shown = GUI::get().prompt;
        GUI::get().pressed.reset();
        GUI::get().bind_ctrl = false;
        GUI::get().bind_shift = false;
        GUI::get().bind_alt = false;
    }
    if (!ImGui::IsPopupOpen("Confirm Keybind")) ImGui::OpenPopup("Confirm Keybind");
    if (!ImGui::BeginPopupModal("Confirm Keybind", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::string label = GUI::get().prompt;
    for (auto const& action : kListed) {
        if (GUI::get().prompt == action.setting) label = action.label;
    }
    auto existing = GUI::get().stored_bind(GUI::get().prompt.c_str());
    if (existing != "None") ImGui::Text("Existing keybind: %s", existing.c_str());
    ImGui::Text("Press a key for '%s'", label.c_str());

    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        auto imguiKey = static_cast<ImGuiKey>(key);
        if (imguiKey >= ImGuiKey_MouseLeft) continue;
        if (GUI::get().is_modifier(imguiKey)) {
            if (GUI::get().pressed || !ImGui::IsKeyReleased(imguiKey)) continue;
            auto mapped = GUI::get().cocos_key(imguiKey);
            if (!mapped) continue;
            auto own = GUI::get().modifier_for_key(*mapped);
            GUI::get().pressed = mapped;
            GUI::get().bind_ctrl = static_cast<bool>(own & KeyboardModifier::Control);
            GUI::get().bind_shift = static_cast<bool>(own & KeyboardModifier::Shift);
            GUI::get().bind_alt = static_cast<bool>(own & KeyboardModifier::Alt);
            break;
        }
        if (!ImGui::IsKeyPressed(imguiKey, false)) continue;
        auto mapped = GUI::get().cocos_key(imguiKey);
        if (!mapped) continue;
        GUI::get().pressed = mapped;
        GUI::get().bind_ctrl = ImGui::GetIO().KeyCtrl;
        GUI::get().bind_shift = ImGui::GetIO().KeyShift;
        GUI::get().bind_alt = ImGui::GetIO().KeyAlt;
        break;
    }

    float bindW = GUI::get().shared_button_width({"Save", "Remove"});
    if (GUI::get().pressed) {
        auto own = GUI::get().modifier_for_key(*GUI::get().pressed);
        std::string pressed = "Pressed key: " + GUI::get().key_name(*GUI::get().pressed);
        if (GUI::get().bind_ctrl && !(own & KeyboardModifier::Control)) pressed += " + Ctrl";
        if (GUI::get().bind_shift && !(own & KeyboardModifier::Shift)) pressed += " + Shift";
        if (GUI::get().bind_alt && !(own & KeyboardModifier::Alt)) pressed += " + Alt";
        ImGui::TextUnformatted(pressed.c_str());
        if (ImGui::Button("Save", ImVec2(bindW, 0.f))) {
            GUI::get().store_bind(GUI::get().prompt.c_str(), *GUI::get().pressed, GUI::get().bind_ctrl, GUI::get().bind_shift, GUI::get().bind_alt);
            GUI::get().prompt.clear();
            GUI::get().prompt_shown.clear();
            GUI::get().prompt_nested = false;
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Save", ImVec2(bindW, 0.f));
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (existing != "None") {
        if (ImGui::Button("Remove", ImVec2(bindW, 0.f))) {
            GUI::get().store_bind(GUI::get().prompt.c_str(), cocos2d::KEY_None, false, false, false);
            GUI::get().prompt.clear();
            GUI::get().prompt_shown.clear();
            GUI::get().prompt_nested = false;
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Remove", ImVec2(bindW, 0.f));
        ImGui::EndDisabled();
    }
    if (ImGui::Button("Exit", ImVec2(GUI::get().shared_button_width({"Exit"}), 0.f))) {
        GUI::get().prompt.clear();
        GUI::get().prompt_shown.clear();
        GUI::get().prompt_nested = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

$execute {
    static ListenerHandle menuDrag = MouseMoveEvent().listen([](int32_t, int32_t) {
        auto& imgui = ImGuiCocos::get();
        if (!imgui.isInitialized() || !imgui.isVisible()) return false;
        return ImGui::GetIO().WantCaptureMouse || ImGui::IsAnyItemActive();
    });
    static ListenerHandle rightClick = MouseInputEvent().listen([](MouseInputData& data) {
        if (data.button != MouseInputData::Button::Right) return false;
        auto& imgui = ImGuiCocos::get();
        if (!imgui.isInitialized() || !imgui.isVisible()) return false;
        ImGui::GetIO().AddMouseButtonEvent(1, data.action == MouseInputData::Action::Press);
        return ImGui::GetIO().WantCaptureMouse;
    });
}

PickerState& GUI::picker() {
    return GUI::get().replay_picker;
}

std::string GUI::lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

ShownReplay GUI::shown_replay(std::filesystem::path const& path) {
    std::string file = geode::utils::string::pathToString(path.filename());
    std::string lower = GUI::get().lower_copy(file);
    auto cut = [&](char const* ext) {
        size_t n = std::char_traits<char>::length(ext);
        if (lower.size() < n || lower.compare(lower.size() - n, n, ext) != 0) return false;
        file.resize(file.size() - n);
        lower.resize(lower.size() - n);
        return true;
    };
    auto const extensions = {".echo.json", ".json", ".echo", ".txt", ".slc", ".tcm", ".gdr"};
    auto found = std::ranges::find_if(extensions, cut);
    std::string ext = found == extensions.end() ? std::string() : std::string(*found);
    bool autosave = ext == ".echo" && cut(".autosave");
    if (file.empty()) file = geode::utils::string::pathToString(path.stem());
    std::string tag = autosave ? "(autosave)" : ext.empty() || ext == ".echo" ? std::string() : "(" + ext + ")";
    if (path.parent_path().filename() == "converted") tag = tag.empty() ? "(converted)" : "(converted) " + tag;
    return {file, tag};
}

void GUI::refresh_files() {
    auto& state = GUI::get().picker();
    state.files.clear();
    auto take = [&](std::filesystem::path const& dir) {
        std::error_code error;
        if (!std::filesystem::exists(dir, error)) return;
        for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
            if (error || !entry.is_regular_file()) continue;
            auto name = GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().filename()));
            bool replay = name.ends_with(".echo") || name.ends_with(".json") || name.ends_with(".txt") || name.ends_with(".slc") ||
                name.ends_with(".tcm") || name.ends_with(".gdr");
            if (!replay) continue;
            state.files.push_back(entry.path());
        }
    };
    take(Logic::get().save_root() / "replays");
    take(Logic::get().save_root() / "converted");
    std::sort(state.files.begin(), state.files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
        return GUI::get().lower_copy(geode::utils::string::pathToString(a.filename())) < GUI::get().lower_copy(geode::utils::string::pathToString(b.filename()));
    });
}

void GUI::open_macro_picker(bool sequence, int amount) {
    GUI::get().reveal_menu();
    auto& state = GUI::get().picker();
    state.sequence = sequence;
    state.amount = std::max(amount, 1);
    state.filter.clear();
    if (!sequence) {
        GJGameLevel* level = nullptr;
        if (auto* play = PlayLayer::get()) level = play->m_level;
        else if (auto* editor = LevelEditorLayer::get()) level = editor->m_level;
        if (level) state.filter = level->m_levelName;
    }
    state.want_open = true;
    GUI::get().refresh_files();
}
void GUI::draw_macro_picker() {
    auto& state = GUI::get().picker();
    static int drawnFrame = -1;
    if (drawnFrame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Macros###echo-macro-picker");
    float pickerScale = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
    if (!ImGui::BeginPopup("Echo Macros###echo-macro-picker")) return;
    drawnFrame = ImGui::GetFrameCount();
    state.want_open = false;
    ImGui::TextUnformatted(state.sequence ? "Add a macro to the sequence" : "Load a macro");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##macro-filter", "Type to filter macros", &state.filter);
    ImGui::BeginChild("##macro-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (auto const& path : state.files) {
        auto shownName = GUI::get().shown_replay(path);
        std::string blob = GUI::get().lower_copy(shownName.name + " " + shownName.tag + " " + geode::utils::string::pathToString(path.filename()));
        if (!needle.empty() && blob.find(needle) == std::string::npos) continue;
        ++shown;
        ImVec2 row = ImGui::GetCursorScreenPos();
        float rowH = ImGui::GetTextLineHeight();
        std::string rowId = "##replay-" + geode::utils::string::pathToString(path);
        bool picked = ImGui::Selectable(rowId.c_str(), false, 0, ImVec2(0.f, rowH));
        auto* draw = ImGui::GetWindowDrawList();
        float textX = row.x + ImGui::GetStyle().FramePadding.x;
        draw->AddText(ImVec2(textX, row.y), ImGui::GetColorU32(ImGuiCol_Text), shownName.name.c_str());
        if (!shownName.tag.empty()) {
            float tagX = textX + ImGui::CalcTextSize(shownName.name.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
            draw->AddText(ImVec2(tagX, row.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), shownName.tag.c_str());
        }
        if (picked) {
            if (state.sequence) Logic::get().append_sequence_file(path, state.amount);
            else Logic::get().read_file(path);
            ImGui::CloseCurrentPopup();
        }
    }
    if (shown == 0) ImGui::TextDisabled("No macros match");
    ImGui::EndChild();
    float pickerW = GUI::get().shared_button_width({"Refresh", "Close"});
    if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refresh_files();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

ThemePicker& GUI::themes() {
    return GUI::get().theme_picker;
}

void GUI::refresh_themes() {
    auto& state = GUI::get().themes();
    state.files.clear();
    auto dir = Logic::get().save_root() / "themes";
    std::error_code error;
    if (!std::filesystem::exists(dir, error)) return;
    for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file()) continue;
        auto name = GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().filename()));
        if (!name.ends_with(".ui.json")) continue;
        if (name == "current.ui.json") continue;
        state.files.push_back(entry.path());
    }
    std::sort(state.files.begin(), state.files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
        return GUI::get().lower_copy(geode::utils::string::pathToString(a.filename())) < GUI::get().lower_copy(geode::utils::string::pathToString(b.filename()));
    });
}

void GUI::open_theme_picker() {
    GUI::get().reveal_menu();
    auto& state = GUI::get().themes();
    state.filter.clear();
    state.want_open = true;
    GUI::get().refresh_themes();
}
void GUI::draw_theme_picker() {
    auto& state = GUI::get().themes();
    static int drawnFrame = -1;
    if (drawnFrame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Themes###echo-theme-picker");
    float pickerScale = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
    if (!ImGui::BeginPopup("Echo Themes###echo-theme-picker")) return;
    drawnFrame = ImGui::GetFrameCount();
    state.want_open = false;
    ImGui::TextUnformatted("Load a theme");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##theme-filter", "Type to filter themes", &state.filter);
    ImGui::BeginChild("##theme-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (auto const& path : state.files) {
        std::string name = geode::utils::string::pathToString(path.filename());
        if (name.size() >= 8) name.resize(name.size() - 8);
        if (!needle.empty() && GUI::get().lower_copy(name).find(needle) == std::string::npos) continue;
        ++shown;
        if (ImGui::Selectable(name.c_str(), false)) {
            std::ifstream file(path);
            auto parsed = matjson::Value::parse(file);
            if (parsed) {
                GUI::get().apply_theme_json(parsed.unwrap());
            }
            ImGui::CloseCurrentPopup();
        }
    }
    if (shown == 0) ImGui::TextDisabled("No themes match");
    ImGui::EndChild();
    float pickerW = GUI::get().shared_button_width({"Refresh", "Close"});
    if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refresh_themes();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

ClickpackPicker& GUI::clickpacks() {
    return GUI::get().clickpack_picker;
}

void GUI::refresh_clickpacks() {
    auto& state = GUI::get().clickpacks();
    state.folders.clear();
    auto dir = Logic::get().save_root() / "clickpacks";
    std::error_code error;
    if (!std::filesystem::exists(dir, error)) return;
    for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
        if (error || !entry.is_directory()) continue;
        state.folders.push_back(entry.path());
    }
    std::sort(state.folders.begin(), state.folders.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
        return GUI::get().lower_copy(geode::utils::string::pathToString(a.filename())) < GUI::get().lower_copy(geode::utils::string::pathToString(b.filename()));
    });
}

void GUI::open_clickpack_picker() {
    GUI::get().reveal_menu();
    auto& state = GUI::get().clickpacks();
    state.filter.clear();
    state.want_open = true;
    GUI::get().refresh_clickpacks();
}
void GUI::draw_clickpack_picker() {
    auto& state = GUI::get().clickpacks();
    static int drawnFrame = -1;
    if (drawnFrame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Clickpacks###echo-clickpack-picker");
    float pickerScale = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
    if (!ImGui::BeginPopup("Echo Clickpacks###echo-clickpack-picker")) return;
    drawnFrame = ImGui::GetFrameCount();
    state.want_open = false;
    ImGui::TextUnformatted(state.player == 0 ? "Pick a clickpack for Player 1" : "Pick a clickpack for Player 2");
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##clickpack-filter", "Type to filter clickpacks", &state.filter);
    ImGui::BeginChild("##clickpack-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (auto const& path : state.folders) {
        std::string name = geode::utils::string::pathToString(path.filename());
        if (!needle.empty() && GUI::get().lower_copy(name).find(needle) == std::string::npos) continue;
        ++shown;
        if (ImGui::Selectable(name.c_str(), false)) {
            try {
                if (state.player == 0) Logic::get().clickpack = path.string();
                else Logic::get().clickpack_p2 = path.string();
            } catch (...) {
                Logic::get().notify("Rename the clickpack folder to use plain characters", false);
            }
            Logic::get().push_runtime_settings();
            ImGui::CloseCurrentPopup();
        }
    }
    if (shown == 0) ImGui::TextDisabled("No clickpacks match");
    ImGui::EndChild();
    float pickerW = GUI::get().shared_button_width({"Refresh", "Close"});
    if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refresh_clickpacks();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

RenderPicker& GUI::renders() {
    return GUI::get().render_picker;
}

void GUI::refresh_renders() {
    auto& state = GUI::get().renders();
    state.files.clear();
    auto dir = Logic::get().save_root() / "renders";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (!std::filesystem::exists(dir, error)) return;
    std::string extra = GUI::get().lower_copy(Logic::get().render_extension);
    if (!extra.empty() && extra.front() != '.') extra.insert(extra.begin(), '.');
    for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file()) continue;
        auto name = GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().filename()));
        bool video = name.ends_with(".mp4") || name.ends_with(".mkv") || name.ends_with(".mov") || name.ends_with(".webm");
        if (!video && !extra.empty() && extra != ".") video = name.ends_with(extra);
        if (!video) continue;
        state.files.push_back(entry.path());
    }
    std::sort(state.files.begin(), state.files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
        return GUI::get().lower_copy(geode::utils::string::pathToString(a.filename())) < GUI::get().lower_copy(geode::utils::string::pathToString(b.filename()));
    });
}

void GUI::open_render_picker() {
    GUI::get().reveal_menu();
    auto& state = GUI::get().renders();
    state.second = false;
    state.first.clear();
    state.filter.clear();
    state.want_open = true;
    GUI::get().refresh_renders();
}

void GUI::draw_render_picker() {
    auto& state = GUI::get().renders();
    static int drawnFrame = -1;
    if (drawnFrame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Renders###echo-render-picker");
    float pickerScale = ImGui::GetFontSize() / 18.f;
    ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
    if (!ImGui::BeginPopup("Echo Renders###echo-render-picker")) return;
    drawnFrame = ImGui::GetFrameCount();
    state.want_open = false;
    ImGui::TextUnformatted(state.second ? "Select the second video" : "Select the first video");
    if (state.second) ImGui::TextDisabled("%s", geode::utils::string::pathToString(state.first.filename()).c_str());
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##render-filter", "Type to filter videos", &state.filter);
    ImGui::BeginChild("##render-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (auto const& path : state.files) {
        std::string name = geode::utils::string::pathToString(path.filename());
        if (!needle.empty() && GUI::get().lower_copy(name).find(needle) == std::string::npos) continue;
        ++shown;
        std::string rowId = name + "##render-" + geode::utils::string::pathToString(path);
        if (!ImGui::Selectable(rowId.c_str(), false)) continue;
        if (!state.second) {
            state.first = path;
            state.second = true;
            state.filter.clear();
            continue;
        }
        state.second = false;
        state.first.clear();
        ImGui::CloseCurrentPopup();
    }
    if (shown == 0) ImGui::TextDisabled("No videos match");
    ImGui::EndChild();
    float pickerW = GUI::get().shared_button_width({"Refresh", "Close"});
    if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refresh_renders();
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
