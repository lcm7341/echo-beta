#include "GUI/widgets.hpp"
#include "GUI/gui.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>

using namespace geode::prelude;

namespace EchoUI {
    static ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    }

    static ImVec4 color(ImGuiCol slot) {
        return ImGui::GetStyleColorVec4(slot);
    }

    static ImVec4 faded(ImVec4 value, float alpha) {
        value.w *= alpha;
        return value;
    }

    static void circle_filled_cached(ImDrawList* draw, const ImVec2& center, float radius, ImU32 col, int segments = 0) {
        if (segments <= 0 || segments > 64) {
            draw->AddCircleFilled(center, radius, col, segments);
            return;
        }
        if ((col & IM_COL32_A_MASK) == 0 || radius < 0.5f) return;
        segments = std::max(segments, 3);
        static std::array<std::vector<ImVec2>, 65> tables;
        auto& unit = tables[(std::size_t)segments];
        if (unit.empty()) {
            float a_max = (IM_PI * 2.0f) * ((float)segments - 1.0f) / (float)segments;
            for (int i = 0; i < segments; ++i) {
                float a = a_max * (float)i / (float)(segments - 1);
                unit.push_back(ImVec2(ImCos(a), ImSin(a)));
            }
        }
        draw->PathClear();
        for (const auto& v : unit)
            draw->PathLineTo(ImVec2(center.x + v.x * radius, center.y + v.y * radius));
        draw->PathFillConvex(col);
    }

    // ImHashData is a crc, which is linear: hashing 0, 1, 2.. put every "random" star & petal on a few straight lines
    const unsigned RANDOM_MIX_1 = 2146121005u, RANDOM_MIX_2 = 2221713035u;

    static unsigned random_bits(unsigned x) {
        x ^= x >> 16;
        x *= RANDOM_MIX_1;
        x ^= x >> 15;
        x *= RANDOM_MIX_2;
        x ^= x >> 16;
        return x;
    }

    static float unit_random(unsigned item, unsigned seed) {
        return (float)(random_bits(item ^ random_bits(seed)) & 16777215) / 16777215.f;
    }

    static void gloss(ImDrawList* draw, const ImRect& bb, float rounding) {
        if (!g_rainbow_rim) return;
        float alpha = ImGui::GetStyle().Alpha;
        int from = draw->VtxBuffer.Size;
        draw->AddRect(ImVec2(bb.Min.x + .5f, bb.Min.y + .5f), ImVec2(bb.Max.x - .5f, bb.Max.y - .5f), IM_COL32_WHITE, rounding, 0, 1.f);
        float height = std::max(bb.GetHeight(), 1.f);
        for (int v = from; v < draw->VtxBuffer.Size; ++v) {
            auto& vert = draw->VtxBuffer[v];
            float t = std::clamp((vert.pos.y - bb.Min.y) / height, 0.f, 1.f);
            float keep = (float)((vert.col >> IM_COL32_A_SHIFT) & 255) / 255.f;
            unsigned shown = (unsigned)(255.f * keep * (.42f - .32f * t) * alpha);
            vert.col = (vert.col & ~IM_COL32_A_MASK) | (shown << IM_COL32_A_SHIFT);
        }
    }

    static void button_skin(ImDrawList* draw, const ImRect& body, float rounding, ImGuiID id, float hot, float down, float keep = 1.f, const ImVec4* own = nullptr, float own_amount = 1.f);

    struct ColorGrade {
        float turn = 0.f;
        float saturation = 1.f;
        float brightness = 1.f;

        bool plain() const {
            return std::abs(turn) < .002f && std::abs(saturation - 1.f) < .002f && std::abs(brightness - 1.f) < .002f;
        }
    };

    static ColorGrade body_turn, accent_turn;

    // a light theme turned down doesnt get grey, it goes to its night look. night = how far the sky is (0-1),
    // night_menu = how far the menu itself is: it fades to dark w/ light text from NIGHT_TOP down to NIGHT_LOW
    const float NIGHT_TOP = .88f, NIGHT_LOW = .72f;
    static float night = 0.f;
    static float night_menu = 0.f;

    static float night_fade(float depth) {
        float t = std::clamp((NIGHT_TOP - depth) / (NIGHT_TOP - NIGHT_LOW), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    static ImVec4 turned(ImVec4 tint, const ColorGrade& grade) {
        if (grade.plain()) return tint;
        float h, sat, val;
        ImGui::ColorConvertRGBtoHSV(tint.x, tint.y, tint.z, h, sat, val);
        h += grade.turn;
        h -= std::floor(h);
        sat = std::clamp(sat * grade.saturation, 0.f, 1.f);
        if (grade.brightness < 1.f) val = std::pow(val, 1.f / std::max(grade.brightness, .1f));
        else val = std::clamp(val * grade.brightness, 0.f, 1.f);
        ImGui::ColorConvertHSVtoRGB(h, sat, val, tint.x, tint.y, tint.z);
        return tint;
    }

    static void turn_verts(ImDrawList* draw, int from, const ColorGrade& turn) {
        if (turn.plain()) return;
        for (int v = from; v < draw->VtxBuffer.Size; ++v) {
            auto& vert = draw->VtxBuffer[v];
            ImVec4 tint = turned(ImGui::ColorConvertU32ToFloat4(vert.col), turn);
            vert.col = (ImGui::ColorConvertFloat4ToU32(tint) & ~IM_COL32_A_MASK) | (vert.col & IM_COL32_A_MASK);
        }
    }

    static ImGuiID sub(ImGuiID id, const char* what) {
        return ImHashStr(what, 0, id);
    }

    float ease(ImGuiID id, float target, float speed) {
        static std::unordered_map<ImGuiID, float> values;
        auto [at, fresh] = values.try_emplace(id, target);
        if (fresh) return target;
        float dt = std::min(ImGui::GetIO().DeltaTime, 1.f / 30.f);
        float step = std::min(1.f, dt * speed * GUI::get().anim_rate());
        at->second += (target - at->second) * step;
        if (std::abs(target - at->second) < .002f) at->second = target;
        return at->second;
    }

    float glide_in(float t) {
        t = std::clamp(t, 0.f, 1.f);
        if (t >= 1.f) return 1.f;
        return 1.f - std::pow(2.f, -10.f * t);
    }

    struct EntranceAnimation {
        ImDrawList* list = nullptr;
        int start = 0;
        float t = 1.f;
    };

    static EntranceAnimation g_entrance;

    void BeginEntrance(float t) {
        g_entrance.list = ImGui::GetWindowDrawList();
        g_entrance.start = g_entrance.list->VtxBuffer.Size;
        g_entrance.t = std::clamp(t, 0.f, 1.f);
    }

    void EndEntrance() {
        ImDrawList* list = g_entrance.list;
        g_entrance.list = nullptr;
        if (!list || g_entrance.t >= 1.f) return;
        int end = list->VtxBuffer.Size;
        if (end <= g_entrance.start) return;
        ImVec2 low(FLT_MAX, FLT_MAX);
        ImVec2 high(-FLT_MAX, -FLT_MAX);
        for (int i = g_entrance.start; i < end; ++i) {
            low = ImMin(low, list->VtxBuffer[i].pos);
            high = ImMax(high, list->VtxBuffer[i].pos);
        }
        ImVec2 middle((low.x + high.x) * .5f, (low.y + high.y) * .5f);
        float glided = glide_in(g_entrance.t);
        float size = .55f + .45f * glided;
        float lift = (1.f - glided) * em(.9f);
        float alpha = std::clamp(g_entrance.t * 2.5f, 0.f, 1.f);
        for (int i = g_entrance.start; i < end; ++i) {
            ImDrawVert& vert = list->VtxBuffer[i];
            vert.pos.x = middle.x + (vert.pos.x - middle.x) * size;
            vert.pos.y = middle.y + (vert.pos.y - middle.y) * size - lift;
            unsigned shown = (unsigned)((float)((vert.col >> IM_COL32_A_SHIFT) & 255) * alpha);
            vert.col = (vert.col & ~IM_COL32_A_MASK) | (shown << IM_COL32_A_SHIFT);
        }
    }

    bool Checkbox(const char* label, bool* value) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        ImGuiID id = window->GetID(label);
        ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
        float side = ImGui::GetFrameHeight();
        ImVec2 pos = window->DC.CursorPos;
        ImRect total(pos, ImVec2(pos.x + side + (label_size.x > 0.f ? style.ItemInnerSpacing.x + label_size.x : 0.f), pos.y + label_size.y + style.FramePadding.y * 2.f));
        ImGui::ItemSize(total, style.FramePadding.y);
        if (!ImGui::ItemAdd(total, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(total, id, &hovered, &held);
        if (pressed) {
            *value = !*value;
            ImGui::MarkItemEdited(id);
        }

        float on = ease(id, *value ? 1.f : 0.f);
        float hot = ease(sub(id, "hot"), hovered ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        ImRect box(pos, ImVec2(pos.x + side, pos.y + side));
        float sink = held && hovered ? side * .04f : 0.f;
        box.Expand(-sink);
        float rounding = std::max(style.FrameRounding, side * .28f);
        ImVec4 off = mix(color(ImGuiCol_FrameBg), color(ImGuiCol_FrameBgHovered), hot);
        draw->AddRectFilled(box.Min, box.Max, ImGui::GetColorU32(mix(off, color(ImGuiCol_CheckMark), on)), rounding);
        ImVec4 edge = mix(color(ImGuiCol_Border), color(ImGuiCol_CheckMark), hot * .7f);
        draw->AddRect(box.Min, box.Max, ImGui::GetColorU32(faded(edge, 1.f - on)), rounding, 0, std::max(1.f, side * .06f));
        if (on > .01f) {
            ImVec2 mid = box.GetCenter();
            float reach = side * (.55f + .45f * on);
            auto at = [&](float x, float y) {
                return ImVec2(mid.x + (x - .5f) * reach, mid.y + (y - .5f) * reach);
            };
            ImVec4 tick = color(ImGuiCol_WindowBg);
            tick.w = 1.f;
            draw->PathLineTo(at(.27f, .52f));
            draw->PathLineTo(at(.44f, .69f));
            draw->PathLineTo(at(.74f, .33f));
            draw->PathStroke(ImGui::GetColorU32(faded(tick, on)), 0, std::max(1.5f, side * .11f));
        }
        if (label_size.x > 0.f)
            ImGui::RenderText(ImVec2(pos.x + side + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y), label);
        return pressed;
    }

    bool Button(const char* label, const ImVec2& size_arg) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        ImGuiID id = window->GetID(label);
        ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImGui::CalcItemSize(size_arg, label_size.x + style.FramePadding.x * 2.f, label_size.y + style.FramePadding.y * 2.f);
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(size, style.FramePadding.y);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

        float hot = ease(id, hovered ? 1.f : 0.f);
        float down = ease(sub(id, "down"), held && hovered ? 1.f : 0.f, 22.f);
        ImDrawList* draw = window->DrawList;
        ImRect body = bb;
        body.Expand(-down * std::max(1.f, size.y * .04f));
        ImVec4 fill = mix(mix(color(ImGuiCol_Button), color(ImGuiCol_ButtonHovered), hot), color(ImGuiCol_ButtonActive), down);
        draw->AddRectFilled(body.Min, body.Max, ImGui::GetColorU32(fill), style.FrameRounding);
        gloss(draw, body, style.FrameRounding);
        bool tinted = false;
        for (const auto& pushed : g.ColorStack)
            tinted = tinted || pushed.Col == ImGuiCol_Button;
        ImVec4 own = color(ImGuiCol_Button);
        button_skin(draw, body, style.FrameRounding, id, hot, down, 1.f, tinted ? &own : nullptr);
        if (hot > .01f)
            draw->AddRect(body.Min, body.Max, ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), hot * .55f)), style.FrameRounding, 0, 1.f);
        ImGui::RenderNavCursor(bb, id);
        ImGui::RenderTextClipped(ImVec2(bb.Min.x + style.FramePadding.x, bb.Min.y + style.FramePadding.y), ImVec2(bb.Max.x - style.FramePadding.x, bb.Max.y - style.FramePadding.y),
            label, nullptr, &label_size, style.ButtonTextAlign, &bb);
        return pressed;
    }

    static void fade_verts(ImDrawList* draw, int from, float alpha) {
        for (int v = from; v < draw->VtxBuffer.Size; ++v) {
            ImU32 col = draw->VtxBuffer[v].col;
            unsigned keep = (unsigned)((float)((col >> IM_COL32_A_SHIFT) & 255) * alpha);
            draw->VtxBuffer[v].col = (col & ~IM_COL32_A_MASK) | (keep << IM_COL32_A_SHIFT);
        }
    }

    static float candle() {
        float t = (float)(ImGui::GetTime());
        return .5f + .25f * std::sin(t * 2.1f) + .15f * std::sin(t * 5.3f + 1.7f) + .1f * std::sin(t * 11.7f + .6f);
    }

    static void rim(ImGuiWindow* window, float alpha) {
        ImDrawList* draw = window->DrawList;
        ImVec2 a = window->Pos;
        ImVec2 b(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
        float font = ImGui::GetFontSize();
        float band = std::max(2.f, std::round(font * .14f));
        float outline = 1.f;
        float rounding = window->WindowRounding;
        ImVec4 band4 = mix(color(ImGuiCol_CheckMark), color(ImGuiCol_WindowBg), .5f);
        band4.w = 1.f;
        draw->PushClipRect(a, b, false);
        int from = draw->VtxBuffer.Size;
        draw->AddRect(ImVec2(a.x + outline * .5f, a.y + outline * .5f), ImVec2(b.x - outline * .5f, b.y - outline * .5f), IM_COL32(0, 0, 0, 200), rounding, 0, outline);
        fade_verts(draw, from, alpha);
        from = draw->VtxBuffer.Size;
        float inset = outline + band * .5f;
        draw->AddRect(ImVec2(a.x + inset, a.y + inset), ImVec2(b.x - inset, b.y - inset), ImGui::ColorConvertFloat4ToU32(band4), std::max(0.f, rounding - inset), 0, band);
        if (g_rainbow_rim) {
            ImVec2 mid((a.x + b.x) * .5f, (a.y + b.y) * .5f);
            float drift = (float)(ImGui::GetTime()) * .16f;
            for (int v = from; v < draw->VtxBuffer.Size; ++v) {
                auto& vert = draw->VtxBuffer[v];
                float hue = std::atan2(vert.pos.y - mid.y, vert.pos.x - mid.x) / 6.2831853f + drift;
                hue -= std::floor(hue);
                ImVec4 shade(0.f, 0.f, 0.f, 1.f);
                ImGui::ColorConvertHSVtoRGB(hue, .6f, 1.f, shade.x, shade.y, shade.z);
                vert.col = (ImGui::ColorConvertFloat4ToU32(shade) & ~IM_COL32_A_MASK) | (vert.col & IM_COL32_A_MASK);
            }
        }
        else if (g_scene != Scene::None) {
            ImVec2 mid((a.x + b.x) * .5f, (a.y + b.y) * .5f);
            float time = (float)(ImGui::GetTime());
            float flame = candle();
            for (int v = from; v < draw->VtxBuffer.Size; ++v) {
                auto& vert = draw->VtxBuffer[v];
                float turn = std::atan2(vert.pos.y - mid.y, vert.pos.x - mid.x);
                ImVec4 shade(1.f, 1.f, 1.f, 1.f);
                switch (g_scene) {
                case Scene::Halloween: {
                    shade = mix(ImVec4(.52f, .22f, .86f, 1.f), ImVec4(1.f, .5f, .08f, 1.f), .5f + .5f * std::sin(turn * 3.f + time * 1.6f));
                    float glow = .72f + .28f * flame;
                    shade.x *= glow;
                    shade.y *= glow;
                    shade.z *= glow;
                    break;
                }
                case Scene::Hell: {
                    float low = std::clamp((vert.pos.y - a.y) / std::max(b.y - a.y, 1.f), 0.f, 1.f);
                    float heat = low * (.65f + .35f * std::sin(vert.pos.x / font * .8f + time * 4.f)) * (.8f + .2f * flame);
                    shade = mix(ImVec4(.4f, .03f, .02f, 1.f), ImVec4(1.f, .42f, .04f, 1.f), std::min(1.f, heat * 1.4f));
                    shade = mix(shade, ImVec4(1.f, .9f, .4f, 1.f), std::clamp((heat - .7f) * 1.5f, 0.f, 1.f));
                    break;
                }
                case Scene::Heaven: {
                    float glint = std::pow(.5f + .5f * std::sin(turn - time * 1.4f), 6.f);
                    shade = mix(ImVec4(1.f, .78f, .3f, 1.f), ImVec4(1.f, 1.f, .95f, 1.f), glint);
                    break;
                }
                case Scene::Space: {
                    float hue = .68f + .14f * std::sin(turn * 2.f + time * .7f);
                    ImGui::ColorConvertHSVtoRGB(hue, .62f, 1.f, shade.x, shade.y, shade.z);
                    float gap = std::remainder(turn - time * 1.2f, 6.2831853f);
                    shade = mix(shade, ImVec4(1.f, 1.f, 1.f, 1.f), std::exp(-gap * gap * 8.f));
                    break;
                }
                case Scene::Ocean: {
                    shade = mix(ImVec4(.1f, .35f, .8f, 1.f), ImVec4(.3f, .95f, .85f, 1.f), .5f + .5f * std::sin(turn * 2.f + std::sin(time * .8f) * 2.f + time * .9f));
                    break;
                }
                case Scene::Sakura: {
                    shade = mix(ImVec4(1.f, .5f, .72f, 1.f), ImVec4(1.f, .93f, .96f, 1.f), std::pow(.5f + .5f * std::sin(turn * 4.f + time * 1.1f), 3.f));
                    break;
                }
                case Scene::None:
                    break;
                }
                shade = turned(shade, accent_turn);
                vert.col = (ImGui::ColorConvertFloat4ToU32(shade) & ~IM_COL32_A_MASK) | (vert.col & IM_COL32_A_MASK);
            }
        }
        fade_verts(draw, from, alpha);
        from = draw->VtxBuffer.Size;
        float inner = outline + band + .5f;
        draw->AddRect(ImVec2(a.x + inner, a.y + inner), ImVec2(b.x - inner, b.y - inner), IM_COL32(0, 0, 0, 70), std::max(0.f, rounding - inner), 0, 1.f);
        fade_verts(draw, from, alpha);
        draw->PopClipRect();
    }

    static void rainbow_body(ImGuiWindow* window, float alpha) {
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        ImVec2 a = window->Pos;
        ImVec2 b(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
        float rounding = window->WindowRounding;
        float time = (float)(ImGui::GetTime());
        float screen = std::max(ImGui::GetIO().DisplaySize.x, 1.f);
        float width = b.x - a.x;
        float tall = std::max(b.y - a.y, 1.f);
        float span = std::max(width, screen * .45f);
        float tilt = .09f * std::sin(time * .7f);
        float light = std::fmod(time * .28f, 1.6f) - .3f;
        auto shade = [&](ImVec2 at) {
            float across = at.x / span;
            float hue = across * .85f - time * .11f + ((at.y - a.y) / tall - .5f) * tilt + .03f * std::sin(time * 1.3f + across * 9.f);
            hue -= std::floor(hue);
            float away = (at.x + (at.y - a.y) * .35f) / screen - light;
            float sheen = std::exp(-away * away * 90.f);
            ImVec4 out(0.f, 0.f, 0.f, 1.f);
            ImGui::ColorConvertHSVtoRGB(hue, .64f - .14f * sheen, .46f + .16f * sheen, out.x, out.y, out.z);
            return ImGui::ColorConvertFloat4ToU32(out);
        };
        int bands = std::clamp((int)(width / em(3.f)), 2, 48);
        int from = draw->VtxBuffer.Size;
        for (int i = 0; i < bands; ++i) {
            ImVec2 min(a.x + width * (float)(i) / (float)(bands), a.y);
            ImVec2 max(a.x + width * (float)(i + 1) / (float)(bands) + (i + 1 < bands ? .5f : 0.f), b.y);
            bool end = i == 0 || i + 1 == bands;
            draw->AddRectFilled(min, max, IM_COL32_WHITE, end ? rounding : 0.f, i == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
        }
        for (int v = from; v < draw->VtxBuffer.Size; ++v) {
            auto& vert = draw->VtxBuffer[v];
            unsigned keep = (unsigned)((float)((vert.col >> IM_COL32_A_SHIFT) & 255) * .96f * alpha);
            vert.col = (shade(vert.pos) & ~IM_COL32_A_MASK) | (keep << IM_COL32_A_SHIFT);
        }
    }

    struct Backdrop {
        ImDrawList* draw = nullptr;
        ImVec2 a, b;
        ImVec2 screen;
        float width = 0.f;
        float tall = 0.f;
        float rounding = 0.f;
        float time = 0.f;
        float unit = 0.f;
        float alpha = 1.f;
        ImGuiID id = 0;
    };

    static float chance(const Backdrop& back, int item, int use) {
        return unit_random((unsigned)item, back.id + (unsigned)use * 7919u);
    }

    static int scatter(const Backdrop& back, float each, int least, int most) {
        return std::clamp((int)(back.width * back.tall / (back.unit * back.unit * each)), least, most);
    }

    static bool outside(const Backdrop& back, ImVec2 at, float reach) {
        return at.x + reach < back.a.x || at.x - reach > back.b.x || at.y + reach < back.a.y || at.y - reach > back.b.y;
    }

    static void soft_disc(ImDrawList* draw, ImVec2 at, float radius, ImVec4 tint, float squash = 1.f, int sides = 16) {
        ImU32 middle = ImGui::ColorConvertFloat4ToU32(tint);
        ImU32 half = ImGui::ColorConvertFloat4ToU32(faded(tint, .38f));
        ImU32 edge = middle & ~IM_COL32_A_MASK;
        ImVec2 uv = draw->_Data->TexUvWhitePixel;
        draw->PrimReserve(sides * 9, sides * 2 + 1);
        unsigned first = draw->_VtxCurrentIdx;
        draw->PrimWriteVtx(at, uv, middle);
        for (float ring : { .45f, 1.f }) {
            for (int i = 0; i < sides; ++i) {
                float angle = 6.2831853f * (float)(i) / (float)(sides);
                draw->PrimWriteVtx(ImVec2(at.x + std::cos(angle) * radius * ring, at.y + std::sin(angle) * radius * ring * squash), uv, ring < 1.f ? half : edge);
            }
        }
        auto idx = [&](unsigned offset) {
            draw->PrimWriteIdx((ImDrawIdx)(first + offset));
        };
        for (int i = 0; i < sides; ++i) {
            unsigned in = 1 + (unsigned)i;
            unsigned in_next = 1 + (unsigned)((i + 1) % sides);
            unsigned out = in + (unsigned)sides;
            unsigned out_next = in_next + (unsigned)sides;
            idx(0);
            idx(in);
            idx(in_next);
            idx(in);
            idx(out);
            idx(out_next);
            idx(in);
            idx(out_next);
            idx(in_next);
        }
    }

    static void beam(ImDrawList* draw, ImVec2 from, ImVec2 left, ImVec2 right, ImVec4 tint) {
        ImU32 near_col = ImGui::ColorConvertFloat4ToU32(tint), far_col = near_col & ~IM_COL32_A_MASK;
        ImVec2 uv = draw->_Data->TexUvWhitePixel;
        draw->PrimReserve(3, 3);
        auto first = (ImDrawIdx)draw->_VtxCurrentIdx;
        draw->PrimWriteVtx(from, uv, near_col);
        draw->PrimWriteVtx(left, uv, far_col);
        draw->PrimWriteVtx(right, uv, far_col);
        draw->PrimWriteIdx(first);
        draw->PrimWriteIdx((ImDrawIdx)(first + 1));
        draw->PrimWriteIdx((ImDrawIdx)(first + 2));
    }

    const unsigned NOISE_X = 374761393u, NOISE_Y = 668265263u, NOISE_MIX = 1274126177u;

    static float noise(float x, float y) {
        auto lattice = [](int ix, int iy) {
            unsigned mixed = (unsigned)ix * NOISE_X + (unsigned)iy * NOISE_Y;
            mixed = (mixed ^ (mixed >> 13)) * NOISE_MIX;
            return (float)((mixed ^ (mixed >> 16)) & 65535) / 65535.f;
        };
        float fx = std::floor(x);
        float fy = std::floor(y);
        int ix = (int)(fx);
        int iy = (int)(fy);
        float tx = x - fx;
        float ty = y - fy;
        tx = tx * tx * (3.f - 2.f * tx);
        ty = ty * ty * (3.f - 2.f * ty);
        return std::lerp(std::lerp(lattice(ix, iy), lattice(ix + 1, iy), tx), std::lerp(lattice(ix, iy + 1), lattice(ix + 1, iy + 1), tx), ty);
    }

    static void ribbon(const Backdrop& back, float y, float thick, float swell, float speed, float seed, ImVec4 tint) {
        if (y + thick * 1.5f + swell < back.a.y || y - thick * 1.5f - swell > back.b.y) return;
        ImVec2 uv = back.draw->_Data->TexUvWhitePixel;
        float time = back.time * speed;

        struct Point {
            float y;
            float thick;
            ImU32 bright;
        };

        auto point = [&](float x) {
            float u = x / back.unit;
            float wander = noise(u * .11f + time * .12f + seed * 7.f, time * .3f + seed) + .45f * noise(u * .33f - time * .2f + seed * 3.f, time * .55f + seed * 5.f) - .725f;
            float patch = std::clamp((noise(u * .2f - time * .16f + seed * 11.f, time * .22f + seed * 2.f) - .22f) * 2.2f, 0.f, 1.f);
            float girth = .55f + .9f * noise(u * .27f + seed * 13.f, time * .4f);
            return Point{ y + swell * 2.f * wander, thick * girth, ImGui::ColorConvertFloat4ToU32(faded(tint, patch)) };
        };
        int strips = std::clamp((int)(back.width / (back.unit * .6f)), 3, 140);
        Point left = point(back.a.x);
        for (int i = 0; i < strips; ++i) {
            float x0 = back.a.x + back.width * (float)(i) / (float)(strips);
            float x1 = back.a.x + back.width * (float)(i + 1) / (float)(strips);
            Point right = point(x1);
            back.draw->PrimReserve(12, 6);
            unsigned first = back.draw->_VtxCurrentIdx;
            back.draw->PrimWriteVtx(ImVec2(x0, left.y - left.thick), uv, left.bright & ~IM_COL32_A_MASK);
            back.draw->PrimWriteVtx(ImVec2(x1, right.y - right.thick), uv, right.bright & ~IM_COL32_A_MASK);
            back.draw->PrimWriteVtx(ImVec2(x0, left.y), uv, left.bright);
            back.draw->PrimWriteVtx(ImVec2(x1, right.y), uv, right.bright);
            back.draw->PrimWriteVtx(ImVec2(x0, left.y + left.thick), uv, left.bright & ~IM_COL32_A_MASK);
            back.draw->PrimWriteVtx(ImVec2(x1, right.y + right.thick), uv, right.bright & ~IM_COL32_A_MASK);
            for (unsigned offset : { 0u, 1u, 3u, 0u, 3u, 2u, 2u, 3u, 5u, 2u, 5u, 4u })
                back.draw->PrimWriteIdx((ImDrawIdx)(first + offset));
            left = right;
        }
    }

    static void backdrop_fill(const Backdrop& back, ImU32 top, ImU32 bottom) {
        int from = back.draw->VtxBuffer.Size;
        back.draw->AddRectFilled(back.a, back.b, IM_COL32_WHITE, back.rounding);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(back.draw, from, back.draw->VtxBuffer.Size, back.a, ImVec2(back.a.x, back.b.y), top, bottom);
        fade_verts(back.draw, from, .97f * back.alpha);
    }

    static void backdrop_glow(const Backdrop& back, ImU32 tint, float from, float strength) {
        float top = back.a.y + back.tall * from;
        int first = back.draw->VtxBuffer.Size;
        back.draw->AddRectFilled(ImVec2(back.a.x, top), back.b, tint, back.rounding, ImDrawFlags_RoundCornersBottom);
        for (int v = first; v < back.draw->VtxBuffer.Size; ++v) {
            auto& vert = back.draw->VtxBuffer[v];
            float low = std::clamp((vert.pos.y - top) / std::max(back.b.y - top, 1.f), 0.f, 1.f);
            unsigned keep = (unsigned)((float)((vert.col >> IM_COL32_A_SHIFT) & 255) * low * strength * back.alpha);
            vert.col = (vert.col & ~IM_COL32_A_MASK) | (keep << IM_COL32_A_SHIFT);
        }
    }

    static void sparks(const Backdrop& back, int count, ImVec4 cool, ImVec4 hot, float speed) {
        for (int i = 0; i < count; ++i) {
            float r1 = chance(back, i, 1), r2 = chance(back, i, 2), r3 = chance(back, i, 3);
            float r4 = chance(back, i, 4);
            float life = back.time * (.05f + .07f * r2) * speed + r3;
            life -= std::floor(life);
            ImVec2 at(back.a.x + back.width * r1 + std::sin(back.time * (.8f + r4) + r3 * 6.2831853f) * back.unit * .6f, back.b.y - life * back.tall * (.55f + .45f * r4));
            float lit = std::sin(3.1415927f * life) * (.6f + .4f * std::sin(back.time * 7.f + r1 * 40.f)) * back.alpha;
            ImVec4 tint = mix(cool, hot, r2);
            float radius = back.unit * (.05f + .07f * r4);
            soft_disc(back.draw, at, radius * 3.2f, faded(tint, .3f * lit), 1.f, 8);
            circle_filled_cached(back.draw, at, radius, ImGui::ColorConvertFloat4ToU32(faded(tint, .8f * lit)), 6);
        }
    }

    static void flames(const Backdrop& back, float height, float speed, float seed, ImVec4 tint) {
        height = std::min(height, back.tall * .6f);
        ImU32 root = ImGui::ColorConvertFloat4ToU32(faded(tint, back.alpha));
        ImU32 tip = root & ~IM_COL32_A_MASK;
        ImVec2 uv = back.draw->_Data->TexUvWhitePixel;
        float time = back.time * speed;
        auto top = [&](float x) {
            float u = x / back.unit;
            float lick = .5f + .22f * std::sin(u * .9f + time * 1.7f + seed) + .16f * std::sin(u * 2.3f - time * 2.9f + seed * 2.f) + .12f * std::sin(u * 5.1f + time * 4.3f);
            return std::clamp(lick, .05f, 1.f) * height;
        };
        auto floor = [&](float x) {
            float in = std::min(x - back.a.x, back.b.x - x);
            if (in >= back.rounding) return back.b.y;
            float short_ = back.rounding - std::max(in, 0.f);
            return back.b.y - (back.rounding - std::sqrt(std::max(back.rounding * back.rounding - short_ * short_, 0.f)));
        };
        int strips = std::clamp((int)(back.width / (back.unit * .4f)), 4, 200);
        for (int i = 0; i < strips; ++i) {
            float x0 = back.a.x + back.width * (float)(i) / (float)(strips);
            float x1 = back.a.x + back.width * (float)(i + 1) / (float)(strips);
            float y0 = floor(x0), y1 = floor(x1);
            back.draw->PrimReserve(6, 4);
            back.draw->PrimQuadUV(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x1, y1 - top(x1)), ImVec2(x0, y0 - top(x0)), uv, uv, uv, uv, root);
            back.draw->VtxBuffer[back.draw->VtxBuffer.Size - 2].col = tip;
            back.draw->VtxBuffer[back.draw->VtxBuffer.Size - 1].col = tip;
        }
    }

    static void halloween_scene(const Backdrop& back) {
        backdrop_fill(back, IM_COL32(24, 11, 40, 255), IM_COL32(10, 4, 14, 255));
        backdrop_glow(back, IM_COL32(255, 120, 20, 255), .45f, .16f + .16f * candle());
        back.draw->PushClipRect(back.a, back.b, true);
        sparks(back, scatter(back, 70.f, 3, 26), ImVec4(1.f, .45f, .08f, 1.f), ImVec4(1.f, .85f, .35f, 1.f), 1.f);
        back.draw->PopClipRect();
    }

    static void hell_scene(const Backdrop& back) {
        float glow = candle();
        backdrop_fill(back, IM_COL32(18, 3, 3, 255), IM_COL32(64, 9, 2, 255));
        backdrop_glow(back, IM_COL32(255, 70, 10, 255), .2f, .26f + .2f * glow);
        back.draw->PushClipRect(back.a, back.b, true);
        flames(back, back.unit * 5.2f, .6f, 0.f, ImVec4(.8f, .08f, .02f, .5f));
        flames(back, back.unit * 3.2f, .85f, 2.3f, ImVec4(1.f, .42f, .04f, .55f));
        int tongues = std::clamp((int)(back.width / (back.unit * .55f)), 6, 70);
        for (int i = 0; i < tongues; ++i) {
            float r1 = chance(back, i, 11), r2 = chance(back, i, 12), r3 = chance(back, i, 13);
            float r4 = chance(back, i, 14);
            float life = back.time * (.45f + .5f * r2) + r3;
            life -= std::floor(life);
            float climb = std::min(back.unit * (3.5f + 5.5f * r4), back.tall * .75f);
            ImVec2 at(back.a.x + back.width * r1 + std::sin(back.time * 2.2f + r3 * 6.2831853f) * back.unit * .5f * life + life * life * back.unit * 1.2f * (r2 - .5f), back.b.y - life * climb);
            ImVec4 orange(1.f, .5f, .06f, 1.f);
            ImVec4 tint = life < .35f ? mix(ImVec4(1.f, .92f, .5f, 1.f), orange, life / .35f) : mix(orange, ImVec4(.75f, .06f, .02f, 1.f), (life - .35f) / .65f);
            float lit = .5f * std::pow(1.f - life, 1.2f) * std::min(1.f, life * 8.f) * back.alpha;
            soft_disc(back.draw, at, back.unit * (1.25f - .95f * life) * (.6f + .6f * r4), faded(tint, lit), 1.25f, 10);
        }
        flames(back, back.unit * 1.4f, 1.2f, 4.1f, ImVec4(1.f, .88f, .4f, .55f));
        sparks(back, scatter(back, 26.f, 8, 40), ImVec4(1.f, .3f, .05f, 1.f), ImVec4(1.f, .8f, .3f, 1.f), 3.2f);
        int cinders = scatter(back, 120.f, 2, 12);
        for (int i = 0; i < cinders; ++i) {
            float r1 = chance(back, i, 21), r2 = chance(back, i, 22), r3 = chance(back, i, 23);
            float life = back.time * (.12f + .1f * r2) + r3;
            life -= std::floor(life);
            ImVec2 at(back.a.x + back.width * r1 + (r2 - .5f) * back.unit * 10.f * life, back.b.y - back.tall * life * (.7f + .3f * r3));
            float lit = std::sin(3.1415927f * life) * (.55f + .45f * std::sin(back.time * 13.f + r1 * 30.f)) * back.alpha;
            soft_disc(back.draw, at, back.unit * .5f, ImVec4(1.f, .4f, .05f, .5f * lit), 1.f, 8);
            circle_filled_cached(back.draw, at, back.unit * .1f, ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .85f, .4f, lit)), 6);
        }
        back.draw->PopClipRect();
    }

    static void heaven_scene(const Backdrop& back) {
        float dawn = .5f + .5f * std::sin(back.time * .3f);
        auto by_night = [](ImVec4 day, ImVec4 dusk, ImVec4 dark) {
            return night < .5f ? mix(day, dusk, night * 2.f) : mix(dusk, dark, night * 2.f - 1.f);
        };
        ImVec4 sky_top = by_night(mix(ImVec4(.70f, .85f, 1.f, 1.f), ImVec4(1.f, .82f, .9f, 1.f), dawn), ImVec4(.36f, .27f, .66f, 1.f), mix(ImVec4(.05f, .06f, .18f, 1.f), ImVec4(.1f, .06f, .2f, 1.f), dawn));
        ImVec4 sky_low = by_night(ImVec4(1.f, .96f, .86f, 1.f), ImVec4(1.f, .6f, .42f, 1.f), ImVec4(.2f, .14f, .3f, 1.f));
        ImVec4 light_tint(1.f, .9f, .55f, 1.f);
        ImVec4 beam_tint(1.f, .8f, .3f, 1.f);
        ImVec4 ring_tint(1.f, .86f, .45f, 1.f);
        ImVec4 cloud_shade = by_night(ImVec4(.52f, .64f, .88f, 1.f), ImVec4(.5f, .32f, .55f, 1.f), ImVec4(.12f, .12f, .26f, 1.f));
        ImVec4 cloud = by_night(ImVec4(1.f, 1.f, 1.f, 1.f), ImVec4(1.f, .8f, .74f, 1.f), ImVec4(.42f, .44f, .62f, 1.f));
        float glow = 1.f - .45f * night;
        backdrop_fill(back, ImGui::ColorConvertFloat4ToU32(sky_top), ImGui::ColorConvertFloat4ToU32(sky_low));
        back.draw->PushClipRect(back.a, back.b, true);
        ImVec2 sun(back.screen.x * .5f, -back.screen.y * .12f);
        soft_disc(back.draw, sun, back.screen.y * (.75f + .12f * std::sin(back.time * 1.1f)), faded(light_tint, .55f * glow * back.alpha), 1.f, 32);
        float reach = back.screen.y * 1.8f;
        for (int k = 0; k < 9; ++k) {
            float number = (float)(k);
            float lean = std::remainder(number * .6981317f + back.time * .09f, 6.2831853f);
            if (std::abs(lean) > 1.45f) continue;
            float wide = .08f + .04f * std::sin(back.time * .9f + number * 2.f);
            beam(back.draw, sun, ImVec2(sun.x + std::sin(lean - wide) * reach, sun.y + std::cos(lean - wide) * reach), ImVec2(sun.x + std::sin(lean + wide) * reach, sun.y + std::cos(lean + wide) * reach), faded(beam_tint, .6f * glow * back.alpha));
        }
        for (int k = 0; k < 4; ++k) {
            float spread = back.time * .16f + (float)(k) * .25f;
            spread -= std::floor(spread);
            back.draw->AddCircle(sun, back.screen.y * (.15f + 1.5f * spread), ImGui::ColorConvertFloat4ToU32(faded(ring_tint, .55f * glow * (1.f - spread) * std::min(1.f, spread * 6.f) * back.alpha)), 96, back.unit * (.12f + .5f * spread));
        }
        const float PUFFS[][3] = { { -1.3f, .15f, .8f }, { -.5f, -.25f, 1.f }, { .4f, -.1f, 1.15f }, { 1.2f, .2f, .75f } };
        for (int k = 0; k < 6; ++k) {
            float number = (float)(k);
            float size = back.unit * (2.2f + .5f * (float)(k % 3));
            float span = back.screen.x + back.unit * 24.f;
            ImVec2 at(std::fmod(back.time * back.unit * (1.6f + .7f * number) + number * span * .23f, span) - back.unit * 12.f, back.screen.y * (.1f + .16f * number) + std::sin(back.time * .5f + number) * back.unit * .8f);
            if (outside(back, at, size * 2.8f)) continue;
            for (const auto& puff : PUFFS)
                soft_disc(back.draw, ImVec2(at.x + puff[0] * size, at.y + (puff[1] + .28f) * size), size * puff[2], faded(cloud_shade, .45f * back.alpha), .6f);
            for (const auto& puff : PUFFS)
                soft_disc(back.draw, ImVec2(at.x + puff[0] * size, at.y + puff[1] * size), size * puff[2], faded(cloud, .95f * back.alpha), .62f);
        }
        sparks(back, scatter(back, 45.f, 4, 30), ImVec4(1.f, .62f, .1f, 1.f), ImVec4(1.f, .8f, .3f, 1.f), 1.4f);
        int glints = scatter(back, 40.f, 4, 30);
        for (int i = 0; i < glints; ++i) {
            float twinkle = std::pow(.5f + .5f * std::sin(back.time * (2.f + 3.f * chance(back, i, 3)) + chance(back, i, 4) * 6.2831853f), 3.f);
            float size = back.unit * (.22f + .34f * chance(back, i, 5)) * twinkle;
            ImVec2 at(back.a.x + back.width * chance(back, i, 1), back.a.y + back.tall * chance(back, i, 2));
            ImU32 gold = ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .66f, .08f, .95f * twinkle * back.alpha));
            back.draw->AddQuadFilled(ImVec2(at.x - size, at.y), ImVec2(at.x, at.y - size * .18f), ImVec2(at.x + size, at.y), ImVec2(at.x, at.y + size * .18f), gold);
            back.draw->AddQuadFilled(ImVec2(at.x, at.y - size), ImVec2(at.x + size * .18f, at.y), ImVec2(at.x, at.y + size), ImVec2(at.x - size * .18f, at.y), gold);
        }
        back.draw->PopClipRect();
    }

    static void space_scene(const Backdrop& back) {
        backdrop_fill(back, IM_COL32(4, 5, 15, 255), IM_COL32(4, 5, 15, 255));
        back.draw->PushClipRect(back.a, back.b, true);
        float diagonal = std::sqrt(back.width * back.width + back.tall * back.tall);
        auto drifted = [&](ImVec2 at, float speed) {
            float x = at.x - back.a.x - back.time * back.unit * speed;
            x -= std::floor(x / back.width) * back.width;
            return ImVec2(back.a.x + x, at.y);
        };
        auto spikes = [&](ImVec2 at, float reach, float thin, ImVec4 tint) {
            ImU32 bright = ImGui::ColorConvertFloat4ToU32(tint);
            ImU32 gone = bright & ~IM_COL32_A_MASK;
            ImVec2 uv = back.draw->_Data->TexUvWhitePixel;
            for (ImVec2 dir : { ImVec2(1.f, 0.f), ImVec2(0.f, 1.f) }) {
                ImVec2 side(-dir.y * thin, dir.x * thin);
                back.draw->PrimReserve(12, 6);
                unsigned first = back.draw->_VtxCurrentIdx;
                back.draw->PrimWriteVtx(ImVec2(at.x - dir.x * reach, at.y - dir.y * reach), uv, gone);
                back.draw->PrimWriteVtx(ImVec2(at.x + side.x, at.y + side.y), uv, bright);
                back.draw->PrimWriteVtx(ImVec2(at.x + dir.x * reach, at.y + dir.y * reach), uv, gone);
                back.draw->PrimWriteVtx(ImVec2(at.x - side.x, at.y - side.y), uv, bright);
                back.draw->PrimWriteVtx(at, uv, bright);
                back.draw->PrimWriteVtx(at, uv, bright);
                for (unsigned offset : { 0u, 1u, 4u, 0u, 3u, 4u, 2u, 1u, 4u, 2u, 3u, 4u })
                    back.draw->PrimWriteIdx((ImDrawIdx)(first + offset));
            }
        };
        float band_angle = -.35f - .3f * chance(back, 0, 90);
        ImVec2 band_dir(std::cos(band_angle), std::sin(band_angle));
        ImVec2 band_side(-band_dir.y, band_dir.x);
        ImVec2 band_mid(back.a.x + back.width * (.35f + .3f * chance(back, 0, 91)), back.a.y + back.tall * (.4f + .2f * chance(back, 0, 92)));
        float band_wide = diagonal * .08f;
        int dust = scatter(back, 1.f, 80, 900);
        for (int i = 0; i < dust; ++i) {
            float along = (chance(back, i, 110) - .5f) * diagonal * 1.1f;
            float across = (chance(back, i, 111) + chance(back, i, 112) + chance(back, i, 113) - 1.5f) * band_wide;
            ImVec2 at = drifted(ImVec2(band_mid.x + band_dir.x * along + band_side.x * across, band_mid.y + band_dir.y * along + band_side.y * across), .1f);
            float lit = (.15f + .35f * chance(back, i, 115)) * (.7f + .3f * std::sin(back.time * (1.f + 2.f * chance(back, i, 116)) + 30.f * chance(back, i, 117))) * back.alpha;
            float size = std::max(.6f, back.unit * .035f);
            back.draw->AddRectFilled(ImVec2(at.x - size * .5f, at.y - size * .5f), ImVec2(at.x + size * .5f, at.y + size * .5f), ImGui::ColorConvertFloat4ToU32(ImVec4(.85f, .86f, 1.f, lit)));
        }
        const float TEMPS[][4] = { { .66f, .78f, 1.f, .2f }, { .86f, .9f, 1.f, .75f }, { 1.f, .95f, .84f, .93f }, { 1.f, .8f, .62f, 1.f } };
        const float LAYER_SPEED[] = { .15f, .4f, .9f };
        int stars = scatter(back, 3.5f, 30, 260);
        for (int i = 0; i < stars; ++i) {
            float r = chance(back, i, 3);
            float bright = r * r * r;
            int layer = std::min(2, (int)(chance(back, i, 8) * 3.f + bright * 1.5f));
            ImVec2 at = drifted(ImVec2(back.a.x + back.width * chance(back, i, 1), back.a.y + back.tall * chance(back, i, 2)), LAYER_SPEED[layer]);
            float pick = chance(back, i, 5);
            int temp = 0;
            while (temp < 3 && pick > TEMPS[temp][3])
                ++temp;
            ImVec4 tint(TEMPS[temp][0], TEMPS[temp][1], TEMPS[temp][2], 1.f);
            float wave = .5f + .5f * std::sin(back.time * (1.5f + 4.f * chance(back, i, 4)) + 40.f * chance(back, i, 6));
            float twinkle = 1.f - (.3f + .45f * (1.f - bright)) * wave * wave;
            float lit = (.3f + .7f * bright) * twinkle * back.alpha;
            float size = back.unit * (.035f + .08f * bright) * (.75f + .25f * (float)(layer));
            circle_filled_cached(back.draw, at, std::max(size, .7f), ImGui::ColorConvertFloat4ToU32(faded(tint, lit)), 6);
            if (bright > .75f)
                spikes(at, size * (5.f + 6.f * twinkle), std::max(.5f, size * .3f), faded(tint, .6f * lit));
        }
        {
            float every = 14.f + 8.f * chance(back, 0, 61);
            float clock = back.time / every + chance(back, 0, 62);
            int which = (int)(std::floor(clock));
            float run = (clock - std::floor(clock)) / .7f;
            if (run < 1.f) {
                float y0 = back.a.y + back.tall * (.1f + .8f * chance(back, which, 63));
                float y1 = back.a.y + back.tall * (.1f + .8f * chance(back, which, 64));
                bool rightward = chance(back, which, 65) < .5f;
                float x = rightward ? back.a.x + back.width * run : back.b.x - back.width * run;
                ImVec2 at(x, y0 + (y1 - y0) * run);
                float blink = std::fmod(back.time, 1.1f) < .12f ? 1.f : .45f;
                circle_filled_cached(back.draw, at, std::max(.8f, back.unit * .06f), ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .95f, .9f, blink * back.alpha)), 6);
            }
        }
        for (int lane = 0; lane < 2; ++lane) {
            float every = 2.5f + 2.5f * chance(back, lane, 71);
            float clock = back.time / every + chance(back, lane, 72);
            int which = (int)(std::floor(clock));
            float run = (clock - std::floor(clock)) / .22f;
            if (run >= 1.f) continue;
            int seed = which * 2 + lane;
            ImVec2 start(back.a.x + back.width * (.05f + .7f * chance(back, seed, 73)), back.a.y + back.tall * (.02f + .45f * chance(back, seed, 74)));
            float angle = .3f + .5f * chance(back, seed, 75);
            float travel = back.unit * (14.f + 12.f * chance(back, seed, 76));
            ImVec2 dir(std::cos(angle), std::sin(angle));
            ImVec2 head(start.x + dir.x * travel * run, start.y + dir.y * travel * run);
            float fade = std::sin(3.1415927f * run) * back.alpha;
            float tail = back.unit * (2.f + 5.f * run);
            ImVec2 end(head.x - dir.x * tail, head.y - dir.y * tail);
            ImU32 bright = ImGui::ColorConvertFloat4ToU32(ImVec4(.92f, .95f, 1.f, .9f * fade));
            ImU32 gone = bright & ~IM_COL32_A_MASK;
            float thick = std::max(.8f, back.unit * .07f);
            ImVec2 side(-dir.y * thick, dir.x * thick);
            back.draw->PrimReserve(3, 3);
            auto base = back.draw->_VtxCurrentIdx;
            ImVec2 uv = back.draw->_Data->TexUvWhitePixel;
            back.draw->PrimWriteVtx(ImVec2(head.x + side.x, head.y + side.y), uv, bright);
            back.draw->PrimWriteVtx(ImVec2(head.x - side.x, head.y - side.y), uv, bright);
            back.draw->PrimWriteVtx(end, uv, gone);
            back.draw->PrimWriteIdx((ImDrawIdx)base);
            back.draw->PrimWriteIdx((ImDrawIdx)(base + 1));
            back.draw->PrimWriteIdx((ImDrawIdx)(base + 2));
            circle_filled_cached(back.draw, head, thick * 1.2f, bright, 6);
        }
        back.draw->PopClipRect();
    }

    static void ocean_scene(const Backdrop& back) {
        backdrop_fill(back, IM_COL32(10, 86, 114, 255), IM_COL32(2, 12, 38, 255));
        back.draw->PushClipRect(back.a, back.b, true);
        float reach = back.screen.y * 1.5f;
        for (int k = 0; k < 5; ++k) {
            float number = (float)(k);
            ImVec2 from(back.screen.x * (.1f + .2f * number) + std::sin(back.time * .3f + number * 1.9f) * back.unit * 3.f, -back.screen.y * .2f);
            float lean = -.28f + .08f * std::sin(back.time * .35f + number);
            float wide = .05f + .02f * std::sin(back.time * .6f + number * 2.f);
            beam(back.draw, from, ImVec2(from.x + std::sin(lean - wide) * reach, from.y + std::cos(lean - wide) * reach), ImVec2(from.x + std::sin(lean + wide) * reach, from.y + std::cos(lean + wide) * reach), ImVec4(.5f, 1.f, .9f, .34f * back.alpha));
        }
        for (int k = 0; k < 6; ++k) {
            float number = (float)(k);
            float depth = back.screen.y * (.06f + .18f * number);
            ribbon(back, depth, back.unit * (1.5f + .3f * number), back.unit * (1.6f + .3f * number), .8f + .1f * number, number * 1.7f, ImVec4(.4f, .92f, 1.f, (.3f - .035f * number) * back.alpha));
            for (int line = 0; line < 2; ++line) {
                float shift = (float)(line);
                ribbon(back, depth + back.unit * (2.2f + 2.6f * shift), back.unit * .3f, back.unit * (1.8f + .5f * shift), (line ? -1.9f : 1.6f) - .1f * number, number * 2.9f + shift * 4.f + 20.f, ImVec4(.75f, 1.f, .97f, (.42f - .045f * number) * back.alpha));
            }
        }
        int motes = scatter(back, 20.f, 6, 60);
        for (int i = 0; i < motes; ++i) {
            float r = chance(back, i, 51);
            float drift = chance(back, i, 52) + back.time * (.012f + .03f * r);
            drift -= std::floor(drift);
            ImVec2 at(back.a.x + back.width * drift, back.a.y + back.tall * chance(back, i, 53) + std::sin(back.time * (.5f + r) + r * 30.f) * back.unit * .7f);
            soft_disc(back.draw, at, back.unit * (.1f + .1f * r), ImVec4(.75f, 1.f, .95f, (.25f + .35f * r) * back.alpha), 1.f, 6);
        }
        int bubbles = scatter(back, 45.f, 5, 30);
        for (int i = 0; i < bubbles; ++i) {
            float r = chance(back, i, 2);
            float rise = back.time * (.06f + .08f * r) + chance(back, i, 3);
            rise -= std::floor(rise);
            float radius = back.unit * (.1f + .26f * chance(back, i, 4));
            ImVec2 at(back.a.x + back.width * chance(back, i, 1) + std::sin(back.time * (1.2f + r) + r * 20.f) * back.unit * .4f, back.b.y + radius - rise * (back.tall + radius * 2.f));
            float lit = std::min(1.f, (1.f - rise) * 4.f) * back.alpha;
            back.draw->AddCircle(at, radius, ImGui::ColorConvertFloat4ToU32(ImVec4(.75f, 1.f, 1.f, .42f * lit)), 14, std::max(1.f, back.unit * .05f));
            circle_filled_cached(back.draw, ImVec2(at.x - radius * .35f, at.y - radius * .35f), radius * .2f, ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, .5f * lit)), 6);
        }
        back.draw->PopClipRect();
    }

    static void sakura_scene(const Backdrop& back) {
        backdrop_fill(back, IM_COL32(44, 19, 48, 255), IM_COL32(96, 38, 72, 255));
        backdrop_glow(back, IM_COL32(255, 150, 190, 255), .45f, .16f);
        back.draw->PushClipRect(back.a, back.b, true);
        int lights = scatter(back, 110.f, 2, 10);
        for (int i = 0; i < lights; ++i) {
            float r = chance(back, i, 61);
            ImVec2 at(back.a.x + back.width * chance(back, i, 62) + std::sin(back.time * (.2f + .2f * r) + r * 9.f) * back.unit * 1.5f, back.a.y + back.tall * chance(back, i, 63) + std::cos(back.time * (.17f + .2f * r) + r * 5.f) * back.unit);
            soft_disc(back.draw, at, back.unit * (1.3f + 1.5f * r), ImVec4(1.f, .8f, .88f, (.1f + .07f * std::sin(back.time * (.6f + r) + r * 20.f)) * back.alpha));
        }
        int petals = scatter(back, 38.f, 5, 36);
        for (int i = 0; i < petals; ++i) {
            float r1 = chance(back, i, 1), r2 = chance(back, i, 2), r3 = chance(back, i, 3);
            float r4 = chance(back, i, 4);
            float fall = back.time * (.045f + .05f * r2) + r3;
            fall -= std::floor(fall);
            float side = r1 * back.width + fall * back.unit * 6.f * (.4f + r4) + std::sin(back.time * (1.f + r2) + r3 * 6.2831853f) * back.unit * .8f;
            ImVec2 at(back.a.x + std::fmod(side + back.width, back.width), back.a.y - back.unit + fall * (back.tall + back.unit * 2.f));
            float spin = back.time * (1.f + 2.f * r4) * (r3 > .5f ? 1.f : -1.f) + r3 * 6.2831853f;
            float tumble = .3f + .7f * std::abs(std::sin(back.time * (1.5f + 2.f * r2) + r1 * 9.f));
            float size = back.unit * (.22f + .2f * r4);
            ImVec4 tint = mix(ImVec4(1.f, .6f, .77f, 1.f), ImVec4(1.f, .9f, .94f, 1.f), r2);
            back.draw->AddEllipseFilled(at, ImVec2(size, size * .55f * tumble), ImGui::ColorConvertFloat4ToU32(faded(tint, .88f * back.alpha)), spin, 10);
        }
        back.draw->PopClipRect();
    }

    struct Splash {
        float began = 0.f;
        float at = 0.f;
        unsigned seed = 0;
    };

    struct Liquid {
        float level;
        float swing;
        float settle;
        float height;
        float idle;
        int drops;
        float drop_size, throw_up, spread, gravity;
        ImVec4 body, top, drop;
    };

    const Liquid WATER{ .72f, 1.f, .7f, .9f, 1.f, 10, .065f, 4.5f, 6.f, 16.f, { .65f, 1.f, .95f, .4f }, { .65f, 1.f, .95f, .65f }, { .75f, .97f, 1.f, .85f } };
    const Liquid LAVA{ .74f, .55f, 1.1f, .6f, .5f, 4, .11f, 2.6f, 2.5f, 7.f, { 1.f, .38f, .05f, .75f }, { 1.f, .78f, .25f, .95f }, { 1.f, .6f, .15f, .95f } };

    static std::unordered_map<ImGuiID, std::vector<Splash>> g_splashes;
    static void button_skin(ImDrawList* draw, const ImRect& body, float rounding, ImGuiID id, float hot, float down, float keep, const ImVec4* given, float given_amount) {
        Scene which = g_scene;
        if (which == Scene::None || keep <= .01f) return;

        struct LiquidLook {
            ImVec4 top, bottom;
            ImVec4 glow;
            ImVec4 edge;
        };

        static const LiquidLook LIQUID_LOOKS[] = {
            {},
            { { .30f, .15f, .44f, 1.f }, { .13f, .06f, .21f, 1.f }, { 1.f, .5f, .08f, 1.f }, { 1.f, .55f, .12f, 1.f } },
            { { .44f, .07f, .04f, 1.f }, { .15f, .02f, .02f, 1.f }, { 1.f, .45f, .05f, 1.f }, { 1.f, .4f, .08f, 1.f } },
            { { 1.f, .97f, .86f, 1.f }, { 1.f, .83f, .48f, 1.f }, { 1.f, 1.f, 1.f, 1.f }, { .85f, .62f, .18f, 1.f } },
            { { .21f, .16f, .5f, 1.f }, { .07f, .05f, .22f, 1.f }, { .6f, .88f, 1.f, 1.f }, { .5f, .6f, 1.f, 1.f } },
            { { .1f, .5f, .62f, 1.f }, { .03f, .19f, .37f, 1.f }, { .65f, 1.f, .95f, 1.f }, { .35f, .9f, .85f, 1.f } },
            { { .64f, .3f, .48f, 1.f }, { .33f, .13f, .3f, 1.f }, { 1.f, .82f, .9f, 1.f }, { 1.f, .62f, .8f, 1.f } },
        };
        LiquidLook look = LIQUID_LOOKS[(int)(which)];
        ColorGrade dim{ 0.f, 1.f, body_turn.brightness };
        look.top = turned(turned(look.top, accent_turn), dim);
        look.bottom = turned(turned(look.bottom, accent_turn), dim);
        if (night_menu > 0.f) {
            ImVec4 night_button = color(ImGuiCol_Button);
            night_button.w = 1.f;
            look.top = mix(look.top, mix(night_button, ImVec4(1.f, 1.f, 1.f, 1.f), .08f), night_menu);
            look.bottom = mix(look.bottom, ImVec4(night_button.x * .7f, night_button.y * .7f, night_button.z * .7f, 1.f), night_menu);
        }
        look.glow = turned(look.glow, accent_turn);
        look.edge = turned(look.edge, accent_turn);
        float lively = 1.f;
        if (given) {
            look.top = mix(look.top, mix(*given, ImVec4(1.f, 1.f, 1.f, 1.f), .14f), given_amount);
            look.bottom = mix(look.bottom, ImVec4(given->x * .58f, given->y * .58f, given->z * .58f, 1.f), given_amount);
            lively = std::lerp(1.f, std::clamp(std::max({ given->x, given->y, given->z }) * 1.8f, .3f, 1.f), given_amount);
        }
        float alpha = ImGui::GetStyle().Alpha * keep;
        float time = (float)(ImGui::GetTime());
        float wide = body.GetWidth();
        float tall = std::max(body.GetHeight(), 1.f);
        auto own = [&](int item) {
            return unit_random((unsigned)item, id);
        };
        auto shaded = [&](ImVec4 tint) {
            tint = mix(mix(tint, look.glow, hot * .22f), ImVec4(0.f, 0.f, 0.f, 1.f), down * .2f);
            tint.w = 1.f;
            return ImGui::ColorConvertFloat4ToU32(tint);
        };
        int from = draw->VtxBuffer.Size;
        draw->AddRectFilled(body.Min, body.Max, shaded(mix(look.top, look.bottom, .6f)), rounding);
        fade_verts(draw, from, .94f * alpha);
        alpha *= lively;

        draw->PushClipRect(body.Min, body.Max, true);
        auto sparkle = [&](ImVec2 at, float reach, ImVec4 tint) {
            float thin = reach * .18f;
            ImU32 col = ImGui::ColorConvertFloat4ToU32(tint);
            draw->AddQuadFilled(ImVec2(at.x - reach, at.y), ImVec2(at.x, at.y - thin), ImVec2(at.x + reach, at.y), ImVec2(at.x, at.y + thin), col);
            draw->AddQuadFilled(ImVec2(at.x, at.y - reach), ImVec2(at.x + thin, at.y), ImVec2(at.x, at.y + reach), ImVec2(at.x - thin, at.y), col);
        };
        auto line = [&](float y, float thick, ImVec4 tint, float inset) {
            draw->AddLine(ImVec2(body.Min.x + inset, y), ImVec2(body.Max.x - inset, y), ImGui::ColorConvertFloat4ToU32(tint), thick);
        };
        auto tank = [&](const Liquid& liquid) {
            ImGuiContext& g = *GImGui;
            auto& presses = g_splashes[id];
            if (g.ActiveId == id && g.ActiveIdIsJustActivated) {
                static unsigned count = 0;
                presses.push_back({ time, std::clamp((ImGui::GetIO().MousePos.x - body.Min.x) / std::max(wide, 1.f), 0.f, 1.f), ++count });
            }
            std::erase_if(presses, [&](const Splash& press) {
                return time - press.began > liquid.settle * 4.6f;
            });
            const int MODES = 8;
            float modes[MODES] = {};
            float total = 0.f;
            for (int n = 1; n <= MODES; ++n)
                total += std::exp(-(float)(n * n) * .02f);
            for (const auto& press : presses) {
                float since = time - press.began;
                float settle = std::exp(-since / liquid.settle);
                for (int n = 1; n <= MODES; ++n) {
                    float share = std::exp(-(float)(n * n) * .02f) / total;
                    modes[n - 1] += share * std::cos(3.1415927f * (float)(n) * press.at) * std::sin(6.2831853f * liquid.swing * std::sqrt((float)(n)) * since) * settle;
                }
            }
            int points = std::clamp((int)(wide / (tall * .2f)), 8, 64);
            float level = liquid.level + .03f * std::sin(time * .7f * liquid.idle + own(6) * 6.2831853f);
            auto surface = [&](float x) {
                float u = (x - body.Min.x) / std::max(wide, 1.f);
                float slosh = 0.f;
                for (int n = 1; n <= MODES; ++n)
                    slosh += modes[n - 1] * std::cos(3.1415927f * (float)(n) * u);
                return body.Min.y + tall * std::clamp(level + .07f * (noise(x / tall * .5f + time * .45f * liquid.idle, time * .4f * liquid.idle + own(1) * 9.f) - .5f) + liquid.height * slosh, .04f, .98f);
            };
            // the buttons real outline, rounded corners & all (the same points its frame is drawn w/), so the
            // liquid stops where the button does & not at the rectangle
            draw->PathRect(body.Min, body.Max, rounding);
            std::vector<ImVec2> outline(draw->_Path.begin(), draw->_Path.end());
            draw->PathClear();
            auto inside = [&](float x, float& top, float& bottom) {
                top = body.Max.y;
                bottom = body.Min.y;
                for (std::size_t i = 0; i < outline.size(); ++i) {
                    ImVec2 a = outline[i], b = outline[(i + 1) % outline.size()];
                    if (std::abs(a.x - b.x) < 1e-3f) {
                        if (std::abs(x - a.x) > 1e-3f) continue;
                        top = std::min({ top, a.y, b.y });
                        bottom = std::max({ bottom, a.y, b.y });
                        continue;
                    }
                    if (x < std::min(a.x, b.x) || x > std::max(a.x, b.x)) continue;
                    float y = a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            };
            std::vector<float> xs;
            for (int i = 0; i <= points; ++i)
                xs.push_back(body.Min.x + wide * (float)(i) / (float)(points));
            for (const auto& corner : outline)
                xs.push_back(std::clamp(corner.x, body.Min.x, body.Max.x));
            std::sort(xs.begin(), xs.end());
            std::vector<ImVec2> top_line, floor_line;
            for (float x : xs) {
                float top = 0.f, bottom = 0.f;
                inside(x, top, bottom);
                if (bottom < top) continue;
                top_line.emplace_back(x, std::clamp(surface(x), top, bottom));
                floor_line.emplace_back(x, bottom);
            }
            for (const auto& at : top_line)
                draw->PathLineTo(at);
            for (auto it = floor_line.rbegin(); it != floor_line.rend(); ++it)
                draw->PathLineTo(*it);
            draw->PathFillConcave(ImGui::ColorConvertFloat4ToU32(faded(turned(liquid.body, accent_turn), alpha)));
            for (const auto& at : top_line)
                draw->PathLineTo(at);
            draw->PathStroke(ImGui::ColorConvertFloat4ToU32(faded(turned(liquid.top, accent_turn), alpha)), 0, std::max(1.f, tall * .045f));
            float flight = 2.f * liquid.throw_up / liquid.gravity;
            auto fold = [](float v, float lo, float hi) {
                float span = std::max(hi - lo, 1.f);
                float t = std::fmod(v - lo, span * 2.f);
                if (t < 0.f) t += span * 2.f;
                return lo + (t <= span ? t : span * 2.f - t);
            };
            for (const auto& press : presses) {
                float since = time - press.began;
                if (since > flight) continue;
                float start_x = body.Min.x + wide * press.at;
                float start_y = body.Min.y + tall * level;
                for (int i = 0; i < liquid.drops; ++i) {
                    float r1 = unit_random((unsigned)i, press.seed);
                    float r2 = unit_random((unsigned)i, press.seed + 7919u);
                    float radius = std::max(1.f, tall * liquid.drop_size * (.6f + .8f * r2));
                    float x = start_x + (r1 - .5f) * tall * liquid.spread * since;
                    float y = start_y - tall * liquid.throw_up * (.6f + .4f * r2) * since + .5f * tall * liquid.gravity * since * since;
                    ImVec2 at(fold(x, body.Min.x + radius, body.Max.x - radius), fold(y, body.Min.y + radius, body.Max.y));
                    if (at.y > surface(at.x) && since > .08f) continue;
                    float top = 0.f, bottom = 0.f;
                    inside(at.x, top, bottom);
                    if (at.y - radius < top || at.y + radius > bottom) continue;
                    circle_filled_cached(draw, at, radius, ImGui::ColorConvertFloat4ToU32(faded(turned(liquid.drop, accent_turn), alpha)), 10);
                }
            }
        };
        switch (g_button_effects ? which : Scene::None) {
        case Scene::Halloween: {
            float flame = .55f + .25f * std::sin(time * (3.f + 3.f * own(1)) + own(2) * 40.f) + .2f * std::sin(time * (9.f + 4.f * own(3)) + own(4) * 20.f);
            line(body.Max.y - 1.5f, std::max(1.5f, tall * .07f), faded(look.glow, flame * alpha), rounding * .5f);
            float every = 4.f + 3.f * own(11);
            float clock = time / every + own(12);
            float run = (clock - std::floor(clock)) / .5f;
            if (run < 1.f) {
                int which = (int)(std::floor(clock));
                float lift = unit_random((unsigned)which, id + 17u);
                ImVec2 at(body.Min.x - tall * .4f + run * (wide + tall * .8f), body.Min.y + tall * (.35f + .3f * lift) + std::sin(run * 18.f) * tall * .08f);
                float span = tall * .22f;
                float flap = std::sin(time * 22.f) * span * .6f;
                ImU32 bat = ImGui::ColorConvertFloat4ToU32(ImVec4(.06f, .02f, .1f, .9f * alpha));
                draw->AddTriangleFilled(at, ImVec2(at.x - span, at.y - flap), ImVec2(at.x - span * .45f, at.y + span * .25f), bat);
                draw->AddTriangleFilled(at, ImVec2(at.x + span, at.y - flap), ImVec2(at.x + span * .45f, at.y + span * .25f), bat);
                circle_filled_cached(draw, at, span * .2f, bat, 8);
            }
            break;
        }
        case Scene::Hell: {
            tank(LAVA);
            for (int i = 0; i < 4; ++i) {
                float life = time * (.5f + .5f * own(10 + i)) + own(20 + i);
                life -= std::floor(life);
                ImVec2 at(body.Min.x + wide * own(30 + i) + std::sin(time * 3.f + own(40 + i) * 9.f) * tall * .12f, body.Min.y + tall * LAVA.level - life * tall * .9f);
                ImVec4 ember = mix(ImVec4(1.f, .85f, .3f, 1.f), ImVec4(1.f, .35f, .05f, 1.f), life);
                ember = turned(ember, accent_turn);
                ember.w = (1.f - life) * alpha;
                circle_filled_cached(draw, at, std::max(.8f, tall * .045f * (1.f - life * .5f)), ImGui::ColorConvertFloat4ToU32(ember), 6);
            }
            break;
        }
        case Scene::Heaven: {
            float breath = .55f + .25f * std::sin(time * 1.3f + own(6) * 6.2831853f) + .2f * hot;
            float inset = std::max(2.f, tall * .12f);
            draw->AddRect(ImVec2(body.Min.x + inset, body.Min.y + inset), ImVec2(body.Max.x - inset, body.Max.y - inset), ImGui::ColorConvertFloat4ToU32(faded(look.edge, breath * alpha)), std::max(0.f, rounding - inset), 0, 1.f);
            int glints = std::clamp((int)(wide / (tall * 2.5f)), 1, 4);
            for (int i = 0; i < glints; ++i) {
                float clock = time * (.35f + .25f * own(50 + i)) + own(60 + i);
                int which = (int)(std::floor(clock));
                float life = clock - std::floor(clock);
                float pop = std::max(0.f, std::sin(3.1415927f * life));
                unsigned spot = (unsigned)(which * 8 + i);
                ImVec2 at(body.Min.x + wide * (.06f + .88f * unit_random(spot, id + 3u)), body.Min.y + tall * (.2f + .6f * unit_random(spot, id + 4u)));
                sparkle(at, tall * .2f * pop, faded(look.edge, .9f * pop * alpha));
            }
            break;
        }
        case Scene::Sakura: {
            for (int i = 0; i < 2; ++i) {
                float r = own(10 + i);
                float pass = time * (.05f + .04f * r) + own(20 + i);
                pass -= std::floor(pass);
                ImVec2 at(body.Min.x - tall * .3f + pass * (wide + tall * .6f), body.Min.y + tall * (.3f + .4f * own(30 + i)) + std::sin(time * (1.2f + r) + r * 20.f) * tall * .15f);
                float spin = time * (.8f + r) + r * 6.2831853f;
                float tumble = .35f + .65f * std::abs(std::sin(time * (1.4f + r) + r * 9.f));
                draw->AddEllipseFilled(at, ImVec2(tall * .18f, tall * .1f * tumble), ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .86f, .92f, .8f * alpha)), spin, 10);
            }
            break;
        }
        case Scene::Space: {
            int stars = std::clamp((int)(wide / (tall * .5f)), 4, 18);
            for (int i = 0; i < stars; ++i) {
                float r = own(10 + i);
                float wave = .5f + .5f * std::sin(time * (1.5f + 3.f * r) + r * 40.f);
                float x = wide * own(20 + i) - time * tall * (.15f + .45f * r);
                x -= std::floor(x / wide) * wide;
                ImVec2 at(body.Min.x + x, body.Min.y + tall * (.12f + .76f * own(30 + i)));
                float size = std::max(.7f, tall * (.025f + .05f * r * r));
                circle_filled_cached(draw, at, size, ImGui::ColorConvertFloat4ToU32(ImVec4(.9f, .93f, 1.f, (.35f + .6f * r) * (.35f + .65f * wave) * alpha)), 6);
                if (r > .9f)
                    sparkle(at, size * (1.5f + 1.f * wave), ImVec4(.9f, .93f, 1.f, .6f * wave * alpha));
            }
            float every = 3.f + 3.f * own(70);
            float clock = time / every + own(71);
            float run = (clock - std::floor(clock)) / .25f;
            if (run < 1.f) {
                ImVec2 head(body.Min.x + wide * (.1f + run * .8f), body.Min.y + tall * (.2f + .5f * run));
                ImVec2 tail(head.x - tall * 1.2f, head.y - tall * .6f);
                float fade = std::sin(3.1415927f * run) * alpha;
                draw->AddLine(tail, head, ImGui::ColorConvertFloat4ToU32(ImVec4(.9f, .95f, 1.f, .25f * fade)), std::max(1.f, tall * .05f));
                circle_filled_cached(draw, head, std::max(.9f, tall * .045f), ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, .9f * fade)), 6);
            }
            break;
        }
        case Scene::Ocean:
            tank(WATER);
            break;
        case Scene::None:
            break;
        }
        draw->PopClipRect();

        ImVec4 edge = look.edge;
        if (which == Scene::Space && g_button_effects) {
            ImGui::ColorConvertHSVtoRGB(.66f + .14f * std::sin(time * .8f + own(5) * 6.2831853f), .6f, 1.f, edge.x, edge.y, edge.z);
            edge = turned(edge, accent_turn);
        }
        draw->AddRect(body.Min, body.Max, ImGui::ColorConvertFloat4ToU32(faded(edge, (.45f + .45f * hot) * alpha)), rounding, 0, 1.f);
    }

    static ImDrawVert mix_vertex(const ImDrawVert& a, const ImDrawVert& b, float t) {
        ImDrawVert out;
        out.pos = ImVec2(a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t);
        out.uv = ImVec2(a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t);
        ImU32 mixed = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            float ca = (float)((a.col >> shift) & 255), cb = (float)((b.col >> shift) & 255);
            mixed |= ((ImU32)std::lround(ca + (cb - ca) * t) & 255) << shift;
        }
        out.col = mixed;
        return out;
    }

    static void clip_to_edge(std::vector<ImDrawVert>& poly, std::vector<ImDrawVert>& next, ImVec2 a, ImVec2 b, float side) {
        auto inside = [&](ImVec2 p) {
            return side * ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) >= 0.f;
        };
        next.clear();
        for (std::size_t k = 0; k < poly.size(); ++k) {
            const ImDrawVert& p = poly[k];
            const ImDrawVert& q = poly[(k + 1) % poly.size()];
            bool p_in = inside(p.pos), q_in = inside(q.pos);
            if (p_in) next.push_back(p);
            if (p_in != q_in) {
                float dp = (b.x - a.x) * (p.pos.y - a.y) - (b.y - a.y) * (p.pos.x - a.x);
                float dq = (b.x - a.x) * (q.pos.y - a.y) - (b.y - a.y) * (q.pos.x - a.x);
                next.push_back(mix_vertex(p, q, dp / (dp - dq)));
            }
        }
        poly.swap(next);
    }

    static void round_off(ImDrawList* draw, int idx_from, int cmd_from, const ImRect& box, float rounding) {
        if (rounding < .5f || draw->CmdBuffer.Size == 0) return;
        draw->PathRect(box.Min, box.Max, rounding);
        std::vector<ImVec2> outline(draw->_Path.begin(), draw->_Path.end());
        draw->PathClear();
        if (outline.size() < 3) return;
        float turn = 0.f;
        for (std::size_t i = 0; i < outline.size(); ++i) {
            ImVec2 a = outline[i], b = outline[(i + 1) % outline.size()];
            turn += (b.x - a.x) * (b.y + a.y);
        }
        float side = turn < 0.f ? 1.f : -1.f;
        auto corner_free_band = [&](ImVec2 p) {
            return (p.x >= box.Min.x + rounding && p.x <= box.Max.x - rounding) ? 1 : (p.y >= box.Min.y + rounding && p.y <= box.Max.y - rounding) ? 2 : 0;
        };
        ImVector<ImDrawIdx> indices;
        indices.reserve(draw->IdxBuffer.Size);
        for (int i = 0; i < idx_from; ++i)
            indices.push_back(draw->IdxBuffer[i]);
        std::vector<ImDrawVert> poly, next;
        for (int c = std::max(cmd_from, 0); c < draw->CmdBuffer.Size; ++c) {
            ImDrawCmd& cmd = draw->CmdBuffer[c];
            unsigned begin = cmd.IdxOffset, end = cmd.IdxOffset + cmd.ElemCount;
            unsigned start = std::max(begin, (unsigned)idx_from);
            unsigned new_offset = begin < (unsigned)idx_from ? begin : (unsigned)indices.Size;
            for (unsigned i = start; i + 3 <= end; i += 3) {
                ImDrawIdx tri[3] = { draw->IdxBuffer[i], draw->IdxBuffer[i + 1], draw->IdxBuffer[i + 2] };
                const ImDrawVert* at[3] = { &draw->VtxBuffer[cmd.VtxOffset + tri[0]], &draw->VtxBuffer[cmd.VtxOffset + tri[1]], &draw->VtxBuffer[cmd.VtxOffset + tri[2]] };
                int band = corner_free_band(at[0]->pos);
                if (band != 0 && corner_free_band(at[1]->pos) == band && corner_free_band(at[2]->pos) == band) {
                    indices.push_back(tri[0]);
                    indices.push_back(tri[1]);
                    indices.push_back(tri[2]);
                    continue;
                }
                poly = { *at[0], *at[1], *at[2] };
                for (std::size_t e = 0; e < outline.size() && !poly.empty(); ++e)
                    clip_to_edge(poly, next, outline[e], outline[(e + 1) % outline.size()], side);
                if (poly.size() < 3) continue;
                // the new points need indices that still fit after this cmds vertex offset
                if (sizeof(ImDrawIdx) == 2 && draw->VtxBuffer.Size - (int)cmd.VtxOffset + (int)poly.size() >= (1 << 16)) {
                    indices.push_back(tri[0]);
                    indices.push_back(tri[1]);
                    indices.push_back(tri[2]);
                    continue;
                }
                auto first = (ImDrawIdx)(draw->VtxBuffer.Size - (int)cmd.VtxOffset);
                for (const auto& vert : poly)
                    draw->VtxBuffer.push_back(vert);
                for (std::size_t k = 1; k + 1 < poly.size(); ++k) {
                    indices.push_back(first);
                    indices.push_back((ImDrawIdx)(first + k));
                    indices.push_back((ImDrawIdx)(first + k + 1));
                }
            }
            cmd.IdxOffset = new_offset;
            cmd.ElemCount = (unsigned)indices.Size - new_offset;
        }
        draw->IdxBuffer.swap(indices);
        draw->_VtxWritePtr = draw->VtxBuffer.Data + draw->VtxBuffer.Size;
        draw->_IdxWritePtr = draw->IdxBuffer.Data + draw->IdxBuffer.Size;
        draw->_VtxCurrentIdx = (unsigned)draw->VtxBuffer.Size - draw->CmdBuffer.back().VtxOffset;
    }

    static void drawScene(ImGuiWindow* window, float alpha) {
        Backdrop back;
        back.draw = ImGui::GetBackgroundDrawList();
        back.a = window->Pos;
        back.b = ImVec2(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
        back.screen = ImGui::GetIO().DisplaySize;
        back.width = window->Size.x;
        back.tall = std::max(window->Size.y, 1.f);
        back.rounding = window->WindowRounding;
        back.time = (float)(ImGui::GetTime());
        back.unit = em(1.f);
        back.alpha = alpha;
        back.id = window->ID;
        int from = back.draw->VtxBuffer.Size;
        int idx_from = back.draw->IdxBuffer.Size;
        int cmd_from = back.draw->CmdBuffer.Size - 1;
        switch (g_scene) {
        case Scene::Halloween:
            halloween_scene(back);
            break;
        case Scene::Hell:
            hell_scene(back);
            break;
        case Scene::Heaven:
            heaven_scene(back);
            break;
        case Scene::Space:
            space_scene(back);
            break;
        case Scene::Ocean:
            ocean_scene(back);
            break;
        case Scene::Sakura:
            sakura_scene(back);
            break;
        case Scene::None:
            break;
        }
        turn_verts(back.draw, from, body_turn);
        round_off(back.draw, idx_from, cmd_from, ImRect(back.a, back.b), back.rounding);
    }

    void WindowRims(float alpha) {
        if (alpha <= .01f) return;
        ImGuiContext& g = *GImGui;
        for (ImGuiWindow* window : g.Windows) {
            if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_ChildWindow))
                continue;
            if (window->Size.x < 4.f || window->Size.y < 4.f || window->IsFallbackWindow) continue;
            if (window->Flags & ImGuiWindowFlags_NoBackground) continue;
            bool plain = (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0;
            if (g_rainbow_rim && !plain) rainbow_body(window, alpha);
            if (g_scene != Scene::None && !plain) drawScene(window, alpha);
            rim(window, alpha);
        }
    }

    struct FieldState {
        bool hovered = false;
        bool active = false;
    };

    static std::unordered_map<ImGuiID, FieldState> g_field_states;

    struct FocusedField {
        ImGuiID id = 0;
        ImRect bb;
        ImGuiWindow* window = nullptr;
    };

    static FocusedField g_field;

    static void draw_field(ImGuiWindow* window, ImGuiID id, const ImRect& bb, float fill, const FieldState& state) {
        const ImGuiStyle& style = ImGui::GetStyle();
        float hot = ease(sub(id, "field-hot"), state.hovered ? 1.f : 0.f);
        float act = ease(sub(id, "field-act"), state.active ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        float rounding = style.FrameRounding;
        draw->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(mix(color(ImGuiCol_FrameBg), color(ImGuiCol_FrameBgHovered), hot)), rounding);
        gloss(draw, bb, rounding);
        if (fill > .004f) {
            float edge = bb.Min.x + bb.GetWidth() * std::min(fill, 1.f);
            draw->PushClipRect(bb.Min, ImVec2(edge, bb.Max.y), true);
            draw->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), .16f + .08f * hot)), rounding);
            draw->PopClipRect();
        }
        ImVec4 edge = mix(faded(color(ImGuiCol_Border), .55f), color(ImGuiCol_CheckMark), std::max(hot * .5f, act));
        draw->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(edge), rounding, 0, 1.f);
        if (act > .01f) {
            float half = bb.GetWidth() * .32f * act;
            float mid = (bb.Min.x + bb.Max.x) * .5f;
            float thick = std::max(2.f, ImGui::GetFontSize() * .11f);
            draw->AddRectFilled(ImVec2(mid - half, bb.Max.y - thick), ImVec2(mid + half, bb.Max.y), ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), act)), thick * .5f);
        }
    }

    static void push_clear_frame() {
        ImVec4 none(0.f, 0.f, 0.f, 0.f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, none);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, none);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, none);
    }

    static float range_fill(double value, double min, double max) {
        if (!(min < max) || max - min > 1e7) return -1.f;
        return (float)(std::clamp((value - min) / (max - min), 0.0, 1.0));
    }

    static void stroke_icon(ImDrawList* draw, ImVec2 c, float s, Icon icon, ImU32 ink) {
        float t = std::max(1.5f, s * .12f);
        auto at = [&](float x, float y) {
            return ImVec2(c.x + x * s, c.y + y * s);
        };
        switch (icon) {
        case Icon::Play:
            // clockwise or the smooth edge breaks fsr
            draw->AddTriangleFilled(at(-.3f, -.4f), at(.44f, 0.f), at(-.3f, .4f), ink);
            break;
        case Icon::Record:
            circle_filled_cached(draw, c, s * .34f, ink, 24);
            break;
        case Icon::Stop:
            draw->AddRectFilled(at(-.3f, -.3f), at(.3f, .3f), ink, s * .1f);
            break;
        case Icon::Macro: {
            ImVec2 outline[] = { at(-.38f, .3f), at(-.38f, -.3f), at(-.08f, -.3f), at(.04f, -.18f), at(.38f, -.18f), at(.38f, .3f) };
            draw->AddPolyline(outline, 6, ink, ImDrawFlags_Closed, t);
            break;
        }
        case Icon::Editor: {
            float knobs[] = { -.2f, .2f, -.2f };
            for (int i = 0; i < 3; ++i) {
                float y = ((float)(i) - 1.f) * .28f;
                draw->AddLine(at(-.44f, y), at(.44f, y), ink, t * .8f);
                circle_filled_cached(draw, at(knobs[i], y), s * .11f, ink, 12);
            }
            break;
        }
        case Icon::Tools: {
            const float WIDTH = .15f, HEAD_SIZE = .21f, HEAD_AT = .2f, JAW = .06f, TIP = .16f, CAP_AT = -.38f;
            const float PI_F = 3.14159265f;
            ImVec2 half[40];
            int m = 0;
            for (int i = 0; i <= 6; ++i) {
                float a = PI_F + PI_F * .5f * (float)(i) / 6.f;
                half[m++] = ImVec2(CAP_AT + WIDTH * .5f * std::cos(a), WIDTH * .5f * std::sin(a));
            }
            float neck = HEAD_AT - std::sqrt(HEAD_SIZE * HEAD_SIZE - WIDTH * WIDTH * .25f);
            float from = std::atan2(-WIDTH * .5f, neck - HEAD_AT), to = -std::acos(TIP / HEAD_SIZE);
            if (from > to) from -= 2.f * PI_F;
            for (int i = 1; i <= 24; ++i) {
                float a = from + (to - from) * (float)(i) / 24.f;
                half[m++] = ImVec2(HEAD_AT + HEAD_SIZE * std::cos(a), HEAD_SIZE * std::sin(a));
            }
            half[m++] = ImVec2(HEAD_AT + TIP, -JAW);
            half[m++] = ImVec2(HEAD_AT, -JAW);
            ImVec2 shape[80];
            int n = 0;
            for (int i = 0; i < m; ++i)
                shape[n++] = half[i];
            for (int i = m - 1; i >= 1; --i)
                shape[n++] = ImVec2(half[i].x, -half[i].y);
            float mid = (CAP_AT - WIDTH * .5f + HEAD_AT + TIP) * .5f;
            for (int i = 0; i < n; ++i) {
                float u = shape[i].x - mid, v = shape[i].y;
                shape[i] = at((u + v) * .7071068f * 1.15f + .03f, (v - u) * .7071068f * 1.15f);
            }
            draw->AddConcavePolyFilled(shape, n, ink);
            break;
        }
        case Icon::Gear: {
            ImVec2 rim[32];
            const float STEP = 6.2831853f / 8.f;
            for (int i = 0; i < 8; ++i) {
                float a = (float)(i) * STEP;
                float turns[] = { a - STEP * .3f, a - STEP * .17f, a + STEP * .17f, a + STEP * .3f };
                float reach[] = { .27f, .39f, .39f, .27f };
                for (int k = 0; k < 4; ++k)
                    rim[i * 4 + k] = ImVec2(c.x + std::cos(turns[k]) * s * reach[k], c.y + std::sin(turns[k]) * s * reach[k]);
            }
            draw->AddPolyline(rim, 32, ink, ImDrawFlags_Closed, t * .85f);
            draw->AddCircle(c, s * .13f, ink, 16, t * .85f);
            break;
        }
        case Icon::Visuals:
            draw->AddEllipse(c, ImVec2(s * .38f, s * .24f), ink, 0.f, 24, t);
            circle_filled_cached(draw, c, s * .12f, ink, 12);
            break;
        case Icon::Sequence:
            for (int i = 0; i < 3; ++i) {
                float y = ((float)(i) - 1.f) * .28f;
                circle_filled_cached(draw, at(-.36f, y), s * .07f, ink, 10);
                draw->AddLine(at(-.16f, y), at(.42f, y), ink, t * .8f);
            }
            break;
        case Icon::Clickbot: {
            ImVec2 arrow[] = { at(-.3f, -.44f), at(.32f, .1f), at(.05f, .12f), at(.2f, .42f), at(.07f, .48f), at(-.08f, .18f), at(-.3f, .36f) };
            draw->AddConcavePolyFilled(arrow, 7, ink);
            break;
        }
        case Icon::Render:
            draw->AddRect(at(-.38f, -.26f), at(.12f, .26f), ink, s * .08f, 0, t);
            draw->AddTriangleFilled(at(.2f, 0.f), at(.44f, -.24f), at(.44f, .24f), ink);
            break;
        case Icon::Theme:
            draw->AddCircle(c, s * .36f, ink, 24, t);
            draw->PathArcTo(c, s * .36f, -1.5707963f, 1.5707963f, 16);
            draw->PathFillConvex(ink);
            break;
        case Icon::None:
            break;
        }
    }

    static ImVec4 ink_on(const ImVec4& fill) {
        float lum = fill.x * .2126f + fill.y * .7152f + fill.z * .0722f;
        if (lum > .55f) {
            ImVec4 dark = color(ImGuiCol_WindowBg);
            float body_lum = dark.x * .2126f + dark.y * .7152f + dark.z * .0722f;
            if (body_lum > .12f) dark = ImVec4(.08f, .08f, .08f, 1.f);
            dark.w = 1.f;
            return dark;
        }
        ImVec4 text = color(ImGuiCol_Text);
        float text_lum = text.x * .2126f + text.y * .7152f + text.z * .0722f;
        return text_lum > .5f ? text : ImVec4(1.f, 1.f, 1.f, 1.f);
    }

    void begin_field(const char* label, float fill, int parts, float trim) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImGuiID id = window->GetID(label);
        ImVec2 pos = window->DC.CursorPos;
        float width = std::max(1.f, ImGui::CalcItemWidth() - trim);
        ImRect bb(pos, ImVec2(pos.x + width, pos.y + ImGui::GetFrameHeight()));
        g_field = { id, bb, window };
        if (!window->SkipItems && ImGui::IsRectVisible(bb.Min, bb.Max)) {
            if (parts <= 1) {
                draw_field(window, id, bb, fill, g_field_states[id]);
            }
            else {
                float gap = ImGui::GetStyle().ItemInnerSpacing.x;
                float each = std::max(1.f, std::trunc((width - gap * (float)(parts - 1)) / (float)(parts)));
                float last = std::max(1.f, std::trunc(width - (each + gap) * (float)(parts - 1)));
                float x = pos.x;
                for (int i = 0; i < parts; ++i) {
                    float w = i + 1 == parts ? last : each;
                    draw_field(window, sub(id, i == 0 ? "part0" : i == 1 ? "part1"
                                                              : i == 2   ? "part2"
                                                                         : "part3"),
                        ImRect(ImVec2(x, pos.y), ImVec2(x + w, bb.Max.y)), -1.f, g_field_states[id]);
                    x += w + gap;
                }
            }
        }
        push_clear_frame();
    }

    void end_field() {
        ImGui::PopStyleColor(3);
        ImGuiContext& g = *GImGui;
        auto& state = g_field_states[g_field.id];
        bool over = g.HoveredWindow == g_field.window && g_field.bb.Contains(g.IO.MousePos);
        state.hovered = ImGui::IsItemHovered() || (over && g.ActiveId == 0);
        state.active = ImGui::IsItemActive() || (g.ActiveId != 0 && g.ActiveIdWindow == g_field.window && g_field.bb.Contains(g.IO.MouseClickedPos[0]) && g.IO.MouseDown[0]);
    }

    bool DragFloat(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags) {
        return field(label, [&] {
            return ImGui::DragFloat(label, value, speed, min, max, format, flags);
        },
            range_fill(*value, min, max));
    }

    bool DragInt(const char* label, int* value, float speed, int min, int max, const char* format, ImGuiSliderFlags flags) {
        return field(label, [&] {
            return ImGui::DragInt(label, value, speed, min, max, format, flags);
        },
            range_fill(*value, min, max));
    }

    bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImGuiID id = window->GetID(label);
        float side = ImGui::GetFrameHeight();
        ImVec2 pos = window->DC.CursorPos;
        ImRect bb(pos, ImVec2(pos.x + ((flags & ImGuiComboFlags_NoPreview) ? side : ImGui::CalcItemWidth()), pos.y + side));
        auto& state = g_field_states[id];
        float turned = ease(sub(id, "combo-open"), state.active ? 1.f : 0.f, 16.f);
        if (!window->SkipItems && ImGui::IsRectVisible(bb.Min, bb.Max)) {
            draw_field(window, id, bb, -1.f, state);
            ImVec2 mid(bb.Max.x - side * .5f, (bb.Min.y + bb.Max.y) * .5f);
            float arm = side * .16f;
            float flip = 1.f - 2.f * turned;
            ImVec2 points[] = { ImVec2(mid.x - arm, mid.y - arm * .55f * flip), ImVec2(mid.x, mid.y + arm * .55f * flip), ImVec2(mid.x + arm, mid.y - arm * .55f * flip) };
            window->DrawList->AddPolyline(points, 3, ImGui::GetColorU32(mix(color(ImGuiCol_TextDisabled), color(ImGuiCol_CheckMark), std::max(turned, state.hovered ? .6f : 0.f))), 0, std::max(1.5f, side * .07f));
        }
        ImGuiContext& g = *GImGui;
        bool over = g.HoveredWindow == window && bb.Contains(g.IO.MousePos);
        push_clear_frame();
        bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
        ImGui::PopStyleColor(3);
        state.hovered = over && !open;
        state.active = open;
        return open;
    }

    bool Combo(const char* label, int* current, const char* const items[], int count) {
        const char* shown = *current >= 0 && *current < count ? items[*current] : "";
        if (!BeginCombo(label, shown)) return false;
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (Selectable(items[i], i == *current)) {
                *current = i;
                changed = true;
            }
            if (i == *current) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
        return changed;
    }

    bool Combo(const char* label, int* current, const char* zero_separated) {
        ImVector<const char*> items;
        for (const char* at = zero_separated; *at; at += strlen(at) + 1)
            items.push_back(at);
        return Combo(label, current, items.Data, items.Size);
    }

    bool Selectable(const char* label, bool selected, ImGuiSelectableFlags flags, const ImVec2& size) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        static ImDrawListSplitter layers;
        ImDrawList* draw = window->DrawList;
        layers.Split(draw, 2);
        layers.SetCurrentChannel(draw, 1);
        ImVec4 none(0.f, 0.f, 0.f, 0.f);
        ImGui::PushStyleColor(ImGuiCol_Header, none);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, none);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, none);
        bool pressed = ImGui::Selectable(label, selected, flags, size);
        ImGui::PopStyleColor(3);
        ImGuiID id = ImGui::GetItemID();
        ImRect row(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        float hot = ease(sub(id, "row-hot"), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);
        float on = ease(sub(id, "row-on"), selected ? 1.f : 0.f, 18.f);
        layers.SetCurrentChannel(draw, 0);
        float strength = std::max(hot * .75f, on);
        if (strength > .01f) {
            float edge = ImGui::GetFontSize() * .38f;
            float right = window->Pos.x + window->Size.x - edge - (window->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.f);
            row.Min.x = std::max(row.Min.x, window->Pos.x + edge);
            row.Max.x = std::min(row.Max.x, right);
            row.Min.y += 1.f;
            row.Max.y -= 1.f;
            float rounding = std::min(ImGui::GetStyle().FrameRounding, row.GetHeight() * .4f);
            draw->AddRectFilled(row.Min, row.Max, ImGui::GetColorU32(faded(mix(color(ImGuiCol_HeaderHovered), color(ImGuiCol_Header), on), strength)), rounding);
        }
        layers.Merge(draw);
        return pressed;
    }

    bool ActionButton(const char* label, Icon icon, bool active, const ImVec2& size_arg, ImVec4 tint) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        ImGuiID id = window->GetID(label);
        ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImGui::CalcItemSize(size_arg, label_size.x + style.FramePadding.x * 2.f, label_size.y + style.FramePadding.y * 2.f);
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(size, style.FramePadding.y);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

        float hot = ease(id, hovered ? 1.f : 0.f);
        float down = ease(sub(id, "down"), held && hovered ? 1.f : 0.f, 22.f);
        float on = ease(sub(id, "on"), active ? 1.f : 0.f, 12.f);
        ImVec4 accent = tint.w > 0.f ? tint : color(ImGuiCol_CheckMark);
        ImDrawList* draw = window->DrawList;
        ImRect body = bb;
        body.Expand(-down * std::max(1.f, size.y * .04f));
        ImVec4 idle = mix(mix(color(ImGuiCol_Button), color(ImGuiCol_ButtonHovered), hot), color(ImGuiCol_ButtonActive), down);
        ImVec4 running = mix(accent, ImVec4(1.f, 1.f, 1.f, accent.w), hot * .12f);
        ImVec4 fill = mix(idle, running, on);
        draw->AddRectFilled(body.Min, body.Max, ImGui::GetColorU32(fill), style.FrameRounding);
        gloss(draw, body, style.FrameRounding);
        button_skin(draw, body, style.FrameRounding, id, hot, down, 1.f, &running, on);
        draw->AddRect(body.Min, body.Max, ImGui::GetColorU32(faded(accent, std::max(hot * .55f, .35f * (1.f - on)))), style.FrameRounding, 0, 1.f);
        ImVec4 ink = mix(color(ImGuiCol_Text), ink_on(running), on);
        float glyph = ImGui::GetFontSize() * .78f;
        float gap = style.ItemInnerSpacing.x * .8f;
        float group = glyph + gap + label_size.x;
        float left = bb.Min.x + std::max(style.FramePadding.x * .6f, (size.x - group) * .5f);
        ImVec2 center(left + glyph * .5f, (bb.Min.y + bb.Max.y) * .5f);
        ImVec4 glyph_ink = mix(accent, ink, on);
        stroke_icon(draw, center, glyph, active ? Icon::Stop : icon, ImGui::GetColorU32(glyph_ink));
        ImGui::PushStyleColor(ImGuiCol_Text, ink);
        ImRect text_box(ImVec2(left + glyph + gap, bb.Min.y), ImVec2(bb.Max.x - style.FramePadding.x * .5f, bb.Max.y));
        ImGui::RenderTextClipped(ImVec2(text_box.Min.x, bb.Min.y + style.FramePadding.y), ImVec2(text_box.Max.x, bb.Max.y - style.FramePadding.y), label, nullptr, &label_size, ImVec2(0.f, .5f), &text_box);
        ImGui::PopStyleColor();
        return pressed;
    }

    float nav_pad() {
        return ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * .3f;
    }

    float nav_gap() {
        return ImGui::GetFontSize() * .45f;
    }

    float nav_icon_width() {
        return nav_pad() * 2.f + ImGui::GetFontSize() + nav_gap();
    }

    bool NavItem(const char* label, Icon icon, bool selected, float width, float height) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGuiID id = window->GetID(label);
        ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size(width + nav_icon_width(), height > 0.f ? height : ImGui::GetFrameHeight());
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(size, style.FramePadding.y);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        float hot = ease(id, hovered ? 1.f : 0.f);
        float on = ease(sub(id, "on"), selected ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        float plate = std::max(hot * .7f, on);
        if (plate > .01f)
            draw->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(faded(mix(color(ImGuiCol_FrameBgHovered), color(ImGuiCol_Header), on), plate)), style.FrameRounding);
        float glyph = ImGui::GetFontSize();
        float x = bb.Min.x + nav_pad();
        ImVec4 ink = mix(color(ImGuiCol_TextDisabled), color(ImGuiCol_Text), std::max(hot, on));
        stroke_icon(draw, ImVec2(x + glyph * .5f, (bb.Min.y + bb.Max.y) * .5f), glyph, icon, ImGui::GetColorU32(mix(ink, color(ImGuiCol_CheckMark), on)));
        ImGui::PushStyleColor(ImGuiCol_Text, ink);
        ImVec2 text(std::round(x + glyph + nav_gap()), std::round((bb.Min.y + bb.Max.y - label_size.y) * .5f));
        ImGui::RenderText(text, label);
        ImGui::PopStyleColor();
        return pressed;
    }

    void Separator() {
        ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImU32 line = ImGui::GetColorU32(ImGuiCol_Separator);
        ImU32 clear = ImGui::GetColorU32(faded(color(ImGuiCol_Separator), 0.f));
        if (b.x - a.x >= b.y - a.y) {
            float y = std::floor((a.y + b.y) * .5f);
            float mid = (a.x + b.x) * .5f;
            draw->AddRectFilledMultiColor(ImVec2(a.x, y), ImVec2(mid, y + 1.f), clear, line, line, clear);
            draw->AddRectFilledMultiColor(ImVec2(mid, y), ImVec2(b.x, y + 1.f), line, clear, clear, line);
        }
        else {
            float x = std::floor((a.x + b.x) * .5f);
            float mid = (a.y + b.y) * .5f;
            draw->AddRectFilledMultiColor(ImVec2(x, a.y), ImVec2(x + 1.f, mid), clear, clear, line, line);
            draw->AddRectFilledMultiColor(ImVec2(x, mid), ImVec2(x + 1.f, b.y), line, line, clear, clear);
        }
    }

    void SectionLabel(const char* label) {
        ImGui::Dummy(ImVec2(0.f, 6.f));
        float font = ImGui::GetFontSize();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        float tick = std::max(2.f, font * .16f);
        draw->AddRectFilled(ImVec2(pos.x, pos.y + font * .18f), ImVec2(pos.x + tick, pos.y + font * .86f), ImGui::GetColorU32(ImGuiCol_CheckMark), tick * .5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tick + font * .38f);
        ImGui::PushStyleColor(ImGuiCol_Text, color(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 text = ImGui::GetItemRectSize();
        float y = std::floor(min.y + text.y * .55f);
        float from = min.x + text.x + font * .6f, to = from + font * 7.f;
        draw->AddRectFilledMultiColor(ImVec2(from, y), ImVec2(to, y + 1.f), ImGui::GetColorU32(ImGuiCol_Separator), ImGui::GetColorU32(faded(color(ImGuiCol_Separator), 0.f)),
            ImGui::GetColorU32(faded(color(ImGuiCol_Separator), 0.f)), ImGui::GetColorU32(ImGuiCol_Separator));
    }

    void Help() {
        ImVec2 size = ImGui::CalcTextSize("(?)");
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImGuiID id = ImGui::GetCurrentWindow()->GetID((int)(pos.x * 31.f + pos.y));
        float hot = ease(id, ImGui::IsItemHovered() ? 1.f : 0.f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 mid(pos.x + size.x * .5f, pos.y + size.y * .5f);
        float radius = size.y * .42f;
        ImVec4 ink = mix(color(ImGuiCol_TextDisabled), color(ImGuiCol_CheckMark), hot);
        draw->AddCircle(mid, radius, ImGui::GetColorU32(ink), 20, std::max(1.f, size.y * .07f));
        float mark_size = ImGui::GetFontSize() * .72f;
        ImVec2 mark = ImGui::GetFont()->CalcTextSizeA(mark_size, FLT_MAX, 0.f, "?");
        draw->AddText(ImGui::GetFont(), mark_size, ImVec2(mid.x - mark.x * .5f, mid.y - mark.y * .5f), ImGui::GetColorU32(ink), "?");
    }

    void ProgressBar(float fraction, const ImVec2& size_arg, const char* overlay) {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImVec2 size = ImGui::CalcItemSize(size_arg, ImGui::CalcItemWidth(), ImGui::GetFrameHeight());
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 max(pos.x + size.x, pos.y + size.y);
        float rounding = std::min(style.FrameRounding, size.y * .5f);
        draw->AddRectFilled(pos, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        ImU32 fill = ImGui::GetColorU32(mix(color(ImGuiCol_FrameBg), color(ImGuiCol_CheckMark), .6f));
        float from = 0.f, to = std::clamp(fraction, 0.f, 1.f);
        if (fraction < 0.f) {
            float sweep = .5f + .5f * std::sin((float)(ImGui::GetTime()) * 2.2f);
            from = sweep * .7f;
            to = from + .3f;
        }
        if (to > from + .002f) {
            draw->PushClipRect(ImVec2(pos.x + size.x * from, pos.y), ImVec2(pos.x + size.x * to, max.y), true);
            draw->AddRectFilled(pos, max, fill, rounding);
            draw->PopClipRect();
        }
        draw->AddRect(pos, max, ImGui::GetColorU32(faded(color(ImGuiCol_Border), .55f)), rounding);
        if (overlay && *overlay) {
            ImVec2 text = ImGui::CalcTextSize(overlay);
            ImRect box(pos, max);
            ImGui::RenderTextClipped(pos, max, overlay, nullptr, &text, ImVec2(.5f, .5f), &box);
        }
    }

    void StatusDot(const ImVec4& tint) {
        float font = ImGui::GetFontSize();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        pos.y += ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
        ImGui::Dummy(ImVec2(font * .62f, font));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImVec2 mid(pos.x + font * .31f, pos.y + font * .52f);
        ImVec4 shown = tint;
        shown.w *= ImGui::GetStyle().Alpha;
        circle_filled_cached(draw, mid, font * .32f, ImGui::ColorConvertFloat4ToU32(faded(shown, .22f)), 20);
        circle_filled_cached(draw, mid, font * .19f, ImGui::ColorConvertFloat4ToU32(shown), 16);
    }

    float em(float n) {
        return ImGui::GetFontSize() * n;
    }

    static ImVector<float> g_row_widths;

    static float row_width() {
        return g_row_widths.empty() ? ImGui::GetContentRegionAvail().x : g_row_widths.back();
    }

    static float control_start() {
        return ImGui::GetStyle().WindowPadding.x + (row_width() + ImGui::GetStyle().ItemSpacing.x) * .5f;
    }

    static bool close_button(ImGuiWindow* window, const ImRect& bb, const char* id_text) {
        ImGuiID id = window->GetID(id_text);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        float hot = ease(id, hovered ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        ImVec2 mid = bb.GetCenter();
        float side = bb.GetWidth();
        circle_filled_cached(draw, mid, side * .5f, ImGui::GetColorU32(faded(color(ImGuiCol_FrameBgHovered), hot)), 24);
        float arm = side * .2f;
        ImU32 ink = ImGui::GetColorU32(mix(color(ImGuiCol_TextDisabled), color(ImGuiCol_Text), hot));
        float thick = std::max(1.5f, side * .09f);
        draw->AddLine(ImVec2(mid.x - arm, mid.y - arm), ImVec2(mid.x + arm, mid.y + arm), ink, thick);
        draw->AddLine(ImVec2(mid.x - arm, mid.y + arm), ImVec2(mid.x + arm, mid.y - arm), ink, thick);
        return pressed;
    }

    bool TitleBar(const char* title, Icon icon, bool closable, float width) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        const ImGuiStyle& style = ImGui::GetStyle();
        float height = ImGui::GetFrameHeight();
        float start_y = ImGui::GetCursorPosY();
        ImVec2 screen = ImGui::GetCursorScreenPos();
        float x = 0.f;
        if (icon != Icon::None) {
            float glyph = ImGui::GetFontSize() * .86f;
            stroke_icon(window->DrawList, ImVec2(screen.x + glyph * .5f, screen.y + height * .5f), glyph, icon, ImGui::GetColorU32(ImGuiCol_CheckMark));
            x = glyph + style.ItemInnerSpacing.x;
        }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + x);
        ImGui::SetWindowFontScale(1.12f);
        ImGui::SetCursorPosY(start_y + (height - ImGui::GetFontSize()) * .5f);
        const char* end = ImGui::FindRenderedTextEnd(title);
        ImGui::TextUnformatted(title, end);
        ImGui::SetWindowFontScale(1.f);
        bool pressed = false;
        if (closable) {
            float side = height * .9f;
            float right = width > 0.f ? window->Pos.x + style.WindowPadding.x + width : window->Pos.x + window->Size.x - style.WindowPadding.x;
            ImRect bb(ImVec2(right - side, screen.y + (height - side) * .5f), ImVec2(right, screen.y + (height + side) * .5f));
            pressed = close_button(window, bb, "##echo-title-close");
        }
        ImGui::SetCursorPosY(start_y + height + style.ItemSpacing.y * .6f);
        return pressed;
    }

    struct CardRow {
        ImGuiID id = 0;
        float tallest = 0.f;
        int cards = 0;
    };

    static ImVector<CardRow> g_card_rows;

    static std::unordered_map<ImGuiID, float> g_row_heights;

    static float g_card_needs = -1.f;

    bool CollapseBar(const char* title, Icon icon, bool open) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGuiID id = window->GetID("##echo-collapse");
        float height = ImGui::GetFrameHeight();
        ImVec2 pos = window->DC.CursorPos;
        float size = ImGui::GetFontSize() * 1.12f;
        float glyph = em(.86f);
        float need = glyph + style.ItemInnerSpacing.x + ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, title).x + height * 1.3f;
        float width = std::max(need, window->Size.x - style.WindowPadding.x * 2.f);
        ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
        ImGui::ItemSize(ImVec2(need, height), 0.f);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        float hot = ease(id, hovered ? 1.f : 0.f);
        float turned = ease(sub(id, "open"), open ? 1.f : 0.f, 16.f);
        ImDrawList* draw = window->DrawList;
        ImVec4 ink = mix(color(ImGuiCol_Text), color(ImGuiCol_CheckMark), hot * .6f);
        if (icon != Icon::None)
            stroke_icon(draw, ImVec2(pos.x + glyph * .5f, pos.y + height * .5f), glyph, icon, ImGui::GetColorU32(ImGuiCol_CheckMark));
        draw->AddText(ImGui::GetFont(), size, ImVec2(pos.x + glyph + style.ItemInnerSpacing.x, pos.y + (height - size) * .5f), ImGui::GetColorU32(ink), title);
        ImVec2 mid(bb.Max.x - height * .45f, pos.y + height * .5f);
        float arm = height * .17f;
        float angle = (1.f - turned) * -1.5707963f;
        float ca = std::cos(angle);
        float sa = std::sin(angle);
        auto spun = [&](float x, float y) {
            return ImVec2(mid.x + x * ca - y * sa, mid.y + x * sa + y * ca);
        };
        ImVec2 points[] = { spun(-arm, -arm * .55f), spun(0.f, arm * .55f), spun(arm, -arm * .55f) };
        draw->AddPolyline(points, 3, ImGui::GetColorU32(mix(color(ImGuiCol_TextDisabled), color(ImGuiCol_CheckMark), hot)), 0, std::max(1.5f, height * .07f));
        return pressed;
    }

    void BeginCardRow(const char* id) {
        CardRow row;
        row.id = ImGui::GetID(id);
        g_card_rows.push_back(row);
    }

    static ImGuiID g_last_card_row = 0;

    static std::unordered_map<ImGuiID, float> g_row_extras;

    void EndCardRow() {
        g_row_heights[g_card_rows.back().id] = g_card_rows.back().tallest;
        g_last_card_row = g_card_rows.back().id;
        g_card_rows.pop_back();
    }

    ImGuiID& last_card_row() {
        return g_last_card_row;
    }

    float& card_row_extra(ImGuiID row) {
        return g_row_extras[row];
    }

    float CardFill(float min_em) {
        float least = em(min_em);
        g_card_needs = ImGui::GetCursorPosY() + least + ImGui::GetStyle().WindowPadding.y;
        return std::max(least, ImGui::GetContentRegionAvail().y);
    }

    bool g_single_column = false;

    void ScrollMark() {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->ScrollMax.y <= 0.f) return;
        ImRect box = window->Rect();
        float tall = box.GetHeight();
        float thumb = std::max(em(1.2f), tall * tall / (tall + window->ScrollMax.y));
        float at = box.Min.y + (tall - thumb) * (window->Scroll.y / window->ScrollMax.y);
        float wide = std::max(2.f, em(.14f));
        float x = box.Max.x - wide;
        ImDrawList* draw = window->DrawList;
        draw->PushClipRect(box.Min, box.Max, false);
        draw->AddRectFilled(ImVec2(x, at), ImVec2(x + wide, at + thumb), ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), .7f)), wide * .5f);
        draw->PopClipRect();
    }

    static bool close_button(ImGuiWindow* window, const ImRect& bb, const char* id_text);

    struct TileLayout {
        bool on = false;
        float left = 0.f, top = 0.f;
        float column = 0.f;
        float gap = 0.f;
        std::vector<float> below;
        std::string group;
        int first = 0;
        int span = 1;
        float y = 0.f;
        float bottom = 0.f;
        float scroll = 0.f;
        TileState* open = nullptr;
        bool skipped = false;
        int frame = 0;
        bool dragged = false;
        float shown = 1.f;
        float rate = 1.f;
        int born = 0;
    };

    static TileLayout g_tiles;

    static std::unordered_map<ImGuiID, float> g_tile_widths;

    static void card_heading(const char* title) {
        const char* end = ImGui::FindRenderedTextEnd(title);
        if (end == title) return;
        float font = ImGui::GetFontSize();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float tick = std::max(2.f, font * .16f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, pos.y + font * .16f), ImVec2(pos.x + tick, pos.y + font * .88f), ImGui::GetColorU32(ImGuiCol_CheckMark), tick * .5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tick + font * .4f);
        ImGui::PushStyleColor(ImGuiCol_Text, color(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(title, end);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.f, font * .05f));
    }

    static bool begin_tile(const char* title, float width_em) {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGuiContext& g = *GImGui;
        std::string key = g_tiles.group + "/" + title;
        TileState& state = g_tile_states[key];
        state.seen = g_tiles.frame;
        g_tiles.open = &state;
        g_tiles.skipped = state.hidden;
        if (state.hidden) {
            g_row_widths.push_back(0.f);
            return false;
        }
        std::string name = std::string(title) + "###echo-tile-" + g_tiles.group + "-" + title;
        ImGuiID id = ImHashStr(name.c_str());
        int columns = (int)(g_tiles.below.size());
        float wanted = width_em > 0.f ? em(width_em) + style.WindowPadding.x * 2.f : g_tile_widths[id];
        int span = std::clamp((int)(std::ceil((wanted + g_tiles.gap - 1.f) / (g_tiles.column + g_tiles.gap))), 1, columns);
        float width = g_tiles.column * (float)(span) + g_tiles.gap * (float)(span - 1);
        ImVec2 display = g.IO.DisplaySize;
        if (state.drawn < g_tiles.frame - 1) {
            static std::mt19937 rng{ std::random_device{}() };
            state.from = std::uniform_int_distribution<int>(0, 3)(rng);
            state.flown = -.07f * (float)(std::min(g_tiles.born++, 8));
        }
        state.drawn = g_tiles.frame;
        state.flown = std::min(1.f, state.flown + g.IO.DeltaTime * 1.7f * g_tiles.rate);
        float along = std::clamp(std::min(state.flown, g_tiles.shown), 0.f, 1.f);
        bool leaving = g_tiles.shown < state.flown - .02f;
        float landed = leaving ? 1.f - std::pow(1.f - along, 3.f) : glide_in(along);
        auto flying = [&](ImVec2 at) {
            if (along >= 1.f) return at;
            float pad = em(1.5f);
            float tall = state.height > 0.f ? state.height : display.y;
            ImVec2 away = at;
            switch (state.from) {
            case 0:
                away.x = -width - pad;
                break;
            case 1:
                away.x = display.x + pad;
                break;
            case 2:
                away.y = -tall - pad;
                break;
            default:
                away.y = display.y + pad;
                break;
            }
            return ImVec2(std::lerp(away.x, at.x, landed), std::lerp(away.y, at.y, landed));
        };
        if (state.pinned) {
            ImVec2 at(state.x * display.x, state.y * display.y);
            at.x = std::clamp(at.x, 0.f, std::max(0.f, display.x - width));
            at.y = std::clamp(at.y, 0.f, std::max(0.f, display.y - em(2.f)));
            ImGui::SetNextWindowPos(flying(at), ImGuiCond_Always);
        }
        else {
            float tall = state.height > 0.f ? state.height : em(8.f);
            int best = 0;
            float best_y = FLT_MAX;
            for (int c = 0; c + span <= columns; ++c) {
                float y = 0.f;
                for (int k = 0; k < span; ++k)
                    y = std::max(y, g_tiles.below[(size_t)(c + k)]);
                float x = g_tiles.left + (float)(c) * (g_tiles.column + g_tiles.gap);
                for (int pass = 0; pass < 8; ++pass) {
                    bool moved = false;
                    for (const auto& [other_key, other] : g_tile_states) {
                        if (!other.pinned || other.hidden || other.seen < g_tiles.frame - 1 || other.height <= 0.f)
                            continue;
                        float ox = other.x * display.x;
                        float oy = other.y * display.y + g_tiles.scroll;
                        if (x < ox + other.width + g_tiles.gap && x + width + g_tiles.gap > ox && y < oy + other.height + g_tiles.gap && y + tall + g_tiles.gap > oy) {
                            y = oy + other.height + g_tiles.gap;
                            moved = true;
                        }
                    }
                    if (!moved) break;
                }
                if (y < best_y - .5f) {
                    best_y = y;
                    best = c;
                }
            }
            g_tiles.first = best;
            g_tiles.span = span;
            g_tiles.y = best_y;
            ImVec2 home(g_tiles.left + (float)(best) * (g_tiles.column + g_tiles.gap), best_y);
            ImGui::SetNextWindowPos(flying(ImVec2(ease(sub(id, "x"), home.x, 16.f), ease(sub(id, "y"), home.y, 16.f) - g_tiles.scroll)), ImGuiCond_Always);
        }
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
        if (width_em > 0.f) {
            ImGui::SetNextWindowSize(ImVec2(width, 0.f), ImGuiCond_Always);
            flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
        }
        bool shown = ImGui::Begin(name.c_str(), nullptr, flags);
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        float inner = (width_em > 0.f ? width : window->Size.x) - style.WindowPadding.x * 2.f;
        g_row_widths.push_back(inner);
        if (!shown) return false;

        ImVec2 start = ImGui::GetCursorPos(), start_screen = ImGui::GetCursorScreenPos();
        card_heading(title);
        ImVec2 after = ImGui::GetCursorPos();
        float bar = ImGui::GetFontSize() * 1.15f;
        float cross = bar;
        ImGui::SetCursorPos(start);
        ImGui::InvisibleButton("##echo-tile-grab", ImVec2(std::max(1.f, inner - cross - style.ItemInnerSpacing.x), bar));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        if (along >= 1.f && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!state.pinned) {
                state.pinned = true;
                state.x = window->Pos.x / display.x;
                state.y = window->Pos.y / display.y;
            }
            state.x += g.IO.MouseDelta.x / display.x;
            state.y += g.IO.MouseDelta.y / display.y;
            g_tiles.dragged = true;
        }
        if (ImGui::IsItemDeactivated() && state.pinned) g_tiles_changed = true;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && state.pinned) {
            state.pinned = false;
            g_tiles_changed = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive())
            ImGui::SetTooltip(state.pinned ? "Drag to move. Double-click to unpin." : "Drag to pin this somewhere.");
        ImRect cross_box(ImVec2(start_screen.x + inner - cross, start_screen.y), ImVec2(start_screen.x + inner, start_screen.y + bar));
        if (close_button(window, cross_box, "##echo-tile-hide")) {
            state.hidden = true;
            g_tiles_changed = true;
        }
        if (state.pinned) {
            ImVec2 dot(cross_box.Min.x - bar * .45f, start_screen.y + bar * .5f);
            circle_filled_cached(window->DrawList, dot, bar * .14f, ImGui::GetColorU32(ImGuiCol_CheckMark), 12);
        }
        ImGui::SetCursorPos(after);
        return true;
    }

    static void end_tile() {
        g_row_widths.pop_back();
        if (g_tiles.skipped) {
            g_tiles.skipped = false;
            return;
        }
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        g_tile_widths[window->ID] = window->Size.x;
        if (TileState* state = g_tiles.open) {
            state->width = window->Size.x;
            state->height = window->Size.y;
            if (!state->pinned) {
                float bottom = g_tiles.y + window->Size.y + g_tiles.gap;
                for (int k = 0; k < g_tiles.span; ++k)
                    g_tiles.below[(size_t)(g_tiles.first + k)] = bottom;
            }
        }
        ImGui::End();
    }

    std::unordered_map<std::string, TileState> g_tile_states;

    bool g_tiles_changed = false;

    void BeginTiles(float left, float top, float right, float bottom, float tile_width_em, float shown, float rate) {
        const ImGuiStyle& style = ImGui::GetStyle();
        g_tiles.on = true;
        g_tiles.shown = shown;
        g_tiles.rate = rate;
        g_tiles.born = 0;
        g_tiles.frame = ImGui::GetFrameCount();
        g_tiles.dragged = false;
        g_tiles.bottom = bottom;
        g_tiles.gap = em(.55f);
        g_tiles.left = left;
        g_tiles.top = top;
        float natural = em(tile_width_em) + style.WindowPadding.x * 2.f;
        int columns = std::max(1, (int)((right - left + g_tiles.gap) / (natural + g_tiles.gap)));
        g_tiles.column = (right - left - g_tiles.gap * (float)(columns - 1)) / (float)(columns);
        g_tiles.below.assign((size_t)columns, top);
    }

    static bool g_tile_of_cards = false;

    bool BeginTileOfCards(const char* title, float width_em) {
        if (!g_tiles.on) return true;
        bool shown = begin_tile(title, width_em);
        g_tiles.on = false;
        g_tile_of_cards = true;
        return shown;
    }

    void EndTileOfCards() {
        if (!g_tile_of_cards) return;
        g_tile_of_cards = false;
        g_tiles.on = true;
        end_tile();
    }

    void TileGroup(const char* name) {
        g_tiles.group = name;
    }

    void EndTiles() {
        g_tiles.on = false;
        float lowest = g_tiles.top;
        for (float y : g_tiles.below)
            lowest = std::max(lowest, y - g_tiles.gap);
        float over = std::max(0.f, lowest - g_tiles.bottom);
        ImGuiContext& g = *GImGui;
        if (over > 0.f && g.IO.MouseWheel != 0.f && g.ActiveId == 0) {
            bool taken = false;
            for (ImGuiWindow* window = g.HoveredWindow; window; window = window->ParentWindow) {
                if (window->ScrollMax.y > 0.f || (window->Flags & ImGuiWindowFlags_Popup))
                    taken = true;
            }
            if (!taken) g_tiles.scroll -= g.IO.MouseWheel * em(3.f);
        }
        g_tiles.scroll = std::clamp(g_tiles.scroll, 0.f, over);
        if (over <= 0.f) return;
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        float track = g_tiles.bottom - g_tiles.top;
        float total = lowest - g_tiles.top;
        float thumb = std::max(em(1.5f), track * track / total);
        float at = g_tiles.top + (track - thumb) * (g_tiles.scroll / over);
        float x = g.IO.DisplaySize.x - em(.32f);
        float wide = std::max(2.f, em(.16f));
        draw->AddRectFilled(ImVec2(x, at), ImVec2(x + wide, at + thumb), ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), .8f)), wide * .5f);
    }

    bool BeginCard(const char* title, float width_em) {
        if (g_tiles.on) return begin_tile(title, width_em);
        const ImGuiStyle& style = ImGui::GetStyle();
        float width = em(width_em), height = 0.f;
        if (g_single_column) width = std::max(width, ImGui::GetContentRegionAvail().x);
        if (!g_card_rows.empty() && !g_single_column) {
            if (g_card_rows.back().cards++ > 0) ImGui::SameLine();
            auto known = g_row_heights.find(g_card_rows.back().id);
            if (known != g_row_heights.end())
                height = known->second + card_row_extra(g_card_rows.back().id);
        }
        g_card_needs = -1.f;
        bool shown = ImGui::BeginChild(title, ImVec2(width, height), height > 0.f ? ImGuiChildFlags_Borders : ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        g_row_widths.push_back(width - style.WindowPadding.x * 2.f);
        if (shown) card_heading(title);
        return shown;
    }

    void EndCard() {
        if (g_tiles.on) {
            g_card_needs = -1.f;
            end_tile();
            return;
        }
        g_row_widths.pop_back();
        if (!g_card_rows.empty()) {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            float needs = g_card_needs >= 0.f ? g_card_needs : window->DC.CursorMaxPos.y - window->Pos.y + ImGui::GetStyle().WindowPadding.y;
            g_card_rows.back().tallest = std::max(g_card_rows.back().tallest, needs);
        }
        g_card_needs = -1.f;
        ImGui::EndChild();
    }

    void RowWith(const char* label, float reserve_em, const char* tip) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        float start = control_start();
        ImGui::SameLine(start);
        float reserve = reserve_em > 0.f ? em(reserve_em) + ImGui::GetStyle().ItemSpacing.x : 0.f;
        ImGui::SetNextItemWidth(std::max(em(2.f), ImGui::GetStyle().WindowPadding.x + row_width() - start - reserve));
    }

    void Row(const char* label, const char* tip) {
        RowWith(label, 0.f, tip);
    }

    void RowHalf() {
        ImGui::SameLine(control_start());
    }

    float RowButton(int count) {
        float gap = ImGui::GetStyle().ItemSpacing.x;
        return (row_width() - gap * (float)(count - 1)) / (float)(std::max(count, 1));
    }

    float RowRight(float width_em) {
        float width = em(width_em);
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + row_width() - width);
        return width;
    }

    bool Gear(const char* id_text) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        float side = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + row_width() - side);
        if (window->SkipItems) return false;
        ImGuiID id = window->GetID(id_text);
        ImVec2 pos = window->DC.CursorPos;
        ImRect bb(pos, ImVec2(pos.x + side, pos.y + side));
        ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        float hot = ease(id, hovered ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        draw->AddRectFilled(bb.Min, bb.Max, ImGui::GetColorU32(faded(color(ImGuiCol_FrameBgHovered), .35f + .65f * hot)), ImGui::GetStyle().FrameRounding);
        stroke_icon(draw, bb.GetCenter(), side * .64f, Icon::Gear, ImGui::GetColorU32(mix(color(ImGuiCol_TextDisabled), color(ImGuiCol_CheckMark), hot)));
        return pressed;
    }

    bool BeginModal(const char* title, float width_em, bool closable) {
        ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar;
        ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(display.x * .97f, display.y * .95f));
