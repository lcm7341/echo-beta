// playback & everything around an attempt
//
// play_macro runs at the start of every physics step: the inputs stored for that step go into gds button queue,
//   so gd applies them exactly like a real press
// resets: held buttons, the queue, the seed, click counters & labels all go back to a known state. a key still down from
//   before the reset would fire again & look like a new press, so it is swallowed until its really let go
// holds across death & respawn are remembered & put back silently. no new press = no jump & no flipped gravity
// practice checkpoints, maintain / same gravity, follow player & the seed handling are in here too
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"
#include "GUI/gui.hpp"
#include "Hack/trajectory.hpp"
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJRobotSprite.hpp>
#include <Geode/binding/GJSpiderSprite.hpp>
#include <Geode/binding/HardStreak.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
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
    if (!play)
        return 0;
    return Logic::get().step_frame;
}

void Logic::toggle_recording() {
    auto& logic = Logic::get();
    if (logic.is_playing() && !logic.is_recording()) {
        bool record1 = logic.record_p1 && !logic.play_p1, record2 = logic.record_p2 && !logic.play_p2;
        if (!record1 && !record2) {
            logic.message = "That player is already playing";
            return;
        }
    }
    bool was_recording = logic.is_recording();
    if (logic.state == RunState::Idle) {
        logic.macro_unrounded = logic.velocity_unrounding;
        logic.macro_silicate_variance = false;
        auto* play = PlayLayer::get();
        // ldm takes objects OUT of the level (some are hazards) so the macro has to know
        if (logic.inputs.empty() && play && play->m_level) {
            logic.macro_ldm = play->m_level->m_lowDetailModeToggled;
            logic.macro_ldm_known = true;
        }
        if (logic.inputs.empty() && !logic.macro_name_typed && play && play->m_level && !std::string(play->m_level->m_levelName).empty()) {
            logic.macro_name = play->m_level->m_levelName;
            logic.macro_path.clear();
        }
    }
    switch (logic.state) {
    case RunState::Idle:
        logic.state = RunState::Recording;
        break;
    case RunState::Recording:
        logic.state = RunState::Idle;
        break;
    case RunState::Playing:
        logic.state = RunState::RecordingAndPlaying;
        break;
    case RunState::RecordingAndPlaying:
        logic.state = RunState::Playing;
        break;
    }
    logic.message = Logic::get().state_name(logic.state);
    // small buffer NOT 8mb per recording
    if (logic.is_recording() && !was_recording && logic.inputs.capacity() < 4096)
        logic.inputs.reserve(4096);
    if (was_recording && !(logic).is_recording())
        Logic::get().autosave_replay();
}

void Logic::toggle_playing() {
    auto& logic = Logic::get();
    switch (logic.state) {
    case RunState::Idle:
        logic.state = RunState::Playing;
        break;
    case RunState::Playing:
        logic.state = RunState::Idle;
        break;
    case RunState::Recording:
        logic.state = RunState::Playing;
        break;
    case RunState::RecordingAndPlaying:
        logic.state = RunState::Recording;
        break;
    }
    if ((logic).is_playing())
        Logic::get().set_replay_pos(PlayLayer::get());
    else
        logic.hold_sync = false;
    logic.message = Logic::get().state_name(logic.state);
}

bool Logic::steer_player(bool& isPlayer1) {
    bool record1 = records_player(false), record2 = records_player(true);
    if (record1 == record2)
        return false;
    isPlayer1 = !record2;
    return true;
}

PlayerObject* Logic::icon_body(GJBaseGameLayer* play, bool icon2) {
    if (!play)
        return nullptr;
    return icon2 ? play->m_player2 : play->m_player1;
}

bool Logic::play_input(GJBaseGameLayer* play, bool icon2, bool down, int button) {
    PlayerObject* body = icon_body(play, icon2);
    if (!body || body->m_isDead || body->m_controlsDisabled)
        return false;
    if (button == 0)
        button = static_cast<int>(PlayerButton::Jump);
    auto key = static_cast<PlayerButton>(button);
    if (down)
        body->pushButton(key);
    else
        body->releaseButton(key);
    return true;
}

bool Logic::physical_hold(GJBaseGameLayer* play, bool icon2) {
    if (!play || !play->m_uiLayer)
        return false;
    if (suppress_auto_hold)
        return false;
    auto* ui = play->m_uiLayer;
    bool p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1 || key_jump_p1 || (restart_hold_captured && death_jump_p1);
    bool p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1 || key_jump_p2 || (restart_hold_captured && death_jump_p2);
    if (restart_hold_captured && ((death_jump_p1 && (ui->m_p1Jumping || ui->m_p1TouchId != -1)) || (death_jump_p2 && (ui->m_p2Jumping || ui->m_p2TouchId != -1)))) {
        restart_hold_captured = false;
    }
    // maintain gravity reads what im holding so the trajectory autoclicker flips THAT, not the button
    if (traj_autoclicker && maintain_gravity) {
        p1 = p1 != traj_flip_p1;
        p2 = p2 != traj_flip_p2;
    }
    bool record1 = records_player(false), record2 = records_player(true);
    bool one_player_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    if (record1 != record2 && !one_player_dual) {
        if (icon2 != record2)
            return icon2 ? p2 : p1;
        return p1 || p2;
    }
    if (flip_controls())
        std::swap(p1, p2);
    if (one_player_dual || click_both)
        return p1 || p2;
    return icon2 ? p2 : p1;
}

bool Logic::steer_key(GJBaseGameLayer* play, enumKeyCodes key, bool down, double timestamp) {
    if (!play || resetting || refresh_only)
        return false;
    int code = static_cast<int>(key);
    auto held = steered_keys.find(code);
    if (!down && held != steered_keys.end()) {
        int button = held->second & 0xF;
        bool icon2 = (held->second >> 4) != 0;
        steered_keys.erase(held);
        bool still = std::ranges::any_of(steered_keys, [&](auto const& entry) {
            return entry.second == ((icon2 ? 1 : 0) << 4 | button);
        });
        if (button == static_cast<int>(PlayerButton::Jump))
            (icon2 ? key_jump_p2 : key_jump_p1) = still;
        if (!still)
            play->queueButton(button, false, icon2, timestamp);
        if (button == static_cast<int>(PlayerButton::Jump) && !still) {
            restart_hold_captured = false;
            death_jump_p1 = death_jump_p2 = false;
        }
        return true;
    }
    if (!down)
        return false;
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
    if (!steer_player(target1))
        return false;
    bool icon2 = !target1;
    int button = 0;
    switch (key) {
    case KEY_Space:
    case KEY_W:
    case KEY_Up:
        button = static_cast<int>(PlayerButton::Jump);
        break;
    case KEY_A:
    case KEY_Left:
        if (play->m_isPlatformer)
            button = static_cast<int>(PlayerButton::Left);
        break;
    case KEY_D:
    case KEY_Right:
        if (play->m_isPlatformer)
            button = static_cast<int>(PlayerButton::Right);
        break;
    default:
        break;
    }
    if (button == 0)
        return false;
    if (held != steered_keys.end())
        return true;
    int tag = (icon2 ? 1 : 0) << 4 | button;
    bool already = std::ranges::any_of(steered_keys, [&](auto const& entry) {
        return entry.second == tag;
    });
    steered_keys[code] = tag;
    if (button == static_cast<int>(PlayerButton::Jump)) {
        (icon2 ? key_jump_p2 : key_jump_p1) = true;
    }
    if (!already)
        play->queueButton(button, true, icon2, timestamp);
    return true;
}

void Logic::set_routed_hold(bool icon2, int button, bool down) {
    if (button == 0)
        button = static_cast<int>(PlayerButton::Jump);
    auto& logic = Logic::get();
    auto set = [&](bool& target) {
        target = down;
    };
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    if (button == jump) {
        if (icon2)
            set(logic.physical_jump_p2);
        else
            set(logic.physical_jump_p1);
    }
    else if (button == left) {
        if (icon2)
            set(logic.physical_left_p2);
        else
            set(logic.physical_left_p1);
    }
    else if (button == right) {
        if (icon2)
            set(logic.physical_right_p2);
        else
            set(logic.physical_right_p1);
    }
}

