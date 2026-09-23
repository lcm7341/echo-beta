#pragma once

#include "Logic/logic.hpp"

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <unordered_map>
#include <initializer_list>
#include <vector>

enum class Slide { Left, Right, Up, Down };

struct MenuMotion {
	float open = 0.f;
	std::string tab;
	float tab_blend = 1.f;
};

struct ShelfMotion {
	ImVec2 size{};
	ImVec2 pos{};
	bool ready = false;
	bool placed = false;
};

struct PickerState {
	bool want_open = false;
	bool sequence = false;
	int amount = 1;
	std::string filter;
	std::vector<std::filesystem::path> files;
};

struct ThemePicker {
	bool want_open = false;
	std::string filter;
	std::vector<std::filesystem::path> files;
};

struct ClickpackPicker {
	bool want_open = false;
	int player = 0;
	std::string filter;
	std::vector<std::filesystem::path> folders;
};

struct RenderPicker {
	bool want_open = false;
	bool second = false;
	std::filesystem::path first;
	std::string filter;
	std::vector<std::filesystem::path> files;
};

struct TabPage {
	char const* name;
	void (*draw)();
};
struct StyleVarGuard {
	int count = 0;
	~StyleVarGuard() {
		if (count > 0) ImGui::PopStyleVar(count);
	}
};
struct ShownReplay {
	std::string name;
	std::string tag;
};
struct BindAction {
	char const* setting;
	char const* label;
};

class GUI {
public:
	static GUI& get();
	void rewrite_selected(std::function<void(Frame&)> const& edit);
	void move_selected(int dir);
	void add_input(int frame);
	void delete_selected();
	void reset_macro();
	void offset_chosen(int delta);
	void random_offset_chosen();
	void set_frame_advance(bool on);
	void apply_seed_override();
	void set_speed(float speed, bool settle);
	void set_playback_fps(float fps);
	void drop_carry();
	void apply_no_mirror();
	bool two_player_dual();
	std::string render_blocker();

	void release_stuck_mouse();
	void open_popup();
	void reveal_menu();
	void open_macro_picker(bool sequence, int amount);
	void draw_macro_picker();
	void open_theme_picker();
	void draw_theme_picker();
	void open_clickpack_picker();
	void draw_clickpack_picker();
	void open_render_picker();
	void draw_render_picker();
	void draw_main();
	void draw_macro();
	void draw_tools();
	void draw_visuals();
	void draw_sequence();
	void draw_clickbot();
	void draw_editor();
	void draw_theme();
	void draw_render();
	void apply_saved_theme();
	bool typing();
	void apply_theme_json(matjson::Value const& json);
	void sync_menu_scale();
	void on_style_replaced();
	void offer_rebind(char const* setting_id, char const* tip = nullptr);
	void draw_keybind_prompt(bool nested = false);

	MenuMotion motion;
	bool closing = false;
	std::unordered_map<std::string, ImVec2> shelf_size;
	std::unordered_map<std::string, ImVec2> shelf_content;
	std::unordered_map<std::string, ImVec2> shelf_limit;
	std::unordered_map<std::string, bool> shelf_collapsed;
	std::unordered_map<std::string, float> shelf_room;
	std::unordered_map<std::string, float> shelf_at_scale;
	float scale_cap = 3.f;
	bool scale_dragging = false;
	bool shelf_packed = true;
	std::unordered_map<std::string, ShelfMotion> shelf_motion;
	ImVec2 dock_home = ImVec2(72.f, 36.f);
	ImVec2 dock_size = ImVec2(480.f, 320.f);
	bool dock_return = false;
	std::unordered_map<std::string, Slide> slide;
	Slide dock_slide = Slide::Up;
	std::string page = "Play";
	float applied_scale = 1.f;
	ImGuiStyle base_style{};
	bool has_base = false;
	std::string prompt;
	std::string prompt_shown;
	bool prompt_nested = false;
	std::optional<cocos2d::enumKeyCodes> pressed;
	bool bind_ctrl = false;
	bool bind_shift = false;
	bool bind_alt = false;
	int selected = -1;
	uint64_t selected_id = 0;
	int anchor = -1;
	uint64_t anchor_id = 0;
	void select_input(int index, bool range = false);
	void follow_selection();
	bool shift_selecting() const;
	bool selection_span(int& first, int& last) const {
		if (selected < 0) return false;
		first = selected;
		last = anchor >= 0 ? anchor : selected;
		if (first > last) std::swap(first, last);
		return true;
	}
	bool has_range() const {
		int first = 0;
		int last = 0;
		return selection_span(first, last) && first != last;
	}
	int selected_count() const {
		int first = 0;
		int last = 0;
		if (!selection_span(first, last)) return 0;
		return last - first + 1;
	}
	bool input_in_range(int index) const {
		int first = 0;
		int last = 0;
		return selection_span(first, last) && index >= first && index <= last;
	}
	int mark_first = -1;
	int mark_last = -1;
	int mark_player = -1;
	bool scroll_to_mark = false;
	bool show_editor_p1 = true;
	bool show_editor_p2 = true;
	Frame draft;

