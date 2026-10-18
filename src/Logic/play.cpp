#include <cstring>
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"
#ifdef GEODE_IS_MACOS
#include "Logic/mac_refresh_clock.hpp"
#endif
#include "GUI/gui.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/orbs.hpp"
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJRobotSprite.hpp>
#include <Geode/binding/GJSpiderSprite.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/EditorPauseLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <cstdlib>
#include <limits>
#include <filesystem>
#include <vector>
#include "Logic/input.hpp"
#include <Geode/binding/GameToolbox.hpp>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>

namespace chrono = std::chrono;

using namespace geode::prelude;

int Logic::get_frame(PlayLayer* play) {
    if (!play) return 0;
    return step_frame;
}

void Logic::toggle_recording() {
    if (is_playing() && !is_recording()) {
        bool record1 = record_p1 && !play_p1, record2 = record_p2 && !play_p2;
        if (!record1 && !record2) {
            message = "That player is already playing";
            return;
        }
    }
    bool was_recording = is_recording();
    if (state == RunState::Idle) {
        macro_unrounded = velocity_unrounding;
        macro_silicate_variance = false;
        auto* play = PlayLayer::get();
        if (inputs.empty() && play && play->m_level) {
            macro_ldm = play->m_level->m_lowDetailModeToggled;
            macro_ldm_known = true;
        }
        if (inputs.empty() && !macro_name_typed && play && play->m_level && !std::string(play->m_level->m_levelName).empty()) {
            macro_name = play->m_level->m_levelName;
            macro_path.clear();
        }
    }
    switch (state) {
    case RunState::Idle:
        state = RunState::Recording;
        break;
    case RunState::Recording:
        state = RunState::Idle;
        break;
    case RunState::Playing:
        state = RunState::RecordingAndPlaying;
        break;
    case RunState::RecordingAndPlaying:
        state = RunState::Playing;
        break;
    }
    message = state_name(state);
    if (is_recording() && !was_recording && inputs.capacity() < 4096) inputs.reserve(4096);
    if (was_recording && !is_recording()) autosave_replay();
}

void Logic::toggle_playing() {
    switch (state) {
    case RunState::Idle:
        state = RunState::Playing;
        break;
    case RunState::Playing:
        state = RunState::Idle;
        break;
    case RunState::Recording:
        state = RunState::Playing;
        break;
    case RunState::RecordingAndPlaying:
        state = RunState::Recording;
        break;
    }
    if (is_playing()) set_replay_pos(PlayLayer::get());
    else hold_sync = false;
    message = state_name(state);
}

bool Logic::route_to_recorded_player(bool& player1) {
    bool record1 = records_player(false), record2 = records_player(true);
    if (record1 == record2) return false;
    player1 = !record2;
    return true;
}

PlayerObject* Logic::icon_player(GJBaseGameLayer* play, bool icon2) {
    if (!play) return nullptr;
    return icon2 ? play->m_player2 : play->m_player1;
}

bool Logic::play_input(GJBaseGameLayer* play, bool icon2, bool down, int button) {
    PlayerObject* body = icon_player(play, icon2);
    if (!body || body->m_isDead || body->m_controlsDisabled) return false;
    if (button == 0) button = JUMP;
    auto key = (PlayerButton)button;
    if (down) body->pushButton(key);
    else body->releaseButton(key);
    return true;
}

bool Logic::raw_jump(GJBaseGameLayer* play, bool icon2) const {
    if (!play || !play->m_uiLayer) return false;
    auto* ui = play->m_uiLayer;
    return icon2 ? ui->m_p2Jumping || ui->m_p2TouchId != -1 || key_jump_p2 : ui->m_p1Jumping || ui->m_p1TouchId != -1 || key_jump_p1;
}

bool Logic::physical_hold(GJBaseGameLayer* play, bool icon2) {
    if (!play || !play->m_uiLayer) return false;
    if (suppress_auto_hold) return false;
    auto* ui = play->m_uiLayer;
    bool p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1 || key_jump_p1 || (restart_hold_captured && death_jump_p1);
    bool p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1 || key_jump_p2 || (restart_hold_captured && death_jump_p2);
    if (best_hold_owned) {
        bool icon = !best_frame_click_p1;
        if (Logic::raw_jump(play, icon) != best_hold_raw) best_hold_owned = false;
        else (icon ? p2 : p1) = best_hold;
    }
    if (restart_hold_captured && ((death_jump_p1 && (ui->m_p1Jumping || ui->m_p1TouchId != -1)) || (death_jump_p2 && (ui->m_p2Jumping || ui->m_p2TouchId != -1)))) {
        restart_hold_captured = false;
    }
    jump_held_p1 = p1;
    jump_held_p2 = p2;
    if (traj_flip_p1 && p1 != traj_flip_hold_p1) traj_flip_p1 = false;
    if (traj_flip_p2 && p2 != traj_flip_hold_p2) traj_flip_p2 = false;
    bool record1 = records_player(false), record2 = records_player(true);
    bool one_player_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    auto mine = [&] {
        if (record1 != record2 && !one_player_dual) {
            if (icon2 != record2) return icon2 ? p2 : p1;
            return p1 || p2;
        }
        if (flip_controls()) std::swap(p1, p2);
        if (one_player_dual || click_both) return p1 || p2;
        return icon2 ? p2 : p1;
    };
    bool held = mine();
    if (icon2 && !one_player_dual ? inverse_p2 : inverse_p1) held = !held;
    if (traj_autoclicker && maintain_gravity)
        held = held != (icon2 && !one_player_dual ? traj_flip_p2 : traj_flip_p1);
    return held;
}

bool Logic::route_key(GJBaseGameLayer* play, enumKeyCodes key, bool down, double timestamp) {
    if (!play || resetting || refreshing_visuals) return false;
    int code = (int)key;
    auto held = routed_keys.find(code);
    if (!down && held != routed_keys.end()) {
        int button = held->second & 15;
        bool icon2 = (held->second >> 4) != 0;
        routed_keys.erase(held);
        bool still = std::ranges::any_of(routed_keys, [&](const auto& entry) {
            return entry.second == ((icon2 ? 1 : 0) << 4 | button);
        });
        if (button == JUMP) (icon2 ? key_jump_p2 : key_jump_p1) = still;
        if (!still) play->queueButton(button, false, icon2, timestamp);
        if (button == JUMP && !still) {
            restart_hold_captured = false;
            death_jump_p1 = death_jump_p2 = false;
        }
        return true;
    }
    if (!down) return false;
    if (suppress_auto_hold) {
        switch (key) {
        case KEY_Space:
        case KEY_W:
        case KEY_Up:
            return true;
        default:
            break;
        }
    }
    bool target1 = true;
    if (!route_to_recorded_player(target1)) return false;
    bool icon2 = !target1;
    int button = 0;
    switch (key) {
    case KEY_Space:
    case KEY_W:
    case KEY_Up:
        button = JUMP;
        break;
    case KEY_A:
    case KEY_Left:
        if (play->m_isPlatformer) button = LEFT;
        break;
    case KEY_D:
    case KEY_Right:
        if (play->m_isPlatformer) button = RIGHT;
        break;
    default:
        break;
    }
    if (button == 0) return false;
    if (held != routed_keys.end()) return true;
    int tag = (icon2 ? 1 : 0) << 4 | button;
    bool already = std::ranges::any_of(routed_keys, [&](const auto& entry) {
        return entry.second == tag;
    });
    routed_keys[code] = tag;
    if (button == JUMP) {
        (icon2 ? key_jump_p2 : key_jump_p1) = true;
    }
    if (!already) play->queueButton(button, true, icon2, timestamp);
    return true;
}

void Logic::set_routed_hold(bool icon2, int button, bool down) {
    if (button == 0) button = JUMP;
    if (button == JUMP) (icon2 ? physical_jump_p2 : physical_jump_p1) = down;
    else if (button == LEFT) (icon2 ? physical_left_p2 : physical_left_p1) = down;
    else if (button == RIGHT) (icon2 ? physical_right_p2 : physical_right_p1) = down;
}

void Logic::reinforce_routed_hold(PlayerObject* body) {
    auto* layer = GJBaseGameLayer::get();
    if (!layer || !body || maintain_gravity || suppress_auto_hold) return;
    bool icon2 = body == icon_player(layer, true);
    if (!icon2 && body != icon_player(layer, false)) return;
    if (!records_player(icon2)) return;
    body->m_controlsDisabled = false;
    auto hold = [&](bool want, PlayerButton key) {
        if (!want || Logic::button_held(body, (int)key)) return;
        body->pushButton(key);
    };
    if (icon2) {
        hold(physical_jump_p2, PlayerButton::Jump);
        hold(physical_left_p2, PlayerButton::Left);
        hold(physical_right_p2, PlayerButton::Right);
    }
    else {
        hold(physical_jump_p1, PlayerButton::Jump);
        hold(physical_left_p1, PlayerButton::Left);
        hold(physical_right_p1, PlayerButton::Right);
    }
}

void Logic::capture_restart_holds(PlayLayer* play) {
    restart_hold_captured = false;
    traj_flip_p1 = traj_flip_p2 = false;
    traj_flip_hold_p1 = traj_flip_hold_p2 = false;
    if (!play) return;
    for (const auto& cmd : play->m_queuedButtons)
        note_direction_press(cmd);
    bool from_checkpoint = get_latest_checkpoint(play);
    if (from_checkpoint) {
        suppress_auto_hold = false;
        return;
    }
    // holding thru death must NOT hold the next attempt
    // but if nothing is down there is nothing to block (it ate the first click)
    bool down = Input::jump_key_down() || Input::mouse_down() || key_jump_p1 || key_jump_p2 || physical_jump_p1 || physical_jump_p2;
    if (auto* ui = play->m_uiLayer)
        down = down || ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
    clear_physical_holds();
    if (auto* ui = play->m_uiLayer) {
        ui->m_p1Jumping = false;
        ui->m_p2Jumping = false;
        ui->m_p1TouchId = -1;
        ui->m_p2TouchId = -1;
    }
    suppress_auto_hold = down;
    if (play->m_player1) play->m_player1->releaseAllButtons();
    if (play->m_player2) play->m_player2->releaseAllButtons();
}

void Logic::poll_hold_block() {
    if (!suppress_auto_hold) return;
    bool down = Input::jump_key_down() || Input::mouse_down();
    if (auto* play = PlayLayer::get(); play && play->m_uiLayer) {
        auto* ui = play->m_uiLayer;
        down = down || ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
    }
    if (!down) suppress_auto_hold = false;
}

void Logic::match_live_holds(PlayLayer* play) {
    if (!play || suppress_auto_hold) return;
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    for (bool icon2 : { false, true }) {
        if (icon2 && !two) continue;
        auto* body = icon_player(play, icon2);
        if (!body || plays_player(icon2)) continue;
        auto match = [&](int button, bool want) {
            if (want == button_held(body, button)) return;
            PlayerButtonCommand cmd;
            cmd.m_button = (PlayerButton)button;
            cmd.m_isPush = want;
            cmd.m_isPlayer2 = icon2;
            cmd.m_step = step_frame + 1;
            cmd.m_timestamp = 0.0;
            play->m_queuedButtons.push_back(cmd);
        };
        bool held = physical_hold(play, icon2);
        if (maintain_gravity && body->m_isDart && records_player(icon2))
            held = held != body->m_isUpsideDown;
        if (!autoclicker_drives(icon2)) match(JUMP, held);
        if (!play->m_isPlatformer) continue;
        match(LEFT, icon2 ? key_left_p2 || physical_left_p2 : key_left_p1 || physical_left_p1);
        match(RIGHT, icon2 ? key_right_p2 || physical_right_p2 : key_right_p1 || physical_right_p1);
    }
}

#define GRAVITY_FIX_TRIES 8