void Logic::reinforce_routed_hold(PlayerObject* body) {
    auto* layer = GJBaseGameLayer::get();
    if (!layer || !body || maintain_gravity || suppress_auto_hold)
        return;
    bool icon2 = body == icon_body(layer, true);
    if (!icon2 && body != icon_body(layer, false))
        return;
    if (!records_player(icon2))
        return;
    body->m_controlsDisabled = false;
    auto hold = [&](bool want, PlayerButton key) {
        if (!want || Logic::button_held(body, static_cast<int>(key)))
            return;
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
    if (!play)
        return;
    std::ranges::for_each(play->m_queuedButtons, [](PlayerButtonCommand const& cmd) {
        Logic::get().remember_move(cmd);
    });
    bool from_checkpoint = Logic::get().get_latest_checkpoint(play);
    if (from_checkpoint) {
        suppress_auto_hold = false;
        return;
    }
    // holding thru death must NOT hold the next attempt
    // but if nothing is down there is nothing to block (it ate the first click)
    bool down = Input::jump_key_down() || Input::mouseDown() || key_jump_p1 || key_jump_p2 || physical_jump_p1 || physical_jump_p2;
    if (auto* ui = play->m_uiLayer)
        down = down || ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
    Logic::get().clear_physical_holds();
    if (auto* ui = play->m_uiLayer) {
        ui->m_p1Jumping = false;
        ui->m_p2Jumping = false;
        ui->m_p1TouchId = -1;
        ui->m_p2TouchId = -1;
    }
    suppress_auto_hold = down;
    if (play->m_player1)
        play->m_player1->releaseAllButtons();
    if (play->m_player2)
        play->m_player2->releaseAllButtons();
}

// the block only ended inside a step so frame advance never got out of it
void Logic::poll_hold_block() {
    if (!suppress_auto_hold)
        return;
    bool down = Input::jump_key_down() || Input::mouseDown();
    if (auto* play = PlayLayer::get(); play && play->m_uiLayer) {
        auto* ui = play->m_uiLayer;
        down = down || ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
    }
    if (!down)
        suppress_auto_hold = false;
}

// a step back loads the buttons of THAT frame, not what im pressing now
// so next step press/release whatever is different
void Logic::match_live_holds(PlayLayer* play) {
    if (!play || suppress_auto_hold)
        return;
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    for (bool icon2 : { false, true }) {
        if (icon2 && !two)
            continue;
        auto* body = icon_body(play, icon2);
        if (!body || plays_player(icon2))
            continue;
        auto match = [&](int button, bool want) {
            if (want == button_held(body, button))
                return;
            PlayerButtonCommand cmd;
            cmd.m_button = static_cast<PlayerButton>(button);
            cmd.m_isPush = want;
            cmd.m_isPlayer2 = icon2;
            cmd.m_step = step_frame + 1;
            cmd.m_timestamp = 0.0;
            play->m_queuedButtons.push_back(cmd);
        };
        bool held = physical_hold(play, icon2);
        if (maintain_gravity && body->m_isDart && records_player(icon2))
            held = held != body->m_isUpsideDown;
        match(jump, held);
        if (!play->m_isPlatformer)
            continue;
        match(left, icon2 ? key_left_p2 || physical_left_p2 : key_left_p1 || physical_left_p1);
        match(right, icon2 ? key_right_p2 || physical_right_p2 : key_right_p1 || physical_right_p1);
    }
}

void Logic::apply_maintain_gravity(GJBaseGameLayer* play) {
    // TODO
}

void Logic::sync_same_gravity(GJBaseGameLayer* play) {
    // port this
}

void Logic::toggle_frame_advance() {
    auto& logic = Logic::get();
    logic.frame_advance = !logic.frame_advance;
    logic.advance_queued = false;
    if (!logic.frame_advance)
        Logic::get().stop_best_frame();
    logic.holding_advance = false;
    if (logic.frame_advance)
        Logic::get().disarm_length_pause_if_holding();
    logic.message = logic.frame_advance ? "Frame advance on" : "Frame advance off";
}

void Logic::disarm_length_pause_if_holding() {
    auto& logic = Logic::get();
    if (!logic.pause_at_traj_length)
        return;
    auto* play = PlayLayer::get();
    if (!play || !play->m_player1)
        return;
    if (Logic::get().physical_hold(play, false) || Logic::get().physical_hold(play, true)) {
        logic.pause_length_armed = false;
    }
}

void Logic::advance_one_frame() {
    auto& logic = Logic::get();
    bool was_off = !logic.frame_advance;
    if (!logic.frame_advance)
        logic.frame_advance = true;
    if (was_off)
        Logic::get().disarm_length_pause_if_holding();
    logic.advance_steps = 1;
    logic.advance_queued = true;
}

void Logic::poll_advance_hold() {
    auto& logic = Logic::get();
    if (!logic.frame_advance || !logic.holding_advance || !PlayLayer::get())
        return;
    if (GUI::get().typing()) {
        logic.holding_advance = false;
        return;
    }
    auto now = chrono::steady_clock::now();
    auto start = logic.advance_hold_start + chrono::milliseconds(logic.advance_hold_ms);
    if (now < start)
        return;
    auto from = std::max(start, logic.last_advance_step);
    double rate = logic.sample_rate() * std::clamp(logic.advance_speed, 1.f, 200.f) / 100.0 * logic.speedhack;
    if (!std::isfinite(rate) || !(rate > 0.0))
        return;
    double elapsed = chrono::duration<double>(now - from).count();
    int cap = std::min(logic.unlocked_step_cap(), logic.max_steps_for_frame());
    double wanted = std::floor(elapsed * rate);
    if (wanted < 1.0)
        return;
    int steps = static_cast<int>(std::min(wanted, static_cast<double>(cap)));
    logic.last_advance_step = wanted > cap ? now : from + chrono::duration_cast<decltype(now - from)>(chrono::duration<double>(steps / rate));
    logic.advance_steps = logic.advance_queued ? std::min(cap, logic.advance_steps + steps) : steps;
    logic.advance_queued = true;
}

void Logic::check_advance_pause(PlayLayer* play) {
    if (!play || play->m_playerDied || Logic::get().render_capturing())
        return;
    auto& logic = Logic::get();
    if (!logic.pause_before_kill || !logic.is_recording())
        return;
    auto watch = [&](bool player2) {
        if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2))
            return false;
        if (logic.is_recording())
            return logic.records_player(player2);
        return true;
    };
    bool death_next = false;
    if (watch(false) && logic.death_next_p1)
        death_next = true;
    if (watch(true) && logic.death_next_p2)
        death_next = true;
    if (!death_next) {
        logic.pause_kill_armed = true;
        return;
    }
    bool before_kill = logic.pause_kill_armed;
    if (!before_kill)
        return;
    bool was_holding = logic.holding_advance;
    bool was_off = !logic.frame_advance;
    if (!was_off && !was_holding)
        return;
    logic.frame_advance = true;
    logic.holding_advance = false;
    logic.advance_queued = false;
    logic.pause_kill_armed = false;
    if (was_off || was_holding) {
        logic.message = "Paused before kill";
    }
}

void Logic::on_keybind(std::string_view key, bool down) {
    if (down && GUI::get().typing())
        return;
    if (key == "open-menu") {
        if (down)
            GUI::get().open_popup();
        return;
    }
    if (!down && key == "step-back") {
        Logic::get().holding_back = false;
        return;
    }
    if (!down && key == "step-frame") {
        Logic::get().holding_advance = false;
        Logic::get().disarm_length_pause_if_holding();
        return;
    }
    if (!down)
        return;
    if (key == "find-best-frame") {
        Logic::get().request_best_frame();
        return;
    }
    if (key == "step-back") {
        auto now = chrono::steady_clock::now();
        Logic::get().holding_back = true;
        Logic::get().back_hold_start = now;
        Logic::get().last_back_step = now;
        return;
    }
    if (key == "step-frame") {
        auto now = chrono::steady_clock::now();
        Logic::get().holding_advance = true;
        Logic::get().advance_hold_start = now;
        Logic::get().last_advance_step = now;
        Logic::get().advance_one_frame();
        return;
    }
    Logic::get().dispatch_setting_key(key, true);
}

void Logic::resume_attempt(PlayLayer* play) {
    if (!play)
        return;
    auto& logic = Logic::get();
    logic.resume_pending = true;
    logic.carry = 0.f;
    logic.present = true;
    play->m_extraDelta = 0.0;
    play->m_resumeTimer = 0;
    bool menu = Logic::get().pause_menu_open();
    if (!menu)
        play->m_isPaused = false;
    if (auto* director = CCDirector::sharedDirector()) {
        if (!menu && director->isPaused())
            director->resume();
        director->setNextDeltaTimeZero(false);
    }
    Logic::get().sync_time_scale();
}

void Logic::retain_playback_hold(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!play || !logic.is_playing())
        return;
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    auto hold = [&](bool icon2) {
        if (!logic.plays_player(icon2))
            return;
        auto* body = Logic::get().icon_body(play, icon2);
        if (!body)
            return;
        auto keep = [&](int button, bool want) {
            if (!want)
                return;
            body->m_holdingButtons[button] = true;
            if (button == jump)
                body->m_jumpBuffered = true;
            if (button == left)
                body->m_holdingLeft = true;
            if (button == right)
                body->m_holdingRight = true;
        };
        int frame = logic.step_frame;
        keep(jump, Logic::get().held_at(logic, icon2, frame, jump));
        if (!play->m_isPlatformer)
            return;
        keep(left, Logic::get().held_at(logic, icon2, frame, left));
        keep(right, Logic::get().held_at(logic, icon2, frame, right));
    };
    hold(false);
    hold(true);
}

void Logic::on_focus_change() {
    auto& logic = Logic::get();
    logic.carry = 0.f;
    logic.present = true;
    if (auto* director = CCDirector::sharedDirector())
        director->setNextDeltaTimeZero(true);
    if (auto* play = PlayLayer::get()) {
        play->m_extraDelta = 0.0;
        if (!play->m_playerDied) {
            if (play->m_player1)
                play->m_player1->m_isDead = false;
            if (play->m_player2)
                play->m_player2->m_isDead = false;
        }
    }
    Logic::get().sync_time_scale();
}

namespace {
    struct HeldLook {
        bool held = false;
        bool wave = false;
        bool regular = false;
        bool ship = false;
        bool ghost = false;
        bool fire_mid = false, fire_bot = false, fire_top = false;
        bool dash = false;
    };

    HeldLook* held_looks() {
        static HeldLook players[2]{};
        return players;
    }

    HeldLook& held_look(PlayerObject* player) {
        auto* play = PlayLayer::get();
        bool second = play && player == play->m_player2;
        return held_looks()[second ? 1 : 0];
    }

    void take_node(CCNode* node, bool& saved, bool already) {
        if (!node)
            return;
        if (!already)
            saved = node->isVisible();
        node->setVisible(false);
    }

    void give_node(CCNode* node, bool saved) {
        if (node)
            node->setVisible(saved);
    }

    void stop_particle(CCParticleSystemQuad* system) {
        if (!system)
            return;
        system->setVisible(false);
        system->stopSystem();
    }

    void show_particle(CCParticleSystemQuad* system) {
        if (system)
            system->setVisible(true);
    }

    void restore_player(PlayerObject* player, HeldLook const& held) {
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
        give_node(player->m_waveTrail, held.wave);
        give_node(player->m_regularTrail, held.regular);
        give_node(player->m_shipStreak, held.ship);
        give_node(player->m_ghostTrail, held.ghost);
        give_node(player->m_swingFireMiddle, held.fire_mid);
        give_node(player->m_swingFireBottom, held.fire_bot);
        give_node(player->m_swingFireTop, held.fire_top);
        give_node(player->m_dashSpritesContainer, held.dash);
        show_particle(player->m_playerGroundParticles);
        show_particle(player->m_trailingParticles);
        show_particle(player->m_shipClickParticles);
        show_particle(player->m_vehicleGroundParticles);
        show_particle(player->m_ufoClickParticles);
        show_particle(player->m_robotBurstParticles);
        show_particle(player->m_dashParticles);
        show_particle(player->m_swingBurstParticles1);
        show_particle(player->m_swingBurstParticles2);
        show_particle(player->m_landParticles0);
        show_particle(player->m_landParticles1);
    }
}

bool Logic::player_hidden(PlayerObject* player) const {
    // TODO
    return {};
}

bool Logic::hitbox_player_hidden(bool player2) const {
    if (hitboxes_follow_hide_player)
        return player2 ? hide_p2 : hide_p1;
    return player2 ? hide_hitbox_p2 : hide_hitbox_p1;
}

