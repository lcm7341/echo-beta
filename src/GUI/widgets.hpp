#pragma once

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace EchoUI {
	float ease(ImGuiID id, float target, float speed = 14.f);
	float noise(float x, float y);
	float unit_random(unsigned item, unsigned seed);
	void soft_disc(ImDrawList* draw, ImVec2 at, float radius, ImVec4 tint, float squash = 1.f, int sides = 16);
	void beam(ImDrawList* draw, ImVec2 from, ImVec2 left, ImVec2 right, ImVec4 tint);

	bool Checkbox(const char* label, bool* value);
	bool Button(const char* label, const ImVec2& size = ImVec2(0.f, 0.f));

	enum class Icon {
		None,
		Play,
		Record,
		Stop,
		Macro,
		Editor,
		Tools,
		Gear,
		Visuals,
		Sequence,
		Clickbot,
		Render,
		Theme,
		Script
	};
	bool ActionButton(const char* label, Icon icon, bool active, const ImVec2& size, ImVec4 tint = ImVec4(0.f, 0.f, 0.f, 0.f));
	bool NavItem(const char* label, Icon icon, bool selected, float width, float height = 0.f, bool labelled = true);
	float nav_icon_width();
	void NavMarker(ImVec2 min, ImVec2 max, bool under = false);

	void begin_field(const char* label, float fill = -1.f, int parts = 1, float trim = 0.f);
	void end_field();

	template <class Widget>
	auto field(const char* label, Widget&& widget, float fill = -1.f, int parts = 1, float trim = 0.f) {
		begin_field(label, fill, parts, trim);
		auto result = widget();
		end_field();
		return result;
	}

	bool DragFloat(const char* label, float* value, float speed = 1.f, float min = 0.f, float max = 0.f, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
	bool DragInt(const char* label, int* value, float speed = 1.f, int min = 0, int max = 0, const char* format = "%d", ImGuiSliderFlags flags = 0);

	template <class... Args>
	bool DragFloat2(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::DragFloat2(label, std::forward<Args>(args)...);
		},
			-1.f, 2);
	}

	template <class... Args>
	bool SliderFloat(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::SliderFloat(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool SliderFloat2(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::SliderFloat2(label, std::forward<Args>(args)...);
		},
			-1.f, 2);
	}

	template <class... Args>
	bool InputText(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputText(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool InputTextWithHint(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputTextWithHint(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool InputInt(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputInt(label, std::forward<Args>(args)...);
		},
			-1.f, 1, (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x) * 2.f);
	}

	template <class... Args>
	bool InputScalar(const char* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputScalar(label, std::forward<Args>(args)...);
		});
	}

	bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
	bool Combo(const char* label, int* current, const char* const items[], int count);
	bool Combo(const char* label, int* current, const char* zero_separated);
	bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0.f, 0.f));

	void Separator();
	void SectionLabel(const char* label);
	void Help();
	void ProgressBar(float fraction, const ImVec2& size, const char* overlay = nullptr);
	void StatusDot(const ImVec4& tint);
	void StatusText(const ImVec4& tint, const char* text);
	ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);

	extern const ImVec4 ERROR_RED;
	extern const ImVec4 RUNNING_GREEN;

	void WindowRims(float alpha);

	struct HeaderLook {
		std::string title = "Echo";
		bool logo = true, version = true, close = true;
		float height = 1.2f;
	};

	bool Header(const HeaderLook& look, const char* version, ImTextureID logo);
	void Logo(ImTextureID logo, float side);
	bool TitleBar(const char* title, Icon icon = Icon::None, bool closable = true, float width = 0.f);
	bool BeginNavBar(const char* id, const ImVec2& size, bool sideways);
	void EndNavBar();
	void StatusBack(ImVec2 min, ImVec2 max);
	void BarBack(ImVec2 min, ImVec2 max);
	bool sharp();
	bool sharp_setting();
	void set_sharp(bool on);
	void sharpen(ImGuiStyle& live);
	float corner(float rounding);

	bool CollapseBar(const char* title, Icon icon, bool open);

	extern bool g_single_column;
	
	void ScrollMark();
	void BeginTiles(float left, float top, float right, float bottom, float tile_width_em, float shown = 1.f, float rate = 1.f);

	struct TileState {
		bool pinned = false;
		bool hidden = false;
		float x = 0.f, y = 0.f;
		float width = 0.f, height = 0.f;
		int seen = 0;
		int drawn = 0;
		int from = 0;
		float flown = 1.f;
	};

	extern std::unordered_map<std::string, TileState> g_tile_states;
	extern bool g_tiles_changed;
	void TileGroup(const char* name);
	bool BeginTileOfCards(const char* title, float width_em);
	void EndTileOfCards();
	void EndTiles();
	float em(float n);
	float glide_in(float t);
	void BeginEntrance(float t);
	void EndEntrance();
	bool BeginCard(const char* title, float width_em = 20.f, float fixed_height = 0.f);
	void EndCard();

	struct CardPlan {
		std::vector<std::string> cards;
		std::vector<float> widths;
		std::vector<std::string> pages;
	};

	void BeginCardPlan(const CardPlan& plan, float width_em, float gap, int columns);
	void CardPage(const char* page);
	void EndCardPlan();
	void BeginCardRow(const char* id);
	void EndCardRow();
	ImGuiID& last_card_row();
	float& card_row_extra(ImGuiID row);
	float CardFill(float min_em);
	void Row(const char* label, const char* tip = nullptr);
	void RowWith(const char* label, float reserve_em, const char* tip = nullptr);
	void RowHalf();
	float RowButton(int count);
	float RowRight(float width_em);
	bool Gear(const char* id);
	bool BeginModal(const char* title, float width_em = 22.f, bool closable = true);
	void EndModal();
	bool BeginBigModal(const char* title, ImVec2 size, ImVec2 least);

	bool ColorEdit4(const char* label, float* rgba, ImGuiColorEditFlags flags = 0);
	bool ColorEdit3(const char* label, float* rgb, ImGuiColorEditFlags flags = 0);
	bool InputTextMultiline(const char* label, std::string* text, const ImVec2& size = ImVec2(0.f, 0.f), ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);

	struct CodeEditorState {
		int error_line = 0;
		int jump_to_line = 0;
		int line = 1, column = 1;
		bool wrap = false;
	};
	bool CodeEditor(const char* label, std::string* text, const ImVec2& size, const std::vector<std::string>& names, CodeEditorState& state);
	bool Slider(const char* label, float* value, float min, float max, const char* format = "%.2f", bool rainbow = false);
	bool ThemeTile(const char* id, ImTextureID logo, bool selected);

	enum class Scene {
		None,
		Halloween,
		Hell,
		Heaven,
		Space,
		Ocean,
		Sakura
	};

	struct Palette {
		float hue = 0.4333f, accent_hue = 0.4333f;
		float richness = 1.f;
		float depth = 1.f;
		float accent_saturation = .5f, accent_value = 1.f;
		bool light = false;
		bool rainbow = false;
		Scene scene = Scene::None;
	};

	struct Preset {
		const char* name;
		Palette palette;
	};

	int preset_count();
	const Preset& preset(int index);
	extern bool g_rainbow_rim;
	extern bool g_button_effects;
	extern Scene g_scene;
	const char* scene_name(Scene which);
	Scene scene_named(std::string_view name);
	void apply_palette(const Palette& palette);
	void sync_scene_turn(const Palette& palette);
	void sync_night_menu(const Palette& palette);
	void apply_metrics();
	void apply_echo_theme();
}
