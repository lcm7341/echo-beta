// themes: loading & saving (.ui.json), the style editor, the keybind prompt & the file pickers
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

namespace fs = std::filesystem;

using namespace geode::prelude;

void GUI::help_marker(char const* text) {
    eui::Help();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
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
    log::debug("pair json");
    matjson::Value json = matjson::Value::array();
    json.push(value.x);
    json.push(value.y);
    return json;
}

void GUI::read_pair(matjson::Value const& json, char const* key, ImVec2& value) {
    if (!json[key].isArray() || json[key].size() < 2)
        return;
    if (auto x = json[key][0].asDouble()) {
        value.x = (float)(x.unwrap());
    }
    if (auto y = json[key][1].asDouble()) {
        value.y = (float)(y.unwrap());
    }
}

void GUI::read_color(matjson::Value const& json, char const* key, float* color) {
    if (!json[key].isArray() || json[key].size() < 4)
        return;
    for (int i = 0; i < 4; ++i) {
        if (auto channel = json[key][i].asDouble()) {
            color[i] = (float)(channel.unwrap());
        }
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
    json["rainbow_rim"] = eui::rainbow_rim();
    json["button_effects"] = eui::button_effects();
    json["scene"] = eui::scene_name(eui::scene());
    if (GUI::get().has_palette) {
        auto const& palette = GUI::get().palette;
        matjson::Value saved;
        saved["hue"] = palette.hue;
        saved["accent_hue"] = palette.accent_hue;
        saved["richness"] = palette.richness;
        saved["depth"] = palette.depth;
        saved["accent_saturation"] = palette.accent_saturation;
        saved["accent_value"] = palette.accent_value;
        saved["light"] = palette.light;
        saved["rainbow"] = palette.rainbow;
        saved["scene"] = eui::scene_name(palette.scene);
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

void GUI::apply_style_json(matjson::Value const& json) {
    GUI::get().restore_base_sizes();
    auto& style = ImGui::GetStyle();
    auto& logic = Logic::get();
    if (auto text = json["name"].asString()) {
        logic.theme_name = text.unwrap();
    }
    if (auto number = json["scale"].asDouble()) {
        logic.menu_scale = std::clamp((float)(number.unwrap()), 0.35f, 3.f);
    }
    eui::rainbow_rim() = json["rainbow_rim"].asBool().unwrapOr(false);
    if (json.contains("button_effects")) {
        eui::button_effects() = json["button_effects"].asBool().unwrapOr(true);
    }
    auto scene_in = [](matjson::Value const& from) {
        if (from["halloween"].asBool().unwrapOr(false)) {
            return eui::Scene::Halloween;
        }
        return eui::scene_named(from["scene"].asString().unwrapOr(""));
    };
    eui::scene() = scene_in(json);
    GUI::get().has_palette = json.contains("palette");
    if (GUI::get().has_palette) {
        auto const& saved = json["palette"];
        auto number = [&](char const* key, float fallback) {
            return (float)(saved[key].asDouble().unwrapOr(fallback));
        };
        eui::Palette palette;
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
        eui::sync_scene_turn(palette);
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
    auto number = [&](char const* key, float& field) {
        if (auto parsed = json[key].asDouble()) {
            field = (float)(parsed.unwrap());
        }
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
    else if (style.CircleTessellationMaxError > 0.f) {
        style.CircleTessellationMaxError = 0.1f;
    }
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    if (json["colors"].isArray()) {
        int amt = std::min(static_cast<int>(json["colors"].size()), static_cast<int>(ImGuiCol_COUNT));
        for (int i = 0; i < amt; ++i) {
            if (!json["colors"][i].isArray() || json["colors"][i].size() < 4) {
                continue;
            }
            ImVec4 color = style.Colors[i];
            if (auto channel = json["colors"][i][0].asDouble()) {
                color.x = (float)(channel.unwrap());
            }
            if (auto channel = json["colors"][i][1].asDouble()) {
                color.y = (float)(channel.unwrap());
            }
            if (auto channel = json["colors"][i][2].asDouble()) {
                color.z = (float)(channel.unwrap());
            }
            if (auto channel = json["colors"][i][3].asDouble()) {
                color.w = (float)(channel.unwrap());
            }
            style.Colors[i] = color;
        }
    }
    GUI::get().read_color(json, "editor_p1", logic.editor_p1);
    GUI::get().read_color(json, "editor_p2", logic.editor_p2);
    GUI::get().read_color(json, "editor_sel_p1", logic.editor_sel_p1);
    GUI::get().read_color(json, "editor_sel_p2", logic.editor_sel_p2);
    if (style.CurveTessellationTol < 0.10f) {
        style.CurveTessellationTol = 0.10f;
    }
    bool absolute = false;
    if (auto flag = json["metrics_absolute"].asBool()) {
        absolute = flag.unwrap();
    }
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

fs::path GUI::theme_path(std::string const& name) {
    auto safe = name.empty() ? std::string("Default") : name;
    for (char& glyph : safe) {
        if (glyph == '/' || glyph == '\\' || glyph == ':') {
            glyph = '_';
        }
    }
    return Logic::get().save_root() / "themes" / (safe + ".ui.json");
}

void GUI::export_theme(fs::path const& path) {
    log::debug("export theme");
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream file(path);
    if (!file)
        return;
    file << GUI::get().style_json().dump(2);
}

void GUI::note_style_edit() {
    if (ImGui::IsItemDeactivatedAfterEdit())
        GUI::get().export_theme(GUI::get().theme_path("current"));
}

bool edit_style_vec(char const* id, ImVec2& live, float min, float max) {
    struct Draft {
        ImVec2 value;
        bool drag = false;
    };

    static std::unordered_map<std::string, Draft> drafts;
    auto& draft = drafts[id];
    if (!draft.drag) {
        draft.value = live;
    }
    eui::SliderFloat2(id, &draft.value.x, min, max, "%.0f");
    if (ImGui::IsItemActivated()) {
        draft.drag = true;
    }
    if (!ImGui::IsItemDeactivated()) {
        return false;
    }
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
    if (!draft.drag) {
        draft.value = live;
    }
    eui::SliderFloat(id, &draft.value, min, max, "%.0f");
    if (ImGui::IsItemActivated()) {
        draft.drag = true;
    }
    if (!ImGui::IsItemDeactivated()) {
        return false;
    }
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
    auto const& colors = ImGui::GetStyle().Colors;
    ImVec4 base = colors[ImGuiCol_FrameBg], accent = colors[ImGuiCol_CheckMark];
    base.w = accent.w = 1.f;
    float h, sat, val;
    ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, h, sat, val);
    ImVec4 second = accent;
    if (sat < .08f) {
    }
    else {
        float turned = h + 50.f / 360.f;
        turned -= std::floor(turned);
        ImGui::ColorConvertHSVtoRGB(turned, sat, val, second.x, second.y, second.z);
    }
    auto blend = [&](ImVec4 const& tint, float amount) {
        return ImVec4(base.x + (tint.x - base.x) * amount, base.y + (tint.y - base.y) * amount, base.z + (tint.z - base.z) * amount, 1.f);
    };
    GUI::get().paint_editor(logic.editor_p1, blend(accent, .32f));
    GUI::get().paint_editor(logic.editor_p2, blend(second, .32f));
    GUI::get().paint_editor(logic.editor_sel_p1, blend(accent, .62f));
    GUI::get().paint_editor(logic.editor_sel_p2, blend(second, .62f));
}

void GUI::use_palette(eui::Palette const& palette, std::string const& name) {
    eui::apply_palette(palette);
    GUI::get().palette = palette;
    GUI::get().has_palette = true;
    auto& logic = Logic::get();
    GUI::get().paint_editor_from_theme();
    logic.theme_name = name;
    GUI::get().export_theme(GUI::get().theme_path("current"));
    Logic::get().save_settings_file();
}

void GUI::randomize_theme() {
    log::debug("randomize theme");
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    eui::Palette palette;
    palette.hue = unit(rng);
    float lean = unit(rng) < .35f ? 0.f : (unit(rng) - .5f) * (70.f / 360.f);
    palette.accent_hue = palette.hue + lean;
    palette.richness = .7f + unit(rng) * .5f;
    palette.depth = .85f + unit(rng) * .35f;
    palette.accent_saturation = .38f + unit(rng) * .34f;
    GUI::get().use_palette(palette, "Random");
}

void GUI::color_row(char const* label, float* color, int id) {
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
    constexpr int ECHO_STYLE = 2;
    auto& logic = Logic::get();
    if (logic.ui_style < ECHO_STYLE) {
        std::error_code error;
        auto kept = GUI::get().theme_path("Before Echo style");
        if (fs::exists(path, error) && !fs::exists(kept, error)) {
            fs::rename(path, kept, error);
        }
        logic.ui_style = ECHO_STYLE;
        logic.theme_name = "Echo";
        GUI::get().palette = eui::Palette{};
        GUI::get().has_palette = true;
        GUI::get().export_theme(path);
        Logic::get().save_settings_file();
        return;
    }
    if (!fs::exists(path))
        return;
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed)
        return;
    GUI::get().apply_style_json(parsed.unwrap());
}

// convert old
matjson::Value GUI::convert_old_theme(matjson::Value const& old, std::string const& name) {
    matjson::Value json;
    json["name"] = name;
    json["metrics_absolute"] = true;
    if (auto scale = old["GlobalScale"].asDouble()) {
        json["scale"] = scale.unwrap();
    }
    for (std::string key : { "WindowPadding", "FramePadding", "CellPadding", "ItemSpacing", "ItemInnerSpacing", "TouchExtraPadding",
             "WindowTitleAlign", "ButtonTextAlign", "SelectableTextAlign", "DisplaySafeAreaPadding" }) {
        auto x = old[key + "X"].asDouble();
        auto y = old[key + "Y"].asDouble();
        if (x) {
            if (y) {
                json[key] = GUI::get().pair_json(ImVec2((float)(x.unwrap()), (float)(y.unwrap())));
            }
        }
    }
    for (std::string key : { "IndentSpacing", "ScrollbarSize", "GrabMinSize", "WindowBorderSize", "ChildBorderSize", "PopupBorderSize",
             "FrameBorderSize", "TabBorderSize", "WindowRounding", "ChildRounding", "FrameRounding", "PopupRounding", "ScrollbarRounding",
             "GrabRounding", "TabRounding", "CurveTessellationTol", "CircleTessellationMaxError" }) {
        if (old.contains(key)) {
            json[key] = old[key];
        }
    }
    auto& style = ImGui::GetStyle();
    matjson::Value colors = matjson::Value::array();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        std::string key = ImGui::GetStyleColorName(i);
        if (i == ImGuiCol_TabSelected) {
            key = "TabActive";
        }
        if (i == ImGuiCol_TabDimmed) {
            key = "TabUnfocused";
        }
        if (i == ImGuiCol_TabDimmedSelected) {
            key = "TabUnfocusedActive";
        }
        if (i == ImGuiCol_NavCursor) {
            key = "NavHighlight";
        }
        if (old[key].isArray() && old[key].size() >= 4)
            colors.push(old[key]);
        else
            colors.push(GUI::get().color_json(style.Colors[i]));
    }
    json["colors"] = colors;
    for (auto [from, to] : { std::pair{ "player_1_button_color", "editor_p1" }, std::pair{ "player_2_button_color", "editor_p2" },
             std::pair{ "input_selected_color_p1", "editor_sel_p1" }, std::pair{ "input_selected_color_p2", "editor_sel_p2" } }) {
        if (old.contains(from)) {
            json[to] = old[from];
        }
    }
    return json;
}