#ifdef GEODE_IS_MOBILE
        ImGui::SetNextWindowPos(ImVec2(display.x * .5f, display.y * .5f), ImGuiCond_Always, ImVec2(.5f, .5f));
        flags |= ImGuiWindowFlags_NoMove;
#endif
        if (!ImGui::BeginPopupModal(title, nullptr, flags)) return false;
        {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            bool top = g.OpenPopupStack.Size > 0 && g.OpenPopupStack.back().Window == window;
            if (top && !ImGui::IsWindowAppearing() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !window->Rect().Contains(g.IO.MousePos))
                ImGui::CloseCurrentPopup();
        }
        float width = em(width_em);
        g_row_widths.push_back(width > 0.f ? width : ImGui::GetContentRegionAvail().x);
        if (TitleBar(title, Icon::None, closable, width)) ImGui::CloseCurrentPopup();
        if (width > 0.f) ImGui::Dummy(ImVec2(width, 0.f));
        return true;
    }

    void EndModal() {
        g_row_widths.pop_back();
        ImGui::EndPopup();
    }

    bool ColorEdit4(const char* label, float* rgba, ImGuiColorEditFlags flags) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        const ImGuiStyle& style = ImGui::GetStyle();
        bool alpha = !(flags & ImGuiColorEditFlags_NoAlpha);
        ImGuiID id = window->GetID(label);
        ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
        float side = ImGui::GetFrameHeight();
        ImVec2 pos = window->DC.CursorPos;
        ImRect total(pos, ImVec2(pos.x + side + (label_size.x > 0.f ? style.ItemInnerSpacing.x + label_size.x : 0.f), pos.y + side));
        ImGui::ItemSize(total, style.FramePadding.y);
        bool changed = false;
        ImGui::PushID(id);
        if (ImGui::ItemAdd(total, id)) {
            bool hovered = false;
            bool held = false;
            if (ImGui::ButtonBehavior(total, id, &hovered, &held)) ImGui::OpenPopup("##echo-color");
            float hot = ease(id, hovered ? 1.f : 0.f);
            ImDrawList* draw = window->DrawList;
            ImVec2 mid(pos.x + side * .5f, pos.y + side * .5f);
            float radius = side * (.38f + .03f * hot);
            ImVec4 shown(rgba[0], rgba[1], rgba[2], alpha ? rgba[3] : 1.f);
            if (shown.w < .995f) {
                circle_filled_cached(draw, mid, radius, IM_COL32(60, 60, 60, (int)(255 * style.Alpha)), 24);
                draw->PathArcTo(mid, radius, -1.5707963f, 1.5707963f, 12);
                draw->PathFillConvex(IM_COL32(200, 200, 200, (int)(255 * style.Alpha)));
            }
            shown.w *= style.Alpha;
            circle_filled_cached(draw, mid, radius, ImGui::ColorConvertFloat4ToU32(shown), 24);
            draw->AddCircle(mid, radius, ImGui::GetColorU32(mix(color(ImGuiCol_Border), color(ImGuiCol_CheckMark), hot)), 24, std::max(1.f, side * .06f));
            if (label_size.x > 0.f)
                ImGui::RenderText(ImVec2(pos.x + side + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y), label);
        }
        if (ImGui::BeginPopup("##echo-color")) {
            float width = em(12.f);
            ImGui::SetNextItemWidth(width);
            ImGuiColorEditFlags picker = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoInputs |
                                         (alpha ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha);
            changed |= ImGui::ColorPicker4("##pick", rgba, picker);
            static std::string hex;
            static ImGuiID hex_for = 0;
            auto to_hex = [&] {
                auto byte = [](float v) {
                    return (int)(std::clamp(v, 0.f, 1.f) * 255.f + .5f);
                };
                char text[16];
                if (alpha)
                    snprintf(text, sizeof(text), "%02X%02X%02X%02X", byte(rgba[0]), byte(rgba[1]), byte(rgba[2]), byte(rgba[3]));
                else
                    snprintf(text, sizeof(text), "%02X%02X%02X", byte(rgba[0]), byte(rgba[1]), byte(rgba[2]));
                return std::string(text);
            };
            bool typing = hex_for == id && ImGui::IsPopupOpen("##echo-color") && ImGui::GetActiveID() == ImGui::GetID("##hex");
            if (!typing) {
                hex = to_hex();
                hex_for = id;
            }
            ImGui::SetNextItemWidth(width);
            if (InputTextWithHint("##hex", alpha ? "RRGGBBAA" : "RRGGBB", &hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
                size_t need = alpha ? 8 : 6;
                if (hex.size() == need || (alpha && hex.size() == 6)) {
                    unsigned long packed = strtoul(hex.c_str(), nullptr, 16);
                    if (hex.size() == 8) {
                        rgba[0] = ((packed >> 24) & 255) / 255.f;
                        rgba[1] = ((packed >> 16) & 255) / 255.f;
                        rgba[2] = ((packed >> 8) & 255) / 255.f;
                        rgba[3] = (packed & 255) / 255.f;
                    }
                    else {
                        rgba[0] = ((packed >> 16) & 255) / 255.f;
                        rgba[1] = ((packed >> 8) & 255) / 255.f;
                        rgba[2] = (packed & 255) / 255.f;
                    }
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return changed;
    }

    bool ColorEdit3(const char* label, float* rgb, ImGuiColorEditFlags flags) {
        float rgba[4] = { rgb[0], rgb[1], rgb[2], 1.f };
        if (!ColorEdit4(label, rgba, flags | ImGuiColorEditFlags_NoAlpha)) return false;
        rgb[0] = rgba[0];
        rgb[1] = rgba[1];
        rgb[2] = rgba[2];
        return true;
    }

    bool InputTextMultiline(const char* label, std::string* text, const ImVec2& size_arg) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGuiID id = window->GetID(label);
        ImVec2 size = ImGui::CalcItemSize(size_arg, ImGui::CalcItemWidth(), ImGui::GetFontSize() * 8.f + style.FramePadding.y * 2.f);
        ImVec2 pos = window->DC.CursorPos;
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        g_field = { id, bb, window };
        if (!window->SkipItems && ImGui::IsRectVisible(bb.Min, bb.Max))
            draw_field(window, id, bb, -1.f, g_field_states[id]);
        push_clear_frame();
        bool changed = ImGui::InputTextMultiline(label, text, size_arg);
        end_field();
        return changed;
    }

    bool Slider(const char* label, float* value, float min, float max, const char* format, bool rainbow) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiContext& g = *GImGui;
        const ImGuiStyle& style = g.Style;
        ImGuiID id = window->GetID(label);
        ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
        float width = ImGui::CalcItemWidth(), height = ImGui::GetFrameHeight();
        ImVec2 pos = window->DC.CursorPos;
        ImRect frame(pos, ImVec2(pos.x + width, pos.y + height));
        ImRect total(pos, ImVec2(frame.Max.x + (label_size.x > 0.f ? style.ItemInnerSpacing.x + label_size.x : 0.f), frame.Max.y));
        ImGui::ItemSize(total, style.FramePadding.y);
        if (!ImGui::ItemAdd(total, id, &frame)) return false;
        bool hovered = false;
        bool held = false;
        ImGui::ButtonBehavior(frame, id, &hovered, &held);
        float knob = height * .34f;
        float left = frame.Min.x + knob, right = frame.Max.x - knob;
        bool changed = false;
        if (held && right > left) {
            float t = std::clamp((g.IO.MousePos.x - left) / (right - left), 0.f, 1.f);
            float next = min + (max - min) * t;
            if (next != *value) {
                *value = next;
                changed = true;
                ImGui::MarkItemEdited(id);
            }
        }
        float t = max > min ? std::clamp((*value - min) / (max - min), 0.f, 1.f) : 0.f;
        float hot = ease(id, hovered || held ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        float mid_y = (frame.Min.y + frame.Max.y) * .5f;
        float thick = std::max(4.f, height * .22f);
        ImVec2 a(left, mid_y - thick * .5f);
        ImVec2 b(right, mid_y + thick * .5f);
        float alpha = style.Alpha;
        if (rainbow) {
            for (int i = 0; i < 6; ++i) {
                float t0 = (float)(i) / 6.f, t1 = (float)(i + 1) / 6.f;
                ImVec4 c0(0.f, 0.f, 0.f, alpha);
                ImVec4 c1(0.f, 0.f, 0.f, alpha);
                ImGui::ColorConvertHSVtoRGB(t0, .7f, 1.f, c0.x, c0.y, c0.z);
                ImGui::ColorConvertHSVtoRGB(t1 >= 1.f ? 0.f : t1, .7f, 1.f, c1.x, c1.y, c1.z);
                ImU32 u0 = ImGui::ColorConvertFloat4ToU32(c0), u1 = ImGui::ColorConvertFloat4ToU32(c1);
                draw->AddRectFilledMultiColor(ImVec2(a.x + (b.x - a.x) * t0, a.y), ImVec2(a.x + (b.x - a.x) * t1 + .5f, b.y), u0, u1, u1, u0);
            }
            draw->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f), ImGui::GetColorU32(ImGuiCol_WindowBg), thick * .5f, 0, 2.f);
        }
        else {
            draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), thick * .5f);
            draw->AddRectFilled(a, ImVec2(left + (right - left) * t, b.y), ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), .75f)), thick * .5f);
        }
        ImVec2 at(left + (right - left) * t, mid_y);
        ImVec4 knob_fill = color(ImGuiCol_Text);
        if (rainbow) {
            ImGui::ColorConvertHSVtoRGB(t >= 1.f ? 0.f : t, .7f, 1.f, knob_fill.x, knob_fill.y, knob_fill.z);
            knob_fill.w = 1.f;
        }
        circle_filled_cached(draw, at, knob * (1.f + .15f * hot), ImGui::GetColorU32(knob_fill), 24);
        draw->AddCircle(at, knob * (1.f + .15f * hot), ImGui::GetColorU32(mix(color(ImGuiCol_WindowBg), color(ImGuiCol_CheckMark), hot)), 24, std::max(1.5f, height * .07f));
        if (held && format && *format) ImGui::SetTooltip(format, *value);
        if (label_size.x > 0.f)
            ImGui::RenderText(ImVec2(frame.Max.x + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y), label);
        return changed;
    }

    bool ThemeTile(const char* id_text, ImTextureID logo, bool selected) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        ImGuiID id = window->GetID(id_text);
        float side = em(2.5f);
        ImVec2 pos = window->DC.CursorPos;
        ImRect bb(pos, ImVec2(pos.x + side, pos.y + side));
        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, id)) return false;
        bool hovered = false;
        bool held = false;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
        float hot = ease(id, hovered ? 1.f : 0.f);
        float on = ease(sub(id, "on"), selected ? 1.f : 0.f);
        ImDrawList* draw = window->DrawList;
        float inset = side * (.12f - .05f * hot);
        if (logo)
            draw->AddImage(ImTextureRef(logo), ImVec2(bb.Min.x + inset, bb.Min.y + inset), ImVec2(bb.Max.x - inset, bb.Max.y - inset), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f), ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, 1.f)));
        if (on > .01f)
            draw->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(faded(color(ImGuiCol_Text), on)), side * .26f, 0, std::max(2.f, side * .05f));
        return pressed;
    }

    bool Header(const char* title, const char* version, ImTextureID logo) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        const ImGuiStyle& style = ImGui::GetStyle();
        float height = ImGui::GetFrameHeight() * 1.2f;
        float start_y = ImGui::GetCursorPosY();
        ImVec2 screen = ImGui::GetCursorScreenPos();
        if (logo) {
            ImGui::Image(ImTextureRef(logo), ImVec2(height, height));
            ImGui::SameLine();
        }
        float big = 1.25f;
        ImGui::SetWindowFontScale(big);
        ImGui::SetCursorPosY(start_y + (height - ImGui::GetFontSize()) * .5f);
        ImGui::TextUnformatted(title);
        float title_bottom = ImGui::GetItemRectMax().y;
        ImGui::SetWindowFontScale(1.f);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (title_bottom - ImGui::GetCursorScreenPos().y) - ImGui::GetFontSize() - ImGui::GetFontSize() * .06f);
        ImGui::TextDisabled("%s", version);

        float side = height * .82f;
        float right = window->Pos.x + window->Size.x - style.WindowPadding.x;
        ImRect bb(ImVec2(right - side, screen.y + (height - side) * .5f), ImVec2(right, screen.y + (height + side) * .5f));
        bool pressed = close_button(window, bb, "##echo-close");
        ImGui::SetCursorPosY(start_y + height + style.ItemSpacing.y);
        return pressed;
    }

    void NavMarker(ImVec2 min, ImVec2 max) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImGuiID id = window->GetID("##echo-nav-marker");
        float top = window->Pos.y + ease(id, min.y - window->Pos.y, 18.f);
        float bottom = window->Pos.y + ease(sub(id, "bottom"), max.y - window->Pos.y, 18.f);
        float inset = (bottom - top) * .2f;
        float width = std::max(2.f, ImGui::GetFontSize() * .16f);
        float x = min.x + width;
        window->DrawList->AddRectFilled(ImVec2(x, top + inset), ImVec2(x + width, bottom - inset), ImGui::GetColorU32(ImGuiCol_CheckMark), width * .5f);
    }

    bool g_rainbow_rim = false;

    bool g_button_effects = true;

    Scene g_scene = Scene::None;

    static const char* const SCENE_NAMES[] = { "", "halloween", "hell", "heaven", "space", "ocean", "sakura" };

    const char* scene_name(Scene which) {
        return SCENE_NAMES[(int)(which)];
    }

    Scene scene_named(std::string_view name) {
        for (int i = 1; i < (int)(std::size(SCENE_NAMES)); ++i) {
            if (name == SCENE_NAMES[i]) return (Scene)i;
        }
        return Scene::None;
    }

    void apply_metrics() {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 14.f;
        style.ChildRounding = 10.f;
        style.PopupRounding = 10.f;
        style.FrameRounding = 7.f;
        style.GrabRounding = 6.f;
        style.ScrollbarRounding = 8.f;
        style.TabRounding = 7.f;
        style.WindowPadding = ImVec2(14.f, 12.f);
        style.FramePadding = ImVec2(9.f, 5.f);
        style.ItemSpacing = ImVec2(8.f, 7.f);
        style.ItemInnerSpacing = ImVec2(7.f, 5.f);
        style.WindowBorderSize = 0.f;
        style.ChildBorderSize = 1.f;
        style.PopupBorderSize = 0.f;
        style.FrameBorderSize = 0.f;
        style.ScrollbarSize = 12.f;
        style.GrabMinSize = 10.f;
        style.WindowTitleAlign = ImVec2(.5f, .5f);
    }

    void sync_scene_turn(const Palette& palette) {
        body_turn = accent_turn = ColorGrade{};
        night = 0.f;
        auto ratio = [](float now, float own) {
            return own > .001f ? std::max(now, 0.f) / own : 1.f;
        };
        for (int i = 0; i < preset_count() && g_scene != Scene::None; ++i) {
            const auto& own = preset(i).palette;
            if (own.scene != g_scene) continue;
            body_turn = { palette.hue - own.hue, ratio(palette.richness, own.richness), ratio(palette.depth, own.depth) };
            if (own.light) {
                night = std::clamp((1.f - body_turn.brightness) / (1.f - .5f), 0.f, 1.f);
                body_turn.brightness = std::min(1.f, palette.depth / .5f);
            }
            accent_turn = { palette.accent_hue - own.accent_hue, ratio(palette.accent_saturation, own.accent_saturation), ratio(palette.accent_value, own.accent_value) };
            break;
        }
    }

    void apply_palette(const Palette& palette) {
        ImGuiStyle& style = ImGui::GetStyle();
        auto hsv = [](float h, float s, float v, float a = 1.f) {
            h -= std::floor(h);
            ImVec4 out(0.f, 0.f, 0.f, a);
            ImGui::ColorConvertHSVtoRGB(h, std::clamp(s, 0.f, 1.f), std::clamp(v, 0.f, 1.f), out.x, out.y, out.z);
            return out;
        };
        float h = palette.hue;
        float ha = palette.accent_hue;
        float s = palette.richness;
        float d = palette.depth;
        float as = palette.accent_saturation;
        float av = palette.accent_value;
        struct ColorSet {
            ImVec4 background, panel, raised, line, text, muted, accent, accent_hot, button, button_hot, button_down;
        };
        auto light_set = [&](float lift) {
            return ColorSet{ hsv(h, .05f * s, .93f * lift, .98f), hsv(h, .05f * s, .97f * lift), hsv(h, .06f * s, .85f * lift), hsv(h, .08f * s, .68f * lift, .8f),
                hsv(h, .25f * s, .12f), hsv(h, .12f * s, .42f), hsv(ha, as, .24f), hsv(ha, as, .38f),
                hsv(ha, as * .4f, .80f), hsv(ha, as * .4f, .73f), hsv(ha, as * .4f, .65f) };
        };
        auto dark_set = [&](float dd) {
            dd = std::max(dd, 0.f);
            auto value = [dd](float base, float floor) {
                return dd >= .5f ? base * dd : floor + (base * .5f - floor) * dd / .5f;
            };
            auto pressed = [dd](float base, float floor) {
                return dd >= .5f ? base * std::sqrt(dd) : floor + (base * std::sqrt(.5f) - floor) * dd / .5f;
            };
            return ColorSet{ hsv(h, .50f * s, value(.10f, 0.f), .98f), hsv(h, .50f * s, value(.145f, .025f)), hsv(h, .51f * s, value(.21f, .06f)), hsv(h, .50f * s, value(.33f, .14f), .8f),
                hsv(h, .09f * s, 1.f), hsv(h, .27f * s, .68f), hsv(ha, as, av), hsv(ha, as * .7f, std::min(1.f, av * 1.08f)),
                hsv(ha, std::min(1.f, as * 1.28f), pressed(.33f, .1f)), hsv(ha, std::min(1.f, as * 1.25f), pressed(.44f, .16f)), hsv(ha, std::min(1.f, as * 1.22f), pressed(.54f, .22f)) };
        };
        ColorSet look;
        night_menu = 0.f;
        if (!palette.light) look = dark_set(d);
        else {
            float fade = night_fade(d);
            ColorSet day = light_set(std::min(1.f, .85f + .15f * (d - NIGHT_LOW) / (1.f - NIGHT_LOW)));
            ColorSet dark = dark_set(.8f + (std::min(d, NIGHT_TOP) - .5f) / (NIGHT_TOP - .5f) * .7f);
            ImVec4 under = mix(day.panel, dark.panel, fade);
            auto linear = [](float c) {
                return std::pow(c, 2.2f);
            };
            float luminance = .2126f * linear(under.x) + .7152f * linear(under.y) + .0722f * linear(under.z);
            float flip = luminance < .18f ? 1.f : 0.f;
            // the buttons go dark w/ the text, light text on a still light button is unreadable too
            float buttons = std::max(fade, flip);
            night_menu = buttons;
            look = { mix(day.background, dark.background, fade), mix(day.panel, dark.panel, fade), mix(day.raised, dark.raised, fade), mix(day.line, dark.line, fade),
                mix(day.text, dark.text, flip), mix(day.muted, dark.muted, flip), mix(day.accent, dark.accent, flip), mix(day.accent_hot, dark.accent_hot, flip),
                mix(day.button, dark.button, buttons), mix(day.button_hot, dark.button_hot, buttons), mix(day.button_down, dark.button_down, buttons) };
        }
        ImVec4 background = look.background, panel = look.panel, raised = look.raised, line = look.line, text = look.text, muted = look.muted;
        ImVec4 accent = look.accent, accent_hot = look.accent_hot, button = look.button, button_hot = look.button_hot, buttonDown = look.button_down;
        g_rainbow_rim = palette.rainbow;
        if (palette.rainbow) {
            background.w = 0.f;
            raised = ImVec4(0.f, 0.f, 0.f, .26f);
            line = ImVec4(1.f, 1.f, 1.f, .26f);
            text = ImVec4(1.f, 1.f, 1.f, 1.f);
            muted = ImVec4(1.f, 1.f, 1.f, .68f);
            accent = ImVec4(1.f, 1.f, 1.f, 1.f);
            accent_hot = ImVec4(1.f, 1.f, 1.f, 1.f);
            button = ImVec4(1.f, 1.f, 1.f, .11f);
            button_hot = ImVec4(1.f, 1.f, 1.f, .2f);
            buttonDown = ImVec4(1.f, 1.f, 1.f, .3f);
        }
        g_scene = palette.rainbow ? Scene::None : palette.scene;
        sync_scene_turn(palette);
        if (g_scene != Scene::None) background.w = 0.f;
        auto set = [&](ImGuiCol slot, ImVec4 value) {
            style.Colors[slot] = value;
        };
        set(ImGuiCol_Text, text);
        set(ImGuiCol_TextDisabled, muted);
        set(ImGuiCol_WindowBg, background);
        set(ImGuiCol_ChildBg, palette.rainbow ? ImVec4(0.f, 0.f, 0.f, .30f) : faded(panel, .6f));
        set(ImGuiCol_PopupBg, panel);
        set(ImGuiCol_Border, line);
        set(ImGuiCol_BorderShadow, ImVec4(0.f, 0.f, 0.f, 0.f));
        set(ImGuiCol_FrameBg, raised);
        set(ImGuiCol_FrameBgHovered, mix(raised, accent, .18f));
        set(ImGuiCol_FrameBgActive, mix(raised, accent, .32f));
        set(ImGuiCol_TitleBg, background);
        set(ImGuiCol_TitleBgActive, panel);
        set(ImGuiCol_TitleBgCollapsed, background);
        set(ImGuiCol_MenuBarBg, panel);
        set(ImGuiCol_ScrollbarBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        set(ImGuiCol_ScrollbarGrab, button);
        set(ImGuiCol_ScrollbarGrabHovered, button_hot);
        set(ImGuiCol_ScrollbarGrabActive, accent);
        set(ImGuiCol_CheckMark, accent);
        set(ImGuiCol_SliderGrab, accent);
        set(ImGuiCol_SliderGrabActive, accent_hot);
        set(ImGuiCol_Button, button);
        set(ImGuiCol_ButtonHovered, button_hot);
        set(ImGuiCol_ButtonActive, buttonDown);
        set(ImGuiCol_Header, mix(raised, accent, .22f));
        set(ImGuiCol_HeaderHovered, mix(raised, accent, .34f));
        set(ImGuiCol_HeaderActive, mix(raised, accent, .48f));
        set(ImGuiCol_Separator, line);
        set(ImGuiCol_SeparatorHovered, accent);
        set(ImGuiCol_SeparatorActive, accent_hot);
        set(ImGuiCol_ResizeGrip, button);
        set(ImGuiCol_ResizeGripHovered, button_hot);
        set(ImGuiCol_ResizeGripActive, accent);
        set(ImGuiCol_Tab, panel);
        set(ImGuiCol_TabHovered, button_hot);
        set(ImGuiCol_TabActive, button);
        set(ImGuiCol_TabUnfocused, background);
        set(ImGuiCol_TabUnfocusedActive, panel);
        set(ImGuiCol_PlotLines, accent);
        set(ImGuiCol_PlotLinesHovered, accent_hot);
        set(ImGuiCol_PlotHistogram, accent);
        set(ImGuiCol_PlotHistogramHovered, accent_hot);
        set(ImGuiCol_TableHeaderBg, raised);
        set(ImGuiCol_TableBorderStrong, line);
        set(ImGuiCol_TableBorderLight, faded(line, .5f));
        set(ImGuiCol_TableRowBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        set(ImGuiCol_TableRowBgAlt, faded(raised, .35f));
        set(ImGuiCol_TextSelectedBg, faded(accent, .35f));
        set(ImGuiCol_DragDropTarget, accent_hot);
        set(ImGuiCol_NavHighlight, accent);
        set(ImGuiCol_NavWindowingHighlight, ImVec4(1.f, 1.f, 1.f, .7f));
        set(ImGuiCol_NavWindowingDimBg, ImVec4(.2f, .2f, .2f, .2f));
        set(ImGuiCol_ModalWindowDimBg, ImVec4(0.f, 0.f, 0.f, .5f));
    }

    void apply_echo_theme() {
        apply_metrics();
        apply_palette(Palette{});
    }

    static float deg(float degrees) {
        return degrees / 360.f;
    }

    static const Preset PRESETS[] = {
        { "Echo", Palette{} },
        { "Red", Palette{ deg(0.f), deg(0.f) } },
        { "Orange", Palette{ deg(28.f), deg(28.f) } },
        { "Yellow", Palette{ deg(54.f), deg(54.f) } },
        { "Green", Palette{ deg(130.f), deg(130.f) } },
        { "Blue", Palette{ deg(215.f), deg(215.f) } },
        { "Indigo", Palette{ deg(245.f), deg(245.f) } },
        { "Violet", Palette{ deg(280.f), deg(280.f) } },
        { "Silver", Palette{ 0.f, 0.f, 0.f, 1.15f, 0.f, .82f } },
        { "Black", Palette{ 0.f, 0.f, 0.f, .55f, 0.f, .66f } },
        { "White", Palette{ 0.f, 0.f, 0.f, 1.f, 0.f, 1.f, true } },
        { "Rainbow", Palette{ deg(250.f), deg(250.f), .45f, .9f, .14f, 1.f, false, true } },
        { "Halloween", Palette{ deg(275.f), deg(26.f), 1.15f, .85f, .8f, 1.f, false, false, Scene::Halloween } },
        { "Hell", Palette{ deg(4.f), deg(16.f), 1.3f, .8f, .8f, 1.f, false, false, Scene::Hell } },
        { "Heaven", Palette{ deg(44.f), deg(40.f), 1.4f, 1.f, .7f, 1.f, true, false, Scene::Heaven } },
        { "Space", Palette{ deg(250.f), deg(192.f), 1.2f, .75f, .55f, 1.f, false, false, Scene::Space } },
        { "Ocean", Palette{ deg(200.f), deg(168.f), 1.2f, .9f, .6f, 1.f, false, false, Scene::Ocean } },
        { "Sakura", Palette{ deg(322.f), deg(335.f), 1.1f, .95f, .5f, 1.f, false, false, Scene::Sakura } },
    };

    int preset_count() {
        return (int)(std::size(PRESETS));
    }

    const Preset& preset(int index) {
        return PRESETS[std::clamp(index, 0, preset_count() - 1)];
    }
}
