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

        void sparks(Backdrop const& back, int count, ImVec4 cool, ImVec4 hot, float speed) {
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
                back.draw->AddCircleFilled(at, radius, ImGui::ColorConvertFloat4ToU32(faded(tint, .8f * lit)), 6);
            }
        }

        void flames(Backdrop const& back, float height, float speed, float seed, ImVec4 tint) {
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
                if (in >= back.rounding)
                    return back.b.y;
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

        void halloween_scene(Backdrop const& back) {
            backdrop_fill(back, IM_COL32(24, 11, 40, 255), IM_COL32(10, 4, 14, 255));
            backdrop_glow(back, IM_COL32(255, 120, 20, 255), .45f, .16f + .16f * candle());
            back.draw->PushClipRect(back.a, back.b, true);
            sparks(back, scatter(back, 70.f, 3, 26), ImVec4(1.f, .45f, .08f, 1.f), ImVec4(1.f, .85f, .35f, 1.f), 1.f);
            back.draw->PopClipRect();
        }

        void hell_scene(Backdrop const& back) {
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
                back.draw->AddCircleFilled(at, back.unit * .1f, ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .85f, .4f, lit)), 6);
            }
            back.draw->PopClipRect();
        }

        void heaven_scene(Backdrop const& back) {
            float dawn = .5f + .5f * std::sin(back.time * .3f);
            backdrop_fill(back, ImGui::ColorConvertFloat4ToU32(mix(ImVec4(.70f, .85f, 1.f, 1.f), ImVec4(1.f, .82f, .9f, 1.f), dawn)), IM_COL32(255, 245, 220, 255));
            back.draw->PushClipRect(back.a, back.b, true);
            ImVec2 sun(back.screen.x * .5f, -back.screen.y * .12f);
            soft_disc(back.draw, sun, back.screen.y * (.75f + .12f * std::sin(back.time * 1.1f)), ImVec4(1.f, .9f, .55f, .55f * back.alpha), 1.f, 32);
            float reach = back.screen.y * 1.8f;
            for (int k = 0; k < 9; ++k) {
                float number = (float)(k);
                float lean = std::remainder(number * .6981317f + back.time * .09f, 6.2831853f);
                if (std::abs(lean) > 1.45f)
                    continue;
                float wide = .08f + .04f * std::sin(back.time * .9f + number * 2.f);
                beam(back.draw, sun, ImVec2(sun.x + std::sin(lean - wide) * reach, sun.y + std::cos(lean - wide) * reach), ImVec2(sun.x + std::sin(lean + wide) * reach, sun.y + std::cos(lean + wide) * reach), ImVec4(1.f, .8f, .3f, .6f * back.alpha));
            }
            for (int k = 0; k < 4; ++k) {
                float spread = back.time * .16f + (float)(k) * .25f;
                spread -= std::floor(spread);
                back.draw->AddCircle(sun, back.screen.y * (.15f + 1.5f * spread), ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, .86f, .45f, .55f * (1.f - spread) * std::min(1.f, spread * 6.f) * back.alpha)), 96, back.unit * (.12f + .5f * spread));
            }
            constexpr float PUFFS[][3] = { { -1.3f, .15f, .8f }, { -.5f, -.25f, 1.f }, { .4f, -.1f, 1.15f }, { 1.2f, .2f, .75f } };
            for (int k = 0; k < 6; ++k) {
                float number = (float)(k);
                float size = back.unit * (2.2f + .5f * (float)(k % 3));
                float span = back.screen.x + back.unit * 24.f;
                ImVec2 at(std::fmod(back.time * back.unit * (1.6f + .7f * number) + number * span * .23f, span) - back.unit * 12.f, back.screen.y * (.1f + .16f * number) + std::sin(back.time * .5f + number) * back.unit * .8f);
                if (outside(back, at, size * 2.8f))
                    continue;
                for (auto const& puff : PUFFS)
                    soft_disc(back.draw, ImVec2(at.x + puff[0] * size, at.y + (puff[1] + .28f) * size), size * puff[2], ImVec4(.52f, .64f, .88f, .45f * back.alpha), .6f);
                for (auto const& puff : PUFFS)
                    soft_disc(back.draw, ImVec2(at.x + puff[0] * size, at.y + puff[1] * size), size * puff[2], ImVec4(1.f, 1.f, 1.f, .95f * back.alpha), .62f);
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

        void space_scene(Backdrop const& back) {
            backdrop_fill(back, IM_COL32(5, 5, 18, 255), IM_COL32(14, 8, 34, 255));
            back.draw->PushClipRect(back.a, back.b, true);
            constexpr float GAS[][3] = { { .55f, .14f, .62f }, { .1f, .42f, .7f }, { .22f, .2f, .75f }, { .6f, .2f, .35f } };
            for (int k = 0; k < 4; ++k) {
                float number = (float)(k);
                float turn = back.time * (.05f + .02f * number) + number * 2.1f;
                ImVec2 at(back.screen.x * (.5f + .36f * std::cos(turn)), back.screen.y * (.5f + .32f * std::sin(turn * 1.3f)));
                soft_disc(back.draw, at, back.screen.y * .62f, ImVec4(GAS[k][0], GAS[k][1], GAS[k][2], .26f * back.alpha), .75f, 32);
            }
            int dust = scatter(back, 6.f, 10, 110);
            for (int i = 0; i < dust; ++i) {
                float r = chance(back, i, 33);
                float across = chance(back, i, 31) + back.time * (.004f + .012f * r);
                float down = chance(back, i, 32) + back.time * (.002f + .005f * r);
                ImVec2 at(back.a.x + back.width * (across - std::floor(across)), back.a.y + back.tall * (down - std::floor(down)));
                float glimmer = .25f + .75f * (.5f + .5f * std::sin(back.time * (.8f + 2.4f * r) + r * 50.f));
                soft_disc(back.draw, at, back.unit * (.09f + .09f * r), ImVec4(.82f, .88f, 1.f, (.3f + .5f * r) * glimmer * back.alpha), 1.f, 6);
            }
            auto star = [&](ImVec2 at, float size, ImVec4 tint, float lit, bool rays, float spin) {
                soft_disc(back.draw, at, size * 5.f, faded(tint, .4f * lit), 1.f, 8);
                back.draw->AddCircleFilled(at, size, ImGui::ColorConvertFloat4ToU32(faded(tint, lit)), 6);
                if (!rays)
                    return;
                float reach = size * (4.f + 7.f * lit);
                float thin = size * .45f;
                ImU32 ray = ImGui::ColorConvertFloat4ToU32(faded(tint, .75f * lit));
                ImVec2 along(std::cos(spin), std::sin(spin));
                ImVec2 across(-along.y, along.x);
                auto off = [&](ImVec2 dir, float by) {
                    return ImVec2(at.x + dir.x * by, at.y + dir.y * by);
                };
                back.draw->AddQuadFilled(off(along, -reach), off(across, -thin), off(along, reach), off(across, thin), ray);
                back.draw->AddQuadFilled(off(across, -reach), off(along, thin), off(across, reach), off(along, -thin), ray);
            };
            constexpr float TINTS[][3] = { { .75f, .86f, 1.f }, { 1.f, .93f, .8f }, { 1.f, .78f, .9f }, { .7f, 1.f, .96f } };
            int stars = scatter(back, 14.f, 6, 70);
            for (int i = 0; i < stars; ++i) {
                float r = chance(back, i, 3);
                float twinkle = .3f + .7f * (.5f + .5f * std::sin(back.time * (1.f + 3.f * r) + chance(back, i, 4) * 6.2831853f));
                auto const& tint = TINTS[(int)(chance(back, i, 5) * 3.999f)];
                float across = chance(back, i, 1) + back.time * (.01f + .03f * r);
                float down = chance(back, i, 2) + back.time * (.004f + .012f * r);
                ImVec2 at(back.a.x + back.width * (across - std::floor(across)), back.a.y + back.tall * (down - std::floor(down)));
                float spin = back.time * (.25f + .5f * chance(back, i, 6)) * (chance(back, i, 7) < .5f ? -1.f : 1.f);
                star(at, back.unit * (.04f + .06f * r), ImVec4(tint[0], tint[1], tint[2], 1.f), twinkle * back.alpha, r > .68f, spin);
            }
            for (int lane = 0; lane < 2; ++lane) {
                float every = 2.2f + 1.6f * chance(back, lane, 71);
                float clock = back.time / every + chance(back, lane, 72);
                int which = (int)(std::floor(clock));
                float run = (clock - std::floor(clock)) / .45f;
                if (run >= 1.f)
                    continue;
                int seed = which * 2 + lane;
                ImVec2 start(back.a.x + back.width * (.05f + .7f * chance(back, seed, 73)), back.a.y + back.tall * (.02f + .4f * chance(back, seed, 74)));
                float angle = .35f + .4f * chance(back, seed, 75);
                float travel = back.unit * (14.f + 10.f * chance(back, seed, 76));
                ImVec2 dir(std::cos(angle), std::sin(angle));
                ImVec2 head(start.x + dir.x * travel * run, start.y + dir.y * travel * run);
                float fade = std::sin(3.1415927f * run) * back.alpha;
                float tail = back.unit * (3.f + 3.f * run);
                ImVec2 end(head.x - dir.x * tail, head.y - dir.y * tail);
                ImU32 bright = ImGui::ColorConvertFloat4ToU32(ImVec4(.9f, .95f, 1.f, .9f * fade));
                ImU32 gone = ImGui::ColorConvertFloat4ToU32(ImVec4(.9f, .95f, 1.f, 0.f));
                float thick = std::max(1.f, back.unit * .09f);
                ImVec2 side(-dir.y * thick, dir.x * thick);
                back.draw->PrimReserve(6, 4);
                auto base = back.draw->_VtxCurrentIdx;
                ImVec2 uv = back.draw->_Data->TexUvWhitePixel;
                back.draw->PrimWriteVtx(ImVec2(head.x + side.x, head.y + side.y), uv, bright);
                back.draw->PrimWriteVtx(ImVec2(head.x - side.x, head.y - side.y), uv, bright);
                back.draw->PrimWriteVtx(end, uv, gone);
                back.draw->PrimWriteVtx(end, uv, gone);
                back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
                back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
                back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(base));
                back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
                back.draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
                soft_disc(back.draw, head, back.unit * .35f, ImVec4(.85f, .92f, 1.f, .5f * fade), 1.f, 8);
                back.draw->AddCircleFilled(head, thick * 1.1f, bright, 8);
            }
            float every = 3.5f + 3.f * chance(back, 0, 41);
            float clock = back.time / every + chance(back, 0, 42);
            int which = (int)(clock);
            float flare = (clock - (float)(which)) / .3f;
            if (flare < 1.f) {
                float burst = std::sin(3.1415927f * flare);
                float drift = clock - (float)(which);
                ImVec2 at(back.a.x + back.width * (.08f + .84f * chance(back, which, 43)) + drift * back.unit * 2.f, back.a.y + back.tall * (.08f + .84f * chance(back, which, 44)) + drift * back.unit);
                star(at, back.unit * .13f * burst, ImVec4(1.f, 1.f, 1.f, 1.f), burst * back.alpha, true, flare * 1.2f);
            }
            back.draw->PopClipRect();
        }

        void ocean_scene(Backdrop const& back) {
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
                back.draw->AddCircleFilled(ImVec2(at.x - radius * .35f, at.y - radius * .35f), radius * .2f, ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 1.f, 1.f, .5f * lit)), 6);
            }
            back.draw->PopClipRect();
        }

        void sakura_scene(Backdrop const& back) {
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

        void button_skin(ImDrawList* draw, ImRect const& body, float rounding, ImGuiID id, float hot, float down, float keep, ImVec4 const* given) {
            Scene which = scene();
            if (which == Scene::None || keep <= .01f)
                return;

            struct Look {
                ImVec4 top, bottom;
                ImVec4 glow;
                ImVec4 edge;
            };

            static Look const LOOKS[] = {
                {},
                { { .30f, .15f, .44f, 1.f }, { .13f, .06f, .21f, 1.f }, { 1.f, .5f, .08f, 1.f }, { 1.f, .55f, .12f, 1.f } },
                { { .44f, .07f, .04f, 1.f }, { .15f, .02f, .02f, 1.f }, { 1.f, .45f, .05f, 1.f }, { 1.f, .4f, .08f, 1.f } },
                { { 1.f, .97f, .86f, 1.f }, { 1.f, .83f, .48f, 1.f }, { 1.f, 1.f, 1.f, 1.f }, { .85f, .62f, .18f, 1.f } },
                { { .21f, .16f, .5f, 1.f }, { .07f, .05f, .22f, 1.f }, { .6f, .88f, 1.f, 1.f }, { .5f, .6f, 1.f, 1.f } },
                { { .1f, .5f, .62f, 1.f }, { .03f, .19f, .37f, 1.f }, { .65f, 1.f, .95f, 1.f }, { .35f, .9f, .85f, 1.f } },
                { { .64f, .3f, .48f, 1.f }, { .33f, .13f, .3f, 1.f }, { 1.f, .82f, .9f, 1.f }, { 1.f, .62f, .8f, 1.f } },
            };
            Look look = LOOKS[(int)(which)];
            look.top = turned(look.top, accent_turn);
            look.bottom = turned(look.bottom, accent_turn);
            look.glow = turned(look.glow, accent_turn);
            look.edge = turned(look.edge, accent_turn);
            float lively = 1.f;
            if (given) {
                look.top = mix(*given, ImVec4(1.f, 1.f, 1.f, 1.f), .14f);
                look.bottom = ImVec4(given->x * .58f, given->y * .58f, given->z * .58f, 1.f);
                lively = std::clamp(std::max({ given->x, given->y, given->z }) * 1.8f, .3f, 1.f);
            }
            float alpha = ImGui::GetStyle().Alpha * keep;
            float time = (float)(ImGui::GetTime());
            float wide = body.GetWidth();
            float tall = std::max(body.GetHeight(), 1.f);
            auto own = [&](int item) {
                ImGuiID mixed = ImHashData(&item, sizeof(item), id);
                return (float)(mixed & 0xffff) / 65535.f;
            };
            auto shaded = [&](ImVec4 tint) {
                tint = mix(mix(tint, look.glow, hot * .22f), ImVec4(0.f, 0.f, 0.f, 1.f), down * .2f);
                tint.w = 1.f;
                return ImGui::ColorConvertFloat4ToU32(tint);
            };
            int from = draw->VtxBuffer.Size;
            draw->AddRectFilled(body.Min, body.Max, IM_COL32_WHITE, rounding);
            ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, from, draw->VtxBuffer.Size, body.Min, ImVec2(body.Min.x, body.Max.y), shaded(look.top), shaded(look.bottom));
            fade_verts(draw, from, .94f * alpha);
            alpha *= lively;

            draw->PushClipRect(body.Min, body.Max, true);
            auto underglow = [&](float strength) {
                float flicker = .55f + .25f * std::sin(time * (3.f + 3.f * own(1)) + own(2) * 40.f) + .2f * std::sin(time * (9.f + 4.f * own(3)) + own(4) * 20.f);
                float top = body.Min.y + tall * .25f;
                int first = draw->VtxBuffer.Size;
                draw->AddRectFilled(ImVec2(body.Min.x, top), body.Max, ImGui::ColorConvertFloat4ToU32(look.glow), rounding, ImDrawFlags_RoundCornersBottom);
                for (int v = first; v < draw->VtxBuffer.Size; ++v) {
                    auto& vert = draw->VtxBuffer[v];
                    float low = std::clamp((vert.pos.y - top) / std::max(body.Max.y - top, 1.f), 0.f, 1.f);
                    unsigned shown = static_cast<unsigned>((float)((vert.col >> IM_COL32_A_SHIFT) & 0xff) * low * low * strength * flicker * (1.f + hot * .7f) * alpha);
                    vert.col = (vert.col & ~IM_COL32_A_MASK) | (std::min(shown, 255u) << IM_COL32_A_SHIFT);
                }
            };
            auto gleam = [&](float speed, float strength) {
                float at = (std::fmod(time * speed, 1.8f) - .4f) * ImGui::GetIO().DisplaySize.x;
                float half = tall * .9f;
                float slant = tall * .5f;
                if (at + half + slant < body.Min.x || at - half - slant > body.Max.x)
                    return;
                ImU32 bright = ImGui::ColorConvertFloat4ToU32(faded(look.glow, strength * alpha));
                ImU32 gone = bright & ~IM_COL32_A_MASK;
                ImVec2 uv = draw->_Data->TexUvWhitePixel;
                draw->PrimReserve(12, 6);
                unsigned first = draw->_VtxCurrentIdx;
                draw->PrimWriteVtx(ImVec2(at - half + slant, body.Min.y), uv, gone);
                draw->PrimWriteVtx(ImVec2(at + slant, body.Min.y), uv, bright);
                draw->PrimWriteVtx(ImVec2(at + half + slant, body.Min.y), uv, gone);
                draw->PrimWriteVtx(ImVec2(at - half - slant, body.Max.y), uv, gone);
                draw->PrimWriteVtx(ImVec2(at - slant, body.Max.y), uv, bright);
                draw->PrimWriteVtx(ImVec2(at + half - slant, body.Max.y), uv, gone);
                for (unsigned offset : { 0u, 1u, 4u, 0u, 4u, 3u, 1u, 2u, 5u, 1u, 5u, 4u })
                    draw->PrimWriteIdx(static_cast<ImDrawIdx>(first + offset));
            };
            // off = same colors but nothing moves
            switch (button_effects() ? which : Scene::None) {
            case Scene::Halloween:
                underglow(.5f);
                break;
            case Scene::Hell: {
                underglow(.75f);
                for (int i = 0; i < 3; ++i) {
                    float life = time * (.5f + .5f * own(10 + i)) + own(20 + i);
                    life -= std::floor(life);
                    ImVec2 at(body.Min.x + wide * own(30 + i) + std::sin(time * 3.f + own(40 + i) * 9.f) * tall * .12f, body.Max.y - life * tall * 1.1f);
                    soft_disc(draw, at, tall * .13f, ImVec4(1.f, .7f, .2f, .85f * (1.f - life) * alpha), 1.f, 6);
                }
                break;
            }
            case Scene::Heaven:
                gleam(.34f, .85f);
                break;
            case Scene::Sakura:
                gleam(.26f, .4f);
                break;
            case Scene::Space: {
                int stars = std::clamp((int)(wide / (tall * 1.1f)), 2, 7);
                for (int i = 0; i < stars; ++i) {
                    float r = own(10 + i);
                    float twinkle = .5f + .5f * std::sin(time * (1.5f + 2.5f * r) + r * 30.f);
                    float across = own(20 + i) + time * (.03f + .07f * r) * tall / std::max(wide, 1.f) * 3.f;
                    across -= std::floor(across);
                    soft_disc(draw, ImVec2(body.Min.x + wide * across, body.Min.y + tall * (.15f + .7f * own(30 + i))), tall * (.07f + .1f * twinkle), faded(look.glow, .85f * twinkle * alpha), 1.f, 6);
                }
                break;
            }
            case Scene::Ocean: {
                int points = std::clamp((int)(wide / (tall * .3f)), 4, 48);
                for (int line = 0; line < 2; ++line) {
                    float deep = (float)(line);
                    for (int i = 0; i <= points; ++i) {
                        float x = body.Min.x + wide * (float)(i) / (float)(points);
                        draw->PathLineTo(ImVec2(x, body.Min.y + tall * (.12f + .4f * deep + .34f * noise(x / tall * .55f + time * (.5f - .9f * deep), time * .45f + own(1) * 9.f + deep * 5.f))));
                    }
                    draw->PathStroke(ImGui::ColorConvertFloat4ToU32(faded(look.glow, (.42f - .2f * deep) * alpha)), 0, std::max(1.f, tall * .07f));
                }
                break;
            }
            case Scene::None:
                break;
            }
            draw->PopClipRect();

            draw->AddLine(ImVec2(body.Min.x + rounding, body.Min.y + 1.f), ImVec2(body.Max.x - rounding, body.Min.y + 1.f), IM_COL32(255, 255, 255, (int)(60.f * alpha)));
            ImVec4 edge = look.edge;
            if (which == Scene::Space && button_effects())
                ImGui::ColorConvertHSVtoRGB(.66f + .14f * std::sin(time * .8f + own(5) * 6.2831853f), .6f, 1.f, edge.x, edge.y, edge.z);
            draw->AddRect(body.Min, body.Max, ImGui::ColorConvertFloat4ToU32(faded(edge, (.45f + .45f * hot) * alpha)), rounding, 0, 1.f);
        }

        void drawScene(ImGuiWindow* window, float alpha) {
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
            switch (scene()) {
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
        }
    }

    void WindowRims(float alpha) {
        if (alpha <= .01f)
            return;
        ImGuiContext& g = *GImGui;
        for (ImGuiWindow* window : g.Windows) {
            if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_ChildWindow))
                continue;
            if (window->Size.x < 4.f || window->Size.y < 4.f || window->IsFallbackWindow)
                continue;
            if (window->Flags & ImGuiWindowFlags_NoBackground)
                continue;
            bool plain = (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) != 0;
            if (rainbow_rim() && !plain)
                rainbow_body(window, alpha);
            if (scene() != Scene::None && !plain)
                drawScene(window, alpha);
            rim(window, alpha);
        }
    }

    namespace {
        struct FieldState {
            bool hovered = false;
            bool active = false;
        };

        std::unordered_map<ImGuiID, FieldState>& field_states() {
            static std::unordered_map<ImGuiID, FieldState> states;
            return states;
        }

        struct OpenField {
            ImGuiID id = 0;
            ImRect bb;
            ImGuiWindow* window = nullptr;
        };

        OpenField s_field;

        void draw_field(ImGuiWindow* window, ImGuiID id, ImRect const& bb, float fill, FieldState const& state) {
            ImGuiStyle const& style = ImGui::GetStyle();
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

        void push_clear_frame() {
            ImVec4 none(0.f, 0.f, 0.f, 0.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, none);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, none);
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, none);
        }

        float range_fill(double value, double min, double max) {
            if (!(min < max) || !std::isfinite(min) || !std::isfinite(max) || max - min > 1e7)
                return -1.f;
            return (float)(std::clamp((value - min) / (max - min), 0.0, 1.0));
        }

        void stroke_icon(ImDrawList* draw, ImVec2 c, float s, Icon icon, ImU32 ink) {
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
                draw->AddCircleFilled(c, s * .34f, ink, 24);
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
                    draw->AddCircleFilled(at(knobs[i], y), s * .11f, ink, 12);
                }
                break;
            }
            case Icon::Tools: {
                constexpr float WIDTH = .15f, HEAD_SIZE = .21f, HEAD_AT = .2f, JAW = .06f, TIP = .16f, CAP_AT = -.38f;
                constexpr float PI_F = 3.14159265f;
                ImVec2 half[40];
                int m = 0;
                for (int i = 0; i <= 6; ++i) {
                    float a = PI_F + PI_F * .5f * (float)(i) / 6.f;
                    half[m++] = ImVec2(CAP_AT + WIDTH * .5f * std::cos(a), WIDTH * .5f * std::sin(a));
                }
                float neck = HEAD_AT - std::sqrt(HEAD_SIZE * HEAD_SIZE - WIDTH * WIDTH * .25f);
                float from = std::atan2(-WIDTH * .5f, neck - HEAD_AT), to = -std::acos(TIP / HEAD_SIZE);
                if (from > to)
                    from -= 2.f * PI_F;
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
                constexpr float STEP = 6.2831853f / 8.f;
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
                draw->AddCircleFilled(c, s * .12f, ink, 12);
                break;
            case Icon::Sequence:
                for (int i = 0; i < 3; ++i) {
                    float y = ((float)(i) - 1.f) * .28f;
                    draw->AddCircleFilled(at(-.36f, y), s * .07f, ink, 10);
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