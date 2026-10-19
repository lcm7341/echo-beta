// the input editor: every input in a list that can be selected, moved, offset & deleted
#include "GUI/gui.hpp"
#include "Hack/label_types.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cfloat>
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
#include <cctype>

using namespace geode::prelude;

ImGuiChildFlags GUI::fit_card() {
    return ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY;
}

float GUI::column_width(std::initializer_list<const char*> labels) {
    float width = 0.f;
    for (const char* label : labels)
        width = std::max(width, GUI::get().label_width(label));
    return width;
}

std::string GUI::ordinal(int number) {
    int teen = number % 100;
    if (teen >= 11 && teen <= 13) return std::to_string(number) + "th";
    switch (number % 10) {
    case 1:
        return std::to_string(number) + "st";
    case 2:
        return std::to_string(number) + "nd";
    case 3:
        return std::to_string(number) + "rd";
    default:
        return std::to_string(number) + "th";
    }
}

std::string GUI::format_plain(const Logic& value) {
    std::ostringstream out;
    out << value.macro_fps << "\n";
    for (size_t i = 0; i < value.inputs.size(); ++i) {
        const auto& input = value.inputs[i];
        int button = input.button == 0 ? 1 : input.button;
        out << input.number << " " << (input.holding ? 1 : 0) << " " << (input.player2 ? 1 : 0) << " " << button;
        if (i + 1 != value.inputs.size()) out << "\n";
    }
    return out.str();
}

void GUI::apply_plain(const std::string& text) {
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line)) return;
    float fps = 0.f;
    std::istringstream header(line);
    if (header >> fps && fps >= 1.f) {
        auto& logic = Logic::get();
        if (!logic.is_recording() && !logic.is_playing()) Logic::get().set_tps(fps);
    }
    std::vector<Frame> next;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        Frame input;
        int holding = 0;
        int player2 = 0;
        int button = 1;
        if (!(row >> input.number >> holding >> player2)) continue;
        row >> button;
        input.holding = holding != 0;
        input.player2 = player2 != 0;
        input.button = button == 0 ? 1 : button;
        if (input.number < 0) input.number = 0;
        next.push_back(input);
    }
    Logic::get().inputs = std::move(next);
    Logic::get().poses.clear();
    Logic::get().input_ids.clear();
    Logic::get().mark_inputs_changed();
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps();
    Logic::get().autosave_replay();
}