static bool gravity_fix_allowed(Logic& logic, GJBaseGameLayer* play, PlayerObject* body, bool icon2) {
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode || two;
    if (!body || !body->m_isDart || logic.plays_player(icon2) || (icon2 && !dual) || body->m_controlsDisabled)
        return false;
    if (logic.records_player(icon2)) return true;
    bool one_dual = play->m_gameState.m_isDualMode && !two;
    return one_dual && !icon2 && logic.records_player(true);
}

static bool wrong_gravity(Logic& logic, GJBaseGameLayer* play, PlayerObject* body, bool icon2, bool& push) {
    push = false;
    if (!gravity_fix_allowed(logic, play, body, icon2)) return false;
    bool physical = logic.physical_hold(play, icon2);
    if (body->m_isUpsideDown != (physical == body->m_jumpBuffered)) return false;
    push = physical != body->m_isUpsideDown;
    return true;
}

static void click_for_gravity(Logic& logic, GJBaseGameLayer* play, PlayerObject* body, bool icon2, bool push, bool record) {
    if (!body) return;
    if (record) {
        Frame input;
        input.number = logic.step_frame + 1;
        input.holding = push;
        input.player2 = icon2;
        input.button = (unsigned char)JUMP;
        size_t at = logic.add_input(input);
        if (logic.save_debug) logic.set_pose_at(at, logic.capture_pose(body));
    }
    body->m_isLocked = false;
    if (!record) logic.mirror_click = true;
    logic.play_input(play, icon2, push, JUMP);
    logic.mirror_click = false;
}

static void fix_gravity(Logic& logic, GJBaseGameLayer* play, PlayerObject* body, bool icon2) {
    logic.maintaining_gravity = true;
    for (int step = 0; step < GRAVITY_FIX_TRIES; ++step) {
        bool push = false;
        if (!wrong_gravity(logic, play, body, icon2, push)) break;
        bool flip = body->m_isUpsideDown, buffer = body->m_jumpBuffered;
        click_for_gravity(logic, play, body, icon2, push, true);
        if (body->m_isUpsideDown == flip && body->m_jumpBuffered == buffer) break;
    }
    logic.maintaining_gravity = false;
}

static void fix_gravity_mirrored(Logic& logic, GJBaseGameLayer* play) {
    auto* body = play->m_player1;
    logic.maintaining_gravity = true;
    for (int step = 0; step < GRAVITY_FIX_TRIES; ++step) {
        bool push = false;
        if (!wrong_gravity(logic, play, body, false, push)) break;
        bool flip = body->m_isUpsideDown, buffer = body->m_jumpBuffered;
        click_for_gravity(logic, play, body, false, push, true);
        if (body->m_isUpsideDown == flip && body->m_jumpBuffered == buffer) break;
        click_for_gravity(logic, play, play->m_player2, true, push, false);
    }
    logic.maintaining_gravity = false;
}

static void fix_gravity_both(Logic& logic, GJBaseGameLayer* play) {
    auto* body1 = play->m_player1;
    auto* body2 = play->m_player2;
    logic.maintaining_gravity = true;
    for (int step = 0; step < GRAVITY_FIX_TRIES; ++step) {
        bool push1 = false, push2 = false;
        bool need1 = wrong_gravity(logic, play, body1, false, push1), need2 = wrong_gravity(logic, play, body2, true, push2);
        if (!need1 && !need2) break;
        bool flip1 = body1 && body1->m_isUpsideDown, buffer1 = body1 && body1->m_jumpBuffered;
        bool flip2 = body2 && body2->m_isUpsideDown, buffer2 = body2 && body2->m_jumpBuffered;
        if (need1) click_for_gravity(logic, play, body1, false, push1, true);
        if (need2) click_for_gravity(logic, play, body2, true, push2, true);
        bool changed1 = need1 && body1 && (body1->m_isUpsideDown != flip1 || body1->m_jumpBuffered != buffer1);
        bool changed2 = need2 && body2 && (body2->m_isUpsideDown != flip2 || body2->m_jumpBuffered != buffer2);
        if (!changed1 && !changed2) break;
    }
    logic.maintaining_gravity = false;
}

void Logic::apply_maintain_gravity(GJBaseGameLayer* play) {
    if (!play || !play->m_uiLayer || play->m_levelEndAnimationStarted || applying_playback || restoring_holds)
        return;
    if (!maintain_gravity) return;
    if (!is_recording()) return;
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode || two;
    if (click_both && dual) physical_hold(play, false);
    if (play->m_gameState.m_isDualMode && !two) fix_gravity_mirrored(*this, play);
    else if (click_both && dual && !two) fix_gravity_both(*this, play);
    else {
        fix_gravity(*this, play, play->m_player1, false);
        fix_gravity(*this, play, play->m_player2, true);
    }
}

void Logic::sync_same_gravity(GJBaseGameLayer* play) {
    if (!play || !same_gravity || !maintain_gravity || applying_playback || restoring_holds) return;
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode && two;
    if (!dual || !play->m_player1 || !play->m_player2) return;
    auto* first = play->m_player1;
    auto* second = play->m_player2;
    bool now1 = first->m_isUpsideDown, now2 = second->m_isUpsideDown;
    // playback plays what it has. DONT force it
    if (!records_player(false) && !records_player(true)) {
        same_gravity_was_p1 = now1;
        same_gravity_was_p2 = now2;
        same_gravity_seen = true;
        return;
    }
    bool target = now1;
    if (same_gravity_seen && now2 != same_gravity_was_p2 && now1 == same_gravity_was_p1)
        target = now2;
    auto match = [&](PlayerObject* body, bool icon2) {
        if (!body || !records_player(icon2) || body->m_controlsDisabled || body->m_isUpsideDown == target)
            return;
        bool clicks = body->m_isSwing || ((body->m_isBall || body->m_isSpider) && body->m_isOnGround);
        if (body->m_isBall || body->m_isSpider || body->m_isSwing) {
            if (!clicks) return;
            auto put = [&](bool push) {
                Frame input;
                input.number = step_frame + 1;
                input.holding = push;
                input.player2 = icon2;
                input.button = (unsigned char)JUMP;
                add_input(input);
                body->m_isLocked = false;
                play_input(play, icon2, push, JUMP);
            };
            maintaining_gravity = true;
            if (Logic::button_held(body, JUMP)) put(false);
            put(true);
            put(false);
            maintaining_gravity = false;
            return;
        }
    };
    match(first, false);
    match(second, true);
    same_gravity_was_p1 = first->m_isUpsideDown;
    same_gravity_was_p2 = second->m_isUpsideDown;
    same_gravity_seen = true;
}

void Logic::toggle_frame_advance() {
    frame_advance = !frame_advance;
    advance_queued = false;
    if (!frame_advance) stop_best_frame();
    holding_advance = false;
    if (frame_advance) disarm_length_pause_if_holding();
    message = frame_advance ? "Frame advance on" : "Frame advance off";
}

void Logic::disarm_length_pause_if_holding() {
    if (!pause_at_traj_length) return;
    auto* play = PlayLayer::get();
    if (!play || !play->m_player1) return;
    if (physical_hold(play, false) || physical_hold(play, true)) {
        pause_length_armed = false;
    }
}

void Logic::advance_one_frame() {
    bool was_off = !frame_advance;
    if (!frame_advance) frame_advance = true;
    if (was_off) disarm_length_pause_if_holding();
    advance_steps = 1;
    advance_queued = true;
}

int Logic::owed_hold_steps(double& owed, double cap) {
    double rate = sample_rate() * std::clamp(advance_speed, 1.f, 200.f) / 100.0 * speedhack;
    owed += rate * CCDirector::sharedDirector()->getAnimationInterval();
    double wanted = std::floor(owed);
    if (wanted < 1.0) return 0;
    int steps = (int)std::min(wanted, cap);
    owed = std::min(owed - steps, 1.0);
    return steps;
}

void Logic::poll_advance_hold() {
    if (!frame_advance || !holding_advance || !PlayLayer::get()) return;
    if (GUI::get().typing()) {
        holding_advance = false;
        return;
    }
    auto now = chrono::steady_clock::now();
    auto start = advance_hold_start + chrono::milliseconds(advance_hold_ms);
    if (now < start) {
        advance_steps_owed = 0.0;
        return;
    }
    if (advance_queued) return;
    int steps = owed_hold_steps(advance_steps_owed, real_time_now() ? unlocked_step_cap() : 1);
    if (steps == 0) return;
    last_advance_step = now;
    advance_steps = steps;
    advance_queued = true;
}

void Logic::check_advance_pause(PlayLayer* play) {
    if (!play || play->m_playerDied || render_capturing()) return;
    if (!pause_before_kill || !is_recording()) return;
    auto watch = [&](bool player2) {
        if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2)) return false;
        return records_player(player2);
    };
    bool death_next = false;
    if (watch(false) && death_next_p1) death_next = true;
    if (watch(true) && death_next_p2) death_next = true;
    if (!death_next) {
        pause_kill_armed = true;
        return;
    }
    bool before_kill = pause_kill_armed;
    if (!before_kill) return;
    bool was_holding = holding_advance;
    bool was_off = !frame_advance;
    if (!was_off && !was_holding) return;
    frame_advance = true;
    holding_advance = false;
    advance_queued = false;
    pause_kill_armed = false;
    message = "Paused before kill";
}

bool Logic::editor_holds_keys() {
    auto* editor = LevelEditorLayer::get();
    if (!Logic::get().no_editor_keybinds || !editor) return false;
    return !editor->getChildByType<EditorPauseLayer>(0);
}

void Logic::on_keybind(std::string_view key, bool down) {
    if (down && GUI::get().typing()) return;
    if (down && Logic::editor_holds_keys()) return;
    if (key == "open-menu") {
        if (down) GUI::get().open_popup();
        return;
    }
    if (!down && key == "step-back") {
        holding_back = false;
        return;
    }
    if (!down && key == "step-frame") {
        holding_advance = false;
        disarm_length_pause_if_holding();
        return;
    }
    if (!down) return;
    if (key == "find-best-frame") {
        request_best_frame();
        return;
    }
    if (key == "step-back") {
        auto now = chrono::steady_clock::now();
        holding_back = true;
        back_hold_start = now;
        last_back_step = now;
        back_tap_queued = true;
        return;
    }
    if (key == "step-frame") {
        auto now = chrono::steady_clock::now();
        holding_advance = true;
        advance_hold_start = now;
        last_advance_step = now;
        advance_one_frame();
        return;
    }
    dispatch_setting_key(key, true);
}

void Logic::sync_time_scale() {
    auto* scheduler = CCDirector::sharedDirector()->getScheduler();
    float speed = speedhack;
    if (render_active()) {
        scheduler->setTimeScale(1.f);
        return;
    }
    if (auto* play = PlayLayer::get(); play && play->m_playerDied) {
        scheduler->setTimeScale(speed);
        return;
    }
    scheduler->setTimeScale(1.f);
}

void Logic::set_level_interval(float interval) {
    auto* director = CCDirector::sharedDirector();
    if (!(interval_before_level_fps > 0.f)) interval_before_level_fps = (float)director->getAnimationInterval();
    director->setAnimationInterval(interval);
}

void Logic::release_level_interval() {
    float held = std::exchange(interval_before_level_fps, 0.f);
    if (held > 0.f) CCDirector::sharedDirector()->setAnimationInterval(held);
}

void Logic::sync_playback_fps() {
    if (render_active()) return;
    auto* director = CCDirector::sharedDirector();
    if (playback_fps >= 1.f) {
        if (!playback_fps_held)
            interval_before_playback_fps = interval_before_level_fps > 0.f ? interval_before_level_fps : director->getAnimationInterval();
        playback_fps_held = true;
        director->setAnimationInterval(1.f / playback_fps);
        return;
    }
    if (!playback_fps_held) return;
    playback_fps_held = false;
    director->setAnimationInterval(interval_before_playback_fps > 0.f ? interval_before_playback_fps : 1.f / 60.f);
}

