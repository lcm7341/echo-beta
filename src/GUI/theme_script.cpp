#include "GUI/theme_script.hpp"
#include "GUI/gui.hpp"
#include "GUI/logo_texture.hpp"
#include "Logic/conversions/codecs.hpp"
#include "Script/host.hpp"
#include "Script/host_helpers.hpp"
#include "Script/lua_runtime.hpp"

#include <Geode/Geode.hpp>
#include <imgui_internal.h>
#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;
using Script::read_text;
using Script::set_bool;
using Script::set_number;
using Script::take_function;

#define THEME_MEMORY_LIMIT 67108864ull
#define THEME_FRAME_INSTRUCTIONS 20000000
#define THEME_LOAD_INSTRUCTIONS 50000000
#define RELOAD_CHECK_SECONDS 0.5
#define DEFAULT_EASE_SPEED 14.0
#define MAX_NEW_FOLDERS 1000
#define MAX_TEXT_BYTES 65536
#define MAX_SHAPE_POINTS 4096
#define MAX_THEME_IMAGES 256
#define MAX_CLIP_DEPTH 64
#define MAX_EASE_KEYS 65536
#define MAX_TEXT_SIZE 512.f
#define MENU_FONT_SIZE 18.f
#define LOGO_RINGS "rings"
#define BUNDLED_THEME_PREFIX "echo-theme-"
#define GUIDE_FILE "echo-theme-guide.md"
#define FREEZE_MARKER_FILE "script-froze.txt"

namespace ThemeScript {
    bool g_hooks[(int)Hook::Count] = {};
}

using ThemeScript::Widget;

static const char* const NEW_THEME = R"(theme = {
    palette = { body = 0.75, accent = 0.9 },
}

function button(w)
    w.default()
    draw.rect(w.x, w.y, w.x + w.w, w.y + w.h, fade(colors.accent, w.hover), w.rounding, 1)
end
)";

static const char* const HOOK_NAMES[] = { "window_background", "window_frame", "card", "header", "button", "action_button", "checkbox", "field", "slider", "selectable",
    "nav_item", "nav_marker", "separator", "heading", "progress", "close_button", "gear", "help", "status_dot", "swatch", "scroll_mark", "logo", "title_bar", "nav_bar",
    "status_bar", "bar", "tile_bar", "collapse_bar", "theme_tile", "floating_logo" };
static_assert(std::size(HOOK_NAMES) == (size_t)ThemeScript::Hook::Count);

struct MetricSlot {
    const char* name;
    float ImGuiStyle::* single;
    ImVec2 ImGuiStyle::* pair;
};

static const MetricSlot METRICS[] = {
    { "window_rounding", &ImGuiStyle::WindowRounding, nullptr },
    { "child_rounding", &ImGuiStyle::ChildRounding, nullptr },
    { "popup_rounding", &ImGuiStyle::PopupRounding, nullptr },
    { "frame_rounding", &ImGuiStyle::FrameRounding, nullptr },
    { "grab_rounding", &ImGuiStyle::GrabRounding, nullptr },
    { "scrollbar_rounding", &ImGuiStyle::ScrollbarRounding, nullptr },
    { "tab_rounding", &ImGuiStyle::TabRounding, nullptr },
    { "window_border", &ImGuiStyle::WindowBorderSize, nullptr },
    { "child_border", &ImGuiStyle::ChildBorderSize, nullptr },
    { "popup_border", &ImGuiStyle::PopupBorderSize, nullptr },
    { "frame_border", &ImGuiStyle::FrameBorderSize, nullptr },
    { "tab_border", &ImGuiStyle::TabBorderSize, nullptr },
    { "indent", &ImGuiStyle::IndentSpacing, nullptr },
    { "scrollbar_size", &ImGuiStyle::ScrollbarSize, nullptr },
    { "grab_min_size", &ImGuiStyle::GrabMinSize, nullptr },
    { "window_padding", nullptr, &ImGuiStyle::WindowPadding },
    { "frame_padding", nullptr, &ImGuiStyle::FramePadding },
    { "item_spacing", nullptr, &ImGuiStyle::ItemSpacing },
    { "item_inner_spacing", nullptr, &ImGuiStyle::ItemInnerSpacing },
    { "cell_padding", nullptr, &ImGuiStyle::CellPadding },
};

struct DrawTarget {
    ImDrawList* list = nullptr;
    float alpha = 1.f;
    int clips = 0;
};

struct LookCall {
    long long token = 0;
    void (*run)(void*) = nullptr;
    void* context = nullptr;
};

struct ThemeInfo {
    std::string name, logo;
    bool paints_logo = false;
    EchoUI::LogoPaint logo_paints[LOGO_PART_COUNT];
    bool has_palette = false;
    EchoUI::Palette palette;
    std::vector<std::pair<int, ImVec4>> colors;
    int functions[(int)ThemeScript::Hook::Count] = {};
    int update = LUA_NOREF, overlay = LUA_NOREF, background = LUA_NOREF;
    ThemeScript::Layout layout;
    std::vector<std::pair<int, ImVec2>> metrics;
    bool sharp = false;
};

struct PendingRun {
    bool waiting = false;
    bool saved = false;
    std::string text;
    fs::path file;
};

enum class PaletteUse {
    Current,
    Preview,
    Save
};

static std::unique_ptr<Script::Runtime> g_runtime;
static fs::path g_file;
static fs::file_time_type g_written;
static double g_checked_at = 0.0;
static double g_floating_time = 0.0;
static std::string g_error;
static bool g_stopping = false;
static ThemeInfo g_theme;
static int g_layout_version = 0;
static ImVec4 g_window_color;
static std::unordered_map<std::string, CCTexture2D*> g_images;
static CCTexture2D* g_logo = nullptr;
static CCTexture2D* g_painted_logo = nullptr;
static DrawTarget g_target;
static LookCall g_look;
static long long g_look_count = 0;
static std::unordered_set<ImGuiID> g_ease_keys;
static std::deque<std::string> g_output;
static PendingRun g_pending;

static lua_Integer packed(ImVec4 tint) {
    auto byte = [](float channel) {
        return (lua_Integer)std::lround(std::clamp(channel, 0.f, 1.f) * 255.f);
    };
    return (byte(tint.x) << 24) | (byte(tint.y) << 16) | (byte(tint.z) << 8) | byte(tint.w);
}

static ImVec4 unpacked(lua_Integer value) {
    auto channel = [&](int shift) {
        return (float)((value >> shift) & 255) / 255.f;
    };
    return ImVec4(channel(24), channel(16), channel(8), channel(0));
}

static bool parse_hex(const char* text, ImVec4& out) {
    const int short_digits = 6, long_digits = 8, hex_base = 16;
    if (*text == '#') ++text;
    int digits = (int)std::strlen(text);
    if (digits != short_digits && digits != long_digits) return false;
    for (int i = 0; i < digits; ++i) {
        if (!std::isxdigit((unsigned char)text[i])) return false;
    }
    lua_Integer value = (lua_Integer)std::strtoull(text, nullptr, hex_base);
    if (digits == short_digits) value = (value << 8) | 255;
    out = unpacked(value);
    return true;
}

static ImVec4 read_color(lua_State* lua, int index) {
    index = lua_absindex(lua, index);
    if (lua_type(lua, index) == LUA_TNUMBER) {
        int whole = 0;
        lua_Integer value = lua_tointegerx(lua, index, &whole);
        if (!whole) luaL_argerror(lua, index, "a color number has to be whole, like 0xff8800ff");
        return unpacked(value);
    }
    if (lua_type(lua, index) == LUA_TSTRING) {
        ImVec4 out;
        if (!parse_hex(lua_tostring(lua, index), out))
            luaL_argerror(lua, index, "colors look like \"#RRGGBB\" or \"#RRGGBBAA\"");
        return out;
    }
    if (lua_istable(lua, index)) {
        ImVec4 out(0.f, 0.f, 0.f, 1.f);
        float* channels[] = { &out.x, &out.y, &out.z, &out.w };
        const char* names[] = { "r", "g", "b", "a" };
        for (int i = 0; i < 4; ++i) {
            lua_rawgeti(lua, index, i + 1);
            if (lua_isnil(lua, -1)) {
                lua_pop(lua, 1);
                lua_getfield(lua, index, names[i]);
            }
            if (lua_isnumber(lua, -1)) *channels[i] = (float)lua_tonumber(lua, -1);
            lua_pop(lua, 1);
        }
        return out;
    }
    luaL_argerror(lua, index, "expected a color");
    return ImVec4();
}

static ImU32 draw_color(lua_State* lua, int index) {
    ImVec4 tint = read_color(lua, index);
    tint.w *= g_target.alpha;
    return ImGui::ColorConvertFloat4ToU32(tint);
}

