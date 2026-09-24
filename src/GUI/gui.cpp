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

namespace {
char const* menu_title() {
    static std::string const title = fmt::format("Echo {}###Echo", Mod::get()->getVersion().toVString());
    return title.c_str();
}
}

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

void GUI::load_menu_font() {
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
    {"Play", [] { GUI::get().draw_main(); }},
    {"Macro", [] { GUI::get().draw_macro(); }},
    {"Editor", [] { GUI::get().draw_editor(); }},
    {"Macro Tools", [] { GUI::get().draw_tools(); }},
    {"Visuals", [] { GUI::get().draw_visuals(); }},
    {"Sequence", [] { GUI::get().draw_sequence(); }},
    {"Clickbot", [] { GUI::get().draw_clickbot(); }},
    {"Render", [] { GUI::get().draw_render(); }},
    {"Theme", [] { GUI::get().draw_theme(); }},
};

std::vector<TabPage> GUI::ordered_pages() {
    auto& saved = Logic::get().tab_order;
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

void GUI::ease_toward(float& current, float target, float dt) {
    float step = 1.f - std::exp(-dt * 16.f);
    if (std::abs(current - target) <= 0.35f) current = target;
    else current += (target - current) * step;
}

void GUI::ease_toward(ImVec2& current, ImVec2 target, float dt) {
    GUI::get().ease_toward(current.x, target.x, dt);
    GUI::get().ease_toward(current.y, target.y, dt);
}

Slide GUI::roll_slide() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 3);
    return static_cast<Slide>(dist(rng));
}