void Logic::begin_editor_playtest() {
    editor_testing = true;
    editor_paused = false;
    trail_p1.clear();
    trail_p2.clear();
    clear_macrobuff_samples();
    step_frame = 0;
    progress_base_set = false;
    leftover_time = 0.f;
    respawn_single_steps = 0;
    follow_log.clear();
    buttons_prepared = false;
    replay_clicks_left = 0;
    clear_click_hitbox();
    if (is_recording() && !is_playing()) trim_fresh_attempt();
    if (is_playing()) set_replay_pos(nullptr);
    sync_time_scale();
}

void Logic::end_editor_playtest(GJBaseGameLayer* editor) {
    editor_testing = false;
    capture_macrobuff_baseline(editor);
    editor_paused = false;
    leftover_time = 0.f;
    steps_left = 0;
    sync_time_scale();
}

void Logic::resume_attempt(PlayLayer* play) {
    if (!play) return;
    resume_pending = true;
    leftover_time = 0.f;
    play->m_extraDelta = 0.0;
    play->m_resumeTimer = 0;
    bool menu = pause_menu_open();
    if (!menu) play->m_isPaused = false;
    auto* director = CCDirector::sharedDirector();
    if (!menu && director->isPaused()) director->resume();
    director->setNextDeltaTimeZero(false);
    sync_time_scale();
}

void Logic::retain_playback_hold(PlayLayer* play) {
    if (!play || !is_playing()) return;
    auto hold = [&](bool icon2) {
        if (!plays_player(icon2)) return;
        auto* body = icon_player(play, icon2);
        if (!body) return;
        auto keep = [&](int button, bool want) {
            if (!want) return;
            body->m_holdingButtons[button] = true;
            if (button == JUMP) body->m_jumpBuffered = true;
            if (button == LEFT) body->m_holdingLeft = true;
            if (button == RIGHT) body->m_holdingRight = true;
        };
        int frame = step_frame;
        keep(JUMP, held_at(icon2, frame, JUMP));
        if (!play->m_isPlatformer) return;
        keep(LEFT, held_at(icon2, frame, LEFT));
        keep(RIGHT, held_at(icon2, frame, RIGHT));
    };
    hold(false);
    hold(true);
}

void Logic::on_focus_change() {
    leftover_time = 0.f;
    CCDirector::sharedDirector()->setNextDeltaTimeZero(true);
    if (auto* play = PlayLayer::get()) {
        play->m_extraDelta = 0.0;
        if (!play->m_playerDied) {
            if (play->m_player1) play->m_player1->m_isDead = false;
            if (play->m_player2) play->m_player2->m_isDead = false;
        }
    }
    sync_time_scale();
}

struct VisibleBeforeHide {
    bool held = false;
    bool wave = false;
    bool regular = false;
    bool ship = false;
    bool ghost = false;
    bool fire_mid = false, fire_bot = false, fire_top = false;
    bool dash = false;
};

static VisibleBeforeHide g_visible_before_hide[2]{};

static VisibleBeforeHide& visible_before_hide(PlayerObject* player) {
    auto* play = PlayLayer::get();
    bool second = play && player == play->m_player2;
    return g_visible_before_hide[second ? 1 : 0];
}

static void hide_node(CCNode* node, bool& saved, bool already) {
    if (!node) return;
    if (!already) saved = node->isVisible();
    node->setVisible(false);
}

static void reshow_node(CCNode* node, bool saved) {
    if (node) node->setVisible(saved);
}

static void hide_particles(CCParticleSystemQuad* system) {
    if (!system) return;
    system->setVisible(false);
    system->stopSystem();
}

static void show_particles(CCParticleSystemQuad* system) {
    if (system) system->setVisible(true);
}

static void unhide_player(PlayerObject* player, const VisibleBeforeHide& held) {
    player->m_playEffects = true;
    if (!player->m_isDead) {
        player->setVisible(true);
        player->updatePlayerArt();
        player->updatePlayerGlow();
        if (player->m_hasGlow && player->m_isRobot && player->m_robotSprite)
            player->m_robotSprite->showGlow();
        if (player->m_hasGlow && player->m_isSpider && player->m_spiderSprite)
            player->m_spiderSprite->showGlow();
    }
    reshow_node(player->m_waveTrail, held.wave);
    reshow_node(player->m_regularTrail, held.regular);
    reshow_node(player->m_shipStreak, held.ship);
    reshow_node(player->m_ghostTrail, held.ghost);
    reshow_node(player->m_swingFireMiddle, held.fire_mid);
    reshow_node(player->m_swingFireBottom, held.fire_bot);
    reshow_node(player->m_swingFireTop, held.fire_top);
    reshow_node(player->m_dashSpritesContainer, held.dash);
    show_particles(player->m_playerGroundParticles);
    show_particles(player->m_trailingParticles);
    show_particles(player->m_shipClickParticles);
    show_particles(player->m_vehicleGroundParticles);
    show_particles(player->m_ufoClickParticles);
    show_particles(player->m_robotBurstParticles);
    show_particles(player->m_dashParticles);
    show_particles(player->m_swingBurstParticles1);
    show_particles(player->m_swingBurstParticles2);
    show_particles(player->m_landParticles0);
    show_particles(player->m_landParticles1);
}

bool Logic::player_hidden(PlayerObject* player) const {
    auto* play = PlayLayer::get();
    if (!play || !player) return false;
    if (player == play->m_player1) return hide_p1;
    if (player == play->m_player2) return hide_p2;
    return false;
}

bool Logic::hitbox_player_hidden(bool player2) const {
    if (hitboxes_follow_hide_player) return player2 ? hide_p2 : hide_p1;
    return player2 ? hide_hitbox_p2 : hide_hitbox_p1;
}

bool Logic::traj_player_hidden(bool player2) const {
    if (traj_follow_hide_player) return player2 ? hide_p2 : hide_p1;
    return player2 ? hide_traj_p2 : hide_traj_p1;
}

void Logic::conceal_player(PlayerObject* player, bool hide) {
    if (!player) return;
    auto& held = visible_before_hide(player);
    if (!hide) {
        if (!held.held) return;
        VisibleBeforeHide saved = held;
        held = {};
        unhide_player(player, saved);
        return;
    }
    bool already = held.held;
    held.held = true;
    player->setVisible(false);
    player->m_playEffects = false;
    hide_node(player->m_waveTrail, held.wave, already);
    hide_node(player->m_regularTrail, held.regular, already);
    hide_node(player->m_shipStreak, held.ship, already);
    hide_node(player->m_ghostTrail, held.ghost, already);
    hide_node(player->m_swingFireMiddle, held.fire_mid, already);
    hide_node(player->m_swingFireBottom, held.fire_bot, already);
    hide_node(player->m_swingFireTop, held.fire_top, already);
    hide_node(player->m_dashSpritesContainer, held.dash, already);
    if (player->m_robotSprite) player->m_robotSprite->hideGlow();
    if (player->m_spiderSprite) player->m_spiderSprite->hideGlow();
    hide_particles(player->m_playerGroundParticles);
    hide_particles(player->m_trailingParticles);
    hide_particles(player->m_shipClickParticles);
    hide_particles(player->m_vehicleGroundParticles);
    hide_particles(player->m_ufoClickParticles);
    hide_particles(player->m_robotBurstParticles);
    hide_particles(player->m_dashParticles);
    hide_particles(player->m_swingBurstParticles1);
    hide_particles(player->m_swingBurstParticles2);
    hide_particles(player->m_landParticles0);
    hide_particles(player->m_landParticles1);
}

void Logic::after_frame(PlayLayer* play, float dt) {
    if (!play || resetting) return;
    flush_click_hitboxes(play);
    conceal_player(play->m_player1, hide_p1);
    conceal_player(play->m_player2, hide_p2);
    if (is_recording()) recording_seconds += (double)dt;
    if (play->m_hasCompletedLevel && is_recording()) {
        state = is_playing() ? RunState::Playing : RunState::Idle;
    }
    if (play->m_endPortal) end_x = play->m_endPortal->getPositionX();
    if (arm_immediate_restart && play->m_uiLayer && play->m_attemptLabel && !pause_menu_open()) {
        arm_immediate_restart = false;
        Loader::get()->queueInMainThread([] {
            auto* layer = PlayLayer::get();
            if (!layer || Logic::get().pause_menu_open() || !layer->m_uiLayer || !layer->m_attemptLabel)
                return;
            layer->resetLevel();
        });
    }
    if (!render_capturing()) update_cps(play);
    apply_layout(play);
    if (!in_scheduler) draw_hitboxes(play);
    draw_overlay(play);
}

void Logic::on_play_init(PlayLayer* play) {
    g_visible_before_hide[0] = {};
    g_visible_before_hide[1] = {};
    replay_pos = 0;
    replay_index = 0;
    sequence_order.clear();
    step_frame = 0;
    progress_base_set = false;
    removed_frames = 0;
    leftover_time = 0.f;
    respawn_single_steps = 0;
    skip_record_frames = 0;
    refreshing_visuals = false;
    refresh_queued = false;
    in_scheduler = false;
    buttons_prepared = false;
    replay_clicks_left = 0;
    cancel_steps();
    clear_click_hitbox();
    apply_rate(0);
    layout_styled = false;
    clear_stuck_death(play);
    clear_physical_holds();
    checkpoints.clear();
    restored_checkpoint = nullptr;
    click_frames_p1.clear();
    click_frames_p2.clear();
    ++clicks_version;
    attempts = 1;
    trail_p1.clear();
    trail_p2.clear();
    hidden_decorations.clear();
    last_click_p1 = 0.0;
    last_click_p2 = 0.0;
    if (play->m_level) {
        macro_name = play->m_level->m_levelName;
        if (macro_name.empty()) macro_name = "Replay";
        macro_name_typed = false;
    }
    if (lock_display_fps) {
        set_level_interval(1.f / fps);
        display_fps_locked = true;
    }
    else if (playback_fps < 1.f) set_level_interval(1.f / fps);
    sync_time_scale();
    reset_opening_music_hold();
    if (seed_override) {
        lvl_seed = seed_override_value;
        lvl_seed_known = true;
        lvl_seed_from_file = false;
    }
    else if (!is_playing() && !lvl_seed_from_file) {
        lvl_seed = 0;
        lvl_seed_known = false;
    }
    arm_immediate_restart = immediate_restart;
}

struct RandomTrigger {
    int unique_id = 0;
    unsigned long long* state = nullptr;
};

static std::vector<RandomTrigger> g_random_triggers;

struct KeptVariance {
    bool active = false;
    std::array<float, 2000> values{};
};

static KeptVariance g_kept_variance;

static void fill_variance(PlayLayer* play, unsigned long long seed) {
    unsigned long long state = seed;
    for (float& slot : play->m_varianceValues) {
        slot = (float)(state & 65535) / 32768.f - 1.f;
        state = 214013ull * state + 2531011ull; // msvc rand()
    }
}

unsigned long long Logic::game_random_state() {
    return GameToolbox::getfast_srand();
}

void Logic::set_game_random_state(unsigned long long state) {
    GameToolbox::fast_srand(state);
}

void Logic::note_rand_trigger(int unique_id, unsigned long long* state) {
    if (!state) return;
    for (const auto& trigger : g_random_triggers) {
        if (trigger.state == state) return;
    }
    g_random_triggers.push_back({ unique_id, state });
}

void Logic::clear_rand_triggers() {
    g_random_triggers.clear();
}

void Logic::reseed_rand_triggers(unsigned long long seed) {
    for (auto& trigger : g_random_triggers) {
        if (!trigger.state) continue;
        *trigger.state = seed ^ ((unsigned long long)trigger.unique_id * 2137ull);
    }
}