static ImU32 draw_color_or_white(lua_State* lua, int index) {
    if (lua_isnoneornil(lua, index))
        return ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, g_target.alpha));
    return draw_color(lua, index);
}

static ImVec2 point(lua_State* lua, int index) {
    return ImVec2((float)luaL_checknumber(lua, index), (float)luaL_checknumber(lua, index + 1));
}

static float number(lua_State* lua, int index, float fallback) {
    return (float)luaL_optnumber(lua, index, fallback);
}

static long long whole_number(lua_State* lua, int index, lua_Number fallback) {
    const lua_Number limit = 9.0e18;
    lua_Number value = luaL_optnumber(lua, index, fallback);
    if (!(value > -limit && value < limit)) return 0;
    return (long long)value;
}

static float font_unit() {
    float unit = ImGui::GetFontSize();
    return unit > 0.f ? unit : MENU_FONT_SIZE;
}

static float text_size(lua_State* lua, int index) {
    float size = number(lua, index, font_unit());
    if (!(size > 0.f && size <= MAX_TEXT_SIZE))
        luaL_argerror(lua, index, lua_pushfstring(lua, "text size has to be above 0 and at most %d", (int)MAX_TEXT_SIZE));
    return size;
}

static void read_points(lua_State* lua, int index, std::vector<ImVec2>& points) {
    luaL_checktype(lua, index, LUA_TTABLE);
    lua_Integer count = luaL_len(lua, index);
    if (count > MAX_SHAPE_POINTS * 2)
        luaL_error(lua, "a shape can have at most %d points", MAX_SHAPE_POINTS);
    for (lua_Integer i = 1; i + 1 <= count; i += 2) {
        lua_rawgeti(lua, index, i);
        lua_rawgeti(lua, index, i + 1);
        if (!lua_isnumber(lua, -2) || !lua_isnumber(lua, -1))
            luaL_error(lua, "points are a list of numbers: { x1, y1, x2, y2, ... }");
        points.emplace_back((float)lua_tonumber(lua, -2), (float)lua_tonumber(lua, -1));
        lua_pop(lua, 2);
    }
}

static bool safe_relative(const std::string& text, fs::path& out) {
    out = ReplayIO::utf8_path(text);
    if (out.empty() || out.has_root_path()) return false;
    for (const auto& part : out) {
        if (part == "..") return false;
    }
    return true;
}

static CCTexture2D* image_named(const std::string& name) {
    fs::path relative;
    if (!safe_relative(name, relative)) return nullptr;
    fs::path full = g_file.parent_path() / relative;
    std::string full_name = geode::utils::string::pathToString(full);
    auto found = g_images.find(full_name);
    if (found != g_images.end()) return found->second;
    if (g_images.size() >= MAX_THEME_IMAGES) return nullptr;
    CCTexture2D* texture = nullptr;
    std::error_code error;
    if (fs::is_regular_file(full, error))
        texture = CCTextureCache::sharedTextureCache()->addImage(full_name.c_str(), false);
    if (texture) texture->retain();
    g_images[full_name] = texture;
    return texture;
}

static void forget_images() {
    for (auto& [name, texture] : g_images) {
        if (!texture) continue;
        CCTextureCache::sharedTextureCache()->removeTexture(texture);
        texture->release();
    }
    g_images.clear();
    g_logo = nullptr;
    if (g_painted_logo) g_painted_logo->release();
    g_painted_logo = nullptr;
}

static ImTextureRef texture_ref(CCTexture2D* texture) {
    return ImTextureRef((ImTextureID)texture->getName());
}

static void shade_between(ImDrawList* list, int from, ImVec2 start, ImVec2 end, ImVec4 first, ImVec4 second) {
    ImVec2 span(end.x - start.x, end.y - start.y);
    float length = span.x * span.x + span.y * span.y;
    for (int v = std::max(from, 0); v < list->VtxBuffer.Size; ++v) {
        auto& vert = list->VtxBuffer[v];
        float t = length > 0.f ? std::clamp(((vert.pos.x - start.x) * span.x + (vert.pos.y - start.y) * span.y) / length, 0.f, 1.f) : 0.f;
        ImVec4 tint = EchoUI::mix(first, second, t);
        tint.w *= (float)((vert.col >> IM_COL32_A_SHIFT) & 255) / 255.f;
        vert.col = ImGui::ColorConvertFloat4ToU32(tint);
    }
}

static int api_rgb(lua_State* lua) {
    lua_pushinteger(lua, packed(ImVec4((float)luaL_checknumber(lua, 1), (float)luaL_checknumber(lua, 2), (float)luaL_checknumber(lua, 3), number(lua, 4, 1.f))));
    return 1;
}

static int api_hsv(lua_State* lua) {
    float hue = (float)luaL_checknumber(lua, 1);
    hue -= std::floor(hue);
    ImVec4 tint(0.f, 0.f, 0.f, number(lua, 4, 1.f));
    ImGui::ColorConvertHSVtoRGB(hue, std::clamp((float)luaL_checknumber(lua, 2), 0.f, 1.f), std::clamp((float)luaL_checknumber(lua, 3), 0.f, 1.f), tint.x, tint.y, tint.z);
    lua_pushinteger(lua, packed(tint));
    return 1;
}

static int api_blend(lua_State* lua) {
    lua_pushinteger(lua, packed(EchoUI::mix(read_color(lua, 1), read_color(lua, 2), (float)luaL_checknumber(lua, 3))));
    return 1;
}

static int api_fade(lua_State* lua) {
    ImVec4 tint = read_color(lua, 1);
    tint.w *= (float)luaL_checknumber(lua, 2);
    lua_pushinteger(lua, packed(tint));
    return 1;
}

static int api_channels(lua_State* lua) {
    ImVec4 tint = read_color(lua, 1);
    lua_pushnumber(lua, tint.x);
    lua_pushnumber(lua, tint.y);
    lua_pushnumber(lua, tint.z);
    lua_pushnumber(lua, tint.w);
    return 4;
}

static int api_lerp(lua_State* lua) {
    double a = luaL_checknumber(lua, 1), b = luaL_checknumber(lua, 2);
    lua_pushnumber(lua, a + (b - a) * luaL_checknumber(lua, 3));
    return 1;
}

static int api_clamp(lua_State* lua) {
    double value = luaL_checknumber(lua, 1), low = luaL_checknumber(lua, 2), high = luaL_checknumber(lua, 3);
    lua_pushnumber(lua, std::clamp(value, std::min(low, high), std::max(low, high)));
    return 1;
}

static int api_smoothstep(lua_State* lua) {
    double from = luaL_checknumber(lua, 1), to = luaL_checknumber(lua, 2), x = luaL_checknumber(lua, 3);
    double t = to != from ? std::clamp((x - from) / (to - from), 0.0, 1.0) : (x < from ? 0.0 : 1.0);
    lua_pushnumber(lua, t * t * (3.0 - 2.0 * t));
    return 1;
}

static int api_noise(lua_State* lua) {
    const float limit = 1.0e9f;
    float x = (float)luaL_checknumber(lua, 1), y = number(lua, 2, 0.f);
    bool usable = std::fabs(x) < limit && std::fabs(y) < limit;
    lua_pushnumber(lua, usable ? EchoUI::noise(x, y) : 0.f);
    return 1;
}

static int api_stable_random(lua_State* lua) {
    luaL_checknumber(lua, 1);
    auto item = (unsigned)whole_number(lua, 1, 0.0);
    auto seed = (unsigned)whole_number(lua, 2, 0.0);
    lua_pushnumber(lua, EchoUI::unit_random(item, seed));
    return 1;
}

static ImGuiID ease_seed() {
    static ImGuiID seed = ImHashStr("echo theme script ease");
    return seed;
}

static int api_ease(lua_State* lua) {
    ImGuiID key = 0;
    if (lua_type(lua, 1) == LUA_TSTRING) {
        key = ImHashStr(lua_tostring(lua, 1), 0, ease_seed());
    }
    else {
        luaL_checknumber(lua, 1);
        long long whole = whole_number(lua, 1, 0.0);
        key = ImHashData(&whole, sizeof(whole), ease_seed());
    }
    if (!g_ease_keys.contains(key)) {
        if (g_ease_keys.size() >= MAX_EASE_KEYS)
            luaL_error(lua, "ease can track at most %d keys", MAX_EASE_KEYS);
        g_ease_keys.insert(key);
    }
    lua_pushnumber(lua, EchoUI::ease(key, (float)luaL_checknumber(lua, 2), (float)luaL_optnumber(lua, 3, DEFAULT_EASE_SPEED)));
    return 1;
}

