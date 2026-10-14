// whats on each page of the menu: replay, assists, visuals, library, clickbot, render & settings
#include "Hooks/opt.hpp"
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
#ifdef GEODE_IS_MOBILE
        // a finger needs more than a mouse does
        float grip_w = font1 * 1.9f;
#else
        float grip_w = font1 * .9f;
#endif
        float title_x = ImGui::GetStyle().WindowPadding.x + font1 * 2.7f + grip_w;
        float field_w = font1 * 3.4f;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        int columns = logic.labels_auto ? 2 : 4;
        // the list of labels is the only part that scrolls. it gets whatever height the screen has left under the settings
        // above, so the window itself never has to scroll (it did, all of it, & on a phone that was the only way to reach anything)
        float inset = ImGui::GetStyle().WindowPadding.x;
        float row_tall = ImGui::GetFrameHeightWithSpacing();
        float list_needs = row_tall * (float)(order.size());
        float room = ImGui::GetIO().DisplaySize.y * .95f - ImGui::GetCursorPosY() - row_tall - inset * 2.f;
        float list_tall = std::min(list_needs, std::max(room, row_tall * 3.f));
        float bar = list_needs > list_tall + 1.f ? ImGui::GetStyle().ScrollbarSize : 0.f;
        float fields_x = inset + eui::em(34.f) - (field_w + gap) * (float)(columns) + gap - bar;
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
        ImGui::BeginChild("##label-list", ImVec2(eui::em(34.f), list_tall));
        // a child starts at 0, the window started at its padding
        title_x -= inset;
        fields_x -= inset;
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto const& label = *order[i];
            auto& place = logic.*label.place;
            ImGui::PushID(label.key);
            eui::Selectable("=##grip", false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(grip_w, 0.f));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            // the grip is dragged up & down, which is also how a touch scrolls the page. while its held the page stays put
            if (ImGui::IsItemActive())
                GUI::get().scroll_hold = ImGui::GetItemID();
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
        ImGui::EndChild();
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
#ifdef GEODE_IS_IOS
        bool forced = true;
#else
        bool forced = false;