bool Logic::traj_player_hidden(bool player2) const {
    if (traj_follow_hide_player)
        return player2 ? hide_p2 : hide_p1;
    return player2 ? hide_traj_p2 : hide_traj_p1;
}

void Logic::hide_icon(PlayerObject* player, bool hide) {
    // port this
}

void Logic::after_frame(PlayLayer* play, float dt) {
    auto& logic = Logic::get();
    if (!play || logic.resetting)
        return;
    Logic::get().flush_click_hitboxes(play);
    Logic::get().hide_icon(play->m_player1, logic.hide_p1);
    Logic::get().hide_icon(play->m_player2, logic.hide_p2);
    if ((logic).is_recording())
        logic.recording_seconds += static_cast<double>(dt);
    if (play->m_hasCompletedLevel && (logic).is_recording()) {
        logic.state = (logic).is_playing() ? RunState::Playing : RunState::Idle;
    }
    if (play->m_endPortal)
        logic.end_x = play->m_endPortal->getPositionX();
    if (logic.arm_immediate_restart && play->m_uiLayer && play->m_attemptLabel && !Logic::get().pause_menu_open()) {
        logic.arm_immediate_restart = false;
        Loader::get()->queueInMainThread([] {
            auto* layer = PlayLayer::get();
            if (!layer || Logic::get().pause_menu_open() || !layer->m_uiLayer || !layer->m_attemptLabel)
                return;
            layer->resetLevel();
        });
    }
    if (!Logic::get().render_capturing())
        Logic::get().update_cps(play);
    if (!logic.present)
        return;
    Logic::get().apply_layout(play);
    if (!logic.in_scheduler)
        Logic::get().draw_hitboxes(play);
    Logic::get().draw_overlay(play);
}

void Logic::on_play_init(PlayLayer* play) {
    held_looks()[0] = {};
    held_looks()[1] = {};
    auto& logic = Logic::get();
    logic.replay_pos = 0;
    logic.replay_index = 0;
    logic.sequence_order.clear();
    logic.step_frame = 0;
    logic.progress_base_set = false;
    logic.removed_frames = 0;
    logic.carry = 0.f;
    logic.respawn_frames = 0;
    logic.ignore_record_frames = 0;
    logic.refresh_only = false;
    logic.refresh_queued = false;
    logic.in_scheduler = false;
    logic.buttons_prepared = false;
    logic.replay_clicks_left = 0;
    Logic::get().cancel_steps();
    Logic::get().clear_click_hitbox();
    Logic::get().apply_rate(0);
    logic.present = true;
    logic.layout_styled = false;
    Logic::get().clear_stuck_death(play);
    Logic::get().clear_physical_holds();
    logic.checkpoints.clear();
    logic.restored_checkpoint = nullptr;
    logic.clicks_p1.clear();
    logic.clicks_p2.clear();
    ++logic.clicks_revision;
    logic.attempts = 1;
    logic.trail_p1.clear();
    logic.trail_p2.clear();
    logic.hidden_decorations.clear();
    logic.last_click_p1 = 0.0;
    logic.last_click_p2 = 0.0;
    if (play->m_level) {
        logic.macro_name = play->m_level->m_levelName;
        if (logic.macro_name.empty())
            logic.macro_name = "Replay";
        logic.macro_name_typed = false;
    }
    if (logic.lock_display_fps) {
        CCDirector::sharedDirector()->setAnimationInterval(1.f / std::max(logic.fps, 1.f));
        logic.display_fps_locked = true;
    }
    Logic::get().sync_time_scale();
    Logic::get().reset_opening_music_hold();
    if (logic.seed_override) {
        logic.lvl_seed = logic.seed_override_value;
        logic.lvl_seed_known = true;
        logic.lvl_seed_from_file = false;
    }
    else if (!logic.is_playing() && !logic.lvl_seed_from_file) {
        logic.lvl_seed = 0;
        logic.lvl_seed_known = false;
    }
    logic.arm_immediate_restart = logic.immediate_restart;
}

namespace {
    struct LiveRand {
        int unique_id = 0;
        uint64_t* state = nullptr;
    };

    std::vector<LiveRand>& live_rand() {
        static std::vector<LiveRand> triggers;
        return triggers;
    }

    struct VarianceLock {
        bool held = false;
        std::array<float, 2000> values{};
    };

    VarianceLock& variance_lock() {
        static VarianceLock lock;
        return lock;
    }

    void fill_variance(PlayLayer* play, uint64_t seed) {
        uint64_t state = seed;
        for (float& slot : play->m_varianceValues) {
            slot = static_cast<float>(state & 0xFFFF) / 32768.f - 1.f;
            state = 214013ull * state + 2531011ull; // msvc rand()
        }
    }
}

uint64_t Logic::game_random_state() {
    return GameToolbox::getfast_srand();
}

void Logic::set_game_random_state(uint64_t state) {
    GameToolbox::fast_srand(state);
}

void Logic::note_rand_trigger(int unique_id, uint64_t* state) {
    if (!state)
        return;
    for (auto const& trigger : live_rand()) {
        if (trigger.state == state)
            return;
    }
    live_rand().push_back({ unique_id, state });
}

void Logic::clear_rand_triggers() {
    live_rand().clear();
}

void Logic::reseed_rand_triggers(uint64_t seed) {
    for (auto& trigger : live_rand()) {
        if (!trigger.state)
            continue;
        *trigger.state = seed ^ (static_cast<uint64_t>(trigger.unique_id) * 2137ull);
    }
}

void Logic::roll_live_seed(PlayLayer* play) {
    static std::mt19937_64 rng{ std::random_device{}() };
    auto& logic = Logic::get();
    uint64_t rolled = rng();
    if (rolled == 0)
        rolled = 1;
    variance_lock().held = false;
    if (!logic.lvl_seed_from_file || logic.is_recording()) {
        logic.lvl_seed = rolled;
        logic.lvl_seed_known = true;
        if (logic.is_recording())
            logic.lvl_seed_from_file = false;
    }
    if (!play)
        return;
    play->m_randomSeed = rolled;
    play->m_replayRandSeed = rolled;
    play->m_unk32e0 = rolled;
    Logic::get().set_game_random_state(rolled);
    logic.shake_state = rolled & 0x7FFF;
    logic.tp_state = rolled & 0x7FFF;
    fill_variance(play, rolled);
    Logic::get().reseed_rand_triggers(rolled);
}

void Logic::apply_attempt_variance(PlayLayer* play, uint64_t seed) {
    if (!play)
        return;
    fill_variance(play, seed);
    auto& lock = variance_lock();
    lock.held = true;
    std::copy(play->m_varianceValues.begin(), play->m_varianceValues.end(), lock.values.begin());
}

void Logic::keep_variance(PlayLayer* play) {
    auto& lock = variance_lock();
    auto const& logic = Logic::get();
    if (!play || !lock.held)
        return;
    if (!logic.seed_override && !logic.is_playing())
        return;
    replay::restore_variance(play->m_varianceValues, lock.values);
}

void Logic::plant_lvl_seed(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!play || logic.keep_lvl_seed)
        return;
    if (logic.seed_override) {
        logic.lvl_seed = logic.seed_override_value;
        logic.lvl_seed_known = true;
    }
    if (!logic.lvl_seed_known)
        return;
    play->m_randomSeed = logic.lvl_seed;
    play->m_replayRandSeed = logic.lvl_seed;
    play->m_unk32e0 = logic.lvl_seed;
    Logic::get().set_game_random_state(logic.lvl_seed);
    logic.shake_state = logic.lvl_seed & 0x7FFF;
    logic.tp_state = logic.lvl_seed & 0x7FFF;
    Logic::get().apply_attempt_variance(play, logic.lvl_seed);
    Logic::get().reseed_rand_triggers(logic.lvl_seed);
}

void Logic::restore_checkpoint_random(PlayLayer* play) {
    auto& logic = Logic::get();
    auto* saved = Logic::get().get_latest_checkpoint(play);
    if (!saved)
        return;
    auto const& checkpoint = *saved;
    Logic::get().set_game_random_state(checkpoint.random_state);
    logic.shake_state = checkpoint.shake_state;
    logic.tp_state = checkpoint.tp_state;
    if (checkpoint.variance.size() == play->m_varianceValues.size()) {
        std::copy(checkpoint.variance.begin(), checkpoint.variance.end(), play->m_varianceValues.begin());
        auto& lock = variance_lock();
        lock.held = true;
        std::copy(play->m_varianceValues.begin(), play->m_varianceValues.end(), lock.values.begin());
    }
    for (auto const& saved : checkpoint.rand_states) {
        for (auto& trigger : live_rand()) {
            if (trigger.unique_id == saved.first && trigger.state)
                *trigger.state = saved.second;
        }
    }
}

void reshuffle_sequence(Session& value) {
    value.sequence_order.resize(value.sequence.size());
    std::iota(value.sequence_order.begin(), value.sequence_order.end(), size_t{ 0 });
    std::mt19937 rng{ std::random_device{}() };
    std::shuffle(value.sequence_order.begin(), value.sequence_order.end(), rng);
    value.replay_index = 0;
}