static int api_draw_rect(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddRect(point(lua, 1), point(lua, 3), draw_color(lua, 5), number(lua, 6, 0.f), 0, number(lua, 7, 1.f));
    return 0;
}

static int api_draw_fill(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddRectFilled(point(lua, 1), point(lua, 3), draw_color(lua, 5), number(lua, 6, 0.f));
    return 0;
}

static int api_draw_gradient(lua_State* lua) {
    ImDrawList* list = g_target.list;
    if (!list) return 0;
    ImVec2 min = point(lua, 1), max = point(lua, 3);
    ImVec4 first = read_color(lua, 5), second = read_color(lua, 6);
    first.w *= g_target.alpha;
    second.w *= g_target.alpha;
    bool across = lua_toboolean(lua, 8);
    int from = list->VtxBuffer.Size;
    list->AddRectFilled(min, max, IM_COL32_WHITE, number(lua, 7, 0.f));
    shade_between(list, from, min, across ? ImVec2(max.x, min.y) : ImVec2(min.x, max.y), first, second);
    return 0;
}

static int api_draw_line(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddLine(point(lua, 1), point(lua, 3), draw_color(lua, 5), number(lua, 6, 1.f));
    return 0;
}

static int api_draw_circle(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddCircle(point(lua, 1), (float)luaL_checknumber(lua, 3), draw_color(lua, 4), (int)luaL_optinteger(lua, 6, 0), number(lua, 5, 1.f));
    return 0;
}

static int api_draw_disc(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddCircleFilled(point(lua, 1), (float)luaL_checknumber(lua, 3), draw_color(lua, 4), (int)luaL_optinteger(lua, 5, 0));
    return 0;
}

static int api_draw_ellipse(lua_State* lua) {
    if (!g_target.list) return 0;
    ImVec2 center = point(lua, 1), radius = point(lua, 3);
    ImU32 tint = draw_color(lua, 5);
    float turn = number(lua, 6, 0.f), thickness = number(lua, 7, 0.f);
    if (thickness > 0.f) g_target.list->AddEllipse(center, radius, tint, turn, 0, thickness);
    else g_target.list->AddEllipseFilled(center, radius, tint, turn);
    return 0;
}

static int api_draw_arc(lua_State* lua) {
    const float full_turn = 6.2831853f;
    if (!g_target.list) return 0;
    float from = (float)luaL_checknumber(lua, 4), to = (float)luaL_checknumber(lua, 5);
    if (!std::isfinite(from) || !std::isfinite(to)) return 0;
    to = std::clamp(to, from - full_turn, from + full_turn);
    g_target.list->PathArcTo(point(lua, 1), (float)luaL_checknumber(lua, 3), from, to);
    g_target.list->PathStroke(draw_color(lua, 6), 0, number(lua, 7, 1.f));
    return 0;
}

static int api_draw_triangle(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddTriangleFilled(point(lua, 1), point(lua, 3), point(lua, 5), draw_color(lua, 7));
    return 0;
}

static int api_draw_quad(lua_State* lua) {
    if (!g_target.list) return 0;
    g_target.list->AddQuadFilled(point(lua, 1), point(lua, 3), point(lua, 5), point(lua, 7), draw_color(lua, 9));
    return 0;
}

static int api_draw_polygon(lua_State* lua) {
    std::vector<ImVec2> points;
    read_points(lua, 1, points);
    ImU32 tint = draw_color(lua, 2);
    Script::Runtime::from(lua)->charge(lua, (long long)(points.size() * points.size()));
    if (g_target.list && points.size() >= 3)
        g_target.list->AddConcavePolyFilled(points.data(), (int)points.size(), tint);
    return 0;
}

static int api_draw_polyline(lua_State* lua) {
    std::vector<ImVec2> points;
    read_points(lua, 1, points);
    ImU32 tint = draw_color(lua, 2);
    Script::Runtime::from(lua)->charge(lua, (long long)points.size());
    if (g_target.list && points.size() >= 2)
        g_target.list->AddPolyline(points.data(), (int)points.size(), tint, lua_toboolean(lua, 4) ? ImDrawFlags_Closed : 0, number(lua, 3, 1.f));
    return 0;
}

static int api_draw_glow(lua_State* lua) {
    if (!g_target.list) return 0;
    ImVec4 tint = read_color(lua, 4);
    tint.w *= g_target.alpha;
    EchoUI::soft_disc(g_target.list, point(lua, 1), (float)luaL_checknumber(lua, 3), tint, number(lua, 5, 1.f));
    return 0;
}

static int api_draw_beam(lua_State* lua) {
    if (!g_target.list) return 0;
    ImVec4 tint = read_color(lua, 7);
    tint.w *= g_target.alpha;
    EchoUI::beam(g_target.list, point(lua, 1), point(lua, 3), point(lua, 5), tint);
    return 0;
}

static int api_draw_text(lua_State* lua) {
    size_t length = 0;
    const char* text = luaL_checklstring(lua, 3, &length);
    if (length > MAX_TEXT_BYTES)
        luaL_error(lua, "draw.text takes at most %d bytes", MAX_TEXT_BYTES);
    Script::Runtime::from(lua)->charge(lua, (long long)length);
    float size = text_size(lua, 5);
    if (!g_target.list) return 0;
    g_target.list->AddText(ImGui::GetFont(), size, point(lua, 1), draw_color(lua, 4), text, text + length, number(lua, 6, 0.f));
    return 0;
}

static int api_draw_text_size(lua_State* lua) {
    size_t length = 0;
    const char* text = luaL_checklstring(lua, 1, &length);
    if (length > MAX_TEXT_BYTES)
        luaL_error(lua, "draw.text_size takes at most %d bytes", MAX_TEXT_BYTES);
    Script::Runtime::from(lua)->charge(lua, (long long)length);
    ImVec2 size = ImGui::GetFont()->CalcTextSizeA(text_size(lua, 2), FLT_MAX, number(lua, 3, 0.f), text, text + length);
    lua_pushnumber(lua, size.x);
    lua_pushnumber(lua, size.y);
    return 2;
}

static void turned_corners(ImVec2 min, ImVec2 max, float turn, ImVec2* corners) {
    ImVec2 middle((min.x + max.x) * .5f, (min.y + max.y) * .5f);
    float cosine = std::cos(turn), sine = std::sin(turn);
    const ImVec2 spots[] = { min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y) };
    for (int i = 0; i < 4; ++i) {
        float dx = spots[i].x - middle.x, dy = spots[i].y - middle.y;
        corners[i] = ImVec2(middle.x + dx * cosine - dy * sine, middle.y + dx * sine + dy * cosine);
    }
}

static void draw_texture(lua_State* lua, ImTextureID texture, int first) {
    ImVec2 min = point(lua, first), max = point(lua, first + 2);
    ImU32 tint = draw_color_or_white(lua, first + 4);
    float turn = number(lua, first + 5, 0.f);
    if (!g_target.list || !texture) return;
    if (turn == 0.f) {
        g_target.list->AddImage(ImTextureRef(texture), min, max, ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), tint);
        return;
    }
    ImVec2 corners[4];
    turned_corners(min, max, turn, corners);
    g_target.list->AddImageQuad(ImTextureRef(texture), corners[0], corners[1], corners[2], corners[3], ImVec2(0.f, 0.f), ImVec2(1.f, 0.f), ImVec2(1.f, 1.f), ImVec2(0.f, 1.f), tint);
}

static int logo_part_named(const char* name) {
    for (int part = 0; part < LOGO_PART_COUNT; ++part) {
        if (std::strcmp(name, EchoUI::logo_part_name(part)) == 0) return part;
    }
    return -1;
}

static void read_logo_paint(lua_State* lua, int index, EchoUI::LogoPaint& paint) {
    index = lua_absindex(lua, index);
    if (lua_isnoneornil(lua, index)) return;
    if (lua_isboolean(lua, index)) {
        paint.shown = lua_toboolean(lua, index);
        return;
    }
    paint.shown = true;
    bool gradient = false;
    if (lua_istable(lua, index)) {
        lua_getfield(lua, index, "from");
        gradient = !lua_isnil(lua, -1);
        lua_pop(lua, 1);
    }
    if (!gradient) {
        ImU32 flat = ImGui::ColorConvertFloat4ToU32(read_color(lua, index));
        for (auto& corner : paint.corners) corner = flat;
        return;
    }
    lua_getfield(lua, index, "from");
    ImVec4 from = read_color(lua, -1);
    lua_getfield(lua, index, "to");
    ImVec4 to = lua_isnil(lua, -1) ? from : read_color(lua, -1);
    lua_getfield(lua, index, "angle");
    float angle = (float)luaL_optnumber(lua, -1, IM_PI * .5f);
    lua_pop(lua, 3);
    float across = std::cos(angle), down = std::sin(angle);
    float reach = std::abs(across) + std::abs(down);
    const ImVec2 spots[] = { ImVec2(-.5f, -.5f), ImVec2(.5f, -.5f), ImVec2(.5f, .5f), ImVec2(-.5f, .5f) };
    for (int i = 0; i < 4; ++i) {
        float t = .5f + (spots[i].x * across + spots[i].y * down) / reach;
        paint.corners[i] = ImGui::ColorConvertFloat4ToU32(EchoUI::mix(from, to, t));
    }
}

