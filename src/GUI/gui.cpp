#include "GUI/gui.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
#include <imgui_internal.h>
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
#include <optional>
#include <Geode/binding/CCTextInputNode.hpp>
#include <functional>


GUI& GUI::get() {
	static GUI instance;
	return instance;
}




using namespace geode::prelude;


bool GUI::typing() {
	if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) return true;
	auto* director = CCDirector::sharedDirector();
	auto* scene = director ? director->getRunningScene() : nullptr;
	if (!scene) return false;
	bool found = false;
	std::function<void(CCNode*)> walk = [&](CCNode* node) {
		if (!node || found) return;
		if (auto* input = typeinfo_cast<CCTextInputNode*>(node); input && input->m_selected) found = true;
		auto* children = node->getChildren();
		if (!children) return;
		for (auto* child : CCArrayExt<CCNode*>(children)) walk(child);
	};
	walk(scene);
	return found;
}

void GUI::loadMenuFont() {
	auto& io = ImGui::GetIO();
	auto const path = Mod::get()->getResourcesDir() / "Roboto-Medium.ttf";
	ImFontConfig config;
	config.OversampleH = 2;
	config.OversampleV = 2;
	ImFont* font = nullptr;
	if (std::filesystem::exists(path)) {
		font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 18.f, &config);
	}
	if (!font) io.Fonts->AddFontDefault();
}


TabPage const kPages[] = {
	{"Play", [] { GUI::get().drawMain(); }},
	{"Macro", [] { GUI::get().drawMacro(); }},
	{"Editor", [] { GUI::get().drawEditor(); }},
	{"Macro Tools", [] { GUI::get().drawTools(); }},
	{"Visuals", [] { GUI::get().drawVisuals(); }},
	{"Sequence", [] { GUI::get().drawSequence(); }},
	{"Clickbot", [] { GUI::get().drawClickbot(); }},
	{"Render", [] { GUI::get().drawRender(); }},
	{"Theme", [] { GUI::get().drawTheme(); }},
};

std::vector<TabPage> GUI::orderedPages() {
	auto& saved = Logic::get().tabOrder;
	std::vector<TabPage> pages;
	auto already = [&](std::string const& name) {
		for (auto const& kept : pages) {
			if (kept.name == name) return true;
		}
		return false;
	};
	for (auto const& savedName : saved) {
		std::string name = savedName;
		if (name == "Main") name = "Play";
		else if (name == "Assist" || name == "Tools") name = "Macro Tools";
		if (already(name)) continue;
		for (auto const& page : kPages) {
			if (name == page.name) pages.push_back(page);
		}
	}
	for (auto const& page : kPages) {
		if (already(page.name)) continue;
		// A page added later goes right after the one it belongs beside.
		std::string name = page.name;
		char const* after = name == "Editor" ? "Macro" : name == "Visuals" ? "Macro Tools" : nullptr;
		if (after) {
			auto at = pages.begin();
			for (; at != pages.end(); ++at) {
				if (std::string(at->name) == after) {
					++at;
					break;
				}
			}
			pages.insert(at, page);
		} else {
			pages.push_back(page);
		}
	}
	return pages;
}

