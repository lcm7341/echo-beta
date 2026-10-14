// the menu window
// docked = 1 window w/ a sidebar of pages. undocked = every section is its own tile that packs across the screen & can be pinned or hidden
#include "GUI/gui.hpp"
#include <imgui-cocos.hpp>
#include <Geode/binding/PlatformToolbox.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
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

namespace fs = std::filesystem;

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
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput)
        return true;
    auto* director = CCDirector::sharedDirector();
    auto* scene = director ? director->getRunningScene() : nullptr;
    if (!scene)
        return false;
    bool it = false;
    std::function<void(CCNode*)> walk = [&](CCNode* node) {
        if (!node || it)
            return;
        if (auto* input = typeinfo_cast<CCTextInputNode*>(node); input && input->m_selected)
            it = true;
        auto* children = node->getChildren();
        if (!children)
            return;
        for (auto* child : CCArrayExt<CCNode*>(children))
            walk(child);
    };
    walk(scene);
    return it;
}

void GUI::load_menu_font() {
    auto& io = ImGui::GetIO();
    auto const path = Mod::get()->getResourcesDir() / "Roboto-Medium.ttf";
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 2;
    ImFont* font = nullptr;
    if (fs::exists(path)) {
        font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 18.f, &config);
    }
    if (!font)
        io.Fonts->AddFontDefault();
}

TabPage const PAGES[] = {
    { "Replay", [] {
         GUI::get().main();
     } },
    { "Assists", [] {
         GUI::get().tools();
     } },
    { "Visuals", [] {
         GUI::get().draw_visuals();
     } },
    { "Editor", [] {
         GUI::get().editor();
     } },
    { "Library", [] {
         GUI::get().draw_macro();
     } },
    { "Clickbot", [] {
         GUI::get().clickbot();
     } },
    { "Render", [] {
         GUI::get().renderer();
     } },
    { "Settings", [] {
         GUI::get().draw_theme();
     } },
};

std::vector<TabPage> GUI::ordered_pages() {
    return { std::begin(PAGES), std::end(PAGES) };
}

