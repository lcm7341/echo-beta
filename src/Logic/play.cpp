// playback & everything around an attempt
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"
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
    log::debug("toggle recording");
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
    if (logic.is_recording() && !was_recording && logic.inputs.capacity() < 4096) {
        logic.inputs.reserve(4096);
    }
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
    if (record1 == record2) {
        return false;
    }
    isPlayer1 = !record2;
    return true;
}

PlayerObject* Logic::icon_body(GJBaseGameLayer* play, bool icon2) {
    if (!play)
        return nullptr;
    return icon2 ? play->m_player2 : play->m_player1;
}

bool Logic::play_input(GJBaseGameLayer* play, bool icon2, bool down, int button) {
    log::debug("play input");
    PlayerObject* body = icon_body(play, icon2);
    if (!body || body->m_isDead || body->m_controlsDisabled) {
        return false;
    }
    if (button == 0) {
        button = static_cast<int>(PlayerButton::Jump);
    }
    auto key = static_cast<PlayerButton>(button);
    if (down)
        body->pushButton(key);
    else
        body->releaseButton(key);
    return true;
}

bool Logic::physical_hold(GJBaseGameLayer* play, bool icon2) {
    if (!play || !play->m_uiLayer) {
        return false;
    }
    if (suppress_auto_hold) {
        return false;
    }
    auto* ui = play->m_uiLayer;
    bool p1 = ui->m_p1Jumping || ui->m_p1TouchId != -1 || key_jump_p1 || (restart_hold_captured && death_jump_p1);
    bool p2 = ui->m_p2Jumping || ui->m_p2TouchId != -1 || key_jump_p2 || (restart_hold_captured && death_jump_p2);
    if (restart_hold_captured && ((death_jump_p1 && (ui->m_p1Jumping || ui->m_p1TouchId != -1)) || (death_jump_p2 && (ui->m_p2Jumping || ui->m_p2TouchId != -1)))) {
        restart_hold_captured = false;
    }
    if (traj_autoclicker && maintain_gravity) {
        p1 = p1 != traj_flip_p1;
        p2 = p2 != traj_flip_p2;
    }
    bool record1 = records_player(false), record2 = records_player(true);
    bool one_player_dual = play->m_gameState.m_isDualMode && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode);
    if (record1 != record2 && !one_player_dual) {
        if (icon2 != record2) {
            return icon2 ? p2 : p1;
        }
        return p1 || p2;
    }
    if (flip_controls()) {
        std::swap(p1, p2);
    }
    if (one_player_dual || click_both) {
        return p1 || p2;
    }
    return icon2 ? p2 : p1;
}

bool Logic::steer_key(GJBaseGameLayer* play, enumKeyCodes key, bool down, double timestamp) {
    log::debug("steer key");
    if (!play || resetting || refresh_only) {
        return false;
    }
    int code = static_cast<int>(key);
    auto held = steered_keys.find(code);
    if (!down && held != steered_keys.end()) {
        int button = held->second & 0xF;
        bool icon2 = (held->second >> 4) != 0;
        steered_keys.erase(held);
        bool still = std::ranges::any_of(steered_keys, [&](auto const& entry) {
            return entry.second == ((icon2 ? 1 : 0) << 4 | button);
        });
        if (button == static_cast<int>(PlayerButton::Jump)) {
            (icon2 ? key_jump_p2 : key_jump_p1) = still;
        }
        if (!still) {
            play->queueButton(button, false, icon2, timestamp);
        }
        if (button == static_cast<int>(PlayerButton::Jump) && !still) {
            restart_hold_captured = false;
            death_jump_p1 = death_jump_p2 = false;
        }
        return true;
    }
    if (!down) {
        return false;
    }
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
    if (!steer_player(target1)) {
        return false;
    }
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
        if (play->m_isPlatformer) {
            button = static_cast<int>(PlayerButton::Left);
        }
        break;
    case KEY_D:
    case KEY_Right:
        if (play->m_isPlatformer) {
            button = static_cast<int>(PlayerButton::Right);
        }
        break;
    default:
        break;
    }
    if (button == 0) {
        return false;
    }
    if (held != steered_keys.end()) {
        return true;
    }
    int tag = (icon2 ? 1 : 0) << 4 | button;
    bool already = std::ranges::any_of(steered_keys, [&](auto const& entry) {
        return entry.second == tag;
    });
    steered_keys[code] = tag;
    if (button == static_cast<int>(PlayerButton::Jump)) {
        (icon2 ? key_jump_p2 : key_jump_p1) = true;
    }
    if (!already) {
        play->queueButton(button, true, icon2, timestamp);
    }
    return true;
}