	bool editor_shows(Frame const& input) const {
		return input.player2 ? show_editor_p2 : show_editor_p1;
	}

	bool in_cps_mark(Frame const& input) const {
		if (mark_first < 0) return false;
		if (input.number < mark_first || input.number > mark_last) return false;
		if (mark_player >= 0 && input.player2 != (mark_player == 1)) return false;
		return true;
	}
	void clear_cps_mark() {
		mark_first = -1;
		mark_last = -1;
		mark_player = -1;
		scroll_to_mark = false;
	}
	void show_cps_range(int first, int last, bool player2);

	void sync_draft() {
		follow_selection();
		auto& inputs = Logic::get().inputs;
		if (selected < 0 || selected >= static_cast<int>(inputs.size())) return;
		draft = inputs[selected];
	}
	bool auto_scroll = true;
	bool plain = false;
	int input_offset = 0;
	std::string plain_text;
	PickerState replay_picker;
	ThemePicker theme_picker;
	ClickpackPicker clickpack_picker;
	RenderPicker render_picker;

	void persist() {
		Logic::get().push_runtime_settings();
	}

	bool checkbox_setting(char const* label, bool& field) {
		if (!ImGui::Checkbox(label, &field)) return false;
		persist();
		return true;
	}

	int pair_index(bool first, bool second) {
		if (first && second) return 0;
		if (first) return 1;
		if (second) return 2;
		return 0;
	}

	void apply_pair(int index, bool& first, bool& second) {
		first = index != 2;
		second = index != 1;
	}

	int quad_index(bool first, bool second) {
		if (first && second) return 3;
		if (first) return 1;
		if (second) return 2;
		return 0;
	}

	void apply_quad(int index, bool& first, bool& second) {
		first = index == 1 || index == 3;
		second = index == 2 || index == 3;
	}

	float combo_box_width(char const* const* labels, int count) {
		float width = 0.f;
		for (int i = 0; i < count; ++i) width = std::max(width, ImGui::CalcTextSize(labels[i]).x);
		return width + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f + ImGui::GetStyle().ItemInnerSpacing.x;
	}