void Logic::roll_live_seed(PlayLayer* play) {
    static std::mt19937_64 rng{ std::random_device{}() };
    unsigned long long rolled = rng();
    if (rolled == 0) rolled = 1;
    g_kept_variance.active = false;
    if (!lvl_seed_from_file || is_recording()) {
        lvl_seed = rolled;
        lvl_seed_known = true;
        if (is_recording()) lvl_seed_from_file = false;
    }
    if (!play) return;
    play->m_randomSeed = rolled;
    play->m_replayRandSeed = rolled;
    play->m_unk32e0 = rolled;
    set_game_random_state(rolled);
    shake_random = rolled & 32767;
    teleport_random = rolled & 32767;
    fill_variance(play, rolled);
    reseed_rand_triggers(rolled);
}

void Logic::apply_attempt_variance(PlayLayer* play, unsigned long long seed) {
    if (!play) return;
    fill_variance(play, seed);
    auto& kept = g_kept_variance;
    kept.active = true;
    std::copy(play->m_varianceValues.begin(), play->m_varianceValues.end(), kept.values.begin());
}

void Logic::keep_variance(PlayLayer* play) {
    auto& kept = g_kept_variance;
    if (!play || !kept.active) return;
    if (!seed_override && !is_playing()) return;
    restore_variance(play->m_varianceValues, kept.values);
}

void Logic::plant_lvl_seed(PlayLayer* play) {
    if (!play || keep_lvl_seed) return;
    if (seed_override) {
        lvl_seed = seed_override_value;
        lvl_seed_known = true;
    }
    if (!lvl_seed_known) return;
    play->m_randomSeed = lvl_seed;
    play->m_replayRandSeed = lvl_seed;
    play->m_unk32e0 = lvl_seed;
    set_game_random_state(lvl_seed);
    shake_random = lvl_seed & 32767;
    teleport_random = lvl_seed & 32767;
    apply_attempt_variance(play, lvl_seed);
    reseed_rand_triggers(lvl_seed);
}

void Logic::restore_checkpoint_random(PlayLayer* play) {
    auto* saved = get_latest_checkpoint(play);
    if (!saved) return;
    const auto& checkpoint = *saved;
    set_game_random_state(checkpoint.random_state);
    shake_random = checkpoint.shake_random;
    teleport_random = checkpoint.teleport_random;
    if (checkpoint.variance.size() == play->m_varianceValues.size()) {
        std::copy(checkpoint.variance.begin(), checkpoint.variance.end(), play->m_varianceValues.begin());
        auto& kept = g_kept_variance;
        kept.active = true;
        std::copy(play->m_varianceValues.begin(), play->m_varianceValues.end(), kept.values.begin());
    }
    for (const auto& saved : checkpoint.rand_states) {
        for (auto& trigger : g_random_triggers) {
            if (trigger.unique_id == saved.first && trigger.state) *trigger.state = saved.second;
        }
    }
}

static void reshuffle_sequence(Logic& logic) {
    logic.sequence_order.resize(logic.sequence.size());
    std::iota(logic.sequence_order.begin(), logic.sequence_order.end(), size_t{ 0 });
    std::mt19937 rng{ std::random_device{}() };
    std::shuffle(logic.sequence_order.begin(), logic.sequence_order.end(), rng);
    logic.replay_index = 0;
}

void Logic::on_reset(PlayLayer* play) {
    // a new attempt (not a step back) starts w/ nothing held by best frame
    if (!backstepping) best_hold_owned = false;
    clear_click_hitbox();
    trail_p1.clear();
    trail_p2.clear();
    reset_opening_music_hold();
    if (!layout_active()) {
        for (auto* obj : hidden_decorations) {
            if (obj) obj->m_isHide = false;
        }
        hidden_decorations.clear();
        layout_styled = false;
    }
    currently_pressing = false;
    death_next_p1 = false;
    death_next_p2 = false;
    noclip_dead_p1 = false;
    noclip_dead_p2 = false;
    Orbs::clear();
    showed_new_best = false;
    over_max_cps = false;
    cps_breaking = false;
    if (play->m_endPortal) end_x = play->m_endPortal->getPositionX();
    if (is_recording() && !backstepping) ++attempts;

    if (sequence_enabled && !is_recording() && !backstepping) {
        auto use_clip = [&](const Replay& clip) {
            inputs = clip.actions;
            clear_input_extras();
            macro_name = clip.name;
            macro_name_typed = false;
            macro_fps = clip.fps;
            rates = clip.rates;
            lvl_seed = clip.lvl_seed;
            lvl_seed_known = clip.lvl_seed_known;
            lvl_seed_from_file = clip.lvl_seed_known;
            macro_unrounded = clip.unrounded;
            macro_silicate_variance = clip.silicate_variance;
            apply_rate(step_frame);
        };
        if (sequence_random && !sequence.empty()) {
            if (sequence_order.size() != sequence.size() || replay_index >= sequence_order.size())
                reshuffle_sequence(*this);
            size_t pick = sequence_order[replay_index];
            use_clip(sequence[pick]);
            ++replay_index;
            sort_inputs();
        }
        else if (replay_index < sequence.size()) {
            use_clip(sequence[replay_index]);
            ++replay_index;
            sort_inputs();
        }
        else {
            replay_index = 0;
        }
    }

    const bool practice = play->m_isPracticeMode || backstepping;
    auto* saved = get_latest_checkpoint(play);
    const bool from_checkpoint = practice && saved;
    if (from_checkpoint) {
        step_frame = saved->frame;
    }
    else {
        step_frame = 0;
        progress_base_set = false;
        removed_frames = 0;
        same_gravity_seen = false;
    }
    apply_rate(step_frame);
    auto replayed = [this](int frame) {
        return frame >= step_frame;
    };
    std::erase_if(click_frames_p1, replayed);
    std::erase_if(click_frames_p2, replayed);
    rewind_follow(play);
    ++clicks_version;
    play->m_extraDelta = 0.0;
    play->m_resumeTimer = 0;
    CCDirector::sharedDirector()->setNextDeltaTimeZero(false);
    clear_stuck_death(play);

    if (sort_automatically && is_recording() && is_playing()) sort_inputs();
    if (from_checkpoint) {
        if (!backstepping) object_restore = saved->object;
        trim_from_checkpoint(play, *saved);
        play->m_queuedButtons.clear();
    }
    else if (is_recording()) {
        trim_fresh_attempt();
    }
    if (from_checkpoint) {
        play->m_extraDelta = 0.0;
        refresh_queued = true;
        if (!in_scheduler && scheduler_depth == 0) flush_visual_refresh();
    }
    if (!practice) checkpoints.clear();
    if (is_playing()) set_replay_pos(play);
    Trajectory::get().invalidate();
    Trajectory::get().refresh(play);
    restore_holds(play);
    if (from_checkpoint) {
        resume_pending = false;
        play->m_extraDelta = 0.0;
        play->m_resumeTimer = 0;
        if (!pause_menu_open()) play->m_isPaused = false;
        clear_stuck_death(play);
        sync_time_scale();
        // respawn is NOT a gravity flip
        same_gravity_was_p1 = play->m_player1 && play->m_player1->m_isUpsideDown;
        same_gravity_was_p2 = play->m_player2 && play->m_player2->m_isUpsideDown;
        same_gravity_seen = true;
    }
    else {
        resume_attempt(play);
    }
}

static void set_jump_hold(PlayerObject* body, bool held) {
    if (!body) return;
    body->m_holdingButtons[JUMP] = held;
    body->m_jumpBuffered = held;
    if (!held) {
        body->m_wasJumpBuffered = false;
        body->m_stateJumpBuffered = 0;
    }
}

static void reset_holds_when_idle(Logic& logic, PlayLayer* play) {
    logic.restart_hold_captured = false;
    logic.death_jump_p1 = logic.death_jump_p2 = false;
    logic.death_left_p1 = logic.death_right_p1 = false;
    logic.death_left_p2 = logic.death_right_p2 = false;
    logic.key_jump_p1 = logic.key_jump_p2 = false;
    logic.physical_jump_p1 = logic.physical_jump_p2 = false;
    logic.routed_keys.clear();
    bool p1 = false, p2 = false;
    if (auto* ui = play->m_uiLayer) {
        p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1;
        p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1;
    }
#ifdef GEODE_IS_WINDOWS
    if (!Input::jump_key_down() && !Input::mouse_down()) p1 = p2 = false;
#endif
    play->m_queuedButtons.clear();
    if (play->m_player1) play->m_player1->releaseAllButtons();
    if (play->m_player2) play->m_player2->releaseAllButtons();
    bool checkpoint = logic.get_latest_checkpoint(play);
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool shared = play->m_gameState.m_isDualMode && !two;
    bool allow = checkpoint && !logic.suppress_auto_hold;
    set_jump_hold(play->m_player1, allow && p1);
    set_jump_hold(play->m_player2, allow && (shared ? p1 : two && p2));
    if (auto* ui = play->m_uiLayer) {
        ui->m_p1Jumping = allow && p1;
        ui->m_p2Jumping = allow && (shared ? p1 : two && p2);
        if (!ui->m_p1Jumping) ui->m_p1TouchId = -1;
        if (!ui->m_p2Jumping) ui->m_p2TouchId = -1;
    }
}

static void put_hold(Logic& logic, PlayLayer* play, bool icon2, int button, bool want) {
    auto* body = logic.icon_player(play, icon2);
    if (!body) return;
    bool have = Logic::button_held(body, button);
    bool wave = button == JUMP && body->m_isDart && logic.maintain_gravity && logic.records_player(icon2);
    if (!want && button == JUMP) body->m_jumpBuffered = false;
    if (!want && button == LEFT) body->m_holdingLeft = false;
    if (!want && button == RIGHT) body->m_holdingRight = false;
    if (have == want && !(wave && body->m_jumpBuffered != want)) return;
    body->m_holdingButtons[button] = want;
    if (button == JUMP) body->m_jumpBuffered = want;
    if (button == LEFT) body->m_holdingLeft = want;
    if (button == RIGHT) body->m_holdingRight = want;
}

static bool live_jump_held(Logic& logic, PlayLayer* play, bool icon2) {
    bool ui_down = false;
    if (auto* ui = play->m_uiLayer) {
        if (icon2) ui_down = ui->m_p2Jumping || ui->m_p2TouchId != -1;
        else ui_down = ui->m_p1Jumping || ui->m_p1TouchId != -1;
    }
    bool& key = icon2 ? logic.key_jump_p2 : logic.key_jump_p1;
    bool& physical = icon2 ? logic.physical_jump_p2 : logic.physical_jump_p1;
    if (logic.suppress_auto_hold) {
        if (!Input::jump_key_down() && !ui_down) {
            logic.key_jump_p1 = logic.key_jump_p2 = false;
            logic.physical_jump_p1 = logic.physical_jump_p2 = false;
            logic.suppress_auto_hold = false;
        }
        return false;
    }
    if (ui_down) return true;
    if (!Input::jump_key_down() && !Input::mouse_down()) {
        key = false;
        physical = false;
        return false;
    }
    if (key) return true;
    if (physical) return true;
    bool sticky = logic.restart_hold_captured && (icon2 ? logic.death_jump_p2 : logic.death_jump_p1);
    return sticky && Input::jump_key_down();
}

static bool live_direction_held(Logic& logic, bool sticky, bool& key, bool& physical, bool down) {
    if (logic.suppress_auto_hold) return false;
#ifdef GEODE_IS_MOBILE
    down = down || physical;
#endif
    if (!down) {
        key = false;
        physical = false;
        return false;
    }
    if (key || physical) return true;
    return logic.restart_hold_captured && sticky;
}

static void restore_recorded_button(Logic& logic, PlayLayer* play, bool icon2, bool one_dual, int button, bool held, std::vector<PlayerButtonCommand>& deferred) {
    bool stored = logic.held_at(icon2 && !one_dual, logic.step_frame, button);
    logic.restoring_holds = true;
    put_hold(logic, play, icon2, button, stored);
    logic.restoring_holds = false;
    if (held == stored) return;
    if (button == JUMP && logic.autoclicker_drives(icon2)) return;
    PlayerButtonCommand cmd;
    cmd.m_button = (PlayerButton)button;
    cmd.m_isPush = held;
    cmd.m_isPlayer2 = icon2;
    cmd.m_step = logic.step_frame + 1;
    cmd.m_timestamp = 0.0;
    deferred.push_back(cmd);
}