float GUI::smoothstep01(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

void GUI::ease_toward(float& current, float target, float dt) {
    float step = 1.f - std::exp(-dt * 16.f);
    if (std::abs(current - target) <= 0.35f)
        current = target;
    else
        current += (target - current) * step;
}

void GUI::ease_toward(ImVec2& current, ImVec2 target, float dt) {
    GUI::get().ease_toward(current.x, target.x, dt);
    GUI::get().ease_toward(current.y, target.y, dt);
}

Slide GUI::roll_slide() {
    static std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(0, 3);
    return static_cast<Slide>(dist(rng));
}

void GUI::roll_directions() {
    for (auto const& page : PAGES)
        GUI::get().slide[page.name] = GUI::get().roll_slide();
    GUI::get().dock_slide = GUI::get().roll_slide();
}

ImVec2 GUI::offscreen(Slide dir, ImVec2 target, ImVec2 size, ImVec2 display) {
    float pad = 28.f;
    switch (dir) {
    case Slide::Left:
        return ImVec2(-size.x - pad, target.y);
    case Slide::Right:
        return ImVec2(display.x + pad, target.y);
    case Slide::Down:
        return ImVec2(target.x, display.y + pad);
    case Slide::Up:
        return ImVec2(target.x, -size.y - pad);
    }
    return ImVec2(target.x, -size.y - pad);
}

float GUI::anim_rate() {
    return std::max(Logic::get().menu_anim_speed, 0.05f);
}

void GUI::begin_close() {
    if (!ImGuiCocos::get().isVisible() || GUI::get().bar_only)
        return;
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
    if (GUI::get().motion.open <= 0.f)
        GUI::get().roll_directions();
    GUI::get().motion.open = std::min(1.f, GUI::get().motion.open + dt * 2.4f * rate);
}

void GUI::push_open_fade(StyleVarGuard& guard) {
    float alpha = ImGui::GetStyle().Alpha * std::max(GUI::get().smoothstep01(GUI::get().motion.open), 0.04f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    guard.count += 1;
}

float GUI::shared_button_width(std::initializer_list<char const*> labels) {
    int amt = std::max(static_cast<int>(labels.size()), 1);
    float need = 0.f;
    for (char const* label : labels)
        need = std::max(need, GUI::get().labelWidth(label));
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool stretches = (window->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0 || (window->ChildFlags & ImGuiChildFlags_AutoResizeX) != 0;
    // GetContentRegionAvail dont work idk
    if (stretches)
        return need;
    float along = window->DC.CursorPos.x - window->DC.CursorStartPos.x;
    float inner = along + std::max(0.f, ImGui::GetContentRegionAvail().x);
    float spacing = ImGui::GetStyle().ItemSpacing.x * (float)(std::max(amt - 1, 0));
    float room = std::max(0.f, inner - spacing);
    return std::max(need, room / (float)(amt));
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
        if (GUI::get().page == page.name)
            return &page;
    }
    return pages.empty() ? nullptr : &pages.front();
}

ImGuiWindowFlags GUI::menu_flags() {
    return ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
}

namespace {
    eui::Icon icon_for(std::string_view page) {
        if (page == "Replay")
            return eui::Icon::Play;
        if (page == "Assists")
            return eui::Icon::Tools;
        if (page == "Visuals")
            return eui::Icon::Visuals;
        if (page == "Editor")
            return eui::Icon::Editor;
        if (page == "Library")
            return eui::Icon::Macro;
        if (page == "Clickbot")
            return eui::Icon::Clickbot;
        if (page == "Render")
            return eui::Icon::Render;
        if (page == "Settings")
            return eui::Icon::Gear;
        return eui::Icon::None;
    }
}

void GUI::draw_nav(std::vector<TabPage> const& pages, float height) {
    ImGuiStyle const& style = ImGui::GetStyle();
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.05f);
    float width = 0.f;
    for (auto const& page : pages)
        width = std::max(width, GUI::get().labelWidth(page.name));
    float amt = (float)(std::max<size_t>(pages.size(), 1));
    float least = ImGui::GetFrameHeight() * 1.2f;
    float gaps = style.ItemSpacing.y * (amt - 1.f) + style.WindowPadding.y * 2.f;
    GUI::get().nav_needs = least * amt + gaps;
    float tall = std::max(height, least * amt + gaps);
    float row = std::clamp((tall - gaps) / amt, least, least * 2.6f);
    ImGui::BeginChild("##echo-nav", ImVec2(0.f, tall), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX);
    ImVec2 mark_min{}, mark_max{};
    bool marked = false;
    for (auto const& page : pages) {
        bool selected = GUI::get().page == page.name;
        if (eui::NavItem(page.name, icon_for(page.name), selected, width, row))
            GUI::get().page = page.name;
        if (selected) {
            mark_min = ImGui::GetItemRectMin();
            mark_max = ImGui::GetItemRectMax();
            marked = true;
        }
    }
    if (marked)
        eui::NavMarker(mark_min, mark_max);
    ImGui::EndChild();
    ImGui::PopFont();
}

void GUI::draw_floating(std::vector<TabPage> const& pages) {
    auto& logic = Logic::get();
    StyleVarGuard fade;
    GUI::get().push_open_fade(fade);
    auto is_open = [&](char const* name) {
        return std::find(logic.open_pages.begin(), logic.open_pages.end(), name) != logic.open_pages.end();
    };
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        for (auto const& place : logic.tile_places) {
            auto& state = eui::tile_states()[place.key];
            state.pinned = place.pinned;
            state.hidden = place.hidden;
            state.x = place.x;
            state.y = place.y;
        }
    }
    auto set_open = [&](char const* name, bool on) {
        auto at = std::find(logic.open_pages.begin(), logic.open_pages.end(), name);
        if (on && at == logic.open_pages.end())
            logic.open_pages.emplace_back(name);
        if (!on && at != logic.open_pages.end())
            logic.open_pages.erase(at);
        if (on) {
            auto prefix = std::string(name) + "/";
            for (auto& [key, state] : eui::tile_states()) {
                if (key.starts_with(prefix))
                    state.hidden = false;
            }
            eui::tiles_changed() = true;
        }
        Logic::get().save_settings_file();
    };
    ImGuiStyle const& style = ImGui::GetStyle();
    float font = ImGui::GetFontSize();
    float margin = font * .8f;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

    static float bar_tall = 0.f;
    float opened = GUI::get().motion.open;
    float drop = GUI::get().closing ? GUI::get().smoothstep01(opened) : eui::glide_in(opened);
    ImGui::SetNextWindowPos(ImVec2(margin, margin - (1.f - drop) * (bar_tall + margin)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(display.x - margin * 2.f, 0.f), ImGuiCond_Always);
    float bar_bottom = margin;
    if (ImGui::Begin("###echo-bar", nullptr, fixed)) {
        int item = 0;
        float items = (float)(pages.size() + 5);
        auto enter = [&] {
            eui::BeginEntrance((opened - .55f * (float)(item++) / items) / .45f);
        };
        float height = ImGui::GetFrameHeight();
        if (ImTextureID logo = GUI::get().theme_logo()) {
            enter();
            ImGui::Image(ImTextureRef(logo), ImVec2(height, height));
            eui::EndEntrance();
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        enter();
        ImGui::TextUnformatted("Echo");
        eui::EndEntrance();
        ImGui::SameLine();
        enter();
        ImGui::TextDisabled("%s", Mod::get()->getVersion().toVString().c_str());
        eui::EndEntrance();
        for (auto const& page : pages) {
            ImGui::SameLine(0.f, style.ItemSpacing.x * .6f);
            bool open = is_open(page.name);
            enter();
            bool pressed = eui::NavItem(page.name, icon_for(page.name), open, GUI::get().labelWidth(page.name));
            eui::EndEntrance();
            if (pressed)
                set_open(page.name, !open);
        }
        ImGui::SameLine(0.f, font);
        enter();
        GUI::get().draw_status(false);
        eui::EndEntrance();
        float close_w = GUI::get().labelWidth("Close"), reset_w = GUI::get().labelWidth("Reset Layout");
        ImGui::SameLine(ImGui::GetWindowWidth() - style.WindowPadding.x - close_w - style.ItemSpacing.x - reset_w);
        enter();
        bool reset = eui::Button("Reset Layout", ImVec2(reset_w, 0.f));
        eui::EndEntrance();
        if (reset) {
            for (auto& [key, state] : eui::tile_states()) {
                state.pinned = false;
                state.hidden = false;
            }
            eui::tiles_changed() = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Unpins all tiles and shows the hidden ones.");
        ImGui::SameLine();
        enter();
        bool close = eui::Button("Close", ImVec2(close_w, 0.f));
        eui::EndEntrance();
        if (close)
            GUI::get().begin_close();
        GUI::get().offer_rebind("open-menu");
        bar_tall = ImGui::GetWindowSize().y;
        bar_bottom = margin + bar_tall;
    }
    ImGui::End();

    eui::BeginTiles(margin, bar_bottom + font * .55f, display.x - margin, display.y - margin, 18.f, GUI::get().motion.open, GUI::get().anim_rate());
    for (auto const& page : pages) {
        if (!is_open(page.name))
            continue;
        eui::TileGroup(page.name);
        auto host = fmt::format("###echo-host-{}", page.name);
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1.f, 1.f), ImGuiCond_Always);
        if (ImGui::Begin(host.c_str(), nullptr, fixed | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing)) {
            page.draw();
        }
        ImGui::End();
    }
    eui::EndTiles();
    if (eui::tiles_changed()) {
        eui::tiles_changed() = false;
        logic.tile_places.clear();
        for (auto const& [key, state] : eui::tile_states()) {
            if (state.pinned || state.hidden)
                logic.tile_places.push_back({ key, state.pinned, state.hidden, state.x, state.y });
        }
        Logic::get().save_settings_file();
    }
}

void GUI::draw_frame_bar() {
    auto& gui = GUI::get();
    auto& logic = Logic::get();
    auto* play = PlayLayer::get();
    if (!play)
        return;
    float font = ImGui::GetFontSize();
    // drag it by the frame number. saved as a part of the screen so it lands the same after a rotate
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 spot(display.x * .5f, font * .5f);
    if (logic.frame_bar_x >= 0.f && logic.frame_bar_y >= 0.f)
        spot = ImVec2(logic.frame_bar_x * display.x, logic.frame_bar_y * display.y);
    float half = std::max(gui.bar_area.z - gui.bar_area.x, 0.f) * .5f, tall = std::max(gui.bar_area.w - gui.bar_area.y, 0.f);
    spot.x = std::clamp(spot.x, half, std::max(display.x - half, half));
    spot.y = std::clamp(spot.y, 0.f, std::max(display.y - tall, 0.f));
    ImGui::SetNextWindowPos(spot, ImGuiCond_Always, ImVec2(.5f, 0.f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("###echo-frame-bar", nullptr, flags)) {
#ifdef GEODE_IS_MOBILE
        // big enough to hit w/ a thumb, & the same size whatever the menu got scaled to
        ImGui::SetWindowFontScale(1.25f / std::max(gui.applied_scale, .35f));
        font = ImGui::GetFontSize();
        ImVec2 size(font * 4.4f, font * 2.3f);
#else
        ImGui::SetWindowFontScale(.85f);
        font = ImGui::GetFontSize();
        ImVec2 size(font * 4.f, font * 1.9f);
#endif
        ImVec2 at = ImGui::GetCursorPos();
        ImGui::SetCursorPosY(at.y + (size.y - font) * .5f);
        ImGui::TextDisabled("Frame %d", logic.get_frame(play) + 1);
        ImGui::SetCursorPos(ImVec2(at.x + font * 6.f, at.y));
        auto button = [&](char const* label, int which, bool enabled, ImVec4& where) {
            ImGui::BeginDisabled(!enabled);
            bool held = gui.bar_pressed == which;
            if (held)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            eui::Button(label, size);
            if (held)
                ImGui::PopStyleColor();
            ImGui::EndDisabled();
            ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            where = enabled ? ImVec4(min.x, min.y, max.x, max.y) : ImVec4();
        };
        button("Back", 1, logic.backstep, gui.bar_back);
        ImGui::SameLine();
        button("Step", 2, true, gui.bar_step);
        ImGui::SameLine();
        button("Best", 4, logic.backstep, gui.bar_best);
        ImGui::SameLine();
        button(logic.frame_advance ? "Resume" : "Pause", 3, true, gui.bar_resume);
        ImVec2 min = ImGui::GetWindowPos();
        ImVec2 size2 = ImGui::GetWindowSize();
        gui.bar_area = ImVec4(min.x, min.y, min.x + size2.x, min.y + size2.y);
    }
    ImGui::End();
}

void GUI::draw_window() {
    if (GUI::get().bar_only) {
        GUI::get().motion.open = 0.f;
        GUI::get().closing = false;
        GUI::get().sync_menu_scale();
        GUI::get().draw_frame_bar();
        return;
    }
    if (!ImGuiCocos::get().isVisible()) {
        GUI::get().motion.open = 0.f;
        GUI::get().closing = false;
        return;
    }
    GUI::get().tick_motion();
    if (!ImGuiCocos::get().isVisible())
        return;
    GUI::get().sync_menu_scale();
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(GUI::BIND_POPUP) && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGuiContext& context = *ImGui::GetCurrentContext();
        if (context.OpenPopupStack.Size > 0)
            ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
    }
#ifdef GEODE_IS_MOBILE
    if (!GUI::get().closing && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive() &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        GUI::get().begin_close();
    }
#endif
    auto pages = GUI::get().ordered_pages();
    if (!Logic::get().docked_menu) {
        GUI::get().dock_return = true;
        GUI::get().draw_floating(pages);
        GUI::get().show_keybind_prompt();
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
    static bool settled = false;
    bool arriving = settle && !settled;
    settled = settle;
    bool centered = GUI::get().at_scale_cap;
#ifdef GEODE_IS_MOBILE
    centered = true;
#endif
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImVec2 home = GUI::get().dock_home;
    if (centered)
        home = ImVec2(std::max(0.f, (display.x - GUI::get().dock_size.x) * .5f), std::max(0.f, (display.y - GUI::get().dock_size.y) * .5f));
    if (centered && settle) {
        bool tall = GUI::get().dock_size.y > display.y;
        ImGui::SetNextWindowPos(ImVec2(display.x * .5f, tall ? 0.f : display.y * .5f), ImGuiCond_Always, ImVec2(.5f, tall ? 0.f : .5f));
    }
    else if (returning || arriving) {
        ImGui::SetNextWindowPos(home, ImGuiCond_Always);
    }
    else if (!settle) {
        ImVec2 origin = GUI::get().offscreen(GUI::get().dock_slide, home, GUI::get().dock_size, display);
        float along = GUI::get().closing ? eased : eui::glide_in(GUI::get().motion.open);
        ImVec2 pos(std::lerp(origin.x, home.x, along), std::lerp(origin.y, home.y, along));
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    }
    bool open = true;
    if (!ImGui::Begin(menu_title(), &open, GUI::get().menu_flags() | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::End();
        if (!open)
            GUI::get().begin_close();
        return;
    }
    if (!open)
        GUI::get().begin_close();
    if (settle && !returning && !arriving) {
        if (!centered)
            GUI::get().dock_home = ImGui::GetWindowPos();
        GUI::get().dock_size = ImGui::GetWindowSize();
    }
    {
        float scale = std::max(GUI::get().applied_scale, .01f);
        static float steady_scale = 0.f;
        static int steady_frames = 0;
        steady_frames = scale == steady_scale ? steady_frames + 1 : 0;
        steady_scale = scale;
        if (steady_frames >= 3) {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            ImVec2 pad = ImGui::GetStyle().WindowPadding;
            ImVec2 needs(window->ContentSize.x + pad.x * 2.f, window->ContentSize.y + pad.y * 2.f);
            GUI::get().dock_unit = ImVec2(std::max(GUI::get().dock_unit.x, needs.x / scale), std::max(GUI::get().dock_unit.y, needs.y / scale));
        }
    }
    if (eui::Header("Echo", Mod::get()->getVersion().toVString().c_str(), GUI::get().theme_logo()))
        GUI::get().begin_close();
    static float page_height = 0.f;
    static float page_width = 0.f;
    static float measured_at = 0.f;
    static bool settings_measured = false;
    if (measured_at != ImGui::GetFontSize()) {
        measured_at = ImGui::GetFontSize();
        page_height = 0.f;
        page_width = 0.f;
        settings_measured = false;
    }
    float nav_top = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y;
    // how tall the page part is allowed to be on a phone: the menu never gets taller than the screen. at the default scale
    // every page fits & is stretched to end where the sidebar ends. at a bigger scale the page scrolls inside (drag it)
    // & the header & sidebar stay put
    float view_cap = FLT_MAX;
#ifdef GEODE_IS_MOBILE
    {
        ImGuiStyle const& style = ImGui::GetStyle();
        float under = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.f + style.WindowPadding.y;
        view_cap = std::max(display.y * .98f - nav_top - under, ImGui::GetFrameHeight() * 6.f);
    }
#endif
    float fill = std::min(page_height, view_cap);
    GUI::get().draw_nav(pages, fill);
    {
        ImGuiStyle const& style = ImGui::GetStyle();
        float least = nav_top + GUI::get().nav_needs + ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.f + style.WindowPadding.y;
        GUI::get().dock_least_tall = least / std::max(GUI::get().applied_scale, .01f);
    }
    ImGui::SameLine();
    bool boxed = settings_measured && page_height > view_cap + 1.f;
    // control sizes on mobile: the page keeps the room it has at 1x & everything in it is drawn bigger, and do only 1 card per row
    // what doesnt fit we just scroll. there is probably a better way to do this too
    // TODO: brainstorm a better way to scale UI for fat thumbs
    float inner = 1.f;
    // off for now: on a real phone every card came out as just its heading. the setting is kept, nothing reads it
#if 0
    inner = std::clamp(Logic::get().menu_inner_scale, 1.f, 1.5f);
#endif
    bool roomy = settings_measured && inner > 1.02f && page_width > 1.f && fill > 1.f;
    auto finish = [] {
        GUI::get().draw_status();
        GUI::get().show_keybind_prompt();
        GUI::get().draw_macro_picker();
        GUI::get().draw_theme_picker();
        GUI::get().draw_clickpack_picker();
        GUI::get().draw_render_picker();
        // best frame, 1st time in a lvl that moves things w/ moving objects off for the trajectory
        auto& logic = Logic::get();
        if (logic.ask_moving_objects && !ImGui::IsPopupOpen("Moving Objects"))
            ImGui::OpenPopup("Moving Objects");
        if (eui::BeginModal("Moving Objects", 22.f)) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted("This level moves objects and the trajectory is not following them, so Best Frame may not be accurate.");
            ImGui::TextDisabled("Turn on Moving Objects for the trajectory?");
            ImGui::PopTextWrapPos();
            float half = eui::RowButton(2);
            int picked = 0;
            if (eui::Button("Keep Off", ImVec2(half, 0.f)))
                picked = 1;
            ImGui::SameLine();
            if (eui::Button("Turn On", ImVec2(half, 0.f)))
                picked = 2;
            if (picked) {
                if (picked == 2) {
                    logic.traj_moving_objects = true;
                    logic.save_settings_file();
                }
                logic.ask_moving_objects = false;
                ImGui::CloseCurrentPopup();
                GUI::get().begin_close();
                logic.request_best_frame();
            }
            eui::EndModal();
        }
        ImGui::End();
    };
    if (roomy) {
        ImGui::BeginChild("##echo-page-roomy", ImVec2(page_width, fill), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        // NOT SetWindowFontScale
        ImGui::PushFont(nullptr, ImGui::GetCurrentContext()->FontSizeBase * inner);
        ImGuiStyle const& style = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * inner, style.FramePadding.y * inner));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x * inner, style.ItemSpacing.y * inner));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(style.ItemInnerSpacing.x * inner, style.ItemInnerSpacing.y * inner));
        eui::single_column() = true;
        if (auto* page = GUI::get().active_page(pages)) {
            GUI::get().page = page->name;
            GUI::get().fade_page(page->name, page->draw);
        }
        eui::single_column() = false;
        ImGui::PopStyleVar(3);
        ImGui::PopFont();
        eui::ScrollMark();
        ImGui::EndChild();
        finish();
        return;
    }
    if (boxed)
        ImGui::BeginChild("##echo-page", ImVec2(0.f, view_cap), ImGuiChildFlags_AutoResizeX, ImGuiWindowFlags_NoScrollbar);
    ImGui::BeginGroup();
    eui::last_card_row() = 0;
    bool measuring = false;