static bool is_logo_parts(lua_State* lua, int index) {
    if (!lua_istable(lua, index)) return false;
    lua_rawgeti(lua, index, 1);
    lua_getfield(lua, index, "r");
    bool color = !lua_isnil(lua, -2) || !lua_isnil(lua, -1);
    lua_pop(lua, 2);
    return !color;
}

static void read_logo_paints(lua_State* lua, int index, EchoUI::LogoPaint* paints) {
    index = lua_absindex(lua, index);
    for (int part = 0; part < LOGO_PART_COUNT; ++part) paints[part] = EchoUI::logo_part_default(part);
    lua_pushnil(lua);
    while (lua_next(lua, index)) {
        if (lua_type(lua, -2) != LUA_TSTRING) luaL_error(lua, "logo parts are named, like border = \"#ff0000\"");
        const char* name = lua_tostring(lua, -2);
        if (std::strcmp(name, LOGO_RINGS) != 0 && logo_part_named(name) < 0) luaL_error(lua, "the logo has no part named %s", name);
        lua_pop(lua, 1);
    }
    lua_getfield(lua, index, LOGO_RINGS);
    for (const char* ring : { "ring1", "ring2", "ring3" }) read_logo_paint(lua, -1, paints[logo_part_named(ring)]);
    lua_pop(lua, 1);
    for (int part = 0; part < LOGO_PART_COUNT; ++part) {
        lua_getfield(lua, index, EchoUI::logo_part_name(part));
        read_logo_paint(lua, -1, paints[part]);
        lua_pop(lua, 1);
    }
}

static void draw_logo_part(int part, ImVec2 min, ImVec2 max, float turn, const EchoUI::LogoPaint& paint) {
    CCTexture2D* texture = EchoUI::logo_part_texture(part);
    if (!g_target.list || !texture || !paint.shown) return;
    ImVec2 corners[4];
    turned_corners(min, max, turn, corners);
    ImDrawList* list = g_target.list;
    list->PushTexture(texture_ref(texture));
    list->PrimReserve(6, 4);
    list->PrimQuadUV(corners[0], corners[1], corners[2], corners[3], ImVec2(0.f, 0.f), ImVec2(1.f, 0.f), ImVec2(1.f, 1.f), ImVec2(0.f, 1.f), IM_COL32_WHITE);
    ImDrawVert* written = list->_VtxWritePtr - 4;
    for (int i = 0; i < 4; ++i) {
        ImVec4 tint = ImGui::ColorConvertU32ToFloat4(paint.corners[i]);
        tint.w *= g_target.alpha;
        written[i].col = ImGui::ColorConvertFloat4ToU32(tint);
    }
    list->PopTexture();
}

static int api_draw_image(lua_State* lua) {
    CCTexture2D* texture = image_named(luaL_checkstring(lua, 1));
    draw_texture(lua, texture ? (ImTextureID)texture->getName() : ImTextureID{}, 2);
    return 0;
}

static int api_draw_logo(lua_State* lua) {
    if (is_logo_parts(lua, 5)) {
        EchoUI::LogoPaint paints[LOGO_PART_COUNT];
        read_logo_paints(lua, 5, paints);
        ImVec2 min = point(lua, 1), max = point(lua, 3);
        float turn = number(lua, 6, 0.f);
        for (int part = 0; part < LOGO_PART_COUNT; ++part) draw_logo_part(part, min, max, turn, paints[part]);
        return 0;
    }
    static const std::string white_logo = fmt::format("{}/echo-logo-white.png", Mod::get()->getID());
    draw_texture(lua, EchoUI::logo_image(white_logo.c_str()), 1);
    return 0;
}

static int api_draw_logo_part(lua_State* lua) {
    int part = logo_part_named(luaL_checkstring(lua, 1));
    if (part < 0) luaL_argerror(lua, 1, "the logo has no part with that name");
    ImVec2 min = point(lua, 2), max = point(lua, 4);
    EchoUI::LogoPaint paint = EchoUI::logo_part_default(part);
    read_logo_paint(lua, 6, paint);
    draw_logo_part(part, min, max, number(lua, 7, 0.f), paint);
    return 0;
}

static int api_draw_image_part(lua_State* lua) {
    CCTexture2D* texture = image_named(luaL_checkstring(lua, 1));
    ImVec2 min = point(lua, 2), max = point(lua, 4), uv_min = point(lua, 6), uv_max = point(lua, 8);
    ImU32 tint = draw_color_or_white(lua, 10);
    if (g_target.list && texture)
        g_target.list->AddImage(texture_ref(texture), min, max, uv_min, uv_max, tint);
    return 0;
}

static int api_draw_image_size(lua_State* lua) {
    CCTexture2D* texture = image_named(luaL_checkstring(lua, 1));
    if (!texture) return 0;
    CCSize size = texture->getContentSizeInPixels();
    lua_pushnumber(lua, size.width);
    lua_pushnumber(lua, size.height);
    return 2;
}

static int api_draw_clip(lua_State* lua) {
    if (!g_target.list) return 0;
    if (g_target.clips >= MAX_CLIP_DEPTH)
        luaL_error(lua, "draw.clip can nest at most %d deep", MAX_CLIP_DEPTH);
    g_target.list->PushClipRect(point(lua, 1), point(lua, 3), true);
    ++g_target.clips;
    return 0;
}

static int api_draw_unclip(lua_State* lua) {
    if (!g_target.list || g_target.clips <= 0) return 0;
    g_target.list->PopClipRect();
    --g_target.clips;
    return 0;
}

static int api_draw_mark(lua_State* lua) {
    lua_pushinteger(lua, g_target.list ? g_target.list->VtxBuffer.Size : 0);
    return 1;
}

static int api_draw_shade(lua_State* lua) {
    int from = (int)luaL_checkinteger(lua, 1);
    ImVec2 start = point(lua, 2), end = point(lua, 4);
    ImVec4 first = read_color(lua, 6), second = read_color(lua, 7);
    if (!g_target.list) return 0;
    Script::Runtime::from(lua)->charge(lua, std::max(0, g_target.list->VtxBuffer.Size - std::max(from, 0)));
    shade_between(g_target.list, from, start, end, first, second);
    return 0;
}

static int api_draw_recolor(lua_State* lua) {
    int from = (int)luaL_checkinteger(lua, 1);
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    ImDrawList* list = g_target.list;
    if (!list) return 0;
    int end = list->VtxBuffer.Size;
    for (int v = std::max(from, 0); v < end; ++v) {
        ImDrawVert vert = list->VtxBuffer[v];
        ImVec4 now = ImGui::ColorConvertU32ToFloat4(vert.col);
        float kept = now.w;
        now.w = 1.f;
        lua_pushvalue(lua, 2);
        lua_pushnumber(lua, vert.pos.x);
        lua_pushnumber(lua, vert.pos.y);
        lua_pushinteger(lua, packed(now));
        lua_call(lua, 3, 1);
        ImVec4 tint = read_color(lua, -1);
        lua_pop(lua, 1);
        tint.w *= kept;
        list->VtxBuffer[v].col = ImGui::ColorConvertFloat4ToU32(tint);
    }
    return 0;
}

static int register_api(lua_State* lua) {
    const luaL_Reg helpers[] = {
        { "rgb", api_rgb },
        { "hsv", api_hsv },
        { "blend", api_blend },
        { "fade", api_fade },
        { "channels", api_channels },
        { "lerp", api_lerp },
        { "clamp", api_clamp },
        { "smoothstep", api_smoothstep },
        { "noise", api_noise },
        { "stable_random", api_stable_random },
        { "ease", api_ease },
        { nullptr, nullptr },
    };
    lua_pushglobaltable(lua);
    luaL_setfuncs(lua, helpers, 0);
    lua_pop(lua, 1);
    const luaL_Reg drawing[] = {
        { "rect", api_draw_rect },
        { "fill", api_draw_fill },
        { "gradient", api_draw_gradient },
        { "line", api_draw_line },
        { "circle", api_draw_circle },
        { "disc", api_draw_disc },
        { "ellipse", api_draw_ellipse },
        { "arc", api_draw_arc },
        { "triangle", api_draw_triangle },
        { "quad", api_draw_quad },
        { "polygon", api_draw_polygon },
        { "polyline", api_draw_polyline },
        { "glow", api_draw_glow },
        { "beam", api_draw_beam },
        { "text", api_draw_text },
        { "text_size", api_draw_text_size },
        { "image", api_draw_image },
        { "image_part", api_draw_image_part },
        { "image_size", api_draw_image_size },
        { "logo", api_draw_logo },
        { "logo_part", api_draw_logo_part },
        { "clip", api_draw_clip },
        { "unclip", api_draw_unclip },
        { "mark", api_draw_mark },
        { "shade", api_draw_shade },
        { "recolor", api_draw_recolor },
        { nullptr, nullptr },
    };
    luaL_newlib(lua, drawing);
    lua_setglobal(lua, "draw");
    return 0;
}