static void restore_recording_holds(Logic& logic, PlayLayer* play, bool icon2, bool one_dual, std::vector<PlayerButtonCommand>& deferred) {
    if (logic.suppress_auto_hold) {
        logic.restoring_holds = true;
        put_hold(logic, play, icon2, JUMP, false);
        logic.restoring_holds = false;
        if (auto* body = logic.icon_player(play, icon2)) body->releaseAllButtons();
        return;
    }
    if (!logic.records_player(icon2)) {
        if (!one_dual || logic.plays_player(icon2) || !logic.records_player(!icon2)) return;
        logic.restoring_holds = true;
        put_hold(logic, play, icon2, JUMP, logic.held_at(false, logic.step_frame, JUMP));
        if (play->m_isPlatformer) {
            put_hold(logic, play, icon2, LEFT, logic.held_at(false, logic.step_frame, LEFT));
            put_hold(logic, play, icon2, RIGHT, logic.held_at(false, logic.step_frame, RIGHT));
        }
        logic.restoring_holds = false;
        return;
    }
    if (logic.plays_player(icon2)) return;
    bool physical_jump_held = live_jump_held(logic, play, icon2);
    bool jump_held = physical_jump_held;
    auto* body = logic.icon_player(play, icon2);
    if (icon2 && !one_dual ? logic.inverse_p2 : logic.inverse_p1) jump_held = !jump_held;
    if (logic.maintain_gravity && body && body->m_isDart) jump_held = jump_held != body->m_isUpsideDown;
    bool left_held = live_direction_held(logic, icon2 ? logic.death_left_p2 : logic.death_left_p1, icon2 ? logic.key_left_p2 : logic.key_left_p1,
        icon2 ? logic.physical_left_p2 : logic.physical_left_p1, Input::left_key_down());
    bool right_held = live_direction_held(logic, icon2 ? logic.death_right_p2 : logic.death_right_p1, icon2 ? logic.key_right_p2 : logic.key_right_p1,
        icon2 ? logic.physical_right_p2 : logic.physical_right_p1, Input::right_key_down());
    restore_recorded_button(logic, play, icon2, one_dual, JUMP, jump_held, deferred);
    if (!physical_jump_held && play->m_uiLayer) {
        if (icon2) {
            play->m_uiLayer->m_p2Jumping = false;
            play->m_uiLayer->m_p2TouchId = -1;
        }
        else {
            play->m_uiLayer->m_p1Jumping = false;
            play->m_uiLayer->m_p1TouchId = -1;
        }
    }
    if (!play->m_isPlatformer) return;
    restore_recorded_button(logic, play, icon2, one_dual, LEFT, left_held, deferred);
    restore_recorded_button(logic, play, icon2, one_dual, RIGHT, right_held, deferred);
}

static void restore_playback_hold(Logic& logic, PlayLayer* play, bool player2, int button, int frame) {
    if (!logic.plays_player(player2)) return;
    bool want = logic.held_at(player2, frame, button);
    logic.applying_playback = !logic.records_player(player2);
    put_hold(logic, play, player2, button, want);
    logic.applying_playback = false;
}

void Logic::restore_holds(PlayLayer* play) {
    if (!play) return;
    if (is_playing()) repair_player_order(play);
    if (!is_playing() && !is_recording()) {
        reset_holds_when_idle(*this, play);
        return;
    }
    for (const auto& cmd : play->m_queuedButtons)
        note_direction_press(cmd);
    play->m_queuedButtons.clear();
    std::memset(restored_clicks, 0, sizeof(restored_clicks));

    bool playing = is_playing();
    bool recording = is_recording();
    bool checkpoint = get_latest_checkpoint(play);
    if (checkpoint && !playing && !recording) return;
    if (playing && !recording && checkpoints.empty()) {
        if (play->m_player1) play->m_player1->releaseAllButtons();
        if (play->m_player2) play->m_player2->releaseAllButtons();
    }
    std::vector<PlayerButtonCommand> deferred;
    bool one_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    if (recording) {
        if (suppress_auto_hold) {
            if (play->m_player1) play->m_player1->releaseAllButtons();
            if (play->m_player2) play->m_player2->releaseAllButtons();
            if (auto* ui = play->m_uiLayer) {
                ui->m_p1Jumping = false;
                ui->m_p2Jumping = false;
                ui->m_p1TouchId = -1;
                ui->m_p2TouchId = -1;
            }
        }
        restore_recording_holds(*this, play, false, one_dual, deferred);
        restore_recording_holds(*this, play, true, one_dual, deferred);
    }
    if (checkpoint && playing) {
        restoring_holds = true;
        int frame = step_frame;
        restore_playback_hold(*this, play, false, JUMP, frame);
        if (play->m_isPlatformer) {
            restore_playback_hold(*this, play, false, LEFT, frame);
            restore_playback_hold(*this, play, false, RIGHT, frame);
        }
        if (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode) {
            restore_playback_hold(*this, play, true, JUMP, frame);
            if (play->m_isPlatformer) {
                restore_playback_hold(*this, play, true, LEFT, frame);
                restore_playback_hold(*this, play, true, RIGHT, frame);
            }
        }
    }
    // NOT while a step back is loading
    if ((playing || recording) && !backstepping) play_macro(play, play, 0.f, true);
    for (const auto& cmd : deferred) {
        if (cmd.m_button == PlayerButton::Jump) ++restored_clicks[cmd.m_isPlayer2 ? 1 : 0][cmd.m_isPush ? 1 : 0];
        play->m_queuedButtons.push_back(cmd);
    }
    if (recording && checkpoint) sort_inputs();
    restoring_holds = false;
    restart_hold_captured = false;
    death_jump_p1 = death_jump_p2 = false;
    death_left_p1 = death_right_p1 = false;
    death_left_p2 = death_right_p2 = false;
}

void Logic::save_checkpoint(PlayLayer* play, CheckpointObject* object) {
    if (!play || !object || predicting) return;
    if (std::ranges::any_of(checkpoints, [object](const Checkpoint& saved) {
            return saved.object == object;
        }))
        return;
    int frame = get_frame(play) + (in_step ? 1 : 0);
    checkpoints.push_back(capture_checkpoint(play, object, frame));
    object_capture = object;
}

Checkpoint Logic::capture_checkpoint(PlayLayer* play, CheckpointObject* object, int frame) {
    Checkpoint checkpoint;
    checkpoint.object = object;
    checkpoint.frame = frame;
    checkpoint.layer = capture_layer(play);
    checkpoint.player1 = capture_player(play->m_player1);
    checkpoint.player2 = capture_player(play->m_player2);
    checkpoint.replay_pos = replay_pos;
    checkpoint.removed_frames = removed_frames;
    checkpoint.held_p1 = held_at(false, checkpoint.frame, JUMP);
    checkpoint.held_p2 = held_at(true, checkpoint.frame, JUMP);
    checkpoint.held_left_p1 = held_at(false, checkpoint.frame, LEFT);
    checkpoint.held_left_p2 = held_at(true, checkpoint.frame, LEFT);
    checkpoint.held_right_p1 = held_at(false, checkpoint.frame, RIGHT);
    checkpoint.held_right_p2 = held_at(true, checkpoint.frame, RIGHT);
    checkpoint.random_state = game_random_state();
    checkpoint.shake_random = shake_random;
    checkpoint.teleport_random = teleport_random;
    checkpoint.variance.assign(play->m_varianceValues.begin(), play->m_varianceValues.end());
    for (const auto& trigger : g_random_triggers) {
        if (trigger.state) checkpoint.rand_states.emplace_back(trigger.unique_id, *trigger.state);
    }
    return checkpoint;
}

void Logic::sync_checkpoints(PlayLayer* play) {
    std::erase_if(checkpoints, [play](const Checkpoint& saved) {
        return !play || !play->m_checkpointArray || !play->m_checkpointArray->containsObject(saved.object);
    });
    if (restored_checkpoint && (!play || !play->m_checkpointArray ||
                                         !play->m_checkpointArray->containsObject(restored_checkpoint)))
        restored_checkpoint = nullptr;
}

const Checkpoint* Logic::get_latest_checkpoint(PlayLayer* play) {
    if (!play || !(play->m_isPracticeMode || backstepping)) return nullptr;
    auto* obj = restored_checkpoint ? restored_checkpoint : play->getLastCheckpoint();
    if (!obj || !play->m_checkpointArray || !play->m_checkpointArray->containsObject(obj))
        return nullptr;
    auto it = std::ranges::find_if(checkpoints, [obj](const Checkpoint& saved) {
        return saved.object == obj;
    });
    return it == checkpoints.end() ? nullptr : &*it;
}

void Logic::on_complete(PlayLayer* play) {
    bool was_recording = is_recording();
    if (was_recording) state = is_playing() ? RunState::Playing : RunState::Idle;
    if (play->m_player1) end_x = play->m_player1->getPositionX();
    if (was_recording) autosave_replay();
    message = "Level complete";
}

void Logic::on_quit(PlayLayer*) {
    best_hold_owned = false;
    finish_backstep(nullptr);
    stop_best_frame();
    clear_backsteps();
    holding_back = false;
    back_tap_queued = false;
    frame_advance = false;
    advance_queued = false;
    holding_advance = false;
    for (auto* obj : hidden_decorations) {
        if (obj) obj->m_isHide = false;
    }
    hidden_decorations.clear();
    clear_physical_holds();
    Input::clear_keys();
    display_fps_locked = false;
    release_level_interval();
    CCDirector::sharedDirector()->getScheduler()->setTimeScale(1.f);
    stop_render();
    save_settings_file();
}

void Logic::clear_stuck_death(PlayLayer* play) {
    if (!play) return;
    play->m_playerDied = false;
    noclip_dead_p1 = false;
    noclip_dead_p2 = false;
    if (play->m_player1) play->m_player1->m_isDead = false;
    if (play->m_player2) play->m_player2->m_isDead = false;
}

bool Logic::should_noclip(PlayLayer* play, PlayerObject* player) {
    if (player == play->m_player1) return noclip_p1;
    if (player == play->m_player2) return noclip_p2;
    return false;
}

bool Logic::click_between_frames_enabled() const {
    const char* ids[] = { "syzzi.click_between_frames", "zmx.cbf-lite" };
    for (const char* id : ids) {
        auto* mod = Loader::get()->getLoadedMod(id);
        if (!mod) continue;
        if (mod->hasSetting("soft-toggle") && mod->getSettingValue<bool>("soft-toggle")) continue;
        return true;
    }
    return false;
}

bool Logic::vanilla_click_physics() const {
    if (auto* play = PlayLayer::get()) {
        if (play->m_clickBetweenSteps || play->m_clickOnSteps) return true;
    }
    else if (GameManager::get()->getGameVariable("0177") || GameManager::get()->getGameVariable("0176"))
        return true;
    return click_between_frames_enabled();
}

bool Logic::defer_physics() {
    if (refreshing_visuals) return false;
    if (auto* play = PlayLayer::get(); play && !play->m_started && !render_capturing()) return true;
    if (physics_locked()) return false;
    if (render_capturing()) return false;
    if (std::abs(speedhack - 1.f) >= .00001f || sample_rate() != VANILLA_TPS) return false;
    return vanilla_click_physics();
}

void Logic::flatten_click_times(GJBaseGameLayer* play) {
    if (!play || predicting || click_between_frames_enabled()) return;
    for (auto& cmd : play->m_queuedButtons)
        cmd.m_timestamp = 0.0;
}

double Logic::click_stamp() const {
    return click_between_frames_enabled() ? std::numeric_limits<double>::min() : 0.0;
}

