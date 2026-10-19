#include "GUI/widgets.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#define CODE_INDENT "    "

namespace EchoUI {
    enum class Token : unsigned char {
        Plain,
        Keyword,
        Known,
        Number,
        String,
        Comment,
        Count
    };

    struct EditorInput {
        bool tab = false;
        bool enter = false;
        int cursor = -1;
    };

    static const char* const LUA_KEYWORDS[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };
    static const char* const LUA_NAMES[] = { "assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable", "tonumber", "tostring", "type", "xpcall", "math", "string", "table", "utf8", "coroutine" };

    static bool name_start(char letter) {
        return std::isalpha((unsigned char)letter) || letter == '_';
    }

    static bool name_letter(char letter) {
        return std::isalnum((unsigned char)letter) || letter == '_';
    }

    static int long_bracket(const std::string& text, size_t at) {
        if (at >= text.size() || text[at] != '[') return -1;
        size_t i = at + 1;
        while (i < text.size() && text[i] == '=') ++i;
        if (i < text.size() && text[i] == '[') return (int)(i - at - 1);
        return -1;
    }

    static size_t long_bracket_end(const std::string& text, size_t from, int level) {
        std::string close = "]" + std::string((size_t)level, '=') + "]";
        size_t found = text.find(close, from);
        return found == std::string::npos ? text.size() : found + close.size();
    }

    static void color_lua(const std::string& text, const std::unordered_set<std::string_view>& known, std::vector<Token>& tokens) {
        static const std::unordered_set<std::string_view> keywords(std::begin(LUA_KEYWORDS), std::end(LUA_KEYWORDS));
        tokens.assign(text.size(), Token::Plain);
        size_t i = 0;
        while (i < text.size()) {
            char letter = text[i];
            size_t start = i;
            Token token = Token::Plain;
            if (letter == '-' && i + 1 < text.size() && text[i + 1] == '-') {
                int level = long_bracket(text, i + 2);
                if (level >= 0) i = long_bracket_end(text, i + 4 + (size_t)level, level);
                else i = std::min(text.find('\n', i), text.size());
                token = Token::Comment;
            }
            else if (letter == '"' || letter == '\'') {
                ++i;
                while (i < text.size() && text[i] != letter && text[i] != '\n') {
                    if (text[i] == '\\' && i + 1 < text.size()) ++i;
                    ++i;
                }
                if (i < text.size() && text[i] == letter) ++i;
                token = Token::String;
            }
            else if (long_bracket(text, i) >= 0) {
                int level = long_bracket(text, i);
                i = long_bracket_end(text, i + 2 + (size_t)level, level);
                token = Token::String;
            }
            else if (std::isdigit((unsigned char)letter) || (letter == '.' && i + 1 < text.size() && std::isdigit((unsigned char)text[i + 1]))) {
                ++i;
                while (i < text.size()) {
                    char next = text[i];
                    char before = (char)std::tolower((unsigned char)text[i - 1]);
                    bool sign = (next == '+' || next == '-') && (before == 'e' || before == 'p');
                    if (!name_letter(next) && next != '.' && !sign) break;
                    ++i;
                }
                token = Token::Number;
            }
            else if (name_start(letter)) {
                while (i < text.size() && name_letter(text[i])) ++i;
                std::string_view word(text.data() + start, i - start);
                bool member_of_known = start >= 2 && text[start - 1] == '.' && tokens[start - 2] == Token::Known;
                if (keywords.contains(word)) token = Token::Keyword;
                else if (member_of_known || known.contains(word)) token = Token::Known;
            }
            else {
                ++i;
            }
            std::fill(tokens.begin() + (long long)start, tokens.begin() + (long long)i, token);
        }
    }