void Logic::onReset(PlayLayer* play) {
    auto& logic = Logic::get();
    Logic::get().clear_click_hitbox();
    logic.trail_p1.clear();
    logic.trail_p2.clear();
    Logic::get().reset_opening_music_hold();
    if (!Logic::get().layout_active()) {
        for (auto* obj : logic.hidden_decorations) {
            if (obj)
                obj->m_isHide = false;
        }
        logic.hidden_decorations.clear();
        logic.layout_styled = false;
    }
    logic.currently_pressing = false;
    logic.death_next_p1 = false;
    logic.death_next_p2 = false;
    logic.noclip_dead_p1 = false;
    logic.noclip_dead_p2 = false;
    logic.showed_new_best = false;
    logic.over_max_cps = false;
    logic.cps_breaking = false;
    if (play->m_endPortal)
        logic.end_x = play->m_endPortal->getPositionX();
    if ((logic).is_recording() && !logic.backstepping)
        ++logic.attempts;

    if (logic.sequence_enabled && !(logic).is_recording() && !logic.backstepping) {
        auto use_clip = [&](Replay const& clip) {
            logic.inputs = clip.actions;
            logic.clear_sides();
            logic.macro_name = clip.name;
            logic.macro_name_typed = false;
            logic.macro_fps = clip.fps;
            logic.rates = clip.rates;
            logic.lvl_seed = clip.lvl_seed;
            logic.lvl_seed_known = clip.lvl_seed_known;
            logic.lvl_seed_from_file = clip.lvl_seed_known;
            logic.macro_unrounded = clip.unrounded;
            logic.macro_silicate_variance = clip.silicate_variance;
            Logic::get().apply_rate(logic.step_frame);
        };
        if (logic.sequence_random && !logic.sequence.empty()) {
            if (logic.sequence_order.size() != logic.sequence.size() || logic.replay_index >= logic.sequence_order.size())
                reshuffle_sequence(logic);
            size_t pick = logic.sequence_order[logic.replay_index];
            use_clip(logic.sequence[pick]);
            ++logic.replay_index;
            Logic::get().sort_inputs();
        }
        else if (logic.replay_index < logic.sequence.size()) {
            use_clip(logic.sequence[logic.replay_index]);
            ++logic.replay_index;
            Logic::get().sort_inputs();
        }
        else {
            logic.replay_index = 0;
        }
    }

    bool const practice = play->m_isPracticeMode || logic.backstepping;
    auto* saved = Logic::get().get_latest_checkpoint(play);
    bool const from_checkpoint = practice && saved;
    if (from_checkpoint) {
        logic.step_frame = saved->frame;
    }
    else {
        logic.step_frame = 0;
        logic.progress_base_set = false;
        logic.removed_frames = 0;
        logic.same_gravity_seen = false;
    }
    Logic::get().apply_rate(logic.step_frame);
    auto replayed = [&logic](int frame) {
        return frame >= logic.step_frame;
    };
    std::erase_if(logic.clicks_p1, replayed);
    std::erase_if(logic.clicks_p2, replayed);
    Logic::get().rewind_follow(play);
    ++logic.clicks_revision;
    play->m_extraDelta = 0.0;
    play->m_resumeTimer = 0;
    if (auto* director = CCDirector::sharedDirector())
        director->setNextDeltaTimeZero(false);
    Logic::get().clear_stuck_death(play);
    logic.present = true;

    if (logic.sort_automatically && (logic).is_recording() && (logic).is_playing())
        Logic::get().sort_inputs();
    if (from_checkpoint) {
        if (!logic.backstepping)
            logic.object_restore = saved->object;
        Logic::get().trim_from_checkpoint(play, logic, *saved);
        play->m_queuedButtons.clear();
    }
    else if ((logic).is_recording()) {
        Logic::get().trim_fresh_attempt(play, logic);
    }
    if (from_checkpoint) {
        play->m_extraDelta = 0.0;
        logic.refresh_queued = true;
        if (!logic.in_scheduler && logic.scheduler_depth == 0)
            Logic::get().flush_visual_refresh();
    }
    if (!practice)
        logic.checkpoints.clear();
    if ((logic).is_playing())
        Logic::get().set_replay_pos(play);
    Trajectory::get().invalidate();
    Trajectory::get().refresh(play);
    Logic::get().restore_holds(play);
    if (from_checkpoint) {
        logic.resume_pending = false;
        play->m_extraDelta = 0.0;
        play->m_resumeTimer = 0;
        if (!Logic::get().pause_menu_open())
            play->m_isPaused = false;
        Logic::get().clear_stuck_death(play);
        Logic::get().sync_time_scale();
    }
    else {
        Logic::get().resume_attempt(play);
    }
}

void Logic::restore_holds(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!play)
        return;
    if (!logic.is_playing() && !logic.is_recording()) {
        logic.restart_hold_captured = false;
        logic.death_jump_p1 = logic.death_jump_p2 = false;
        logic.death_left_p1 = logic.death_right_p1 = false;
        logic.death_left_p2 = logic.death_right_p2 = false;
        logic.key_jump_p1 = logic.key_jump_p2 = false;
        logic.physical_jump_p1 = logic.physical_jump_p2 = false;
        logic.steered_keys.clear();
        bool p1 = false, p2 = false;
        if (auto* ui = play->m_uiLayer) {
            p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1;
            p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1;
        }
        if (!Input::jump_key_down() && !Input::mouseDown())
            p1 = p2 = false;
        play->m_queuedButtons.clear();
        if (play->m_player1)
            play->m_player1->releaseAllButtons();
        if (play->m_player2)
            play->m_player2->releaseAllButtons();
        bool checkpoint = Logic::get().get_latest_checkpoint(play);
        bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
        bool shared = play->m_gameState.m_isDualMode && !two;
        bool allow = checkpoint && !logic.suppress_auto_hold;
        auto restore = [allow](PlayerObject* body, bool held) {
            if (!body)
                return;
            held = allow && held;
            body->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = held;
            body->m_jumpBuffered = held;
            if (!held) {
                body->m_wasJumpBuffered = false;
                body->m_stateJumpBuffered = 0;
            }
        };
        restore(play->m_player1, p1);
        restore(play->m_player2, shared ? p1 : two && p2);
        if (auto* ui = play->m_uiLayer) {
            ui->m_p1Jumping = allow && p1;
            ui->m_p2Jumping = allow && (shared ? p1 : two && p2);
            if (!ui->m_p1Jumping)
                ui->m_p1TouchId = -1;
            if (!ui->m_p2Jumping)
                ui->m_p2TouchId = -1;
        }
        return;
    }
    for (auto const& cmd : play->m_queuedButtons)
        Logic::get().remember_move(cmd);
    play->m_queuedButtons.clear();

    bool playing = (logic).is_playing();
    bool recording = (logic).is_recording();
    bool checkpoint = Logic::get().get_latest_checkpoint(play);
    if (checkpoint && !playing && !recording)
        return;
    if (playing && !recording) {
        if (logic.checkpoints.empty()) {
            if (play->m_player1)
                play->m_player1->releaseAllButtons();
            if (play->m_player2)
                play->m_player2->releaseAllButtons();
        }
    }
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    auto put_hold = [&](bool icon2, int button, bool want) {
        auto* body = Logic::get().icon_body(play, icon2);
        if (!body)
            return;
        bool have = Logic::button_held(body, button);
        bool wave = button == jump && body->m_isDart && logic.maintain_gravity && logic.records_player(icon2);
        if (!want && button == jump)
            body->m_jumpBuffered = false;
        if (!want && button == left)
            body->m_holdingLeft = false;
        if (!want && button == right)
            body->m_holdingRight = false;
        if (have == want && !(wave && body->m_jumpBuffered != want))
            return;
        body->m_holdingButtons[button] = want;
        if (button == jump)
            body->m_jumpBuffered = want;
        if (button == left)
            body->m_holdingLeft = want;
        if (button == right)
            body->m_holdingRight = want;
    };
    std::vector<PlayerButtonCommand> deferred;
    bool one_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    if (recording) {
        if (logic.suppress_auto_hold) {
            if (play->m_player1)
                play->m_player1->releaseAllButtons();
            if (play->m_player2)
                play->m_player2->releaseAllButtons();
            if (auto* ui = play->m_uiLayer) {
                ui->m_p1Jumping = false;
                ui->m_p2Jumping = false;
                ui->m_p1TouchId = -1;
                ui->m_p2TouchId = -1;
            }
        }
        auto jump_key_down = &Input::jump_key_down;
        auto left_key_down = &Input::left_key_down;
        auto rightKeyDown = &Input::rightKeyDown;
        auto mouseDown = &Input::mouseDown;
        auto live_jump = [&](bool icon2) {
            bool ui_down = false;
            if (auto* ui = play->m_uiLayer) {
                if (icon2)
                    ui_down = ui->m_p2Jumping || ui->m_p2TouchId != -1;
                else
                    ui_down = ui->m_p1Jumping || ui->m_p1TouchId != -1;
            }
            bool down = ui_down || (icon2 ? logic.key_jump_p2 : logic.key_jump_p1) || (icon2 ? logic.physical_jump_p2 : logic.physical_jump_p1) || jump_key_down();
            if (logic.suppress_auto_hold) {
                if (!jump_key_down() && !ui_down) {
                    logic.key_jump_p1 = logic.key_jump_p2 = false;
                    logic.physical_jump_p1 = logic.physical_jump_p2 = false;
                    logic.suppress_auto_hold = false;
                }
                return false;
            }
            if (ui_down)
                return true;
            if (!jump_key_down() && !mouseDown()) {
                (icon2 ? logic.key_jump_p2 : logic.key_jump_p1) = false;
                (icon2 ? logic.physical_jump_p2 : logic.physical_jump_p1) = false;
                return false;
            }
            if (icon2 ? logic.key_jump_p2 : logic.key_jump_p1)
                return true;
            if (icon2 ? logic.physical_jump_p2 : logic.physical_jump_p1)
                return true;
            bool sticky = logic.restart_hold_captured && (icon2 ? logic.death_jump_p2 : logic.death_jump_p1);
            return sticky && jump_key_down();
        };
        auto live_side = [&](bool icon2, bool sticky, bool& key, bool& physical, bool down) {
            if (logic.suppress_auto_hold)
                return false;
            if (!down) {
                key = false;
                physical = false;
                return false;
            }
            if (key || physical)
                return true;
            return logic.restart_hold_captured && sticky;
        };
        auto live = [&](bool icon2) {
            if (logic.suppress_auto_hold) {
                logic.restoring_holds = true;
                put_hold(icon2, jump, false);
                logic.restoring_holds = false;
                if (auto* body = Logic::get().icon_body(play, icon2))
                    body->releaseAllButtons();
                return;
            }
            if (!logic.records_player(icon2)) {
                if (!one_dual || logic.plays_player(icon2) || !logic.records_player(!icon2))
                    return;
                auto follow = [&](int button) {
                    put_hold(icon2, button, Logic::get().held_at(logic, false, logic.step_frame, button));
                };
                logic.restoring_holds = true;
                follow(jump);
                if (play->m_isPlatformer) {
                    follow(left);
                    follow(right);
                }
                logic.restoring_holds = false;
                return;
            }
            if (logic.plays_player(icon2))
                return;
            bool jump_held = live_jump(icon2);
            auto* body = Logic::get().icon_body(play, icon2);
            if (logic.maintain_gravity && body && body->m_isDart)
                jump_held = jump_held != body->m_isUpsideDown;
            bool left_held = live_side(
                icon2,
                icon2 ? logic.death_left_p2 : logic.death_left_p1,
                icon2 ? logic.key_left_p2 : logic.key_left_p1,
                icon2 ? logic.physical_left_p2 : logic.physical_left_p1,
                left_key_down());
            bool right_held = live_side(
                icon2,
                icon2 ? logic.death_right_p2 : logic.death_right_p1,
                icon2 ? logic.key_right_p2 : logic.key_right_p1,
                icon2 ? logic.physical_right_p2 : logic.physical_right_p1,
                rightKeyDown());
            auto note = [&](int button, bool held) {
                bool stored = Logic::get().held_at(logic, icon2 && !one_dual, logic.step_frame, button);
                logic.restoring_holds = true;
                put_hold(icon2, button, stored);
                logic.restoring_holds = false;
                if (held == stored)
                    return;
                PlayerButtonCommand cmd;
                cmd.m_button = static_cast<PlayerButton>(button);
                cmd.m_isPush = held;
                cmd.m_isPlayer2 = icon2;
                cmd.m_step = logic.step_frame + 1;
                cmd.m_timestamp = 0.0;
                deferred.push_back(cmd);
            };
            note(jump, jump_held);
            if (!jump_held && play->m_uiLayer) {
                if (icon2) {
                    play->m_uiLayer->m_p2Jumping = false;
                    play->m_uiLayer->m_p2TouchId = -1;
                }
                else {
                    play->m_uiLayer->m_p1Jumping = false;
                    play->m_uiLayer->m_p1TouchId = -1;
                }
            }
            if (!play->m_isPlatformer)
                return;
            note(left, left_held);
            note(right, right_held);
        };
        live(false);
        live(true);
    }
    if (checkpoint && playing) {
        logic.restoring_holds = true;
        int frame = logic.step_frame;
        auto ensure = [&](bool stored2, int button) {
            if (!logic.plays_player(stored2))
                return;
            bool want = Logic::get().held_at(logic, stored2, frame, button);
            if (logic.records_player(stored2))
                logic.applying_playback = false;
            else
                logic.applying_playback = true;
            put_hold(stored2, button, want);
            logic.applying_playback = false;
        };
        ensure(false, jump);
        if (play->m_isPlatformer) {
            ensure(false, left);
            ensure(false, right);
        }
        if (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode) {
            ensure(true, jump);
            if (play->m_isPlatformer) {
                ensure(true, left);
                ensure(true, right);
            }
        }
    }
    // NOT while a step back is loading
    if ((playing || recording) && !logic.backstepping)
        Logic::get().play_macro(play, play, 0.f, true);
    std::ranges::for_each(deferred, [play](PlayerButtonCommand const& cmd) {
        play->m_queuedButtons.push_back(cmd);
    });
    if (recording && checkpoint)
        Logic::get().sort_inputs();
    logic.restoring_holds = false;
    logic.restart_hold_captured = false;
    logic.death_jump_p1 = logic.death_jump_p2 = false;
    logic.death_left_p1 = logic.death_right_p1 = false;
    logic.death_left_p2 = logic.death_right_p2 = false;
}