void GUI::roll_directions() {
    for (auto const& page : kPages) GUI::get().slide[page.name] = GUI::get().roll_slide();
    GUI::get().dock_slide = GUI::get().roll_slide();
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

float GUI::anim_rate() {
    return std::max(Logic::get().menu_anim_speed, 0.05f);
}

void GUI::begin_close() {
    if (!ImGuiCocos::get().isVisible()) return;
    GUI::get().closing = true;
}

void GUI::tick_motion() {
    float dt = ImGui::GetIO().DeltaTime;
    float rate = GUI::get().anim_rate();
    if (GUI::get().closing) {
        GUI::get().motion.open = std::max(0.f, GUI::get().motion.open - dt * 2.6f * rate);
        if (GUI::get().motion.open <= 0.f) {
            GUI::get().closing = false;
            GUI::get().motion.open = 0.f;
            ImGuiCocos::get().setVisible(false);
        }
        return;
    }
    if (GUI::get().motion.open <= 0.f) GUI::get().roll_directions();
    GUI::get().motion.open = std::min(1.f, GUI::get().motion.open + dt * 2.4f * rate);
}

void GUI::push_open_fade(StyleVarGuard& guard) {
    float alpha = ImGui::GetStyle().Alpha * std::max(GUI::get().smoothstep01(GUI::get().motion.open), 0.04f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    guard.count += 1;
}

float GUI::shelf_shown(int index, int count) {
    float stagger = 0.045f / GUI::get().anim_rate();
    float span = std::max(0.42f, 1.f - stagger * static_cast<float>(std::max(count - 1, 0)));
    float start = stagger * static_cast<float>(index);
    return GUI::get().smoothstep01(std::clamp((GUI::get().motion.open - start) / span, 0.f, 1.f));
}

namespace {
float shelf_margin(float font) {
    return std::max(font * 0.75f, 10.f);
}

float shelf_gap(float font) {
    return std::max(font * 0.45f, 8.f);
}

int shelf_index_of(std::vector<std::string> const& names, char const* want) {
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == want) return static_cast<int>(i);
    }
    return -1;
}

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

bool shelf_pack(std::vector<std::string> const& names, std::vector<ImVec2> const& sizes, ImVec2 display, float margin, float gap, std::vector<ImVec2>& positions, std::vector<ImVec2>& limits, bool shrinkOk) {
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
            int parent = shelf_index_of(names, parentName);
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
        int idx = shelf_index_of(names, name);
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

bool shelf_pack_fits(std::vector<std::string> const& names, std::vector<ImVec2> const& sizes, ImVec2 display, float margin, float gap) {
    std::vector<ImVec2> positions;
    std::vector<ImVec2> limits;
    return shelf_pack(names, sizes, display, margin, gap, positions, limits, false);
}

void shelf_place(std::vector<std::string> const& names, std::vector<ImVec2> const& sizes, ImVec2 display, float margin, float gap, std::vector<ImVec2>& positions, std::vector<ImVec2>& limits) {
    GUI::get().shelf_packed = shelf_pack(names, sizes, display, margin, gap, positions, limits, false);
    if (!GUI::get().shelf_packed) shelf_pack(names, sizes, display, margin, gap, positions, limits, true);
}
}

float GUI::shared_button_width(std::initializer_list<char const*> labels) {
    int count = std::max(static_cast<int>(labels.size()), 1);
    float need = 0.f;
    for (char const* label : labels) need = std::max(need, GUI::get().label_width(label));
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool stretches = (window->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0 || (window->ChildFlags & ImGuiChildFlags_AutoResizeX) != 0;
    if (stretches) return need;
    float along = window->DC.CursorPos.x - window->DC.CursorStartPos.x;
    float inner = along + std::max(0.f, ImGui::GetContentRegionAvail().x);
    float spacing = ImGui::GetStyle().ItemSpacing.x * static_cast<float>(std::max(count - 1, 0));
    float room = std::max(0.f, inner - spacing);
    return std::max(need, room / static_cast<float>(count));
}

float GUI::max_fitting_scale() {
    if (Logic::get().docked_menu) return 3.f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x < 64.f || display.y < 64.f) return 3.f;
    auto pages = GUI::get().ordered_pages();
    if (pages.empty()) return 3.f;
    float current = std::max(Logic::get().menu_scale, 0.05f);
    float font1 = ImGui::GetFontSize() / current;
    std::vector<std::string> names;
    std::vector<ImVec2> atUnit;
    names.reserve(pages.size());
    atUnit.reserve(pages.size());
    for (auto const& page : pages) {
        names.emplace_back(page.name);
        ImVec2 size(font1 * 16.f, font1 * 11.f);
        float measured = current;
        auto at = GUI::get().shelf_at_scale.find(page.name);
        if (at != GUI::get().shelf_at_scale.end() && at->second > 0.05f) measured = at->second;
        auto drawn = GUI::get().shelf_size.find(page.name);
        if (drawn != GUI::get().shelf_size.end() && drawn->second.x > 8.f && drawn->second.y > 8.f) {
            size = ImVec2(drawn->second.x / measured, drawn->second.y / measured);
        } else {
            auto known = GUI::get().shelf_content.find(page.name);
            if (known != GUI::get().shelf_content.end() && known->second.x > 8.f && known->second.y > 8.f) {
                size = ImVec2(known->second.x / measured, known->second.y / measured);
            }
        }
        auto folded = GUI::get().shelf_collapsed.find(page.name);
        if (folded != GUI::get().shelf_collapsed.end() && folded->second) {
            size.y = std::max(font1 + 8.f, ImGui::GetFrameHeight() / current);
        }
        atUnit.push_back(size);
    }
    auto fits = [&](float scale) {
        std::vector<ImVec2> sized;
        sized.reserve(atUnit.size());
        for (ImVec2 size : atUnit) sized.push_back(ImVec2(size.x * scale, size.y * scale));
        float font = font1 * scale;
        return shelf_pack_fits(names, sized, display, shelf_margin(font), shelf_gap(font));
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

void GUI::layout_shelf(std::vector<TabPage> const& pages, std::vector<ImVec2>& targets) {
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float font = ImGui::GetFontSize();
    float margin = shelf_margin(font);
    float gap = shelf_gap(font);
    float current = std::max(Logic::get().menu_scale, 0.05f);
    std::vector<std::string> names;
    std::vector<ImVec2> sizes;
    names.reserve(pages.size());
    sizes.reserve(pages.size());
    float foldedH = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowBorderSize * 2.f;
    for (auto const& page : pages) {
        names.emplace_back(page.name);
        ImVec2 size(font * 16.f, font * 11.f);
        float measured = current;
        auto at = GUI::get().shelf_at_scale.find(page.name);
        if (at != GUI::get().shelf_at_scale.end() && at->second > 0.05f) measured = at->second;
        float grow = std::abs(current - measured) < 0.02f ? 1.f : current / measured;
        auto drawn = GUI::get().shelf_size.find(page.name);
        if (drawn != GUI::get().shelf_size.end() && drawn->second.x > 8.f && drawn->second.y > 8.f) {
            size = ImVec2(drawn->second.x * grow, drawn->second.y * grow);
        } else {
            auto known = GUI::get().shelf_content.find(page.name);
            if (known != GUI::get().shelf_content.end() && known->second.x > 8.f && known->second.y > 8.f) {
                size = ImVec2(known->second.x * grow, known->second.y * grow);
            }
        }
        auto folded = GUI::get().shelf_collapsed.find(page.name);
        if (folded != GUI::get().shelf_collapsed.end() && folded->second) size.y = foldedH;
        sizes.push_back(size);
    }
    std::vector<ImVec2> limits;
    shelf_place(names, sizes, display, margin, gap, targets, limits);
    for (size_t i = 0; i < pages.size(); ++i) {
        GUI::get().shelf_limit[pages[i].name] = limits[i];
        GUI::get().shelf_room[pages[i].name] = limits[i].y;
        auto& shelf = GUI::get().shelf_motion[pages[i].name];
        shelf.pos = targets[i];
        shelf.size = limits[i];
        shelf.ready = true;
        shelf.placed = true;
    }
}

template <typename A1>
void GUI::fade_page(char const* name, A1&& draw) {
    if (GUI::get().motion.tab != name) {
        GUI::get().motion.tab = name;
        GUI::get().motion.tab_blend = 0.f;
    }
    GUI::get().motion.tab_blend = std::min(1.f, GUI::get().motion.tab_blend + ImGui::GetIO().DeltaTime * 6.5f);
    float alpha = ImGui::GetStyle().Alpha * (0.4f + 0.6f * GUI::get().smoothstep01(GUI::get().motion.tab_blend));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    draw();
    ImGui::PopStyleVar();
}

const TabPage* GUI::active_page(std::vector<TabPage> const& pages) {
    for (auto const& page : pages) {
        if (GUI::get().page == page.name) return &page;
    }
    return pages.empty() ? nullptr : &pages.front();
}

ImGuiWindowFlags GUI::menu_flags() {
    return ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

void GUI::draw_nav(std::vector<TabPage> const& pages) {
    float width = 0.f;
    for (auto const& page : pages) width = std::max(width, GUI::get().label_width(page.name));
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
void GUI::draw_loose(char const* id, char const* title, bool can_close, ImVec2 target, float shown, A1&& draw) {
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float edge = std::max(ImGui::GetFontSize() * 0.5f, 8.f);
    float maxW = std::max(64.f, display.x - edge * 2.f);
    float maxH = std::max(64.f, display.y - edge * 2.f);
    auto limit = GUI::get().shelf_limit.find(id);
    auto content = GUI::get().shelf_content.find(id);
    if (limit != GUI::get().shelf_limit.end()) {
        float needW = content != GUI::get().shelf_content.end() ? content->second.x : limit->second.x;
        float needH = content != GUI::get().shelf_content.end() ? content->second.y : limit->second.y;
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
    bool expanded = ImGui::Begin(title, can_close ? &open : nullptr, flags);
    ImGuiWindow* shelfWindow = ImGui::GetCurrentWindow();
    ImGuiStyle const& shelfStyle = ImGui::GetStyle();
    if (!expanded) {
        GUI::get().shelf_collapsed[id] = true;
        GUI::get().shelf_size[id] = shelfWindow->SizeFull;
        GUI::get().shelf_at_scale[id] = std::max(Logic::get().menu_scale, 0.05f);
        ImGui::End();
        menuStyle.WindowMenuButtonPosition = menuSide;
        if (can_close && !open) GUI::get().begin_close();
        ImGui::PopStyleVar();
        return;
    }
    GUI::get().shelf_collapsed[id] = false;
    if (can_close && !open) GUI::get().begin_close();
    draw();
    float contentW = shelfWindow->ContentSizeIdeal.x + shelfStyle.WindowPadding.x * 2.f + shelfStyle.WindowBorderSize * 2.f;
    float contentH = shelfWindow->ContentSizeIdeal.y + shelfStyle.WindowPadding.y * 2.f + shelfWindow->TitleBarHeight + shelfStyle.WindowBorderSize * 2.f;
    if (contentW < 8.f) contentW = shelfWindow->SizeFull.x;
    if (contentH < ImGui::GetFrameHeight()) contentH = shelfWindow->SizeFull.y;
    GUI::get().shelf_content[id] = ImVec2(contentW, contentH);
    GUI::get().shelf_size[id] = shelfWindow->SizeFull;
    GUI::get().shelf_at_scale[id] = std::max(Logic::get().menu_scale, 0.05f);
    ImGui::End();
    menuStyle.WindowMenuButtonPosition = menuSide;
    ImGui::PopStyleVar();
}

void GUI::draw_window() {
    if (!ImGuiCocos::get().isVisible()) {
        GUI::get().motion.open = 0.f;
        GUI::get().closing = false;
        return;
    }
    GUI::get().tick_motion();
    if (!ImGuiCocos::get().isVisible()) return;
    GUI::get().sync_menu_scale();
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("Confirm Keybind") && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGuiContext& context = *ImGui::GetCurrentContext();
        if (context.OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
    }
    auto pages = GUI::get().ordered_pages();
    if (!Logic::get().docked_menu) {
        GUI::get().dock_return = true;
        std::vector<ImVec2> targets;
        GUI::get().layout_shelf(pages, targets);
        for (size_t i = 0; i < pages.size(); ++i) {
            bool home = std::string(pages[i].name) == "Play";
            float shown = GUI::get().shelf_shown(static_cast<int>(i), static_cast<int>(pages.size()));
            GUI::get().draw_loose(pages[i].name, home ? menu_title() : pages[i].name, home, targets[i], shown, pages[i].draw);
        }
        GUI::get().draw_keybind_prompt();
        GUI::get().draw_macro_picker();
        GUI::get().draw_theme_picker();
        GUI::get().draw_clickpack_picker();
        GUI::get().draw_render_picker();
        return;
    }
    StyleVarGuard fade;
    GUI::get().push_open_fade(fade);
    float eased = GUI::get().smoothstep01(GUI::get().motion.open);
    bool settle = eased > 0.985f && !GUI::get().closing;
    bool returning = GUI::get().dock_return;
    GUI::get().dock_return = false;
    if (returning) {
        ImGui::SetNextWindowPos(GUI::get().dock_home, ImGuiCond_Always);
    } else if (!settle) {
        ImVec2 origin = GUI::get().offscreen(GUI::get().dock_slide, GUI::get().dock_home, GUI::get().dock_size, ImGui::GetIO().DisplaySize);
        ImVec2 pos(std::lerp(origin.x, GUI::get().dock_home.x, eased), std::lerp(origin.y, GUI::get().dock_home.y, eased));
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    }
    bool open = true;
    if (!ImGui::Begin(menu_title(), &open, GUI::get().menu_flags() | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open) GUI::get().begin_close();
        return;
    }
    if (!open) GUI::get().begin_close();
    if (settle && !returning) {
        GUI::get().dock_home = ImGui::GetWindowPos();
        GUI::get().dock_size = ImGui::GetWindowSize();
    }
    GUI::get().draw_nav(pages);
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (auto* page = GUI::get().active_page(pages)) {
        GUI::get().page = page->name;
        GUI::get().fade_page(page->name, page->draw);
    }
    ImGui::EndGroup();
    GUI::get().draw_keybind_prompt();
    GUI::get().draw_macro_picker();
    GUI::get().draw_theme_picker();
    GUI::get().draw_clickpack_picker();
    GUI::get().draw_render_picker();
    ImGui::End();
}

$on_mod(Loaded) {
    ImGuiCocos::get().setup([] {
        GUI::get().load_menu_font();
        GUI::get().apply_saved_theme();
        ImGuiCocos::get().setVisible(false);
    }).draw([] {
        GUI::get().draw_window();
    });
}

void GUI::sync_menu_scale() {
    auto& value = Logic::get();
    auto& gui = GUI::get();
    if (value.docked_menu) {
        gui.scale_cap = 3.f;
        gui.shelf_packed = true;
    } else {
        gui.scale_cap = std::min(3.f, gui.max_fitting_scale());
    }
    float scale = value.menu_scale;
    if (!GUI::get().has_base) {
        GUI::get().base_style = ImGui::GetStyle();
        GUI::get().base_style._MainScale = 1.f;
        GUI::get().has_base = true;
    }
    ImGui::GetIO().FontGlobalScale = scale;
    GUI::get().applied_scale = scale;
}

void GUI::on_style_replaced() {
    GUI::get().base_style = ImGui::GetStyle();
    GUI::get().base_style._MainScale = 1.f;
    GUI::get().has_base = true;
    GUI::get().applied_scale = 1.f;
    GUI::get().sync_menu_scale();
}

void GUI::release_stuck_mouse() {
    auto& io = ImGui::GetIO();
    for (int button = 0; button < 5; ++button) io.AddMouseButtonEvent(button, false);
}
void GUI::open_popup() {
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
    GUI::get().begin_close();
}
void GUI::reveal_menu() {
    auto& imgui = ImGuiCocos::get();
    if (!imgui.isInitialized()) return;
    if (!imgui.isVisible()) imgui.setVisible(true);
}