    static int on_editor_input(ImGuiInputTextCallbackData* data) {
        auto& input = *static_cast<EditorInput*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
            if (data->EventChar == '\t' && ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                input.tab = true;
                return 1;
            }
            if (data->EventChar == '\n') input.enter = true;
            return 0;
        }
        if (input.tab) {
            input.tab = false;
            if (data->HasSelection()) {
                int from = std::min(data->SelectionStart, data->SelectionEnd);
                data->DeleteChars(from, std::abs(data->SelectionEnd - data->SelectionStart));
            }
            data->InsertChars(data->CursorPos, CODE_INDENT);
        }
        if (input.enter) {
            input.enter = false;
            int line_end = data->CursorPos - 1;
            if (line_end >= 0 && data->Buf[line_end] == '\n') {
                int line_start = line_end;
                while (line_start > 0 && data->Buf[line_start - 1] != '\n') --line_start;
                int indent_end = line_start;
                while (indent_end < line_end && (data->Buf[indent_end] == ' ' || data->Buf[indent_end] == '\t')) ++indent_end;
                std::string indent(data->Buf + line_start, data->Buf + indent_end);
                if (!indent.empty()) data->InsertChars(data->CursorPos, indent.c_str());
            }
        }
        input.cursor = data->CursorPos;
        return 0;
    }

    static void draw_code_line(ImDrawList* draw, float size, ImVec2 pos, const char* begin, const char* end, const Token* tokens, const ImU32* colors, float right) {
        ImFontBaked* baked = ImGui::GetFont()->GetFontBaked(size);
        float scale = size / baked->Size;
        float x = IM_TRUNC(pos.x), y = IM_TRUNC(pos.y);
        for (const char* at = begin; at < end && x <= right;) {
            unsigned int letter = (unsigned char)*at;
            ImU32 tint = colors[(int)tokens[at - begin]];
            at += letter < 128 ? 1 : ImTextCharFromUtf8(&letter, at, end);
            if (letter == '\r') continue;
            const ImFontGlyph* glyph = baked->FindGlyph((ImWchar)letter);
            if (!glyph) continue;
            if (glyph->Visible) {
                if (glyph->Colored) tint |= ~IM_COL32_A_MASK;
                draw->PrimReserve(6, 4);
                draw->PrimRectUV(ImVec2(x + glyph->X0 * scale, y + glyph->Y0 * scale), ImVec2(x + glyph->X1 * scale, y + glyph->Y1 * scale), ImVec2(glyph->U0, glyph->V0), ImVec2(glyph->U1, glyph->V1), tint);
            }
            x += glyph->AdvanceX * scale;
        }
    }

    static void token_colors(ImU32* colors) {
        const auto& style = ImGui::GetStyle().Colors;
        ImVec4 text = style[ImGuiCol_Text];
        float brightness = text.x * .3f + text.y * .59f + text.z * .11f;
        bool dark = brightness > .5f;
        ImVec4 accent = style[ImGuiCol_CheckMark];
        accent.w = text.w;
        ImVec4 known(text.x + (accent.x - text.x) * .5f, text.y + (accent.y - text.y) * .5f, text.z + (accent.z - text.z) * .5f, text.w);
        ImVec4 number = dark ? ImVec4(.55f, .82f, 1.f, 1.f) : ImVec4(.1f, .4f, .75f, 1.f);
        ImVec4 quoted = dark ? ImVec4(.95f, .74f, .48f, 1.f) : ImVec4(.66f, .36f, .05f, 1.f);
        colors[(int)Token::Plain] = ImGui::ColorConvertFloat4ToU32(text);
        colors[(int)Token::Keyword] = ImGui::ColorConvertFloat4ToU32(accent);
        colors[(int)Token::Known] = ImGui::ColorConvertFloat4ToU32(known);
        colors[(int)Token::Number] = ImGui::ColorConvertFloat4ToU32(number);
        colors[(int)Token::String] = ImGui::ColorConvertFloat4ToU32(quoted);
        colors[(int)Token::Comment] = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    }

    static std::vector<int> line_starts_of(const std::string& text) {
        std::vector<int> starts = { 0 };
        for (int i = 0; i < (int)text.size(); ++i) {
            if (text[i] == '\n') starts.push_back(i + 1);
        }
        return starts;
    }

    struct CodeRow {
        int from = 0, to = 0;
        int line = 0;
        bool first = true;
    };

    static std::vector<CodeRow> rows_of(const std::string& text, const std::vector<int>& line_starts, float wrap_width) {
        std::vector<CodeRow> rows;
        const char* begin = text.c_str();
        const char* end = begin + text.size();
        if (wrap_width <= 0.f) {
            for (int line = 0; line < (int)line_starts.size(); ++line) {
                int to = line + 1 < (int)line_starts.size() ? line_starts[(size_t)line + 1] - 1 : (int)text.size();
                rows.push_back({ line_starts[(size_t)line], to, line, true });
            }
            return rows;
        }
        ImGuiContext& g = *GImGui;
        int line = 0;
        for (const char* s = begin; s < end; s = (*s == '\n') ? s + 1 : s) {
            int from = (int)(s - begin);
            while (line + 1 < (int)line_starts.size() && line_starts[(size_t)line + 1] <= from) ++line;
            s = ImFontCalcWordWrapPositionEx(g.Font, g.FontSize, s, end, wrap_width, ImDrawTextFlags_WrapKeepBlanks);
            rows.push_back({ from, (int)(s - begin), line, from == line_starts[(size_t)line] });
        }
        if (rows.empty() || text.empty() || text.back() == '\n') {
            int last = (int)line_starts.size() - 1;
            rows.push_back({ (int)text.size(), (int)text.size(), last, true });
        }
        return rows;
    }

    bool CodeEditor(const char* label, std::string* text, const ImVec2& size, const std::vector<std::string>& names, CodeEditorState& state) {
        const float default_lines = 16.f, current_line_alpha = .05f;
        const ImVec4 error_line(1.f, .36f, .44f, .16f);
        ImGuiContext& g = *GImGui;
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;
        const ImGuiStyle& style = g.Style;
        float font = ImGui::GetFontSize();
        std::vector<int> line_starts = line_starts_of(*text);
        int digits = std::max(2, (int)std::to_string(line_starts.size()).size());
        float gutter = ImGui::CalcTextSize("0").x * (float)digits + style.ItemInnerSpacing.x * 2.f;
        ImVec2 start = window->DC.CursorPos;
        ImVec2 box_size = ImGui::CalcItemSize(size, ImGui::GetContentRegionAvail().x, font * default_lines);
        ImGui::SetCursorScreenPos(ImVec2(start.x + gutter, start.y));

        EditorInput input;
        ImVec4 ink = style.Colors[ImGuiCol_Text];
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ink);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(ink.x, ink.y, ink.z, 0.f));
        ImGuiID id = window->GetID(label);
        ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackAlways;
        if (state.wrap) flags |= ImGuiInputTextFlags_WordWrap;
        bool changed = InputTextMultiline(label, text, ImVec2(box_size.x - gutter, box_size.y), flags, on_editor_input, &input);
        ImGui::PopStyleColor(2);
        if (changed) line_starts = line_starts_of(*text);
        if (input.cursor >= 0) {
            int cursor = std::min(input.cursor, (int)text->size());
            auto line = std::upper_bound(line_starts.begin(), line_starts.end(), cursor) - 1;
            state.line = (int)(line - line_starts.begin()) + 1;
            state.column = ImTextCountCharsFromUtf8(text->c_str() + *line, text->c_str() + cursor) + 1;
        }

        const char* box_name = nullptr;
        ImFormatStringToTempBuffer(&box_name, nullptr, "%s/%s_%08X", window->Name, label, id);
        ImGuiWindow* box = ImGui::FindWindowByName(box_name);
        if (!box || box->LastFrameActive != ImGui::GetFrameCount()) return changed;
        float wrap_width = state.wrap ? std::max(1.f, box->Size.x - style.FramePadding.x * 2.f - style.ScrollbarSize) : 0.f;
        std::vector<CodeRow> rows = rows_of(*text, line_starts, wrap_width);
        if (state.jump_to_line > 0) {
            int row_at = 0;
            for (int row = 0; row < (int)rows.size(); ++row) {
                if (rows[(size_t)row].line + 1 == state.jump_to_line) {
                    row_at = row;
                    break;
                }
            }
            ImGui::SetScrollY(box, std::max(0.f, (float)row_at * font - box->Size.y * .5f));
            state.jump_to_line = 0;
        }

        ImGuiInputTextState* typing = ImGui::GetInputTextState(id);
        bool scrolled = typing && (g.ActiveId == id || g.ActiveId == ImGui::GetWindowScrollbarID(box, ImGuiAxis_Y));
        float scroll_x = scrolled && !state.wrap ? typing->Scroll.x : 0.f;
        ImVec2 origin(box->Pos.x + style.FramePadding.x - scroll_x, box->Pos.y + style.FramePadding.y - box->Scroll.y);
        ImVec2 clip_min = box->Pos, clip_max(box->Pos.x + box->Size.x - box->ScrollbarSizes.x, box->Pos.y + box->Size.y);
        int first = std::max((int)((clip_min.y - origin.y) / font), 0);
        int last = std::min(std::max((int)std::ceil((clip_max.y - origin.y) / font), first), (int)rows.size());
        float first_y = IM_TRUNC(origin.y + (float)first * font);

        std::unordered_set<std::string_view> known(std::begin(LUA_NAMES), std::end(LUA_NAMES));
        for (const auto& name : names)
            known.insert(name);
        std::vector<Token> tokens;
        color_lua(*text, known, tokens);
        ImU32 colors[(int)Token::Count];
        token_colors(colors);

        ImDrawList* draw = window->DrawList;
        draw->PushClipRect(ImVec2(start.x, clip_min.y), clip_max, true);
        for (int at = first; at < last; ++at) {
            const CodeRow& row = rows[(size_t)at];
            float y = first_y + (float)(at - first) * font;
            bool error = row.line + 1 == state.error_line;
            bool current = row.line + 1 == state.line && g.ActiveId == id;
            if (error) draw->AddRectFilled(ImVec2(clip_min.x, y), ImVec2(clip_max.x, y + font), ImGui::GetColorU32(error_line));
            else if (current) draw->AddRectFilled(ImVec2(clip_min.x, y), ImVec2(clip_max.x, y + font), ImGui::GetColorU32(ImGuiCol_Text, current_line_alpha));
            if (row.first) {
                std::string number = std::to_string(row.line + 1);
                float number_x = start.x + gutter - style.ItemInnerSpacing.x - ImGui::CalcTextSize(number.c_str()).x;
                ImU32 number_ink = error ? ImGui::GetColorU32(ERROR_RED) : ImGui::GetColorU32(current ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                draw->AddText(ImVec2(number_x, y), number_ink, number.c_str());
            }
            draw->PushClipRect(clip_min, clip_max, true);
            draw_code_line(draw, font, ImVec2(origin.x, y), text->c_str() + row.from, text->c_str() + row.to, tokens.data() + row.from, colors, clip_max.x);
            draw->PopClipRect();
        }
        draw->PopClipRect();
        return changed;
    }
}