static void set_color(lua_State* lua, const char* key, ImVec4 value) {
    lua_pushinteger(lua, packed(value));
    lua_setfield(lua, -2, key);
}

static int publish_frame(lua_State* lua) {
    ImGuiIO& io = ImGui::GetIO();
    lua_pushnumber(lua, ImGui::GetTime());
    lua_setglobal(lua, "time");
    lua_pushnumber(lua, io.DeltaTime);
    lua_setglobal(lua, "dt");
    lua_pushnumber(lua, font_unit());
    lua_setglobal(lua, "unit");
    lua_createtable(lua, 0, 2);
    set_number(lua, "w", io.DisplaySize.x);
    set_number(lua, "h", io.DisplaySize.y);
    lua_setglobal(lua, "screen");
    lua_createtable(lua, 0, 6);
    set_number(lua, "x", io.MousePos.x);
    set_number(lua, "y", io.MousePos.y);
    set_bool(lua, "down", io.MouseDown[0]);
    set_bool(lua, "clicked", io.MouseClicked[0]);
    set_bool(lua, "released", io.MouseReleased[0]);
    set_number(lua, "wheel", io.MouseWheel);
    lua_setglobal(lua, "mouse");
    const auto& colors = ImGui::GetStyle().Colors;
    lua_createtable(lua, 0, 18);
    set_color(lua, "text", colors[ImGuiCol_Text]);
    set_color(lua, "muted", colors[ImGuiCol_TextDisabled]);
    set_color(lua, "window", g_window_color);
    set_color(lua, "panel", colors[ImGuiCol_PopupBg]);
    set_color(lua, "card", colors[ImGuiCol_ChildBg]);
    set_color(lua, "frame", colors[ImGuiCol_FrameBg]);
    set_color(lua, "frame_hover", colors[ImGuiCol_FrameBgHovered]);
    set_color(lua, "frame_active", colors[ImGuiCol_FrameBgActive]);
    set_color(lua, "accent", colors[ImGuiCol_CheckMark]);
    set_color(lua, "accent_hot", colors[ImGuiCol_SliderGrabActive]);
    set_color(lua, "button", colors[ImGuiCol_Button]);
    set_color(lua, "button_hover", colors[ImGuiCol_ButtonHovered]);
    set_color(lua, "button_down", colors[ImGuiCol_ButtonActive]);
    set_color(lua, "border", colors[ImGuiCol_Border]);
    set_color(lua, "header", colors[ImGuiCol_Header]);
    set_color(lua, "header_hover", colors[ImGuiCol_HeaderHovered]);
    set_color(lua, "header_active", colors[ImGuiCol_HeaderActive]);
    set_color(lua, "separator", colors[ImGuiCol_Separator]);
    lua_setglobal(lua, "colors");
    lua_pushboolean(lua, EchoUI::sharp());
    lua_setglobal(lua, "sharp");
    lua_pushboolean(lua, GUI::get().docked());
    lua_setglobal(lua, "docked");
    return 0;
}

static int call_default(lua_State* lua) {
    if (lua_tointeger(lua, lua_upvalueindex(1)) != g_look.token || !g_look.run) return 0;
    auto* run = g_look.run;
    g_look.run = nullptr;
    run(g_look.context);
    return 0;
}

static void push_widget(lua_State* lua, const Widget& widget) {
    const int fields = 20;
    lua_createtable(lua, 0, fields);
    lua_pushstring(lua, HOOK_NAMES[(int)widget.hook]);
    lua_setfield(lua, -2, "type");
    lua_pushinteger(lua, widget.id);
    lua_setfield(lua, -2, "id");
    set_number(lua, "x", widget.min.x);
    set_number(lua, "y", widget.min.y);
    set_number(lua, "w", widget.max.x - widget.min.x);
    set_number(lua, "h", widget.max.y - widget.min.y);
    set_number(lua, "cx", (widget.min.x + widget.max.x) * .5f);
    set_number(lua, "cy", (widget.min.y + widget.max.y) * .5f);
    if (widget.label) {
        const char* end = ImGui::FindRenderedTextEnd(widget.label);
        lua_pushlstring(lua, widget.label, (size_t)(end - widget.label));
        lua_setfield(lua, -2, "label");
    }
    set_number(lua, "hover", widget.hover);
    set_number(lua, "down", widget.down);
    set_number(lua, "on", widget.on);
    if (widget.value >= 0.f) set_number(lua, "value", widget.value);
    set_number(lua, "rounding", widget.rounding);
    set_bool(lua, "pressed", widget.pressed);
    set_bool(lua, "clicked", widget.clicked);
    set_bool(lua, "popup", widget.popup);
    set_bool(lua, "rainbow", widget.rainbow);
    if (widget.tint.w > 0.f) set_color(lua, "tint", widget.tint);
    lua_pushinteger(lua, g_look.token);
    lua_pushcclosure(lua, call_default, 1);
    lua_setfield(lua, -2, "default");
}

static int call_hook(lua_State* lua) {
    const auto& widget = *static_cast<const Widget*>(lua_touserdata(lua, 1));
    lua_rawgeti(lua, LUA_REGISTRYINDEX, (int)lua_tointeger(lua, 2));
    push_widget(lua, widget);
    lua_call(lua, 1, 0);
    return 0;
}

static int call_background(lua_State* lua) {
    const auto& area = *static_cast<const ImRect*>(lua_touserdata(lua, 1));
    lua_rawgeti(lua, LUA_REGISTRYINDEX, (int)lua_tointeger(lua, 2));
    lua_createtable(lua, 0, 6);
    set_number(lua, "x", area.Min.x);
    set_number(lua, "y", area.Min.y);
    set_number(lua, "w", area.GetWidth());
    set_number(lua, "h", area.GetHeight());
    set_number(lua, "cx", area.GetCenter().x);
    set_number(lua, "cy", area.GetCenter().y);
    lua_call(lua, 1, 0);
    return 0;
}

static int call_function(lua_State* lua) {
    lua_rawgeti(lua, LUA_REGISTRYINDEX, (int)lua_tointeger(lua, 2));
    lua_pushnumber(lua, ImGui::GetIO().DeltaTime);
    lua_call(lua, 1, 0);
    return 0;
}

static int publish_floating_clock(lua_State* lua) {
    lua_pushnumber(lua, g_floating_time);
    lua_setglobal(lua, "time");
    lua_pushnumber(lua, *static_cast<const float*>(lua_touserdata(lua, 1)));
    lua_setglobal(lua, "dt");
    return 0;
}

static void read_flag(lua_State* lua, int table, const char* key, bool& value) {
    lua_getfield(lua, table, key);
    if (!lua_isnil(lua, -1)) value = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
}

static void read_number(lua_State* lua, int table, const char* key, float& value) {
    lua_getfield(lua, table, key);
    if (lua_isnumber(lua, -1)) value = (float)lua_tonumber(lua, -1);
    lua_pop(lua, 1);
}

static void read_string(lua_State* lua, int table, const char* key, std::string& value) {
    lua_getfield(lua, table, key);
    if (lua_type(lua, -1) == LUA_TSTRING) value = lua_tostring(lua, -1);
    lua_pop(lua, 1);
}

static void read_palette(lua_State* lua, int table, EchoUI::Palette& palette) {
    read_number(lua, table, "body", palette.hue);
    read_number(lua, table, "accent", palette.accent_hue);
    read_number(lua, table, "richness", palette.richness);
    read_number(lua, table, "brightness", palette.depth);
    read_number(lua, table, "accent_strength", palette.accent_saturation);
    read_number(lua, table, "accent_value", palette.accent_value);
    read_flag(lua, table, "light", palette.light);
    read_flag(lua, table, "rainbow", palette.rainbow);
    std::string scene;
    read_string(lua, table, "scene", scene);
    palette.scene = EchoUI::scene_named(GUI::get().lower_copy(scene));
}

