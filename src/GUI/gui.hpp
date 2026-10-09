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
	float tabBlend = 1.f;
};

struct ShelfMotion {
	ImVec2 size{};
	ImVec2 pos{};
	bool ready = false;
	bool placed = false;
};

struct PickerState {
	bool wantOpen = false;
	bool sequence = false;
	int amount = 1;
	std::string filter;
	std::vector<std::filesystem::path> files;
};

struct ThemePicker {
	bool wantOpen = false;
	std::string filter;
	std::vector<std::filesystem::path> files;
};

struct ClickpackPicker {
	bool wantOpen = false;
	int player = 0;
	std::string filter;
	std::vector<std::filesystem::path> folders;
};

struct RenderPicker {
	bool wantOpen = false;
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
	void rewriteSelected(std::function<void(Frame&)> const& edit);
	void moveSelected(int dir);
	void addInput(int frame);
	void deleteSelected();
	void resetMacro();
	void offsetChosen(int delta);
	void randomOffsetChosen();
	void setFrameAdvance(bool on);
	void applySeedOverride();
	void setSpeed(float speed, bool settle);
	void setPlaybackFps(float fps);
	void dropCarry();
	void applyNoMirror();
	bool twoPlayerDual();
	// Empty when rendering can start, otherwise what the user has to do first.
	std::string renderBlocker();

	void releaseStuckMouse();
	void openPopup();
	void revealMenu();
	void openMacroPicker(bool sequence, int amount);
	void drawMacroPicker();
	void openThemePicker();
	void drawThemePicker();
	void openClickpackPicker();
	void drawClickpackPicker();
	void openRenderPicker();
	void drawRenderPicker();
	void drawMain();
	void drawMacro();
	void drawTools();
	void drawVisuals();
	void drawSequence();
	void drawClickbot();
	void drawEditor();
	void drawTheme();
	void drawRender();
	void applySavedTheme();
	bool typing();
	void applyThemeJson(matjson::Value const& json);
	void syncMenuScale();
	void onStyleReplaced();
	void offerRebind(char const* settingId);
	// `nested`: drawn inside the popup whose setting asked for it (offerRebind inside a popup).
	void drawKeybindPrompt(bool nested = false);

	MenuMotion motion;
	bool closing = false;
	std::unordered_map<std::string, ImVec2> shelfSize;
	std::unordered_map<std::string, ImVec2> shelfContent;
	std::unordered_map<std::string, ImVec2> shelfLimit;
	std::unordered_map<std::string, bool> shelfCollapsed;
	std::unordered_map<std::string, float> shelfRoom;
	// Global Scale at which shelfSize / shelfContent were last measured.
	std::unordered_map<std::string, float> shelfAtScale;
	float scaleCap = 3.f;
	bool scaleDragging = false;
	bool shelfPacked = true;
	std::unordered_map<std::string, ShelfMotion> shelfMotion;
	ImVec2 dockHome = ImVec2(72.f, 36.f);
	ImVec2 dockSize = ImVec2(480.f, 320.f);
	bool dockReturn = false;
	std::unordered_map<std::string, Slide> slide;
	Slide dockSlide = Slide::Up;
	std::string page = "Play";
	float appliedScale = 1.f;
	ImGuiStyle baseStyle{};
	bool hasBase = false;
	std::string prompt;
	std::string promptShown;
	// The rebind prompt was asked for inside a popup: it opens nested in that popup, so closing it
	// goes back there. Opened at the top level, ImGui closed the popup it came from.
	bool promptNested = false;
	std::optional<cocos2d::enumKeyCodes> pressed;
	bool bindCtrl = false;
	bool bindShift = false;
	bool bindAlt = false;
	int selected = -1;
	uint64_t selectedId = 0;
	int anchor = -1;
	uint64_t anchorId = 0;
	void selectInput(int index, bool range = false);
	void followSelection();
	bool shiftSelecting() const;
	bool selectionSpan(int& first, int& last) const {
		if (selected < 0) return false;
		first = selected;
		last = anchor >= 0 ? anchor : selected;
		if (first > last) std::swap(first, last);
		return true;
	}
	bool hasRange() const {
		int first = 0;
		int last = 0;
		return selectionSpan(first, last) && first != last;
	}
	int selectedCount() const {
		int first = 0;
		int last = 0;
		if (!selectionSpan(first, last)) return 0;
		return last - first + 1;
	}
	bool inputInRange(int index) const {
		int first = 0;
		int last = 0;
		return selectionSpan(first, last) && index >= first && index <= last;
	}
	int markFirst = -1;
	int markLast = -1;
	int markPlayer = -1;
	bool scrollToMark = false;
	bool showEditorP1 = true;
	bool showEditorP2 = true;
	Frame draft;

	bool editorShows(Frame const& input) const {
		return input.player2 ? showEditorP2 : showEditorP1;
	}

	bool inCpsMark(Frame const& input) const {
		if (markFirst < 0) return false;
		if (input.number < markFirst || input.number > markLast) return false;
		if (markPlayer >= 0 && input.player2 != (markPlayer == 1)) return false;
		return true;
	}
	void clearCpsMark() {
		markFirst = -1;
		markLast = -1;
		markPlayer = -1;
		scrollToMark = false;
	}
	void showCpsRange(int first, int last, bool player2);

