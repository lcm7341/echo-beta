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

        static float input_tps = -1.f;
        if (input_tps < 0.f) {
            input_tps = logic.fps;
        }
        static float input_fps = -1.f;
        if (input_fps < 0.f) {
            input_fps = logic.playback_fps >= 1.f ? logic.playback_fps : 240.f;
        }
        float set_em = 3.2f;
        eui::RowWith("TPS", set_em, "Physics steps per second.");
        eui::DragFloat("##tps", &input_tps, 1.f, 1.f, FLT_MAX, "%.2f");
        bool can_set_tps = !logic.is_playing() || logic.is_recording() || logic.playback_tps_changes;
        ImGui::BeginDisabled(!can_set_tps);
        float set_w = eui::RowRight(set_em);
        if (eui::Button("Set##tps", ImVec2(set_w, 0.f))) {
            input_tps = std::clamp(input_tps, 1.f, FLT_MAX);
            Logic::get().set_tps(input_tps);
            input_tps = logic.fps;
        }
        ImGui::EndDisabled();
        eui::RowWith("FPS", set_em, "Frames drawn per second.");
        eui::DragFloat("##fps", &input_fps, 1.f, 1.f, FLT_MAX, "%.2f");
        set_w = eui::RowRight(set_em);
        if (eui::Button("Set##fps", ImVec2(set_w, 0.f))) {
            input_fps = std::clamp(input_fps, 1.f, FLT_MAX);
            GUI::get().set_playback_fps(input_fps);
        }
        if (toggle("Lock TPS", logic.lock_delta)) {
            GUI::get().drop_carry();
        }
        tip("Forces your TPS value to this. Turn this on for maximum accuracy, but it shouldn't be necessary.");
        eui::RowHalf();
        if (toggle("Real Time", logic.real_time_mode)) {
            GUI::get().drop_carry();
        }
        tip("Keeps the game at full speed when it lags instead of slowing it down.");
        toggle("Audio Speedhack", logic.audio_speedhack);
        tip("Makes the audio follow your speedhack.");
        eui::RowHalf();
        toggle("Sync Audio", logic.sync_audio);
        tip("Syncs the audio if the game lags");
        if (eui::Gear("##sync-audio-settings")) {
            open_sync_audio = true;
        }
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("replay-1");
    if (eui::BeginCard("Replay", card_width())) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (eui::InputTextWithHint("###replay_name", "Replay name", &logic.macro_name)) {
            logic.macro_name_typed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            logic.macro_path.clear();
        }
        ImGui::TextDisabled("%i inputs   %s TPS%s", (int)(logic.inputs.size()), fmt::format("{:g}", logic.fps).c_str(), !logic.macro_ldm_known ? "" : logic.macro_ldm ? "   LDM on" : "   LDM off");
        float half = eui::RowButton(2);
        if (eui::Button("Save", ImVec2(half, 0.f))) {
            Logic::get().write_file();
        }
        GUI::get().offer_rebind("key-save");
        ImGui::SameLine();
        if (eui::Button("Load", ImVec2(half, 0.f))) {
            Logic::get().pick_replay();
        }
        GUI::get().offer_rebind("key-load");
        ImGui::BeginDisabled(busy);
        if (eui::Button("Merge In", ImVec2(half, 0.f))) {
            GUI::get().open_merge_picker();
        }
        ImGui::EndDisabled();
        tip("Merges another replay's inputs into this one. Both replays need the same TPS.");
        ImGui::SameLine();
        ImGui::BeginDisabled(logic.inputs.empty());
        if (eui::Button("Clear", ImVec2(half, 0.f))) {
            want_reset = true;
        }
        ImGui::EndDisabled();
        tip("Removes every input from the current replay.");
        toggle("No Overwrite", logic.no_overwrite);
        GUI::get().offer_rebind("key-no-overwrite", "Saves as a new file instead of overwriting the current replay.");
        if (eui::Gear("##overwrite-settings")) {
            open_overwrite = true;
        }
        eui::Separator();
        ImGui::BeginDisabled(!logic.backstep && !logic.best_frame_running);