#endif
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
    bool want_reset = false, open_overwrite = false, open_sync_audio = false, open_real_time = false;

    eui::BeginCardRow("replay-0");
    if (eui::BeginCard("Session", card_width())) {
        float half = eui::RowButton(2);
        ImVec2 big(half, eui::em(2.3f));
        if (eui::ActionButton(logic.is_recording() ? "Stop Recording" : "Record", eui::Icon::Record, logic.is_recording(), big, RECORD_RED))
            Logic::get().toggle_recording();
        GUI::get().offer_rebind("toggle-record");
        ImGui::SameLine();
        if (eui::ActionButton(logic.is_playing() ? "Stop Playing" : "Play", eui::Icon::Play, logic.is_playing(), big))
            Logic::get().toggle_playing();
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
            if (auto* level = PlayLayer::get())
                level->resetLevel();
        }
        GUI::get().offer_rebind("key-reset");
        ImGui::SameLine();
        if (eui::Button("Close Menu", ImVec2(half, 0.f)))
            GUI::get().begin_close();
        GUI::get().offer_rebind("open-menu");
    }
    eui::EndCard();
    if (eui::BeginCard("Speed and Timing", card_width())) {
        eui::Row("Speed");
        static float dragged = 1.f;
        ImGuiID speed_id = ImGui::GetID("##speed");
        bool typing = ImGui::TempInputIsActive(speed_id);
        bool dragging = ImGui::GetActiveID() == speed_id && !typing;
        if (!dragging)
            dragged = logic.speedhack;
        float stepped = std::clamp(std::round(dragged / .05f) * .05f, .01f, 5.f);
        auto shown = fmt::format("{:.2f}x", stepped);
        if (eui::DragFloat("##speed", &dragged, 0.005f, 0.01f, 5.f, dragging ? shown.c_str() : "%.3fx", dragging ? ImGuiSliderFlags_NoRoundToFormat : ImGuiSliderFlags_None)) {
            GUI::get().setSpeed(typing ? dragged : std::clamp(std::round(dragged / .05f) * .05f, .01f, 5.f), false);
        }
        GUI::get().offer_rebind("key-speedhack");
        if (ImGui::IsItemDeactivatedAfterEdit())
            GUI::get().setSpeed(logic.speedhack, true);

        static float input_tps = -1.f;
        if (input_tps < 0.f)
            input_tps = logic.fps;
        static float input_fps = -1.f;
        if (input_fps < 0.f)
            input_fps = logic.playback_fps >= 1.f ? logic.playback_fps : 240.f;
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
        if (toggle("Lock FPS", logic.lock_delta))
            GUI::get().drop_carry();
        tip("Moves the level exactly one frame for every frame drawn, essentially smooth fix.");
        eui::RowHalf();
        // lock fps never has time left over for real time to catch up on
        ImGui::BeginDisabled(logic.lock_delta);
        if (toggle("Real Time", logic.real_time_mode))
            GUI::get().drop_carry();
        tip("Keeps the game at full speed when it lags instead of slowing it down.");
        if (eui::Gear("##real-time-settings"))
            open_real_time = true;
        ImGui::EndDisabled();
        toggle("Audio Speedhack", logic.audio_speedhack);
        tip("Makes the audio follow your speedhack.");
        eui::RowHalf();
        toggle("Sync Audio", logic.sync_audio);
        tip("Syncs the audio if the game lags");
        if (eui::Gear("##sync-audio-settings"))
            open_sync_audio = true;
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("replay-1");
    if (eui::BeginCard("Replay", card_width())) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (eui::InputTextWithHint("###replay_name", "Replay name", &logic.macro_name))
            logic.macro_name_typed = true;
        if (ImGui::IsItemDeactivatedAfterEdit())
            logic.macro_path.clear();
        ImGui::TextDisabled("%i inputs   %s TPS%s", (int)(logic.inputs.size()), fmt::format("{:g}", logic.fps).c_str(), !logic.macro_ldm_known ? "" : logic.macro_ldm ? "   LDM on" : "   LDM off");
        float half = eui::RowButton(2);
        // better saving info
        bool just_saved = std::chrono::steady_clock::now() - logic.saved_at < std::chrono::seconds(1);
        ImGui::BeginDisabled(just_saved);
        if (eui::Button(just_saved ? "Saved###save-replay" : "Save###save-replay", ImVec2(half, 0.f)))
            Logic::get().write_file();
        ImGui::EndDisabled();
        GUI::get().offer_rebind("key-save");
        ImGui::SameLine();
        if (eui::Button("Load", ImVec2(half, 0.f)))
            Logic::get().pick_replay();
        GUI::get().offer_rebind("key-load");
        ImGui::BeginDisabled(busy);
        if (eui::Button("Merge In", ImVec2(half, 0.f)))
            GUI::get().open_merge_picker();
        ImGui::EndDisabled();
        tip("Merges another replay's inputs into this one. Both replays need the same TPS.");
        ImGui::SameLine();
        ImGui::BeginDisabled(logic.inputs.empty());
        if (eui::Button("Clear", ImVec2(half, 0.f)))
            want_reset = true;
        ImGui::EndDisabled();
        tip("Removes every input from the current replay.");
        toggle("No Overwrite", logic.no_overwrite);
        GUI::get().offer_rebind("key-no-overwrite", "Saves as a new file instead of overwriting the current replay.");
        if (eui::Gear("##overwrite-settings"))
            open_overwrite = true;
        eui::Separator();
        ImGui::BeginDisabled(!logic.backstep && !logic.best_frame_running);
#ifdef GEODE_IS_MOBILE
        if (eui::Button(logic.best_frame_running ? "Stop Searching" : "Stop On Best Frame", ImVec2(-FLT_MIN, 0.f)))
            Logic::get().request_best_frame();
        tip("Finds the frame where your next click is the safest and stops there. Requires Backstep.");
#else
        if (eui::Button(logic.best_frame_running ? "Stop Searching" : "Find Best Frame", ImVec2(-FLT_MIN, 0.f)))
            Logic::get().request_best_frame();
        GUI::get().offer_rebind("find-best-frame", "Finds the frame where your next click is the safest and steps to it. Requires Backstep.");
        if (toggle("Click That Frame", logic.best_frame_click))
            persist();
        tip("Automatically clicks on the best frame instead of only stopping there.");
        eui::RowHalf();
        ImGui::BeginDisabled(!logic.best_frame_click);
        if (toggle("Keep Going", logic.best_frame_auto))
            persist();
        ImGui::EndDisabled();
        tip("Keeps finding the next best frame after each click until none survive.");
#endif
        ImGui::EndDisabled();
        eui::Row("Search range", "How many steps ahead to search. Limited by how far Backstep can go back.");
        if (eui::DragInt("##best-frame-range", &logic.best_frame_range, 1.f, 1, 2000, "%d steps"))
            logic.best_frame_range = std::clamp(logic.best_frame_range, 1, 2000);
        saved_on_release();
    }
    eui::EndCard();
    if (eui::BeginCard("Frame Advance", card_width())) {
        bool advancing = logic.frame_advance;
#ifdef GEODE_IS_MOBILE
        // since resume/pause added, unticking fa takes the bar away
        advancing = advancing || logic.frame_bar_open;
#endif
        if (eui::Checkbox("Frame Advance", &advancing)) {
            GUI::get().set_frame_advance(advancing);
            logic.frame_bar_open = advancing;
        }
        GUI::get().offer_rebind("toggle-advance");
#ifdef GEODE_IS_MOBILE
        // the fa bar is hidden while the menu is open
        // same row as step so the card (& the whole menu) doesnt get taller
        if (logic.frame_advance || logic.frame_bar_open) {
            float both = eui::RowRight(9.6f);
            float each = (both - ImGui::GetStyle().ItemSpacing.x) * .5f;
            if (eui::Button(logic.frame_advance ? "Resume" : "Pause", ImVec2(each, 0.f)))
                GUI::get().set_frame_advance(!logic.frame_advance);
            tip("Keeps playing, or stops again. The bar stays on screen.");
            ImGui::SameLine();
            if (eui::Button("Step", ImVec2(each, 0.f)))
                Logic::get().advance_one_frame();
        }
        else {
            float step_w = eui::RowRight(4.6f);
            if (eui::Button("Step", ImVec2(step_w, 0.f)))
                Logic::get().advance_one_frame();
        }
#else
        float step_w = eui::RowRight(4.6f);
        if (eui::Button("Step", ImVec2(step_w, 0.f)))
            Logic::get().advance_one_frame();
#endif
        GUI::get().offer_rebind("step-frame");
        eui::Row("Hold delay", "How long Step is held before it starts repeating.");
        int hold = (int)(logic.advance_hold_ms);
        if (eui::DragInt("##advance-hold", &hold, 1.f, 0, 1000, "%d ms"))
            logic.advance_hold_ms = static_cast<unsigned>(std::max(hold, 0));
        saved_on_release();
        eui::Row("Held speed", "Percent of your TPS, scaled with speedhack. 100% at 1x will advance you in real time.");
        int held = (int)(std::lround(logic.advance_speed));
        if (eui::DragInt("##advance-speed", &held, 1.f, 1, 100, "%d%%"))
            logic.advance_speed = (float)(std::clamp(held, 1, 200));
        saved_on_release();
        eui::Separator();
        if (toggle("Backstep", logic.backstep) && !logic.backstep)
            Logic::get().clear_backsteps();
        tip("Allows you to step backwards while recording. Higher TPS saves less often and replays your inputs from the last save.");
        float back_w = eui::RowRight(4.6f);
        if (eui::Button("Back", ImVec2(back_w, 0.f)))
            Logic::get().step_back(PlayLayer::get(), 1);
        GUI::get().offer_rebind("step-back");
        eui::Row("Saved states", "How many states to keep as a percent of your TPS. 100% at 240 TPS keeps 240.");
        if (eui::DragInt("##backstep-saves", &logic.backstep_percent, 1.f, 1, 200, "%d%%"))
            logic.backstep_percent = std::clamp(logic.backstep_percent, 1, 200);
        saved_on_release();
    }
    eui::EndCard();
    eui::EndCardRow();

    if (open_real_time)
        ImGui::OpenPopup("Real Time Settings");
    if (eui::BeginModal("Real Time Settings", 20.f)) {
        if (toggle("True Real Time", logic.true_real_time))
            GUI::get().drop_carry();
        tip("Runs every step as close to real time as the game allows. Accuracy is untested.");
        eui::EndModal();
    }
    if (open_sync_audio)
        ImGui::OpenPopup("Sync Audio Settings");
    if (eui::BeginModal("Sync Audio Settings", 20.f)) {
        eui::Row("Threshold", "How far the audio can drift before it gets synced.");
        if (eui::DragInt("##sync-threshold", &logic.sync_audio_threshold, 5.f, 20, 2000, "%d ms"))
            logic.sync_audio_threshold = std::clamp(logic.sync_audio_threshold, 20, 2000);
        saved_on_release();
        eui::EndModal();
    }
    if (open_overwrite)
        ImGui::OpenPopup("Overwrite Settings");
    if (eui::BeginModal("Overwrite Settings", 20.f)) {
        eui::Row("Rename format", "Added to the name instead of overwriting. # is the number, so _# saves Replay_1.");
        eui::InputText("##rename-format", &logic.rename_format);
        saved_on_release();
        std::string shown = logic.rename_format.empty() ? std::string("_#") : logic.rename_format;
        if (shown.find('#') == std::string::npos)
            shown += "#";
        shown.replace(shown.find('#'), 1, "1");
        ImGui::TextDisabled("Replay%s.echo", shown.c_str());
        eui::EndModal();
    }
    if (want_reset)
        ImGui::OpenPopup("Clear Replay");
    if (eui::BeginModal("Clear Replay", 16.f)) {
        ImGui::TextUnformatted("Remove every input from this replay?");
        float half = eui::RowButton(2);
        if (eui::Button("Clear", ImVec2(half, 0.f))) {
            GUI::get().reset_macro();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (eui::Button("Keep", ImVec2(half, 0.f)))
            ImGui::CloseCurrentPopup();
        eui::EndModal();
    }
}