void Logic::save_checkpoint(PlayLayer* play, CheckpointObject* object) {
    auto& logic = Logic::get();
    if (!play || !object || logic.predicting)
        return;
    if (std::ranges::any_of(logic.checkpoints, [object](Checkpoint const& saved) {
            return saved.object == object;
        }))
        return;
    int frame = Logic::get().get_frame(play) + (logic.in_step ? 1 : 0);
    logic.checkpoints.push_back(Logic::get().capture_checkpoint(play, object, frame));
    logic.object_capture = object;
}

Checkpoint Logic::capture_checkpoint(PlayLayer* play, CheckpointObject* object, int frame) {
    auto& logic = Logic::get();
    Checkpoint checkpoint;
    checkpoint.object = object;
    checkpoint.frame = frame;
    checkpoint.layer = Logic::get().capture_layer(play);
    checkpoint.player1 = Logic::get().capture_player(play->m_player1);
    checkpoint.player2 = Logic::get().capture_player(play->m_player2);
    checkpoint.replay_pos = logic.replay_pos;
    checkpoint.removed_frames = logic.removed_frames;
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    checkpoint.held_p1 = Logic::get().held_at(logic, false, checkpoint.frame, jump);
    checkpoint.held_p2 = Logic::get().held_at(logic, true, checkpoint.frame, jump);
    checkpoint.held_left_p1 = Logic::get().held_at(logic, false, checkpoint.frame, left);
    checkpoint.held_left_p2 = Logic::get().held_at(logic, true, checkpoint.frame, left);
    checkpoint.held_right_p1 = Logic::get().held_at(logic, false, checkpoint.frame, right);
    checkpoint.held_right_p2 = Logic::get().held_at(logic, true, checkpoint.frame, right);
    checkpoint.random_state = Logic::get().game_random_state();
    checkpoint.shake_state = logic.shake_state;
    checkpoint.tp_state = logic.tp_state;
    checkpoint.variance.assign(play->m_varianceValues.begin(), play->m_varianceValues.end());
    for (auto const& trigger : live_rand()) {
        if (trigger.state)
            checkpoint.rand_states.emplace_back(trigger.unique_id, *trigger.state);
    }
    return checkpoint;
}

void Logic::sync_checkpoints(PlayLayer* play) {
    auto& logic = Logic::get();
    std::erase_if(logic.checkpoints, [play](Checkpoint const& saved) {
        return !play || !play->m_checkpointArray || !play->m_checkpointArray->containsObject(saved.object);
    });
    if (logic.restored_checkpoint && (!play || !play->m_checkpointArray ||
                                         !play->m_checkpointArray->containsObject(logic.restored_checkpoint)))
        logic.restored_checkpoint = nullptr;
}

Checkpoint const* Logic::get_latest_checkpoint(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!play || !(play->m_isPracticeMode || logic.backstepping))
        return nullptr;
    auto* obj = logic.restored_checkpoint ? logic.restored_checkpoint : play->getLastCheckpoint();
    if (!obj || !play->m_checkpointArray || !play->m_checkpointArray->containsObject(obj))
        return nullptr;
    auto it = std::ranges::find_if(logic.checkpoints, [obj](Checkpoint const& saved) {
        return saved.object == obj;
    });
    return it == logic.checkpoints.end() ? nullptr : &*it;
}

void Logic::on_complete(PlayLayer* play) {
    auto& logic = Logic::get();
    bool was_recording = (logic).is_recording();
    if (was_recording)
        logic.state = (logic).is_playing() ? RunState::Playing : RunState::Idle;
    if (play->m_player1)
        logic.end_x = play->m_player1->getPositionX();
    if (was_recording)
        Logic::get().autosave_replay();
    logic.message = "Level complete";
}

void Logic::onQuit(PlayLayer*) {
    auto& logic = Logic::get();
    Logic::get().stop_best_frame();
    logic.holding_back = false;
    logic.frame_advance = false;
    logic.advance_queued = false;
    logic.holding_advance = false;
    for (auto* obj : logic.hidden_decorations) {
        if (obj)
            obj->m_isHide = false;
    }
    logic.hidden_decorations.clear();
    Logic::get().clear_physical_holds();
    Input::clear_keys();
    if (logic.display_fps_locked) {
        CCDirector::sharedDirector()->setAnimationInterval(1.f / 60.f);
        logic.display_fps_locked = false;
    }
    if (auto* scheduler = CCDirector::sharedDirector()->getScheduler())
        scheduler->setTimeScale(1.f);
    Logic::get().stop_render();
    Logic::get().save_settings_file();
}

void Logic::clear_stuck_death(PlayLayer* play) {
    if (!play)
        return;
    play->m_playerDied = false;
    Logic::get().noclip_dead_p1 = false;
    Logic::get().noclip_dead_p2 = false;
    if (play->m_player1)
        play->m_player1->m_isDead = false;
    if (play->m_player2)
        play->m_player2->m_isDead = false;
    Logic::get().present = true;
}

bool Logic::should_noclip(PlayLayer* play, PlayerObject* player) {
    auto const& logic = Logic::get();
    if (player == play->m_player1)
        return logic.noclip_p1;
    if (player == play->m_player2)
        return logic.noclip_p2;
    return false;
}

bool Logic::cbf_enabled() const {
    char const* ids[] = { "syzzi.click_between_frames", "zmx.cbf-lite" };
    for (char const* id : ids) {
        auto* mod = Loader::get()->getLoadedMod(id);
        if (!mod)
            continue;
        if (mod->hasSetting("soft-toggle") && mod->getSettingValue<bool>("soft-toggle"))
            continue;
        return true;
    }
    return false;
}

bool Logic::vanilla_click_physics() const {
    if (auto* play = PlayLayer::get()) {
        if (play->m_clickBetweenSteps || play->m_clickOnSteps)
            return true;
    }
    else if (auto* gm = GameManager::get()) {
        if (gm->getGameVariable("0177") || gm->getGameVariable("0176"))
            return true;
    }
    return Logic::get().cbf_enabled();
}

bool Logic::defer_physics() const {
    auto const& logic = Logic::get();
    if (logic.refresh_only)
        return false;
    if (auto* play = PlayLayer::get(); play && !play->m_started && !Logic::get().render_capturing())
        return true;
    if (logic.physics_locked())
        return false;
    if (Logic::get().render_capturing())
        return false;
    return Logic::get().vanilla_click_physics();
}

double Logic::click_stamp() const {
    return Logic::get().cbf_enabled() ? std::numeric_limits<double>::min() : 0.0;
}

namespace {
    void drop_ignored_inputs(GJBaseGameLayer* play) {
        auto& logic = Logic::get();
        if (!play || logic.restoring_holds || !logic.is_playing() || !logic.ignore_inputs)
            return;
        auto& user = play->m_queuedButtons;
        user.erase(std::remove_if(user.begin(), user.end(), [&](PlayerButtonCommand const& cmd) {
            return logic.plays_player(cmd.m_isPlayer2);
        }),
            user.end());
        if (!logic.is_recording())
            user.clear();
    }
}