#ifdef GEODE_IS_MOBILE
        if (eui::Button(logic.best_frame_running ? "Stop Searching" : "Stop On Best Frame", ImVec2(-FLT_MIN, 0.f))) {
            Logic::get().request_best_frame();
        }
        tip("Finds the frame where your next click is the safest and stops there. Requires Backstep.");
#else
        if (eui::Button(logic.best_frame_running ? "Stop Searching" : "Find Best Frame", ImVec2(-FLT_MIN, 0.f))) {
            Logic::get().request_best_frame();
        }
        GUI::get().offer_rebind("find-best-frame", "Finds the frame where your next click is the safest and steps to it. Requires Backstep.");
        if (toggle("Click That Frame", logic.best_frame_click)) {
            persist();
        }
        tip("Automatically clicks on the best frame instead of only stopping there.");
        eui::RowHalf();
        if (toggle("Keep Going", logic.best_frame_auto)) {
            persist();
        }
        tip("Keeps finding the next best frame after each click until none survive.");
#endif
        ImGui::EndDisabled();
        eui::Row("Search range", "How many steps ahead to search. Limited by how far Backstep can go back.");
        if (eui::DragInt("##best-frame-range", &logic.best_frame_range, 1.f, 1, 2000, "%d steps")) {
            logic.best_frame_range = std::clamp(logic.best_frame_range, 1, 2000);
        }
        saved_on_release();
    }
    eui::EndCard();
    if (eui::BeginCard("Frame Advance", card_width())) {
        bool advancing = logic.frame_advance;
        if (eui::Checkbox("Frame Advance", &advancing)) {
            GUI::get().set_frame_advance(advancing);
        }
        GUI::get().offer_rebind("toggle-advance");
        float step_w = eui::RowRight(4.6f);
        if (eui::Button("Step", ImVec2(step_w, 0.f))) {
            Logic::get().advance_one_frame();
        }
        GUI::get().offer_rebind("step-frame");
        eui::Row("Hold delay", "How long Step is held before it starts repeating.");
        int hold = (int)(logic.advance_hold_ms);
        if (eui::DragInt("##advance-hold", &hold, 1.f, 0, 1000, "%d ms")) {
            logic.advance_hold_ms = static_cast<unsigned>(std::max(hold, 0));
        }
        saved_on_release();
        eui::Row("Held speed", "Percent of your TPS, scaled with speedhack. 100% at 1x will advance you in real time.");
        int held = (int)(std::lround(logic.advance_speed));
        if (eui::DragInt("##advance-speed", &held, 1.f, 1, 100, "%d%%")) {
            logic.advance_speed = (float)(std::clamp(held, 1, 200));
        }
        saved_on_release();
        eui::Separator();
        if (toggle("Backstep", logic.backstep) && !logic.backstep) {
            Logic::get().clear_backsteps();
        }
        tip("Allows you to step backwards while recording. Higher TPS saves less often and replays your inputs from the last save.");
        float back_w = eui::RowRight(4.6f);
        if (eui::Button("Back", ImVec2(back_w, 0.f))) {
            Logic::get().step_back(PlayLayer::get(), 1);
        }
        GUI::get().offer_rebind("step-back");
        eui::Row("Saved states", "How many states to keep as a percent of your TPS. 100% at 240 TPS keeps 240.");
        if (eui::DragInt("##backstep-saves", &logic.backstep_percent, 1.f, 1, 200, "%d%%")) {
            logic.backstep_percent = std::clamp(logic.backstep_percent, 1, 200);
        }
        saved_on_release();
    }
    eui::EndCard();
    eui::EndCardRow();

    if (open_sync_audio) {
        ImGui::OpenPopup("Sync Audio Settings");
    }
    if (eui::BeginModal("Sync Audio Settings", 20.f)) {
        eui::Row("Threshold", "How far the audio can drift before it gets synced.");
        if (eui::DragInt("##sync-threshold", &logic.sync_audio_threshold, 5.f, 20, 2000, "%d ms")) {
            logic.sync_audio_threshold = std::clamp(logic.sync_audio_threshold, 20, 2000);
        }
        saved_on_release();
        eui::EndModal();
    }
    if (open_overwrite) {
        ImGui::OpenPopup("Overwrite Settings");
    }
    if (eui::BeginModal("Overwrite Settings", 20.f)) {
        eui::Row("Rename format", "Added to the name instead of overwriting. # is the number, so _# saves Replay_1.");
        eui::InputText("##rename-format", &logic.rename_format);
        saved_on_release();
        std::string shown = logic.rename_format.empty() ? std::string("_#") : logic.rename_format;
        if (shown.find('#') == std::string::npos) {
            shown += "#";
        }
        shown.replace(shown.find('#'), 1, "1");
        ImGui::TextDisabled("Replay%s.echo", shown.c_str());
        eui::EndModal();
    }
    if (want_reset) {
        ImGui::OpenPopup("Clear Replay");
    }
    if (eui::BeginModal("Clear Replay", 16.f)) {
        ImGui::TextUnformatted("Remove every input from this replay?");
        float half = eui::RowButton(2);
        if (eui::Button("Clear", ImVec2(half, 0.f))) {
            GUI::get().reset_macro();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (eui::Button("Keep", ImVec2(half, 0.f))) {
            ImGui::CloseCurrentPopup();
        }
        eui::EndModal();
    }
}

