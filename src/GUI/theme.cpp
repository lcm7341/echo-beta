#include <imgui_internal.h>
#include "GUI/gui.hpp"
#include "Logic/conversions/codecs.hpp"
#include "Online/replay_db.hpp"
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

namespace fs = std::filesystem;

using namespace geode::prelude;

void GUI::help_marker(const char* text) {
    EchoUI::Help();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

matjson::Value GUI::color_json(const ImVec4& color) {
    matjson::Value value = matjson::Value::array();
    value.push(color.x);
    value.push(color.y);
    value.push(color.z);
    value.push(color.w);
    return value;
}

matjson::Value GUI::pair_json(const ImVec2& value) {
    matjson::Value json = matjson::Value::array();
    json.push(value.x);
    json.push(value.y);
    return json;
}

void GUI::read_pair(const matjson::Value& json, const char* key, ImVec2& value) {
    if (!json[key].isArray() || json[key].size() < 2) return;
    if (auto x = json[key][0].asDouble()) value.x = (float)(x.unwrap());
    if (auto y = json[key][1].asDouble()) value.y = (float)(y.unwrap());
}

void GUI::read_color(const matjson::Value& json, const char* key, float* color) {
    if (!json[key].isArray() || json[key].size() < 4) return;
    for (int i = 0; i < 4; ++i) {
        if (auto channel = json[key][i].asDouble()) color[i] = (float)(channel.unwrap());
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
    ImGuiStyle style = GUI::get().has_base ? GUI::get().base_style : ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        style.Colors[i] = ImGui::GetStyle().Colors[i];
    auto& logic = Logic::get();
    matjson::Value json;
    json["name"] = logic.theme_name;
    json["scale"] = logic.menu_scale;
    json["rainbow_rim"] = EchoUI::g_rainbow_rim;
    json["button_effects"] = EchoUI::g_button_effects;
    json["scene"] = EchoUI::scene_name(EchoUI::g_scene);
    if (GUI::get().has_palette) {
        const auto& palette = GUI::get().palette;
        matjson::Value saved;
        saved["hue"] = palette.hue;
        saved["accent_hue"] = palette.accent_hue;
        saved["richness"] = palette.richness;
        saved["depth"] = palette.depth;
        saved["accent_saturation"] = palette.accent_saturation;
        saved["accent_value"] = palette.accent_value;
        saved["light"] = palette.light;
        saved["rainbow"] = palette.rainbow;
        saved["scene"] = EchoUI::scene_name(palette.scene);
        json["palette"] = saved;
    }
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
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        json["colors"].push(GUI::get().color_json(style.Colors[i]));
    json["editor_p1"] = matjson::Value::array();
    json["editor_p2"] = matjson::Value::array();
    json["editor_sel_p1"] = matjson::Value::array();
    json["editor_sel_p2"] = matjson::Value::array();
    for (int i = 0; i < 4; ++i) {
        json["editor_p1"].push(logic.editor_p1[i]);
        json["editor_p2"].push(logic.editor_p2[i]);
        json["editor_sel_p1"].push(logic.editor_sel_p1[i]);
        json["editor_sel_p2"].push(logic.editor_sel_p2[i]);
    }
    return json;
}

void GUI::apply_style_json(const matjson::Value& json) {
    GUI::get().restore_base_sizes();
    auto& style = ImGui::GetStyle();
    auto& logic = Logic::get();
    if (auto text = json["name"].asString()) logic.theme_name = text.unwrap();
    if (auto number = json["scale"].asDouble())
        logic.menu_scale = std::clamp((float)(number.unwrap()), 0.35f, 3.f);
    EchoUI::g_rainbow_rim = json["rainbow_rim"].asBool().unwrapOr(false);
    if (json.contains("button_effects"))
        EchoUI::g_button_effects = json["button_effects"].asBool().unwrapOr(true);
    auto scene_in = [](const matjson::Value& from) {
        if (from["halloween"].asBool().unwrapOr(false)) return EchoUI::Scene::Halloween;
        return EchoUI::scene_named(from["scene"].asString().unwrapOr(""));
    };
    EchoUI::g_scene = scene_in(json);
    GUI::get().has_palette = json.contains("palette");
    if (GUI::get().has_palette) {
        const auto& saved = json["palette"];
        auto number = [&](const char* key, float fallback) {
            return (float)(saved[key].asDouble().unwrapOr(fallback));
        };
        EchoUI::Palette palette;
        palette.hue = number("hue", palette.hue);
        palette.accent_hue = number("accent_hue", palette.accent_hue);
        palette.richness = number("richness", palette.richness);
        palette.depth = number("depth", palette.depth);
        palette.accent_saturation = number("accent_saturation", palette.accent_saturation);
        palette.accent_value = number("accent_value", palette.accent_value);
        palette.light = saved["light"].asBool().unwrapOr(false);
        palette.rainbow = saved["rainbow"].asBool().unwrapOr(false);
        palette.scene = scene_in(saved);
        GUI::get().palette = palette;
        EchoUI::sync_scene_turn(palette);
    }
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
    auto number = [&](const char* key, float& field) {
        if (auto parsed = json[key].asDouble()) field = (float)(parsed.unwrap());
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
    }
    else if (style.CircleTessellationMaxError >= 2.5f) {
        style.CircleTessellationMaxError = 5.f;
    }
    else {
        style.CircleTessellationMaxError = 0.1f;
    }
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    if (json["colors"].isArray()) {
        int amt = std::min((int)json["colors"].size(), (int)ImGuiCol_COUNT);
        for (int i = 0; i < amt; ++i) {
            if (!json["colors"][i].isArray() || json["colors"][i].size() < 4) continue;
            ImVec4 color = style.Colors[i];
            if (auto channel = json["colors"][i][0].asDouble()) color.x = (float)(channel.unwrap());
            if (auto channel = json["colors"][i][1].asDouble()) color.y = (float)(channel.unwrap());
            if (auto channel = json["colors"][i][2].asDouble()) color.z = (float)(channel.unwrap());
            if (auto channel = json["colors"][i][3].asDouble()) color.w = (float)(channel.unwrap());
            style.Colors[i] = color;
        }
    }
    EchoUI::sync_night_menu(GUI::get().has_palette ? GUI::get().palette : EchoUI::Palette{});
    GUI::get().read_color(json, "editor_p1", logic.editor_p1);
    GUI::get().read_color(json, "editor_p2", logic.editor_p2);
    GUI::get().read_color(json, "editor_sel_p1", logic.editor_sel_p1);
    GUI::get().read_color(json, "editor_sel_p2", logic.editor_sel_p2);
    if (style.CurveTessellationTol < 0.10f) style.CurveTessellationTol = 0.10f;
    bool absolute = false;
    if (auto flag = json["metrics_absolute"].asBool()) absolute = flag.unwrap();
    if (!absolute && logic.menu_scale != 1.f) {
        float bake = logic.menu_scale;
        auto grow = [&](ImVec2& pair) {
            pair.x *= bake;
            pair.y *= bake;
        };
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

fs::path GUI::theme_path(const std::string& name) {
    auto safe = ReplayIO::sanitize(name.empty() ? std::string("Default") : name);
    return Logic::get().save_root() / "themes" / ReplayIO::utf8_path(safe + ".ui.json");
}

void GUI::export_theme(const fs::path& path) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path);
    if (!file) return;
    file << GUI::get().style_json().dump(2);
}

void GUI::note_style_edit() {
    if (ImGui::IsItemDeactivatedAfterEdit())
        GUI::get().export_theme(GUI::get().theme_path("current"));
}

bool edit_style_vec(const char* id, ImVec2& live, float min, float max) {
    struct Draft {
        ImVec2 value;
        bool drag = false;
    };

    static std::unordered_map<std::string, Draft> drafts;
    auto& draft = drafts[id];
    if (!draft.drag) draft.value = live;
    EchoUI::SliderFloat2(id, &draft.value.x, min, max, "%.0f");
    if (ImGui::IsItemActivated()) draft.drag = true;
    if (!ImGui::IsItemDeactivated()) return false;
    draft.drag = false;
    live = draft.value;
    GUI::get().note_style_edit();
    return true;
}

bool edit_style_float(const char* id, float& live, float min, float max) {
    struct Draft {
        float value = 0.f;
        bool drag = false;
    };

    static std::unordered_map<std::string, Draft> drafts;
    auto& draft = drafts[id];
    if (!draft.drag) draft.value = live;
    EchoUI::SliderFloat(id, &draft.value, min, max, "%.0f");
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

// editor rows come from the themes own colors so they always sit right on it
// p1 = the accent, p2 = the same accent turned a bit (NOT the opposite color, that clashed)
void GUI::paint_editor_from_theme() {
    auto& logic = Logic::get();
    const auto& colors = ImGui::GetStyle().Colors;
    ImVec4 base = colors[ImGuiCol_FrameBg], accent = colors[ImGuiCol_CheckMark];
    base.w = accent.w = 1.f;
    float h, sat, val;
    ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, h, sat, val);
    ImVec4 second = accent;
    if (sat < .08f) {
        ImGui::ColorConvertHSVtoRGB(.6f, .4f, std::max(val, .6f), second.x, second.y, second.z);
    }
    else {
        float turned = h + 50.f / 360.f;
        turned -= std::floor(turned);
        ImGui::ColorConvertHSVtoRGB(turned, sat, val, second.x, second.y, second.z);
    }
    auto blend = [&](const ImVec4& tint, float amount) {
        return ImVec4(base.x + (tint.x - base.x) * amount, base.y + (tint.y - base.y) * amount, base.z + (tint.z - base.z) * amount, 1.f);
    };
    GUI::get().paint_editor(logic.editor_p1, blend(accent, .32f));
    GUI::get().paint_editor(logic.editor_p2, blend(second, .32f));
    GUI::get().paint_editor(logic.editor_sel_p1, blend(accent, .62f));
    GUI::get().paint_editor(logic.editor_sel_p2, blend(second, .62f));
}

void GUI::use_palette(const EchoUI::Palette& palette, const std::string& name) {
    EchoUI::apply_palette(palette);
    GUI::get().palette = palette;
    GUI::get().has_palette = true;
    auto& logic = Logic::get();
    GUI::get().paint_editor_from_theme();
    logic.theme_name = name;
    GUI::get().export_theme(GUI::get().theme_path("current"));
    Logic::get().save_settings_file();
}

void GUI::randomize_theme() {
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    EchoUI::Palette palette;
    palette.hue = unit(rng);
    float lean = unit(rng) < .35f ? 0.f : (unit(rng) - .5f) * (70.f / 360.f);
    palette.accent_hue = palette.hue + lean;
    palette.richness = .7f + unit(rng) * .5f;
    palette.depth = .85f + unit(rng) * .35f;
    palette.accent_saturation = .38f + unit(rng) * .34f;
    GUI::get().use_palette(palette, "Random");
}

void GUI::color_row(const char* label, float* color, int id) {
    ImGui::PushID(id);
    if (ImGui::ColorEdit4("##editor-color", color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
        Logic::get().save_settings_file();
        GUI::get().export_theme(GUI::get().theme_path("current"));
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
}

void GUI::apply_saved_theme() {
    auto path = GUI::get().theme_path("current");
    const int ECHO_STYLE = 2;
    auto& logic = Logic::get();
    if (logic.ui_style < ECHO_STYLE) {
        std::error_code error;
        auto kept = GUI::get().theme_path("Before Echo style");
        if (fs::exists(path, error) && !fs::exists(kept, error)) fs::rename(path, kept, error);
        logic.ui_style = ECHO_STYLE;
        logic.theme_name = "Echo";
        GUI::get().palette = EchoUI::Palette{};
        GUI::get().has_palette = true;
        GUI::get().export_theme(path);
        Logic::get().save_settings_file();
        return;
    }
    if (!fs::exists(path)) return;
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed) return;
    GUI::get().apply_style_json(parsed.unwrap());
}

matjson::Value GUI::convert_old_theme(const matjson::Value& old, const std::string& name) {
    matjson::Value json;
    json["name"] = name;
    json["metrics_absolute"] = true;
    if (auto scale = old["GlobalScale"].asDouble()) json["scale"] = scale.unwrap();
    for (std::string key : { "WindowPadding", "FramePadding", "CellPadding", "ItemSpacing", "ItemInnerSpacing", "TouchExtraPadding",
             "WindowTitleAlign", "ButtonTextAlign", "SelectableTextAlign", "DisplaySafeAreaPadding" }) {
        auto x = old[key + "X"].asDouble();
        auto y = old[key + "Y"].asDouble();
        if (x) {
            if (y)
                json[key] = GUI::get().pair_json(ImVec2((float)(x.unwrap()), (float)(y.unwrap())));
        }
    }
    for (std::string key : { "IndentSpacing", "ScrollbarSize", "GrabMinSize", "WindowBorderSize", "ChildBorderSize", "PopupBorderSize",
             "FrameBorderSize", "TabBorderSize", "WindowRounding", "ChildRounding", "FrameRounding", "PopupRounding", "ScrollbarRounding",
             "GrabRounding", "TabRounding", "CurveTessellationTol", "CircleTessellationMaxError" }) {
        if (old.contains(key)) json[key] = old[key];
    }
    auto& style = ImGui::GetStyle();
    matjson::Value colors = matjson::Value::array();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        std::string key = ImGui::GetStyleColorName(i);
        if (i == ImGuiCol_TabSelected) key = "TabActive";
        if (i == ImGuiCol_TabDimmed) key = "TabUnfocused";
        if (i == ImGuiCol_TabDimmedSelected) key = "TabUnfocusedActive";
        if (i == ImGuiCol_NavCursor) key = "NavHighlight";
        if (old[key].isArray() && old[key].size() >= 4) colors.push(old[key]);
        else colors.push(GUI::get().color_json(style.Colors[i]));
    }
    json["colors"] = colors;
    for (auto [from, to] : { std::pair{ "player_1_button_color", "editor_p1" }, std::pair{ "player_2_button_color", "editor_p2" },
             std::pair{ "input_selected_color_p1", "editor_sel_p1" }, std::pair{ "input_selected_color_p2", "editor_sel_p2" } }) {
        if (old.contains(from)) json[to] = old[from];
    }
    return json;
}

void GUI::import_theme(const fs::path& path) {
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed) return;
    auto json = parsed.unwrap();
    if (!json["colors"].isArray() && json.contains("FramePaddingX"))
        json = GUI::get().convert_old_theme(json, geode::utils::string::pathToString(path.stem()));
    GUI::get().apply_theme_json(json);
}

void GUI::apply_theme_json(const matjson::Value& json) {
    GUI::get().apply_style_json(json);
    GUI::get().export_theme(GUI::get().theme_path("current"));
    Logic::get().save_settings_file();
}

void GUI::ui_editor() {
    auto& logic = Logic::get();
    auto& style = ImGui::GetStyle();
    auto& sizes = GUI::get().base_style;
    float theme_child_h = ImGui::GetFontSize() * 18.f;
    float filter_offset = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    float sizes_child_h = theme_child_h + filter_offset, theme_child_w = ImGui::GetFontSize() * 26.f;
    float theme_item_w = ImGui::GetFontSize() * 14.f;

    if (ImGui::BeginTabBar("##echo-theme")) {
        if (ImGui::BeginTabItem("Sizes")) {
            ImGui::BeginChild("##echo-sizes", ImVec2(theme_child_w, sizes_child_h), ImGuiChildFlags_Borders);
            ImGui::PushItemWidth(theme_item_w);
            ImGui::TextUnformatted("Main");
            edit_style_vec("WindowPadding", sizes.WindowPadding, 0.f, 20.f);
            edit_style_vec("FramePadding", sizes.FramePadding, 0.f, 20.f);
            edit_style_vec("CellPadding", sizes.CellPadding, 0.f, 20.f);
            edit_style_vec("ItemSpacing", sizes.ItemSpacing, 0.f, 20.f);
            edit_style_vec("ItemInnerSpacing", sizes.ItemInnerSpacing, 0.f, 20.f);
            edit_style_vec("TouchExtraPadding", sizes.TouchExtraPadding, 0.f, 10.f);
            edit_style_float("IndentSpacing", sizes.IndentSpacing, 0.f, 30.f);
            edit_style_float("ScrollbarSize", sizes.ScrollbarSize, 1.f, 20.f);
            edit_style_float("GrabMinSize", sizes.GrabMinSize, 1.f, 20.f);
            ImGui::TextUnformatted("Borders");
            edit_style_float("WindowBorderSize", sizes.WindowBorderSize, 0.f, 1.f);
            edit_style_float("ChildBorderSize", sizes.ChildBorderSize, 0.f, 1.f);
            edit_style_float("PopupBorderSize", sizes.PopupBorderSize, 0.f, 1.f);
            edit_style_float("FrameBorderSize", sizes.FrameBorderSize, 0.f, 1.f);
            edit_style_float("TabBorderSize", sizes.TabBorderSize, 0.f, 1.f);
            ImGui::TextUnformatted("Rounding");
            edit_style_float("WindowRounding", sizes.WindowRounding, 0.f, 12.f);
            edit_style_float("ChildRounding", sizes.ChildRounding, 0.f, 12.f);
            edit_style_float("FrameRounding", sizes.FrameRounding, 0.f, 12.f);
            edit_style_float("PopupRounding", sizes.PopupRounding, 0.f, 12.f);
            edit_style_float("ScrollbarRounding", sizes.ScrollbarRounding, 0.f, 12.f);
            edit_style_float("GrabRounding", sizes.GrabRounding, 0.f, 12.f);
            edit_style_float("TabRounding", sizes.TabRounding, 0.f, 12.f);
            ImGui::TextUnformatted("Alignment");
            EchoUI::DragFloat2("WindowTitleAlign", &sizes.WindowTitleAlign.x, 0.01f, 0.f, 1.f, "%.2f");
            GUI::get().note_style_edit();
            int button_side = (int)sizes.WindowMenuButtonPosition + 1;
            if (EchoUI::Combo("WindowBtnPos", &button_side, "None\0Left\0Right\0")) {
                sizes.WindowMenuButtonPosition = (ImGuiDir)(button_side - 1);
                GUI::get().export_theme(GUI::get().theme_path("current"));
            }
            int color_side = (int)sizes.ColorButtonPosition;
            if (EchoUI::Combo("ColorButtonPosition", &color_side, "Left\0Right\0")) {
                sizes.ColorButtonPosition = (ImGuiDir)color_side;
                GUI::get().export_theme(GUI::get().theme_path("current"));
            }
            EchoUI::DragFloat2("ButtonTextAlign", &sizes.ButtonTextAlign.x, 0.01f, 0.f, 1.f, "%.2f");
            ImGui::SameLine();
            GUI::get().help_marker("Alignment applies when a button is larger than its text.");
            GUI::get().note_style_edit();
            EchoUI::DragFloat2("SelectableTextAlign", &sizes.SelectableTextAlign.x, 0.01f, 0.f, 1.f, "%.2f");
            GUI::get().note_style_edit();
            ImGui::TextUnformatted("Safe Area Padding");
            ImGui::SameLine();
            GUI::get().help_marker("Adjust if the window edges sit off the screen.");
            EchoUI::SliderFloat2("SafePadding", &sizes.DisplaySafeAreaPadding.x, 0.f, 30.f, "%.0f");
            GUI::get().note_style_edit();
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Colors")) {
            static ImGuiTextFilter filter;
            filter.Draw("Filter colors", GUI::get().px(220.f));
            ImGui::BeginChild("##echo-colors", ImVec2(theme_child_w, theme_child_h), ImGuiChildFlags_Borders);
            if (filter.PassFilter("Player 1 In Editor"))
                GUI::get().color_row("Player 1 In Editor", logic.editor_p1, 998);
            if (filter.PassFilter("Player 2 In Editor"))
                GUI::get().color_row("Player 2 In Editor", logic.editor_p2, 999);
            if (filter.PassFilter("Player 1 In Editor (Selected)"))
                GUI::get().color_row("Player 1 In Editor (Selected)", logic.editor_sel_p1, 997);
            if (filter.PassFilter("Player 2 In Editor (Selected)"))
                GUI::get().color_row("Player 2 In Editor (Selected)", logic.editor_sel_p2, 996);
            for (int i = 0; i < ImGuiCol_COUNT; ++i) {
                const char* name = ImGui::GetStyleColorName(i);
                if (!filter.PassFilter(name)) continue;
                ImGui::PushID(i);
                if (ImGui::ColorEdit4("##style-color", &style.Colors[i].x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                    GUI::get().export_theme(GUI::get().theme_path("current"));
                }
                ImGui::SameLine();
                ImGui::TextUnformatted(name);
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

const BindAction LISTED[] = {
    { "open-menu", "Open menu" },
    { "toggle-record", "Toggle recording" },
    { "toggle-play", "Toggle playback" },
    { "toggle-advance", "Toggle frame advance" },
    { "step-frame", "Step one frame" },
    { "step-back", "Step back one frame" },
    { "find-best-frame", "Find best frame" },
    { "key-click-both", "Click both players" },
    { "key-swap", "Swap player input" },
    { "key-shakes", "Disable shake" },
    { "key-layout", "Layout mode" },
    { "key-hitboxes", "Show hitboxes" },
    { "key-trajectory", "Show trajectory" },
    { "key-autoclick", "Autoclicker" },
    { "key-traj-autoclick", "Trajectory autoclicker" },
    { "key-noclip", "Noclip" },
    { "key-no-overwrite", "No overwrite" },
    { "key-ignore", "Ignore actions" },
    { "key-show-recording", "Show recording label" },
    { "key-show-cps", "Show CPS" },
    { "key-show-frame", "Show frame" },
    { "key-show-time", "Show time" },
    { "key-show-percent", "Show percent" },
    { "key-save", "Save replay" },
    { "key-load", "Load replay" },
    { "key-reset", "Reset level" },
    { "key-speedhack", "Speedhack" },
    { "key-click-dash-orbs", "Click dash orbs" },
    { "key-click-black-orbs", "Click black orbs" },
    { "key-pad-black-orb", "Auto black orb on pad" },
    { "key-pad-blue-orb", "Auto blue orb on pad" },
    { "key-pause-before-kill", "Prevent death" },
};

std::span<BindAction const> GUI::bind_actions() {
    return LISTED;
}

std::optional<enumKeyCodes> GUI::cocos_key(ImGuiKey key) {
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return (enumKeyCodes)(KEY_A + (key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return (enumKeyCodes)(KEY_Zero + (key - ImGuiKey_0));
    if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24)
        return (enumKeyCodes)(KEY_F1 + (key - ImGuiKey_F1));
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return (enumKeyCodes)(KEY_NumPad0 + (key - ImGuiKey_Keypad0));
    switch (key) {
    case ImGuiKey_Tab:
        return KEY_Tab;
    case ImGuiKey_LeftArrow:
        return KEY_Left;
    case ImGuiKey_RightArrow:
        return KEY_Right;
    case ImGuiKey_UpArrow:
        return KEY_Up;
    case ImGuiKey_DownArrow:
        return KEY_Down;
    case ImGuiKey_PageUp:
        return KEY_PageUp;
    case ImGuiKey_PageDown:
        return KEY_PageDown;
    case ImGuiKey_Home:
        return KEY_Home;
    case ImGuiKey_End:
        return KEY_End;
    case ImGuiKey_Insert:
        return KEY_Insert;
    case ImGuiKey_Delete:
        return KEY_Delete;
    case ImGuiKey_Backspace:
        return KEY_Backspace;
    case ImGuiKey_Space:
        return KEY_Space;
    case ImGuiKey_Enter:
        return KEY_Enter;
    case ImGuiKey_Escape:
        return KEY_Escape;
    case ImGuiKey_Apostrophe:
        return KEY_OEM7;
    case ImGuiKey_Comma:
        return KEY_OEMComma;
    case ImGuiKey_Minus:
        return KEY_OEMMinus;
    case ImGuiKey_Period:
        return KEY_OEMPeriod;
    case ImGuiKey_Slash:
        return KEY_OEM2;
    case ImGuiKey_Semicolon:
        return KEY_OEM1;
    case ImGuiKey_Equal:
        return KEY_OEMPlus;
    case ImGuiKey_LeftBracket:
        return KEY_OEM4;
    case ImGuiKey_Backslash:
        return KEY_OEM5;
    case ImGuiKey_RightBracket:
        return KEY_OEM6;
    case ImGuiKey_GraveAccent:
        return KEY_OEM3;
    case ImGuiKey_CapsLock:
        return KEY_CapsLock;
    case ImGuiKey_LeftCtrl:
        return KEY_LeftControl;
    case ImGuiKey_RightCtrl:
        return KEY_RightControl;
    case ImGuiKey_LeftShift:
        return KEY_LeftShift;
    case ImGuiKey_RightShift:
        return KEY_RightShift;
    case ImGuiKey_LeftAlt:
        return KEY_LeftMenu;
    case ImGuiKey_RightAlt:
        return KEY_RightMenu;
    case ImGuiKey_LeftSuper:
        return KEY_LeftWindowsKey;
    case ImGuiKey_RightSuper:
        return KEY_RightWindowsKey;
    case ImGuiKey_Menu:
        return KEY_ApplicationsKey;
    case ImGuiKey_ScrollLock:
        return KEY_ScrollLock;
    case ImGuiKey_NumLock:
        return KEY_Numlock;
    case ImGuiKey_PrintScreen:
        return KEY_PrintScreen;
    case ImGuiKey_Pause:
        return KEY_Pause;
    case ImGuiKey_KeypadDecimal:
        return KEY_Decimal;
    case ImGuiKey_KeypadDivide:
        return KEY_Divide;
    case ImGuiKey_KeypadMultiply:
        return KEY_Multiply;
    case ImGuiKey_KeypadSubtract:
        return KEY_Subtract;
    case ImGuiKey_KeypadAdd:
        return KEY_Add;
    case ImGuiKey_KeypadEnter:
        return KEY_Enter;
    default:
        return std::nullopt;
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
    default:
        return false;
    }
}

std::string GUI::key_name(enumKeyCodes key) {
    if (key == KEY_None) return "None";
    const char* name = CCKeyboardDispatcher::get()->keyToString(key);
    if (!name || name[0] == '\0') return "Unknown";
    return name;
}

KeyboardModifier GUI::modifier_for_key(enumKeyCodes key) {
    switch (key) {
    case KEY_LeftControl:
    case KEY_RightControl:
    case KEY_Control:
        return KeyboardModifier::Control;
    case KEY_LeftShift:
    case KEY_RightShift:
    case KEY_Shift:
        return KeyboardModifier::Shift;
    case KEY_LeftMenu:
    case KEY_RightMenu:
    case KEY_Alt:
        return KeyboardModifier::Alt;
    case KEY_LeftWindowsKey:
    case KEY_RightWindowsKey:
        return KeyboardModifier::Super;
    default:
        return KeyboardModifier::None;
    }
}

std::string GUI::stored_bind(const char* setting) {
    auto binds = Mod::get()->getSettingValue<std::vector<Keybind>>(setting);
    if (binds.empty() || binds[0].key == KEY_None) return "None";
    return binds[0].toString();
}

void GUI::store_bind(const char* setting, enumKeyCodes key, bool ctrl, bool shift, bool alt) {
    KeyboardModifier mods = GUI::get().modifier_for_key(key);
    if (ctrl) mods |= KeyboardModifier::Control;
    if (shift) mods |= KeyboardModifier::Shift;
    if (alt) mods |= KeyboardModifier::Alt;
#ifdef GEODE_IS_MACOS
    if (key != KEY_None && GUI::get().bind_super) mods |= KeyboardModifier::Super;
#endif
    Mod::get()->setSettingValue<std::vector<Keybind>>(setting, { Keybind(key, mods) });
    Logic::get().save_settings_file();
}

void GUI::flip_setting(bool& field, const char* setting) {
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
    if (key == "toggle-record") toggle_recording();
    else if (key == "toggle-play") toggle_playing();
    else if (key == "toggle-advance") toggle_frame_advance();
    else if (key == "key-click-both") GUI::get().flip_setting(click_both, "click-both");
    else if (key == "key-swap") GUI::get().flip_setting(swap_players, "swap-players");
    else if (key == "key-shakes") GUI::get().flip_setting(disable_shakes, "disable-shakes");
    else if (key == "key-layout") GUI::get().flip_setting(layout_mode, "layout-mode");
    else if (key == "key-hitboxes") GUI::get().flip_setting(show_hitboxes, "show-hitboxes");
    else if (key == "key-trajectory") GUI::get().flip_setting(show_traj, "show-trajectory");
    else if (key == "key-autoclick") GUI::get().flip_setting(autoclicker, "autoclicker");
    else if (key == "key-traj-autoclick") flip_utility(traj_autoclicker);
    else if (key == "key-noclip") {
        if (noclip_p1 || noclip_p2) {
            noclip_memory_p1 = noclip_p1;
            noclip_memory_p2 = noclip_p2;
            noclip_p1 = noclip_p2 = false;
        }
        else {
            noclip_p1 = noclip_memory_p1;
            noclip_p2 = noclip_memory_p2;
            if (!noclip_p1 && !noclip_p2) noclip_p1 = noclip_p2 = true;
        }
        save_settings_file();
    }
    else if (key == "key-no-overwrite") GUI::get().flip_setting(no_overwrite, "no-overwrite");
    else if (key == "key-ignore") GUI::get().flip_setting(ignore_inputs, "ignore-inputs");
    else if (key == "key-show-recording") GUI::get().flip_setting(show_recording, "show-recording");
    else if (key == "key-show-cps") GUI::get().flip_setting(show_cps, "show-cps");
    else if (key == "key-show-frame") GUI::get().flip_setting(show_frame, "show-frame");
    else if (key == "key-show-time") GUI::get().flip_setting(show_time, "show-time");
    else if (key == "key-show-percent") GUI::get().flip_setting(show_percent, "show-percent");
    else if (key == "key-click-dash-orbs") flip_utility(click_dash_orbs);
    else if (key == "key-click-black-orbs") flip_utility(click_black_orbs);
    else if (key == "key-pad-black-orb") flip_utility(pad_black_orb);
    else if (key == "key-pad-blue-orb") flip_utility(pad_blue_orb);
    else if (key == "key-pause-before-kill") flip_utility(pause_before_kill);
    else if (key == "key-save") write_file();
    else if (key == "key-load") pick_replay();
    else if (key == "open-menu") {
        GUI::get().open_popup();
    }
    else if (key == "key-reset") {
        if (auto* play = PlayLayer::get()) play->resetLevel();
    }
    else if (key == "key-speedhack") {
        if (std::abs(speedhack - 1.f) > 0.05f) {
            saved_speedhack = speedhack;
            speedhack = 1.f;
        }
        else {
            float back = saved_speedhack;
            if (!(back >= 0.01f) || std::abs(back - 1.f) <= 0.05f) return false;
            speedhack = back;
        }
        sync_time_scale();
        sync_playback_fps();
        save_settings_file();
    }
    else {
        return false;
    }
    return true;
}

void GUI::offer_rebind(const char* setting_id, const char* tip) {
#ifdef GEODE_IS_MOBILE
    (void)setting_id;
    if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    return;
#endif
    if (ImGui::IsItemHovered()) {
        auto cur = GUI::get().stored_bind(setting_id);
        if (tip) ImGui::SetTooltip("%s\nRight-click to rebind (%s)", tip, cur.c_str());
        else ImGui::SetTooltip("Right-click to rebind (%s)", cur.c_str());
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        GUI::get().prompt = setting_id;
        GUI::get().prompt_nested = ImGui::GetCurrentContext()->BeginPopupStack.Size > 0;
    }
}

const char* const GUI::BIND_POPUP = "Confirm Keybind###Confirm Keybind";

void GUI::show_keybind_prompt(bool nested) {
    if (GUI::get().prompt.empty() || GUI::get().prompt_nested != nested) return;
    if (GUI::get().prompt_shown != GUI::get().prompt) {
        GUI::get().prompt_shown = GUI::get().prompt;
        GUI::get().pressed.reset();
        GUI::get().bind_ctrl = false;
        GUI::get().bind_shift = false;
        GUI::get().bind_alt = false;
#ifdef GEODE_IS_MACOS
        GUI::get().bind_super = false;
#endif
    }
    if (!ImGui::IsPopupOpen(BIND_POPUP)) ImGui::OpenPopup(BIND_POPUP);
    if (!EchoUI::BeginModal(BIND_POPUP, 18.f, false)) return;
#ifdef GEODE_IS_MACOS
    GUI::get().prompt_drawn_frame = ImGui::GetFrameCount();
#endif

    std::string label = GUI::get().prompt;
    for (const auto& action : LISTED) {
        if (GUI::get().prompt == action.setting) label = action.label;
    }
    auto existing = GUI::get().stored_bind(GUI::get().prompt.c_str());
    if (existing != "None") ImGui::Text("Existing keybind: %s", existing.c_str());
    ImGui::Text("Press a key for '%s'", label.c_str());

#ifndef GEODE_IS_MACOS
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        auto imgui_key = (ImGuiKey)key;
        if (imgui_key >= ImGuiKey_MouseLeft) continue;
        if (GUI::get().is_modifier(imgui_key)) {
            if (GUI::get().pressed || !ImGui::IsKeyReleased(imgui_key)) continue;
            auto mapped = GUI::get().cocos_key(imgui_key);
            if (!mapped) continue;
            auto own = GUI::get().modifier_for_key(*mapped);
            GUI::get().pressed = mapped;
            GUI::get().bind_ctrl = (bool)(own & KeyboardModifier::Control);
            GUI::get().bind_shift = (bool)(own & KeyboardModifier::Shift);
            GUI::get().bind_alt = (bool)(own & KeyboardModifier::Alt);
            break;
        }
        if (!ImGui::IsKeyPressed(imgui_key, false)) continue;
        auto mapped = GUI::get().cocos_key(imgui_key);
        if (!mapped) continue;
        GUI::get().pressed = mapped;
        GUI::get().bind_ctrl = ImGui::GetIO().KeyCtrl;
        GUI::get().bind_shift = ImGui::GetIO().KeyShift;
        GUI::get().bind_alt = ImGui::GetIO().KeyAlt;
        break;
    }
#endif

    float bind_w = GUI::get().shared_button_width({ "Save", "Remove" });
    if (GUI::get().pressed) {
        auto own = GUI::get().modifier_for_key(*GUI::get().pressed);
        std::string pressed = "Pressed key: " + GUI::get().key_name(*GUI::get().pressed);
        if (GUI::get().bind_ctrl && !(own & KeyboardModifier::Control)) pressed += " + Ctrl";
        if (GUI::get().bind_shift && !(own & KeyboardModifier::Shift)) pressed += " + Shift";
        if (GUI::get().bind_alt && !(own & KeyboardModifier::Alt)) pressed += " + Alt";
#ifdef GEODE_IS_MACOS
        if (GUI::get().bind_super && !(own & KeyboardModifier::Super)) pressed += " + Cmd";
#endif
        ImGui::TextUnformatted(pressed.c_str());
        if (EchoUI::Button("Save", ImVec2(bind_w, 0.f))) {
            GUI::get().store_bind(GUI::get().prompt.c_str(), *GUI::get().pressed, GUI::get().bind_ctrl, GUI::get().bind_shift, GUI::get().bind_alt);
            GUI::get().prompt.clear();
            GUI::get().prompt_shown.clear();
            GUI::get().prompt_nested = false;
            ImGui::CloseCurrentPopup();
        }
    }
    else {
        ImGui::BeginDisabled();
        EchoUI::Button("Save", ImVec2(bind_w, 0.f));
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (existing != "None") {
        if (EchoUI::Button("Remove", ImVec2(bind_w, 0.f))) {
            GUI::get().store_bind(GUI::get().prompt.c_str(), KEY_None, false, false, false);
            GUI::get().prompt.clear();
            GUI::get().prompt_shown.clear();
            GUI::get().prompt_nested = false;
            ImGui::CloseCurrentPopup();
        }
    }
    else {
        ImGui::BeginDisabled();
        EchoUI::Button("Remove", ImVec2(bind_w, 0.f));
        ImGui::EndDisabled();
    }
    if (EchoUI::Button("Exit", ImVec2(GUI::get().shared_button_width({ "Exit" }), 0.f))) {
        GUI::get().prompt.clear();
        GUI::get().prompt_shown.clear();
        GUI::get().prompt_nested = false;
        ImGui::CloseCurrentPopup();
    }
    EchoUI::EndModal();
}

$execute {
    static ListenerHandle menu_drag = MouseMoveEvent().listen([](int, int) {
        auto& imgui = ImGuiCocos::get();
        if (!imgui.isInitialized() || !imgui.isVisible()) return false;
        return ImGui::GetIO().WantCaptureMouse || ImGui::IsAnyItemActive();
    });
#ifdef GEODE_IS_MACOS
    static ListenerHandle wheel = ScrollWheelEvent().listen([](double x, double y) {
        auto& imgui = ImGuiCocos::get();
        if (!imgui.isInitialized() || !imgui.isVisible()) return false;
        auto& io = ImGui::GetIO();
        if (!io.WantCaptureMouse) return false;
        double scale = scroll_is_precise() ? .1 : 1.;
        io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        io.AddMouseWheelEvent((float)(x * scale), (float)(y * scale));
        return true;
    });
#endif
    static ListenerHandle right_click = MouseInputEvent().listen([](MouseInputData& data) {
        if (data.button != MouseInputData::Button::Right) return false;
        auto& imgui = ImGuiCocos::get();
        if (!imgui.isInitialized() || !imgui.isVisible()) return false;
        ImGui::GetIO().AddMouseButtonEvent(1, data.action == MouseInputData::Action::Press);
        return ImGui::GetIO().WantCaptureMouse;
    });
}

ReplayPicker& GUI::picker() {
    return GUI::get().replay_picker;
}

std::string GUI::lower_copy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return text;
}

ShownReplay GUI::shown_replay(const fs::path& path) {
    std::string file = geode::utils::string::pathToString(path.filename());
    std::string lower = GUI::get().lower_copy(file);
    auto cut = [&](const char* ext) {
        size_t n = std::char_traits<char>::length(ext);
        if (lower.size() < n || lower.compare(lower.size() - n, n, ext) != 0) return false;
        file.resize(file.size() - n);
        lower.resize(lower.size() - n);
        return true;
    };
    const auto extensions = { ".echo.json", ".json", ".echo", ".txt", ".slc", ".tcm", ".gdr" };
    auto it = std::ranges::find_if(extensions, cut);
    std::string ext = it == extensions.end() ? std::string() : std::string(*it);
    bool autosave = ext == ".echo" && cut(".autosave");
    if (file.empty()) file = geode::utils::string::pathToString(path.stem());
    std::string tag = autosave ? "(autosave)" : ext.empty() || ext == ".echo" ? std::string()
                                                                              : "(" + ext + ")";
    if (path.parent_path().filename() == "converted")
        tag = tag.empty() ? "(converted)" : "(converted) " + tag;
    return { file, tag };
}

void GUI::refresh_files() {
    auto& state = GUI::get().picker();
    state.files.clear();
    auto take = [&](const fs::path& dir) {
        std::error_code error;
        if (!fs::exists(dir, error)) return;
        for (const auto& entry : fs::directory_iterator(dir, error)) {
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
    std::sort(state.files.begin(), state.files.end(), [](const fs::path& a, const fs::path& b) {
        return GUI::get().lower_copy(geode::utils::string::pathToString(a.filename())) < GUI::get().lower_copy(geode::utils::string::pathToString(b.filename()));
    });
}

void GUI::open_macro_picker(bool sequence, int amount) {
    GUI::get().reveal_menu();
    auto& state = GUI::get().picker();
    state.sequence = sequence;
    state.merge = false;
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

void GUI::open_merge_picker() {
    GUI::get().open_macro_picker(false, 1);
    GUI::get().picker().merge = true;
}

static void focus_search_on_open() {
#ifndef GEODE_IS_MOBILE
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
#endif
}

void GUI::draw_macro_picker() {
    auto& state = GUI::get().picker();
    static int drawn_frame = -1;
    if (drawn_frame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Macros###echo-macro-picker");
    float picker_scale = ImGui::GetFontSize() / 18.f, picker_wide = 400.f * picker_scale;
    if (!ImGui::BeginPopup("Echo Macros###echo-macro-picker", ImGuiWindowFlags_AlwaysAutoResize))
        return;
    drawn_frame = ImGui::GetFrameCount();
    state.want_open = false;
    bool tabs = !state.sequence && !state.merge;
    if (!tabs) state.browser.online = false;
    const float online_wide = 660.f;
    if (state.browser.online) picker_wide = online_wide * picker_scale;
    if (EchoUI::TitleBar(state.sequence ? "Add to the Sequence" : state.merge ? "Merge a Replay In"
                                                                           : "Load a Replay",
            EchoUI::Icon::Macro, true, picker_wide))
        ImGui::CloseCurrentPopup();
    if (tabs) GUI::get().draw_source_tabs(state.browser, picker_wide);
    ImGui::SetNextItemWidth(picker_wide);
    focus_search_on_open();
    if (EchoUI::InputTextWithHint("##macro-filter", state.browser.online ? "Level name, ID, creator or uploader" : "Type to filter macros", &state.filter))
        GUI::get().note_filter_edit(state.browser);
    if (state.browser.online) {
        GUI::get().draw_online_picker(state.browser, (int)OnlineKind::Macros, state.filter, picker_wide, 240.f * picker_scale + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
        ImGui::EndPopup();
        return;
    }
    ImGui::BeginChild("##macro-list", ImVec2(picker_wide, 240.f * picker_scale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (const auto& path : state.files) {
        auto shown_name = GUI::get().shown_replay(path);
        std::string blob = GUI::get().lower_copy(shown_name.name + " " + shown_name.tag + " " + geode::utils::string::pathToString(path.filename()));
        if (!needle.empty() && blob.find(needle) == std::string::npos) continue;
        ++shown;
        ImVec2 row = ImGui::GetCursorScreenPos();
        float row_h = ImGui::GetTextLineHeight();
        std::string row_id = "##replay-" + geode::utils::string::pathToString(path);
        bool picked = EchoUI::Selectable(row_id.c_str(), false, 0, ImVec2(0.f, row_h));
        auto* draw = ImGui::GetWindowDrawList();
        float text_x = row.x + ImGui::GetStyle().FramePadding.x;
        draw->AddText(ImVec2(text_x, row.y), ImGui::GetColorU32(ImGuiCol_Text), shown_name.name.c_str());
        if (!shown_name.tag.empty()) {
            float tag_x = text_x + ImGui::CalcTextSize(shown_name.name.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
            draw->AddText(ImVec2(tag_x, row.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), shown_name.tag.c_str());
        }
        if (picked) {
            if (state.sequence) Logic::get().append_sequence_file(path, state.amount);
            else if (state.merge) Logic::get().merge_replay(path);
            else Logic::get().read_file(path);
            ImGui::CloseCurrentPopup();
        }
    }
    if (shown == 0) ImGui::TextDisabled("No macros match");
    ImGui::EndChild();
    if (EchoUI::Button("Refresh", ImVec2(picker_wide, 0.f))) GUI::get().refresh_files();
    ImGui::EndPopup();
}

static std::string replay_tags(const OnlineReplay& replay) {
    std::string tags;
    if (replay.verified) tags += " Verified";
    else if (replay.claims_completion && ReplayDb::get().moderator()) tags += " Verify Replay";
    if (replay.physics == "2.1") tags += " 2.1";
    return tags;
}

static OnlineBrowser* g_browser = nullptr;
static OnlineKind g_kind = OnlineKind::Macros;

static OnlineKind online_kind() {
    return g_kind;
}

static int shown_count() {
    auto& db = ReplayDb::get();
    if (online_kind() == OnlineKind::Clickpacks) return (int)db.clickpacks.size();
    if (online_kind() == OnlineKind::Themes) return (int)db.themes.size();
    return (int)db.results.size();
}

static bool chosen_ok() {
    int chosen = g_browser->chosen;
    return chosen >= 0 && chosen < shown_count();
}

static void draw_online_status(float wide) {
    auto& db = ReplayDb::get();
    if (db.status.empty()) return;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wide);
    if (db.status_bad) ImGui::TextColored(ImVec4(1.f, .36f, .48f, 1.f), "%s", db.status.c_str());
    else ImGui::TextDisabled("%s", db.status.c_str());
    ImGui::PopTextWrapPos();
}

static void draw_level_check_modal() {
    const char* const title = "Check the Level";
    const float modal_width = 20.f;
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    if (std::exchange(state.confirm_level, false)) ImGui::OpenPopup(title);
    if (!EchoUI::BeginModal(title, modal_width)) return;
    std::string level_name;
    int level_id = 0;
    db.open_level(level_name, level_id);
    float wide = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wide);
    ImGui::Text("The replay is called \"%s\" but the open level is \"%s\".", Logic::get().macro_name.c_str(), level_name.c_str());
    if (level_id > 0) ImGui::Text("Is this replay for %s (ID %d)?", level_name.c_str(), level_id);
    else ImGui::Text("Is this replay for %s?", level_name.c_str());
    ImGui::TextDisabled("The upload is listed under whatever level is open right now.");
    ImGui::PopTextWrapPos();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    if (EchoUI::Button("Yes, upload", ImVec2(half, 0.f))) {
        db.upload(state.upload_title, state.upload_notes, state.upload_claims);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EchoUI::Button("No", ImVec2(half, 0.f))) ImGui::CloseCurrentPopup();
    EchoUI::EndModal();
}

static void draw_upload_buttons(float wide, const std::function<void()>& upload) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    ImGui::BeginDisabled(db.uploading());
    if (EchoUI::Button(db.uploading() ? "Uploading..." : "Upload", ImVec2(half, 0.f))) upload();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EchoUI::Button("Back", ImVec2(half, 0.f))) state.upload_open = false;
    draw_online_status(wide);
}

static void draw_upload_page(float wide, float scale) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    if (online_kind() == OnlineKind::Clickpacks) {
        EchoUI::SectionLabel("Upload a clickpack");
        auto installed = db.installed_clickpacks();
        if (installed.empty()) {
            ImGui::TextDisabled("You have no clickpacks installed");
            draw_upload_buttons(wide, [] {});
            return;
        }
        state.upload_pack = std::clamp(state.upload_pack, 0, (int)installed.size() - 1);
        ImGui::SetNextItemWidth(wide);
        if (EchoUI::BeginCombo("##upload-pack", installed[state.upload_pack].c_str())) {
            for (int at = 0; at < (int)installed.size(); ++at) {
                if (EchoUI::Selectable(installed[at].c_str(), at == state.upload_pack)) state.upload_pack = at;
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(wide);
        EchoUI::InputTextWithHint("##upload-pack-name", "Name (defaults to the folder name)", &state.upload_title);
        EchoUI::InputTextMultiline("##upload-pack-notes", &state.upload_notes, ImVec2(wide, 54.f * scale));
        draw_upload_buttons(wide, [&] { db.upload_clickpack(installed[state.upload_pack], state.upload_title, state.upload_notes); });
        return;
    }
    if (online_kind() == OnlineKind::Themes) {
        EchoUI::SectionLabel("Upload current theme");
        ImGui::SetNextItemWidth(wide);
        EchoUI::InputTextWithHint("##upload-theme-name", "Name (defaults to the theme name)", &state.upload_title);
        EchoUI::InputTextMultiline("##upload-theme-notes", &state.upload_notes, ImVec2(wide, 70.f * scale));
        draw_upload_buttons(wide, [&] { db.upload_theme(state.upload_title, state.upload_notes); });
        return;
    }
    EchoUI::SectionLabel("Upload current replay");
    ImGui::SetNextItemWidth(wide);
    EchoUI::InputTextWithHint("##upload-title", "Title (defaults to the level name)", &state.upload_title);
    EchoUI::InputTextMultiline("##upload-notes", &state.upload_notes, ImVec2(wide, 70.f * scale));
    EchoUI::Checkbox("This replay beats the level", &state.upload_claims);
    draw_upload_buttons(wide, [&] {
        std::string level_name;
        int level_id = 0;
        bool level_open = db.open_level(level_name, level_id);
        if (level_open && !ReplayDb::names_match(Logic::get().macro_name, level_name)) state.confirm_level = true;
        else db.upload(state.upload_title, state.upload_notes, state.upload_claims);
    });
    draw_level_check_modal();
}

static void draw_swatches(ImDrawList* draw, ImVec2 from, float size, const std::vector<std::array<float, 4>>& swatches) {
    float gap = size * .25f;
    for (size_t at = 0; at < swatches.size(); ++at) {
        const auto& color = swatches[at];
        ImVec2 corner(from.x + (float)at * (size + gap), from.y);
        draw->AddRectFilled(corner, ImVec2(corner.x + size, corner.y + size), ImGui::GetColorU32(ImVec4(color[0], color[1], color[2], 1.f)), size * .25f);
        draw->AddRect(corner, ImVec2(corner.x + size, corner.y + size), ImGui::GetColorU32(ImGuiCol_Border), size * .25f);
    }
}

static void draw_online_row(int at, float line) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    ImVec2 row = ImGui::GetCursorScreenPos();
    ImGui::PushID(at);
    if (EchoUI::Selectable("##online-row", state.chosen == at, 0, ImVec2(0.f, line * 2.f + 2.f))) state.chosen = at;
    ImGui::PopID();
    auto* draw = ImGui::GetWindowDrawList();
    float text_x = row.x + ImGui::GetStyle().FramePadding.x;
    std::string title, detail;
    if (online_kind() == OnlineKind::Macros) {
        const auto& replay = db.results[at];
        title = replay.title;
        detail = fmt::format("{} · {} · {:g} TPS", replay.level_name, replay.uploader, replay.tps);
        auto tags = replay_tags(replay);
        if (!tags.empty()) {
            float tag_x = text_x + ImGui::CalcTextSize(title.c_str()).x;
            ImVec4 tint = replay.verified ? ImVec4(.24f, .81f, .56f, 1.f) : ImVec4(.95f, .70f, .28f, 1.f);
            draw->AddText(ImVec2(tag_x, row.y), ImGui::GetColorU32(tint), tags.c_str());
        }
    } else if (online_kind() == OnlineKind::Clickpacks) {
        const auto& pack = db.clickpacks[at];
        title = pack.name;
        detail = pack.uploader;
    } else {
        const auto& theme = db.themes[at];
        title = theme.name;
        detail = theme.uploader;
        float size = line * .8f;
        float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetStyle().FramePadding.x;
        float width = (float)theme.swatches.size() * size * 1.25f - size * .25f;
        draw_swatches(draw, ImVec2(right - width, row.y + line * .6f), size, theme.swatches);
    }
    draw->AddText(ImVec2(text_x, row.y), ImGui::GetColorU32(ImGuiCol_Text), title.c_str());
    draw->AddText(ImVec2(text_x, row.y + line + 2.f), ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
}

static void draw_online_list(float wide, float height) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    float top = 0.f;
    if (online_kind() == OnlineKind::Macros) {
        if (EchoUI::Checkbox("Verified only", &state.verified_only)) state.online_stale = true;
        top = ImGui::GetFrameHeightWithSpacing();
    }
    float more = db.total > shown_count() ? ImGui::GetTextLineHeightWithSpacing() : 0.f;
    float list_height = height - top - more;
    ImGui::BeginChild("##online-list", ImVec2(wide, list_height), ImGuiChildFlags_Borders);
    float line = ImGui::GetTextLineHeight();
    for (int at = 0; at < shown_count(); ++at) draw_online_row(at, line);
    const char* const nothing[] = { "No online replays match", "No clickpacks match", "No themes match" };
    if (db.searching() && shown_count() == 0) ImGui::TextDisabled("Searching...");
    else if (db.searched && shown_count() == 0) ImGui::TextDisabled("%s", nothing[(int)online_kind()]);
    ImGui::EndChild();
    if (more > 0.f) ImGui::TextDisabled("%d more, type more to narrow it down", db.total - shown_count());
}

static void draw_chosen_replay(float wide) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    bool picked = chosen_ok();
    if (!picked) {
        ImGui::TextDisabled("Pick a replay from the list");
        ImGui::TextDisabled(" ");
        ImGui::TextDisabled(" ");
    } else {
        const auto& replay = db.results[state.chosen];
        ImGui::TextUnformatted(replay.title.c_str());
        if (!replay.notes.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", replay.notes.c_str());
        ImGui::TextDisabled("%s · by %s", replay.level_name.c_str(), replay.uploader.c_str());
        ImGui::TextDisabled("%g TPS · %d inputs · %d downloads", replay.tps, replay.inputs, replay.downloads);
    }
    ImGui::BeginDisabled(!picked || db.downloading());
    if (EchoUI::Button("Play Replay", ImVec2(half, 0.f))) {
        db.play(db.results[state.chosen]);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (EchoUI::Button("Save to library", ImVec2(half, 0.f))) db.save(db.results[state.chosen]);
    float third = (wide - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
    ImGui::BeginDisabled(!picked || !db.can_go_to_level(db.results[state.chosen]));
    if (EchoUI::Button("Go to level", ImVec2(third, 0.f))) {
        db.go_to_level(db.results[state.chosen]);
        ImGui::CloseCurrentPopup();
        GUI::get().begin_close();
    }
    ImGui::EndDisabled();
    if (picked && (PlayLayer::get() || LevelEditorLayer::get()) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Leave the level first");
    ImGui::SameLine();
    ImGui::BeginDisabled(!picked || db.results[state.chosen].level_id <= 0);
    if (EchoUI::Button("Copy ID", ImVec2(third, 0.f))) db.copy_level_id(db.results[state.chosen]);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EchoUI::Button("Website", ImVec2(third, 0.f))) {
        if (picked) db.open_on_website(db.results[state.chosen]);
        else db.open_website();
    }
}

static void draw_chosen_clickpack(float wide) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    bool picked = chosen_ok();
    if (!picked) {
        ImGui::TextDisabled("Pick a clickpack from the list");
        ImGui::TextDisabled(" ");
        ImGui::TextDisabled(" ");
    } else {
        const auto& pack = db.clickpacks[state.chosen];
        ImGui::TextUnformatted(pack.name.c_str());
        if (!pack.notes.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pack.notes.c_str());
        ImGui::TextDisabled("by %s · %d downloads", pack.uploader.c_str(), pack.downloads);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wide);
        ImGui::TextDisabled("%s", pack.types.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!picked);
    if (EchoUI::Button("Sample clickpack", ImVec2(half, 0.f))) db.sample_clickpack(db.clickpacks[state.chosen]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EchoUI::Button("Website##pack", ImVec2(half, 0.f))) db.open_page(picked ? "/c/" + db.clickpacks[state.chosen].id : std::string("/clickpacks"));
    const float most_volume = 2.f;
    float volume = db.sample_volume();
    const char* const volume_label = "Sample volume##sample-volume";
    ImGui::SetNextItemWidth(wide - ImGui::CalcTextSize(volume_label, nullptr, true).x - ImGui::GetStyle().ItemInnerSpacing.x);
    if (EchoUI::Slider(volume_label, &volume, 0.f, most_volume, "%.2fx")) db.set_sample_volume(volume);
    int player = GUI::get().clickpacks().player + 1;
    ImGui::BeginDisabled(!picked || db.downloading());
    if (EchoUI::Button("Install", ImVec2(half, 0.f))) db.install_clickpack(db.clickpacks[state.chosen], 0);
    ImGui::SameLine();
    if (EchoUI::Button("Install & use", ImVec2(half, 0.f))) db.install_clickpack(db.clickpacks[state.chosen], player);
    ImGui::EndDisabled();
}

static void draw_chosen_theme(float wide) {
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    bool picked = chosen_ok();
    float line = ImGui::GetTextLineHeight();
    if (!picked) {
        ImGui::TextDisabled("Pick a theme from the list");
        ImGui::TextDisabled(" ");
        ImGui::Dummy(ImVec2(wide, line * 1.4f));
    } else {
        const auto& theme = db.themes[state.chosen];
        ImGui::TextUnformatted(theme.name.c_str());
        if (!theme.notes.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", theme.notes.c_str());
        ImGui::TextDisabled("by %s · %d downloads", theme.uploader.c_str(), theme.downloads);
        float size = line * 1.4f;
        draw_swatches(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), size, theme.swatches);
        ImGui::Dummy(ImVec2(wide, size));
    }
    ImGui::BeginDisabled(!picked || db.downloading());
    if (EchoUI::Button("Apply theme", ImVec2(half, 0.f))) db.apply_theme(db.themes[state.chosen]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EchoUI::Button("Website##theme", ImVec2(half, 0.f))) db.open_page(picked ? "/t/" + db.themes[state.chosen].id : std::string("/themes"));
}

static void draw_web_code_modal() {
    const char* const title = "Website Sign-In";
    const float code_scale = 1.8f, modal_width = 18.f;
    auto& db = ReplayDb::get();
    if (std::exchange(db.web_code_ready, false)) ImGui::OpenPopup(title);
    if (!EchoUI::BeginModal(title, modal_width)) return;
    float wide = ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("Your code is copied to the clipboard");
    ImGui::SetWindowFontScale(code_scale);
    float code_width = ImGui::CalcTextSize(db.web_code.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, (wide - code_width) * .5f));
    ImGui::TextUnformatted(db.web_code.c_str());
    ImGui::SetWindowFontScale(1.f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wide);
    ImGui::TextDisabled("Paste it on the website's sign-in page within 5 minutes.");
    ImGui::PopTextWrapPos();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    if (EchoUI::Button("Open sign-in page", ImVec2(half, 0.f))) db.open_sign_in_page();
    ImGui::SameLine();
    if (EchoUI::Button("Done", ImVec2(half, 0.f))) ImGui::CloseCurrentPopup();
    EchoUI::EndModal();
}

static void draw_online_account(float wide) {
    const char* const upload_labels[] = { "Upload current replay", "Upload a clickpack", "Upload current theme" };
    auto& state = *g_browser;
    auto& db = ReplayDb::get();
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    EchoUI::Separator();
    if (!db.signed_in()) {
        ImGui::BeginDisabled(db.signing_in());
        if (EchoUI::Button(db.signing_in() ? "Checking your GD account..." : "Sign in with GD account", ImVec2(wide, 0.f))) db.sign_in();
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("Signed in as %s", db.username().c_str());
        if (EchoUI::Button(upload_labels[(int)online_kind()], ImVec2(wide, 0.f))) {
            state.upload_open = true;
            state.upload_title.clear();
            state.upload_notes.clear();
            if (online_kind() == OnlineKind::Macros) state.upload_title = Logic::get().macro_name;
            if (online_kind() == OnlineKind::Themes) state.upload_title = Logic::get().theme_name;
        }
        if (EchoUI::Button("Website sign-in", ImVec2(half, 0.f))) db.request_web_code();
        ImGui::SameLine();
        if (EchoUI::Button("Sign out", ImVec2(half, 0.f))) {
            db.sign_out();
            state.upload_open = false;
        }
    }
    draw_online_status(wide);
    draw_web_code_modal();
}

void GUI::draw_source_tabs(OnlineBrowser& browser, float wide) {
    float half = (wide - ImGui::GetStyle().ItemSpacing.x) * .5f;
    float label_wide = half - EchoUI::nav_icon_width();
    if (EchoUI::NavItem("Local", EchoUI::Icon::Macro, !browser.online, label_wide)) browser.online = false;
    ImGui::SameLine();
    if (EchoUI::NavItem("Online", EchoUI::Icon::Sequence, browser.online, label_wide)) {
        if (!browser.online) browser.online_stale = true;
        browser.online = true;
    }
}

void GUI::note_filter_edit(OnlineBrowser& browser) {
    browser.filter_changed_at = ImGui::GetTime();
    browser.online_stale = true;
}

void GUI::draw_online_picker(OnlineBrowser& browser, int kind, std::string& filter, float wide, float height) {
    const double search_delay = 0.35;
    const float list_share = .46f;
    if (g_browser != &browser || g_kind != (OnlineKind)kind) {
        browser.online_stale = true;
        browser.chosen = -1;
    }
    g_browser = &browser;
    g_kind = (OnlineKind)kind;
    auto& state = browser;
    auto& db = ReplayDb::get();
    if (std::exchange(db.uploaded, false)) {
        state.upload_open = false;
        state.online_stale = true;
        filter.clear();
    }
    if (state.online_stale && !db.searching() && ImGui::GetTime() - state.filter_changed_at > search_delay) {
        state.online_stale = false;
        state.chosen = -1;
        db.search(online_kind(), filter, state.verified_only);
    }
    float gap = ImGui::GetStyle().ItemSpacing.x * 2.f;
    float left = (wide - gap) * list_share, right = wide - gap - left;
    ImGui::BeginGroup();
    draw_online_list(left, height);
    ImGui::EndGroup();
    ImGui::SameLine(0.f, gap);
    ImGui::BeginChild("##online-side", ImVec2(right, height));
    if (state.upload_open && db.signed_in()) draw_upload_page(right, ImGui::GetFontSize() / 18.f);
    else {
        if (online_kind() == OnlineKind::Clickpacks) draw_chosen_clickpack(right);
        else if (online_kind() == OnlineKind::Themes) draw_chosen_theme(right);
        else draw_chosen_replay(right);
        draw_online_account(right);
    }
    ImGui::EndChild();
}

ThemePicker& GUI::themes() {
    return GUI::get().theme_picker;
}

void GUI::refresh_themes() {
    auto& state = GUI::get().themes();
    state.files.clear();
    auto dir = Logic::get().save_root() / "themes";
    std::error_code error;
    if (!fs::exists(dir, error)) return;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file()) continue;
        auto name = GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().filename()));
        if (!name.ends_with(".ui.json") && !name.ends_with(".ui")) continue;
        if (name == "current.ui.json") continue;
        state.files.push_back(entry.path());
    }
    std::sort(state.files.begin(), state.files.end(), [](const fs::path& a, const fs::path& b) {
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
    static int drawn_frame = -1;
    if (drawn_frame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Themes###echo-theme-picker");
    const float online_wide = 660.f;
    float picker_scale = ImGui::GetFontSize() / 18.f, picker_wide = (state.browser.online ? online_wide : 400.f) * picker_scale;
    if (!ImGui::BeginPopup("Echo Themes###echo-theme-picker", ImGuiWindowFlags_AlwaysAutoResize))
        return;
    drawn_frame = ImGui::GetFrameCount();
    state.want_open = false;
    if (EchoUI::TitleBar("Load a Theme", EchoUI::Icon::Theme, true, picker_wide))
        ImGui::CloseCurrentPopup();
    GUI::get().draw_source_tabs(state.browser, picker_wide);
    ImGui::SetNextItemWidth(picker_wide);
    focus_search_on_open();
    if (EchoUI::InputTextWithHint("##theme-filter", state.browser.online ? "Search themes or uploaders" : "Type to filter themes", &state.filter))
        GUI::get().note_filter_edit(state.browser);
    if (state.browser.online) {
        GUI::get().draw_online_picker(state.browser, (int)OnlineKind::Themes, state.filter, picker_wide, 240.f * picker_scale + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);
        ImGui::EndPopup();
        return;
    }
    ImGui::BeginChild("##theme-list", ImVec2(picker_wide, 240.f * picker_scale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (const auto& path : state.files) {
        std::string name = geode::utils::string::pathToString(path.filename());
        if (name.ends_with(".ui.json")) name.resize(name.size() - 8);
        else if (name.ends_with(".ui")) name.resize(name.size() - 3);
        if (!needle.empty() && GUI::get().lower_copy(name).find(needle) == std::string::npos)
            continue;
        ++shown;
        if (EchoUI::Selectable(name.c_str(), false)) {
            GUI::get().import_theme(path);
            ImGui::CloseCurrentPopup();
        }
    }
    if (shown == 0) ImGui::TextDisabled("No themes match");
    ImGui::EndChild();
    if (EchoUI::Button("Refresh", ImVec2(picker_wide, 0.f))) GUI::get().refresh_themes();
    ImGui::EndPopup();
}

ClickpackPicker& GUI::clickpacks() {
    return GUI::get().clickpack_picker;
}

RenderPicker& GUI::renders() {
    return GUI::get().render_picker;
}

void GUI::refresh_renders() {
    auto& state = GUI::get().renders();
    state.files.clear();
    auto dir = Logic::get().save_root() / "renders";
    std::error_code error;
    fs::create_directories(dir, error);
    if (!fs::exists(dir, error)) return;
    std::string extra = GUI::get().lower_copy(Logic::get().render_extension);
    if (!extra.empty() && extra.front() != '.') extra.insert(extra.begin(), '.');
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file()) continue;
        auto name = GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().filename()));
        bool video = name.ends_with(".mp4") || name.ends_with(".mkv") || name.ends_with(".mov") || name.ends_with(".webm");
        if (!video && !extra.empty() && extra != ".") video = name.ends_with(extra);
        if (!video) continue;
        state.files.push_back(entry.path());
    }
    std::sort(state.files.begin(), state.files.end(), [](const fs::path& a, const fs::path& b) {
        return GUI::get().lower_copy(geode::utils::string::pathToString(a.filename())) < GUI::get().lower_copy(geode::utils::string::pathToString(b.filename()));
    });
}

void GUI::open_render_picker() {
    GUI::get().reveal_menu();
    auto& state = GUI::get().renders();
    state.second = state.confirm = false;
    state.first.clear();
    state.last.clear();
    state.filter.clear();
    state.want_open = true;
    GUI::get().refresh_renders();
}

void GUI::draw_render_picker() {
    auto& state = GUI::get().renders();
    static int drawn_frame = -1;
    if (drawn_frame == ImGui::GetFrameCount()) return;
    if (state.want_open) ImGui::OpenPopup("Echo Renders###echo-render-picker");
    float picker_scale = ImGui::GetFontSize() / 18.f, picker_wide = 400.f * picker_scale;
    if (!ImGui::BeginPopup("Echo Renders###echo-render-picker", ImGuiWindowFlags_AlwaysAutoResize))
        return;
    drawn_frame = ImGui::GetFrameCount();
    state.want_open = false;
    if (state.confirm) {
        if (EchoUI::TitleBar("Join These Videos?", EchoUI::Icon::Render, true, picker_wide))
            ImGui::CloseCurrentPopup();
        ImGui::TextUnformatted(geode::utils::string::pathToString(state.first.filename()).c_str());
        ImGui::TextUnformatted(geode::utils::string::pathToString(state.last.filename()).c_str());
        ImGui::TextDisabled("Uses your render codec and quality settings.");
        float half = (picker_wide - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (EchoUI::Button("Join", ImVec2(half, 0.f))) {
            Logic::get().merge_renders(state.first, state.last);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EchoUI::Button("Cancel", ImVec2(half, 0.f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (EchoUI::TitleBar(state.second ? "Pick the Second Video" : "Pick the First Video", EchoUI::Icon::Render, true, picker_wide))
        ImGui::CloseCurrentPopup();
    if (state.second)
        ImGui::TextDisabled("%s", geode::utils::string::pathToString(state.first.filename()).c_str());
    ImGui::SetNextItemWidth(picker_wide);
    focus_search_on_open();
    EchoUI::InputTextWithHint("##render-filter", "Type to filter videos", &state.filter);
    ImGui::BeginChild("##render-list", ImVec2(picker_wide, 240.f * picker_scale), ImGuiChildFlags_Borders);
    std::string needle = GUI::get().lower_copy(state.filter);
    int shown = 0;
    for (const auto& path : state.files) {
        std::string name = geode::utils::string::pathToString(path.filename());
        if (!needle.empty() && GUI::get().lower_copy(name).find(needle) == std::string::npos)
            continue;
        ++shown;
        std::string row_id = name + "##render-" + geode::utils::string::pathToString(path);
        if (!EchoUI::Selectable(row_id.c_str(), false)) continue;
        if (!state.second) {
            state.first = path;
            state.second = true;
            state.filter.clear();
            continue;
        }
        state.last = path;
        state.confirm = true;
    }
    if (shown == 0) ImGui::TextDisabled("No videos match");
    ImGui::EndChild();
    if (EchoUI::Button("Refresh", ImVec2(picker_wide, 0.f))) GUI::get().refresh_renders();
    ImGui::EndPopup();
}