float GUI::smoothstep01(float t) {
	t = std::clamp(t, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

void GUI::easeToward(float& current, float target, float dt) {
	float step = 1.f - std::exp(-dt * 16.f);
	if (std::abs(current - target) <= 0.35f) current = target;
	else current += (target - current) * step;
}

void GUI::easeToward(ImVec2& current, ImVec2 target, float dt) {
	GUI::get().easeToward(current.x, target.x, dt);
	GUI::get().easeToward(current.y, target.y, dt);
}



Slide GUI::rollSlide() {
	static std::mt19937 rng{std::random_device{}()};
	std::uniform_int_distribution<int> dist(0, 3);
	return static_cast<Slide>(dist(rng));
}

void GUI::rollDirections() {
	for (auto const& page : kPages) GUI::get().slide[page.name] = GUI::get().rollSlide();
	GUI::get().dockSlide = GUI::get().rollSlide();
}

ImVec2 GUI::offscreen(Slide dir, ImVec2 target, ImVec2 size, ImVec2 display) {
	float pad = 28.f;
	switch (dir) {
		case Slide::Left: return ImVec2(-size.x - pad, target.y);
		case Slide::Right: return ImVec2(display.x + pad, target.y);
		case Slide::Down: return ImVec2(target.x, display.y + pad);
		case Slide::Up: return ImVec2(target.x, -size.y - pad);
	}
	return ImVec2(target.x, -size.y - pad);
}

float GUI::animRate() {
	return std::max(Logic::get().menuAnimSpeed, 0.05f);
}

void GUI::beginClose() {
	if (!ImGuiCocos::get().isVisible()) return;
	GUI::get().closing = true;
}

void GUI::tickMotion() {
	float dt = ImGui::GetIO().DeltaTime;
	float rate = GUI::get().animRate();
	if (GUI::get().closing) {
		GUI::get().motion.open = std::max(0.f, GUI::get().motion.open - dt * 2.6f * rate);
		if (GUI::get().motion.open <= 0.f) {
			GUI::get().closing = false;
			GUI::get().motion.open = 0.f;
			ImGuiCocos::get().setVisible(false);
		}
		return;
	}
	if (GUI::get().motion.open <= 0.f) GUI::get().rollDirections();
	GUI::get().motion.open = std::min(1.f, GUI::get().motion.open + dt * 2.4f * rate);
}


void GUI::pushOpenFade(StyleVarGuard& guard) {
	float alpha = ImGui::GetStyle().Alpha * std::max(GUI::get().smoothstep01(GUI::get().motion.open), 0.04f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
	guard.count += 1;
}

float GUI::shelfShown(int index, int count) {
	float stagger = 0.045f / GUI::get().animRate();
	float span = std::max(0.42f, 1.f - stagger * static_cast<float>(std::max(count - 1, 0)));
	float start = stagger * static_cast<float>(index);
	return GUI::get().smoothstep01(std::clamp((GUI::get().motion.open - start) / span, 0.f, 1.f));
}

namespace {
float shelfMargin(float font) {
	return std::max(font * 0.75f, 10.f);
}

float shelfGap(float font) {
	return std::max(font * 0.45f, 8.f);
}

int shelfIndexOf(std::vector<std::string> const& names, char const* want) {
	for (size_t i = 0; i < names.size(); ++i) {
		if (names[i] == want) return static_cast<int>(i);
	}
	return -1;
}

// Preferred draw order — packer places these first so the top row fills before spilling down.
char const* kShelfOrder[] = {"Play", "Macro", "Editor", "Macro Tools", "Visuals", "Sequence", "Render", "Theme", "Clickbot"};

struct ShelfRect {
	float x = 0.f;
	float y = 0.f;
	float w = 0.f;
	float h = 0.f;
	bool overlaps(ShelfRect const& o, float gap) const {
		return x < o.x + o.w + gap && x + w + gap > o.x && y < o.y + o.h + gap && y + h + gap > o.y;
	}
};

// Dense packer: each window goes to the topmost-leftmost free slot that fits.
// Theme still prefers under Play (and Clickbot under Editor) when that cell is free, otherwise
// they fill gaps below shorter top-row windows so the bottom/right of the screen get used.
bool shelfPack(std::vector<std::string> const& names, std::vector<ImVec2> const& sizes, ImVec2 display, float margin, float gap, std::vector<ImVec2>& positions, std::vector<ImVec2>& limits, bool shrinkOk) {
	if (names.size() != sizes.size() || names.empty()) return false;
	float usableRight = display.x - margin;
	float usableBottom = display.y - margin;
	float screenW = std::max(32.f, usableRight - margin);
	float screenH = std::max(32.f, usableBottom - margin);
	positions.assign(names.size(), ImVec2(margin, margin));
	limits.assign(names.size(), ImVec2(32.f, 32.f));

	auto natural = [&](size_t i) {
		ImVec2 size(std::max(32.f, sizes[i].x), std::max(32.f, sizes[i].y));
		if (shrinkOk) {
			size.x = std::min(size.x, screenW);
			size.y = std::min(size.y, screenH);
		}
		return size;
	};
	for (size_t i = 0; i < sizes.size(); ++i) {
		ImVec2 size = natural(i);
		if (!shrinkOk && (size.x > screenW + 0.5f || size.y > screenH + 0.5f)) return false;
	}

	std::vector<ShelfRect> occupied;
	std::vector<bool> placed(names.size(), false);

	auto fitsAt = [&](float x, float y, float w, float h) {
		if (x < margin - 0.5f || y < margin - 0.5f) return false;
		if (x + w > usableRight + 0.5f || y + h > usableBottom + 0.5f) return false;
		ShelfRect rect{x, y, w, h};
		for (auto const& other : occupied) {
			if (rect.overlaps(other, gap * 0.5f)) return false;
		}
		return true;
	};

	auto placeAt = [&](int idx, float x, float y, ImVec2 size) -> bool {
		float w = size.x;
		float h = size.y;
		if (!fitsAt(x, y, w, h)) {
			if (!shrinkOk) return false;
			w = std::min(w, std::max(32.f, usableRight - x));
			h = std::min(h, std::max(32.f, usableBottom - y));
			if (w < 32.f || h < 32.f || !fitsAt(x, y, w, h)) return false;
		}
		positions[static_cast<size_t>(idx)] = ImVec2(x, y);
		limits[static_cast<size_t>(idx)] = ImVec2(w, h);
		occupied.push_back({x, y, w, h});
		placed[static_cast<size_t>(idx)] = true;
		return true;
	};

	// Topmost, then leftmost free cell that can hold (w, h).
	auto findSlot = [&](float w, float h, float& outX, float& outY) -> bool {
		std::vector<float> ys{margin};
		std::vector<float> xs{margin};
		ys.reserve(occupied.size() + 1);
		xs.reserve(occupied.size() + 1);
		for (auto const& other : occupied) {
			ys.push_back(other.y + other.h + gap);
			xs.push_back(other.x + other.w + gap);
		}
		std::sort(ys.begin(), ys.end());
		ys.erase(std::unique(ys.begin(), ys.end(), [](float a, float b) { return std::abs(a - b) < 0.5f; }), ys.end());
		std::sort(xs.begin(), xs.end());
		xs.erase(std::unique(xs.begin(), xs.end(), [](float a, float b) { return std::abs(a - b) < 0.5f; }), xs.end());

		for (float y : ys) {
			if (y + h > usableBottom + 0.5f) continue;
			for (float x : xs) {
				if (x + w > usableRight + 0.5f) continue;
				if (fitsAt(x, y, w, h)) {
					outX = x;
					outY = y;
					return true;
				}
			}
			// Also try packing against the left edge on this scanline even if no rect ends there.
			if (fitsAt(margin, y, w, h)) {
				outX = margin;
				outY = y;
				return true;
			}
		}
		return false;
	};

	auto placeOne = [&](int idx) -> bool {
		if (idx < 0 || placed[static_cast<size_t>(idx)]) return true;
		ImVec2 size = natural(static_cast<size_t>(idx));
		std::string const& name = names[static_cast<size_t>(idx)];

		auto tryAnchor = [&](char const* parentName) -> bool {
			int parent = shelfIndexOf(names, parentName);
			if (parent < 0 || !placed[static_cast<size_t>(parent)]) return false;
			float ax = positions[static_cast<size_t>(parent)].x;
			float ay = positions[static_cast<size_t>(parent)].y + limits[static_cast<size_t>(parent)].y + gap;
			return fitsAt(ax, ay, size.x, size.y) && placeAt(idx, ax, ay, size);
		};

		if (name == "Theme" && tryAnchor("Play")) return true;
		if (name == "Clickbot" && tryAnchor("Editor")) return true;

		float x = 0.f;
		float y = 0.f;
		if (!findSlot(size.x, size.y, x, y)) return false;
		return placeAt(idx, x, y, size);
	};

	std::vector<bool> queued(names.size(), false);
	for (char const* name : kShelfOrder) {
		int idx = shelfIndexOf(names, name);
		if (idx < 0) continue;
		queued[static_cast<size_t>(idx)] = true;
		if (!placeOne(idx)) return false;
	}
	for (size_t i = 0; i < names.size(); ++i) {
		if (queued[i]) continue;
		if (!placeOne(static_cast<int>(i))) return false;
	}
	return true;
}

bool shelfPackFits(std::vector<std::string> const& names, std::vector<ImVec2> const& sizes, ImVec2 display, float margin, float gap) {
	std::vector<ImVec2> positions;
	std::vector<ImVec2> limits;
	return shelfPack(names, sizes, display, margin, gap, positions, limits, false);
}

void shelfPlace(std::vector<std::string> const& names, std::vector<ImVec2> const& sizes, ImVec2 display, float margin, float gap, std::vector<ImVec2>& positions, std::vector<ImVec2>& limits) {
	GUI::get().shelfPacked = shelfPack(names, sizes, display, margin, gap, positions, limits, false);
	if (!GUI::get().shelfPacked) shelfPack(names, sizes, display, margin, gap, positions, limits, true);
}
}

float GUI::sharedButtonWidth(std::initializer_list<char const*> labels) {
	int count = std::max(static_cast<int>(labels.size()), 1);
	float need = 0.f;
	for (char const* label : labels) need = std::max(need, GUI::get().labelWidth(label));
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	bool stretches = (window->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0 || (window->ChildFlags & ImGuiChildFlags_AutoResizeX) != 0;
	// Auto-sized tabs must use the label. GetContentRegionAvail there is the size cap, so using it
	// stretches every button to the screen and the tab becomes a full-width strip.
	if (stretches) return need;
	float along = window->DC.CursorPos.x - window->DC.CursorStartPos.x;
	float inner = along + std::max(0.f, ImGui::GetContentRegionAvail().x);
	float spacing = ImGui::GetStyle().ItemSpacing.x * static_cast<float>(std::max(count - 1, 0));
	float room = std::max(0.f, inner - spacing);
	return std::max(need, room / static_cast<float>(count));
}

float GUI::maxFittingScale() {
	if (Logic::get().dockedMenu) return 3.f;
	ImVec2 display = ImGui::GetIO().DisplaySize;
	if (display.x < 64.f || display.y < 64.f) return 3.f;
	auto pages = GUI::get().orderedPages();
	if (pages.empty()) return 3.f;
	float current = std::max(Logic::get().menuScale, 0.05f);
	float font1 = ImGui::GetFontSize() / current;
	std::vector<std::string> names;
	std::vector<ImVec2> atUnit;
	names.reserve(pages.size());
	atUnit.reserve(pages.size());
	for (auto const& page : pages) {
		names.emplace_back(page.name);
		ImVec2 size(font1 * 16.f, font1 * 11.f);
		float measured = current;
		auto at = GUI::get().shelfAtScale.find(page.name);
		if (at != GUI::get().shelfAtScale.end() && at->second > 0.05f) measured = at->second;
		auto drawn = GUI::get().shelfSize.find(page.name);
		if (drawn != GUI::get().shelfSize.end() && drawn->second.x > 8.f && drawn->second.y > 8.f) {
			size = ImVec2(drawn->second.x / measured, drawn->second.y / measured);
		} else {
			auto known = GUI::get().shelfContent.find(page.name);
			if (known != GUI::get().shelfContent.end() && known->second.x > 8.f && known->second.y > 8.f) {
				size = ImVec2(known->second.x / measured, known->second.y / measured);
			}
		}
		auto folded = GUI::get().shelfCollapsed.find(page.name);
		if (folded != GUI::get().shelfCollapsed.end() && folded->second) {
			size.y = std::max(font1 + 8.f, ImGui::GetFrameHeight() / current);
		}
		atUnit.push_back(size);
	}
	auto fits = [&](float scale) {
		std::vector<ImVec2> sized;
		sized.reserve(atUnit.size());
		for (ImVec2 size : atUnit) sized.push_back(ImVec2(size.x * scale, size.y * scale));
		float font = font1 * scale;
		return shelfPackFits(names, sized, display, shelfMargin(font), shelfGap(font));
	};
	if (fits(3.f)) return 3.f;
	float lo = 0.35f;
	float hi = 3.f;
	for (int step = 0; step < 14; ++step) {
		float mid = (lo + hi) * 0.5f;
		if (fits(mid)) lo = mid;
		else hi = mid;
	}
	return lo;
}

void GUI::layoutShelf(std::vector<TabPage> const& pages, std::vector<ImVec2>& targets) {
	ImVec2 display = ImGui::GetIO().DisplaySize;
	float font = ImGui::GetFontSize();
	float margin = shelfMargin(font);
	float gap = shelfGap(font);
	float current = std::max(Logic::get().menuScale, 0.05f);
	std::vector<std::string> names;
	std::vector<ImVec2> sizes;
	names.reserve(pages.size());
	sizes.reserve(pages.size());
	float foldedH = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowBorderSize * 2.f;
	for (auto const& page : pages) {
		names.emplace_back(page.name);
		ImVec2 size(font * 16.f, font * 11.f);
		// Rescale last measured size to the current Global Scale so wrapping reacts immediately
		// while dragging, instead of packing with the previous frame's smaller windows.
		float measured = current;
		auto at = GUI::get().shelfAtScale.find(page.name);
		if (at != GUI::get().shelfAtScale.end() && at->second > 0.05f) measured = at->second;
		float grow = std::abs(current - measured) < 0.02f ? 1.f : current / measured;
		auto drawn = GUI::get().shelfSize.find(page.name);
		if (drawn != GUI::get().shelfSize.end() && drawn->second.x > 8.f && drawn->second.y > 8.f) {
			// SizeFull is the real drawn window — never prefer ContentSizeIdeal (it overestimates
			// and leaves empty gaps / wraps too early).
			size = ImVec2(drawn->second.x * grow, drawn->second.y * grow);
		} else {
			auto known = GUI::get().shelfContent.find(page.name);
			if (known != GUI::get().shelfContent.end() && known->second.x > 8.f && known->second.y > 8.f) {
				size = ImVec2(known->second.x * grow, known->second.y * grow);
			}
		}
		auto folded = GUI::get().shelfCollapsed.find(page.name);
		if (folded != GUI::get().shelfCollapsed.end() && folded->second) size.y = foldedH;
		sizes.push_back(size);
	}
	std::vector<ImVec2> limits;
	shelfPlace(names, sizes, display, margin, gap, targets, limits);
	for (size_t i = 0; i < pages.size(); ++i) {
		GUI::get().shelfLimit[pages[i].name] = limits[i];
		GUI::get().shelfRoom[pages[i].name] = limits[i].y;
		auto& shelf = GUI::get().shelfMotion[pages[i].name];
		shelf.pos = targets[i];
		shelf.size = limits[i];
		shelf.ready = true;
		shelf.placed = true;
	}
}

template <typename A1>
void GUI::fadePage(char const* name, A1&& draw) {
	if (GUI::get().motion.tab != name) {
		GUI::get().motion.tab = name;
		GUI::get().motion.tabBlend = 0.f;
	}
	GUI::get().motion.tabBlend = std::min(1.f, GUI::get().motion.tabBlend + ImGui::GetIO().DeltaTime * 6.5f);
	float alpha = ImGui::GetStyle().Alpha * (0.4f + 0.6f * GUI::get().smoothstep01(GUI::get().motion.tabBlend));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
	draw();
	ImGui::PopStyleVar();
}


const TabPage* GUI::activePage(std::vector<TabPage> const& pages) {
	for (auto const& page : pages) {
		if (GUI::get().page == page.name) return &page;
	}
	return pages.empty() ? nullptr : &pages.front();
}

ImGuiWindowFlags GUI::menuFlags() {
	return ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

void GUI::drawNav(std::vector<TabPage> const& pages) {
	float width = 0.f;
	for (auto const& page : pages) width = std::max(width, GUI::get().labelWidth(page.name));
	ImGuiChildFlags card = ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY;
	ImGui::BeginChild("##echo-nav", ImVec2(0.f, 0.f), card);
	for (auto const& page : pages) {
		bool selected = GUI::get().page == page.name;
		if (selected) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
		}
		if (ImGui::Button(page.name, ImVec2(width, 0.f))) GUI::get().page = page.name;
		if (selected) ImGui::PopStyleColor(3);
	}
	ImGui::EndChild();
}

template <typename A1>
void GUI::drawLoose(char const* id, char const* title, bool canClose, ImVec2 target, float shown, A1&& draw) {
	ImVec2 display = ImGui::GetIO().DisplaySize;
	float edge = std::max(ImGui::GetFontSize() * 0.5f, 8.f);
	float maxW = std::max(64.f, display.x - edge * 2.f);
	float maxH = std::max(64.f, display.y - edge * 2.f);
	auto limit = GUI::get().shelfLimit.find(id);
	auto content = GUI::get().shelfContent.find(id);
	if (limit != GUI::get().shelfLimit.end()) {
		float needW = content != GUI::get().shelfContent.end() ? content->second.x : limit->second.x;
		float needH = content != GUI::get().shelfContent.end() ? content->second.y : limit->second.y;
		maxW = std::min(maxW, std::max(limit->second.x, needW));
		maxH = std::min(maxH, std::max(ImGui::GetFrameHeight(), std::max(limit->second.y, needH)));
	}
	float maxX = std::max(edge, display.x - edge - 32.f);
	float maxY = std::max(edge, display.y - edge - ImGui::GetFrameHeight());
	ImVec2 home(std::clamp(target.x, edge, maxX), std::clamp(target.y, edge, maxY));
	auto direction = GUI::get().slide.find(id);
	Slide from = direction == GUI::get().slide.end() ? Slide::Up : direction->second;
	ImVec2 origin = GUI::get().offscreen(from, home, ImVec2(maxW, maxH), display);
	float travel = std::clamp(shown, 0.f, 1.f);
	ImVec2 pos(std::lerp(origin.x, home.x, travel), std::lerp(origin.y, home.y, travel));
	ImGui::SetNextWindowSizeConstraints(ImVec2(32.f, ImGui::GetFrameHeight()), ImVec2(maxW, maxH));
	ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(shown, 0.02f));
	ImGuiStyle& menuStyle = ImGui::GetStyle();
	ImGuiDir menuSide = menuStyle.WindowMenuButtonPosition;
	if (menuSide == ImGuiDir_None) menuStyle.WindowMenuButtonPosition = ImGuiDir_Left;
	bool open = true;
	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
	bool expanded = ImGui::Begin(title, canClose ? &open : nullptr, flags);
	ImGuiWindow* shelfWindow = ImGui::GetCurrentWindow();
	ImGuiStyle const& shelfStyle = ImGui::GetStyle();
	if (!expanded) {
		GUI::get().shelfCollapsed[id] = true;
		GUI::get().shelfSize[id] = shelfWindow->SizeFull;
		GUI::get().shelfAtScale[id] = std::max(Logic::get().menuScale, 0.05f);
		ImGui::End();
		menuStyle.WindowMenuButtonPosition = menuSide;
		if (canClose && !open) GUI::get().beginClose();
		ImGui::PopStyleVar();
		return;
	}
	GUI::get().shelfCollapsed[id] = false;
	if (canClose && !open) GUI::get().beginClose();
	draw();
	float contentW = shelfWindow->ContentSizeIdeal.x + shelfStyle.WindowPadding.x * 2.f + shelfStyle.WindowBorderSize * 2.f;
	float contentH = shelfWindow->ContentSizeIdeal.y + shelfStyle.WindowPadding.y * 2.f + shelfWindow->TitleBarHeight + shelfStyle.WindowBorderSize * 2.f;
	if (contentW < 8.f) contentW = shelfWindow->SizeFull.x;
	if (contentH < ImGui::GetFrameHeight()) contentH = shelfWindow->SizeFull.y;
	GUI::get().shelfContent[id] = ImVec2(contentW, contentH);
	GUI::get().shelfSize[id] = shelfWindow->SizeFull;
	GUI::get().shelfAtScale[id] = std::max(Logic::get().menuScale, 0.05f);
	ImGui::End();
	menuStyle.WindowMenuButtonPosition = menuSide;
	ImGui::PopStyleVar();
}

void GUI::drawWindow() {
	if (!ImGuiCocos::get().isVisible()) {
		GUI::get().motion.open = 0.f;
		GUI::get().closing = false;
		return;
	}
	GUI::get().tickMotion();
	if (!ImGuiCocos::get().isVisible()) return;
	GUI::get().syncMenuScale();
	if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("Confirm Keybind") && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		ImGuiContext& context = *ImGui::GetCurrentContext();
		if (context.OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
	}
	auto pages = GUI::get().orderedPages();
	if (!Logic::get().dockedMenu) {
		GUI::get().dockReturn = true;
		std::vector<ImVec2> targets;
		GUI::get().layoutShelf(pages, targets);
		for (size_t i = 0; i < pages.size(); ++i) {
			bool home = std::string(pages[i].name) == "Play";
			float shown = GUI::get().shelfShown(static_cast<int>(i), static_cast<int>(pages.size()));
			GUI::get().drawLoose(pages[i].name, home ? "Echo 2.0" : pages[i].name, home, targets[i], shown, pages[i].draw);
		}
		GUI::get().drawKeybindPrompt();
		GUI::get().drawMacroPicker();
		GUI::get().drawThemePicker();
		GUI::get().drawClickpackPicker();
		GUI::get().drawRenderPicker();
		return;
	}
	StyleVarGuard fade;
	GUI::get().pushOpenFade(fade);
	float eased = GUI::get().smoothstep01(GUI::get().motion.open);
	bool settle = eased > 0.985f && !GUI::get().closing;
	bool returning = GUI::get().dockReturn;
	GUI::get().dockReturn = false;
	if (returning) {
		ImGui::SetNextWindowPos(GUI::get().dockHome, ImGuiCond_Always);
	} else if (!settle) {
		ImVec2 origin = GUI::get().offscreen(GUI::get().dockSlide, GUI::get().dockHome, GUI::get().dockSize, ImGui::GetIO().DisplaySize);
		ImVec2 pos(std::lerp(origin.x, GUI::get().dockHome.x, eased), std::lerp(origin.y, GUI::get().dockHome.y, eased));
		ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
	}
	bool open = true;
	if (!ImGui::Begin("Echo 2.0", &open, GUI::get().menuFlags() | ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		if (!open) GUI::get().beginClose();
		return;
	}
	if (!open) GUI::get().beginClose();
	if (settle && !returning) {
		GUI::get().dockHome = ImGui::GetWindowPos();
		GUI::get().dockSize = ImGui::GetWindowSize();
	}
	GUI::get().drawNav(pages);
	ImGui::SameLine();
	ImGui::BeginGroup();
	if (auto* page = GUI::get().activePage(pages)) {
		GUI::get().page = page->name;
		GUI::get().fadePage(page->name, page->draw);
	}
	ImGui::EndGroup();
	GUI::get().drawKeybindPrompt();
	GUI::get().drawMacroPicker();
	GUI::get().drawThemePicker();
	GUI::get().drawClickpackPicker();
	GUI::get().drawRenderPicker();
	ImGui::End();
}


$on_mod(Loaded) {
	ImGuiCocos::get().setup([] {
		GUI::get().loadMenuFont();
		GUI::get().applySavedTheme();
		ImGuiCocos::get().setVisible(false);
	}).draw([] {
		GUI::get().drawWindow();
	});
}

void GUI::syncMenuScale() {
	auto& value = Logic::get();
	auto& gui = GUI::get();
	if (value.dockedMenu) {
		gui.scaleCap = 3.f;
		gui.shelfPacked = true;
	} else {
		// Recompute every frame (including while dragging) so wrapping can raise the cap.
		gui.scaleCap = std::min(3.f, gui.maxFittingScale());
	}
	if (!gui.scaleDragging) value.menuScale = std::clamp(value.menuScale, 0.35f, gui.scaleCap);
	else value.menuScale = std::clamp(value.menuScale, 0.35f, 3.f);
	if (!GUI::get().hasBase) {
		GUI::get().baseStyle = ImGui::GetStyle();
		GUI::get().baseStyle._MainScale = 1.f;
		GUI::get().hasBase = true;
	}
	ImGui::GetIO().FontGlobalScale = value.menuScale;
	GUI::get().appliedScale = value.menuScale;
}

void GUI::onStyleReplaced() {
	GUI::get().baseStyle = ImGui::GetStyle();
	GUI::get().baseStyle._MainScale = 1.f;
	GUI::get().hasBase = true;
	GUI::get().appliedScale = 1.f;
	GUI::get().syncMenuScale();
}

void GUI::releaseStuckMouse() {
	auto& io = ImGui::GetIO();
	for (int button = 0; button < 5; ++button) io.AddMouseButtonEvent(button, false);
}
void GUI::openPopup() {
	auto& imgui = ImGuiCocos::get();
	if (!imgui.isInitialized()) return;
	if (!imgui.isVisible()) {
		GUI::get().closing = false;
		imgui.setVisible(true);
		return;
	}
	if (GUI::get().closing) {
		GUI::get().closing = false;
		return;
	}
	GUI::get().beginClose();
}
void GUI::revealMenu() {
	auto& imgui = ImGuiCocos::get();
	if (!imgui.isInitialized()) return;
	if (!imgui.isVisible()) imgui.setVisible(true);
}
