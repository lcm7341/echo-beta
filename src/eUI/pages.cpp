// whats on each page of the menu: replay, assists, visuals, library, clickbot, render & settings
#include "GUI/gui.hpp"
#include "GUI/logo_texture.hpp"
#include "Hack/label_kinds.hpp"

#include <imgui-cocos.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <Geode/utils/web.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace geode::prelude;

namespace {
    float card_width() {
        return Logic::get().docked_menu ? 21.f : 18.f;
    }

    char const* const PLAYERS[] = { "Both", "Player 1", "Player 2" };
    char const* const QUAD[] = { "Off", "Player 1", "Player 2", "Both" };
    ImVec4 const RECORD_RED(1.f, .36f, .44f, 1.f);

    void tip(char const* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", text);
    }

    bool toggle(char const* label, bool& field) {
        return GUI::get().checkbox_setting(label, field);
    }

    void save() {
        Logic::get().save_settings_file();
    }

    void saved_on_release() {
        if (ImGui::IsItemDeactivatedAfterEdit())
            save();
    }

    void persist_on_release() {
        if (ImGui::IsItemDeactivatedAfterEdit())
            GUI::get().persist();
    }

    bool quad_row(char const* label, char const* id, bool& p1, bool& p2, char const* hint = nullptr) {
        eui::Row(label, hint);
        int idx = GUI::get().quad_index(p1, p2);
        if (!eui::Combo(id, &idx, QUAD, 4))
            return false;
        GUI::get().apply_quad(idx, p1, p2);
        return true;
    }

    void draw_hitbox_settings() {
        auto& logic = Logic::get();
        toggle("Fill Hitboxes", logic.fill_hitboxes);
        eui::RowHalf();
        toggle("Outline Only", logic.hitbox_outline_only);
        toggle("Hide Activated Objects", logic.hide_activated_objects);
        toggle("Follow Hide Player", logic.hitboxes_follow_hide_player);
        tip("Will hide the hitboxes of the hidden player.");
        ImGui::BeginDisabled(logic.hitboxes_follow_hide_player);
        if (quad_row("Hide hitboxes", "##hide-hitboxes", logic.hide_hitbox_p1, logic.hide_hitbox_p2))
            save();
        ImGui::EndDisabled();
        eui::Row("Thickness");
        GUI::get().drag_float_setting("##hitbox-thickness", logic.hitbox_thickness, 0.01f, -1000000.f, 1000000.f, "%.3f");
        eui::Row("Fill opacity");
        eui::DragFloat("##hitbox-fill", &logic.hitbox_fill_opacity, 0.01f, 0.f, 1.f, "%.2f");
        saved_on_release();

        eui::SectionLabel("Trail");
        toggle("Show Trail", logic.hitbox_trail);
        eui::RowHalf();
        toggle("At visual FPS", logic.hitbox_trail_visual);
        tip("Displays one trail hitbox per displayed frame");
        eui::Row("Length", "How many frames the trail and the click and release boxes stay on screen");
        int trail = logic.trail_length;
        if (eui::DragInt("##trail-length", &trail, 1.f, 0, (int)(logic.fps) * 10, trail <= 0 ? "TPS" : "%d"))
            logic.trail_length = std::max(trail, 0);
        saved_on_release();
        eui::Row("Opacity");
        if (eui::DragFloat("##trail-opacity", &logic.hitbox_trail_opacity, 0.01f, 0.f, 1.f, "%.2f"))
            logic.hitbox_trail_opacity = std::clamp(logic.hitbox_trail_opacity, 0.f, 1.f);
        saved_on_release();

        eui::SectionLabel("Shown and colors");
        auto shape = [](char const* label, bool& enabled, float* tint) {
            ImGui::PushID(label);
            if (eui::ColorEdit4("##color", tint, ImGuiColorEditFlags_AlphaBar))
                save();
            ImGui::SameLine();
            toggle(label, enabled);
            ImGui::PopID();
        };
        shape("Player box", logic.hitbox_player, logic.hitbox_outer_color);
        eui::RowHalf();
        shape("Inner box", logic.hitbox_inner, logic.hitbox_inner_color);
        shape("Rotated box", logic.hitbox_rotated, logic.hitbox_rotated_color);
        eui::RowHalf();
        shape("Player circle", logic.hitbox_circle, logic.hitbox_circle_color);
        shape("Solids", logic.hitbox_solid, logic.hitbox_solid_color);
        eui::RowHalf();
        shape("Slopes", logic.hitbox_slope, logic.hitbox_slope_color);
        shape("Hazards", logic.hitbox_hazard, logic.hitbox_hazard_color);
        eui::RowHalf();
        shape("Orbs, pads, portals", logic.hitbox_interact, logic.hitbox_interact_color);
        shape("Click", logic.hitbox_click, logic.hitbox_click_color);
        eui::RowHalf();
        if (eui::ColorEdit4("Release", logic.hitbox_release_color, ImGuiColorEditFlags_AlphaBar))
            save();
    }

