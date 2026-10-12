#include "GUI/gui.hpp"
#include <imgui-cocos.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

void GUI::rewrite_selected(std::function<void(Frame&)> const& edit) {
    auto& inputs = Logic::get().inputs;
    int first = 0, last = 0;
    if (!selection_span(first, last))
        return;
    std::ranges::for_each(inputs.begin() + first, inputs.begin() + last + 1, edit);
    if (selected >= first && selected <= last) {
        draft = inputs[static_cast<size_t>(selected)];
    }
    if (selected >= 0) {
        selected_id = Logic::get().input_id(static_cast<size_t>(selected));
    }
    Logic::get().touch_inputs();
    Logic::get().refresh_live_hold();
    Logic::get().autosave_replay();
}

void GUI::move_selected(int dir) {
    auto& inputs = Logic::get().inputs;
    int first = 0, last = 0;
    if (!selection_span(first, last))
        return;
    int n = static_cast<int>(inputs.size());
    if (dir < 0 && first <= 0)
        return;
    if (dir > 0 && last + 1 >= n)
        return;
    auto rotate = [&](auto& side) {
        if (static_cast<int>(side.size()) != n)
            return;
        auto at = side.begin();
        if (dir < 0)
            std::rotate(at + first - 1, at + first, at + last + 1);
        else
            std::rotate(at + first, at + last + 1, at + last + 2);
    };
    rotate(inputs);
    rotate(Logic::get().poses);
    rotate(Logic::get().input_ids);
    selected += dir;
    if (anchor >= 0) {
        anchor += dir;
    }
    selected_id = Logic::get().input_id(static_cast<size_t>(selected));
    if (anchor >= 0) {
        anchor_id = Logic::get().input_id(static_cast<size_t>(anchor));
    }
    sync_draft();
    Logic::get().touch_inputs();
    Logic::get().refresh_live_hold();
    Logic::get().autosave_replay();
}

void GUI::addInput(int frame) {
    auto& logic = Logic::get();
    auto& inputs = logic.inputs;
    Frame created;
    uint64_t created_id = Logic::get().mint_input_id();
    created.number = frame;
    created.holding = true;
    created.player2 = !show_editor_p1 && show_editor_p2;
    int idx = selected;
    auto insert_side = [&](auto& side, auto empty, bool optional) {
        if (side.empty() && optional)
            return;
        if (side.size() + 1 != inputs.size() && side.size() != inputs.size()) {
            side.clear();
            return;
        }
        if (side.size() < inputs.size()) {
            if (idx >= static_cast<int>(side.size()))
                side.push_back(empty);
            else
                side.insert(side.begin() + idx, empty);
        }
    };
    if (idx < 0 || idx >= static_cast<int>(inputs.size())) {
        inputs.push_back(created);
        idx = static_cast<int>(inputs.size()) - 1;
    }
    else {
        inputs.insert(inputs.begin() + idx, created);
        Logic::get().touch_inputs();
    }
    insert_side(logic.poses, FramePose{}, true);
    insert_side(logic.input_ids, uint64_t{ 0 }, false);
    select_input(idx);
    Logic::get().set_input_id(static_cast<size_t>(idx), created_id);
    Logic::get().refresh_live_hold();
    plain = false;
    Logic::get().autosave_replay();
}

void GUI::deleteSelected() {
    log::debug("delete selected");
    auto& logic = Logic::get();
    auto& inputs = logic.inputs;
    int first = 0, last = 0;
    if (!selection_span(first, last))
        return;
    auto erase_span = [&](auto& side) {
        if (side.size() != inputs.size()) {
            side.clear();
            return;
        }
        side.erase(side.begin() + first, side.begin() + last + 1);
    };
    erase_span(logic.poses);
    erase_span(logic.input_ids);
    inputs.erase(inputs.begin() + first, inputs.begin() + last + 1);
    Logic::get().touch_inputs();
    Logic::get().refresh_live_hold();
    select_input(std::min(first, static_cast<int>(inputs.size()) - 1));
    Logic::get().autosave_replay();
}

void GUI::reset_macro() {
    auto& logic = Logic::get();
    Logic::get().clear_inputs();
    logic.recording_seconds = 0.0;
    logic.attempts = 0;
    select_input(-1);
    clear_cps_mark();
    show_editor_p1 = true;
    show_editor_p2 = true;
}

void GUI::offset_chosen(int delta) {
    int first = 0, last = 0;
    if (has_range() && selection_span(first, last))
        Logic::get().offset_frames(delta, first, last);
    else
        Logic::get().offset_frames(delta);
    sync_draft();
}

void GUI::random_offset_chosen() {
    log::debug("random offset chosen");
    int first = 0, last = 0;
    if (has_range() && selection_span(first, last))
        Logic::get().offset_inputs(input_offset, first, last);
    else
        Logic::get().offset_inputs(input_offset);
    sync_draft();
}

void GUI::set_frame_advance(bool on) {
    auto& logic = Logic::get();
    logic.frame_advance = on;
    persist();
    logic.advance_queued = false;
    logic.holding_advance = false;
    if (logic.frame_advance) {
        Logic::get().disarm_length_pause_if_holding();
    }
    logic.message = logic.frame_advance ? "Frame advance on" : "Frame advance off";
}

void GUI::apply_seed_override() {
    auto& logic = Logic::get();
    if (!logic.seed_override)
        return;
    logic.lvl_seed = logic.seed_override_value;
    logic.lvl_seed_known = true;
    logic.lvl_seed_from_file = false;
}

void GUI::setSpeed(float speed, bool settle) {
    log::debug("set speed");
    auto& logic = Logic::get();
    logic.speedhack = std::clamp(speed, 0.01f, 100.f);
    bool normal = std::abs(logic.speedhack - 1.f) < .00001f;
    if (!normal) {
        logic.speedhack_memory = logic.speedhack;
    }
    Logic::get().sync_time_scale();
    Logic::get().sync_playback_fps();
    if (settle) {
        persist();
    }
}

void GUI::set_playback_fps(float fps) {
    Logic::get().playback_fps = std::max(fps, 1.f);
    persist();
    Logic::get().sync_playback_fps();
}

void GUI::drop_carry() {
    Logic::get().carry = 0.f;
    if (auto* play = PlayLayer::get())
        play->m_extraDelta = 0.0;
}

void GUI::apply_no_mirror() {
}

std::string GUI::render_blocker() {
#if defined(GEODE_IS_IOS)
    return {};
#else
    if (Logic::get().ffmpeg_ready()) {
        return {};
    }
#ifdef GEODE_IS_WINDOWS
#if ECHO_FFMPEG_API_ENABLED
    return "ffmpeg.exe was not found. Place it next to Geometry Dash, or install the FFmpeg API mod (eclipse.ffmpeg-api).";
#else
    return "ffmpeg.exe was not found. Place it next to Geometry Dash.";
#endif
#elif defined(GEODE_IS_MACOS)
    return "ffmpeg was not found. Install it with Homebrew (brew install ffmpeg), or put an ffmpeg binary in Echo's folder.";
#else
#if ECHO_FFMPEG_API_ENABLED
    return "Rendering needs the FFmpeg API mod (eclipse.ffmpeg-api). Install it from the Geode mod index.";
#else
    return "Rendering is currently available on Windows only.";
#endif
#endif
#endif
}

bool GUI::two_player_dual() {
    auto* live = PlayLayer::get();
    if (!live) {
        return false;
    }
    bool two = live->m_levelSettings && live->m_levelSettings->m_twoPlayerMode;
    return two && live->m_gameState.m_isDualMode;
}