static int style_slot(const char* name) {
    std::string wanted = GUI::get().lower_copy(name);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        if (GUI::get().lower_copy(ImGui::GetStyleColorName(i)) == wanted) return i;
    }
    return -1;
}

static void read_style_colors(lua_State* lua, int table, std::vector<std::pair<int, ImVec4>>& colors) {
    lua_pushnil(lua);
    while (lua_next(lua, table)) {
        if (lua_type(lua, -2) != LUA_TSTRING)
            luaL_error(lua, "theme.style_colors keys are color names like WindowBg");
        int slot = style_slot(lua_tostring(lua, -2));
        if (slot < 0)
            luaL_error(lua, "theme.style_colors has no color named %s", lua_tostring(lua, -2));
        colors.emplace_back(slot, read_color(lua, -1));
        lua_pop(lua, 1);
    }
}

static void read_card_list(lua_State* lua, int table, ThemeScript::PageLayout& page) {
    lua_pushnil(lua);
    while (lua_next(lua, table)) {
        if (lua_type(lua, -1) == LUA_TSTRING) {
            page.cards.emplace_back(lua_tostring(lua, -1));
            page.widths.push_back(0.f);
        }
        else if (lua_istable(lua, -1)) {
            int card = lua_gettop(lua);
            lua_rawgeti(lua, card, 1);
            std::string name = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : "";
            lua_pop(lua, 1);
            if (name.empty()) read_string(lua, card, "card", name);
            if (name.empty()) luaL_error(lua, "each card in a layout page is a name like \"Replay/Session\" or a table with one");
            float width = 0.f;
            read_number(lua, card, "width", width);
            page.cards.push_back(name);
            page.widths.push_back(width);
        }
        else {
            luaL_error(lua, "each card in a layout page is a name like \"Replay/Session\"");
        }
        lua_pop(lua, 1);
    }
}

static void read_pages(lua_State* lua, int table, ThemeScript::Layout& layout) {
    lua_pushnil(lua);
    while (lua_next(lua, table)) {
        ThemeScript::PageLayout page;
        if (lua_type(lua, -1) == LUA_TSTRING) {
            page.page = page.name = lua_tostring(lua, -1);
        }
        else if (lua_istable(lua, -1)) {
            int entry = lua_gettop(lua);
            read_string(lua, entry, "page", page.page);
            read_string(lua, entry, "name", page.name);
            read_string(lua, entry, "icon", page.icon);
            lua_getfield(lua, entry, "cards");
            if (lua_istable(lua, -1)) read_card_list(lua, lua_gettop(lua), page);
            lua_pop(lua, 1);
            if (page.name.empty()) page.name = page.page;
            if (page.name.empty()) luaL_error(lua, "a layout page needs a page or a name");
        }
        else {
            luaL_error(lua, "theme.layout.pages holds page names or tables");
        }
        layout.pages.push_back(std::move(page));
        lua_pop(lua, 1);
    }
}

static void read_card_rules(lua_State* lua, int table, ThemeScript::Layout& layout) {
    lua_pushnil(lua);
    while (lua_next(lua, table)) {
        if (lua_type(lua, -2) != LUA_TSTRING) luaL_error(lua, "theme.layout.cards keys are card names like \"Replay/Session\"");
        ThemeScript::CardLayout rule;
        if (lua_isboolean(lua, -1)) rule.hidden = !lua_toboolean(lua, -1);
        else if (lua_istable(lua, -1)) {
            int entry = lua_gettop(lua);
            read_flag(lua, entry, "hidden", rule.hidden);
            read_number(lua, entry, "width", rule.width);
        }
        layout.cards[lua_tostring(lua, -2)] = rule;
        lua_pop(lua, 1);
    }
}

static void read_layout(lua_State* lua, int table, ThemeScript::Layout& layout) {
    lua_getfield(lua, table, "pages");
    if (lua_istable(lua, -1)) read_pages(lua, lua_gettop(lua), layout);
    lua_pop(lua, 1);
    read_string(lua, table, "nav", layout.nav);
    layout.nav = GUI::get().lower_copy(layout.nav);
    if (layout.nav != "left" && layout.nav != "right" && layout.nav != "top" && layout.nav != "bottom" && layout.nav != "none")
        luaL_error(lua, "theme.layout.nav is \"left\", \"right\", \"top\", \"bottom\" or \"none\"");
    read_flag(lua, table, "nav_labels", layout.nav_labels);
    lua_getfield(lua, table, "docked");
    if (lua_isboolean(lua, -1)) layout.docked = lua_toboolean(lua, -1) ? 1 : 0;
    lua_pop(lua, 1);
    lua_getfield(lua, table, "header");
    if (lua_istable(lua, -1)) {
        int header = lua_gettop(lua);
        read_flag(lua, header, "shown", layout.header_shown);
        read_string(lua, header, "title", layout.header.title);
        read_flag(lua, header, "logo", layout.header.logo);
        read_flag(lua, header, "version", layout.header.version);
        read_flag(lua, header, "close", layout.header.close);
        read_number(lua, header, "height", layout.header.height);
    }
    else if (lua_isboolean(lua, -1)) layout.header_shown = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, table, "bar");
    if (lua_istable(lua, -1)) {
        int bar = lua_gettop(lua);
        read_flag(lua, bar, "shown", layout.bar.shown);
        read_flag(lua, bar, "logo", layout.bar.logo);
        read_flag(lua, bar, "title", layout.bar.title);
        read_flag(lua, bar, "version", layout.bar.version);
        read_flag(lua, bar, "nav", layout.bar.nav);
        read_flag(lua, bar, "status", layout.bar.status);
        read_flag(lua, bar, "reset", layout.bar.reset);
        read_flag(lua, bar, "close", layout.bar.close);
        read_flag(lua, bar, "bottom", layout.bar.bottom);
        read_string(lua, bar, "title_text", layout.bar.title_text);
    }
    else if (lua_isboolean(lua, -1)) layout.bar.shown = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    lua_getfield(lua, table, "status");
    if (lua_istable(lua, -1)) {
        int status = lua_gettop(lua);
        read_flag(lua, status, "shown", layout.status.shown);
        read_flag(lua, status, "state", layout.status.state);
        read_flag(lua, status, "frame", layout.status.frame);
        read_flag(lua, status, "inputs", layout.status.inputs);
        read_flag(lua, status, "tps", layout.status.tps);
        read_flag(lua, status, "speed", layout.status.speed);
    }
    else if (lua_isboolean(lua, -1)) layout.status.shown = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    read_number(lua, table, "tile_width", layout.tile_width);
    read_number(lua, table, "card_width", layout.card_width);
    float columns = 0.f;
    read_number(lua, table, "columns", columns);
    layout.columns = (int)columns;
    lua_getfield(lua, table, "cards");
    if (lua_istable(lua, -1)) read_card_rules(lua, lua_gettop(lua), layout);
    lua_pop(lua, 1);
}

static void read_metrics(lua_State* lua, int table, std::vector<std::pair<int, ImVec2>>& metrics) {
    lua_pushnil(lua);
    while (lua_next(lua, table)) {
        if (lua_type(lua, -2) != LUA_TSTRING) luaL_error(lua, "theme.metrics keys are names like frame_rounding");
        std::string wanted = GUI::get().lower_copy(lua_tostring(lua, -2));
        int slot = -1;
        for (int i = 0; i < (int)std::size(METRICS); ++i) {
            if (wanted == METRICS[i].name) slot = i;
        }
        if (slot < 0) luaL_error(lua, "theme.metrics has no size named %s", wanted.c_str());
        ImVec2 value;
        if (lua_isnumber(lua, -1)) value = ImVec2((float)lua_tonumber(lua, -1), (float)lua_tonumber(lua, -1));
        else if (lua_istable(lua, -1)) {
            lua_rawgeti(lua, -1, 1);
            lua_rawgeti(lua, -2, 2);
            if (!lua_isnumber(lua, -2) || !lua_isnumber(lua, -1)) luaL_error(lua, "theme.metrics.%s is a pair of numbers like {8, 4}", wanted.c_str());
            value = ImVec2((float)lua_tonumber(lua, -2), (float)lua_tonumber(lua, -1));
            lua_pop(lua, 2);
        }
        else luaL_error(lua, "theme.metrics.%s is a number or a pair like {8, 4}", wanted.c_str());
        metrics.emplace_back(slot, value);
        lua_pop(lua, 1);
    }
}