static void drop_ignored_inputs(GJBaseGameLayer* play) {
    auto& logic = Logic::get();
    if (!play || logic.restoring_holds || !logic.is_playing() || !logic.ignore_inputs) return;
    auto& user = play->m_queuedButtons;
    user.erase(std::remove_if(user.begin(), user.end(), [&](const PlayerButtonCommand& cmd) {
        return logic.plays_player(cmd.m_isPlayer2);
    }),
        user.end());
    if (!logic.is_recording()) user.clear();
}

struct MacroEnds {
    unsigned long long version = ~0ull;
    const Frame* data = nullptr;
    size_t size = 0;
    int last[2] = { -1, -1 };
};

static MacroEnds g_macro_ends;

struct DualStart {
    int frame = -1, began = -1;
    bool dual = false;
    std::array<bool, 3> acted{};
};

static DualStart g_dual_start;

void Logic::prepare_buttons(GJBaseGameLayer* play) {
    bool paused = editor_paused;
    if (auto* live = Logic::as_play(play)) paused = paused || live->m_isPaused;
    if (!play || buttons_prepared || resetting || refreshing_visuals || paused || !physics_locked())
        return;
    buttons_prepared = true;

    auto& user = play->m_queuedButtons;
    for (const auto& cmd : user)
        note_direction_press(cmd);

    bool playing = is_playing();
    bool recording = is_recording();
    if (recording && !restoring_holds) {
        auto steer = [&](PlayerButtonCommand& cmd) {
            bool player1 = !cmd.m_isPlayer2;
            if (route_to_recorded_player(player1)) cmd.m_isPlayer2 = !player1;
        };
        for (auto& cmd : user)
            steer(cmd);
    }
    if (!restoring_holds && playing && ignore_inputs) {
        drop_ignored_inputs(play);
    }
    else if (!restoring_holds && recording && skip_record_frames > 0) {
        user.clear();
    }

    replay_clicks_left = 0;
    if (!playing) return;
    int frame = step_frame + 1;
    std::array<char, 6> queued{};
    bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    auto& p2 = g_dual_start;
    bool dual_now = play->m_gameState.m_isDualMode;
    if (frame != p2.frame + 1 || !dual_now) p2.began = -1;
    else if (!p2.dual) {
        p2.began = frame;
        p2.acted = {};
    }
    p2.frame = frame;
    p2.dual = dual_now;
    auto queue = [&](bool holding, int button, bool player2) {
        int slot = Logic::button_slot(button);
        if (slot >= 0) {
            if (player2) p2.acted[(size_t)slot] = true;
            queued[(size_t)((player2 ? 3 : 0) + slot)] = 1;
            // outside a 2 player lvl gd gives a p2 input to BOTH icons, so it counts for p1 too.
            // w/o ts ensure() queued a 2nd p1 press on the same step, & a 2nd pushButton re-arms the orb buffer
            // the 1st press just spent (desideration 79%: pink orb, then the green orb went off with no click)
            if (!two_player) queued[(size_t)((player2 ? 0 : 3) + slot)] = 1;
        }
        PlayerButtonCommand cmd;
        cmd.m_button = (PlayerButton)(button == 0 ? JUMP : button);
        cmd.m_isPush = holding;
        cmd.m_isPlayer2 = player2;
        cmd.m_step = frame;
        cmd.m_timestamp = 0.0;
        play->m_queuedButtons.push_back(cmd);
        ++replay_clicks_left;
        trace_button('Q', holding, (int)play->m_queuedButtons.size());
    };
    if (!restoring_holds && ignore_inputs) {
        while (replay_pos > 0 && replay_pos <= inputs.size() && inputs[replay_pos - 1].number >= frame)
            --replay_pos;
    }
    while (replay_pos < inputs.size() && inputs[replay_pos].number < frame)
        ++replay_pos;
    while (replay_pos < inputs.size() && inputs[replay_pos].number == frame) {
        Frame input = inputs[replay_pos];
        ++replay_pos;
        int button = input.button == 0 ? JUMP : input.button;
        if (!plays_player(input.player2)) continue;
        queue(input.holding, button, input.player2);
    }

    auto& ends = g_macro_ends;
    if (ends.version != inputs_version || ends.data != inputs.data() || ends.size != inputs.size()) {
        ends.version = inputs_version;
        ends.data = inputs.data();
        ends.size = inputs.size();
        ends.last[0] = ends.last[1] = -1;
        for (const auto& input : inputs) {
            int side = input.player2 ? 1 : 0;
            ends.last[side] = std::max(ends.last[side], input.number);
        }
    }
    auto ensure = [&](bool stored2, int button) {
        int side = stored2 ? 1 : 0;
        if (!ignore_inputs && frame > ends.last[side]) return;
        int slot = Logic::button_slot(button);
        if (slot >= 0 && queued[(size_t)((stored2 ? 3 : 0) + slot)]) return;
        auto* body = icon_player(play, stored2);
        if (!body || !plays_player(stored2)) return;
        if (slot > 0 && body->m_controlsDisabled) return;
        if (stored2 && two_player && p2.began >= 0 && slot >= 0 && !p2.acted[(size_t)slot]) return;
        bool want = held_at(stored2, frame, button);
        if (Logic::button_held(body, button) == want) return;
        queue(want, button, stored2);
    };
    ensure(false, JUMP);
    if (play->m_isPlatformer) {
        ensure(false, LEFT);
        ensure(false, RIGHT);
    }
    // normal dual follows p1. sending p2 every step clicks forever xd
    if (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode && play->m_gameState.m_isDualMode) {
        ensure(true, JUMP);
        if (play->m_isPlatformer) {
            ensure(true, LEFT);
            ensure(true, RIGHT);
        }
    }
}

void Logic::note_direction_press(const PlayerButtonCommand& cmd) {
    bool player2 = cmd.m_isPlayer2;
    if (cmd.m_button == PlayerButton::Left) {
        (player2 ? physical_left_p2 : physical_left_p1) = cmd.m_isPush;
    }
    else if (cmd.m_button == PlayerButton::Right) {
        (player2 ? physical_right_p2 : physical_right_p1) = cmd.m_isPush;
    }
}

void run_orb_clicks(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt) {
    auto* live = Logic::as_play(play);
    if (!live || !layer || Logic::get().predicting) return;
    if (!Orbs::has_pending_pads()) {
        Logic::get().suppress_click_hitbox = false;
        return;
    }
    auto saved = std::move(play->m_queuedButtons);
    play->m_queuedButtons.clear();
    Orbs::flush_pads(live);
    auto& logic = Logic::get();
    logic.suppress_click_hitbox = true;
    if (!play->m_queuedButtons.empty()) {
        if (logic.is_recording() && !logic.restoring_holds) {
            for (const auto& cmd : play->m_queuedButtons) {
                Logic::get().record_input(play, cmd.m_isPush, !cmd.m_isPlayer2, (int)cmd.m_button, Orbs::take_queued_assist() || Logic::record_every_input());
            }
        }
        logic.injecting = true;
        layer->GJBaseGameLayer::processQueuedButtons(dt, true);
        logic.injecting = false;
    }
    logic.suppress_click_hitbox = false;
    play->m_queuedButtons = std::move(saved);
}

void Logic::play_macro(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt, bool clear_input_queue) {
    bool paused = editor_paused;
    if (auto* live = Logic::as_play(play)) paused = paused || live->m_isPaused;
    if (!play || !layer || resetting || refreshing_visuals || paused || !physics_locked()) {
        if (layer) {
            drop_ignored_inputs(play);
            gd::vector<PlayerButtonCommand> orbs;
            if (auto* live = Logic::as_play(play); live && !predicting) {
                size_t before = play->m_queuedButtons.size();
                Orbs::flush_pads(live);
                for (size_t i = before; i < play->m_queuedButtons.size(); ++i)
                    orbs.push_back(play->m_queuedButtons[i]);
                if (play->m_queuedButtons.size() > before) play->m_queuedButtons.resize(before);
            }
            layer->GJBaseGameLayer::processQueuedButtons(dt, clear_input_queue);
            if (!orbs.empty()) {
                suppress_click_hitbox = true;
                if (is_recording() && !restoring_holds) {
                    for (const auto& cmd : orbs) {
                        record_input(play, cmd.m_isPush, !cmd.m_isPlayer2, (int)cmd.m_button, Orbs::take_queued_assist() || Logic::record_every_input());
                    }
                }
                play->m_queuedButtons = orbs;
                injecting = true;
                layer->GJBaseGameLayer::processQueuedButtons(dt, true);
                injecting = false;
                suppress_click_hitbox = false;
            }
            if (is_recording() && maintain_gravity && same_gravity && !restoring_holds && !resetting)
                sync_same_gravity(play);
        }
        return;
    }
    prepare_buttons(play);
    int replay = replay_clicks_left;
    if (play->m_queuedButtons.empty()) {
        applying_playback = false;
        if (is_recording() && maintain_gravity && !restoring_holds) apply_maintain_gravity(play);
        run_orb_clicks(play, layer, dt);
        applying_playback = false;
        if (is_recording() && maintain_gravity && same_gravity && !restoring_holds)
            sync_same_gravity(play);
        if (!clear_input_queue && replay == 0) play->m_queuedButtons.clear();
        buttons_prepared = false;
        return;
    }
#ifdef GEODE_IS_WINDOWS
    bool correct_gravity = is_recording() && maintain_gravity && !restoring_holds;
    bool single_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    if (clear_input_queue && play->m_queuedButtons.size() == 1 && (replay == 0 || replay == 1) &&
        !correct_gravity && (replay == 0 || !single_dual) && !Orbs::has_pending_pads()) {
        applying_playback = false;
        if (replay == 1) {
            suppress_click_hitbox = false;
            applying_playback = true;
        }
        layer->GJBaseGameLayer::processQueuedButtons(dt, true);
        applying_playback = false;
        if (replay == 0) {
            if (is_recording() && maintain_gravity && !restoring_holds)
                apply_maintain_gravity(play);
            run_orb_clicks(play, layer, dt);
        }
        if (is_recording() && maintain_gravity && same_gravity && !restoring_holds)
            sync_same_gravity(play);
        buttons_prepared = false;
        return;
    }
#endif
    std::vector<PlayerButtonCommand> queued(play->m_queuedButtons.begin(), play->m_queuedButtons.end());
    if (is_recording() && maintain_gravity && !restoring_holds) {
        // only a wave gets its button from maintain gravity. deleting every JUMP ate all my clicks on a ball or cube
        // (only w/ physics locked to steps, so only w/ the trajectory autoclicker or prevent death on)
        std::erase_if(queued, [&](const PlayerButtonCommand& cmd) {
            int button = (int)cmd.m_button;
            bool jump_button = button == 0 || button == JUMP;
            auto* body = icon_player(play, cmd.m_isPlayer2);
            return jump_button && body && body->m_isDart && records_player(cmd.m_isPlayer2) && !plays_player(cmd.m_isPlayer2);
        });
    }
    int user_count = std::max(0, (int)queued.size() - replay);
    auto run_queue = [&](int from, int to, bool playback) {
        play->m_queuedButtons.clear();
        for (int i = from; i < to; ++i)
            play->m_queuedButtons.push_back(queued[i]);
        applying_playback = playback;
        if (!play->m_queuedButtons.empty()) layer->GJBaseGameLayer::processQueuedButtons(dt, true);
    };
    run_queue(0, user_count, false);
    if (is_recording() && maintain_gravity && !restoring_holds) apply_maintain_gravity(play);
    // maintain gravity used to delete the swifts :/
    run_orb_clicks(play, layer, dt);
    bool one_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    applying_playback = true;
    for (int i = user_count; i < (int)queued.size(); ++i) {
        const auto& cmd = queued[(size_t)i];
        auto apply = [&](bool player2) {
            PlayerButtonCommand next = cmd;
            next.m_isPlayer2 = player2;
            play->m_queuedButtons.clear();
            play->m_queuedButtons.push_back(next);
            layer->GJBaseGameLayer::processQueuedButtons(dt, true);
        };
        unsigned partner_before = other_icon_presses;
        apply(cmd.m_isPlayer2);
        bool partner_clicked = other_icon_presses != partner_before;
        if (one_dual && !cmd.m_isPlayer2 && !partner_clicked && play->m_player2 && !play->m_player2->m_isDead && !play->m_player2->m_controlsDisabled) {
            apply(true);
        }
    }
    applying_playback = false;
    if (is_recording() && maintain_gravity && same_gravity && !restoring_holds)
        sync_same_gravity(play);
    if (!clear_input_queue && replay == 0) {
        play->m_queuedButtons.assign(queued.data(), queued.data() + queued.size());
    }
    buttons_prepared = false;
}

