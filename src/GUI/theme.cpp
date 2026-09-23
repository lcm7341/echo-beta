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
    // TODO
    return {};
}

void GUI::read_pair(matjson::Value const& json, char const* key, ImVec2& value) {
    if (!json[key].isArray() || json[key].size() < 2) return;
    if (auto x = json[key][0].asDouble()) value.x = static_cast<float>(x.unwrap());
    if (auto y = json[key][1].asDouble()) value.y = static_cast<float>(y.unwrap());
}

void GUI::read_color(matjson::Value const& json, char const* key, float* color) {
    // TODO
}

float GUI::unscaled(float value) {
    float scale = GUI::get().applied_scale;
    return scale > 0.f ? value / scale : value;
}

ImVec2 GUI::unscaled(ImVec2 value) {
    return ImVec2(GUI::get().unscaled(value.x), GUI::get().unscaled(value.y));
}

matjson::Value GUI::style_json() {
    // port this
    return {};
}

void GUI::apply_style_json(matjson::Value const& json) {
    // TODO
}

std::filesystem::path GUI::theme_path(std::string const& name) {
    // TODO
    return {};
}

void GUI::write_theme(std::filesystem::path const& path) {
    // TODO
}

void GUI::note_style_edit() {
    if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().write_theme(GUI::get().theme_path("current"));
}

bool edit_style_vec(char const* id, ImVec2& live, float min, float max) {
    // TODO
    return {};
}

bool edit_style_float(char const* id, float& live, float min, float max) {
    // TODO
    return {};
}

void GUI::paint_editor(float* dest, ImVec4 color) {
    // todo
}

void GUI::randomize_theme() {
    // TODO
}

void GUI::color_row(char const* label, float* color, int id) {
    // TODO
}

void GUI::apply_saved_theme() {
    // later
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
    // port this
    return {};
}

bool GUI::is_modifier(ImGuiKey key) {
    // TODO
    return {};
}

std::string GUI::key_name(cocos2d::enumKeyCodes key) {
    // todo
    return {};
}

KeyboardModifier GUI::modifier_for_key(cocos2d::enumKeyCodes key) {
    // TODO
    return {};
}

std::string GUI::stored_bind(char const* setting) {
    auto binds = Mod::get()->getSettingValue<std::vector<Keybind>>(setting);
    if (binds.empty() || binds[0].key == cocos2d::KEY_None) return "None";
    return binds[0].toString();
}

void GUI::store_bind(char const* setting, cocos2d::enumKeyCodes key, bool ctrl, bool shift, bool alt) {
    // port this
}

void GUI::flip_setting(bool& field, char const* setting) {
    field = !field;
    Logic::write_bool(setting, field);
    Logic::get().save_settings_file();
}

static void flip_utility(bool& field) {
    field = !field;
    Logic::get().save_settings_file();
}

bool Logic::dispatch_setting_key(std::string_view key, bool down) {
    // TODO
    return {};
}

void GUI::offer_rebind(char const* setting_id, char const* tip) {
    // TODO
}

void GUI::draw_keybind_prompt(bool nested) {
    // TODO
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
    // todo
    return {};
}

ShownReplay GUI::shown_replay(std::filesystem::path const& path) {
    // TODO
    return {};
}

void GUI::refresh_files() {
    // todo
}

void GUI::open_macro_picker(bool sequence, int amount) {
    // TODO
}
void GUI::draw_macro_picker() {
    // port this
}

ThemePicker& GUI::themes() {
    return GUI::get().theme_picker;
}

void GUI::refresh_themes() {
    // later
}

void GUI::open_theme_picker() {
    // later
}
void GUI::draw_theme_picker() {
    // todo
}

ClickpackPicker& GUI::clickpacks() {
    return GUI::get().clickpack_picker;
}

void GUI::refresh_clickpacks() {
    // TODO
}

void GUI::open_clickpack_picker() {
    // TODO
}
void GUI::draw_clickpack_picker() {
    // TODO
}

RenderPicker& GUI::renders() {
    return GUI::get().render_picker;
}

void GUI::refresh_renders() {
    // todo
}

void GUI::open_render_picker() {
    // TODO
}

void GUI::draw_render_picker() {
    // todo
}