void GUI::draw_edit_fields(bool enabled, float column) {
    auto& inputs = Logic::get().inputs;
    if (!enabled) ImGui::BeginDisabled();
    int amt = enabled ? GUI::get().selected_count() : 0;
    if (amt > 1) ImGui::Text("%i Inputs Selected", amt);
    else {
        int shown = enabled ? GUI::get().selected + 1 : 0;
        ImGui::Text("%s Input Selected", GUI::get().ordinal(std::max(shown, 0)).c_str());
    }
    float frame_label = ImGui::CalcTextSize("Frame").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 4.f, column - frame_label));
    int frame = enabled ? GUI::get().draft.number : 0;
    bool down = enabled ? GUI::get().draft.holding : false;
    bool player2 = enabled ? GUI::get().draft.player2 : false;
    int before_frame = frame;
    static std::vector<int> frames_at_edit_start;
    static int selected_at_edit_start = 0;
    ImGui::PushID(fmt::format("{}:{}:{}", GUI::get().selected_id, GUI::get().anchor_id, GUI::get().selected_id != 0 ? -1 : GUI::get().selected).c_str());
    bool frame_edited = EchoUI::InputInt("Frame", &frame);
    bool edit_started = ImGui::IsItemActivated();
    ImGui::PopID();
    int edit_first = 0, edit_last = 0;
    if (enabled && edit_started && selection_span(edit_first, edit_last)) {
        frames_at_edit_start.clear();
        for (int i = edit_first; i <= edit_last; ++i)
            frames_at_edit_start.push_back(inputs[(size_t)i].number);
        selected_at_edit_start = before_frame;
    }
    if (frame_edited && enabled) {
        if (selection_span(edit_first, edit_last) && (int)frames_at_edit_start.size() == edit_last - edit_first + 1) {
            int delta = std::max(frame, 0) - selected_at_edit_start;
            rewrite_selected([&](Frame& input) {
                size_t at = (size_t)(&input - inputs.data() - edit_first);
                input.number = std::max(frames_at_edit_start[at] + delta, 0);
            });
        }
        else {
            int delta = std::max(frame, 0) - before_frame;
            rewrite_selected([&](Frame& input) {
                input.number = std::max(input.number + delta, 0);
            });
        }
    }
    if (EchoUI::Checkbox("Down", &down) && enabled)
        rewrite_selected([&](Frame& input) {
            input.holding = down;
        });
    ImGui::SameLine();
    if (EchoUI::Checkbox("Player 2", &player2) && enabled)
        rewrite_selected([&](Frame& input) {
            input.player2 = player2;
        });
    const char* button_names[] = { "Jump", "Left", "Right" };
    int button = enabled ? GUI::get().draft.button : JUMP;
    if (button == 0) button = JUMP;
    int button_sel = button == LEFT ? 1 : button == RIGHT ? 2 : 0;
    float button_label = ImGui::CalcTextSize("Button").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(GUI::get().combo_box_width(button_names, 3), column - button_label));
    if (EchoUI::Combo("Button", &button_sel, button_names, 3) && enabled) {
        int next = button_sel == 1 ? LEFT : button_sel == 2 ? RIGHT : JUMP;
        rewrite_selected([&](Frame& input) {
            input.button = next;
        });
    }
    int first = 0, last = 0;
    bool span = GUI::get().selection_span(first, last);
    float move_w = std::max(GUI::get().label_width("Move Up"), GUI::get().label_width("Move Down"));
    if (EchoUI::Button("Move Up", ImVec2(move_w, 0.f)) && enabled && span && first > 0)
        move_selected(-1);
    ImGui::SameLine();
    if (EchoUI::Button("Move Down", ImVec2(move_w, 0.f)) && enabled && span && last + 1 < (int)(inputs.size()))
        move_selected(1);
    if (!enabled) ImGui::EndDisabled();
}

void GUI::show_cps_range(int first, int last, bool player2) {
    if (last < first) std::swap(first, last);
    mark_first = first;
    mark_last = last;
    mark_player = player2 ? 1 : 0;
    show_editor_p1 = !player2;
    show_editor_p2 = player2;
    auto_scroll = false;
    plain = false;
    page = "Editor";
    scroll_to_mark = true;
    Logic::get().identify_inputs();
    select_input(-1);
    const auto& inputs = Logic::get().inputs;
    for (int i = 0; i < (int)(inputs.size()); ++i) {
        if (!in_cps_mark(inputs[i])) continue;
        select_input(i);
        break;
    }
}

bool GUI::shift_selecting() const {
    if (ImGui::GetIO().KeyShift) return true;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) return true;
    if (auto* keys = CCKeyboardDispatcher::get()) return keys->getShiftKeyPressed();
    return false;
}

void GUI::select_input(int index, bool range) {
    auto& inputs = Logic::get().inputs;
    if (index < 0 || index >= (int)(inputs.size())) {
        selected = -1;
        selected_id = 0;
        anchor = -1;
        anchor_id = 0;
        return;
    }
    if (Logic::get().input_id((size_t)index) == 0) Logic::get().identify_inputs(true);
    selected = index;
    selected_id = Logic::get().input_id((size_t)index);
    draft = inputs[index];
    if (!range || anchor < 0 || anchor >= (int)(inputs.size())) {
        anchor = selected;
        anchor_id = selected_id;
    }
}

void GUI::follow_selection() {
    auto& logic = Logic::get();
    auto& inputs = logic.inputs;
    auto find = [&](unsigned long long id, int fallback) {
        if (id != 0) {
#ifdef GEODE_IS_WINDOWS
            if (fallback >= 0 && fallback < (int)(inputs.size()) &&
                logic.input_id((size_t)fallback) == id)
                return fallback;
#endif
            for (int i = 0; i < (int)(inputs.size()); ++i) {
                if (logic.input_id((size_t)i) == id) return i;
            }
            return -1;
        }
        if (fallback >= 0 && fallback < (int)(inputs.size())) return fallback;
        return -1;
    };
    selected = find(selected_id, selected);
    if (selected >= 0) {
        if (selected_id == 0) selected_id = logic.input_id((size_t)selected);
        draft = inputs[selected];
    }
    else {
        selected_id = 0;
    }
    anchor = find(anchor_id, anchor_id != 0 ? anchor : selected);
    if (anchor >= 0) {
        if (anchor_id == 0) anchor_id = logic.input_id((size_t)anchor);
    }
    else {
        anchor = selected;
        anchor_id = selected_id;
    }
}