static int read_theme(lua_State* lua) {
    auto& info = *static_cast<ThemeInfo*>(lua_touserdata(lua, 1));
    for (int i = 0; i < (int)ThemeScript::Hook::Count; ++i)
        info.functions[i] = take_function(lua, HOOK_NAMES[i]);
    info.update = take_function(lua, "update");
    info.overlay = take_function(lua, "overlay");
    info.background = take_function(lua, "background");
    lua_getglobal(lua, "theme");
    if (lua_istable(lua, -1)) {
        int theme = lua_gettop(lua);
        read_string(lua, theme, "name", info.name);
        read_string(lua, theme, "logo", info.logo);
        lua_getfield(lua, theme, "logo");
        if (lua_istable(lua, -1)) {
            info.paints_logo = true;
            read_logo_paints(lua, lua_gettop(lua), info.logo_paints);
        }
        lua_pop(lua, 1);
        lua_getfield(lua, theme, "palette");
        if (lua_istable(lua, -1)) {
            info.has_palette = true;
            read_palette(lua, lua_gettop(lua), info.palette);
        }
        lua_pop(lua, 1);
        lua_getfield(lua, theme, "style_colors");
        if (lua_istable(lua, -1)) read_style_colors(lua, lua_gettop(lua), info.colors);
        lua_pop(lua, 1);
        lua_getfield(lua, theme, "layout");
        if (lua_istable(lua, -1)) read_layout(lua, lua_gettop(lua), info.layout);
        lua_pop(lua, 1);
        lua_getfield(lua, theme, "metrics");
        if (lua_istable(lua, -1)) read_metrics(lua, lua_gettop(lua), info.metrics);
        lua_pop(lua, 1);
        read_flag(lua, theme, "sharp", info.sharp);
    }
    lua_pop(lua, 1);
    return 0;
}

static bool run_protected(Script::Runtime& runtime, lua_CFunction function, void* data, lua_Integer value = 0) {
    lua_State* lua = runtime.state();
    if (!lua) {
        runtime.last_error = "Lua could not start";
        return false;
    }
    lua_pushcfunction(lua, function);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, value);
    return runtime.call(2, 0);
}

static void add_output(const std::string& text) {
    Script::add_output(g_output, text);
}

static void report(const std::string& message) {
    log::warn("[theme script] {}", message);
    g_error = message.substr(0, message.find('\n'));
    add_output(message);
}

static void drop_runtime() {
    std::fill(std::begin(ThemeScript::g_hooks), std::end(ThemeScript::g_hooks), false);
    g_theme = {};
    ++g_layout_version;
    g_runtime.reset();
    forget_images();
    g_ease_keys.clear();
    g_target = {};
    g_look = {};
}

static void fail(const std::string& message) {
    report(message);
    std::fill(std::begin(ThemeScript::g_hooks), std::end(ThemeScript::g_hooks), false);
    g_stopping = true;
}

static void finish_stop() {
    g_stopping = false;
    drop_runtime();
    if (GUI::get().has_palette) EchoUI::apply_palette(GUI::get().palette);
}

static void copy_guide(const fs::path& folder) {
    std::error_code error;
    fs::copy_file(Mod::get()->getResourcesDir() / GUIDE_FILE, folder / "Scripting Guide.md", fs::copy_options::overwrite_existing, error);
}

static bool copy_theme(const fs::path& from, const fs::path& folder, fs::path& made) {
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) return false;
    made = folder / "theme.lua";
    if (!fs::exists(made, error)) fs::copy_file(from, made, error);
    if (error) return false;
    copy_guide(folder);
    return true;
}

static std::string theme_name_of(const fs::path& file) {
    if (file.filename() == "theme.lua")
        return geode::utils::string::pathToString(file.parent_path().filename());
    return geode::utils::string::pathToString(file.stem());
}

static std::string chunk_name(const fs::path& file) {
    return Script::chunk_name(file, ThemeScript::themes_folder());
}

static fs::path freeze_marker() {
    return ThemeScript::themes_folder() / FREEZE_MARKER_FILE;
}

static void watch(const fs::path& file) {
    std::error_code error;
    g_file = file;
    g_written = fs::last_write_time(file, error);
    g_checked_at = ImGui::GetTime();
}

static void apply_theme_palette(PaletteUse use, const EchoUI::Palette& palette, const std::string& name) {
    if (use == PaletteUse::Save) GUI::get().use_palette(palette, name);
    else if (use == PaletteUse::Preview) GUI::get().preview_palette(palette, name);
    else if (GUI::get().has_palette) EchoUI::apply_palette(GUI::get().palette);
    else ThemeScript::recolor();
}

static bool refuse(const std::string& message, const fs::path& file, PaletteUse use) {
    report(message);
    g_stopping = false;
    drop_runtime();
    watch(file);
    apply_theme_palette(use, GUI::get().palette, theme_name_of(file));
    return false;
}

static bool compile(const std::string& text, const fs::path& file, std::unique_ptr<Script::Runtime>& runtime, ThemeInfo& info) {
    runtime = std::make_unique<Script::Runtime>(THEME_MEMORY_LIMIT);
    runtime->on_print = add_output;
    runtime->guard_freezes(freeze_marker(), geode::utils::string::pathToString(file));
    runtime->limit_instructions(THEME_LOAD_INSTRUCTIONS);
    return run_protected(*runtime, register_api, nullptr) && run_protected(*runtime, publish_frame, nullptr) && runtime->run_text(text, chunk_name(file)) && run_protected(*runtime, read_theme, &info);
}

static bool start(const std::string& text, const fs::path& file, PaletteUse use, bool keep_on_error) {
    fs::path running_file = g_file;
    g_file = file;
    std::unique_ptr<Script::Runtime> runtime;
    ThemeInfo info;
    if (!compile(text, file, runtime, info)) {
        std::string message = runtime->error();
        runtime.reset();
        if (!keep_on_error || !ThemeScript::active()) return refuse(message, file, use);
        report(message);
        g_file = running_file;
        if (file == running_file) watch(file);
        return false;
    }
    std::string name = info.name.empty() ? theme_name_of(file) : info.name;
    g_stopping = false;
    drop_runtime();
    g_runtime = std::move(runtime);
    watch(file);
    g_error.clear();
    g_theme = info;
    ++g_layout_version;
    for (int i = 0; i < (int)ThemeScript::Hook::Count; ++i)
        ThemeScript::g_hooks[i] = g_theme.functions[i] != LUA_NOREF;
    if (!info.logo.empty()) g_logo = image_named(info.logo);
    else if (info.paints_logo) g_logo = g_painted_logo = EchoUI::logo_from_parts(info.logo_paints);
    apply_theme_palette(use, info.has_palette ? info.palette : GUI::get().palette, name);
    return true;
}

static void run_pending() {
    PendingRun run = std::move(g_pending);
    g_pending = {};
    if (run.saved && !read_text(run.file, run.text)) {
        report("Could not open " + geode::utils::string::pathToString(run.file.filename()));
        return;
    }
    start(run.text, run.file, run.saved ? PaletteUse::Save : PaletteUse::Preview, true);
}

namespace ThemeScript {
    bool run_hook(const Widget& widget, ImDrawList* list, float alpha, void (*look)(void*), void* context) {
        if (!g_runtime || g_stopping) return false;
        DrawTarget outer_target = g_target;
        LookCall outer_look = g_look;
        g_target = { list, alpha, 0 };
        g_look = { ++g_look_count, look, context };
        bool fine = run_protected(*g_runtime, call_hook, (void*)&widget, g_theme.functions[(int)widget.hook]);
        if (!fine) fail(g_runtime->error());
        for (; g_target.clips > 0; --g_target.clips)
            list->PopClipRect();
        g_target = outer_target;
        g_look = outer_look;
        return fine;
    }

    bool has(Hook hook) {
        return g_hooks[(int)hook];
    }

    bool load(const fs::path& path, bool use_palette) {
        g_stopping = false;
        drop_runtime();
        g_file = path;
        g_error.clear();
        PaletteUse use = use_palette ? PaletteUse::Save : PaletteUse::Current;
        if (path.empty()) return refuse("The theme script's path is not inside the themes folder", path, use);
        std::string froze_file;
        bool froze = Script::froze_last_time(freeze_marker(), froze_file) && froze_file == geode::utils::string::pathToString(path);
        if (froze && !use_palette)
            return refuse("This script froze the game last time so it wasn't started. Press Run on the Scripting page to try it again", path, use);
        std::string text;
        if (!read_text(path, text)) return refuse("Could not open " + geode::utils::string::pathToString(path.filename()), path, use);
        return start(text, path, use, false);
    }

    void unload() {
        g_stopping = false;
        drop_runtime();
        g_file.clear();
        g_error.clear();
    }

    bool reload() {
        if (g_file.empty()) return false;
        fs::path again = g_file;
        watch(again);
        std::string text;
        if (!read_text(again, text)) {
            report("Could not open " + geode::utils::string::pathToString(again.filename()));
            return false;
        }
        return start(text, again, PaletteUse::Save, true);
    }

