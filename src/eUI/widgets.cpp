// echos own look. imgui still does the layout & input, these draw the controls:
#include "GUI/widgets.hpp"
#include "GUI/gui.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <unordered_map>

using namespace geode::prelude;

namespace eui {
    namespace {
        ImVec4 mix(ImVec4 const& a, ImVec4 const& b, float t) {
            return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
        }

        ImVec4 color(ImGuiCol slot) {
            return ImGui::GetStyleColorVec4(slot);
        }

        ImVec4 faded(ImVec4 value, float alpha) {
            value.w *= alpha;
            return value;
        }

        void gloss(ImDrawList* draw, ImRect const& bb, float rounding) {
            if (!rainbow_rim())
                return;
            float alpha = ImGui::GetStyle().Alpha;
            int from = draw->VtxBuffer.Size;
            draw->AddRect(ImVec2(bb.Min.x + .5f, bb.Min.y + .5f), ImVec2(bb.Max.x - .5f, bb.Max.y - .5f), IM_COL32_WHITE, rounding, 0, 1.f);
            float height = std::max(bb.GetHeight(), 1.f);
            for (int v = from; v < draw->VtxBuffer.Size; ++v) {
                auto& vert = draw->VtxBuffer[v];
                float t = std::clamp((vert.pos.y - bb.Min.y) / height, 0.f, 1.f);
                float keep = (float)((vert.col >> IM_COL32_A_SHIFT) & 0xff) / 255.f;
                unsigned shown = static_cast<unsigned>(255.f * keep * (.42f - .32f * t) * alpha);
                vert.col = (vert.col & ~IM_COL32_A_MASK) | (shown << IM_COL32_A_SHIFT);
            }
        }

        void button_skin(ImDrawList* draw, ImRect const& body, float rounding, ImGuiID id, float hot, float down, float keep = 1.f, ImVec4 const* own = nullptr);

        // how far custom color moved off the themes own colors (hue, 0-1)
        float body_turn = 0.f, accent_turn = 0.f;

        ImVec4 turned(ImVec4 tint, float turn) {
            if (std::abs(turn) < .002f)
                return tint;
            float h, sat, val;
            ImGui::ColorConvertRGBtoHSV(tint.x, tint.y, tint.z, h, sat, val);
            h += turn;
            h -= std::floor(h);
            ImGui::ColorConvertHSVtoRGB(h, sat, val, tint.x, tint.y, tint.z);
            return tint;
        }

        void turn_verts(ImDrawList* draw, int from, float turn) {
            if (std::abs(turn) < .002f)
                return;
            for (int v = from; v < draw->VtxBuffer.Size; ++v) {
                auto& vert = draw->VtxBuffer[v];
                ImVec4 tint = turned(ImGui::ColorConvertU32ToFloat4(vert.col), turn);
                vert.col = (ImGui::ColorConvertFloat4ToU32(tint) & ~IM_COL32_A_MASK) | (vert.col & IM_COL32_A_MASK);
            }
        }

        ImGuiID sub(ImGuiID id, char const* what) {
            return ImHashStr(what, 0, id);
        }
    }

    float ease(ImGuiID id, float target, float speed) {
        static std::unordered_map<ImGuiID, float> values;
        auto [at, fresh] = values.try_emplace(id, target);
        if (fresh)
            return target;
        float dt = std::min(ImGui::GetIO().DeltaTime, 1.f / 30.f);
        float step = std::min(1.f, dt * speed * GUI::get().anim_rate());
        at->second += (target - at->second) * step;
        if (std::abs(target - at->second) < .002f)
            at->second = target;
        return at->second;
    }

    float glide_in(float t) {
        t = std::clamp(t, 0.f, 1.f);
        if (t >= 1.f)
            return 1.f;
        return 1.f - std::pow(2.f, -10.f * t);
    }

    namespace {
        struct Entrance {
            ImDrawList* list = nullptr;
            int start = 0;
            float t = 1.f;
        };

        Entrance s_entrance;
    }

    void BeginEntrance(float t) {
        s_entrance.list = ImGui::GetWindowDrawList();
        s_entrance.start = s_entrance.list->VtxBuffer.Size;
        s_entrance.t = std::clamp(t, 0.f, 1.f);
    }