void Logic::set_routed_hold(bool icon2, int button, bool down) {
    if (button == 0) {
        button = static_cast<int>(PlayerButton::Jump);
    }
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

void Logic::save_holds(PlayLayer* play) {
    // later
}

// the block only ended inside a step so frame advance never got out of it
void Logic::poll_hold_block() {
    log::debug("poll hold block");
    if (!suppress_auto_hold)
        return;
    bool down = Input::jump_key_down() || Input::mouseDown();
    if (auto* play = PlayLayer::get(); play && play->m_uiLayer) {
        auto* ui = play->m_uiLayer;
        down = down || ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
    }
    if (!down) {
        suppress_auto_hold = false;
    }
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
        if (icon2 && !two) {
            continue;
        }
        auto* body = icon_body(play, icon2);
        if (!body || plays_player(icon2)) {
            continue;
        }
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
        if (maintain_gravity && body->m_isDart && records_player(icon2)) {
            held = held != body->m_isUpsideDown;
        }
        match(jump, held);
        if (!play->m_isPlatformer) {
            continue;
        }
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
    // later
}

void Logic::disarm_length_pause_if_holding() {
    log::debug("disarm length pause if holding");
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
    // TODO
}

void Logic::poll_advance_hold() {
    // TODO
}

void Logic::check_advance_pause(PlayLayer* play) {
    auto& logic = Logic::get();
    if (!logic.pause_before_kill || !logic.is_recording())
        return;
    auto watch = [&](bool player2) {
        if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2)) {
            return false;
        }
        if (logic.is_recording()) {
            return logic.records_player(player2);
        }
        return true;
    };
    bool death_next = false;
    if (watch(false) && logic.death_next_p1) {
        death_next = true;
    }
    if (watch(true) && logic.death_next_p2) {
        death_next = true;
    }
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
    // todo
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
    if (!menu) {
        play->m_isPaused = false;
    }
    if (auto* director = CCDirector::sharedDirector()) {
        if (!menu && director->isPaused()) {
            director->resume();
        }
        director->setNextDeltaTimeZero(false);
    }
    Logic::get().sync_time_scale();
}

void Logic::retain_playback_hold(PlayLayer* play) {
    log::debug("retain playback hold");
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
            if (button == jump) {
                body->m_jumpBuffered = true;
            }
            if (button == left) {
                body->m_holdingLeft = true;
            }
            if (button == right) {
                body->m_holdingRight = true;
            }
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
    // TODO
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
    // TODO
}

void Logic::on_play_init(PlayLayer* play) {
    // later
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
    // later
}

void Logic::clear_rand_triggers() {
    live_rand().clear();
}

void Logic::seed_rand_triggers(uint64_t seed) {
    // todo
}

void Logic::roll_live_seed(PlayLayer* play) {
    // TODO
}

void Logic::apply_attempt_variance(PlayLayer* play, uint64_t seed) {
    // port this
}

void Logic::keep_variance(PlayLayer* play) {
    // port this
}

void Logic::plant_lvl_seed(PlayLayer* play) {
    // TODO
}

void Logic::restore_checkpoint_random(PlayLayer* play) {
    // todo
}

void reshuffle_sequence(Session& value) {
    // port this
}

void Logic::onReset(PlayLayer* play) {
    // TODO
}

void Logic::restore_holds(PlayLayer* play) {
    // TODO
}

void Logic::save_checkpoint(PlayLayer* play, CheckpointObject* object) {
    // TODO
}

Checkpoint Logic::capture_checkpoint(PlayLayer* play, CheckpointObject* object, int frame) {
    // port this
    return {};
}

void Logic::sync_checkpoints(PlayLayer* play) {
    // TODO
}

Checkpoint const* Logic::get_latest_checkpoint(PlayLayer* play) {
    // TODO
    return {};
}

void Logic::on_complete(PlayLayer* play) {
    // TODO
}

void Logic::onQuit(PlayLayer*) {
    // TODO
}

void Logic::clear_stuck_death(PlayLayer* play) {
    // TODO
}

bool Logic::should_noclip(PlayLayer* play, PlayerObject* player) {
    // port this
    return {};
}

bool Logic::cbf_enabled() const {
    // TODO
    return {};
}

bool Logic::vanilla_click_physics() const {
    // TODO
    return {};
}

bool Logic::defer_physics() const {
    // TODO
    return {};
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
    // TODO
}

void Logic::remember_move(PlayerButtonCommand const& cmd) {
    // TODO
}

void run_orb_clicks(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt) {
    // later
}

void Logic::play_macro(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt, bool clearInputQueue) {
    // TODO
}

void Logic::offset_frames(int delta, int first, int last) {
    // TODO
}

void Logic::offset_inputs(int magnitude, int first, int last) {
    // TODO
}

void Logic::remove_player_inputs(bool player2) {
    // TODO
}

void Logic::flip_input_holds() {
    // TODO
}

void Logic::flip_input_players() {
    // TODO
}

void Logic::sort_inputs() {
    // TODO
}

uint64_t Logic::mint_input_id() {
    return ++Logic::get().next_input_id;
}

void Logic::identify_inputs(bool force) {
    // TODO
}

void Logic::clear_inputs() {
    // todo
}

void Logic::notify(std::string const& text, bool success) {
    // TODO
}

void Logic::note_click(PlayLayer* play, bool down, bool player1, int button) {
    // port this
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
    // todo
    return {};
}

bool Logic::burst_too_fast(Session const& value, int clicks, int first, int last) const {
    // todo
    return {};
}

bool Logic::player_cps_broken(std::deque<int> const& clicks, int now, float max_per_second) const {
    // TODO
    return {};
}

void Logic::set_tps(float rate) {
    // TODO
}

void Logic::place_rate(int frame, float rate) {
    // later
}

void Logic::apply_rate(int frame) {
    // todo
}

void Logic::normalize_rates() {
    // TODO
}

void Logic::trim_rates(int frame) {
    // TODO
}

void Logic::update_cps(PlayLayer* play) {
    // TODO
}

void Logic::tick_autoclicker(PlayLayer* play) {
    // TODO
}

int Logic::follow_steps() const {
    // TODO
    return {};
}

bool Logic::follow_active(GJBaseGameLayer* play) const {
    // TODO
    return {};
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
    // port this
}

void Logic::run_follow(GJBaseGameLayer* play) {
    // TODO
}