#ifdef GEODE_IS_MOBILE
    if (!settings_measured) {
        settings_measured = true;
        measuring = true;
        ImVec2 start = ImGui::GetCursorPos();
        std::vector<std::pair<ImGuiID, float>> rows;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.f);
        for (auto const& page : pages) {
            ImGui::SetCursorPos(start);
            ImGui::BeginGroup();
            eui::last_card_row() = 0;
            page.draw();
            ImGui::EndGroup();
            ImGuiID row = eui::single_column() ? 0 : eui::last_card_row();
            float natural = ImGui::GetItemRectSize().y - (row ? eui::card_row_extra(row) : 0.f);
            page_height = std::max(page_height, natural);
            page_width = std::max(page_width, ImGui::GetItemRectSize().x);
            rows.emplace_back(row, natural);
        }
        ImGui::PopStyleVar();
        for (auto const& [row, natural] : rows) {
            if (!row)
                continue;
            float spare = std::min(page_height, view_cap) - natural;
            eui::card_row_extra(row) = spare > 1.f ? spare : 0.f;
        }
    }
#endif
    if (!measuring) {
        if (auto* page = GUI::get().active_page(pages)) {
            GUI::get().page = page->name;
            GUI::get().fade_page(page->name, page->draw);
        }
    }
    ImGui::EndGroup();
    ImGuiID last_row = measuring || eui::single_column() ? 0 : eui::last_card_row();
    float natural = measuring ? page_height : ImGui::GetItemRectSize().y - (last_row ? eui::card_row_extra(last_row) : 0.f);
    page_height = std::max(page_height, natural);
    if (last_row) {
        float spare = std::min(page_height, view_cap) - natural;
        eui::card_row_extra(last_row) = spare > 1.f ? spare : 0.f;
    }
    if (boxed) {
        eui::ScrollMark();
        ImGui::EndChild();
    }
    finish();
}