void Logic::offset_frames(int delta, int first, int last) {
    int n = (int)inputs.size();
    bool all = first < 0 || last < 0;
    if (all) {
        first = 0;
        last = n - 1;
    }
    if (first > last) std::swap(first, last);
    first = std::max(first, 0);
    last = std::min(last, n - 1);
    for (int i = first; i <= last; ++i) {
        if (inputs[(size_t)i].number + delta >= 0) inputs[(size_t)i].number += delta;
    }
    if (all) {
        for (auto& change : rates)
            change.frame = std::max(change.frame + delta, 0);
        normalize_rates();
    }
    mark_inputs_changed();
    sort_inputs();
    message = fmt::format("Shifted inputs by {}", delta);
    autosave_replay();
}

void Logic::offset_inputs(int magnitude, int first, int last) {
    if (magnitude < 0) magnitude = -magnitude;
    int n = (int)inputs.size();
    bool all = first < 0 || last < 0;
    if (all) {
        first = 0;
        last = n - 1;
    }
    if (first > last) std::swap(first, last);
    first = std::max(first, 0);
    last = std::min(last, n - 1);
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(-magnitude, magnitude);
    for (int i = first; i <= last; ++i) {
        int next = inputs[(size_t)i].number + dist(rng);
        inputs[(size_t)i].number = std::max(next, 0);
    }
    mark_inputs_changed();
    sort_inputs();
    message = fmt::format("Randomly offset inputs by +/- {}", magnitude);
    autosave_replay();
}

void Logic::remove_player_inputs(bool player2) {
    compact_inputs(inputs, poses, input_ids,
        [player2](const Frame& input) {
            return input.player2 != player2;
        });
    inputs.shrink_to_fit();
    poses.shrink_to_fit();
    input_ids.shrink_to_fit();
    mark_inputs_changed();
    refresh_live_hold();
    recompute_cps();
    message = player2 ? "Removed player 2 inputs" : "Removed player 1 inputs";
    autosave_replay();
}

void Logic::flip_input_holds() {
    for (auto& input : inputs)
        input.holding = !input.holding;
    refresh_live_hold();
    recompute_cps();
    message = "Flipped presses and releases";
    autosave_replay();
}

void Logic::flip_input_players() {
    for (auto& input : inputs)
        input.player2 = !input.player2;
    refresh_live_hold();
    recompute_cps();
    message = "Flipped player 1 and 2";
    autosave_replay();
}

void Logic::sort_inputs() {
    // by frame ONLY. outside 2 player levels p1 & p2 inputs drive the same icon, so their order on a frame matters
    if (std::is_sorted(inputs.begin(), inputs.end(), [](const Frame& a, const Frame& b) {
            return a.number < b.number;
        })) {
        mark_inputs_changed();
        refresh_live_hold();
        recompute_cps();
        return;
    }
    std::vector<size_t> order(inputs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return inputs[a].number < inputs[b].number;
    });
    std::vector<Frame> sorted;
    for (size_t i : order)
        sorted.push_back(inputs[i]);
    inputs = std::move(sorted);
    if (poses.size() == order.size()) {
        std::vector<FramePose> sorted_poses;
        for (size_t i : order)
            sorted_poses.push_back(poses[i]);
        poses = std::move(sorted_poses);
    }
    if (input_ids.size() == order.size()) {
        std::vector<unsigned long long> sorted_ids;
        for (size_t i : order)
            sorted_ids.push_back(input_ids[i]);
        input_ids = std::move(sorted_ids);
    }
    mark_inputs_changed();
    refresh_live_hold();
    recompute_cps();
}

unsigned long long Logic::new_input_id() {
    return ++next_input_id;
}

void Logic::identify_inputs(bool force) {
    if (!force && is_recording() && input_ids.empty()) return;

    struct Seen {
        const std::vector<Frame>* owner = nullptr;
        const Frame* data = nullptr;
        size_t count = 0;
        unsigned long long revision = 0;
        unsigned long long next = 0;
        unsigned long long last_id = 0;
    };

    thread_local Seen seen;
    bool kept = seen.owner == &inputs && seen.revision == inputs_version && seen.count <= inputs.size() && next_input_id >= seen.next;
    if (kept && seen.count > 0) kept = input_id(seen.count - 1) == seen.last_id;
    if (kept && seen.count == inputs.size()) kept = seen.data == inputs.data();
    size_t from = kept ? seen.count : 0;
    unsigned long long highest = next_input_id;
    for (size_t i = from; i < inputs.size(); ++i) {
        unsigned long long id = input_id(i);
        if (id > highest) highest = id;
    }
    next_input_id = highest;
    for (size_t i = from; i < inputs.size(); ++i) {
        if (input_id(i) != 0) continue;
        set_input_id(i, ++next_input_id);
    }
    seen = { &inputs, inputs.data(), inputs.size(), inputs_version, next_input_id, inputs.empty() ? 0 : input_id(inputs.size() - 1) };
}

void Logic::clear_inputs() {
    clear_input_extras();
    macro_unrounded = velocity_unrounding;
    macro_silicate_variance = false;
    std::vector<Frame>().swap(inputs);
    std::vector<FramePose>().swap(poses);
    std::vector<unsigned long long>().swap(input_ids);
    release_replay_index();
    mark_inputs_changed();
    rates.clear();
    replay_pos = 0;
    lvl_seed_from_file = false;
    if (seed_override) {
        lvl_seed = seed_override_value;
        lvl_seed_known = true;
    }
    else {
        lvl_seed = 0;
        lvl_seed_known = false;
    }
    message = "Cleared inputs";
}

void Logic::notify(const std::string& text, bool success) {
    if (success) Notification::create(text, NotificationIcon::Success)->show();
    message = text;
}

void Logic::note_click(PlayLayer* play, bool down, bool player1, int button) {
    if (!down || !play) return;
    if (resetting || refreshing_visuals || skip_record_frames > 0) return;
    if (button != 0 && button != JUMP && button != LEFT && button != RIGHT) return;
    if (button == 0) button = JUMP;
    PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
    if (body) {
        if (Logic::button_held(body, button) || (button == JUMP && Logic::button_held(body, 0)))
            return;
    }
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode, one_dual = dual && !two;
    if (one_dual) player1 = true;
    bool player2 = !player1;
    if (player2 && !(two && dual)) return;
    auto& clicks = player2 ? click_frames_p2 : click_frames_p1;
    int now = step_frame + 1;
    clicks.push_back(now);
    double now_t = seconds_at(now);
    size_t before = clicks.size();
    while (clicks.size() > 1 && now_t - seconds_at(clicks.front()) > 2.0)
        clicks.pop_front();
    while (clicks.size() > (1u << 20))
        clicks.pop_front();
    if (clicks.size() != before) ++clicks_version;
}

struct RecentClicks {
    int in_second = 0;
    int on_frame = 0;
};

static RecentClicks recent_clicks(const Logic& logic, const std::deque<int>& clicks, int now) {
    double now_t = logic.seconds_at(now);
    auto end = std::upper_bound(clicks.begin(), clicks.end(), now);
    auto start = std::partition_point(clicks.begin(), end, [&](int frame) {
        return now_t - logic.seconds_at(frame) > 1.0;
    });
    auto same = std::lower_bound(start, end, now);
    return { (int)std::distance(start, end), (int)std::distance(same, end) };
}

static bool same_rates(const std::vector<RateChange>& a, const std::vector<RateChange>& b) {
    return std::ranges::equal(a, b, [](const RateChange& x, const RateChange& y) {
        return x.frame == y.frame && x.fps == y.fps;
    });
}

static double burst_highest_lead(const Logic& logic, const std::deque<int>& clicks) {
    struct BurstCache {
        const std::deque<int>* owner = nullptr;
        unsigned long long clicks_version_seen = 0;
        float macro_fps = 0.f;
        std::vector<RateChange> rates;
        size_t scanned = 0;
        double highest_lead = 0.0;
    };

    thread_local std::array<BurstCache, 2> burst_caches;
    auto& cache = &clicks == &logic.click_frames_p2 ? burst_caches[1] : burst_caches[0];
    bool kept = cache.owner == &clicks && cache.clicks_version_seen == logic.clicks_version && cache.scanned <= clicks.size() &&
                cache.macro_fps == logic.macro_fps && same_rates(cache.rates, logic.rates);
    if (!kept) {
        cache = {};
        cache.owner = &clicks;
        cache.clicks_version_seen = logic.clicks_version;
        cache.macro_fps = logic.macro_fps;
        cache.rates = logic.rates;
    }
    int count = (int)clicks.size();
    int last_start = count - Logic::MIN_BURST_CLICKS;
    if (last_start < 0) return 0.0;
    double rate = (double)Logic::MAX_BURST_RATE;
    while (cache.scanned <= (size_t)last_start) {
        double burst_lead = rate * logic.seconds_at(clicks[cache.scanned]) - (double)cache.scanned;
        cache.highest_lead = cache.scanned == 0 ? burst_lead : std::max(cache.highest_lead, burst_lead);
        ++cache.scanned;
    }
    return cache.highest_lead;
}

unsigned Logic::count_presses_in_last_second(bool player2) {
    int now = PlayLayer::get() ? get_frame(PlayLayer::get()) : 0;
    const auto& clicks = player2 ? click_frames_p2 : click_frames_p1;
    return (unsigned)recent_clicks(*this, clicks, now).in_second;
}

bool Logic::burst_too_fast(int clicks, double span) {
    if (clicks < Logic::MIN_BURST_CLICKS) return false;
    if (!(span > 0.0)) return true;
    return (double)clicks / span > Logic::MAX_BURST_RATE;
}

bool Logic::player_cps_broken(const std::deque<int>& clicks, int now, float max_per_second) const {
    if (clicks.empty()) return false;
    auto recent = recent_clicks(*this, clicks, now);
    if ((float)recent.in_second > max_per_second) return true;
    if (recent.on_frame > Logic::MAX_PER_FRAME) return true;
    int n = (int)clicks.size();
    double back_t = seconds_at(clicks.back());
    if (n < Logic::MIN_BURST_CLICKS || seconds_at(now) - back_t > 1.0) return false;
    double lead_needed = (double)Logic::MAX_BURST_RATE * back_t - (double)n;
    double rounding = Logic::burst_rounding(lead_needed);
    double highest_lead = burst_highest_lead(*this, clicks);
    if (highest_lead > lead_needed + rounding) return true;
    if (highest_lead <= lead_needed - rounding) return false;
    for (int start = 0; start <= n - Logic::MIN_BURST_CLICKS; ++start) {
        if (Logic::burst_too_fast(n - start, back_t - seconds_at(clicks[(size_t)start])))
            return true;
    }
    return false;
}

void Logic::set_tps(float rate) {
    rate = std::clamp(rate, 1.f, FLT_MAX);
    if (!step_patched && rate < VANILLA_TPS) {
        message = "TPS below 240 requires the physics step patch";
        rate = VANILLA_TPS;
    }
    if (is_recording()) {
        int frame = PlayLayer::get() ? get_frame(PlayLayer::get()) : 0;
        place_rate(frame, rate);
        message = fmt::format("TPS {:.2f} at frame {}", rate, frame);
    }
    else if (!is_playing()) {
        macro_fps = rate;
        fps = rate;
        push_runtime_settings();
    }
    else if (playback_tps_changes) {
        // while a macro plays ts ONLY sets its starting tps (the 1 silicate gets wrong). the changes in the macro stay
        macro_fps = rate;
        int frame = PlayLayer::get() ? get_frame(PlayLayer::get()) : 0;
        fps = rate_at(frame);
        rate = fps;
        Trajectory::get().invalidate();
        message = fmt::format("Starting TPS {:.2f}", macro_fps);
    }
    if (playback_fps < 1.f && PlayLayer::get()) set_level_interval(1.f / rate);
}