void GUI::tools() {
    auto& logic = Logic::get();
    bool open_autoclicker = false;

    eui::BeginCardRow("assists-0");
    if (eui::BeginCard("Safety and Auto Input", card_width())) {
        if (quad_row("Noclip", "##noclip", logic.noclip_p1, logic.noclip_p2)) {
            GUI::get().persist();
        }
        toggle("Safe Mode", logic.safeMode);
        tip("Removes the ability to make progress when using hacks. You can still make progress if no hacks are on, though.");
        if (toggle("Prevent Death", logic.pause_before_kill)) {
            Logic::get().reset_traj();
        }
        GUI::get().offer_rebind("key-pause-before-kill", "Stops on the frame before you die while recording. Works without Show Trajectory.");
        eui::Separator();
        toggle("Autoclicker", logic.autoclicker);
        GUI::get().offer_rebind("key-autoclick");
        if (eui::Gear("##autoclicker-settings")) {
            open_autoclicker = true;
        }
        if (toggle("Trajectory Autoclicker", logic.traj_autoclicker)) {
            Logic::get().reset_traj();
        }
        tip("Automatically clicks or releases when the next frame would kill you. Works without Show Trajectory.");
    }
    eui::EndCard();
    if (eui::BeginCard("Two Players", card_width())) {
        toggle("Click Both Players", logic.click_both);
        GUI::get().offer_rebind("key-click-both");
        toggle("Swap Player Input", logic.swap_players);
        GUI::get().offer_rebind("key-swap");
        if (quad_row("Inverse inputs", "##inverse", logic.inverse_p1, logic.inverse_p2)) {
            GUI::get().persist();
        }
        char const* follow[] = { "Off", "P2 follows P1", "P1 follows P2" };
        eui::Row("Follow", "For two player levels, the following player will copy the followed player after that many frames or milliseconds.");
        if (eui::Combo("##follow", &logic.follow_mode, follow, 3)) {
            GUI::get().persist();
        }
        if (logic.follow_mode != 0) {
            eui::RowWith("Delay", 5.f);
            if (eui::DragFloat("##follow-delay", &logic.follow_delay, logic.follow_delay_ms ? 1.f : 0.25f, 0.f, 100000.f, logic.follow_delay_ms ? "%.1f" : "%.0f")) {
                logic.follow_delay = std::max(logic.follow_delay, 0.f);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far behind the follower is. %d steps at the current TPS.", Logic::get().follow_steps());
            }
            persist_on_release();
            char const* units[] = { "frames", "ms" };
            int unit = logic.follow_delay_ms ? 1 : 0;
            ImGui::SetNextItemWidth(eui::RowRight(5.f));
            if (eui::Combo("##follow-unit", &unit, units, 2)) {
                logic.follow_delay_ms = unit == 1;
                GUI::get().persist();
            }
        }
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("assists-1");
    if (eui::BeginCard("Orb Assists", card_width())) {
        ImGui::TextDisabled("While recording");
        if (toggle("Click Dash Orbs", logic.click_dash_orbs)) {
            Logic::get().reset_traj();
        }
        GUI::get().offer_rebind("key-click-dash-orbs", "Automatically swift clicks stacked dash orbs. Does not affect playback.");
        if (toggle("Click Black Orbs", logic.click_black_orbs)) {
            Logic::get().reset_traj();
        }
        GUI::get().offer_rebind("key-click-black-orbs", "Automatically swift clicks black orbs as UFO. Does not affect playback.");
        if (toggle("Black Orb on Pad", logic.pad_black_orb)) {
            Logic::get().reset_traj();
        }
        GUI::get().offer_rebind("key-pad-black-orb", "Automatically swift clicks black orbs when hitting a pad.");
        if (toggle("Blue Orb on Pad", logic.pad_blue_orb)) {
            Logic::get().reset_traj();
        }
        GUI::get().offer_rebind("key-pad-blue-orb", "Automatically swift clicks blue orbs when hitting a pad.");
    }
    eui::EndCard();
    if (eui::BeginCard("Physics", card_width())) {
        bool locked = logic.is_recording() || logic.is_playing();
        bool unrounded = logic.shown_unrounded();
        ImGui::BeginDisabled(locked);
        if (eui::Checkbox("Velocity Unrounding", &unrounded)) {
            logic.set_unrounded(unrounded);
            GUI::get().persist();
        }
        ImGui::EndDisabled();
        tip("Stops GD rounding the player's y velocity to three decimals (it's chizz's Velocity Unrounding mod). CHANGES PHYSICS!");
        {
            auto* level = PlayLayer::get() ? PlayLayer::get()->m_level : nullptr;
            bool ldm = logic.macro_ldm_known ? logic.macro_ldm : level && level->m_lowDetailModeToggled;
            ImGui::BeginDisabled(locked);
            ImGui::EndDisabled();
            tip("Saves LDM setting in the macro, as it sometimes breaks if not set correctly.");
        }
        toggle("Maintain Gravity", logic.maintain_gravity);
        tip("Makes the wave control as if gravity was always normal while recording.");
        if (logic.maintain_gravity && GUI::get().two_player_dual()) {
            toggle("Same Gravity", logic.same_gravity);
            tip("Ensures that Maintain Gravity keeps both players on the same gravity.");
        }
    }
    eui::EndCard();
    eui::EndCardRow();

    if (open_autoclicker) {
        ImGui::OpenPopup("Autoclicker");
    }
    if (eui::BeginModal("Autoclicker", 18.f)) {
        toggle("Enabled", logic.autoclicker);
        char const* players[] = { "Player 1", "Player 2", "Both" };
        int who = logic.autoclick_p1 && logic.autoclick_p2 ? 2 : logic.autoclick_p2 ? 1
                                                                                    : 0;
        eui::Row("Player");
        if (eui::Combo("##autoclick-player", &who, players, 3)) {
            logic.autoclick_p1 = who != 1;
            logic.autoclick_p2 = who != 0;
            GUI::get().persist();
        }
        eui::Row("Click length", "0 makes every click a swift click.");
        int press = logic.auto_click.press_interval;
        if (eui::DragInt("##autoclick-press", &press, 1.f, 0, 10000, "%d frames")) {
            logic.auto_click.press_interval = std::max(press, 0);
        }
        persist_on_release();
        eui::Row("Swift clicks", "Adds extra clicks/releases on the same frame for each autoclick.");
        int swifts = logic.auto_click.swifts;
        if (eui::DragInt("##autoclick-swifts", &swifts, .1f, 0, 8, "%d extra")) {
            logic.auto_click.swifts = std::clamp(swifts, 0, 8);
        }
        persist_on_release();
        eui::Row("Release length");
        int release = logic.auto_click.release_interval;
        if (eui::DragInt("##autoclick-release", &release, 1.f, 1, 10000, "%d frames")) {
            logic.auto_click.release_interval = std::max(release, 1);
        }
        persist_on_release();
        toggle("Turn off by itself", logic.autoclick_auto_disable);
        ImGui::BeginDisabled(!logic.autoclick_auto_disable);
        eui::Row("After");
        int disable_in = logic.auto_click.disable_in;
        if (eui::DragInt("##autoclick-disable", &disable_in, 1.f, 1, 10000, "%d frames")) {
            logic.auto_click.disable_in = std::max(disable_in, 1);
        }
        saved_on_release();
        ImGui::EndDisabled();
        eui::EndModal();
    }
}

void GUI::draw_visuals() {
    log::debug("draw visuals");
    auto& logic = Logic::get();
    bool open_hitboxes = false, open_traj = false, open_layout = false, open_labels = false;

    eui::BeginCardRow("visuals-0");
    if (eui::BeginCard("Overlays", card_width())) {
        toggle("Hitboxes", logic.show_hitboxes);
        GUI::get().offer_rebind("key-hitboxes");
        if (eui::Gear("##hitbox-settings")) {
            open_hitboxes = true;
        }
        toggle("Trajectory", logic.show_traj);
        GUI::get().offer_rebind("key-trajectory");
        if (eui::Gear("##trajectory-settings")) {
            open_traj = true;
        }
        toggle("Layout Mode", logic.layout_mode);
        GUI::get().offer_rebind("key-layout");
        if (eui::Gear("##layout-settings")) {
            open_layout = true;
        }
    }
    eui::EndCard();
    if (eui::BeginCard("Player", card_width())) {
        if (quad_row("Hide player", "##hide-player", logic.hide_p1, logic.hide_p2)) {
            if (logic.traj_follow_hide_player) {
                Logic::get().reset_traj();
            }
            GUI::get().persist();
        }
        toggle("Fix Rotation Bug", logic.fix_tps_rotation);
        tip("Fixes gamemode rotation on high TPS without affecting hitboxes.");
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("visuals-1");
    if (eui::BeginCard("On-Screen Labels", card_width())) {
        auto order = ordered_labels(logic);
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto const& label = *order[i];
            std::string title = label.title;
            if (title.starts_with("Show ")) {
                title = title.substr(5);
            }
            if (i % 2) {
                eui::RowHalf();
            }
            ImGui::PushID(label.key);
            if (eui::Checkbox(title.c_str(), &(logic.*label.shown))) {
                save();
            }
            ImGui::PopID();
        }
        if (eui::Button("Position, Size and Font", ImVec2(-FLT_MIN, 0.f))) {
            open_labels = true;
        }
        tip("Changes where each label sits, its size, opacity, font and order.");
    }
    eui::EndCard();
    if (eui::BeginCard("Level Effects", card_width())) {
        toggle("Disable Shake", logic.disable_shakes);
        GUI::get().offer_rebind("key-shakes");
        toggle("Disable Death Effect", logic.disable_death_effect);
        if (toggle("No Mirror Portals", logic.no_mirror_portals)) {
            GUI::get().apply_no_mirror();
        }
        tip("Stops mirror portals from flipping the level.");
    }
    eui::EndCard();
    eui::EndCardRow();

    if (open_hitboxes) {
        ImGui::OpenPopup("Hitboxes");
    }
    if (eui::BeginModal("Hitboxes", 26.f)) {
        draw_hitbox_settings();
        eui::EndModal();
    }
    if (open_traj) {
        ImGui::OpenPopup("Trajectory");
    }
    if (eui::BeginModal("Trajectory", 20.f)) {
        draw_traj_settings();
        eui::EndModal();
    }
    if (open_layout) {
        ImGui::OpenPopup("Layout Mode");
    }
    if (eui::BeginModal("Layout Mode", 18.f)) {
        toggle("Use Regular Background", logic.layout_use_regular_bg);
        tip("Uses the level's actual background");
        if (eui::ColorEdit3("Background color", logic.backgroundColor)) {
            save();
        }
        if (eui::ColorEdit3("Ground color", logic.blocks_color)) {
            save();
        }
        eui::EndModal();
    }
    if (open_labels) {
        ImGui::OpenPopup("On-Screen Labels");
    }
    if (eui::BeginModal("On-Screen Labels", 34.f)) {
        draw_label_settings();
        eui::EndModal();
    }
}

void GUI::draw_macro() {
    auto& logic = Logic::get();

    eui::BeginCardRow("library-0");
    if (eui::BeginCard("Convert", card_width())) {
        static int format = 0;
        char const* formats[] = { "JSON", "Plain Text", "Silicate", "TCBot", "GDR" };
        eui::Row("Format");
        eui::Combo("##convert-format", &format, formats, 5);
        float half = eui::RowButton(2);
        if (eui::Button("Export", ImVec2(half, 0.f))) {
            Logic::get().export_kind(formats[format]);
        }
        tip("Converts the current replay to the converted folder");
        ImGui::SameLine();
        if (eui::Button("Import", ImVec2(half, 0.f))) {
            Logic::get().import_replays();
        }
        tip("Import a replay of the selected type to convert to Echo");
        if (eui::Button("Convert Folder", ImVec2(half, 0.f))) {
            Logic::get().convert_legacy_echoes();
        }
        tip("Converts every replay in the replays folder to .echo. Old Echo replays are renamed to .old.echo.");
        ImGui::SameLine();
        if (eui::Button("Open Folder", ImVec2(half, 0.f))) {
            geode::utils::file::openFolder(Logic::get().save_root() / "replays");
        }
    }
    eui::EndCard();
    if (eui::BeginCard("Saving", card_width())) {
        eui::Row("New file suffix", "Added to the name when saving as a new file.");
        eui::InputText("##rename-format", &logic.rename_format);
        saved_on_release();
        if (eui::Checkbox("Save Debug Samples", &logic.save_debug)) {
            logic.format = logic.save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
            GUI::get().persist();
        }
        tip("Stores the player's position with every input.");
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("library-1");
    if (eui::BeginCard("This Replay", card_width())) {
        int total = (int)(logic.recording_seconds);
        int clicks = 0;
        for (auto const& input : logic.inputs)
            clicks += input.holding ? 1 : 0;
        auto stat = [](char const* label, std::string const& text) {
            eui::Row(label);
            ImGui::TextUnformatted(text.c_str());
        };
        stat("Recording time", fmt::format("{:02}:{:02}:{:02}", total / 3600, (total % 3600) / 60, total % 60));
        stat("Attempts", fmt::format("{}", logic.attempts));
        stat("Clicks", fmt::format("{}", clicks));
        stat("Level seed", logic.lvl_seed_known && PlayLayer::get() ? fmt::format("{}", logic.lvl_seed) : std::string("not set"));
        if (toggle("Override seed", logic.seed_override)) {
            GUI::get().apply_seed_override();
        }
        tip("Overrides the game seed to this value");
        ImGui::BeginDisabled(!logic.seed_override);
        eui::Row("Seed");
        eui::InputScalar("##seed-override", ImGuiDataType_U64, &logic.seed_override_value);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            GUI::get().apply_seed_override();
            save();
        }
        ImGui::EndDisabled();
    }
    eui::EndCard();
    if (eui::BeginCard("Sequence", card_width())) {
        ImGui::TextDisabled("Play several replays back to back.");
        draw_sequence_card();
    }
    eui::EndCard();
    eui::EndCardRow();
}