$on_mod(Loaded) {
    ImGuiCocos::get().setup([] {
                         GUI::get().load_menu_font();
                         eui::apply_echo_theme();
                         GUI::get().apply_saved_theme();
                         ImGuiCocos::get().setVisible(false);
                     })
        .draw([] {
            GUI::get().touch_scroll();
            GUI::get().draw_window();
            eui::WindowRims(GUI::get().bar_only ? 1.f : GUI::get().smoothstep01(GUI::get().motion.open));
        });
}

void GUI::draw_status(bool rule) {
    auto& logic = Logic::get();
    ImVec4 tint = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    if (logic.is_recording())
        tint = ImVec4(1.f, .36f, .44f, 1.f);
    else if (logic.is_playing())
        tint = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    if (rule)
        eui::Separator();
    else
        ImGui::AlignTextToFramePadding();
    eui::StatusDot(tint);
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(logic.state_name(logic.state));
    auto note = [](std::string const& text) {
        ImGui::SameLine(0.f, ImGui::GetFontSize() * .9f);
        ImGui::TextDisabled("%s", text.c_str());
    };
    if (auto* play = PlayLayer::get())
        note(fmt::format("Frame {}", logic.get_frame(play) + 1));
    note(fmt::format("{} inputs", logic.inputs.size()));
    note(fmt::format("{:g} TPS", logic.fps));
    note(fmt::format("{:g}x speed", logic.speedhack));
    if (logic.frame_advance) {
        ImGui::SameLine(0.f, ImGui::GetFontSize() * .9f);
        eui::StatusDot(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextUnformatted("Frame Advance");
    }
}

void GUI::sync_menu_scale() {
    auto& logic = Logic::get();
    auto& gui = GUI::get();
    gui.scale_cap = 3.f;
    if (logic.docked_menu && gui.dock_unit.x > 1.f) {
        ImVec2 display = ImGui::GetIO().DisplaySize;
        float fits = display.x / gui.dock_unit.x;
        if (gui.dock_least_tall > 1.f)
            fits = std::min(fits, display.y / gui.dock_least_tall);
        if (std::isfinite(fits) && fits > 0.f)
            gui.scale_cap = std::clamp(fits, 0.35f, 3.f);
    }
#ifdef GEODE_IS_MOBILE
    gui.scale_cap = std::min(gui.scale_cap, 1.f);
#endif
    // dont keep the scale
    float& saved = logic.docked_menu ? logic.menu_scale : logic.menu_scale_undocked;
    float scale = saved;
    if (Logic::get().render_active())
        scale = std::clamp(scale, 0.35f, gui.scale_cap);
    else
        scale = saved = std::clamp(saved, 0.35f, gui.scale_cap);
    gui.at_scale_cap = logic.docked_menu && gui.scale_cap < 3.f && scale >= gui.scale_cap - .004f;
    if (!GUI::get().has_base) {
        GUI::get().base_style = ImGui::GetStyle();
        GUI::get().base_style._MainScale = 1.f;
        GUI::get().has_base = true;
    }
    float total = scale * touch_scale();
    ImGuiStyle scaled = GUI::get().base_style;
    scaled.ScaleAllSizes(total);
    ImGuiStyle& live = ImGui::GetStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        scaled.Colors[i] = live.Colors[i];
    scaled.Alpha = live.Alpha;
    live = scaled;
    ImGui::GetIO().FontGlobalScale = total;
    GUI::get().applied_scale = scale;
    apply_touch_style();
}

float GUI::touch_scale() {
#ifdef GEODE_IS_MOBILE
    return std::clamp(ImGui::GetIO().DisplaySize.y / 620.f, 1.f, 2.2f);
#else
    return 1.f;
#endif
}

void GUI::apply_touch_style() {
#ifdef GEODE_IS_MOBILE
    if (!GUI::get().has_base)
        return;
    auto& style = ImGui::GetStyle();
    auto const& base = GUI::get().base_style;
    float grow = touch_scale();
    (void)base;
    style.TouchExtraPadding = ImVec2(std::max(style.TouchExtraPadding.x, 4.f * grow), std::max(style.TouchExtraPadding.y, 4.f * grow));
    // a phone scrolls by dragging anywhere (touch_scroll), nobody grabs the bar. a fat one just ate room from the rows & sat on top of them
    style.ScrollbarSize = 4.f * grow;
    style.GrabMinSize = std::max(style.GrabMinSize, 14.f * grow);
    auto& io = ImGui::GetIO();
    io.MouseDoubleClickMaxDist = 30.f * grow;
    io.MouseDoubleClickTime = .4f;
    // fix double tap
    io.MouseDragThreshold = std::max(io.MouseDragThreshold, 10.f * grow);
#endif
}

void GUI::touch_scroll() {
    auto& io = ImGui::GetIO();
    ImGuiContext& g = *ImGui::GetCurrentContext();
    static ImGuiWindow* target = nullptr;
    static bool scrolling = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        target = nullptr;
        scrolling = false;
        return;
    }
    if (io.MouseSource != ImGuiMouseSource_TouchScreen)
        return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        target = g.HoveredWindow;
        scrolling = false;
    }
    if (!target || io.WantTextInput)
        return;
    if (g.ActiveId && g.ActiveIdWindow && g.ActiveId == ImGui::GetWindowScrollbarID(g.ActiveIdWindow, ImGuiAxis_Y))
        return;
    // something that is dragged up & down itself (the label grips) has the finger, so no scrolling
    if (g.ActiveId && g.ActiveId == GUI::get().scroll_hold)
        return;
    ImGuiWindow* window = target;
    while (window && window->ScrollMax.y <= 0.f)
        window = window->ParentWindow;
    if (!window)
        return;
    if (!scrolling) {
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
        if (std::abs(drag.y) < 10.f * touch_scale() || std::abs(drag.y) < std::abs(drag.x) * 1.5f)
            return;
        scrolling = true;
        ImGui::ClearActiveID();
    }
    ImGui::SetScrollY(window, std::clamp(window->Scroll.y - io.MouseDelta.y, 0.f, window->ScrollMax.y));
}