    void EndEntrance() {
        ImDrawList* list = s_entrance.list;
        s_entrance.list = nullptr;
        if (!list || s_entrance.t >= 1.f)
            return;
        int end = list->VtxBuffer.Size;
        if (end <= s_entrance.start)
            return;
        ImVec2 low(FLT_MAX, FLT_MAX);
        ImVec2 high(-FLT_MAX, -FLT_MAX);
        for (int i = s_entrance.start; i < end; ++i) {
            low = ImMin(low, list->VtxBuffer[i].pos);
            high = ImMax(high, list->VtxBuffer[i].pos);
        }
        ImVec2 middle((low.x + high.x) * .5f, (low.y + high.y) * .5f);
        float glided = glide_in(s_entrance.t);
        float size = .55f + .45f * glided;
        float lift = (1.f - glided) * em(.9f);
        float alpha = std::clamp(s_entrance.t * 2.5f, 0.f, 1.f);
        for (int i = s_entrance.start; i < end; ++i) {
            ImDrawVert& vert = list->VtxBuffer[i];
            vert.pos.x = middle.x + (vert.pos.x - middle.x) * size;
            vert.pos.y = middle.y + (vert.pos.y - middle.y) * size - lift;
            unsigned shown = static_cast<unsigned>((float)((vert.col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
            vert.col = (vert.col & ~IM_COL32_A_MASK) | (shown << IM_COL32_A_SHIFT);
        }
    }

    bool Checkbox(char const* label, bool* value) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        ImGuiContext& g = *GImGui;
        ImGuiStyle const& style = g.Style;
        ImGuiID id = window->GetID(label);
        ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
        float side = ImGui::GetFrameHeight();
        ImVec2 pos = window->DC.CursorPos;
        ImRect total(pos, ImVec2(pos.x + side + (labelSize.x > 0.f ? style.ItemInnerSpacing.x + labelSize.x : 0.f), pos.y + labelSize.y + style.FramePadding.y * 2.f));
        ImGui::ItemSize(total, style.FramePadding.y);
        if (!ImGui::ItemAdd(total, id))
            return false;
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
        if (labelSize.x > 0.f)
            ImGui::RenderText(ImVec2(pos.x + side + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y), label);
        return pressed;
    }

    bool Button(char const* label, ImVec2 const& size_arg) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;
        ImGuiContext& g = *GImGui;
        ImGuiStyle const& style = g.Style;
        ImGuiID id = window->GetID(label);
        ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
        ImVec2 pos = window->DC.CursorPos;
        ImVec2 size = ImGui::CalcItemSize(size_arg, labelSize.x + style.FramePadding.x * 2.f, labelSize.y + style.FramePadding.y * 2.f);
        ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        ImGui::ItemSize(size, style.FramePadding.y);
        if (!ImGui::ItemAdd(bb, id))
            return false;
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
        for (auto const& pushed : g.ColorStack)
            tinted = tinted || pushed.Col == ImGuiCol_Button;
        ImVec4 own = color(ImGuiCol_Button);
        button_skin(draw, body, style.FrameRounding, id, hot, down, 1.f, tinted ? &own : nullptr);
        if (hot > .01f)
            draw->AddRect(body.Min, body.Max, ImGui::GetColorU32(faded(color(ImGuiCol_CheckMark), hot * .55f)), style.FrameRounding, 0, 1.f);
        ImGui::RenderNavCursor(bb, id);
        ImGui::RenderTextClipped(ImVec2(bb.Min.x + style.FramePadding.x, bb.Min.y + style.FramePadding.y), ImVec2(bb.Max.x - style.FramePadding.x, bb.Max.y - style.FramePadding.y),
            label, nullptr, &labelSize, style.ButtonTextAlign, &bb);
        return pressed;
    }

    namespace {
        void fade_verts(ImDrawList* draw, int from, float alpha) {
            for (int v = from; v < draw->VtxBuffer.Size; ++v) {
                ImU32 col = draw->VtxBuffer[v].col;
                unsigned keep = static_cast<unsigned>((float)((col >> IM_COL32_A_SHIFT) & 0xff) * alpha);
                draw->VtxBuffer[v].col = (col & ~IM_COL32_A_MASK) | (keep << IM_COL32_A_SHIFT);
            }
        }

        float candle() {
            float t = (float)(ImGui::GetTime());
            return .5f + .25f * std::sin(t * 2.1f) + .15f * std::sin(t * 5.3f + 1.7f) + .1f * std::sin(t * 11.7f + .6f);
        }

        void rim(ImGuiWindow* window, float alpha) {
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
            if (rainbow_rim()) {
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
            else if (scene() != Scene::None) {
                ImVec2 mid((a.x + b.x) * .5f, (a.y + b.y) * .5f);
                float time = (float)(ImGui::GetTime());
                float flame = candle();
                for (int v = from; v < draw->VtxBuffer.Size; ++v) {
                    auto& vert = draw->VtxBuffer[v];
                    float turn = std::atan2(vert.pos.y - mid.y, vert.pos.x - mid.x);
                    ImVec4 shade(1.f, 1.f, 1.f, 1.f);
                    switch (scene()) {
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
    }

    namespace {
        void rainbow_body(ImGuiWindow* window, float alpha) {
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
                unsigned keep = static_cast<unsigned>((float)((vert.col >> IM_COL32_A_SHIFT) & 0xff) * .96f * alpha);
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

        float chance(Backdrop const& back, int item, int use) {
            ImGuiID mixed = ImHashData(&item, sizeof(item), back.id + static_cast<ImGuiID>(use) * 7919u);
            return (float)(mixed & 0xffffff) / 16777215.f;
        }

        int scatter(Backdrop const& back, float each, int least, int most) {
            return std::clamp((int)(back.width * back.tall / (back.unit * back.unit * each)), least, most);
        }

        bool outside(Backdrop const& back, ImVec2 at, float reach) {
            return at.x + reach < back.a.x || at.x - reach > back.b.x || at.y + reach < back.a.y || at.y - reach > back.b.y;
        }

        void soft_disc(ImDrawList* draw, ImVec2 at, float radius, ImVec4 tint, float squash = 1.f, int sides = 16) {
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
                draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + offset));
            };
            for (int i = 0; i < sides; ++i) {
                unsigned in = 1 + static_cast<unsigned>(i);
                unsigned in_next = 1 + static_cast<unsigned>((i + 1) % sides);
                unsigned out = in + static_cast<unsigned>(sides);
                unsigned out_next = in_next + static_cast<unsigned>(sides);
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

        void beam(ImDrawList* draw, ImVec2 from, ImVec2 left, ImVec2 right, ImVec4 tint) {
            ImU32 near_col = ImGui::ColorConvertFloat4ToU32(tint), far_col = near_col & ~IM_COL32_A_MASK;
            ImVec2 uv = draw->_Data->TexUvWhitePixel;
            draw->PrimReserve(3, 3);
            auto first = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
            draw->PrimWriteVtx(from, uv, near_col);
            draw->PrimWriteVtx(left, uv, far_col);
            draw->PrimWriteVtx(right, uv, far_col);
            draw->PrimWriteIdx(first);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + 1));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + 2));
        }

        float noise(float x, float y) {
            auto lattice = [](int ix, int iy) {
                unsigned mixed = static_cast<unsigned>(ix) * 374761393u + static_cast<unsigned>(iy) * 668265263u;
                mixed = (mixed ^ (mixed >> 13)) * 1274126177u;
                return (float)((mixed ^ (mixed >> 16)) & 0xffff) / 65535.f;
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

        void ribbon(Backdrop const& back, float y, float thick, float swell, float speed, float seed, ImVec4 tint) {
            if (y + thick * 1.5f + swell < back.a.y || y - thick * 1.5f - swell > back.b.y)
                return;
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
                    back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + offset));
                left = right;
            }
        }

        void backdrop_fill(Backdrop const& back, ImU32 top, ImU32 bottom) {
            int from = back.draw->VtxBuffer.Size;
            back.draw->AddRectFilled(back.a, back.b, IM_COL32_WHITE, back.rounding);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(back.draw, from, back.draw->VtxBuffer.Size, back.a, ImVec2(back.a.x, back.b.y), top, bottom);
            fade_verts(back.draw, from, .97f * back.alpha);
        }

        void backdrop_glow(Backdrop const& back, ImU32 tint, float from, float strength) {
            float top = back.a.y + back.tall * from;
            int first = back.draw->VtxBuffer.Size;
            back.draw->AddRectFilled(ImVec2(back.a.x, top), back.b, tint, back.rounding, ImDrawFlags_RoundCornersBottom);
            for (int v = first; v < back.draw->VtxBuffer.Size; ++v) {
                auto& vert = back.draw->VtxBuffer[v];
                float low = std::clamp((vert.pos.y - top) / std::max(back.b.y - top, 1.f), 0.f, 1.f);
                unsigned keep = static_cast<unsigned>((float)((vert.col >> IM_COL32_A_SHIFT) & 0xff) * low * strength * back.alpha);
                vert.col = (vert.col & ~IM_COL32_A_MASK) | (keep << IM_COL32_A_SHIFT);
            }
        }