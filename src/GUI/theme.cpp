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

void GUI::helpMarker(char const* text) {
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

matjson::Value GUI::colorJson(ImVec4 const& color) {
	matjson::Value value = matjson::Value::array();
	value.push(color.x);
	value.push(color.y);
	value.push(color.z);
	value.push(color.w);
	return value;
}

matjson::Value GUI::pairJson(ImVec2 const& value) {
	matjson::Value json = matjson::Value::array();
	json.push(value.x);
	json.push(value.y);
	return json;
}

void GUI::readPair(matjson::Value const& json, char const* key, ImVec2& value) {
	if (!json[key].isArray() || json[key].size() < 2) return;
	if (auto x = json[key][0].asDouble()) value.x = static_cast<float>(x.unwrap());
	if (auto y = json[key][1].asDouble()) value.y = static_cast<float>(y.unwrap());
}

void GUI::readColor(matjson::Value const& json, char const* key, float* color) {
	if (!json[key].isArray() || json[key].size() < 4) return;
	for (int i = 0; i < 4; ++i) {
		if (auto channel = json[key][i].asDouble()) color[i] = static_cast<float>(channel.unwrap());
	}
}

float GUI::unscaled(float value) {
	float scale = GUI::get().appliedScale;
	return scale > 0.f ? value / scale : value;
}

ImVec2 GUI::unscaled(ImVec2 value) {
	return ImVec2(GUI::get().unscaled(value.x), GUI::get().unscaled(value.y));
}

matjson::Value GUI::styleJson() {
	auto& style = ImGui::GetStyle();
	auto& value = Logic::get();
	matjson::Value json;
	json["name"] = value.themeName;
	json["scale"] = value.menuScale;
	json["metrics_absolute"] = true;
	json["WindowPadding"] = GUI::get().pairJson(style.WindowPadding);
	json["FramePadding"] = GUI::get().pairJson(style.FramePadding);
	json["CellPadding"] = GUI::get().pairJson(style.CellPadding);
	json["ItemSpacing"] = GUI::get().pairJson(style.ItemSpacing);
	json["ItemInnerSpacing"] = GUI::get().pairJson(style.ItemInnerSpacing);
	json["TouchExtraPadding"] = GUI::get().pairJson(style.TouchExtraPadding);
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
	json["WindowTitleAlign"] = GUI::get().pairJson(style.WindowTitleAlign);
	json["ButtonTextAlign"] = GUI::get().pairJson(style.ButtonTextAlign);
	json["SelectableTextAlign"] = GUI::get().pairJson(style.SelectableTextAlign);
	json["DisplaySafeAreaPadding"] = GUI::get().pairJson(style.DisplaySafeAreaPadding);
	json["AntiAliasedLines"] = true;
	json["AntiAliasedLinesUseTex"] = true;
	json["AntiAliasedFill"] = true;
	json["CurveTessellationTol"] = style.CurveTessellationTol;
	json["CircleTessellationMaxError"] = style.CircleTessellationMaxError;
	json["SharpEdges"] = style.CircleTessellationMaxError >= 2.5f;
	json["colors"] = matjson::Value::array();
	for (int i = 0; i < ImGuiCol_COUNT; ++i) json["colors"].push(GUI::get().colorJson(style.Colors[i]));
	json["editor_p1"] = matjson::Value::array();
	json["editor_p2"] = matjson::Value::array();
	json["editor_sel_p1"] = matjson::Value::array();
	json["editor_sel_p2"] = matjson::Value::array();
	for (int i = 0; i < 4; ++i) {
		json["editor_p1"].push(value.editorP1[i]);
		json["editor_p2"].push(value.editorP2[i]);
		json["editor_sel_p1"].push(value.editorSelP1[i]);
		json["editor_sel_p2"].push(value.editorSelP2[i]);
	}
	return json;
}

void GUI::applyStyleJson(matjson::Value const& json) {
	auto& style = ImGui::GetStyle();
	auto& value = Logic::get();
	if (auto text = json["name"].asString()) value.themeName = text.unwrap();
	if (auto number = json["scale"].asDouble()) value.menuScale = std::clamp(static_cast<float>(number.unwrap()), 0.35f, 3.f);
	GUI::get().readPair(json, "WindowPadding", style.WindowPadding);
	GUI::get().readPair(json, "FramePadding", style.FramePadding);
	GUI::get().readPair(json, "CellPadding", style.CellPadding);
	GUI::get().readPair(json, "ItemSpacing", style.ItemSpacing);
	GUI::get().readPair(json, "ItemInnerSpacing", style.ItemInnerSpacing);
	GUI::get().readPair(json, "TouchExtraPadding", style.TouchExtraPadding);
	GUI::get().readPair(json, "WindowTitleAlign", style.WindowTitleAlign);
	GUI::get().readPair(json, "ButtonTextAlign", style.ButtonTextAlign);
	GUI::get().readPair(json, "SelectableTextAlign", style.SelectableTextAlign);
	GUI::get().readPair(json, "DisplaySafeAreaPadding", style.DisplaySafeAreaPadding);
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
	GUI::get().readColor(json, "editor_p1", value.editorP1);
	GUI::get().readColor(json, "editor_p2", value.editorP2);
	GUI::get().readColor(json, "editor_sel_p1", value.editorSelP1);
	GUI::get().readColor(json, "editor_sel_p2", value.editorSelP2);
	if (style.CurveTessellationTol < 0.10f) style.CurveTessellationTol = 0.10f;
	bool absolute = false;
	if (auto flag = json["metrics_absolute"].asBool()) absolute = flag.unwrap();
	if (!absolute && value.menuScale != 1.f) {
		float bake = value.menuScale;
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
	GUI::get().onStyleReplaced();
}

std::filesystem::path GUI::themePath(std::string const& name) {
	auto safe = name.empty() ? std::string("Default") : name;
	for (char& glyph : safe) {
		if (glyph == '/' || glyph == '\\' || glyph == ':') glyph = '_';
	}
	return Logic::get().saveRoot() / "themes" / (safe + ".ui.json");
}

void GUI::writeTheme(std::filesystem::path const& path) {
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	std::ofstream file(path);
	if (!file) return;
	file << GUI::get().styleJson().dump(2);
}

void GUI::noteStyleEdit() {
	if (ImGui::IsItemDeactivatedAfterEdit()) GUI::get().writeTheme(GUI::get().themePath("current"));
}

bool editStyleVec(char const* id, ImVec2& live, float min, float max) {
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
	GUI::get().noteStyleEdit();
	return true;
}

bool editStyleFloat(char const* id, float& live, float min, float max) {
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
	GUI::get().noteStyleEdit();
	return true;
}

void GUI::paintEditor(float* dest, ImVec4 color) {
	dest[0] = color.x;
	dest[1] = color.y;
	dest[2] = color.z;
	dest[3] = color.w;
}

void GUI::randomizeTheme() {
	std::mt19937 rng{std::random_device{}()};
	std::uniform_real_distribution<float> unit(0.f, 1.f);
	std::uniform_int_distribution<int> schemePick(0, 6);
	std::uniform_int_distribution<int> moodPick(0, 3);

	auto wrap = [](float h) {
		h = std::fmod(h, 1.f);
		return h < 0.f ? h + 1.f : h;
	};
	auto hsv = [](float h, float s, float v, float a = 1.f) {
		float red = 0.f;
		float green = 0.f;
		float blue = 0.f;
		ImGui::ColorConvertHSVtoRGB(h, std::clamp(s, 0.f, 1.f), std::clamp(v, 0.f, 1.f), red, green, blue);
		return ImVec4(red, green, blue, a);
	};
	auto mix = [](ImVec4 a, ImVec4 b, float t) {
		return ImVec4(
			a.x + (b.x - a.x) * t,
			a.y + (b.y - a.y) * t,
			a.z + (b.z - a.z) * t,
			a.w + (b.w - a.w) * t
		);
	};

	float hue = unit(rng);
	// Equal chance for each Coolors-style harmony.
	enum class Scheme { Complement, Split, Triad, Tetrad, Analogous, Mono, Compound };
	auto scheme = static_cast<Scheme>(schemePick(rng));
	float second = hue;
	float third = hue;
	float fourth = hue;
	switch (scheme) {
		case Scheme::Complement:
			second = wrap(hue + 0.5f);
			third = wrap(hue + 0.5f);
			fourth = hue;
			break;
		case Scheme::Split:
			second = wrap(hue + 0.42f);
			third = wrap(hue + 0.58f);
			fourth = wrap(hue + 0.5f);
			break;
		case Scheme::Triad:
			second = wrap(hue + 1.f / 3.f);
			third = wrap(hue + 2.f / 3.f);
			fourth = second;
			break;
		case Scheme::Tetrad:
			second = wrap(hue + 0.25f);
			third = wrap(hue + 0.5f);
			fourth = wrap(hue + 0.75f);
			break;
		case Scheme::Analogous:
			second = wrap(hue + 0.08f);
			third = wrap(hue - 0.08f);
			fourth = wrap(hue + 0.16f);
			break;
		case Scheme::Mono:
			second = hue;
			third = hue;
			fourth = hue;
			break;
		case Scheme::Compound:
			// Rectangle tetradic: two complementary pairs offset by ~60°.
			second = wrap(hue + 0.17f);
			third = wrap(hue + 0.5f);
			fourth = wrap(hue + 0.67f);
			break;
	}

	// Equal chance mood profiles (Coolors generator ranges).
	enum class Mood { Dark, Light, Muted, Neon };
	auto mood = static_cast<Mood>(moodPick(rng));
	float baseS = 0.28f;
	float baseV = 0.11f;
	float panelV = 0.16f;
	float raisedV = 0.23f;
	float textV = 0.96f;
	float mutedV = 0.62f;
	float accentS = 0.62f;
	float accentV = 0.78f;
	switch (mood) {
		case Mood::Dark:
			baseS = 0.22f + unit(rng) * 0.20f;
			baseV = 0.07f + unit(rng) * 0.08f;
			panelV = baseV + 0.05f;
			raisedV = baseV + 0.12f;
			textV = 0.94f + unit(rng) * 0.05f;
			mutedV = 0.55f + unit(rng) * 0.12f;
			accentS = 0.55f + unit(rng) * 0.30f;
			accentV = 0.70f + unit(rng) * 0.22f;
			break;
		case Mood::Light:
			baseS = 0.08f + unit(rng) * 0.18f;
			baseV = 0.90f + unit(rng) * 0.08f;
			panelV = baseV - 0.04f;
			raisedV = baseV - 0.10f;
			textV = 0.12f + unit(rng) * 0.10f;
			mutedV = 0.40f + unit(rng) * 0.12f;
			accentS = 0.50f + unit(rng) * 0.35f;
			accentV = 0.55f + unit(rng) * 0.25f;
			break;
		case Mood::Muted:
			baseS = 0.10f + unit(rng) * 0.14f;
			baseV = 0.14f + unit(rng) * 0.14f;
			panelV = baseV + 0.06f;
			raisedV = baseV + 0.12f;
			textV = 0.88f + unit(rng) * 0.08f;
			mutedV = 0.58f + unit(rng) * 0.12f;
			accentS = 0.28f + unit(rng) * 0.22f;
			accentV = 0.62f + unit(rng) * 0.20f;
			break;
		case Mood::Neon:
			baseS = 0.35f + unit(rng) * 0.25f;
			baseV = 0.05f + unit(rng) * 0.08f;
			panelV = baseV + 0.06f;
			raisedV = baseV + 0.14f;
			textV = 0.96f;
			mutedV = 0.60f;
			accentS = 0.78f + unit(rng) * 0.20f;
			accentV = 0.88f + unit(rng) * 0.10f;
			break;
	}

	if (scheme == Scheme::Mono) {
		second = hue;
		third = hue;
		fourth = hue;
		accentS = std::min(1.f, accentS + 0.08f);
	}

	auto& style = ImGui::GetStyle();
	auto set = [&](ImGuiCol slot, ImVec4 color) { style.Colors[slot] = color; };

	ImVec4 background = hsv(hue, baseS, baseV, 0.97f);
	ImVec4 panel = hsv(hue, baseS * 0.85f, panelV, 0.98f);
	ImVec4 raised = hsv(hue, baseS * 0.75f, raisedV, 1.f);
	ImVec4 text = hsv(hue, mood == Mood::Light ? 0.12f : 0.04f, textV, 1.f);
	ImVec4 muted = hsv(hue, mood == Mood::Light ? 0.10f : 0.08f, mutedV, 1.f);
	ImVec4 line = hsv(second, baseS * 0.6f, mood == Mood::Light ? raisedV - 0.12f : raisedV + 0.10f, 0.75f);
	ImVec4 accent = hsv(second, accentS, accentV, 1.f);
	ImVec4 accentHot = hsv(second, std::min(1.f, accentS * 0.85f), std::min(1.f, accentV + 0.16f), 1.f);
	ImVec4 accentDeep = hsv(second, accentS * 0.9f, accentV * 0.55f, 1.f);
	ImVec4 support = hsv(third, accentS * 0.85f, accentV * 0.90f, 1.f);
	ImVec4 highlight = hsv(fourth, accentS * 0.75f, std::min(1.f, accentV + 0.08f), 1.f);

	if (mood == Mood::Light) {
		accentDeep = hsv(second, accentS * 0.75f, accentV * 0.75f, 1.f);
		line = hsv(hue, baseS * 0.5f, raisedV - 0.18f, 0.65f);
	}

	set(ImGuiCol_Text, text);
	set(ImGuiCol_TextDisabled, muted);
	set(ImGuiCol_WindowBg, background);
	set(ImGuiCol_ChildBg, ImVec4(panel.x, panel.y, panel.z, 0.55f));
	set(ImGuiCol_PopupBg, panel);
	set(ImGuiCol_Border, line);
	set(ImGuiCol_BorderShadow, ImVec4(0.f, 0.f, 0.f, 0.f));
	set(ImGuiCol_FrameBg, raised);
	set(ImGuiCol_FrameBgHovered, mix(raised, accent, 0.35f));
	set(ImGuiCol_FrameBgActive, mix(raised, accent, 0.55f));
	set(ImGuiCol_TitleBg, hsv(hue, baseS, std::max(0.f, baseV - 0.03f), 1.f));
	set(ImGuiCol_TitleBgActive, mix(panel, accent, 0.45f));
	set(ImGuiCol_TitleBgCollapsed, background);
	set(ImGuiCol_MenuBarBg, panel);
	set(ImGuiCol_ScrollbarBg, background);
	set(ImGuiCol_ScrollbarGrab, accentDeep);
	set(ImGuiCol_ScrollbarGrabHovered, accent);
	set(ImGuiCol_ScrollbarGrabActive, accentHot);
	set(ImGuiCol_CheckMark, accentHot);
	set(ImGuiCol_SliderGrab, accent);
	set(ImGuiCol_SliderGrabActive, accentHot);
	set(ImGuiCol_Button, accentDeep);
	set(ImGuiCol_ButtonHovered, accent);
	set(ImGuiCol_ButtonActive, support);
	set(ImGuiCol_Header, mix(raised, accent, 0.55f));
	set(ImGuiCol_HeaderHovered, mix(raised, accent, 0.75f));
	set(ImGuiCol_HeaderActive, accent);
	set(ImGuiCol_Separator, line);
	set(ImGuiCol_SeparatorHovered, accent);
	set(ImGuiCol_SeparatorActive, accentHot);
	set(ImGuiCol_ResizeGrip, accentDeep);
	set(ImGuiCol_ResizeGripHovered, accent);
	set(ImGuiCol_ResizeGripActive, accentHot);
	set(ImGuiCol_Tab, hsv(hue, baseS * 0.9f, panelV, 1.f));
	set(ImGuiCol_TabHovered, accent);
	set(ImGuiCol_TabActive, mix(panel, accent, 0.55f));
	set(ImGuiCol_TabUnfocused, hsv(hue, baseS * 0.7f, baseV, 1.f));
	set(ImGuiCol_TabUnfocusedActive, mix(panel, accent, 0.35f));
	set(ImGuiCol_PlotLines, support);
	set(ImGuiCol_PlotLinesHovered, accentHot);
	set(ImGuiCol_PlotHistogram, accent);
	set(ImGuiCol_PlotHistogramHovered, accentHot);
	set(ImGuiCol_TableHeaderBg, mix(panel, accent, 0.25f));
	set(ImGuiCol_TableBorderStrong, line);
	set(ImGuiCol_TableBorderLight, ImVec4(line.x, line.y, line.z, 0.45f));
	set(ImGuiCol_TableRowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
	set(ImGuiCol_TableRowBgAlt, ImVec4(raised.x, raised.y, raised.z, 0.35f));
	set(ImGuiCol_TextSelectedBg, ImVec4(accent.x, accent.y, accent.z, 0.45f));
	set(ImGuiCol_DragDropTarget, accentHot);
	set(ImGuiCol_NavHighlight, accent);
	set(ImGuiCol_NavWindowingHighlight, ImVec4(1.f, 1.f, 1.f, 0.7f));
	set(ImGuiCol_NavWindowingDimBg, ImVec4(0.2f, 0.2f, 0.2f, 0.2f));
	set(ImGuiCol_ModalWindowDimBg, ImVec4(0.f, 0.f, 0.f, 0.45f));

	auto& value = Logic::get();
	GUI::get().paintEditor(value.editorP1, hsv(hue, accentS * 0.9f, accentV, 1.f));
	GUI::get().paintEditor(value.editorP2, hsv(second, accentS, std::min(1.f, accentV + 0.05f), 1.f));
	GUI::get().paintEditor(value.editorSelP1, hsv(third, accentS * 0.7f, std::min(1.f, accentV + 0.15f), 1.f));
	GUI::get().paintEditor(value.editorSelP2, highlight);
	GUI::get().writeTheme(GUI::get().themePath("current"));
	Logic::get().saveSettingsFile();
}

void GUI::colorRow(char const* label, float* color, int id) {
	ImGui::PushID(id);
	if (ImGui::ColorEdit4("##editor-color", color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
		Logic::get().saveSettingsFile();
		GUI::get().writeTheme(GUI::get().themePath("current"));
	}
	ImGui::SameLine();
	ImGui::TextUnformatted(label);
	ImGui::PopID();
}



void GUI::applySavedTheme() {
	auto path = GUI::get().themePath("current");
	if (!std::filesystem::exists(path)) return;
	std::ifstream file(path);
	auto parsed = matjson::Value::parse(file);
	if (!parsed) return;
	GUI::get().applyStyleJson(parsed.unwrap());
}
void GUI::applyThemeJson(matjson::Value const& json) {
	GUI::get().applyStyleJson(json);
	GUI::get().writeTheme(GUI::get().themePath("current"));
	Logic::get().saveSettingsFile();
}

void GUI::drawTheme() {
	auto& value = Logic::get();
	auto& style = ImGui::GetStyle();
	GUI::get().sectionLabel("Theme");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.f);
	ImGui::InputText("Name", &value.themeName);
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	float themeW = GUI::get().sharedButtonWidth({"Save", "Load", "Randomize"});
	if (ImGui::Button("Save", ImVec2(themeW, 0.f))) {
		GUI::get().writeTheme(GUI::get().themePath(value.themeName));
		GUI::get().writeTheme(GUI::get().themePath("current"));
		Logic::get().saveSettingsFile();
	}
	ImGui::SameLine();
	if (ImGui::Button("Load", ImVec2(themeW, 0.f))) GUI::get().openThemePicker();
	ImGui::SameLine();
	if (ImGui::Button("Randomize", ImVec2(themeW, 0.f))) GUI::get().randomizeTheme();
	if (ImGui::Checkbox("Docked Menu", &value.dockedMenu)) Logic::get().saveSettingsFile();
	ImGui::SameLine();
	bool sharp = style.CircleTessellationMaxError >= 2.5f;
	if (ImGui::Checkbox("Sharp Edges", &sharp)) {
		style.CircleTessellationMaxError = sharp ? 5.f : 0.1f;
		GUI::get().writeTheme(GUI::get().themePath("current"));
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.f);
	float scaleCap = GUI::get().scaleCap;
	float dragMax = GUI::get().scaleDragging ? 3.f : scaleCap;
	if (ImGui::DragFloat("Global Scale", &value.menuScale, 0.01f, 0.35f, dragMax, "%.1f")) {
		value.menuScale = std::clamp(value.menuScale, 0.35f, 3.f);
	} ImGui::SameLine();
	if (ImGui::IsItemActivated()) GUI::get().scaleDragging = true;
	if (ImGui::IsItemDeactivated()) {
		GUI::get().scaleDragging = false;
		value.menuScale = std::clamp(value.menuScale, 0.35f, GUI::get().scaleCap);
	}
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		Logic::get().saveSettingsFile();
		GUI::get().writeTheme(GUI::get().themePath("current"));
	}
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
	if (ImGui::DragFloat("Animation Speed", &value.menuAnimSpeed, 0.01f, 0.25f, 4.f, "%.2f")) {
		value.menuAnimSpeed = std::clamp(value.menuAnimSpeed, 0.25f, 4.f);
	}
	if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().saveSettingsFile();
	ImGui::Separator();

	style.AntiAliasedLines = true;
	style.AntiAliasedLinesUseTex = true;
	style.AntiAliasedFill = true;

	float themeChildH = ImGui::GetFontSize() * 18.f;
	// Colors sits a filter row above its child; grow Sizes by that same offset.
	float filterOffset = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
	float sizesChildH = themeChildH + filterOffset;
	// Wide enough for the longest style label (e.g. SelectableTextAlign) and ImGui color names.
	float themeChildW = ImGui::GetFontSize() * 26.f;
	float themeItemW = ImGui::GetFontSize() * 14.f;

	if (ImGui::BeginTabBar("##echo-theme")) {
		if (ImGui::BeginTabItem("Sizes")) {
			ImGui::BeginChild("##echo-sizes", ImVec2(themeChildW, sizesChildH), ImGuiChildFlags_Borders);
			ImGui::PushItemWidth(themeItemW);
			ImGui::TextUnformatted("Main");
			editStyleVec("WindowPadding", style.WindowPadding, 0.f, 20.f);
			editStyleVec("FramePadding", style.FramePadding, 0.f, 20.f);
			editStyleVec("CellPadding", style.CellPadding, 0.f, 20.f);
			editStyleVec("ItemSpacing", style.ItemSpacing, 0.f, 20.f);
			editStyleVec("ItemInnerSpacing", style.ItemInnerSpacing, 0.f, 20.f);
			editStyleVec("TouchExtraPadding", style.TouchExtraPadding, 0.f, 10.f);
			editStyleFloat("IndentSpacing", style.IndentSpacing, 0.f, 30.f);
			editStyleFloat("ScrollbarSize", style.ScrollbarSize, 1.f, 20.f);
			editStyleFloat("GrabMinSize", style.GrabMinSize, 1.f, 20.f);
			ImGui::TextUnformatted("Borders");
			editStyleFloat("WindowBorderSize", style.WindowBorderSize, 0.f, 1.f);
			editStyleFloat("ChildBorderSize", style.ChildBorderSize, 0.f, 1.f);
			editStyleFloat("PopupBorderSize", style.PopupBorderSize, 0.f, 1.f);
			editStyleFloat("FrameBorderSize", style.FrameBorderSize, 0.f, 1.f);
			editStyleFloat("TabBorderSize", style.TabBorderSize, 0.f, 1.f);
			ImGui::TextUnformatted("Rounding");
			editStyleFloat("WindowRounding", style.WindowRounding, 0.f, 12.f);
			editStyleFloat("ChildRounding", style.ChildRounding, 0.f, 12.f);
			editStyleFloat("FrameRounding", style.FrameRounding, 0.f, 12.f);
			editStyleFloat("PopupRounding", style.PopupRounding, 0.f, 12.f);
			editStyleFloat("ScrollbarRounding", style.ScrollbarRounding, 0.f, 12.f);
			editStyleFloat("GrabRounding", style.GrabRounding, 0.f, 12.f);
			editStyleFloat("TabRounding", style.TabRounding, 0.f, 12.f);
			ImGui::TextUnformatted("Alignment");
			ImGui::DragFloat2("WindowTitleAlign", &style.WindowTitleAlign.x, 0.01f, 0.f, 1.f, "%.2f");
			GUI::get().noteStyleEdit();
			int buttonSide = static_cast<int>(style.WindowMenuButtonPosition) + 1;
			if (ImGui::Combo("WindowBtnPos", &buttonSide, "None\0Left\0Right\0")) {
				style.WindowMenuButtonPosition = static_cast<ImGuiDir>(buttonSide - 1);
				GUI::get().writeTheme(GUI::get().themePath("current"));
			}
			int colorSide = static_cast<int>(style.ColorButtonPosition);
			if (ImGui::Combo("ColorButtonPosition", &colorSide, "Left\0Right\0")) {
				style.ColorButtonPosition = static_cast<ImGuiDir>(colorSide);
				GUI::get().writeTheme(GUI::get().themePath("current"));
			}
			ImGui::DragFloat2("ButtonTextAlign", &style.ButtonTextAlign.x, 0.01f, 0.f, 1.f, "%.2f");
			ImGui::SameLine();
			GUI::get().helpMarker("Alignment applies when a button is larger than its text.");
			GUI::get().noteStyleEdit();
			ImGui::DragFloat2("SelectableTextAlign", &style.SelectableTextAlign.x, 0.01f, 0.f, 1.f, "%.2f");
			GUI::get().noteStyleEdit();
			ImGui::TextUnformatted("Safe Area Padding");
			ImGui::SameLine();
			GUI::get().helpMarker("Adjust if the window edges sit off the screen.");
			ImGui::SliderFloat2("SafePadding", &style.DisplaySafeAreaPadding.x, 0.f, 30.f, "%.0f");
			GUI::get().noteStyleEdit();
			ImGui::PopItemWidth();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Colors")) {
			static ImGuiTextFilter filter;
			filter.Draw("Filter colors", GUI::get().px(220.f));
			ImGui::BeginChild("##echo-colors", ImVec2(themeChildW, themeChildH), ImGuiChildFlags_Borders);
			if (filter.PassFilter("Player 1 In Editor")) GUI::get().colorRow("Player 1 In Editor", value.editorP1, 998);
			if (filter.PassFilter("Player 2 In Editor")) GUI::get().colorRow("Player 2 In Editor", value.editorP2, 999);
			if (filter.PassFilter("Player 1 In Editor (Selected)")) GUI::get().colorRow("Player 1 In Editor (Selected)", value.editorSelP1, 997);
			if (filter.PassFilter("Player 2 In Editor (Selected)")) GUI::get().colorRow("Player 2 In Editor (Selected)", value.editorSelP2, 996);
			for (int i = 0; i < ImGuiCol_COUNT; ++i) {
				char const* name = ImGui::GetStyleColorName(i);
				if (!filter.PassFilter(name)) continue;
				ImGui::PushID(i);
				if (ImGui::ColorEdit4("##style-color", &style.Colors[i].x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
					GUI::get().writeTheme(GUI::get().themePath("current"));
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


std::optional<cocos2d::enumKeyCodes> GUI::cocosKey(ImGuiKey key) {
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

bool GUI::isModifier(ImGuiKey key) {
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

std::string GUI::keyName(cocos2d::enumKeyCodes key) {
	if (key == cocos2d::KEY_None) return "None";
	char const* name = CCKeyboardDispatcher::get()->keyToString(key);
	if (!name || name[0] == '\0') return "Unknown";
	return name;
}

KeyboardModifier GUI::modifierForKey(cocos2d::enumKeyCodes key) {
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

std::string GUI::storedBind(char const* setting) {
	auto binds = Mod::get()->getSettingValue<std::vector<Keybind>>(setting);
	if (binds.empty() || binds[0].key == cocos2d::KEY_None) return "None";
	return binds[0].toString();
}

void GUI::storeBind(char const* setting, cocos2d::enumKeyCodes key, bool ctrl, bool shift, bool alt) {
	KeyboardModifier mods = GUI::get().modifierForKey(key);
	if (ctrl) mods |= KeyboardModifier::Control;
	if (shift) mods |= KeyboardModifier::Shift;
	if (alt) mods |= KeyboardModifier::Alt;
	Mod::get()->setSettingValue<std::vector<Keybind>>(setting, {Keybind(key, mods)});
	Logic::get().saveSettingsFile();
}

void GUI::flipSetting(bool& field, char const* setting) {
	field = !field;
	Logic::writeBool(setting, field);
	Logic::get().saveSettingsFile();
	Logic::get().refreshPopup();
}



// Recording utilities live in the settings file, not in mod settings.
static void flipUtility(bool& field) {
	field = !field;
	Logic::get().resetTrajectory();
	Logic::get().saveSettingsFile();
	Logic::get().refreshPopup();
}

bool Logic::dispatchSettingKey(std::string_view key, bool down) {
	if (!down) return false;
	auto& value = Logic::get();
	if (key == "toggle-record") Logic::get().toggleRecording();
	else if (key == "toggle-play") Logic::get().togglePlaying();
	else if (key == "toggle-advance") Logic::get().toggleFrameAdvance();
	else if (key == "key-click-both") GUI::get().flipSetting(value.clickBoth, "click-both");
	else if (key == "key-swap") GUI::get().flipSetting(value.swapPlayers, "swap-players");
	else if (key == "key-shakes") GUI::get().flipSetting(value.disableShakes, "disable-shakes");
	else if (key == "key-layout") GUI::get().flipSetting(value.layoutMode, "layout-mode");
	else if (key == "key-hitboxes") GUI::get().flipSetting(value.showHitboxes, "show-hitboxes");
	else if (key == "key-trajectory") GUI::get().flipSetting(value.showTrajectory, "show-trajectory");
	else if (key == "key-autoclick") GUI::get().flipSetting(value.autoclicker, "autoclicker");
	else if (key == "key-no-overwrite") GUI::get().flipSetting(value.noOverwrite, "no-overwrite");
	else if (key == "key-ignore") GUI::get().flipSetting(value.ignoreInputs, "ignore-inputs");
	else if (key == "key-show-recording") GUI::get().flipSetting(value.showRecording, "show-recording");
	else if (key == "key-show-cps") GUI::get().flipSetting(value.showCps, "show-cps");
	else if (key == "key-show-frame") GUI::get().flipSetting(value.showFrame, "show-frame");
	else if (key == "key-show-time") GUI::get().flipSetting(value.showTime, "show-time");
	else if (key == "key-show-percent") GUI::get().flipSetting(value.showPercent, "show-percent");
	else if (key == "key-click-dash-orbs") flipUtility(value.clickDashOrbs);
	else if (key == "key-click-black-orbs") flipUtility(value.clickBlackOrbs);
	else if (key == "key-pad-black-orb") flipUtility(value.padBlackOrb);
	else if (key == "key-pad-blue-orb") flipUtility(value.padBlueOrb);
	else if (key == "key-pause-before-kill") flipUtility(value.pauseBeforeKill);
	else if (key == "key-save") Logic::get().saveReplay();
	else if (key == "key-load") Logic::get().pickReplay();
	else if (key == "open-menu") {
		GUI::get().openPopup();
	}
	else if (key == "key-reset") {
		if (auto* play = PlayLayer::get()) play->resetLevel();
	} else if (key == "key-speedhack") {
		if (std::abs(value.speedhack - 1.f) > 0.05f) {
			value.speedhackMemory = value.speedhack;
			value.speedhack = 1.f;
		} else {
			float back = value.speedhackMemory;
			if (!(back >= 0.01f) || std::abs(back - 1.f) <= 0.05f) return false;
			value.speedhack = back;
		}
		Logic::get().syncTimeScale();
		Logic::get().syncPlaybackFps();
		Logic::get().saveSettingsFile();
	} else {
		return false;
	}
	return true;
}



void GUI::offerRebind(char const* settingId) {
	if (ImGui::IsItemHovered()) {
		auto current = GUI::get().storedBind(settingId);
		ImGui::SetTooltip("Right-click to rebind (%s)", current.c_str());
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
		GUI::get().prompt = settingId;
		GUI::get().promptNested = ImGui::GetCurrentContext()->BeginPopupStack.Size > 0;
	}
}

void GUI::drawKeybindPrompt(bool nested) {
	if (GUI::get().prompt.empty() || GUI::get().promptNested != nested) return;
	if (GUI::get().promptShown != GUI::get().prompt) {
		GUI::get().promptShown = GUI::get().prompt;
		GUI::get().pressed.reset();
		GUI::get().bindCtrl = false;
		GUI::get().bindShift = false;
		GUI::get().bindAlt = false;
	}
	if (!ImGui::IsPopupOpen("Confirm Keybind")) ImGui::OpenPopup("Confirm Keybind");
	if (!ImGui::BeginPopupModal("Confirm Keybind", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

	std::string label = GUI::get().prompt;
	for (auto const& action : kListed) {
		if (GUI::get().prompt == action.setting) label = action.label;
	}
	auto existing = GUI::get().storedBind(GUI::get().prompt.c_str());
	if (existing != "None") ImGui::Text("Existing keybind: %s", existing.c_str());
	ImGui::Text("Press a key for '%s'", label.c_str());

	for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
		auto imguiKey = static_cast<ImGuiKey>(key);
		if (imguiKey >= ImGuiKey_MouseLeft) continue;
		if (GUI::get().isModifier(imguiKey)) {
			if (GUI::get().pressed || !ImGui::IsKeyReleased(imguiKey)) continue;
			auto mapped = GUI::get().cocosKey(imguiKey);
			if (!mapped) continue;
			auto own = GUI::get().modifierForKey(*mapped);
			GUI::get().pressed = mapped;
			GUI::get().bindCtrl = static_cast<bool>(own & KeyboardModifier::Control);
			GUI::get().bindShift = static_cast<bool>(own & KeyboardModifier::Shift);
			GUI::get().bindAlt = static_cast<bool>(own & KeyboardModifier::Alt);
			break;
		}
		if (!ImGui::IsKeyPressed(imguiKey, false)) continue;
		auto mapped = GUI::get().cocosKey(imguiKey);
		if (!mapped) continue;
		GUI::get().pressed = mapped;
		GUI::get().bindCtrl = ImGui::GetIO().KeyCtrl;
		GUI::get().bindShift = ImGui::GetIO().KeyShift;
		GUI::get().bindAlt = ImGui::GetIO().KeyAlt;
		break;
	}

	float bindW = GUI::get().sharedButtonWidth({"Save", "Remove"});
	if (GUI::get().pressed) {
		auto own = GUI::get().modifierForKey(*GUI::get().pressed);
		std::string pressed = "Pressed key: " + GUI::get().keyName(*GUI::get().pressed);
		if (GUI::get().bindCtrl && !(own & KeyboardModifier::Control)) pressed += " + Ctrl";
		if (GUI::get().bindShift && !(own & KeyboardModifier::Shift)) pressed += " + Shift";
		if (GUI::get().bindAlt && !(own & KeyboardModifier::Alt)) pressed += " + Alt";
		ImGui::TextUnformatted(pressed.c_str());
		if (ImGui::Button("Save", ImVec2(bindW, 0.f))) {
			GUI::get().storeBind(GUI::get().prompt.c_str(), *GUI::get().pressed, GUI::get().bindCtrl, GUI::get().bindShift, GUI::get().bindAlt);
			GUI::get().prompt.clear();
			GUI::get().promptShown.clear();
			GUI::get().promptNested = false;
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
			GUI::get().storeBind(GUI::get().prompt.c_str(), cocos2d::KEY_None, false, false, false);
			GUI::get().prompt.clear();
			GUI::get().promptShown.clear();
			GUI::get().promptNested = false;
			ImGui::CloseCurrentPopup();
		}
	} else {
		ImGui::BeginDisabled();
		ImGui::Button("Remove", ImVec2(bindW, 0.f));
		ImGui::EndDisabled();
	}
	if (ImGui::Button("Exit", ImVec2(GUI::get().sharedButtonWidth({"Exit"}), 0.f))) {
		GUI::get().prompt.clear();
		GUI::get().promptShown.clear();
		GUI::get().promptNested = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}


$execute {
	// A held drag in a level is delivered as a touch move, and the menu backend turns that
	// into a cancelled touch. Stopping it here leaves the click held for ImGui.
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
	return GUI::get().replayPicker;
}

std::string GUI::lowerCopy(std::string text) {
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return text;
}


ShownReplay GUI::shownReplay(std::filesystem::path const& path) {
	std::string file = path.filename().string();
	std::string lower = GUI::get().lowerCopy(file);
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
	if (file.empty()) file = path.stem().string();
	std::string tag = autosave ? "(autosave)" : ext.empty() || ext == ".echo" ? std::string() : "(" + ext + ")";
	if (path.parent_path().filename() == "converted") tag = tag.empty() ? "(converted)" : "(converted) " + tag;
	return {file, tag};
}

void GUI::refreshFiles() {
	auto& state = GUI::get().picker();
	state.files.clear();
	auto take = [&](std::filesystem::path const& dir) {
		std::error_code error;
		if (!std::filesystem::exists(dir, error)) return;
		for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
			if (error || !entry.is_regular_file()) continue;
			auto name = GUI::get().lowerCopy(entry.path().filename().string());
			bool replay = name.ends_with(".echo") || name.ends_with(".json") || name.ends_with(".txt") || name.ends_with(".slc") ||
				name.ends_with(".tcm") || name.ends_with(".gdr");
			if (!replay) continue;
			state.files.push_back(entry.path());
		}
	};
	take(Logic::get().saveRoot() / "replays");
	take(Logic::get().saveRoot() / "converted");
	std::sort(state.files.begin(), state.files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
		return GUI::get().lowerCopy(a.filename().string()) < GUI::get().lowerCopy(b.filename().string());
	});
}



void GUI::openMacroPicker(bool sequence, int amount) {
	GUI::get().revealMenu();
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
	state.wantOpen = true;
	GUI::get().refreshFiles();
}
void GUI::drawMacroPicker() {
	auto& state = GUI::get().picker();
	static int drawnFrame = -1;
	if (drawnFrame == ImGui::GetFrameCount()) return;
	if (state.wantOpen) ImGui::OpenPopup("Echo Macros###echo-macro-picker");
	float pickerScale = ImGui::GetFontSize() / 18.f;
	ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
	if (!ImGui::BeginPopup("Echo Macros###echo-macro-picker")) return;
	drawnFrame = ImGui::GetFrameCount();
	state.wantOpen = false;
	ImGui::TextUnformatted(state.sequence ? "Add a macro to the sequence" : "Load a macro");
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##macro-filter", "Type to filter macros", &state.filter);
	ImGui::BeginChild("##macro-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
	std::string needle = GUI::get().lowerCopy(state.filter);
	int shown = 0;
	for (auto const& path : state.files) {
		auto shownName = GUI::get().shownReplay(path);
		std::string blob = GUI::get().lowerCopy(shownName.name + " " + shownName.tag + " " + path.filename().string());
		if (!needle.empty() && blob.find(needle) == std::string::npos) continue;
		++shown;
		ImVec2 row = ImGui::GetCursorScreenPos();
		float rowH = ImGui::GetTextLineHeight();
		std::string rowId = "##replay-" + path.string();
		bool picked = ImGui::Selectable(rowId.c_str(), false, 0, ImVec2(0.f, rowH));
		auto* draw = ImGui::GetWindowDrawList();
		float textX = row.x + ImGui::GetStyle().FramePadding.x;
		draw->AddText(ImVec2(textX, row.y), ImGui::GetColorU32(ImGuiCol_Text), shownName.name.c_str());
		if (!shownName.tag.empty()) {
			float tagX = textX + ImGui::CalcTextSize(shownName.name.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
			draw->AddText(ImVec2(tagX, row.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), shownName.tag.c_str());
		}
		if (picked) {
			if (state.sequence) Logic::get().appendSequenceFile(path, state.amount);
			else Logic::get().readReplay(path);
			ImGui::CloseCurrentPopup();
		}
	}
	if (shown == 0) ImGui::TextDisabled("No macros match");
	ImGui::EndChild();
	float pickerW = GUI::get().sharedButtonWidth({"Refresh", "Close"});
	if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refreshFiles();
	ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

ThemePicker& GUI::themes() {
	return GUI::get().themePicker;
}

void GUI::refreshThemes() {
	auto& state = GUI::get().themes();
	state.files.clear();
	auto dir = Logic::get().saveRoot() / "themes";
	std::error_code error;
	if (!std::filesystem::exists(dir, error)) return;
	for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
		if (error || !entry.is_regular_file()) continue;
		auto name = GUI::get().lowerCopy(entry.path().filename().string());
		if (!name.ends_with(".ui.json")) continue;
		if (name == "current.ui.json") continue;
		state.files.push_back(entry.path());
	}
	std::sort(state.files.begin(), state.files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
		return GUI::get().lowerCopy(a.filename().string()) < GUI::get().lowerCopy(b.filename().string());
	});
}

void GUI::openThemePicker() {
	GUI::get().revealMenu();
	auto& state = GUI::get().themes();
	state.filter.clear();
	state.wantOpen = true;
	GUI::get().refreshThemes();
}
void GUI::drawThemePicker() {
	auto& state = GUI::get().themes();
	static int drawnFrame = -1;
	if (drawnFrame == ImGui::GetFrameCount()) return;
	if (state.wantOpen) ImGui::OpenPopup("Echo Themes###echo-theme-picker");
	float pickerScale = ImGui::GetFontSize() / 18.f;
	ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
	if (!ImGui::BeginPopup("Echo Themes###echo-theme-picker")) return;
	drawnFrame = ImGui::GetFrameCount();
	state.wantOpen = false;
	ImGui::TextUnformatted("Load a theme");
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##theme-filter", "Type to filter themes", &state.filter);
	ImGui::BeginChild("##theme-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
	std::string needle = GUI::get().lowerCopy(state.filter);
	int shown = 0;
	for (auto const& path : state.files) {
		std::string name = path.filename().string();
		if (name.size() >= 8) name.resize(name.size() - 8);
		if (!needle.empty() && GUI::get().lowerCopy(name).find(needle) == std::string::npos) continue;
		++shown;
		if (ImGui::Selectable(name.c_str(), false)) {
			std::ifstream file(path);
			auto parsed = matjson::Value::parse(file);
			if (parsed) {
				GUI::get().applyThemeJson(parsed.unwrap());
			}
			ImGui::CloseCurrentPopup();
		}
	}
	if (shown == 0) ImGui::TextDisabled("No themes match");
	ImGui::EndChild();
	float pickerW = GUI::get().sharedButtonWidth({"Refresh", "Close"});
	if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refreshThemes();
	ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

ClickpackPicker& GUI::clickpacks() {
	return GUI::get().clickpackPicker;
}

void GUI::refreshClickpacks() {
	auto& state = GUI::get().clickpacks();
	state.folders.clear();
	auto dir = Logic::get().saveRoot() / "clickpacks";
	std::error_code error;
	if (!std::filesystem::exists(dir, error)) return;
	for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
		if (error || !entry.is_directory()) continue;
		state.folders.push_back(entry.path());
	}
	std::sort(state.folders.begin(), state.folders.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
		return GUI::get().lowerCopy(a.filename().string()) < GUI::get().lowerCopy(b.filename().string());
	});
}

void GUI::openClickpackPicker() {
	GUI::get().revealMenu();
	auto& state = GUI::get().clickpacks();
	state.filter.clear();
	state.wantOpen = true;
	GUI::get().refreshClickpacks();
}
void GUI::drawClickpackPicker() {
	auto& state = GUI::get().clickpacks();
	static int drawnFrame = -1;
	if (drawnFrame == ImGui::GetFrameCount()) return;
	if (state.wantOpen) ImGui::OpenPopup("Echo Clickpacks###echo-clickpack-picker");
	float pickerScale = ImGui::GetFontSize() / 18.f;
	ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
	if (!ImGui::BeginPopup("Echo Clickpacks###echo-clickpack-picker")) return;
	drawnFrame = ImGui::GetFrameCount();
	state.wantOpen = false;
	ImGui::TextUnformatted(state.player == 0 ? "Pick a clickpack for Player 1" : "Pick a clickpack for Player 2");
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##clickpack-filter", "Type to filter clickpacks", &state.filter);
	ImGui::BeginChild("##clickpack-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
	std::string needle = GUI::get().lowerCopy(state.filter);
	int shown = 0;
	for (auto const& path : state.folders) {
		std::string name = path.filename().string();
		if (!needle.empty() && GUI::get().lowerCopy(name).find(needle) == std::string::npos) continue;
		++shown;
		if (ImGui::Selectable(name.c_str(), false)) {
			if (state.player == 0) Logic::get().clickpack = path.string();
			else Logic::get().clickpackP2 = path.string();
			Logic::get().pushRuntimeSettings();
			ImGui::CloseCurrentPopup();
		}
	}
	if (shown == 0) ImGui::TextDisabled("No clickpacks match");
	ImGui::EndChild();
	float pickerW = GUI::get().sharedButtonWidth({"Refresh", "Close"});
	if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refreshClickpacks();
	ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

RenderPicker& GUI::renders() {
	return GUI::get().renderPicker;
}

void GUI::refreshRenders() {
	auto& state = GUI::get().renders();
	state.files.clear();
	auto dir = Logic::get().saveRoot() / "renders";
	std::error_code error;
	std::filesystem::create_directories(dir, error);
	if (!std::filesystem::exists(dir, error)) return;
	std::string extra = GUI::get().lowerCopy(Logic::get().renderExtension);
	if (!extra.empty() && extra.front() != '.') extra.insert(extra.begin(), '.');
	for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
		if (error || !entry.is_regular_file()) continue;
		auto name = GUI::get().lowerCopy(entry.path().filename().string());
		bool video = name.ends_with(".mp4") || name.ends_with(".mkv") || name.ends_with(".mov") || name.ends_with(".webm");
		if (!video && !extra.empty() && extra != ".") video = name.ends_with(extra);
		if (!video) continue;
		state.files.push_back(entry.path());
	}
	std::sort(state.files.begin(), state.files.end(), [](std::filesystem::path const& a, std::filesystem::path const& b) {
		return GUI::get().lowerCopy(a.filename().string()) < GUI::get().lowerCopy(b.filename().string());
	});
}

void GUI::openRenderPicker() {
	GUI::get().revealMenu();
	auto& state = GUI::get().renders();
	state.second = false;
	state.first.clear();
	state.filter.clear();
	state.wantOpen = true;
	GUI::get().refreshRenders();
}

void GUI::drawRenderPicker() {
	auto& state = GUI::get().renders();
	static int drawnFrame = -1;
	if (drawnFrame == ImGui::GetFrameCount()) return;
	if (state.wantOpen) ImGui::OpenPopup("Echo Renders###echo-render-picker");
	float pickerScale = ImGui::GetFontSize() / 18.f;
	ImGui::SetNextWindowSize(ImVec2(420.f * pickerScale, 360.f * pickerScale), ImGuiCond_Always);
	if (!ImGui::BeginPopup("Echo Renders###echo-render-picker")) return;
	drawnFrame = ImGui::GetFrameCount();
	state.wantOpen = false;
	ImGui::TextUnformatted(state.second ? "Select the second video" : "Select the first video");
	if (state.second) ImGui::TextDisabled("%s", state.first.filename().string().c_str());
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::InputTextWithHint("##render-filter", "Type to filter videos", &state.filter);
	ImGui::BeginChild("##render-list", ImVec2(0.f, 240.f * pickerScale), ImGuiChildFlags_Borders);
	std::string needle = GUI::get().lowerCopy(state.filter);
	int shown = 0;
	for (auto const& path : state.files) {
		std::string name = path.filename().string();
		if (!needle.empty() && GUI::get().lowerCopy(name).find(needle) == std::string::npos) continue;
		++shown;
		std::string rowId = name + "##render-" + path.string();
		if (!ImGui::Selectable(rowId.c_str(), false)) continue;
		if (!state.second) {
			state.first = path;
			state.second = true;
			state.filter.clear();
			continue;
		}
		Logic::get().mergeRenders(state.first, path);
		state.second = false;
		state.first.clear();
		ImGui::CloseCurrentPopup();
	}
	if (shown == 0) ImGui::TextDisabled("No videos match");
	ImGui::EndChild();
	float pickerW = GUI::get().sharedButtonWidth({"Refresh", "Close"});
	if (ImGui::Button("Refresh", ImVec2(pickerW, 0.f))) GUI::get().refreshRenders();
	ImGui::SameLine();
	if (ImGui::Button("Close", ImVec2(pickerW, 0.f))) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}