void Logic::prepare_buttons(GJBaseGameLayer* play) {
    auto& logic = Logic::get();
    bool paused = Logic::get().editor_paused;
    if (auto* live = Logic::as_play(play))
        paused = paused || live->m_isPaused;
    if (!play || logic.buttons_prepared || logic.resetting || logic.refresh_only || paused || !(logic).physics_locked())
        return;
    logic.buttons_prepared = true;

    auto& user = play->m_queuedButtons;
    for (auto const& cmd : user)
        Logic::get().remember_move(cmd);

    bool playing = (logic).is_playing();
    bool recording = (logic).is_recording();
    if (recording && !logic.restoring_holds) {
        auto steer = [&](PlayerButtonCommand& cmd) {
            bool isPlayer1 = !cmd.m_isPlayer2;
            if (Logic::get().steer_player(isPlayer1))
                cmd.m_isPlayer2 = !isPlayer1;
        };
        for (auto& cmd : user)
            steer(cmd);
    }
    if (!logic.restoring_holds && playing && logic.ignore_inputs) {
        drop_ignored_inputs(play);
    }
    else if (!logic.restoring_holds && recording && logic.ignore_record_frames > 0) {
        user.clear();
    }

    logic.replay_clicks_left = 0;
    if (!playing)
        return;
    int frame = Logic::get().step_frame + 1;
    std::array<char, 6> queued{};
    auto queue = [&](bool holding, int button, bool player2) {
        int jump = static_cast<int>(PlayerButton::Jump);
        int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
        int slot = button == 0 || button == jump ? 0 : button == left ? 1
                                                   : button == right  ? 2
                                                                      : -1;
        if (slot >= 0)
            queued[static_cast<size_t>((player2 ? 3 : 0) + slot)] = 1;
        PlayerButtonCommand cmd;
        cmd.m_button = static_cast<PlayerButton>(button == 0 ? jump : button);
        cmd.m_isPush = holding;
        cmd.m_isPlayer2 = player2;
        cmd.m_step = frame;
        cmd.m_timestamp = 0.0;
        play->m_queuedButtons.push_back(cmd);
        ++logic.replay_clicks_left;
        echo::step::trace_button('Q', holding, static_cast<int>(play->m_queuedButtons.size()));
    };
    while (logic.replay_pos < logic.inputs.size() && logic.inputs[logic.replay_pos].number < frame)
        ++logic.replay_pos;
    while (logic.replay_pos < logic.inputs.size() && logic.inputs[logic.replay_pos].number == frame) {
        Frame input = logic.inputs[logic.replay_pos];
        ++logic.replay_pos;
        int button = input.button == 0 ? static_cast<int>(PlayerButton::Jump) : input.button;
        if (!logic.plays_player(input.player2))
            continue;
        queue(input.holding, button, input.player2);
    }

    static struct {
        uint64_t revision = ~0ull;
        Frame const* data = nullptr;
        size_t size = 0;
        int first[2] = { -1, -1 }, last[2] = { -1, -1 };
    } ends;

    if (ends.revision != logic.inputs_revision || ends.data != logic.inputs.data() || ends.size != logic.inputs.size()) {
        ends.revision = logic.inputs_revision;
        ends.data = logic.inputs.data();
        ends.size = logic.inputs.size();
        ends.first[0] = ends.first[1] = std::numeric_limits<int>::max();
        ends.last[0] = ends.last[1] = -1;
        for (auto const& input : logic.inputs) {
            int side = input.player2 ? 1 : 0;
            ends.first[side] = std::min(ends.first[side], input.number);
            ends.last[side] = std::max(ends.last[side], input.number);
        }
    }
    auto ensure = [&](bool stored2, int button) {
        int side = stored2 ? 1 : 0;
        if (!logic.ignore_inputs && frame > ends.last[side])
            return;
        int jump = static_cast<int>(PlayerButton::Jump);
        int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
        int slot = button == jump ? 0 : button == left ? 1
                                    : button == right  ? 2
                                                       : -1;
        if (slot >= 0 && queued[static_cast<size_t>((stored2 ? 3 : 0) + slot)])
            return;
        auto* body = Logic::get().icon_body(play, stored2);
        if (!body || !logic.plays_player(stored2))
            return;
        if (slot > 0 && body->m_controlsDisabled)
            return;
        bool want = Logic::get().held_at(logic, stored2, frame, button);
        if (Logic::button_held(body, button) == want)
            return;
        queue(want, button, stored2);
    };
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    ensure(false, jump);
    if (play->m_isPlatformer) {
        ensure(false, left);
        ensure(false, right);
    }
    if (play->m_gameState.m_isDualMode) {
        ensure(true, jump);
        if (play->m_isPlatformer) {
            ensure(true, left);
            ensure(true, right);
        }
    }
}

void Logic::remember_move(PlayerButtonCommand const& cmd) {
    auto& logic = Logic::get();
    bool player2 = cmd.m_isPlayer2;
    if (cmd.m_button == PlayerButton::Left) {
        (player2 ? logic.physical_left_p2 : logic.physical_left_p1) = cmd.m_isPush;
    }
    else if (cmd.m_button == PlayerButton::Right) {
        (player2 ? logic.physical_right_p2 : logic.physical_right_p1) = cmd.m_isPush;
    }
}

void run_orb_clicks(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt) {
    auto* live = Logic::as_play(play);
    if (!live || !layer || Logic::get().predicting)
        return;
    auto saved = std::move(play->m_queuedButtons);
    play->m_queuedButtons.clear();
    auto& logic = Logic::get();
    logic.suppress_click_hitbox = true;
    if (!play->m_queuedButtons.empty()) {
        if (logic.is_recording() && !logic.restoring_holds) {
            for (auto const& cmd : play->m_queuedButtons) {
                Logic::get().record_input(play, cmd.m_isPush, !cmd.m_isPlayer2, static_cast<int>(cmd.m_button));
            }
        }
        logic.injecting = true;
        layer->GJBaseGameLayer::processQueuedButtons(dt, true);
        logic.injecting = false;
    }
    logic.suppress_click_hitbox = false;
    play->m_queuedButtons = std::move(saved);
}

void Logic::play_macro(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt, bool clearInputQueue) {
    auto& logic = Logic::get();
    bool paused = Logic::get().editor_paused;
    if (auto* live = Logic::as_play(play))
        paused = paused || live->m_isPaused;
    if (!play || !layer || logic.resetting || logic.refresh_only || paused || !(logic).physics_locked()) {
        if (layer) {
            drop_ignored_inputs(play);
            gd::vector<PlayerButtonCommand> orbs;
            if (auto* live = Logic::as_play(play); live && !logic.predicting) {
                size_t before = play->m_queuedButtons.size();
                for (size_t i = before; i < play->m_queuedButtons.size(); ++i)
                    orbs.push_back(play->m_queuedButtons[i]);
                if (play->m_queuedButtons.size() > before)
                    play->m_queuedButtons.resize(before);
            }
            layer->GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
            if (!orbs.empty()) {
                logic.suppress_click_hitbox = true;
                if (logic.is_recording() && !logic.restoring_holds) {
                    for (auto const& cmd : orbs) {
                        Logic::get().record_input(play, cmd.m_isPush, !cmd.m_isPlayer2, static_cast<int>(cmd.m_button));
                    }
                }
                play->m_queuedButtons = orbs;
                logic.injecting = true;
                layer->GJBaseGameLayer::processQueuedButtons(dt, true);
                logic.injecting = false;
                logic.suppress_click_hitbox = false;
            }
            if (logic.is_recording() && logic.maintain_gravity && logic.same_gravity && !logic.restoring_holds && !logic.resetting)
                Logic::get().sync_same_gravity(play);
        }
        return;
    }
    Logic::get().prepare_buttons(play);
    int replay = logic.replay_clicks_left;
    if (play->m_queuedButtons.empty()) {
        logic.applying_playback = false;
        if (logic.is_recording() && logic.maintain_gravity && !logic.restoring_holds)
            Logic::get().apply_maintain_gravity(play);
        run_orb_clicks(play, layer, dt);
        logic.applying_playback = false;
        if (logic.is_recording() && logic.maintain_gravity && logic.same_gravity && !logic.restoring_holds)
            Logic::get().sync_same_gravity(play);
        if (!clearInputQueue && replay == 0)
            play->m_queuedButtons.clear();
        logic.buttons_prepared = false;
        return;
    }
    bool correct_gravity = logic.is_recording() && logic.maintain_gravity && !logic.restoring_holds;
    bool single_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    replay::SmallSnapshot<PlayerButtonCommand, 8> queued(play->m_queuedButtons);
    if (logic.is_recording() && logic.maintain_gravity && !logic.restoring_holds) {
        int jump = static_cast<int>(PlayerButton::Jump);
        queued.removeIf([&](PlayerButtonCommand const& cmd) {
            int button = static_cast<int>(cmd.m_button);
            bool jump_button = button == 0 || button == jump;
            return jump_button && logic.records_player(cmd.m_isPlayer2) && !logic.plays_player(cmd.m_isPlayer2);
        });
    }
    int user_count = std::max(0, static_cast<int>(queued.size()) - replay);
    auto run_queue = [&](int from, int to, bool playback) {
        play->m_queuedButtons.clear();
        for (int i = from; i < to; ++i)
            play->m_queuedButtons.push_back(queued[i]);
        logic.applying_playback = playback;
        if (!play->m_queuedButtons.empty())
            layer->GJBaseGameLayer::processQueuedButtons(dt, true);
    };
    run_queue(0, user_count, false);
    if (logic.is_recording() && logic.maintain_gravity && !logic.restoring_holds)
        Logic::get().apply_maintain_gravity(play);
    bool one_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    logic.applying_playback = true;
    for (int i = user_count; i < static_cast<int>(queued.size()); ++i) {
        auto const& cmd = queued[static_cast<size_t>(i)];
        auto apply = [&](bool player2) {
            PlayerButtonCommand next = cmd;
            next.m_isPlayer2 = player2;
            play->m_queuedButtons.clear();
            play->m_queuedButtons.push_back(next);
            layer->GJBaseGameLayer::processQueuedButtons(dt, true);
        };
        uint32_t partner_before = logic.partner_presses;
        apply(cmd.m_isPlayer2);
        bool partner_clicked = logic.partner_presses != partner_before;
        if (one_dual && !cmd.m_isPlayer2 && !partner_clicked && play->m_player2 && !play->m_player2->m_isDead && !play->m_player2->m_controlsDisabled) {
            apply(true);
        }
    }
    logic.applying_playback = false;
    if (logic.is_recording() && logic.maintain_gravity && logic.same_gravity && !logic.restoring_holds)
        Logic::get().sync_same_gravity(play);
    if (!clearInputQueue && replay == 0) {
        play->m_queuedButtons.assign(queued.begin(), queued.end());
    }
    logic.buttons_prepared = false;
}