void GUI::tools() {
    auto& logic = Logic::get();
    bool open_autoclicker = false, open_straight = false;

    eui::BeginCardRow("assists-0");
    if (eui::BeginCard("Safety and Auto Input", card_width())) {
        if (quad_row("Noclip", "##noclip", logic.noclip_p1, logic.noclip_p2))
            GUI::get().persist();
        toggle("Safe Mode", logic.safeMode);
        tip("Removes the ability to make progress when using hacks. You can still make progress if no hacks are on, though.");
        if (toggle("Prevent Death", logic.pause_before_kill))
            Logic::get().reset_traj();
        GUI::get().offer_rebind("key-pause-before-kill", "Stops on the frame before you die while recording. Works without Show Trajectory.");
        eui::Separator();
        toggle("Autoclicker", logic.autoclicker);
        GUI::get().offer_rebind("key-autoclick");
        if (eui::Gear("##autoclicker-settings"))
            open_autoclicker = true;
        if (toggle("Trajectory Autoclicker", logic.traj_autoclicker))
            Logic::get().reset_traj();
        tip("Automatically clicks or releases when the next frame would kill you. Works without Show Trajectory.");
        ImGui::BeginDisabled(!logic.traj_autoclicker);
        eui::Row("Lookahead", "Clicks when you would die within this many frames.");
        if (eui::InputInt("##traj-lookahead", &logic.traj_click_lookahead))
            logic.traj_click_lookahead = std::clamp(logic.traj_click_lookahead, 1, 240);
        persist_on_release();
        ImGui::EndDisabled();
        if (quad_row("Auto Straight Fly", "##straight-fly", logic.straight_fly_p1, logic.straight_fly_p2, "Holds and lets go for you to keep the ship level. It can drift."))
            GUI::get().persist();
        if (quad_row("Auto Straight UFO", "##straight-ufo", logic.straight_ufo_p1, logic.straight_ufo_p2, "Clicks for you to keep the UFO at one height."))
            GUI::get().persist();
        ImGui::BeginDisabled(!logic.straight_fly() && !logic.straight_ufo());
        if (eui::Button("Straight Fly Settings...", ImVec2(-FLT_MIN, 0.f)))
            open_straight = true;
        ImGui::EndDisabled();
    }
    eui::EndCard();
    if (eui::BeginCard("Two Players", card_width())) {
        toggle("Click Both Players", logic.click_both);
        GUI::get().offer_rebind("key-click-both");
        toggle("Swap Player Input", logic.swap_players);
        GUI::get().offer_rebind("key-swap");
        if (quad_row("Inverse inputs", "##inverse", logic.inverse_p1, logic.inverse_p2))
            GUI::get().persist();
        char const* follow[] = { "Off", "P2 follows P1", "P1 follows P2" };
        eui::Row("Follow", "For two player levels, the following player will copy the followed player after that many frames or milliseconds.");
        if (eui::Combo("##follow", &logic.follow_mode, follow, 3))
            GUI::get().persist();
        ImGui::BeginDisabled(logic.follow_mode == 0);
        {
            eui::RowWith("Delay", 5.f);
            if (eui::DragFloat("##follow-delay", &logic.follow_delay, logic.follow_delay_ms ? 1.f : 0.25f, 0.f, 100000.f, logic.follow_delay_ms ? "%.1f" : "%.0f"))
                logic.follow_delay = std::max(logic.follow_delay, 0.f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How far behind the follower is. %d steps at the current TPS.", Logic::get().follow_steps());
            persist_on_release();
            char const* units[] = { "frames", "ms" };
            int unit = logic.follow_delay_ms ? 1 : 0;
            ImGui::SetNextItemWidth(eui::RowRight(5.f));
            if (eui::Combo("##follow-unit", &unit, units, 2)) {
                logic.follow_delay_ms = unit == 1;
                GUI::get().persist();
            }
        }
        ImGui::EndDisabled();
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("assists-1");
    if (eui::BeginCard("Orb Assists", card_width())) {
        ImGui::TextDisabled("While recording");
        if (toggle("Click Dash Orbs", logic.click_dash_orbs))
            Logic::get().reset_traj();
        GUI::get().offer_rebind("key-click-dash-orbs", "Automatically swift clicks stacked dash orbs. Does not affect playback.");
        if (toggle("Click Black Orbs", logic.click_black_orbs))
            Logic::get().reset_traj();
        GUI::get().offer_rebind("key-click-black-orbs", "Automatically swift clicks black orbs as UFO. Does not affect playback.");
        if (toggle("Black Orb on Pad", logic.pad_black_orb))
            Logic::get().reset_traj();
        GUI::get().offer_rebind("key-pad-black-orb", "Automatically swift clicks black orbs when hitting a pad.");
        if (toggle("Blue Orb on Pad", logic.pad_blue_orb))
            Logic::get().reset_traj();
        GUI::get().offer_rebind("key-pad-blue-orb", "Automatically swift clicks blue orbs when hitting a pad.");
        if (toggle("Easy Black Orb UFO", logic.easy_black_ufo))
            Logic::get().reset_traj();
        tip("Swift clicks black orbs as UFO, but only while you are falling.");
    }
    eui::EndCard();
    if (eui::BeginCard("Physics and Clicks", card_width())) {
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
            if (eui::Checkbox("Low Detail Mode", &ldm))
                logic.set_macro_ldm(ldm);
            ImGui::EndDisabled();
            tip("Saves LDM setting in the macro, as it sometimes breaks if not set correctly.");
        }
        toggle("Maintain Gravity", logic.maintain_gravity);
        tip("Makes the wave control as if gravity was always normal while recording.");
        ImGui::BeginDisabled(!logic.maintain_gravity);
        toggle("Same Gravity", logic.same_gravity);
        ImGui::EndDisabled();
        tip("Ensures that Maintain Gravity keeps both players on the same gravity.");
        toggle("Auto Swift", logic.auto_swift);
        tip("Every click lets go on the same frame it was pressed.");
        eui::RowHalf();
        toggle("Extra Clicks", logic.extra_clicks);
        tip("Adds extra clicks on the same frame for each click.");
        ImGui::BeginDisabled(!logic.extra_clicks);
        eui::Row("Extra click amount");
        if (eui::DragInt("##extra-clicks", &logic.extra_click_count, .1f, 1, 16, "%d extra"))
            logic.extra_click_count = std::clamp(logic.extra_click_count, 1, 16);
        persist_on_release();
        ImGui::EndDisabled();
    }
    eui::EndCard();
    eui::EndCardRow();
    if (open_straight)
        ImGui::OpenPopup("Straight Fly");
    if (eui::BeginModal("Straight Fly", 18.f)) {
        auto value_row = [&](char const* label, char const* id, float& value, float speed, char const* hint) {
            eui::Row(label, hint);
            if (eui::DragFloat(id, &value, speed, 0.f, 100000.f, "%.2f"))
                value = std::max(value, 0.f);
            persist_on_release();
        };
        char const* fly_hint = "How fast the ship has to be going up or down before it is corrected.";
        char const* ufo_hint = "How far from that height the UFO can get before it clicks.";
        eui::SectionLabel("Ship");
        value_row("Threshold P1", "##fly-threshold-1", logic.straight_fly_threshold_p1, .01f, fly_hint);
        value_row("Threshold P2", "##fly-threshold-2", logic.straight_fly_threshold_p2, .01f, fly_hint);
        eui::SectionLabel("UFO");
        value_row("Height P1", "##ufo-target-1", logic.straight_ufo_target_p1, .5f, "The Y position the UFO is kept at.");
        value_row("Threshold P1", "##ufo-threshold-1", logic.straight_ufo_threshold_p1, .05f, ufo_hint);
        value_row("Height P2", "##ufo-target-2", logic.straight_ufo_target_p2, .5f, "The Y position the UFO is kept at.");
        value_row("Threshold P2", "##ufo-threshold-2", logic.straight_ufo_threshold_p2, .05f, ufo_hint);
        eui::EndModal();
    }

    if (open_autoclicker)
        ImGui::OpenPopup("Autoclicker");
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
        if (eui::DragInt("##autoclick-press", &press, 1.f, 0, 10000, "%d frames"))
            logic.auto_click.press_interval = std::max(press, 0);
        persist_on_release();
        eui::Row("Swift clicks", "Adds extra clicks/releases on the same frame for each autoclick.");
        int swifts = logic.auto_click.swifts;
        if (eui::DragInt("##autoclick-swifts", &swifts, .1f, 0, 8, "%d extra"))
            logic.auto_click.swifts = std::clamp(swifts, 0, 8);
        persist_on_release();
        eui::Row("Release length");
        int release = logic.auto_click.release_interval;
        if (eui::DragInt("##autoclick-release", &release, 1.f, 1, 10000, "%d frames"))
            logic.auto_click.release_interval = std::max(release, 1);
        persist_on_release();
        toggle("Turn off by itself", logic.autoclick_auto_disable);
        ImGui::BeginDisabled(!logic.autoclick_auto_disable);
        eui::Row("After");
        int disable_in = logic.auto_click.disable_in;
        if (eui::DragInt("##autoclick-disable", &disable_in, 1.f, 1, 10000, "%d frames"))
            logic.auto_click.disable_in = std::max(disable_in, 1);
        saved_on_release();
        ImGui::EndDisabled();
        eui::EndModal();
    }
}

