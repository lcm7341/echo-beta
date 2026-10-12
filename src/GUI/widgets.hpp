#pragma once

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <unordered_map>
#include <utility>

namespace eui {
	float ease(ImGuiID id, float target, float speed = 14.f);

	bool Checkbox(char const* label, bool* value);
	bool Button(char const* label, ImVec2 const& size = ImVec2(0.f, 0.f));

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
		Theme
	};
	bool ActionButton(char const* label, Icon icon, bool active, ImVec2 const& size, ImVec4 tint = ImVec4(0.f, 0.f, 0.f, 0.f));
	bool NavItem(char const* label, Icon icon, bool selected, float width, float height = 0.f);
	float nav_icon_width();
	void NavMarker(ImVec2 min, ImVec2 max);

	void begin_field(char const* label, float fill = -1.f, int parts = 1);
	void end_field();

	template <class Widget>
	auto field(char const* label, Widget&& widget, float fill = -1.f, int parts = 1) {
		begin_field(label, fill, parts);
		auto result = widget();
		end_field();
		return result;
	}

	bool DragFloat(char const* label, float* value, float speed = 1.f, float min = 0.f, float max = 0.f, char const* format = "%.3f", ImGuiSliderFlags flags = 0);
	bool DragInt(char const* label, int* value, float speed = 1.f, int min = 0, int max = 0, char const* format = "%d", ImGuiSliderFlags flags = 0);

	template <class... Args>
	bool DragFloat2(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::DragFloat2(label, std::forward<Args>(args)...);
		},
			-1.f, 2);
	}

	template <class... Args>
	bool SliderFloat(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::SliderFloat(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool SliderFloat2(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::SliderFloat2(label, std::forward<Args>(args)...);
		},
			-1.f, 2);
	}

	template <class... Args>
	bool InputText(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputText(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool InputTextWithHint(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputTextWithHint(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool InputInt(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputInt(label, std::forward<Args>(args)...);
		});
	}

	template <class... Args>
	bool InputScalar(char const* label, Args&&... args) {
		return field(label, [&] {
			return ImGui::InputScalar(label, std::forward<Args>(args)...);
		});
	}

	bool BeginCombo(char const* label, char const* preview, ImGuiComboFlags flags = 0);
	bool Combo(char const* label, int* current, char const* const items[], int count);
	bool Combo(char const* label, int* current, char const* zero_separated);
	bool Selectable(char const* label, bool selected = false, ImGuiSelectableFlags flags = 0, ImVec2 const& size = ImVec2(0.f, 0.f));

	void Separator();
	void SectionLabel(char const* label);
	void Help();
	void ProgressBar(float fraction, ImVec2 const& size, char const* overlay = nullptr);
	void StatusDot(ImVec4 const& tint);

	void WindowRims(float alpha);
	bool Header(char const* title, char const* version, ImTextureID logo);
	bool TitleBar(char const* title, Icon icon = Icon::None, bool closable = true, float width = 0.f);

	bool CollapseBar(char const* title, Icon icon, bool open);

	bool& single_column();
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

	std::unordered_map<std::string, TileState>& tile_states();
	bool& tiles_changed();
	void TileGroup(char const* name);
	bool BeginTileOfCards(char const* title, float width_em);
	void EndTileOfCards();
	void EndTiles();
	float em(float n);
	float glide_in(float t);
	void BeginEntrance(float t);
	void EndEntrance();
	bool BeginCard(char const* title, float width_em = 20.f);
	void EndCard();
	void BeginCardRow(char const* id);
	void EndCardRow();
	ImGuiID& last_card_row();
	float& card_row_extra(ImGuiID row);
	float CardFill(float min_em);
	void Row(char const* label, char const* tip = nullptr);
	void RowWith(char const* label, float reserve_em, char const* tip = nullptr);
	void RowHalf();
	float RowButton(int count);
	float RowRight(float width_em);
	bool Gear(char const* id);
	bool BeginModal(char const* title, float width_em = 22.f, bool closable = true);
	void EndModal();

	bool ColorEdit4(char const* label, float* rgba, ImGuiColorEditFlags flags = 0);
	bool ColorEdit3(char const* label, float* rgb, ImGuiColorEditFlags flags = 0);
	bool InputTextMultiline(char const* label, std::string* text, ImVec2 const& size = ImVec2(0.f, 0.f));
	bool Slider(char const* label, float* value, float min, float max, char const* format = "%.2f", bool rainbow = false);
	bool ThemeTile(char const* id, ImTextureID logo, bool selected);

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
		char const* name;
		Palette palette;
	};

	int preset_count();
	Preset const& preset(int index);
	bool& rainbow_rim();
	bool& button_effects();
	Scene& scene();
	char const* scene_name(Scene which);
	Scene scene_named(std::string_view name);
	void apply_palette(Palette const& palette);
	void sync_scene_turn(Palette const& palette);
	void apply_metrics();
	void apply_echo_theme();
}