	void syncDraft() {
		followSelection();
		auto& inputs = Logic::get().inputs;
		if (selected < 0 || selected >= static_cast<int>(inputs.size())) return;
		draft = inputs[selected];
	}
	bool autoScroll = true;
	bool plain = false;
	int inputOffset = 0;
	std::string plainText;
	PickerState replayPicker;
	ThemePicker themePicker;
	ClickpackPicker clickpackPicker;
	RenderPicker renderPicker;


	void persist() {
		Logic::get().pushRuntimeSettings();
	}

	bool checkboxSetting(char const* label, bool& field) {
		if (!ImGui::Checkbox(label, &field)) return false;
		persist();
		return true;
	}

	int pairIndex(bool first, bool second) {
		if (first && second) return 0;
		if (first) return 1;
		if (second) return 2;
		return 0;
	}

	void applyPair(int index, bool& first, bool& second) {
		first = index != 2;
		second = index != 1;
	}

	int quadIndex(bool first, bool second) {
		if (first && second) return 3;
		if (first) return 1;
		if (second) return 2;
		return 0;
	}

	void applyQuad(int index, bool& first, bool& second) {
		first = index == 1 || index == 3;
		second = index == 2 || index == 3;
	}

	float comboBoxWidth(char const* const* labels, int count) {
		float width = 0.f;
		for (int i = 0; i < count; ++i) width = std::max(width, ImGui::CalcTextSize(labels[i]).x);
		return width + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.f + ImGui::GetStyle().ItemInnerSpacing.x;
	}

	bool comboItems(char const* id, char const* const* labels, int count, int& selected, float width = 0.f) {
		bool changed = false;
		ImGui::SetNextItemWidth(width > 0.f ? width : comboBoxWidth(labels, count));
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

	void dragFloatSetting(char const* label, float& field, float speed, float min, float max, char const* format) {
		float value = field;
		if (ImGui::DragFloat(label, &value, speed, min, max, format)) field = value;
		if (ImGui::IsItemDeactivatedAfterEdit()) persist();
	}

	float px(float at18) {
		return at18 * (ImGui::GetFontSize() / 18.f);
	}

	float labelWidth(char const* text) {
		return ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
	}

	float sharedButtonWidth(std::initializer_list<char const*> labels);

	void sectionLabel(char const* label) {
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

	void loadMenuFont();
	std::vector<TabPage> orderedPages();
	float smoothstep01(float t);
	void easeToward(float& current, float target, float dt);
	void easeToward(ImVec2& current, ImVec2 target, float dt);
	Slide rollSlide();
	void rollDirections();
	ImVec2 offscreen(Slide dir, ImVec2 target, ImVec2 size, ImVec2 display);
	float animRate();
	void beginClose();
	void tickMotion();
	void pushOpenFade(StyleVarGuard& guard);
	float shelfShown(int index, int count);
	void layoutShelf(std::vector<TabPage> const& pages, std::vector<ImVec2>& targets);
	float maxFittingScale();
	template <typename A1>
	void fadePage(char const* name, A1&& draw);
	const TabPage* activePage(std::vector<TabPage> const& pages);
	ImGuiWindowFlags menuFlags();
	void drawNav(std::vector<TabPage> const& pages);
	template <typename A1>
	void drawLoose(char const* id, char const* title, bool canClose, ImVec2 target, float shown, A1&& draw);
	void drawWindow();
	ImGuiChildFlags fitCard();
	float columnWidth(std::initializer_list<char const*> labels);
	std::string ordinal(int number);
	std::string formatPlain(Session const& value);
	void applyPlain(std::string const& text);
	void drawEditFields(bool enabled, float column);
	void drawClickTimes(bool& softs, float& softMs, bool& hards, float& hardSeconds, bool& micros, int& microMs, char const* suffix);
	void helpMarker(char const* text);
	matjson::Value colorJson(ImVec4 const& color);
	matjson::Value pairJson(ImVec2 const& value);
	void readPair(matjson::Value const& json, char const* key, ImVec2& value);
	void readColor(matjson::Value const& json, char const* key, float* color);
	float unscaled(float value);
	ImVec2 unscaled(ImVec2 value);
	matjson::Value styleJson();
	void applyStyleJson(matjson::Value const& json);
	std::filesystem::path themePath(std::string const& name);
	void writeTheme(std::filesystem::path const& path);
	void noteStyleEdit();
	void paintEditor(float* dest, ImVec4 color);
	void randomizeTheme();
	void colorRow(char const* label, float* color, int id);
	std::optional<cocos2d::enumKeyCodes> cocosKey(ImGuiKey key);
	bool isModifier(ImGuiKey key);
	std::string keyName(cocos2d::enumKeyCodes key);
	geode::KeyboardModifier modifierForKey(cocos2d::enumKeyCodes key);
	std::string storedBind(char const* setting);
	void storeBind(char const* setting, cocos2d::enumKeyCodes key, bool ctrl, bool shift, bool alt);
	void flipSetting(bool& field, char const* setting);
	PickerState& picker();
	std::string lowerCopy(std::string text);
	ShownReplay shownReplay(std::filesystem::path const& path);
	void refreshFiles();
	ThemePicker& themes();
	void refreshThemes();
	ClickpackPicker& clickpacks();
	void refreshClickpacks();
	RenderPicker& renders();
	void refreshRenders();
};