void Logic::offset_frames(int delta, int first, int last) {
    auto& logic = Logic::get();
    int n = static_cast<int>(logic.inputs.size());
    bool all = first < 0 || last < 0;
    if (all) {
        first = 0;
        last = n - 1;
    }
    if (first > last)
        std::swap(first, last);
    first = std::max(first, 0);
    last = std::min(last, n - 1);
    for (int i = first; i <= last; ++i) {
        if (logic.inputs[static_cast<size_t>(i)].number + delta >= 0)
            logic.inputs[static_cast<size_t>(i)].number += delta;
    }
    if (all) {
        for (auto& change : logic.rates)
            change.frame = std::max(change.frame + delta, 0);
        Logic::get().normalize_rates();
    }
    logic.touch_inputs();
    Logic::get().sort_inputs();
    Logic::get().message = fmt::format("Shifted inputs by {}", delta);
    Logic::get().autosave_replay();
}

void Logic::offset_inputs(int magnitude, int first, int last) {
    if (magnitude < 0)
        magnitude = -magnitude;
    auto& logic = Logic::get();
    int n = static_cast<int>(logic.inputs.size());
    bool all = first < 0 || last < 0;
    if (all) {
        first = 0;
        last = n - 1;
    }
    if (first > last)
        std::swap(first, last);
    first = std::max(first, 0);
    last = std::min(last, n - 1);
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_int_distribution<int> dist(-magnitude, magnitude);
    for (int i = first; i <= last; ++i) {
        int next = logic.inputs[static_cast<size_t>(i)].number + dist(rng);
        logic.inputs[static_cast<size_t>(i)].number = std::max(next, 0);
    }
    logic.touch_inputs();
    Logic::get().sort_inputs();
    Logic::get().message = fmt::format("Randomly offset inputs by +/- {}", magnitude);
    Logic::get().autosave_replay();
}

void Logic::remove_player_inputs(bool player2) {
    auto& logic = Logic::get();
    replay::compact(logic.inputs, logic.poses, logic.input_ids,
        [player2](Frame const& input) {
            return input.player2 != player2;
        });
    logic.inputs.shrink_to_fit();
    logic.poses.shrink_to_fit();
    logic.input_ids.shrink_to_fit();
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps(Logic::get());
    Logic::get().message = player2 ? "Removed player 2 inputs" : "Removed player 1 inputs";
    Logic::get().autosave_replay();
}

void Logic::flip_input_holds() {
    for (auto& input : Logic::get().inputs)
        input.holding = !input.holding;
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps(Logic::get());
    Logic::get().message = "Flipped presses and releases";
    Logic::get().autosave_replay();
}

void Logic::flip_input_players() {
    for (auto& input : Logic::get().inputs)
        input.player2 = !input.player2;
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps(Logic::get());
    Logic::get().message = "Flipped player 1 and 2";
    Logic::get().autosave_replay();
}

void Logic::sort_inputs() {
    auto& logic = Logic::get();
    auto& inputs = logic.inputs;
    if (std::is_sorted(inputs.begin(), inputs.end(), [](Frame const& a, Frame const& b) {
            return a.number < b.number || (a.number == b.number && !a.player2 && b.player2);
        })) {
        logic.touch_inputs();
        Logic::get().refresh_live_hold();
        Logic::get().recompute_cps(logic);
        return;
    }
    std::vector<size_t> order(inputs.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return inputs[a].number < inputs[b].number || (inputs[a].number == inputs[b].number && !inputs[a].player2 && inputs[b].player2);
    });
    bool moved = false;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] != i)
            moved = true;
    if (moved) {
        auto permute = [&](auto& side) {
            if (side.size() != order.size())
                return;
            using Value = typename std::decay_t<decltype(side)>::value_type;
            std::vector<Value> next;
            next.reserve(order.size());
            for (size_t i : order)
                next.push_back(side[i]);
            side.swap(next);
        };
        permute(inputs);
        permute(logic.poses);
        permute(logic.input_ids);
        logic.touch_inputs();
        Logic::get().refresh_live_hold();
    }
    Logic::get().recompute_cps(Logic::get());
}

uint64_t Logic::mint_input_id() {
    return ++Logic::get().next_input_id;
}

void Logic::identify_inputs(bool force) {
    auto& logic = Logic::get();
    if (!force && logic.is_recording() && logic.input_ids.empty())
        return;

    struct Seen {
        std::vector<Frame> const* owner = nullptr;
        Frame const* data = nullptr;
        size_t count = 0;
        uint64_t revision = 0;
        uint64_t next = 0;
        uint64_t last_id = 0;
    };

    thread_local Seen seen;
    auto& inputs = logic.inputs;
    bool kept = seen.owner == &inputs && seen.revision == logic.inputs_revision && seen.count <= inputs.size() && logic.next_input_id >= seen.next;
    if (kept && seen.count > 0)
        kept = logic.input_id(seen.count - 1) == seen.last_id;
    if (kept && seen.count == inputs.size())
        kept = seen.data == inputs.data();
    size_t from = kept ? seen.count : 0;
    uint64_t highest = logic.next_input_id;
    for (size_t i = from; i < inputs.size(); ++i) {
        uint64_t id = logic.input_id(i);
        if (id > highest)
            highest = id;
    }
    logic.next_input_id = highest;
    for (size_t i = from; i < inputs.size(); ++i) {
        if (logic.input_id(i) != 0)
            continue;
        logic.set_input_id(i, ++logic.next_input_id);
    }
    seen = { &inputs, inputs.data(), inputs.size(), logic.inputs_revision, logic.next_input_id, inputs.empty() ? 0 : logic.input_id(inputs.size() - 1) };
}

void Logic::clear_inputs() {
    auto& logic = Logic::get();
    logic.inputs.clear();
    logic.clear_sides();
    logic.macro_unrounded = logic.velocity_unrounding;
    logic.macro_silicate_variance = false;
    std::vector<Frame>().swap(logic.inputs);
    std::vector<FramePose>().swap(logic.poses);
    std::vector<uint64_t>().swap(logic.input_ids);
    Logic::get().release_replay_index();
    logic.touch_inputs();
    logic.rates.clear();
    logic.replay_pos = 0;
    logic.lvl_seed_from_file = false;
    if (logic.seed_override) {
        logic.lvl_seed = logic.seed_override_value;
        logic.lvl_seed_known = true;
    }
    else {
        logic.lvl_seed = 0;
        logic.lvl_seed_known = false;
    }
    logic.message = "Cleared inputs";
}

void Logic::notify(std::string const& text, bool success) {
    // errors only go to the status line now, no popup
    if (success)
        Notification::create(text, NotificationIcon::Success)->show();
    Logic::get().message = text;
}

void Logic::note_click(PlayLayer* play, bool down, bool player1, int button) {
    auto& logic = Logic::get();
    if (!down || !play)
        return;
    if (logic.resetting || logic.refresh_only || logic.ignore_record_frames > 0)
        return;
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    if (button != 0 && button != jump && button != left && button != right)
        return;
    if (button == 0)
        button = jump;
    PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
    if (body) {
        if (Logic::button_held(body, button) || (button == jump && Logic::button_held(body, 0)))
            return;
    }
    bool two = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode, one_dual = dual && !two;
    if (one_dual)
        player1 = true;
    bool player2 = !player1;
    if (player2 && !(two && dual))
        return;
    auto& clicks = player2 ? logic.clicks_p2 : logic.clicks_p1;
    int now = Logic::get().step_frame + 1;
    clicks.push_back(now);
    double now_t = logic.seconds_at(now);
    size_t before = clicks.size();
    while (clicks.size() > 1 && now_t - logic.seconds_at(clicks.front()) > 2.0)
        clicks.pop_front();
    while (clicks.size() > 4096)
        clicks.pop_front();
    if (clicks.size() != before)
        ++logic.clicks_revision;
}

namespace {
    struct RecentClicks {
        int in_second = 0;
        int on_frame = 0;
    };

    RecentClicks recent_clicks(Session const& value, std::deque<int> const& clicks, int now) {
        double now_t = value.seconds_at(now);
        auto end = std::upper_bound(clicks.begin(), clicks.end(), now);
        auto start = std::partition_point(clicks.begin(), end, [&](int frame) {
            return now_t - value.seconds_at(frame) > 1.0;
        });
        auto same = std::lower_bound(start, end, now);
        return { static_cast<int>(std::distance(start, end)), static_cast<int>(std::distance(same, end)) };
    }

    bool same_rates(std::vector<RateChange> const& a, std::vector<RateChange> const& b) {
        return std::ranges::equal(a, b, [](RateChange const& x, RateChange const& y) {
            return x.frame == y.frame && x.fps == y.fps;
        });
    }

    double burst_peak(Session const& value, std::deque<int> const& clicks) {
        struct State {
            std::deque<int> const* owner = nullptr;
            uint64_t revision = 0;
            float macro_fps = 0.f;
            std::vector<RateChange> rates;
            size_t covered = 0;
            double best = 0.0;
        };

        thread_local std::array<State, 2> caches;
        auto& state = &clicks == &value.clicks_p2 ? caches[1] : caches[0];
        bool kept = state.owner == &clicks && state.revision == value.clicks_revision && state.covered <= clicks.size() &&
                    state.macro_fps == value.macro_fps && same_rates(state.rates, value.rates);
        if (!kept) {
            state = {};
            state.owner = &clicks;
            state.revision = value.clicks_revision;
            state.macro_fps = value.macro_fps;
            state.rates = value.rates;
        }
        int amt = static_cast<int>(clicks.size());
        int target = amt - Logic::MIN_BURST_CLICKS;
        if (target < 0)
            return 0.0;
        double rate = static_cast<double>(Logic::MAX_BURST_RATE);
        while (state.covered <= static_cast<size_t>(target)) {
            double lead = rate * value.seconds_at(clicks[state.covered]) - static_cast<double>(state.covered);
            state.best = state.covered == 0 ? lead : std::max(state.best, lead);
            ++state.covered;
        }
        return state.best;
    }
}

unsigned Logic::count_presses_in_last_second(bool player2) {
    auto const& logic = Logic::get();
    int now = PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : 0;
    auto const& clicks = player2 ? logic.clicks_p2 : logic.clicks_p1;
    return static_cast<unsigned>(recent_clicks(logic, clicks, now).in_second);
}

bool Logic::burst_too_fast(Session const& value, int clicks, int first, int last) const {
    if (clicks < Logic::MIN_BURST_CLICKS)
        return false;
    double span = value.seconds_at(last) - value.seconds_at(first);
    if (!(span > 0.0))
        return true;
    return static_cast<double>(clicks) / span > Logic::MAX_BURST_RATE;
}