void stroke_selected_button() {
    ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    float rounding = ImGui::GetStyle().FrameRounding;
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRect(min, max, ImGui::GetColorU32(ImVec4(0.f, 0.f, 0.f, .9f)), rounding, 0, 3.f);
    draw->AddRect(ImVec2(min.x + 1.5f, min.y + 1.5f), ImVec2(max.x - 1.5f, max.y - 1.5f), ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, .96f)), rounding, 0, 1.5f);
}

void GUI::editor() {
    auto& logic = Logic::get();
    auto& inputs = logic.inputs;
    Logic::get().identify_inputs();
    GUI::get().follow_selection();
    if (GUI::get().selected >= 0 && !GUI::get().editor_shows(inputs[GUI::get().selected]))
        GUI::get().select_input(-1);

    const ImGuiStyle& edit_style = ImGui::GetStyle();
    float list_w = GUI::get().label_width("Release Right at 0000000") + edit_style.ScrollbarSize + edit_style.WindowPadding.x * 2.f + edit_style.ChildBorderSize * 2.f;
    float edit_pad = edit_style.WindowPadding.x * 2.f + edit_style.ChildBorderSize * 2.f;
    float pair_w = std::max(GUI::get().label_width("Add Input"), GUI::get().label_width("Delete Inputs"));
    pair_w = std::max(pair_w, std::max(GUI::get().label_width("Move Up"), GUI::get().label_width("Move Down")));
    float edit_row = pair_w * 2.f + edit_style.ItemSpacing.x;
    float check_w = ImGui::GetFrameHeight() + edit_style.ItemInnerSpacing.x + ImGui::CalcTextSize("Auto Scroll To Frame").x;
    float sort_w = ImGui::GetFrameHeight() + edit_style.ItemInnerSpacing.x + ImGui::CalcTextSize("Sort Automatically").x + edit_style.ItemSpacing.x + ImGui::CalcTextSize("(?)").x;
    float edit_single = std::max(GUI::get().label_width("Set to Current Frame"), std::max(check_w, sort_w));
    float edit_inner = std::max(edit_single, edit_row), edit_w = edit_inner + edit_pad;
    float card_em = Logic::get().docked_menu ? 21.f : 18.f;
    bool editor_shown = EchoUI::BeginTileOfCards("Input Editor", card_em * 2.f + 1.2f);
    if (editor_shown) {
        if (!Logic::get().docked_menu)
            card_em = (ImGui::GetContentRegionAvail().x - edit_style.ItemSpacing.x) * .5f / ImGui::GetFontSize();
        EchoUI::BeginCardRow("editor-0");
        bool list_shown = EchoUI::BeginCard("Inputs", card_em);
        if (list_shown) {
            list_w = ImGui::GetContentRegionAvail().x;
            const char* players[] = { "Both", "Player 1", "Player 2" };
            int show_sel = GUI::get().pair_index(GUI::get().show_editor_p1, GUI::get().show_editor_p2);
            std::string tps_label = logic.rates.empty() ? "TPS Changes" : fmt::format("TPS Changes - {}", logic.rates.size());
            EchoUI::Row("Show");
            if (EchoUI::Combo("##editor_show", &show_sel, players, 3))
                GUI::get().apply_pair(show_sel, GUI::get().show_editor_p1, GUI::get().show_editor_p2);
            if (EchoUI::Button(tps_label.c_str(), ImVec2(list_w, 0.f)))
                ImGui::OpenPopup("TPS Changes###editor_tps");
            if (EchoUI::BeginModal("TPS Changes###editor_tps", 16.f)) {
                if (logic.rates.empty()) {
                    ImGui::TextUnformatted("No TPS changes");
                }
                else {
                    float line_h = ImGui::GetTextLineHeightWithSpacing();
                    ImGui::BeginChild("##editor-tps", ImVec2(ImGui::GetFontSize() * 16.f, line_h * std::min((int)(logic.rates.size()), 12) + ImGui::GetStyle().WindowPadding.y * 2.f), ImGuiChildFlags_Borders);
                    for (int i = 0; i < (int)(logic.rates.size()); ++i) {
                        const auto& change = logic.rates[(size_t)i];
                        std::string label = fmt::format("Frame {}  {:.2f}##rate-{}", change.frame, change.fps, i);
                        if (EchoUI::Selectable(label.c_str(), false, 0, ImVec2(0.f, line_h))) {
                            GUI::get().mark_first = change.frame;
                            GUI::get().mark_last = change.frame;
                            GUI::get().mark_player = -1;
                            GUI::get().scroll_to_mark = true;
                            GUI::get().auto_scroll = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::EndChild();
                }
                if (EchoUI::Button("Close")) ImGui::CloseCurrentPopup();
                EchoUI::EndModal();
            }

            ImGui::BeginChild("##echo-inputs", ImVec2(list_w, EchoUI::CardFill(12.f)), ImGuiChildFlags_Borders);
            if (inputs.empty() && !GUI::get().plain) {
                GUI::get().select_input(-1);
                GUI::get().scroll_to_mark = false;
                ImGui::TextUnformatted("No Inputs");
            }
            else if (GUI::get().plain) {
                GUI::get().selected = -1;
                static unsigned long long plain_version = 0;
                static size_t plain_count = 0;
                static float plain_fps = 0.f;
                bool typing = ImGui::GetActiveID() == ImGui::GetID("##echo-plain");
                bool stale = plain_version != logic.inputs_version || plain_count != inputs.size() || plain_fps != logic.macro_fps;
                bool synced = false;
                if (GUI::get().plain_text.empty() || (stale && !typing)) {
                    GUI::get().plain_text = GUI::get().format_plain(logic);
                    synced = true;
                }
                if (ImGui::InputTextMultiline("##echo-plain", &GUI::get().plain_text, ImVec2(-1.f, -1.f))) {
                    GUI::get().apply_plain(GUI::get().plain_text);
                    synced = true;
                }
                if (synced) {
                    plain_version = logic.inputs_version;
                    plain_count = inputs.size();
                    plain_fps = logic.macro_fps;
                }
            }
            else {
                bool every_row = GUI::get().show_editor_p1 && GUI::get().show_editor_p2;

                struct ShownRows {
                    const std::vector<Frame>* owner = nullptr;
                    const Frame* data = nullptr;
                    size_t count = 0;
                    unsigned long long revision = 0;
                    bool p1 = true, p2 = true;
                    std::vector<int> rows;
                };

                static ShownRows filtered;
                if (!every_row) {
                    bool kept = filtered.owner == &inputs && filtered.revision == logic.inputs_version && filtered.count <= inputs.size() &&
                                filtered.p1 == GUI::get().show_editor_p1 && filtered.p2 == GUI::get().show_editor_p2 &&
                                (filtered.count < inputs.size() || filtered.data == inputs.data());
                    if (!kept) {
                        filtered.rows.clear();
                        filtered.count = 0;
                    }
                    for (size_t i = filtered.count; i < inputs.size(); ++i) {
                        if (GUI::get().editor_shows(inputs[i])) filtered.rows.push_back((int)(i));
                    }
                    filtered = { &inputs, inputs.data(), inputs.size(), logic.inputs_version, GUI::get().show_editor_p1, GUI::get().show_editor_p2, std::move(filtered.rows) };
                }
                int rows = every_row ? (int)(inputs.size()) : (int)(filtered.rows.size());
                auto input_at = [&](int row) {
                    return every_row ? row : filtered.rows[(size_t)row];
                };
                auto row_of = [&](int i) {
                    if (every_row) return i;
                    auto at = std::lower_bound(filtered.rows.begin(), filtered.rows.end(), i);
                    return at != filtered.rows.end() && *at == i ? (int)(std::distance(filtered.rows.begin(), at)) : 0;
                };
                if (rows == 0) {
                    GUI::get().select_input(-1);
                    GUI::get().scroll_to_mark = false;
                    ImGui::TextUnformatted(GUI::get().show_editor_p1 ? "No Player 1 Inputs" : "No Player 2 Inputs");
                }
                else {
                    int frame = PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : (logic.editor_testing ? logic.step_frame : 0);
                    auto number_at = [&](int row) {
                        return inputs[(size_t)input_at(row)].number;
                    };
                    auto first_row = [&](auto&& before) {
                        int low = 0, high = rows;
                        while (low < high) {
                            int mid = low + (high - low) / 2;
                            if (before(number_at(mid))) low = mid + 1;
                            else high = mid;
                        }
                        return low;
                    };
                    int closest = input_at(0);
                    int best = std::abs(inputs[closest].number - frame);
                    if (Logic::get().inputs_sorted()) {
                        int above = first_row([&](int number) {
                            return number < frame;
                        });
                        int pick = above < rows ? above : rows - 1;
                        if (above > 0 && (above == rows || number_at(above) != frame)) {
                            int below = number_at(above - 1);
                            if (above == rows || frame - below <= number_at(above) - frame)
                                pick = first_row([&](int number) {
                                    return number < below;
                                });
                        }
                        closest = input_at(pick);
                        best = std::abs(inputs[closest].number - frame);
                    }
                    else {
                        for (int row = 0; row < rows; ++row) {
                            int i = input_at(row);
                            int diff = std::abs(inputs[i].number - frame);
                            if (diff < best) {
                                best = diff;
                                closest = i;
                            }
                        }
                    }
                    bool follow = GUI::get().auto_scroll && PlayLayer::get();
                    float row_h = ImGui::GetFrameHeightWithSpacing();
                    if (GUI::get().scroll_to_mark) {
                        int first_mark = 0;
                        for (int row = 0; row < rows; ++row) {
                            if (GUI::get().in_cps_mark(inputs[input_at(row)])) {
                                first_mark = row;
                                break;
                            }
                        }
                        ImGui::SetScrollY(std::max(0.f, first_mark * row_h - (ImGui::GetWindowHeight() - row_h) * 0.5f));
                        GUI::get().scroll_to_mark = false;
                    }
                    else if (follow) {
                        int chosen = closest;
                        int picked = GUI::get().selected;
                        bool picked_shown = GUI::get().selected_id != 0 && picked >= 0 && picked < (int)(inputs.size()) &&
                                            Logic::get().input_id((size_t)picked) == GUI::get().selected_id && GUI::get().editor_shows(inputs[picked]);
                        if (picked_shown && std::abs(inputs[picked].number - frame) == best)
                            chosen = picked;
                        // dont reselect while editing cuz it will rewrite every input xd
                        if (chosen != picked && !ImGui::IsAnyItemActive())
                            GUI::get().select_input(chosen);
                        int row = row_of(chosen);
                        float target = std::max(0.f, row * row_h - (ImGui::GetWindowHeight() - row_h) * 0.5f);
                        float now = ImGui::GetScrollY();
                        float step = 1.f - std::exp(-ImGui::GetIO().DeltaTime * 14.f * GUI::get().anim_rate());
                        ImGui::SetScrollY(std::abs(target - now) < .5f ? target : now + (target - now) * step);
                    }
                    ImGuiListClipper clipper;
                    clipper.Begin(rows, row_h);
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            int i = input_at(row);
                            bool chosen = GUI::get().selected == i;
                            bool marked = chosen || GUI::get().input_in_range(i) || GUI::get().in_cps_mark(inputs[i]);
                            const float* tint = inputs[i].player2 ? logic.editor_p2 : logic.editor_p1;
                            if (marked)
                                tint = inputs[i].player2 ? logic.editor_sel_p2 : logic.editor_sel_p1;
                            bool passed = !marked && (PlayLayer::get() || logic.editor_testing) && inputs[i].number <= frame;
                            float lit = passed ? .45f : 1.f;
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tint[0] * lit, tint[1] * lit, tint[2] * lit, tint[3]));
                            ImVec4 ink = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                            if (passed) ink.w *= .55f;
                            ImGui::PushStyleColor(ImGuiCol_Text, ink);
                            int button = inputs[i].button == 0 ? JUMP : inputs[i].button;
                            const char* action = inputs[i].holding ? "Click" : "Release";
                            if (button == LEFT)
                                action = inputs[i].holding ? "Left" : "Release Left";
                            else if (button == RIGHT)
                                action = inputs[i].holding ? "Right" : "Release Right";
                            std::string label = fmt::format("{} at {}##in{}", action, inputs[i].number, i);
                            EchoUI::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.f));
                            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                                GUI::get().select_input(i, GUI::get().shift_selecting());
                                GUI::get().auto_scroll = false;
                                GUI::get().clear_cps_mark();
                            }
                            if (GUI::get().input_in_range(i)) stroke_selected_button();
                            ImGui::PopStyleColor(2);
                        }
                    }
                }
            }
            ImGui::EndChild();
        }
        EchoUI::EndCard();

        bool edit_shown = EchoUI::BeginCard("Edit", card_em);
        if (edit_shown) {
            edit_inner = ImGui::GetContentRegionAvail().x;
            pair_w = (edit_inner - edit_style.ItemSpacing.x) * .5f;
            bool live = GUI::get().selected >= 0 && GUI::get().selected < (int)(inputs.size()) && !GUI::get().plain;
            GUI::get().draw_edit_fields(live, edit_inner);
            EchoUI::Separator();
            int frame = (PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : (logic.editor_testing ? logic.step_frame : 0)) + 1;
            ImGui::Text("Current Frame: %i", frame);
            if (!live) ImGui::BeginDisabled();
            if (EchoUI::Button("Set to Current Frame", ImVec2(edit_inner, 0.f))) {
                int delta = frame - GUI::get().draft.number;
                rewrite_selected([&](Frame& input) {
                    input.number = std::max(input.number + delta, 0);
                });
            }
            if (!live) ImGui::EndDisabled();
            EchoUI::Checkbox("Auto Scroll To Frame", &GUI::get().auto_scroll);
            if (EchoUI::Checkbox("Edit Plain Text", &GUI::get().plain)) {
                GUI::get().plain_text = GUI::get().plain ? GUI::get().format_plain(logic) : std::string();
            }
            EchoUI::Separator();
            if (EchoUI::Button("Add Input", ImVec2(pair_w, 0.f))) GUI::get().add_input(frame);
            ImGui::SameLine();
            int delete_first = 0, deleteLast = 0;
            bool can_delete = GUI::get().selection_span(delete_first, deleteLast);
            if (!can_delete) ImGui::BeginDisabled();
            if (EchoUI::Button(GUI::get().has_range() ? "Delete Inputs" : "Delete Input", ImVec2(pair_w, 0.f)) && can_delete)
                GUI::get().delete_selected();
            if (!can_delete) ImGui::EndDisabled();

            EchoUI::Separator();
            if (EchoUI::Button("Actions", ImVec2(edit_inner, 0.f)))
                ImGui::OpenPopup("Editor Actions");
            if (EchoUI::BeginModal("Editor Actions", 0.f)) {
                float gap = ImGui::GetStyle().ItemSpacing.x;
                float half = std::max(GUI::get().label_width("Random Offset"), GUI::get().label_width("Remove Player 1"));
                half = std::max(half, GUI::get().label_width("-1 Frame"));
                float row = half * 2.f + gap;
                row = std::max(row, GUI::get().label_width("Flip Presses and Releases"));
                row = std::max(row, GUI::get().label_width("Flip Player 1 and 2"));
                auto random_offset = [] {
                    GUI::get().random_offset_chosen();
                };
                const char* scope = GUI::get().has_range() ? "the selected inputs" : "the whole macro";

                GUI::get().section_label("Shift");
                float frames_label = ImGui::CalcTextSize("Frames").x + ImGui::GetStyle().ItemInnerSpacing.x;
                ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.f, row - frames_label));
                EchoUI::InputInt("Frames###offset_frames", &GUI::get().input_offset);
                if (EchoUI::Button("Offset", ImVec2(half, 0.f)))
                    offset_chosen(GUI::get().input_offset);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Moves %s by that many frames", scope);
                ImGui::SameLine();
                if (EchoUI::Button("Random Offset", ImVec2(half, 0.f))) random_offset();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Moves each input in %s by a random amount from -frames to +frames", scope);
                if (EchoUI::Button("-1 Frame", ImVec2(half, 0.f))) offset_chosen(-1);
                ImGui::SameLine();
                if (EchoUI::Button("+1 Frame", ImVec2(half, 0.f))) offset_chosen(1);

                GUI::get().section_label("Order");
                if (EchoUI::Button("Sort", ImVec2(row, 0.f))) {
                    Logic::get().sort_inputs();
                    GUI::get().sync_draft();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sorts inputs by frame");

                GUI::get().section_label("Players");
                if (EchoUI::Button("Remove Player 1", ImVec2(half, 0.f))) {
                    Logic::get().remove_player_inputs(false);
                    GUI::get().select_input(-1);
                }
                ImGui::SameLine();
                if (EchoUI::Button("Remove Player 2", ImVec2(half, 0.f))) {
                    Logic::get().remove_player_inputs(true);
                    GUI::get().select_input(-1);
                }
                if (EchoUI::Button("Flip Player 1 and 2", ImVec2(row, 0.f))) {
                    Logic::get().flip_input_players();
                    GUI::get().sync_draft();
                }

                GUI::get().section_label("Holds");
                if (EchoUI::Button("Flip Presses and Releases", ImVec2(row, 0.f))) {
                    Logic::get().flip_input_holds();
                    GUI::get().sync_draft();
                }

                EchoUI::EndModal();
            }
            if (GUI::get().checkbox_setting("Sort Automatically", logic.sort_automatically) && logic.sort_automatically) {
                Logic::get().sort_inputs();
                GUI::get().sync_draft();
            }
            ImGui::SameLine();
            EchoUI::Help();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sorts new clicks by frame when recording while playing.");
            EchoUI::Separator();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.f);
            EchoUI::DragInt("Max CPS", &logic.max_cps, 0.2f, 1, 30);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This is based on ILL CPS rules.");
            if (ImGui::IsItemDeactivatedAfterEdit()) Logic::get().save_settings_file();
            if (EchoUI::Button("CPS Breaks", ImVec2(edit_inner, 0.f)))
                ImGui::OpenPopup("CPS Breaks###cps_breaks");
            if (EchoUI::BeginModal("CPS Breaks###cps_breaks", 22.f)) {
                ImGui::Text("Highest CPS: %s", logic.highest_cps.c_str());
                ImGui::Text("Current CPS: %.2f", logic.current_cps);
                EchoUI::Separator();
                int p1 = (int)(logic.cps_notes_p1.size());
                int total = p1 + (int)(logic.cps_notes_p2.size());
                if (total == 0) {
                    ImGui::TextUnformatted("No CPS breaks");
                }
                else {
                    float line_h = ImGui::GetTextLineHeightWithSpacing();
                    ImGui::BeginChild("##cps-notes", ImVec2(ImGui::GetFontSize() * 22.f, line_h * std::min(total, 12) + ImGui::GetStyle().WindowPadding.y * 2.f), ImGuiChildFlags_Borders);
                    ImGuiListClipper clipper;
                    clipper.Begin(total, line_h);
                    while (clipper.Step()) {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                            bool second = i >= p1;
                            const auto& note = second ? logic.cps_notes_p2[i - p1] : logic.cps_notes_p1[i];
                            std::string span = note.first == note.last
                                                   ? std::to_string(note.first)
                                                   : std::to_string(note.first) + "-" + std::to_string(note.last);
                            std::string label = std::string(second ? "P2  " : "P1  ") + span + "  " + note.text + "##cps-" + std::to_string(i);
                            if (EchoUI::Selectable(label.c_str(), false, 0, ImVec2(0.f, line_h))) {
                                GUI::get().show_cps_range(note.first, note.last, second);
                                ImGui::CloseCurrentPopup();
                            }
                            if (ImGui::IsItemHovered()) {
                                const char* tip = "Over 16 inputs in a second";
                                if (note.text == "Rule 2") tip = "Over 3 inputs in a frame";
                                else if (note.text == "Rule 3") tip = "5+ clicks faster than 48/s";
                                ImGui::SetTooltip("%s", tip);
                            }
                        }
                    }
                    ImGui::EndChild();
                }
                EchoUI::EndModal();
            }
        }
        EchoUI::EndCard();
        EchoUI::EndCardRow();
    }
    EchoUI::EndTileOfCards();
}
