// the input editor: every input in a list that can be selected, moved, offset & deleted
#include "GUI/gui.hpp"
#include "Hack/label_kinds.hpp"
#include <imgui-cocos.hpp>
#include <imgui.h>
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

float GUI::columnWidth(std::initializer_list<char const*> labels) {
    float width = 0.f;
    for (char const* label : labels)
        width = std::max(width, GUI::get().labelWidth(label));
    return width;
}

std::string GUI::ordinal(int number) {
    int teen = number % 100;
    if (teen >= 11 && teen <= 13) {
        return std::to_string(number) + "th";
    }
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

std::string GUI::format_plain(Session const& value) {
    std::ostringstream out;
    out << value.fps << "\n";
    for (size_t i = 0; i < value.inputs.size(); ++i) {
        auto const& input = value.inputs[i];
        int button = input.button == 0 ? 1 : input.button;
        out << input.number << " " << (input.holding ? 1 : 0) << " " << (input.player2 ? 1 : 0) << " " << button;
        if (i + 1 != value.inputs.size()) {
            out << "\n";
        }
    }
    return out.str();
}

void GUI::apply_plain(std::string const& text) {
    std::istringstream lines(text);
    std::string line;
    if (!std::getline(lines, line))
        return;
    float fps = 0.f;
    std::istringstream header(line);
    if (header >> fps && std::isfinite(fps) && fps >= 1.f) {
        auto& logic = Logic::get();
        if (!(logic).is_recording() && !(logic).is_playing()) {
            Logic::get().set_tps(fps);
        }
    }
    std::vector<Frame> next;
    while (std::getline(lines, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        Frame input;
        int holding = 0;
        int player2 = 0;
        int button = 1;
        if (!(row >> input.number >> holding >> player2)) {
            continue;
        }
        row >> button;
        input.holding = holding != 0;
        input.player2 = player2 != 0;
        input.button = button == 0 ? 1 : button;
        if (input.number < 0) {
            input.number = 0;
        }
        next.push_back(input);
    }
    Logic::get().inputs = std::move(next);
    Logic::get().poses.clear();
    Logic::get().input_ids.clear();
    Logic::get().touch_inputs();
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps(Logic::get());
    Logic::get().autosave_replay();
}

void GUI::draw_edit_fields(bool enabled, float column) {
    auto& inputs = Logic::get().inputs;
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    int amt = enabled ? GUI::get().selected_count() : 0;
    if (amt > 1)
        ImGui::Text("%i Inputs Selected", amt);
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
    ImGui::PushID(fmt::format("{}:{}:{}", GUI::get().selected_id, GUI::get().anchor_id, GUI::get().selected_id != 0 ? -1 : GUI::get().selected).c_str());
    bool frame_edited = eui::InputInt("Frame", &frame);
    ImGui::PopID();
    if (frame_edited && enabled) {
        int delta = std::max(frame, 0) - before_frame;
        rewrite_selected([&](Frame& input) {
            input.number = std::max(input.number + delta, 0);
        });
    }
    if (eui::Checkbox("Down", &down) && enabled)
        rewrite_selected([&](Frame& input) {
            input.holding = down;
        });
    ImGui::SameLine();
    if (eui::Checkbox("Player 2", &player2) && enabled)
        rewrite_selected([&](Frame& input) {
            input.player2 = player2;
        });
    char const* button_names[] = { "Jump", "Left", "Right" };
    int button = enabled ? GUI::get().draft.button : (int)(PlayerButton::Jump);
    if (button == 0) {
        button = (int)(PlayerButton::Jump);
    }
    int button_sel = button == (int)(PlayerButton::Left) ? 1 : button == (int)(PlayerButton::Right) ? 2
                                                                                                                          : 0;
    float button_label = ImGui::CalcTextSize("Button").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(GUI::get().combo_box_width(button_names, 3), column - button_label));
    if (eui::Combo("Button", &button_sel, button_names, 3) && enabled) {
        int next = button_sel == 1 ? (int)(PlayerButton::Left) : button_sel == 2 ? (int)(PlayerButton::Right)
                                                                                            : (int)(PlayerButton::Jump);
        rewrite_selected([&](Frame& input) {
            input.button = next;
        });
    }
    int first = 0, last = 0;
    bool span = GUI::get().selection_span(first, last);
    float move_w = std::max(GUI::get().labelWidth("Move Up"), GUI::get().labelWidth("Move Down"));
    if (eui::Button("Move Up", ImVec2(move_w, 0.f)) && enabled && span && first > 0) {
        move_selected(-1);
    }
    ImGui::SameLine();
    if (eui::Button("Move Down", ImVec2(move_w, 0.f)) && enabled && span && last + 1 < (int)(inputs.size())) {
        move_selected(1);
    }
    if (!enabled) {
        ImGui::EndDisabled();
    }
}

void GUI::show_cps_range(int first, int last, bool player2) {
    log::debug("show cps range");
    if (last < first) {
        std::swap(first, last);
    }
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
    auto const& inputs = Logic::get().inputs;
    for (int i = 0; i < (int)(inputs.size()); ++i) {
        if (!in_cps_mark(inputs[i])) {
            continue;
        }
        select_input(i);
        break;
    }
}

bool GUI::shift_selecting() const {
    // TODO
    return {};
}

void GUI::select_input(int index, bool range) {
    // TODO
}

void GUI::follow_selection() {
    // TODO
}

void stroke_selected_button() {
    // port this
}

void GUI::editor() {
    // TODO
}