    void draw_traj_settings() {
        auto& logic = Logic::get();
        if (toggle("Moving Object Support", logic.traj_moving_objects))
            Logic::get().reset_traj();
        tip("Moves moving objects along with the trajectory. Turning this off is faster but less accurate.");
        if (toggle("Follow Hide Player", logic.traj_follow_hide_player))
            Logic::get().reset_traj();
        tip("Will hide the trajectory of the hidden player.");
        ImGui::BeginDisabled(logic.traj_follow_hide_player);
        if (quad_row("Hide trajectory", "##hide-trajectory", logic.hide_traj_p1, logic.hide_traj_p2)) {
            Logic::get().reset_traj();
            save();
        }
        ImGui::EndDisabled();
        eui::Row("Length");
        float seconds = logic.traj_seconds;
        if (eui::DragFloat("##trajectory-length", &seconds, 0.05f, 0.1f, 4.f, "%.2f s"))
            logic.traj_seconds = std::clamp(seconds, 0.1f, 4.f);
        saved_on_release();
        eui::Row("Width");
        GUI::get().drag_float_setting("##trajectory-width", logic.traj_width, 0.01f, 0.05f, 0.5f, "%.2f");

        eui::SectionLabel("Lines");
        ImGui::SameLine();
        eui::Help();
        tip("Player 2 uses the inverted colors.");
        auto line = [](char const* label, bool& enabled, float* tint) {
            ImGui::PushID(label);
            if (eui::ColorEdit4("##color", tint, ImGuiColorEditFlags_AlphaBar))
                save();
            ImGui::SameLine();
            toggle(label, enabled);
            ImGui::PopID();
        };
        line("Hold", logic.traj_hold, logic.traj_color);
        line("Release", logic.traj_release, logic.traj_release_color);
        line("Swift", logic.traj_swift, logic.traj_swift_color);
    }