void GUI::restore_base_sizes() {
    if (!GUI::get().has_base)
        return;
    ImGuiStyle& live = ImGui::GetStyle();
    ImGuiStyle plain = GUI::get().base_style;
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        plain.Colors[i] = live.Colors[i];
    plain.Alpha = live.Alpha;
    live = plain;
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
    for (int button = 0; button < 5; ++button)
        io.AddMouseButtonEvent(button, false);
}

void GUI::open_popup() {
    auto& imgui = ImGuiCocos::get();
    if (!imgui.isInitialized())
        return;
    if (!imgui.isVisible() || GUI::get().bar_only) {
        GUI::get().bar_only = false;
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

bool GUI::menu_shown() {
    auto& imgui = ImGuiCocos::get();
    return imgui.isInitialized() && imgui.isVisible() && !GUI::get().bar_only;
}

// show cursor in game being off fix
void GUI::sync_cursor() {
#ifdef GEODE_IS_DESKTOP
    static bool forced = false;
    auto* play = PlayLayer::get();
    bool hidden_in_game = play && !GameManager::get()->getGameVariable("0024");
    if (hidden_in_game && GUI::get().menu_shown()) {
        PlatformToolbox::showCursor();
        forced = true;
    }
    else if (forced) {
        forced = false;
        if (hidden_in_game && !play->m_isPaused && !play->m_hasCompletedLevel)
            PlatformToolbox::hideCursor();
    }
#endif
}

int GUI::menu_frames() {
    return ImGuiCocos::get().isInitialized() ? ImGui::GetFrameCount() : 0;
}

void GUI::draw_menu_now() {
    if (ImGuiCocos::get().isInitialized())
        ImGuiCocos::get().drawFrame();
}

void GUI::reveal_menu() {
    auto& imgui = ImGuiCocos::get();
    if (!imgui.isInitialized())
        return;
    GUI::get().bar_only = false;
    if (!imgui.isVisible())
        imgui.setVisible(true);
}