	bool combo_items(char const* id, char const* const* labels, int count, int& selected, float width = 0.f) {
		bool changed = false;
		ImGui::SetNextItemWidth(width > 0.f ? width : combo_box_width(labels, count));
		if (ImGui::BeginCombo(id, labels[selected])) {
			for (int i = 0; i < count; ++i) {
				if (ImGui::Selectable(labels[i], selected == i)) {
					selected = i;
					changed = true;
				}
				if (selected == i) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void drag_float_setting(char const* label, float& field, float speed, float min, float max, char const* format) {
		float value = field;
		if (ImGui::DragFloat(label, &value, speed, min, max, format)) field = value;
		if (ImGui::IsItemDeactivatedAfterEdit()) persist();
	}

	float px(float at18) {
		return at18 * (ImGui::GetFontSize() / 18.f);
	}

	float label_width(char const* text) {
		return ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
	}

	float shared_button_width(std::initializer_list<char const*> labels);

	void section_label(char const* label) {
		ImGui::Dummy(ImVec2(0.f, 6.f));
		ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		ImGui::PushStyleColor(ImGuiCol_Text, muted);
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImVec2 text = ImGui::CalcTextSize(label);
		ImVec2 min = ImGui::GetItemRectMin();
		float y = min.y + text.y * 0.55f;
		float x0 = min.x + text.x + 12.f;
		float x1 = min.x + std::max(text.x + ImGui::GetFontSize() * 8.f, text.x + 48.f);
		if (x1 > x0 + 8.f) {
			ImU32 line = ImGui::GetColorU32(ImGuiCol_Separator);
			ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(x1, y), line, 1.f);
		}
	}

	void load_menu_font();
	std::vector<TabPage> ordered_pages();
	float smoothstep01(float t);
	void ease_toward(float& current, float target, float dt);
	void ease_toward(ImVec2& current, ImVec2 target, float dt);
	Slide roll_slide();
	void roll_directions();
	ImVec2 offscreen(Slide dir, ImVec2 target, ImVec2 size, ImVec2 display);
	float anim_rate();
	void begin_close();
	void tick_motion();
	void push_open_fade(StyleVarGuard& guard);
	float shelf_shown(int index, int count);
	void layout_shelf(std::vector<TabPage> const& pages, std::vector<ImVec2>& targets);
	float max_fitting_scale();
	template <typename A1>
	void fade_page(char const* name, A1&& draw);
	const TabPage* active_page(std::vector<TabPage> const& pages);
	ImGuiWindowFlags menu_flags();
	void draw_nav(std::vector<TabPage> const& pages);
	template <typename A1>
	void draw_loose(char const* id, char const* title, bool can_close, ImVec2 target, float shown, A1&& draw);
	void draw_window();
	ImGuiChildFlags fit_card();
	float column_width(std::initializer_list<char const*> labels);
	std::string ordinal(int number);
	std::string format_plain(Session const& value);
	void apply_plain(std::string const& text);
	void draw_edit_fields(bool enabled, float column);
	void draw_click_times(bool& softs, float& soft_ms, bool& hards, float& hard_seconds, bool& micros, int& micro_ms, char const* suffix);
	void help_marker(char const* text);
	matjson::Value color_json(ImVec4 const& color);
	matjson::Value pair_json(ImVec2 const& value);
	void read_pair(matjson::Value const& json, char const* key, ImVec2& value);
	void read_color(matjson::Value const& json, char const* key, float* color);
	float unscaled(float value);
	ImVec2 unscaled(ImVec2 value);
	matjson::Value style_json();
	void apply_style_json(matjson::Value const& json);
	std::filesystem::path theme_path(std::string const& name);
	void write_theme(std::filesystem::path const& path);
	void note_style_edit();
	void paint_editor(float* dest, ImVec4 color);
	void randomize_theme();
	void color_row(char const* label, float* color, int id);
	std::optional<cocos2d::enumKeyCodes> cocos_key(ImGuiKey key);
	bool is_modifier(ImGuiKey key);
	std::string key_name(cocos2d::enumKeyCodes key);
	geode::KeyboardModifier modifier_for_key(cocos2d::enumKeyCodes key);
	std::string stored_bind(char const* setting);
	void store_bind(char const* setting, cocos2d::enumKeyCodes key, bool ctrl, bool shift, bool alt);
	void flip_setting(bool& field, char const* setting);
	PickerState& picker();
	std::string lower_copy(std::string text);
	ShownReplay shown_replay(std::filesystem::path const& path);
	void refresh_files();
	ThemePicker& themes();
	void refresh_themes();
	ClickpackPicker& clickpacks();
	void refresh_clickpacks();
	RenderPicker& renders();
	void refresh_renders();
};