void Logic::place_rate(int frame, float rate) {
    rate = std::clamp(rate, 1.f, FLT_MAX);
    if (frame <= 0) {
        macro_fps = rate;
        fps = rate;
        std::erase_if(rates, [](const RateChange& change) {
            return change.frame <= 0;
        });
        return;
    }
    float prev = rate_at(frame - 1);
    auto at = std::find_if(rates.begin(), rates.end(), [frame](const RateChange& change) {
        return change.frame == frame;
    });
    if (rate == prev) {
        if (at != rates.end()) rates.erase(at);
        fps = rate;
        return;
    }
    if (at != rates.end()) at->fps = rate;
    else {
        rates.push_back({ frame, rate });
        std::stable_sort(rates.begin(), rates.end(), [](const RateChange& a, const RateChange& b) {
            return a.frame < b.frame;
        });
    }
    fps = rate;
}

void Logic::apply_rate(int frame) {
    float next = rate_at(frame);
    if (next == fps) return;
    fps = next;
    if (display_fps_locked && playback_fps < 1.f && !render_capturing())
        set_level_interval(1.f / next);
}

float rate_at_frame(const std::vector<RateChange>& rates, float start_fps, int frame) {
    auto after = rate_after(rates, frame);
    float rate = after == rates.begin() ? start_fps : std::prev(after)->fps;
    return std::max(rate, 1.f);
}

double seconds_at_frame(const std::vector<RateChange>& rates, float start_fps, int frame) {
    if (frame <= 0) return 0.0;
    double time = 0.0;
    int at = 0;
    float rate = start_fps;
    auto add = [&](int upto) {
        if (upto <= at) return;
        time += (double)(upto - at) / (double)std::max(rate, 1.f);
        at = upto;
    };
    for (const auto& change : rates) {
        if (change.frame >= frame) break;
        if (change.frame > at) add(change.frame);
        rate = change.fps;
    }
    add(frame);
    return time;
}

void tidy_rates(std::vector<RateChange>& rates, float start_fps) {
    std::erase_if(rates, [](const RateChange& change) {
        return change.frame < 0 || !std::isfinite(change.fps) || change.fps < 1.f;
    });
    std::stable_sort(rates.begin(), rates.end(), [](const RateChange& a, const RateChange& b) {
        return a.frame < b.frame;
    });
    std::vector<RateChange> kept;
    float rate = start_fps;
    for (const auto& change : rates) {
        if (!kept.empty() && kept.back().frame == change.frame) kept.back() = change;
        else kept.push_back(change);
    }
    rates.clear();
    for (const auto& change : kept) {
        if (change.fps == rate) continue;
        rates.push_back(change);
        rate = change.fps;
    }
}

void Logic::normalize_rates() {
    tidy_rates(rates, macro_fps);
}

void Logic::trim_rates(int frame) {
    std::erase_if(rates, [frame](const RateChange& change) {
        return change.frame >= frame;
    });
    apply_rate(std::max(frame, 0));
}

void Logic::update_cps(PlayLayer* play) {
    auto holding = [](PlayerObject* player) {
        if (!player) return false;
        static const int buttons[] = {
            JUMP,
            LEFT,
            RIGHT,
        };
        return std::ranges::any_of(buttons, [player](int button) {
            return Logic::button_held(player, button);
        });
    };
    currently_pressing = holding(play->m_player1) || (play->m_gameState.m_isDualMode && holding(play->m_player2));
    bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool two_dual = two_player && play->m_gameState.m_isDualMode;
    if (!two_dual && !click_frames_p2.empty()) {
        click_frames_p2.clear();
        ++clicks_version;
    }
    unsigned p1 = count_presses_in_last_second(false);
    unsigned p2 = two_dual ? count_presses_in_last_second(true) : 0;
    current_cps = (float)(two_dual ? std::max(p1, p2) : p1);
    int now = get_frame(play);
    float limit = std::max((float)max_cps, 1.f);
    cps_breaking = player_cps_broken(click_frames_p1, now, limit);
    if (two_dual) cps_breaking = cps_breaking || player_cps_broken(click_frames_p2, now, limit);
    if (cps_breaking) over_max_cps = true;
}

void Logic::tick_autoclicker(PlayLayer* play) {
    if (!autoclicker || !play || play->m_isPaused) return;
    auto& click = auto_click;
    if (click.pressed) ++click.since_press;
    else ++click.since_release;

    auto press_player = [&](bool player1) {
        bool two_player = play->m_level && play->m_level->m_twoPlayerMode;
        if (!player1 && !two_player) return;
        play->queueButton(JUMP, true, !player1, click_stamp());
        for (int i = 0; i < click.swifts; ++i) {
            play->queueButton(JUMP, false, !player1, click_stamp());
            play->queueButton(JUMP, true, !player1, click_stamp());
        }
    };
    auto release_player = [&](bool player1) {
        bool two_player = play->m_level && play->m_level->m_twoPlayerMode;
        if (!player1 && !two_player) return;
        play->queueButton(JUMP, false, !player1, click_stamp());
    };

    if (!click.pressed && click.since_release >= click.release_interval) {
        click.pressed = true;
        click.since_release = 0;
        click.since_press = 0;
        if (autoclick_p1) press_player(true);
        if (autoclick_p2) press_player(false);
        if (click.press_interval <= 0) {
            click.pressed = false;
            if (autoclick_p1) release_player(true);
            if (autoclick_p2) release_player(false);
        }
    }
    else if (click.pressed && click.since_press >= click.press_interval) {
        click.pressed = false;
        click.since_press = 0;
        if (autoclick_p1) release_player(true);
        if (autoclick_p2) release_player(false);
    }

    if (autoclick_auto_disable) {
        --click.disable_in;
        if (click.disable_in <= 0) {
            autoclicker = false;
            autoclick_auto_disable = false;
            click.disable_in = 1;
            Logic::write_bool("autoclicker", false);
            message = "Autoclicker disabled";
        }
    }
}

// auto straight fly & auto straight ufo (scarlet utils has these). both just queue plain clicks at the end of a step, the way the
// autoclicker does, so they are recorded & play back like anything i pressed myself.
// fly: a ship that sinks faster than the threshold gets held, one that rises faster gets let go. it ends up wobbling around level.
void Logic::tick_straight(PlayLayer* play) {
    if ((!straight_fly() && !straight_ufo()) || !play || play->m_isPaused || play->m_playerDied)
        return;
    if (is_playing() && !is_recording()) return;
    bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    auto run = [&](PlayerObject* player, bool player2) {
        if (!player || player->m_isDead || plays_player(player2)) return;
        if (player2 && !(two_player && play->m_gameState.m_isDualMode)) return;
        double vel = player->m_yVelocity;
        if (player->m_isUpsideDown) vel = -vel;
        bool held = Logic::button_held(player, JUMP);
        if ((player2 ? straight_fly_p2 : straight_fly_p1) && player->m_isShip) {
            double limit = player2 ? straight_fly_threshold_p2 : straight_fly_threshold_p1;
            if ((vel < -limit && !held) || (vel > limit && held))
                play->queueButton(JUMP, !held, player2, click_stamp());
        }
    };
    run(play->m_player1, false);
    run(play->m_player2, true);
}

void Logic::tick_straight_ufo(PlayLayer* play) {
    if (!straight_ufo() || !play || play->m_isPaused || play->m_playerDied || predicting) return;
    if (is_playing() && !is_recording()) return;
    bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    auto run = [&](PlayerObject* player, bool player2) {
        int side = player2 ? 1 : 0;
        int& phase = straight_ufo_phase[side];
        if (!player || player->m_isDead || plays_player(player2) || !player->m_isBird || !(player2 ? straight_ufo_p2 : straight_ufo_p1)) {
            phase = 0;
            return;
        }
        if (player2 && !(two_player && play->m_gameState.m_isDualMode)) return;
        double up = player->m_isUpsideDown ? -1.0 : 1.0;
        double vel = player->m_yVelocity * up;
        double target = player2 ? straight_ufo_target_p2 : straight_ufo_target_p1;
        double band = player2 ? straight_ufo_threshold_p2 : straight_ufo_threshold_p1;
        double y = player->getPositionY();
        double off = (y - target) * up;
        bool black = false;
        if (auto* rings = player->m_touchingRings) {
            for (auto* object : CCArrayExt<GameObject*>(rings))
                black = black || (object && object->m_objectType == GameObjectType::DropRing);
        }
        int& gone = straight_ufo_gone[side];
        gone = black ? 0 : gone + 1;
        auto queue = [&](bool down) {
            play->queueButton(JUMP, down, player2, click_stamp());
        };
        if (phase != 0 && gone >= 3) {
            if (phase == 1) queue(false);
            phase = 0;
        }
        bool on_orb = black || phase != 0;
        if (!on_orb) {
            if (off < -band && vel <= 0.0) {
                queue(false);
                queue(true);
            }
            return;
        }
        bool up_step = off < -band ? true : off > band ? false : phase != 1;
        if (up_step) {
            queue(true);
            queue(false);
            queue(true);
            phase = 1;
        }
        else {
            queue(false);
            queue(true);
            queue(false);
            phase = 2;
        }
    };
    run(play->m_player1, false);
    run(play->m_player2, true);
}

int Logic::follow_steps() const {
    double steps = follow_delay_ms ? follow_delay / 1000.0 * sample_rate() : follow_delay;
    return std::max(0, (int)std::lround(steps));
}

bool Logic::follow_active(GJBaseGameLayer* play) const {
    bool macro = backstep_target >= 0 ? (backstep_state == RunState::Playing || backstep_state == RunState::RecordingAndPlaying) : is_playing();
    if (follow_mode == 0 || click_both || macro || !play) return false;
    return play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
}

void Logic::note_follow_press(GJBaseGameLayer* play, bool down, int button) {
    follow_log.push_back({ step_frame + 1, play->m_gameState.m_levelTime, down, button });
    while (follow_log.size() > 65536)
        follow_log.pop_front();
}

// delay is in lvl time NOT steps
bool Logic::follow_due(const FollowPress& press, int frame, double time) const {
    if (follow_delay_ms) return press.time + std::max(follow_delay, 0.f) / 1000.0 <= time + 1e-7;
    return press.frame + follow_steps() <= frame;
}

void Logic::rewind_follow(GJBaseGameLayer* play) {
    std::erase_if(follow_log, [this](const auto& press) {
        return press.frame > step_frame;
    });
    if (!play) return;
    double time = play->m_gameState.m_levelTime;
    double delay = std::max(follow_delay, 0.f) / 1000.0;
    for (auto& press : follow_log)
        press.sent = follow_delay_ms ? press.time + delay < time - 1e-7 : press.frame + follow_steps() <= step_frame;
}

void Logic::run_follow(GJBaseGameLayer* play) {
    if (!follow_active(play) || predicting) return;
    bool follower2 = follow_mode == 1;
    int frame = step_frame + 1;
    double time = play->m_gameState.m_levelTime;
    bool replaying = backstep_target >= 0;
    std::vector<std::pair<bool, int>> presses;
    for (auto& press : follow_log) {
        if (press.sent) continue;
        if (!follow_due(press, frame, time)) break;
        press.sent = true;
        if (!replaying)
            presses.push_back({ press.down, press.button });
    }
    for (auto [down, button] : presses) {
        follow_injecting = true;
        routing_input = true;
        play->handleButton(down, button == 0 ? JUMP : button, !follower2);
        routing_input = false;
        follow_injecting = false;
    }
}