void GUI::draw_visuals() {
    auto& logic = Logic::get();
    bool open_hitboxes = false, open_traj = false, open_layout = false, open_labels = false;

    eui::BeginCardRow("visuals-0");
    if (eui::BeginCard("Overlays", card_width())) {
        toggle("Hitboxes", logic.show_hitboxes);
        GUI::get().offer_rebind("key-hitboxes");
        if (eui::Gear("##hitbox-settings"))
            open_hitboxes = true;
        toggle("Trajectory", logic.show_traj);
        GUI::get().offer_rebind("key-trajectory");
        if (eui::Gear("##trajectory-settings"))
            open_traj = true;
        toggle("Layout Mode", logic.layout_mode);
        GUI::get().offer_rebind("key-layout");
        if (eui::Gear("##layout-settings"))
            open_layout = true;
    }
    eui::EndCard();
    if (eui::BeginCard("Player", card_width())) {
        if (quad_row("Hide player", "##hide-player", logic.hide_p1, logic.hide_p2)) {
            if (logic.traj_follow_hide_player)
                Logic::get().reset_traj();
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
            if (title.starts_with("Show "))
                title = title.substr(5);
            if (i % 2)
                eui::RowHalf();
            ImGui::PushID(label.key);
            if (eui::Checkbox(title.c_str(), &(logic.*label.shown)))
                save();
            ImGui::PopID();
        }
        if (eui::Button("Position, Size and Font", ImVec2(-FLT_MIN, 0.f)))
            open_labels = true;
        tip("Changes where each label sits, its size, opacity, font and order.");
    }
    eui::EndCard();
    if (eui::BeginCard("Level Effects", card_width())) {
        toggle("Disable Shake", logic.disable_shakes);
        GUI::get().offer_rebind("key-shakes");
        toggle("Disable Death Effect", logic.disable_death_effect);
        toggle("No Effects", logic.no_effects);
        tip("Stops most effects from showing. Open the level again after changing it.");
        toggle("Hide New Best", logic.hide_new_best);
        toggle("Hide Endscreen", logic.hide_endscreen);
        tip("The end screen never shows, so leave the level from the pause menu.");
        if (toggle("No Mirror Portals", logic.no_mirror_portals))
            GUI::get().apply_no_mirror();
        tip("Stops mirror portals from flipping the level.");
        toggle("Stacked Orb Optimization", echo::opt::effects);
        tip("Clicking many orbs on the same frame only shows one effect.");
    }
    eui::EndCard();
    eui::EndCardRow();

    if (open_hitboxes)
        ImGui::OpenPopup("Hitboxes");
    if (eui::BeginModal("Hitboxes", 26.f)) {
        draw_hitbox_settings();
        eui::EndModal();
    }
    if (open_traj)
        ImGui::OpenPopup("Trajectory");
    if (eui::BeginModal("Trajectory", 20.f)) {
        draw_traj_settings();
        eui::EndModal();
    }
    if (open_layout)
        ImGui::OpenPopup("Layout Mode");
    if (eui::BeginModal("Layout Mode", 18.f)) {
        toggle("Use Regular Background", logic.layout_use_regular_bg);
        tip("Uses the level's actual background");
        if (eui::ColorEdit3("Background color", logic.backgroundColor))
            save();
        if (eui::ColorEdit3("Ground color", logic.blocks_color))
            save();
        eui::EndModal();
    }
    if (open_labels)
        ImGui::OpenPopup("On-Screen Labels");
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
        if (eui::Button("Export", ImVec2(half, 0.f)))
            Logic::get().export_kind(formats[format]);
        tip("Converts the current replay to the converted folder");
        ImGui::SameLine();
        if (eui::Button("Import", ImVec2(half, 0.f)))
            Logic::get().import_replays();
        tip("Import a replay of the selected type to convert to Echo");
        if (eui::Button("Convert Folder", ImVec2(half, 0.f)))
            Logic::get().convert_legacy_echoes();
        tip("Converts every replay in the replays folder to .echo. Old Echo replays are renamed to .old.echo.");
        ImGui::SameLine();
        if (eui::Button("Open Folder", ImVec2(half, 0.f)))
            geode::utils::file::openFolder(Logic::get().save_root() / "replays");
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
        if (toggle("Override seed", logic.seed_override))
            GUI::get().apply_seed_override();
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

void GUI::clickbot() {
    auto& logic = Logic::get();
    static int player = 0;
    eui::BeginCardRow("clickbot-0");
    if (eui::BeginCard("Clickbot", card_width())) {
        toggle("Enabled", logic.clickbot);
        char const* players[] = { "Player 1", "Player 2" };
        eui::Row("Settings for");
        eui::Combo("##clickbot-player", &player, players, 2);
        auto& pack = player == 0 ? logic.clickpack : logic.clickpack_p2;
        auto& volume = player == 0 ? logic.clickbot_volume : logic.clickbot_volume_p2;
        eui::Row("Volume");
        GUI::get().drag_float_setting(player == 0 ? "##clickbot-volume-p1" : "##clickbot-volume-p2", volume, 0.25f, 0.f, 10.f, "%.2fx");

        auto folders_root = Logic::get().save_root() / "clickpacks";
        std::vector<std::string> folders;
        std::error_code folder_error;
        for (auto const& entry : fs::directory_iterator(folders_root, folder_error)) {
            if (!entry.is_directory(folder_error))
                continue;
            try {
                folders.push_back(entry.path().filename().string());
            }
            catch (...) {
            }
        }
        std::string cur;
        try {
            cur = fs::path(pack).filename().string();
        }
        catch (...) {
        }
        if (cur.empty())
            cur = pack;
        eui::RowWith("Click pack", 5.f);
        if (eui::BeginCombo(player == 0 ? "##clickpack-p1" : "##clickpack-p2", cur.empty() ? "None" : cur.c_str())) {
            for (auto const& folder : folders) {
                if (eui::Selectable(folder.c_str(), folder == cur)) {
                    pack = (folders_root / folder).string();
                    GUI::get().persist();
                }
            }
            ImGui::EndCombo();
        }
        float browse_w = eui::RowRight(5.f);
        if (eui::Button("Browse", ImVec2(browse_w, 0.f))) {
            GUI::get().clickpacks().player = player;
            GUI::get().open_clickpack_picker();
        }
    }
    eui::EndCard();
    if (eui::BeginCard(player == 0 ? "Click Types (Player 1)###click-types" : "Click Types (Player 2)###click-types", card_width())) {
        ImGui::PushID(player);
        if (player == 0)
            draw_click_types(logic.p1_softs, logic.p1_soft_ms, logic.p1_hards, logic.p1_hard_seconds, logic.p1_micros, logic.p1_micro_ms);
        else
            draw_click_types(logic.p2_softs, logic.p2_soft_ms, logic.p2_hards, logic.p2_hard_seconds, logic.p2_micros, logic.p2_micro_ms);
        ImGui::PopID();
    }
    eui::EndCard();
    eui::EndCardRow();
}

void GUI::renderer() {
    auto& logic = Logic::get();
    bool open_save_preset = false;
    auto blocker = GUI::get().render_blocker();
    logic.ffmpeg_missing = !blocker.empty();
    if (!blocker.empty()) {
        if (eui::BeginCard("Render", card_width() * 2.f)) {
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextUnformatted(blocker.c_str());
            ImGui::PopTextWrapPos();
#ifdef GEODE_IS_WINDOWS
            if (eui::Button("Download FFmpeg"))
                geode::utils::web::openLinkInBrowser("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip");
            tip("Place ffmpeg.exe next to Geometry Dash once downloaded!");
#endif
#ifdef GEODE_IS_MACOS
            if (eui::Button("Copy Install Command")) {
                geode::utils::clipboard::write("brew install ffmpeg");
                Logic::get().notify("Copied: brew install ffmpeg", true);
            }
            tip("Paste this in Terminal. Requires Homebrew (brew.sh).");
#endif
        }
        eui::EndCard();
        return;
    }

    eui::BeginCardRow("render-0");
    if (eui::BeginCard("Capture", card_width())) {
        bool recording = Logic::get().render_active();
        ImVec2 big(-FLT_MIN, eui::em(2.3f));
        if (recording) {
            if (eui::ActionButton("Stop Rendering", eui::Icon::Render, true, big, RECORD_RED))
                Logic::get().stop_render();
        }
        else if (PlayLayer::get()) {
            if (eui::ActionButton("Start Rendering Here", eui::Icon::Render, false, big))
                Logic::get().start_render();
            tip("Begins rendering from your current position.");
        }
        else if (logic.render_queued) {
            if (eui::ActionButton("Cancel (waiting for a level)", eui::Icon::Render, true, big))
                logic.render_queued = false;
        }
        else {
            if (eui::ActionButton("Start Rendering", eui::Icon::Render, false, big))
                Logic::get().start_render();
            tip("Starts when you enter a level.");
        }
        if (Logic::get().render_capturing()) {
            auto status = Logic::get().render_status();
            eui::ProgressBar(status.known ? status.progress : -1.f, ImVec2(-FLT_MIN, 0.f), status.text.c_str());
        }
        eui::Row("Video name", "Leave empty to name the video after the macro");
        eui::InputTextWithHint("##render-name", logic.macro_name.empty() ? "Replay" : logic.macro_name.c_str(), &logic.render_name);
        float half = eui::RowButton(2);
        ImGui::BeginDisabled(logic.merge_busy);
        if (eui::Button("Join Two Videos", ImVec2(half, 0.f)))
            GUI::get().open_render_picker();
        ImGui::EndDisabled();
#ifdef GEODE_IS_MACOS
        tip("Joins two videos back to back with a crossfade.");
#else
        tip("Joins two videos back to back with a crossfade. Needs ffprobe.exe next to ffmpeg.exe.");
#endif
        ImGui::SameLine();
        if (eui::Button("Open Folder", ImVec2(half, 0.f))) {
            auto folder = Logic::get().save_root() / "renders";
            std::error_code ignored;
            fs::create_directories(folder, ignored);
            geode::utils::file::openFolder(folder);
        }
        tip("Opens the folder your videos are saved in.");
        if (logic.merge_busy) {
            float done = logic.merge_progress();
            auto text = done < 0.f ? std::string("Joining...") : fmt::format("Joining {:.0f}%", done * 100.f);
            eui::ProgressBar(done, ImVec2(-FLT_MIN, 0.f), text.c_str());
            tip(logic.merge_label.c_str());
        }
    }
    eui::EndCard();
    if (eui::BeginCard("Behavior", card_width())) {
        toggle("Use Speedhack", logic.render_speedhack);
        tip("Renders at your speedhack value");
        toggle("Exit level when finished", logic.render_auto_exit);
        toggle("Record the Opening Pause", logic.render_record_pause);
        toggle("Show Level While Rendering", logic.render_show_lvl);
        tip("Shows the level while rendering instead of a black screen.");
        toggle("Immediate Restart", logic.immediate_restart);
        GUI::get().offer_rebind("key-reset", "Restarts the level once as soon as you enter it");
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("render-1");
    if (eui::BeginCard("Video", card_width())) {
        draw_video_shape(open_save_preset);
#ifdef GEODE_IS_IOS
        toggle("Hide Labels", logic.render_hide_labels);
        tip("Hides Echo's labels while rendering.");
        eui::RowHalf();
        toggle("Hide Pause Button", logic.render_hide_pause);
        tip("Hides the pause button while rendering. Use the Echo icon to stop the render.");
        toggle("Color Fix", logic.render_color_fix);
        tip("Fixes video colors to be more accurate to the game.");
#else
        eui::Separator();
#ifdef GEODE_IS_ANDROID
        eui::Row("Codec, extension");
        float gap = ImGui::GetStyle().ItemSpacing.x;
        float pair = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(std::max(eui::em(2.f), (pair - gap) * .62f));
        eui::InputTextWithHint("##render-codec", "libx264", &logic.render_codec);
        saved_on_release();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(eui::em(2.f), ImGui::GetContentRegionAvail().x));
        eui::InputTextWithHint("##render-extension", "mp4", &logic.render_extension);
        saved_on_release();
#else
        eui::Row("File extension");
        eui::InputText("##render-extension", &logic.render_extension);
        saved_on_release();
        eui::Row("Codec");
        eui::InputText("##render-codec", &logic.render_codec);
        saved_on_release();
#endif
        eui::Row("FFmpeg arguments", "Output options placed after the codec information. Leave blank if you don't know what this is");
        eui::InputText("##render-args", &logic.render_args);
        saved_on_release();
        toggle("Color Fix", logic.render_color_fix);
        tip("Fixes video colors to be more accurate to the game.");
        eui::RowHalf();
        toggle("Hide Labels", logic.render_hide_labels);
        tip("Hides Echo's labels while rendering.");
#ifdef GEODE_IS_ANDROID
        toggle("Hide Pause Button", logic.render_hide_pause);
        tip("Hides the pause button while rendering. Use the Echo icon to stop the render.");
#endif
#endif
    }
    eui::EndCard();
    if (eui::BeginCard("Audio", card_width())) {
        eui::Row("Music volume");
        GUI::get().drag_float_setting("##render-music", logic.render_music, 0.01f, 0.f, 1.f, "%.2f");
        eui::Row("SFX volume");
        GUI::get().drag_float_setting("##render-sfx", logic.render_sfx, 0.01f, 0.f, 1.f, "%.2f");
        eui::Row("Fade in");
        GUI::get().drag_float_setting("##render-fade-in", logic.render_fade_in, 0.01f, 0.f, 2.f, "%.2f s");
        eui::Row("Fade out");
        GUI::get().drag_float_setting("##render-fade-out", logic.render_fade_out, 0.01f, 0.f, 2.f, "%.2f s");
        eui::Row("Keep going after end");
        GUI::get().drag_float_setting("##render-after-end", logic.render_after_end, 0.05f, 0.f, 5.f, "%.2f s");
        toggle("SSB Fix", logic.render_ssb_fix);
        tip("Fixes audio sync because of the game's scroll speed bug when rendering.");
        toggle("Real Time Rendering", logic.render_real_time);
        tip("May cause visual bugs. Stacked Orb Optimization may not be recommended.");
    }
    eui::EndCard();
    eui::EndCardRow();
    if (open_save_preset)
        ImGui::OpenPopup("Save Video Preset");
    if (eui::BeginModal("Save Video Preset", 16.f)) {
        static std::string name;
        ImGui::TextDisabled("%i x %i   %i FPS", logic.render_width, logic.render_height, logic.render_fps);
        eui::Row("Name");
        eui::InputTextWithHint("##preset-name", "My preset", &name);
        ImGui::BeginDisabled(name.empty());
        if (eui::Button("Save", ImVec2(-FLT_MIN, 0.f))) {
            std::erase_if(logic.render_presets, [&](RenderPreset const& preset) {
                return preset.name == name;
            });
            logic.render_presets.push_back({ name, logic.render_width, logic.render_height, logic.render_fps, logic.render_bitrate });
            save();
            name.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        eui::EndModal();
    }
}

CCTexture2D* GUI::theme_logo_texture() {
    auto const& name = Logic::get().theme_name;
    for (int i = 1; i < eui::preset_count(); ++i) {
        if (name != eui::preset(i).name)
            continue;
        auto const& preset = eui::preset(i);
        auto sprite = fmt::format("{}/echo-logo-{}.png", Mod::get()->getID(), GUI::get().lower_copy(preset.name));
        if (auto image = eui::logo_texture(sprite.c_str()))
            return image;
    }
    if (!GUI::get().has_palette || name == "Echo")
        return eui::logo_texture("logo.png"_spr);
    auto const& palette = GUI::get().palette;
    char const* base = nullptr;
    if (palette.rainbow)
        base = "rainbow";
    else if (palette.scene != eui::Scene::None)
        base = eui::scene_name(palette.scene);
    else if (palette.light)
        base = "white";
    else if (palette.accent_saturation < .06f)
        base = palette.depth < .8f ? "black" : "silver";
    if (base) {
        auto sprite = fmt::format("{}/echo-logo-{}.png", Mod::get()->getID(), base);
        if (auto image = eui::logo_texture(sprite.c_str()))
            return image;
    }
    return eui::logo_tinted_texture(palette.accent_hue);
}

ImTextureID GUI::theme_logo() {
    auto* texture = GUI::get().theme_logo_texture();
    return texture ? static_cast<ImTextureID>(texture->getName()) : ImTextureID{};
}

void GUI::draw_theme() {
    auto& logic = Logic::get();
    auto& gui = GUI::get();
    auto& style = ImGui::GetStyle();
    bool open_advanced = false;

    eui::BeginCardRow("settings-0");
    if (eui::BeginCard("Theme", card_width())) {
        for (int i = 0; i < eui::preset_count(); ++i) {
            auto const& preset = eui::preset(i);
            if (i % 6 != 0)
                ImGui::SameLine(0.f, (eui::RowButton(6) - eui::em(2.5f)) + ImGui::GetStyle().ItemSpacing.x);
            auto sprite = i == 0 ? std::string("logo.png"_spr) : fmt::format("{}/echo-logo-{}.png", Mod::get()->getID(), gui.lower_copy(preset.name));
            if (eui::ThemeTile(preset.name, eui::logo_image(sprite.c_str()), logic.theme_name == preset.name))
                gui.use_palette(preset.palette, preset.name);
            tip(preset.name);
        }
        float half = eui::RowButton(2);
        if (eui::Button("Randomize", ImVec2(half, 0.f)))
            gui.randomize_theme();
        tip("Picks a random color for the menu and logo.");
        ImGui::SameLine();
        if (eui::Button("Reset", ImVec2(half, 0.f))) {
            gui.restore_base_sizes();
            eui::apply_metrics();
            gui.on_style_replaced();
            gui.use_palette(eui::Palette{}, "Echo");
        }
        tip("Resets to the default Echo theme.");
        eui::Separator();
        eui::Row("Save as", "Saves the current theme under a name.");
        eui::InputText("##theme-name", &logic.theme_name);
        saved_on_release();
        if (eui::Button("Save", ImVec2(half, 0.f))) {
            gui.export_theme(gui.theme_path(logic.theme_name));
            gui.export_theme(gui.theme_path("current"));
            save();
        }
        ImGui::SameLine();
        if (eui::Button("Load", ImVec2(half, 0.f)))
            gui.open_theme_picker();
    }
    eui::EndCard();
    if (eui::BeginCard("Interface", card_width())) {
        eui::Row("Size");
        float drag_max = gui.scale_cap;
        float& menu_size = logic.docked_menu ? logic.menu_scale : logic.menu_scale_undocked;
        if (eui::DragFloat("##menu-scale", &menu_size, 0.01f, 0.35f, drag_max, "%.2fx"))
            menu_size = std::clamp(menu_size, 0.35f, drag_max);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(logic.docked_menu ? "The docked menu's size. The undocked menu has its own." : "The undocked menu's size. The docked menu has its own.");
        if (ImGui::IsItemActivated())
            gui.scale_dragging = true;
        if (ImGui::IsItemDeactivated()) {
            gui.scale_dragging = false;
            menu_size = std::clamp(menu_size, 0.35f, gui.scale_cap);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save();
            gui.export_theme(gui.theme_path("current"));
        }
#if 0 // control size is off for now, it blanked every page on a real phone
        {
            // only applied when the finger lets go. not sure if i like this but eh
            static float draft = 1.f;
            static bool dragging = false;
            if (!dragging)
                draft = logic.menu_inner_scale;
            eui::Row("Control size");
            if (eui::DragFloat("##menu-inner", &draft, 0.005f, 1.f, 1.5f, "%.2fx"))
                draft = std::clamp(draft, 1.f, 1.5f);
            tip("Bigger controls in the same size menu. Pages scroll instead.");
            dragging = ImGui::IsItemActive();
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                logic.menu_inner_scale = draft;
                save();
            }
        }
#endif
        eui::Row("Animation speed");
        if (eui::DragFloat("##menu-anim", &logic.menu_anim_speed, 0.01f, 0.25f, 4.f, "%.2fx"))
            logic.menu_anim_speed = std::clamp(logic.menu_anim_speed, 0.25f, 4.f);
        saved_on_release();
#ifndef GEODE_IS_MOBILE
        if (eui::Checkbox("One window (docked)", &logic.docked_menu))
            save();
        tip("Turning this off makes every page its own window.");
#endif
#ifdef GEODE_IS_MOBILE
        bool floating_logo = true;
#else
        bool floating_logo = false;
#endif
        if (floating_logo) {
            eui::Row("Icon size", "The size of the floating Echo button.");
            if (eui::DragFloat("##icon-size", &logic.floating_size, 0.25f, 24.f, 72.f, "%.0f"))
                logic.floating_size = std::clamp(logic.floating_size, 24.f, 72.f);
            EchoUI::preview_logo(ImGui::IsItemActive());
            saved_on_release();
            if (eui::Checkbox("Hide Icon When Playing", &logic.floating_hide_in_lvl))
                save();
            tip("Hides the floating Echo icon while playing a level. It comes back when you pause.");
        }
        toggle("TPS Changes During Playback", logic.playback_tps_changes);
        tip("Lets you set the macro's starting TPS while it is playing.");
        if (toggle("Practice Music Hack", logic.practice_music_hack)) {
            if (auto* play = PlayLayer::get(); play && logic.practice_music_hack)
                play->m_practiceMusicSync = true;
        }
        tip("Plays the level music in practice mode.");
        toggle("Logging", logic.trace_logging);
        tip("Saves a trace of each playback for bug reports.");
        eui::RowHalf();
        if (eui::Button("Open Logs Folder"))
            geode::utils::file::openFolder(logic.save_root() / "logs");
        tip("Opens the folder the logs are saved in.");
#ifdef GEODE_IS_WINDOWS
        toggle("Inline Getters", echo::opt::getters);
        tip("Inlines the rect getters in the collision check.");
#endif
        if (eui::Checkbox("Button Effects", &eui::button_effects()))
            gui.export_theme(gui.theme_path("current"));
        tip("Enables button animations for the animated themes.");
        eui::RowHalf();
        bool sharp = style.CircleTessellationMaxError >= 2.5f;
        if (eui::Checkbox("Sharp Edges", &sharp)) {
            style.CircleTessellationMaxError = sharp ? 5.f : 0.1f;
            gui.base_style.CircleTessellationMaxError = style.CircleTessellationMaxError;
            gui.export_theme(gui.theme_path("current"));
        }
        tip("Draws curves with fewer segments.");
        bool disabled = Logic::bot_disabled();
        if (eui::Checkbox("Disable Bot", &disabled))
            Logic::set_bot_disabled(disabled);
        tip("Completely disables the bot.");
        if (eui::Button("Advanced Style...", ImVec2(-FLT_MIN, 0.f)))
            open_advanced = true;
        tip("Lets you edit every size and color.");
    }
    eui::EndCard();
    eui::EndCardRow();
    eui::BeginCardRow("settings-1");
    if (eui::BeginCard("Custom Color", card_width())) {
        auto& palette = gui.palette;
        bool live = false;
        bool settled = false;
        auto slider = [&](char const* label, char const* id, float* field, float min, float max, bool rainbow, char const* hint) {
            eui::Row(label, hint);
            if (eui::Slider(id, field, min, max, rainbow ? "" : "%.2f", rainbow))
                live = true;
            if (ImGui::IsItemDeactivatedAfterEdit())
                settled = true;
        };
        slider("Body", "##palette-hue", &palette.hue, 0.f, 1.f, true, "The color the menu's dark body is tinted with.");
        slider("Accent", "##palette-accent", &palette.accent_hue, 0.f, 1.f, true, "The bright color of the rim, toggles and markers. The logo takes it too.");
        slider("Richness", "##palette-richness", &palette.richness, 0.f, 1.4f, false, "How strongly the body is tinted.");
        slider("Brightness", "##palette-depth", &palette.depth, .5f, 1.5f, false, "How dark or light the body is.");
        slider("Accent strength", "##palette-accent-saturation", &palette.accent_saturation, 0.f, .8f, false, "From a white accent to a vivid one.");
        if (live) {
            gui.has_palette = true;
            logic.theme_name = "Custom";
            eui::apply_palette(palette);
            gui.paint_editor_from_theme();
        }
        if (settled)
            gui.use_palette(palette, "Custom");
    }
    eui::EndCard();
#ifndef GEODE_IS_MOBILE
    if (eui::BeginCard("Keybinds", card_width())) {
        ImGui::TextDisabled("Click a key to change it.");
        ImGui::BeginChild("##keybind-list", ImVec2(-FLT_MIN, eui::CardFill(8.f)), ImGuiChildFlags_Borders);
        float key_w = eui::em(6.5f);
        for (auto const& action : GUI::bind_actions()) {
            ImGui::PushID(action.setting);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(action.label);
            ImGui::SameLine(ImGui::GetWindowWidth() - key_w - ImGui::GetStyle().WindowPadding.x - ImGui::GetStyle().ScrollbarSize);
            auto bound = gui.stored_bind(action.setting);
            bool none = bound == "None";
            if (none)
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (eui::Button((bound + "##bind").c_str(), ImVec2(key_w, 0.f))) {
                gui.prompt = action.setting;
                gui.prompt_nested = false;
            }
            if (none)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    eui::EndCard();
#endif
    eui::EndCardRow();

    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;

    if (open_advanced)
        ImGui::OpenPopup("Advanced Style");
    if (eui::BeginModal("Advanced Style", 27.f)) {
        gui.ui_editor();
        eui::EndModal();
    }
}