bool Logic::player_cps_broken(std::deque<int> const& clicks, int now, float max_per_second) const {
    if (clicks.empty())
        return false;
    auto const& logic = Logic::get();
    auto recent = recent_clicks(logic, clicks, now);
    if (static_cast<float>(recent.in_second) > max_per_second)
        return true;
    if (recent.on_frame > Logic::MAX_PER_FRAME)
        return true;
    int n = static_cast<int>(clicks.size());
    double back_t = logic.seconds_at(clicks.back());
    if (n < Logic::MIN_BURST_CLICKS || logic.seconds_at(now) - back_t > 1.0)
        return false;
    double need = static_cast<double>(Logic::MAX_BURST_RATE) * back_t - static_cast<double>(n);
    double slack = 1e-6 + 1e-12 * std::abs(need);
    double top = burst_peak(logic, clicks);
    if (top > need + slack)
        return true;
    if (top <= need - slack)
        return false;
    for (int start = 0; start <= n - Logic::MIN_BURST_CLICKS; ++start) {
        if (Logic::get().burst_too_fast(logic, n - start, clicks[static_cast<size_t>(start)], clicks.back()))
            return true;
    }
    return false;
}

void Logic::set_tps(float rate) {
    auto& logic = Logic::get();
    rate = std::clamp(rate, 1.f, FLT_MAX);
    if (!std::isfinite(rate))
        return;
    if (!step_patched && rate < VANILLA_TPS) {
        logic.message = "TPS below 240 requires the physics step patch";
        rate = VANILLA_TPS;
    }
    if ((logic).is_recording()) {
        int frame = PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : 0;
        Logic::get().place_rate(frame, rate);
        logic.message = fmt::format("TPS {:.2f} at frame {}", rate, frame);
    }
    else if (!(logic).is_playing()) {
        logic.macro_fps = rate;
        logic.fps = rate;
        Logic::get().push_runtime_settings();
    }
    else if (logic.playback_tps_changes) {
        // while a macro plays ts ONLY sets its starting tps (the 1 silicate gets wrong). the changes in the macro stay
        logic.macro_fps = rate;
        int frame = PlayLayer::get() ? Logic::get().get_frame(PlayLayer::get()) : 0;
        logic.fps = logic.rate_at(frame);
        rate = logic.fps;
        Trajectory::get().invalidate();
        logic.message = fmt::format("Starting TPS {:.2f}", logic.macro_fps);
    }
    if (logic.playback_fps < 1.f) {
        if (auto* director = CCDirector::sharedDirector())
            director->setAnimationInterval(1.f / rate);
    }
}

void Logic::place_rate(int frame, float rate) {
    auto& logic = Logic::get();
    rate = std::clamp(rate, 1.f, FLT_MAX);
    if (!std::isfinite(rate))
        return;
    if (frame <= 0) {
        logic.macro_fps = rate;
        logic.fps = rate;
        std::erase_if(logic.rates, [](RateChange const& change) {
            return change.frame <= 0;
        });
        return;
    }
    float prev = logic.rate_at(frame - 1);
    auto at = std::find_if(logic.rates.begin(), logic.rates.end(), [frame](RateChange const& change) {
        return change.frame == frame;
    });
    if (rate == prev) {
        if (at != logic.rates.end())
            logic.rates.erase(at);
        logic.fps = rate;
        return;
    }
    if (at != logic.rates.end())
        at->fps = rate;
    else {
        logic.rates.push_back({ frame, rate });
        std::stable_sort(logic.rates.begin(), logic.rates.end(), [](RateChange const& a, RateChange const& b) {
            return a.frame < b.frame;
        });
    }
    logic.fps = rate;
}

void Logic::apply_rate(int frame) {
    auto& logic = Logic::get();
    float next = logic.rate_at(frame);
    if (!std::isfinite(next) || next < 1.f)
        next = 240.f;
    if (next == logic.fps)
        return;
    logic.fps = next;
    if (logic.display_fps_locked && logic.playback_fps < 1.f && !Logic::get().render_capturing()) {
        if (auto* director = CCDirector::sharedDirector())
            director->setAnimationInterval(1.f / next);
    }
}

void Logic::normalize_rates() {
    auto& logic = Logic::get();
    std::erase_if(logic.rates, [](RateChange const& change) {
        return change.frame < 0 || !std::isfinite(change.fps) || change.fps < 1.f;
    });
    std::stable_sort(logic.rates.begin(), logic.rates.end(), [](RateChange const& a, RateChange const& b) {
        return a.frame < b.frame;
    });
    std::vector<RateChange> kept;
    float rate = logic.macro_fps;
    for (auto const& change : logic.rates) {
        if (!kept.empty() && kept.back().frame == change.frame)
            kept.back() = change;
        else
            kept.push_back(change);
    }
    logic.rates.clear();
    for (auto const& change : kept) {
        if (change.fps == rate)
            continue;
        logic.rates.push_back(change);
        rate = change.fps;
    }
}

void Logic::trim_rates(int frame) {
    std::erase_if(Logic::get().rates, [frame](RateChange const& change) {
        return change.frame >= frame;
    });
    Logic::get().apply_rate(std::max(frame, 0));
}

void Logic::update_cps(PlayLayer* play) {
    auto& logic = Logic::get();
    auto holding = [](PlayerObject* player) {
        if (!player)
            return false;
        static constexpr int buttons[] = {
            static_cast<int>(PlayerButton::Jump),
            static_cast<int>(PlayerButton::Left),
            static_cast<int>(PlayerButton::Right),
        };
        return std::ranges::any_of(buttons, [player](int button) {
            return Logic::button_held(player, button);
        });
    };
    logic.currently_pressing = holding(play->m_player1) || (play->m_gameState.m_isDualMode && holding(play->m_player2));
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool two_dual = twoPlayer && play->m_gameState.m_isDualMode;
    if (!two_dual && !logic.clicks_p2.empty()) {
        logic.clicks_p2.clear();
        ++logic.clicks_revision;
    }
    unsigned p1 = Logic::get().count_presses_in_last_second(false);
    unsigned p2 = two_dual ? Logic::get().count_presses_in_last_second(true) : 0;
    logic.current_cps = static_cast<float>(two_dual ? std::max(p1, p2) : p1);
    int now = Logic::get().get_frame(play);
    float limit = std::max(static_cast<float>(logic.max_cps), 1.f);
    logic.cps_breaking = Logic::get().player_cps_broken(logic.clicks_p1, now, limit);
    if (two_dual)
        logic.cps_breaking = logic.cps_breaking || Logic::get().player_cps_broken(logic.clicks_p2, now, limit);
    if (logic.cps_breaking)
        logic.over_max_cps = true;
}

void Logic::tick_autoclicker(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!logic.autoclicker || !play || play->m_isPaused)
        return;
    auto& click = logic.auto_click;
    if (click.pressed)
        ++click.since_press;
    else
        ++click.since_release;

    int jump = static_cast<int>(PlayerButton::Jump);
    auto press_player = [&](bool player1) {
        bool twoPlayer = play->m_level && play->m_level->m_twoPlayerMode;
        if (!player1 && !twoPlayer)
            return;
        play->queueButton(jump, true, !player1, Logic::get().click_stamp());
        // swift = let go & press again on the same frame
        for (int i = 0; i < click.swifts; ++i) {
            play->queueButton(jump, false, !player1, Logic::get().click_stamp());
            play->queueButton(jump, true, !player1, Logic::get().click_stamp());
        }
    };
    auto release_player = [&](bool player1) {
        bool twoPlayer = play->m_level && play->m_level->m_twoPlayerMode;
        if (!player1 && !twoPlayer)
            return;
        play->queueButton(jump, false, !player1, Logic::get().click_stamp());
    };

    if (!click.pressed && click.since_release >= click.release_interval) {
        click.pressed = true;
        click.since_release = 0;
        click.since_press = 0;
        if (logic.autoclick_p1)
            press_player(true);
        if (logic.autoclick_p2)
            press_player(false);
        if (click.press_interval <= 0) {
            click.pressed = false;
            if (logic.autoclick_p1)
                release_player(true);
            if (logic.autoclick_p2)
                release_player(false);
        }
    }
    else if (click.pressed && click.since_press >= click.press_interval) {
        click.pressed = false;
        click.since_press = 0;
        if (logic.autoclick_p1)
            release_player(true);
        if (logic.autoclick_p2)
            release_player(false);
    }

    if (logic.autoclick_auto_disable) {
        --click.disable_in;
        if (click.disable_in <= 0) {
            logic.autoclicker = false;
            logic.autoclick_auto_disable = false;
            click.disable_in = 1;
            Logic::write_bool("autoclicker", false);
            logic.message = "Autoclicker disabled";
        }
    }
}

int Logic::follow_steps() const {
    double steps = follow_delay_ms ? follow_delay / 1000.0 * sample_rate() : follow_delay;
    if (!std::isfinite(steps))
        return 0;
    return std::max(0, static_cast<int>(std::lround(steps)));
}

bool Logic::follow_active(GJBaseGameLayer* play) const {
    bool macro = backstep_target >= 0 ? (backstep_state == RunState::Playing || backstep_state == RunState::RecordingAndPlaying) : is_playing();
    if (follow_mode == 0 || click_both || macro || !play)
        return false;
    return play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
}

void Logic::note_follow_press(GJBaseGameLayer* play, bool down, int button) {
    follow_log.push_back({ step_frame + 1, play->m_gameState.m_levelTime, down, button });
    while (follow_log.size() > 65536)
        follow_log.pop_front();
}

// delay is in lvl time NOT steps
bool Logic::follow_due(FollowPress const& press, int frame, double time) const {
    if (follow_delay_ms)
        return press.time + std::max(follow_delay, 0.f) / 1000.0 <= time + 1e-7;
    return press.frame + follow_steps() <= frame;
}

void Logic::rewind_follow(GJBaseGameLayer* play) {
    std::erase_if(follow_log, [this](auto const& press) {
        return press.frame > step_frame;
    });
    if (!play)
        return;
    double time = play->m_gameState.m_levelTime;
    double delay = std::max(follow_delay, 0.f) / 1000.0;
    for (auto& press : follow_log)
        press.sent = follow_delay_ms ? press.time + delay < time - 1e-7 : press.frame + follow_steps() <= step_frame;
}

void Logic::run_follow(GJBaseGameLayer* play) {
    // TODO
}
