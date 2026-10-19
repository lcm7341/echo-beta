#pragma once

#include "GUI/widgets.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace cocos2d {
	class CCTexture2D;
}

namespace Script {
	class Host;
}

namespace ThemeScript {
	enum class Hook {
		WindowBackground,
		WindowFrame,
		Card,
		Header,
		Button,
		ActionButton,
		Checkbox,
		Field,
		Slider,
		Selectable,
		NavItem,
		NavMarker,
		Separator,
		Heading,
		Progress,
		CloseButton,
		Gear,
		Help,
		StatusDot,
		Swatch,
		ScrollMark,
		Logo,
		TitleBar,
		NavBar,
		StatusBar,
		Bar,
		TileBar,
		CollapseBar,
		ThemeTile,
		FloatingLogo,
		Count
	};

	struct Widget {
		Hook hook = Hook::Button;
		ImGuiID id = 0;
		ImVec2 min, max;
		const char* label = nullptr;
		float hover = 0.f, down = 0.f, on = 0.f;
		float value = -1.f;
		float rounding = 0.f;
		bool pressed = false, clicked = false;
		bool popup = false, rainbow = false;
		ImVec4 tint = ImVec4(0.f, 0.f, 0.f, 0.f);
	};

	extern bool g_hooks[(int)Hook::Count];

	struct PageLayout {
		std::string page, name, icon;
		std::vector<std::string> cards;
		std::vector<float> widths;
	};

	struct CardLayout {
		bool hidden = false;
		float width = 0.f;
	};

	struct BarLayout {
		bool shown = true, logo = true, title = true, version = true, nav = true, status = true, reset = true, close = true, bottom = false;
		std::string title_text = "Echo";
	};

	struct StatusLayout {
		bool shown = true, state = true, frame = true, inputs = true, tps = true, speed = true;
	};

	struct Layout {
		std::vector<PageLayout> pages;
		std::string nav = "left";
		bool nav_labels = true;
		int docked = -1;
		bool header_shown = true;
		EchoUI::HeaderLook header;
		BarLayout bar;
		StatusLayout status;
		float tile_width = 0.f;
		float card_width = 0.f;
		int columns = 0;
		std::unordered_map<std::string, CardLayout> cards;
	};

	const Layout& layout();
	int layout_version();
	const CardLayout* card_layout(const std::string& page, const std::string& title);
	bool forces_sharp();
	void apply_metrics(ImGuiStyle& style);

	bool run_hook(const Widget& widget, ImDrawList* list, float alpha, void (*look)(void*), void* context);

	template <class Look>
	bool draw(const Widget& widget, ImDrawList* list, float alpha, Look&& look) {
		if (!g_hooks[(int)widget.hook]) return false;
		return run_hook(widget, list, alpha, [](void* context) { (*static_cast<std::remove_reference_t<Look>*>(context))(); }, &look);
	}

	bool has(Hook hook);
	bool load(const std::filesystem::path& file, bool use_palette);
	void unload();
	bool reload();
	bool active();
	const std::string& error();
	std::filesystem::path file();
	std::filesystem::path themes_folder();
	std::string saved_name();
	std::filesystem::path from_saved_name(const std::string& name);
	void begin_frame();
	void draw_overlay(float alpha);
	bool begin_floating_logo(float dt);
	bool has_background();
	void draw_background(ImDrawList* list, float alpha, ImVec2 min, ImVec2 max);
	void recolor();
	ImVec4 window_color();
	cocos2d::CCTexture2D* logo();
	bool make_new(std::filesystem::path& made);
	std::vector<std::filesystem::path> bundled();
	std::string bundled_name(const std::filesystem::path& file);
	bool is_bundled(const std::filesystem::path& file);
	std::filesystem::path installed_folder(const std::filesystem::path& file);
	bool install(const std::filesystem::path& file, std::filesystem::path& made);
	Script::Host& host();
}