    bool active() {
        return g_runtime && !g_stopping;
    }

    const std::string& error() {
        return g_error;
    }

    fs::path file() {
        return g_file;
    }

    fs::path themes_folder() {
        return Logic::get().save_root() / "themes";
    }

    std::string saved_name() {
        if (g_file.empty()) return {};
        std::error_code error;
        fs::path relative = fs::relative(g_file, themes_folder(), error);
        if (error || relative.empty()) return {};
        auto text = relative.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    fs::path from_saved_name(const std::string& name) {
        fs::path relative;
        if (!safe_relative(name, relative)) return {};
        return themes_folder() / relative;
    }

    void begin_frame() {
        if (g_stopping) finish_stop();
        if (g_pending.waiting) run_pending();
        if (g_file.empty()) return;
        double now = ImGui::GetTime();
        if (now - g_checked_at >= RELOAD_CHECK_SECONDS) {
            g_checked_at = now;
            std::error_code error;
            auto written = fs::last_write_time(g_file, error);
            if (!error && written != g_written) reload();
        }
        if (!active()) return;
        g_runtime->limit_instructions(THEME_FRAME_INSTRUCTIONS);
        g_target = {};
        g_look = {};
        if (!run_protected(*g_runtime, publish_frame, nullptr)) {
            fail(g_runtime->error());
            return;
        }
        if (g_theme.update != LUA_NOREF && !run_protected(*g_runtime, call_function, nullptr, g_theme.update))
            fail(g_runtime->error());
    }

    void draw_overlay(float alpha) {
        if (!active() || g_theme.overlay == LUA_NOREF) return;
        ImDrawList* list = ImGui::GetForegroundDrawList();
        g_target = { list, alpha, 0 };
        g_look = {};
        if (!run_protected(*g_runtime, call_function, nullptr, g_theme.overlay)) fail(g_runtime->error());
        for (; g_target.clips > 0; --g_target.clips)
            list->PopClipRect();
        g_target = {};
    }

    bool begin_floating_logo(float dt) {
        if (!active() || !g_hooks[(int)Hook::FloatingLogo]) return false;
        g_floating_time += dt;
        g_runtime->limit_instructions(THEME_FRAME_INSTRUCTIONS);
        g_target = {};
        g_look = {};
        if (run_protected(*g_runtime, publish_frame, nullptr) && run_protected(*g_runtime, publish_floating_clock, &dt)) return true;
        fail(g_runtime->error());
        return false;
    }

    bool has_background() {
        return active() && g_theme.background != LUA_NOREF;
    }

    void draw_background(ImDrawList* list, float alpha, ImVec2 min, ImVec2 max) {
        if (!has_background()) return;
        ImRect area(min, max);
        g_target = { list, alpha, 0 };
        g_look = {};
        if (!run_protected(*g_runtime, call_background, (void*)&area, g_theme.background)) fail(g_runtime->error());
        for (; g_target.clips > 0; --g_target.clips)
            list->PopClipRect();
        g_target = {};
    }

    void recolor() {
        if (!active()) return;
        auto& colors = ImGui::GetStyle().Colors;
        for (const auto& [slot, value] : g_theme.colors)
            colors[slot] = value;
        g_window_color = colors[ImGuiCol_WindowBg];
        if (g_hooks[(int)Hook::WindowBackground] || g_theme.background != LUA_NOREF) colors[ImGuiCol_WindowBg].w = 0.f;
    }

    ImVec4 window_color() {
        return g_window_color;
    }

    const Layout& layout() {
        return g_theme.layout;
    }

    const CardLayout* card_layout(const std::string& page, const std::string& title) {
        if (g_theme.layout.cards.empty()) return nullptr;
        auto found = g_theme.layout.cards.find(page + "/" + title);
        if (found == g_theme.layout.cards.end()) found = g_theme.layout.cards.find(title);
        return found == g_theme.layout.cards.end() ? nullptr : &found->second;
    }

    int layout_version() {
        return g_layout_version;
    }

    bool forces_sharp() {
        return g_theme.sharp;
    }

    void apply_metrics(ImGuiStyle& style) {
        for (const auto& [slot, value] : g_theme.metrics) {
            if (METRICS[slot].single) style.*METRICS[slot].single = value.x;
            else style.*METRICS[slot].pair = value;
        }
    }

    CCTexture2D* logo() {
        return active() ? g_logo : nullptr;
    }

    bool make_new(fs::path& made) {
        std::error_code error;
        fs::path folder;
        for (int number = 1; number <= MAX_NEW_FOLDERS; ++number) {
            folder = themes_folder() / ReplayIO::utf8_path(number == 1 ? std::string("My Theme") : fmt::format("My Theme {}", number));
            if (!fs::exists(folder, error)) break;
        }
        fs::create_directories(folder, error);
        made = folder / "theme.lua";
        if (error || !Script::write_text(made, NEW_THEME)) return false;
        copy_guide(folder);
        return true;
    }

    std::vector<fs::path> bundled() {
        std::vector<fs::path> found;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(Mod::get()->getResourcesDir(), error)) {
            std::string name = geode::utils::string::pathToString(entry.path().filename());
            if (name.starts_with(BUNDLED_THEME_PREFIX) && name.ends_with(".lua")) found.push_back(entry.path());
        }
        return found;
    }

    std::string bundled_name(const fs::path& file) {
        return Script::bundled_name(file, BUNDLED_THEME_PREFIX);
    }

    bool is_bundled(const fs::path& file) {
        return Script::is_bundled(file);
    }

    fs::path installed_folder(const fs::path& file) {
        return themes_folder() / ReplayIO::utf8_path(bundled_name(file));
    }

    bool install(const fs::path& file, fs::path& made) {
        return copy_theme(file, installed_folder(file), made);
    }
}

class ThemeHost : public Script::Host {
public:
    const char* name() const override {
        return "Themes";
    }

    fs::path folder() const override {
        return ThemeScript::themes_folder();
    }

    std::vector<fs::path> files() const override {
        std::vector<fs::path> found;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder(), error)) {
            fs::path script = entry.path() / "theme.lua";
            if (entry.is_directory(error) && fs::is_regular_file(script, error)) found.push_back(script);
            else if (entry.is_regular_file(error) && GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().extension())) == ".lua")
                found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end(), [this](const fs::path& a, const fs::path& b) {
            return GUI::get().lower_copy(label(a)) < GUI::get().lower_copy(label(b));
        });
        return found;
    }

    std::vector<fs::path> examples() const override {
        std::vector<fs::path> found;
        std::error_code error;
        for (const auto& example : ThemeScript::bundled()) {
            if (!fs::exists(ThemeScript::installed_folder(example), error)) found.push_back(example);
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    std::string label(const fs::path& file) const override {
        if (ThemeScript::is_bundled(file)) return ThemeScript::bundled_name(file);
        return theme_name_of(file);
    }

    fs::path current() const override {
        return g_file;
    }

    const std::vector<std::string>& names() const override {
        static const std::vector<std::string> api = [] {
            std::vector<std::string> found(std::begin(HOOK_NAMES), std::end(HOOK_NAMES));
            for (const char* name : { "update", "overlay", "background", "theme", "draw", "colors", "mouse", "screen", "time", "dt", "unit", "sharp", "docked", "rgb", "hsv", "blend", "fade", "channels", "lerp", "clamp", "smoothstep", "noise", "stable_random", "ease" })
                found.push_back(name);
            return found;
        }();
        return api;
    }

    bool make_new(fs::path& made) override {
        return ThemeScript::make_new(made);
    }

    bool install_example(const fs::path& example, fs::path& made) override {
        return ThemeScript::install(example, made);
    }

    bool remove_file(const fs::path& file) override {
        if (file == g_file) stop();
        std::error_code error;
        if (file.filename() == "theme.lua") return fs::remove_all(file.parent_path(), error) > 0;
        return fs::remove(file, error);
    }

    void run_text(const std::string& text, const fs::path& file) override {
        g_pending = { true, false, text, file };
    }

    void run_file(const fs::path& file) override {
        g_pending = { true, true, {}, file };
    }

    void stop() override {
        g_pending = {};
        ThemeScript::unload();
        auto& gui = GUI::get();
        if (gui.has_palette) gui.use_palette(gui.palette, Logic::get().theme_name);
    }

    bool running() const override {
        return ThemeScript::active();
    }

    bool reruns() const override {
        return true;
    }

    const std::string& error() const override {
        return g_error;
    }

    const std::deque<std::string>& output() const override {
        return g_output;
    }

    void clear_output() override {
        g_output.clear();
    }
};

namespace ThemeScript {
    Script::Host& host() {
        static ThemeHost theme_host;
        return theme_host;
    }
}