    void draw_label_settings() {
        auto& logic = Logic::get();
        if (toggle("Auto Position", logic.labels_auto))
            save();
        tip("Automatically stacks labels from a corner in the order below.");
        if (logic.labels_auto) {
            eui::Row("Corner");
            if (eui::Combo("##label-corner", &logic.label_corner, LABEL_CORNERS, IM_ARRAYSIZE(LABEL_CORNERS)))
                save();
        }
        eui::Row("Size", "Scales all labels.");
        eui::DragFloat("##label-size", &logic.labelScale, 0.01f, 0.25f, 3.f, "%.2fx");
        saved_on_release();
        eui::Row("Font");
        int font = label_font_index(logic.labelFont);
        auto const& fonts = label_font_names();
        if (eui::BeginCombo("##label-font", fonts[static_cast<size_t>(font)].c_str())) {
            for (int i = 0; i < (int)(fonts.size()); ++i) {
                if (eui::Selectable(fonts[static_cast<size_t>(i)].c_str(), i == font)) {
                    logic.labelFont = label_font_file(i);
                    save();
                }
            }
            ImGui::EndCombo();
        }
        eui::Row("Percent decimals");
        if (eui::InputInt("##percent-accuracy", &logic.percent_accuracy))
            logic.percent_accuracy = std::clamp(logic.percent_accuracy, 0, 6);
        saved_on_release();

        eui::SectionLabel("Labels");
        ImGui::TextDisabled(logic.labels_auto ? "Drag a label by its grip to reorder it." : "Auto Position stacks the labels in this order.");
        auto order = ordered_labels(logic);
        float font1 = ImGui::GetFontSize();
        float title_x = ImGui::GetStyle().WindowPadding.x + font1 * 3.6f;
        float field_w = font1 * 3.4f;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        int columns = logic.labels_auto ? 2 : 4;
        float fields_x = ImGui::GetStyle().WindowPadding.x + eui::em(34.f) - (field_w + gap) * (float)(columns) + gap;
        ImGui::Dummy(ImVec2(0.f, 0.f));
        float column = fields_x;
        auto heading = [&](char const* text) {
            ImGui::SameLine(column);
            ImGui::TextDisabled("%s", text);
            column += field_w + gap;
        };
        if (!logic.labels_auto) {
            heading("X");
            heading("Y");
        }
        heading("Size");
        heading("Opacity");
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto const& label = *order[i];
            auto& place = logic.*label.place;
            ImGui::PushID(label.key);
            eui::Selectable("=##grip", false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(font1 * .9f, 0.f));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive()) {
                float dy = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y;
                float row_h = ImGui::GetFrameHeightWithSpacing();
                if (std::abs(dy) >= row_h) {
                    int step = dy < 0.f ? -1 : 1;
                    std::ptrdiff_t next = static_cast<std::ptrdiff_t>(i) + step;
                    if (next >= 0 && next < static_cast<std::ptrdiff_t>(order.size())) {
                        move_label(logic, label, step);
                        ImGui::ResetMouseDragDelta();
                    }
                }
            }
            if (ImGui::IsItemDeactivated())
                save();
            ImGui::SameLine();
            if (eui::Checkbox("##shown", &(logic.*label.shown)))
                save();
            ImGui::SameLine(title_x);
            std::string title = label.title;
            if (title.starts_with("Show "))
                title = title.substr(5);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(title.c_str());
            ImGui::SameLine(fields_x);
            ImGui::PushItemWidth(field_w);
            if (!logic.labels_auto) {
                eui::DragFloat("##x", &place.x, 1.f, 0.f, 0.f, "%.0f");
                saved_on_release();
                ImGui::SameLine();
                eui::DragFloat("##y", &place.y, 1.f, 0.f, 0.f, "%.0f");
                saved_on_release();
                ImGui::SameLine();
            }
            eui::DragFloat("##scale", &place.scale, 0.01f, 0.05f, 10.f, "%.2fx");
            saved_on_release();
            ImGui::SameLine();
            int opacity = place.opacity;
            if (eui::DragInt("##opacity", &opacity, 1.f, 0, 100, "%d%%"))
                place.opacity = std::clamp(opacity, 0, 100);
            saved_on_release();
            ImGui::PopItemWidth();
            ImGui::PopID();
        }
    }

    void draw_sequence_card() {
        auto& logic = Logic::get();
        static int selected = -1;
        static int amount = 1;
        if (toggle("Enable Sequence", logic.sequence_enabled))
            logic.replay_index = 0;
        eui::RowHalf();
        if (toggle("Random Order", logic.sequence_random)) {
            logic.replay_index = 0;
            logic.sequence_order.clear();
        }
        tip("Plays the queue in a random order");
        ImGui::BeginChild("##sequence-list", ImVec2(-FLT_MIN, eui::em(9.f)), ImGuiChildFlags_Borders);
        if (logic.sequence.empty())
            ImGui::TextDisabled("The queue is empty");
        for (unsigned i = 0; i < logic.sequence.size(); ++i) {
            ImGui::PushID((int)(i));
            auto row = fmt::format("{}.  {}", i + 1, logic.sequence[i].name);
            if (eui::Selectable(row.c_str(), selected == (int)(i)))
                selected = (int)(i);
            ImGui::PopID();
        }
        ImGui::EndChild();
        bool picked = selected >= 0 && selected < (int)(logic.sequence.size());
        float third = eui::RowButton(3);
        if (eui::Button("Add", ImVec2(third, 0.f)))
            GUI::get().open_macro_picker(true, amount);
        ImGui::SameLine();
        ImGui::BeginDisabled(!picked);
        if (eui::Button("Remove", ImVec2(third, 0.f)) && picked) {
            logic.sequence.erase(logic.sequence.begin() + selected);
            selected = -1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(logic.sequence.empty());
        if (eui::Button("Clear", ImVec2(third, 0.f))) {
            logic.sequence.clear();
            logic.sequence_order.clear();
            logic.replay_index = 0;
            selected = -1;
            logic.message = "Sequence cleared";
        }
        ImGui::EndDisabled();
        eui::Row("Copies per add");
        eui::InputInt("##sequence-amount", &amount);
        if (amount < 1)
            amount = 1;
    }

    void draw_video_shape(bool& open_save_preset) {
        auto& logic = Logic::get();

        struct Shape {
            char const* name;
            int width, height, fps, mbps;
        };

        static constexpr Shape DEFAULTS[] = {
            { "720p 30", 1280, 720, 30, 8 },
            { "720p 60", 1280, 720, 60, 12 },
            { "1080p 60", 1920, 1080, 60, 20 },
            { "1080p 120", 1920, 1080, 120, 30 },
            { "1440p 60", 2560, 1440, 60, 30 },
            { "4K 60", 3840, 2160, 60, 50 },
        };
        static constexpr int SIZES[][2] = { { 640, 360 }, { 854, 480 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
        static constexpr int RATES[] = { 24, 30, 48, 50, 60, 72, 90, 100, 120, 144, 165, 180, 240, 360 };
        auto matches = [&](int width, int height, int fps) {
            return logic.render_width == width && logic.render_height == height && logic.render_fps == fps;
        };
        auto use = [&](int width, int height, int fps, int bitrate) {
            logic.render_width = width;
            logic.render_height = height;
            logic.render_fps = fps;
            logic.render_bitrate = bitrate;
            save();
        };

        std::string shown = "Custom";
        int mine = -1;
        for (auto const& preset : DEFAULTS) {
            if (matches(preset.width, preset.height, preset.fps))
                shown = preset.name;
        }
        for (int i = 0; i < (int)(logic.render_presets.size()); ++i) {
            auto const& preset = logic.render_presets[static_cast<size_t>(i)];
            if (!matches(preset.width, preset.height, preset.fps))
                continue;
            shown = preset.name;
            mine = i;
        }
#ifdef GEODE_IS_MOBILE
        bool forced = false;
        ImGui::BeginDisabled(forced);
        if (toggle("Match Screen Shape", logic.render_screen_shape))
            save();
        ImGui::EndDisabled();
        tip(forced ? "iOS always renders at the screen's shape: the game cannot draw at another." : "On: the video is as wide as your screen's shape at the chosen height (1080 high on a phone comes out wider than 1920).\nOff: the size as chosen.");
#endif
        bool shaped = logic.render_uses_screen_shape();
        auto sized = [&](std::string const& name, int width, int height) {
            if (!shaped)
                return name;
            return fmt::format("{}  ({} x {})", name, logic.render_width_for(width, height), height);
        };
        for (auto const& preset : DEFAULTS) {
            if (shown == preset.name)
                shown = sized(preset.name, preset.width, preset.height);
        }
        if (mine >= 0)
            shown = sized(shown, logic.render_width, logic.render_height);
        eui::RowWith("Preset", mine >= 0 ? 4.4f : 0.f);
        if (eui::BeginCombo("##render-preset", shown.c_str())) {
            for (auto const& preset : DEFAULTS) {
                auto label = sized(preset.name, preset.width, preset.height) + "##" + preset.name;
                if (eui::Selectable(label.c_str(), matches(preset.width, preset.height, preset.fps)))
                    use(preset.width, preset.height, preset.fps, preset.mbps * 1000000);
            }
            if (!logic.render_presets.empty())
                eui::Separator();
            for (int i = 0; i < (int)(logic.render_presets.size()); ++i) {
                auto preset = logic.render_presets[static_cast<size_t>(i)];
                ImGui::PushID(i);
                auto label = sized(preset.name, preset.width, preset.height);
                if (eui::Selectable(label.c_str(), mine == i))
                    use(preset.width, preset.height, preset.fps, preset.bitrate);
                ImGui::PopID();
            }
            eui::Separator();
            if (eui::Selectable("Save current as..."))
                open_save_preset = true;
            ImGui::EndCombo();
        }
        if (mine >= 0) {
            float delete_w = eui::RowRight(4.4f);
            if (eui::Button("Delete", ImVec2(delete_w, 0.f))) {
                logic.render_presets.erase(logic.render_presets.begin() + mine);
                save();
            }
        }

        int size_index = -1;
        for (int i = 0; i < IM_ARRAYSIZE(SIZES); ++i) {
            if (logic.render_width == SIZES[i][0] && logic.render_height == SIZES[i][1])
                size_index = i;
        }
        static bool custom_size = false;
        if (size_index < 0)
            custom_size = true;
        if (!custom_size) {
            eui::Row("Size");
            auto text = fmt::format("{} x {}", logic.render_width_for(logic.render_width, logic.render_height), logic.render_height);
            if (eui::DragInt("##render-size", &size_index, .04f, 0, IM_ARRAYSIZE(SIZES) - 1, text.c_str())) {
                size_index = std::clamp(size_index, 0, IM_ARRAYSIZE(SIZES) - 1);
                logic.render_width = SIZES[size_index][0];
                logic.render_height = SIZES[size_index][1];
            }
            saved_on_release();
        }
        else {
            int width = logic.render_width, height = logic.render_height;
            eui::Row("Width", shaped ? "Set by the screen's shape and the height." : nullptr);
            if (shaped) {
                int fitted = logic.render_width_for(width, height);
                ImGui::BeginDisabled();
                eui::DragInt("##render-width", &fitted, 0.f, fitted, fitted, "%d px");
                ImGui::EndDisabled();
            }
            else {
                if (eui::DragInt("##render-width", &width, 2.f, 2, 3840, "%d px"))
                    logic.render_width = std::clamp(width, 2, 7680);
                saved_on_release();
            }
            eui::Row("Height");
            if (eui::DragInt("##render-height", &height, 2.f, 2, 2160, "%d px"))
                logic.render_height = std::clamp(height, 2, 4320);
            saved_on_release();
        }
        eui::Checkbox("Custom size", &custom_size);
        tip("Lets you type a custom width and height.");

        int rate_index = 0;
        for (int i = 0; i < IM_ARRAYSIZE(RATES); ++i) {
            if (std::abs(RATES[i] - logic.render_fps) < std::abs(RATES[rate_index] - logic.render_fps))
                rate_index = i;
        }
        eui::Row("Frame rate");
        auto rate = fmt::format("{} FPS", logic.render_fps);
        if (eui::DragInt("##render-fps", &rate_index, .04f, 0, IM_ARRAYSIZE(RATES) - 1, rate.c_str()))
            logic.render_fps = RATES[std::clamp(rate_index, 0, IM_ARRAYSIZE(RATES) - 1)];
        saved_on_release();
        float mbps = (float)(logic.render_bitrate) / 1000000.f;
        eui::Row("Bitrate");
        if (eui::DragFloat("##render-bitrate", &mbps, 0.1f, 0.f, 100.f, mbps <= 0.f ? "Lossless" : "%.1f Mbps"))
            logic.render_bitrate = mbps <= 0.f ? 0 : std::max((int)(mbps * 1000000.f), 1000);
        saved_on_release();
    }

    void draw_click_types(bool& softs, float& soft_ms, bool& hards, float& hard_seconds, bool& micros, int& micro_ms) {
        toggle("Soft Clicks", softs);
        eui::RowHalf();
        toggle("Hard Clicks", hards);
        toggle("Micro Clicks", micros);
        eui::Row("Micro up to");
        int micro = micro_ms;
        if (eui::DragInt("##micro-ms", &micro, 1.f, 0, 100, "%d ms"))
            micro_ms = std::max(micro, 0);
        saved_on_release();
        eui::Row("Soft up to");
        GUI::get().drag_float_setting("##soft-ms", soft_ms, 1.f, 0.f, 500.f, "%.0f ms");
        eui::Row("Hard from");
        GUI::get().drag_float_setting("##hard-seconds", hard_seconds, 0.01f, 0.f, 5.f, "%.2f s");
        ImGui::TextDisabled("Micro  0 to %i ms", micro_ms);
        ImGui::TextDisabled("Soft  %i to %.0f ms", micro_ms, soft_ms);
        ImGui::TextDisabled("Regular  %.0f ms to %.2f s", soft_ms, hard_seconds);
        ImGui::TextDisabled("Hard  %.2f s and up", hard_seconds);
    }
}

void GUI::main() {
    auto& logic = Logic::get();
    bool busy = logic.is_recording() || logic.is_playing();
    bool want_reset = false, open_overwrite = false, open_sync_audio = false;

    eui::BeginCardRow("replay-0");
    if (eui::BeginCard("Session", card_width())) {
        float half = eui::RowButton(2);
        ImVec2 big(half, eui::em(2.3f));
        if (eui::ActionButton(logic.is_recording() ? "Stop Recording" : "Record", eui::Icon::Record, logic.is_recording(), big, RECORD_RED)) {
            Logic::get().toggle_recording();
        }
        GUI::get().offer_rebind("toggle-record");
        ImGui::SameLine();
        if (eui::ActionButton(logic.is_playing() ? "Stop Playing" : "Play", eui::Icon::Play, logic.is_playing(), big)) {
            Logic::get().toggle_playing();
        }
        GUI::get().offer_rebind("toggle-play");
        ImGui::BeginDisabled(busy);
        eui::Row("Record", "Which player's inputs are recorded.");
        int record = GUI::get().pair_index(logic.record_p1, logic.record_p2);
        if (eui::Combo("##record-players", &record, PLAYERS, 3)) {
            GUI::get().apply_pair(record, logic.record_p1, logic.record_p2);
            GUI::get().persist();
        }
        eui::Row("Play back", "Which player's inputs are played.");
        int play = GUI::get().pair_index(logic.play_p1, logic.play_p2);
        if (eui::Combo("##play-players", &play, PLAYERS, 3)) {
            GUI::get().apply_pair(play, logic.play_p1, logic.play_p2);
            GUI::get().persist();
        }
        ImGui::EndDisabled();
        toggle("Ignore Inputs During Playback", logic.ignore_inputs);
        GUI::get().offer_rebind("key-ignore");
        if (eui::Button("Reset Level", ImVec2(half, 0.f))) {
            if (auto* level = PlayLayer::get()) {
                level->resetLevel();
            }
        }
        GUI::get().offer_rebind("key-reset");
        ImGui::SameLine();
        if (eui::Button("Close Menu", ImVec2(half, 0.f))) {
            GUI::get().begin_close();
        }
        GUI::get().offer_rebind("open-menu");
    }
    eui::EndCard();
    if (eui::BeginCard("Speed and Timing", card_width())) {
        eui::Row("Speed");
        static float dragged = 1.f;
        ImGuiID speed_id = ImGui::GetID("##speed");
        bool typing = ImGui::TempInputIsActive(speed_id);
        bool dragging = ImGui::GetActiveID() == speed_id && !typing;
        if (!dragging) {
            dragged = logic.speedhack;
        }
        float stepped = std::clamp(std::round(dragged / .05f) * .05f, .01f, 5.f);
        auto shown = fmt::format("{:.2f}x", stepped);
        if (eui::DragFloat("##speed", &dragged, 0.005f, 0.01f, 5.f, dragging ? shown.c_str() : "%.3fx", dragging ? ImGuiSliderFlags_NoRoundToFormat : ImGuiSliderFlags_None)) {
            GUI::get().setSpeed(typing ? dragged : std::clamp(std::round(dragged / .05f) * .05f, .01f, 5.f), false);
        }
        GUI::get().offer_rebind("key-speedhack");
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            GUI::get().setSpeed(logic.speedhack, true);
        }