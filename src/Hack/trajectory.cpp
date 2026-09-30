// shows where the player will go
//
// 2 fake players get the real players state copied on & run physics steps, once as if the button is held & once released
// each path is drawn as lines w/ a marker where it dies
// predicting must NEVER touch the real attempt: triggers dont fire for real, deaths stay on the fake, the random state is put back after
// the same thing answers "does the next step kill me" (pause before kill, the autoclicker) & "how long do i live if i flip now" (find best frame)
#include "Hack/trajectory.hpp"
#include "Hack/trajectory_path.hpp"
#include "Hack/orbs.hpp"
#include "Logic/logic.hpp"
#include "Physics/checkpoint.hpp"
#include "Physics/collisions.hpp"
#include "Physics/object.hpp"
#include "Physics/prediction_art.hpp"
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace {
}

void Trajectory::park(PlayerObject* player) {
    if (!player)
        return;
    player->setVisible(false);
    player->m_isHidden = true;
    player->stopAllActions();
    player->unscheduleUpdate();
    // if i leave the fake here the real player hits it next step xd
    player->setPosition({ -100000.f, -100000.f });
    player->m_position = CCPoint{ -100000.f, -100000.f };
    if (auto* play = PlayLayer::get())
        play->removeObjectFromSection(player);
}

PlayerObject* Trajectory::spawn_fake(PlayLayer* play, char const* id) {
    auto* player = PlayerObject::create(1, 1, play, play, true);
    player->retain();
    player->setID(id);
    player->setVisible(false);
    player->m_isHidden = true;
    player->setOpacity(0);
    player->stopAllActions();
    player->unscheduleUpdate();
    player->setPosition({ -100000.f, -100000.f });
    if (player->m_iconSprite)
        player->m_iconSprite->setVisible(false);
    if (player->m_iconSpriteSecondary)
        player->m_iconSpriteSecondary->setVisible(false);
    if (player->m_iconSpriteWhitener)
        player->m_iconSpriteWhitener->setVisible(false);
    player->removeFromParentAndCleanup(false);
    park(player);
    return player;
}

void Trajectory::attach(PlayLayer* play) {
    detach();
    if (!play || !play->m_objectLayer)
        return;
    node = CCDrawNode::create();
    node->retain();
    node->m_bUseArea = false;
    node->setID("trajectory"_spr);
    node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
    Logic::get().seat_overlay(node, play);
    fake1 = spawn_fake(play, "trajectory-player-1"_spr);
    fake2 = spawn_fake(play, "trajectory-player-2"_spr);
}

void Trajectory::sync_seat(PlayLayer* play) {
    if (!node || !play)
        return;
    Logic::get().seat_overlay(node, play);
}

void Trajectory::detach() {
    auto drop = [](auto*& object) {
        if (!object)
            return;
        object->stopAllActions();
        object->unscheduleAllSelectors();
        object->removeFromParentAndCleanup(true);
        object->release();
        object = nullptr;
    };
    drop(fake1);
    drop(fake2);
    drop(node);
    used_p1.clear();
    used_p2.clear();
    busy = false;
    cached = false;
    clicked_frame = -1;
    delta = 0.f;
    Logic::get().predicting = false;
    Logic::get().sync_predict_hooks();
}

void Trajectory::note_button(bool player1, bool down) {
    (player1 ? held_p1 : held_p2) = down;
}

bool Trajectory::mark_dead(PlayerObject* player) {
    if (!player)
        return false;
    if (player == fake1)
        dead_p1 = true;
    else if (player == fake2)
        dead_p2 = true;
    else
        return false;
    return true;
}

bool Trajectory::mark_predict_death(PlayerObject* player) {
    if (mark_dead(player))
        return true;
    if (colliding_fake && mark_dead(colliding_fake))
        return true;
    auto* play = PlayLayer::get();
    if (!play || !player)
        return false;
    if (player == play->m_player1) {
        dead_p1 = true;
        return true;
    }
    if (player == play->m_player2) {
        dead_p2 = true;
        return true;
    }
    return false;
}

bool Trajectory::fakeActivated(PlayerObject* player, EnhancedGameObject* object) const {
    if (!object || object->m_isMultiActivate)
        return false;
    if (player == fake1)
        return used_p1.contains(object);
    if (player == fake2)
        return used_p2.contains(object);
    return false;
}

bool Trajectory::realActivated(PlayerObject* player, EnhancedGameObject* object) const {
    if (!object)
        return false;
    auto* layer = GJBaseGameLayer::get();
    PlayerObject* real = player;
    if (layer && player == fake1)
        real = layer->m_player1;
    else if (layer && player == fake2)
        real = layer->m_player2;
    return phys::hasBeenActivatedByPlayer(real, object);
}

void Trajectory::rememberActivation(EnhancedGameObject* object, PlayerObject* player) {
    if (player == fake1)
        used_p1.insert(object);
    else if (player == fake2)
        used_p2.insert(object);
}

ccColor4F Trajectory::color_for(PlayerObject* fake, Click click) const {
    auto const& logic = Logic::get();
    float const* c = logic.traj_color;
    switch (click) {
    case Click::Swift:
        c = logic.traj_swift_color;
        break;
    case Click::Release:
        c = logic.traj_release_color;
        break;
    case Click::Hold:
    case Click::Replay:
        break;
    }
    if (fake == fake2)
        return { 1.f - c[0], 1.f - c[1], 1.f - c[2], c[3] };
    return { c[0], c[1], c[2], c[3] };
}

float Trajectory::lineWidth(PlayLayer* play) const {
    return Logic::get().traj_width / std::max(play->m_gameState.m_cameraZoom, 0.05f);
}

Trajectory::Key Trajectory::make_key(PlayLayer* play) const {
    auto const& logic = Logic::get();
    Key key;
    key.frame = Logic::get().get_frame(play);
    auto fill = [](PlayerObject* player, float* out) {
        if (!player)
            return;
        out[0] = player->m_position.x;
        out[1] = player->m_position.y;
        out[2] = player->getRotation();
        out[3] = static_cast<float>(player->m_yVelocity);
        out[4] = static_cast<float>(player->m_platformerXVelocity);
        out[5] = static_cast<float>((player->m_holdingLeft ? 1 : 0) | (player->m_holdingRight ? 2 : 0) | (player->m_leftPressedFirst ? 4 : 0));
    };
    fill(play->m_player1, key.p1);
    fill(play->m_player2, key.p2);
    key.dual = play->m_gameState.m_isDualMode;
    auto jump_held = [](PlayerObject* body) {
        if (!body)
            return false;
        int jump = static_cast<int>(PlayerButton::Jump);
        auto it = body->m_holdingButtons.find(jump);
        return it != body->m_holdingButtons.end() && it->second;
    };
    if (logic.maintain_gravity) {
        key.held_p1 = Logic::get().physical_hold(play, false);
        key.held_p2 = Logic::get().physical_hold(play, true);
    }
    else {
        key.held_p1 = jump_held(play->m_player1);
        key.held_p2 = jump_held(play->m_player2);
    }
    key.dead = play->m_playerDied;
    key.playing = logic.is_playing();
    key.recording = logic.is_recording();
    key.hold = logic.traj_hold;
    key.swift = logic.traj_swift;
    key.release = logic.traj_release;
    key.dash_orbs = Orbs::click_orb_assist(false) || Orbs::click_orb_assist(true);
    key.black_orbs = key.dash_orbs && logic.click_black_orbs;
    key.pad_black = logic.pad_black_orb;
    key.pad_blue = logic.pad_blue_orb;
    key.pause_kill = logic.pause_before_kill && logic.is_recording() && logic.show_traj;
    key.pause_length = logic.pause_at_traj_length;
    key.pause_ms = logic.pause_traj_ms;
    key.hide_p1 = Logic::get().traj_player_hidden(false);
    key.hide_p2 = Logic::get().traj_player_hidden(true);
    key.orbs = Orbs::pose_hash(play, play->m_player1);
    if (play->m_player2)
        key.orbs ^= Orbs::pose_hash(play, play->m_player2) + 0x9e3779b9u; // same magic number as hash_combine
    key.seconds = logic.traj_seconds;
    key.width = logic.traj_width;
    key.zoom = play->m_gameState.m_cameraZoom;
    std::copy(std::begin(logic.traj_color), std::end(logic.traj_color), key.color);
    std::copy(std::begin(logic.traj_swift_color), std::end(logic.traj_swift_color), key.swift_color);
    std::copy(std::begin(logic.traj_release_color), std::end(logic.traj_release_color), key.release_color);
    return key;
}

// w silly cat skid
void Trajectory::seed(PlayerObject* fake, PlayerObject* real) {
    fake->copyAttributes(real);
    SavedPlayerCheckpoint checkpoint = SavedPlayerCheckpoint::create(real);
    checkpoint.apply(fake);
    fake->m_pendingCheckpoint = nullptr;
    fake->m_maybeReducedEffects = real->m_maybeReducedEffects;
    fake->m_isHidden = true;
    fake->m_playEffects = false;
    fake->setPosition(real->m_position);
    fake->setRotation(real->getRotation());
    fake->m_isObjectRectDirty = true;
    fake->updateOrientedBox();
}

void Trajectory::dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on) {
    auto& state = layer->m_gameState;
    if (state.m_isDualMode == on || !fake1 || !fake2)
        return;
    bool twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    auto with_fakes = [&](auto&& action) {
        PlayerObject* one = layer->m_player1;
        PlayerObject* two = layer->m_player2;
        layer->m_player1 = fake1;
        layer->m_player2 = fake2;
        action();
        layer->m_player1 = one;
        layer->m_player2 = two;
    };
    if (on) {
        if (player != fake1 || pairing)
            return;
        state.m_isDualMode = true;
        fake1->m_unkA99 = true;
        if (join_real)
            seed(fake2, join_real);
        fake2->m_unkA99 = true;
        fake2->resetObject();
        fake2->copyAttributes(fake1);
        bool unlinked = state.m_unlinkDualGravity;
        fake2->flipGravity(unlinked ? fake1->m_isUpsideDown : !fake1->m_isUpsideDown, true);
        fake2->setYVelocity(unlinked ? fake1->m_yVelocity : -fake1->m_yVelocity, 0);
        fake2->m_isOnGround = false;
        fake2->m_isOnGround2 = false;
        if (twoPlayer) {
            if (!fake2->m_inputsLocked)
                fake2->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = false;
            if (!fake2->m_controlsDisabled) {
                fake2->m_jumpBuffered = false;
                fake2->m_stateRingJump = false;
                fake2->m_touchedPad = true;
                if (fake2->m_isDashing)
                    fake2->stopDashing();
            }
        }
        fake2->setVisible(false);
        fake2->m_isHidden = true;
        fake2->m_playEffects = false;
        fake2->m_isObjectRectDirty = true;
        state.m_lastActivatedPortal2 = portal;
        with_fakes([&] {
            layer->playerWillSwitchMode(player, portal);
        });
        joined = joinable;
        return;
    }
    state.m_isDualMode = false;
    fake1->m_unkA99 = false;
    fake2->m_unkA99 = false;
    if (player == fake2)
        fake1->copyAttributes(fake2);
    fake2->releaseAllButtons();
    state.m_lastActivatedPortal2 = nullptr;
    int mode = 6;
    if (fake1->m_isShip || fake1->m_isBird || fake1->m_isDart || fake1->m_isSwing)
        mode = 5;
    else if (fake1->m_isBall)
        mode = 0x10;
    else if (fake1->m_isSpider)
        mode = 0x21;
    with_fakes([&] {
        layer->updateDualGround(player, mode, false, 0.f);
    });
    parted = true;
}

void Trajectory::press(PlayLayer* play, PlayerObject* fake, Click click) {
    auto& logic = Logic::get();
    bool stash_orbs = Orbs::click_orb_assist(fake == fake2);
    if (stash_orbs)
        logic.suppress_rings = true;
    switch (click) {
    case Click::Replay:
        break;
    case Click::Hold:
        fake->pushButton(PlayerButton::Jump);
        break;
    case Click::Swift:
        fake->releaseButton(PlayerButton::Jump);
        fake->m_jumpBuffered = false;
        fake->pushButton(PlayerButton::Jump);
        fake->releaseButton(PlayerButton::Jump);
        break;
    case Click::Release:
        fake->releaseButton(PlayerButton::Jump);
        fake->m_jumpBuffered = false;
        break;
    }
    if (stash_orbs)
        logic.suppress_rings = false;
}

bool Trajectory::advance(PlayLayer* play, PlayerObject* fake, float step_delta, bool draw, Click click) {
    CCPoint from = fake->getPosition();

    // w nat
    fake->m_totalTime += Logic::get().physics_step();
    auto clear_log = [&](CCDictionary* log) {
        if (!log)
            return;
        auto shared = [&](PlayerObject* body) {
            if (!body)
                return false;
            return log == body->m_collisionLogTop || log == body->m_collisionLogBottom || log == body->m_collisionLogLeft || log == body->m_collisionLogRight;
        };
        if (shared(play->m_player1) || shared(play->m_player2))
            return;
        log->removeAllObjects();
    };
    clear_log(fake->m_collisionLogTop);
    clear_log(fake->m_collisionLogBottom);
    clear_log(fake->m_collisionLogLeft);
    clear_log(fake->m_collisionLogRight);
    fake->m_playEffects = false;
    bool live_was_dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    fake->update(step_delta);
    fake->m_unkUnused3 = fake->getRotation();
    fake->updateRotation(step_delta);
    fake->m_shipRotation = fake->getPosition();
    PlayerObject*& slot = (fake == fake2) ? play->m_player2 : play->m_player1;
    PlayerObject* saved_slot = slot;
    if (slot)
        slot = fake;
    colliding_fake = fake;
    int hit = play->checkCollisions(fake, step_delta, false);
    colliding_fake = nullptr;
    if (slot)
        slot = saved_slot;
    (void)hit;
    if (fake->m_isDead)
        mark_dead(fake);
    bool live_now_dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    if (!live_was_dead && live_now_dead)
        mark_dead(fake);
    phys::checkSpawnObjects(play, fake);

    bool dead = (fake == fake1 ? dead_p1 : dead_p2) || fake->m_isDead;
    if (dead) {
        if (fake == fake1)
            dead_p1 = true;
        else if (fake == fake2)
            dead_p2 = true;
        if (draw) {
            flush_path(play, fake, click);
            CCPoint saved = fake->getPosition();
            fake->setPosition(from);
            draw_death(fake);
            fake->setPosition(saved);
        }
        return true;
    }
    if (draw) {
        auto& path = paths[fake == fake2];
        if (path.empty())
            path.push_back(from);
        path.push_back(fake->getPosition());
    }
    return false;
}

void Trajectory::flush_path(PlayLayer* play, PlayerObject* fake, Click click) {
    auto& path = paths[fake == fake2];
    float width = lineWidth(play);
    auto color = color_for(fake, click);
    echo::trajectory::draw_path(path, [&](CCPoint a, CCPoint b) {
        node->drawSegment(a, b, width, color);
    });
    path.clear();
}

void Trajectory::draw_death(PlayerObject* fake) {
    auto* play = PlayLayer::get();
    if (!play)
        return;
    auto const& logic = Logic::get();
    float width = lineWidth(play);
    auto color = [](float const* c) {
        return ccColor4F{ c[0], c[1], c[2], c[3] };
    };
    node->drawRect(fake->getObjectRect(), { 0.f, 0.f, 0.f, 0.f }, width, color(logic.hitbox_outer_color), BorderAlignment::Inside);
    node->drawRect(fake->getObjectRect(0.3f, 0.3f), { 0.f, 0.f, 0.f, 0.f }, width, color(logic.hitbox_inner_color), BorderAlignment::Inside);
}

void Trajectory::simulate(PlayLayer* play, bool player1, Click click, bool both, bool draw, int limit, int measure) {
    auto& logic = Logic::get();
    constexpr int MEASURE_LIVE = 1, MEASURE_HOLD = 2, MEASURE_RELEASE = 4;
    bool watch = measure != 0;
    PlayerObject* fake = player1 ? fake1 : fake2;
    PlayerObject* real = player1 ? play->m_player1 : play->m_player2;
    PlayerObject* partner = otherFake(fake);
    PlayerObject* partner_real = player1 ? play->m_player2 : play->m_player1;
    if (!fake || !real)
        return;
    auto random_state = GameToolbox::getfast_srand();
    auto teleport_roll = logic.tp_state;
    auto shake_roll = logic.shake_state;
    bool pair = both && play->m_gameState.m_isDualMode && partner && partner_real;
    auto* manager = GameManager::get();
    bool performance = manager && manager->m_performanceMode;
    if (manager)
        manager->m_performanceMode = true;

    auto first = SavedPlayerCheckpoint::create(play->m_player1);
    auto second = play->m_player2 ? SavedPlayerCheckpoint::create(play->m_player2) : SavedPlayerCheckpoint();
    bool has_second = play->m_player2 != nullptr;
    bool died = play->m_playerDied;
    auto queued = play->m_queuedButtons;
    auto recorded = play->m_queuedRecordedButtons;
    int recorded_size = play->m_queuedRecordedButtonsSize;
    play->m_playerDied = false;
    auto logs_of = [](PlayerObject* body) {
        return body ? std::array{ body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight } : std::array<CCDictionary*, 4>{};
    };

    struct Trails {
        HardStreak* wave = nullptr;
        CCMotionStreak* regular = nullptr;
        GhostTrailEffect* ghost = nullptr;
    };

    auto trails_of = [](PlayerObject* body) {
        if (!body)
            return Trails{};
        return Trails{ body->m_waveTrail, body->m_regularTrail, body->m_ghostTrail };
    };
    auto logs1 = logs_of(play->m_player1);
    auto logs2 = logs_of(play->m_player2);
    auto trails1 = trails_of(play->m_player1);
    auto trails2 = trails_of(play->m_player2);
    auto* layer = play->m_objectLayer;
    CCPoint layer_pos = layer ? layer->getPosition() : CCPoint{};
    float layer_scale_x = layer ? layer->getScaleX() : 1.f, layer_scale_y = layer ? layer->getScaleY() : 1.f;
    float layer_rotation = layer ? layer->getRotation() : 0.f;
    auto restore_players = [&] {
        auto put_body = [](PlayerObject* body, SavedPlayerCheckpoint& shot, std::array<CCDictionary*, 4> const& logs, Trails const& trails) {
            if (!body)
                return;
            shot.apply(body);
            body->m_collisionLogTop = logs[0];
            body->m_collisionLogBottom = logs[1];
            body->m_collisionLogLeft = logs[2];
            body->m_collisionLogRight = logs[3];
            body->m_waveTrail = trails.wave;
            body->m_regularTrail = trails.regular;
            body->m_ghostTrail = trails.ghost;
        };
        put_body(play->m_player1, first, logs1, trails1);
        if (has_second)
            put_body(play->m_player2, second, logs2, trails2);
        play->m_playerDied = died;
        play->m_queuedButtons = queued;
        play->m_queuedRecordedButtons = recorded;
        play->m_queuedRecordedButtonsSize = recorded_size;
        if (layer) {
            layer->setPosition(layer_pos);
            layer->setScaleX(layer_scale_x);
            layer->setScaleY(layer_scale_y);
            layer->setRotation(layer_rotation);
        }
    };

    Orbs::reset_prediction_pads();
    seed(fake, real);
    if (pair)
        seed(partner, partner_real);
    fake->m_controlsDisabled = false;
    if (pair && partner)
        partner->m_controlsDisabled = false;
    dead_p1 = false;
    dead_p2 = false;

    std::array<PlayerObject*, 2> bodies = { fake, pair ? partner : nullptr };
    auto each_body = [&](auto&& action) {
        for (auto* body : bodies) {
            if (body)
                action(body);
        }
    };

    bool played = logic.plays_player(!player1);
    bool clicker = !played && logic.autoclicker && logic.records_player(!player1) && (player1 ? logic.autoclick_p1 : logic.autoclick_p2);
    auto jump_held = [](PlayerObject* body) {
        if (!body)
            return false;
        int jump = static_cast<int>(PlayerButton::Jump);
        auto it = body->m_holdingButtons.find(jump);
        return it != body->m_holdingButtons.end() && it->second;
    };
    bool key_held = Logic::get().physical_hold(play, !player1);
    bool started_held = jump_held(real) || (logic.maintain_gravity && key_held && real->m_isDart);
    AutoClick clock = logic.auto_click;
    if (clicker) {
        bool stash_orbs = Orbs::click_orb_assist(!player1) || (pair && Orbs::click_orb_assist(player1));
        if (stash_orbs)
            logic.suppress_rings = true;
        each_body([&](PlayerObject* body) {
            if (clock.pressed)
                body->pushButton(PlayerButton::Jump);
            else
                body->releaseButton(PlayerButton::Jump);
        });
        if (stash_orbs)
            logic.suppress_rings = false;
    }
    else {
        each_body([&](PlayerObject* body) {
            body->m_controlsDisabled = false;
            body->m_isLocked = false;
            bool already_held = jump_held(body);
            if (!already_held && logic.maintain_gravity && key_held && body->m_isDart && click == Click::Hold) {
                already_held = true;
            }
            // holding thru stays a hold not new press
            if (click == Click::Hold && already_held)
                return;
            if (click != Click::Release && click != Click::Replay)
                body->m_jumpBuffered = false;
            press(play, body, click);
        });
    }

    float warp = play->m_gameState.m_timeWarp;
    float step_delta = delta > 0.f ? delta : logic.physics_step() * 60.f * std::min(warp, 1.f);
    // a macro can change tps part way so each step gets its own length (NOT the last real frames)
    bool varying = logic.is_playing() && !logic.rates.empty();
    float step_len = logic.physics_step();
    if (varying) {
        step_len = 1.f / std::max(logic.rate_at(logic.get_frame(play)), 1.f);
        step_delta = step_len * 60.f * std::min(warp, 1.f);
    }
    int steps = echo::trajectory::prediction_steps(logic.traj_seconds, logic.sample_rate(), warp);
    if (limit > 0)
        steps = std::min(steps, limit);
    if (draw) {
        paths[fake == fake2].reserve(steps + 1);
        if (partner)
            paths[partner == fake2].reserve(steps + 1);
    }
    int frame = logic.get_frame(play);
    size_t cursor = logic.replay_pos;
    constexpr int HOLD_BUTTONS[] = { static_cast<int>(PlayerButton::Jump), static_cast<int>(PlayerButton::Left), static_cast<int>(PlayerButton::Right) };
    std::array<std::array<bool, 3>, 2> macro_held{};
    std::array<int, 2> last_input = { -1, -1 };
    if (logic.is_playing()) {
        for (int side = 0; side < 2; ++side) {
            if (!logic.plays_player(side == 1))
                continue;
            for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b)
                macro_held[side][b] = Logic::get().held_at(logic, side == 1, frame, HOLD_BUTTONS[b]);
        }
        for (size_t i = logic.inputs.size(); i-- > 0 && (last_input[0] < 0 || last_input[1] < 0);) {
            int& last = last_input[logic.inputs[i].player2 ? 1 : 0];
            if (last < 0)
                last = logic.inputs[i].number;
        }
    }
    bool done1 = false, done2 = !pair;
    int live_steps1 = 0, live_steps2 = 0;
    auto poses = Orbs::capture(play, real);
    if (pair) {
        auto extra = Orbs::capture(play, partner_real);
        poses.insert(poses.end(), extra.begin(), extra.end());
    }
    auto restore_orbs = [&] {
        Orbs::restore(poses);
    };
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool ended1 = false, ended2 = false;
    pairing = pair;
    join_real = partner_real;
    joinable = player1 && !pair && partner == fake2 && partner_real && !Logic::get().traj_player_hidden(true);
    joined = false;
    parted = false;
    auto portals = [&] {
        if (std::exchange(joined, false)) {
            pair = true;
            pairing = true;
            done2 = false;
            partner->m_controlsDisabled = false;
            partner->m_isLocked = false;
            if (!twoPlayer)
                bodies[1] = partner;
            if (draw)
                paths[partner == fake2].reserve(steps + 1);
        }
        if (std::exchange(parted, false)) {
            pairing = false;
            if (fake == fake2) {
                ended1 = !done1;
                done1 = true;
            }
            else if (pair) {
                ended2 = !done2;
                done2 = true;
                bodies[1] = nullptr;
            }
        }
    };
    std::array<std::unordered_set<int>, 2> orb_clicks;

    struct SavedFlag {
        EnhancedGameObject* object;
        bool p1, p2;
    };

    std::vector<SavedFlag> flags;
    for (auto const& pose : poses) {
        auto* ring = static_cast<EnhancedGameObject*>(pose.object);
        if (ring)
            flags.push_back({ ring, ring->m_activatedByPlayer1, ring->m_activatedByPlayer2 });
    }
    auto snap_rings = [](PlayerObject* player) {
        std::vector<CCObject*> rings;
        if (!player || !player->m_touchingRings)
            return rings;
        unsigned int count = player->m_touchingRings->count();
        rings.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
            rings.push_back(player->m_touchingRings->objectAtIndex(i));
        return rings;
    };
    auto put_rings = [](PlayerObject* player, std::vector<CCObject*> const& rings) {
        if (!player || !player->m_touchingRings)
            return;
        player->m_touchingRings->removeAllObjects();
        for (auto* ring : rings) {
            if (ring)
                player->m_touchingRings->addObject(ring);
        }
    };
    auto rings1 = snap_rings(play->m_player1);
    auto rings2 = snap_rings(play->m_player2);
    auto restore_live = [&] {
        restore_orbs();
        for (auto const& flag : flags) {
            if (!flag.object)
                continue;
            flag.object->m_activatedByPlayer1 = flag.p1;
            flag.object->m_activatedByPlayer2 = flag.p2;
        }
        put_rings(play->m_player1, rings1);
        put_rings(play->m_player2, rings2);
    };

    std::vector<CCPoint> player_path;
    bool note_path = (measure & MEASURE_LIVE) && fake == fake1 && steps >= 30 && !died;
    if (note_path) {
        player_path.reserve(static_cast<size_t>(steps) + 1);
        player_path.push_back(fake->getPosition());
    }
    for (int i = 0; i < steps && !(done1 && done2); ++i) {
        if (varying) {
            step_len = 1.f / std::max(logic.rate_at(frame + i), 1.f);
            step_delta = step_len * 60.f * std::min(warp, 1.f);
        }
        {
            constexpr float REACH = 240.f;
            auto at = fake->getPosition();
            float left = at.x, right = at.x, bottom = at.y, top = at.y;
            if (pair && partner) {
                auto other = partner->getPosition();
                left = std::min(left, other.x);
                right = std::max(right, other.x);
                bottom = std::min(bottom, other.y);
                top = std::max(top, other.y);
            }
        }
        std::array<bool, 2> orb_press{};
        if (played || (pair && logic.is_playing())) {
            int at = frame + i + 1;
            bool shared = pair && logic.records_player(false) == logic.records_player(true) && (!logic.is_playing() || logic.play_p1 == logic.play_p2);
            auto send = [&](PlayerObject* target, bool player2, PlayerButton button, bool holding) {
                auto apply = [&](PlayerObject* body, bool icon2) {
                    if (!body)
                        return;
                    if (holding) {
                        body->pushButton(button);
                        if (button == PlayerButton::Jump)
                            orb_press[icon2 ? 1 : 0] = true;
                    }
                    else
                        body->releaseButton(button);
                };
                apply(target, player2);
                if (shared)
                    apply(otherFake(target), !player2);
            };
            std::array<std::array<bool, 3>, 2> sent{};
            bool two_player_lvl = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
            while (cursor < logic.inputs.size() && logic.inputs[cursor].number <= at) {
                auto const& input = logic.inputs[cursor++];
                int raw = input.button == 0 ? 1 : input.button;
                // outside 2 player levels gd gives p2 inputs to p1 (silicate macros can be all p2)
                bool to_p2 = input.player2 && two_player_lvl;
                send(to_p2 ? fake2 : fake1, to_p2, static_cast<PlayerButton>(raw), input.holding);
                int side = to_p2 ? 1 : 0;
                for (int b = 0; b < 3; ++b) {
                    if (HOLD_BUTTONS[b] != raw)
                        continue;
                    macro_held[side][b] = input.holding;
                    sent[side][b] = true;
                }
            }
            for (int side = 0; side < (two_player_lvl ? 2 : 1); ++side) {
                if (!logic.plays_player(side == 1))
                    continue;
                if (!logic.ignore_inputs && at > last_input[side])
                    continue;
                PlayerObject* body = side == 1 ? fake2 : fake1;
                if (body != fake && !(pair && body == partner))
                    continue;
                for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b) {
                    if (sent[side][b])
                        continue;
                    if (b > 0 && body->m_controlsDisabled)
                        continue;
                    if (Logic::button_held(body, HOLD_BUTTONS[b]) == macro_held[side][b])
                        continue;
                    send(body, side == 1, static_cast<PlayerButton>(HOLD_BUTTONS[b]), macro_held[side][b]);
                }
            }
        }

        bool pressed_now = i == 0 && (click == Click::Swift || (click == Click::Hold && !started_held));
        if (clicker) {
            if (clock.pressed)
                ++clock.since_press;
            else
                ++clock.since_release;
            bool flip = clock.pressed ? clock.since_press >= clock.press_interval : clock.since_release >= clock.release_interval;
            if (flip) {
                clock.pressed = !clock.pressed;
                clock.since_press = 0;
                clock.since_release = 0;
                pressed_now = clock.pressed;
                bool stash_orbs = Orbs::click_orb_assist(!player1) || (pair && Orbs::click_orb_assist(player1));
                if (stash_orbs)
                    logic.suppress_rings = true;
                each_body([&](PlayerObject* body) {
                    if (clock.pressed)
                        body->pushButton(PlayerButton::Jump);
                    else
                        body->releaseButton(PlayerButton::Jump);
                });
                if (stash_orbs)
                    logic.suppress_rings = false;
                if (clock.pressed && clock.press_interval <= 0) {
                    clock.pressed = false;
                    pressed_now = false;
                    each_body([&](PlayerObject* body) {
                        body->releaseButton(PlayerButton::Jump);
                    });
                }
            }
        }

        auto maintain = [&](PlayerObject* body) {
            bool icon2 = body == fake2;
            if (!body->m_isDart || logic.plays_player(icon2) || !logic.maintain_gravity || !logic.records_player(icon2))
                return;
            bool physical = clicker ? clock.pressed : click == Click::Hold;
            bool stash_orbs = Orbs::click_orb_assist(icon2);
            if (stash_orbs)
                logic.suppress_rings = true;
            for (int step = 0; step < 8 && !body->m_controlsDisabled; ++step) {
                if (body->m_isUpsideDown != (physical == body->m_jumpBuffered))
                    break;
                bool before_flip = body->m_isUpsideDown, before_buffer = body->m_jumpBuffered;
                if (physical != body->m_isUpsideDown) {
                    body->pushButton(PlayerButton::Jump);
                }
                else
                    body->releaseButton(PlayerButton::Jump);
                if (body->m_isUpsideDown == before_flip && body->m_jumpBuffered == before_buffer)
                    break;
            }
            if (stash_orbs)
                logic.suppress_rings = false;
        };
        each_body(maintain);
        each_body([&](PlayerObject* body) {
            bool icon2 = body == fake2;
            bool fresh = orb_press[icon2 ? 1 : 0] || (!logic.plays_player(icon2) && pressed_now);
            Orbs::click_fake(play, body, icon2, orb_clicks[icon2 ? 1 : 0], fresh);
            Orbs::apply_prediction_pads(play, body, icon2);
        });
        each_body(maintain);

        // w skidicate
        auto& state = play->m_gameState;
        float dt = varying ? step_len : logic.physics_step();
        state.m_totalTime += dt;
        state.m_unkDouble3 += dt / state.m_timeWarp;
        state.m_currentProgress++;
        state.m_unkUint5 += static_cast<int>(roundf(state.m_timeWarp * 1000.0f));
        float timeMod = *reinterpret_cast<float*>(&state.m_timeModRelated);
        if (timeMod != 0.0f) {
            state.m_timeModRelated = 0;
            state.m_timeModRelated2 = 0;
            each_body([&](PlayerObject* body) {
                body->updateTimeMod(timeMod, true);
            });
        }

        if (!done1)
            fake->resetTouchedRings(false);
        if (!done2 && partner)
            partner->resetTouchedRings(false);
        bool was1 = done1, was2 = done2;
        if (!done1)
            done1 = advance(play, fake, step_delta, draw, click);
        if (note_path && !done1)
            player_path.push_back(fake->getPosition());
        if (path_sink && !done1)
            path_sink->push_back(fake->getPosition());
        portals();
        if (!done2)
            done2 = advance(play, partner, step_delta, draw, click);
        portals();
        restore_orbs();
        if ((measure & MEASURE_LIVE) && i == 0 && !died) {
            if (player1) {
                if (!was1 && done1 && !ended1)
                    logic.death_next_p1 = true;
                if (pair && !was2 && done2 && !ended2)
                    logic.death_next_p2 = true;
            }
            else if (!was1 && done1 && !ended1) {
                logic.death_next_p2 = true;
            }
        }
        if (watch) {
            if (!was1 && !done1)
                ++live_steps1;
            if (pair && !was2 && !done2)
                ++live_steps2;
        }
    }

    if (watch) {
        measured[0] = done1 && !ended1 ? live_steps1 : steps;
        measured[1] = pair && done2 && !ended2 ? live_steps2 : steps;
        measured_full = steps;
        float scale = std::clamp(warp, 0.01f, 1.f) / static_cast<float>(logic.sample_rate());
        auto store = [&](float& alive1, bool& dies1, float& alive2, bool& dies2) {
            if (player1) {
                alive1 = static_cast<float>(live_steps1) * scale;
                dies1 = done1 && !ended1;
                if (pair) {
                    alive2 = static_cast<float>(live_steps2) * scale;
                    dies2 = done2 && !ended2;
                }
            }
            else {
                alive2 = static_cast<float>(live_steps1) * scale;
                dies2 = done1 && !ended1;
            }
        };
        if (measure & MEASURE_LIVE)
            store(logic.traj_alive_p1, logic.traj_dies_p1, logic.traj_alive_p2, logic.traj_dies_p2);
        if (measure & MEASURE_HOLD) {
            if (player1) {
                logic.traj_hold_alive_p1 = static_cast<float>(live_steps1) * scale;
                if (pair)
                    logic.traj_hold_alive_p2 = static_cast<float>(live_steps2) * scale;
            }
            else {
                logic.traj_hold_alive_p2 = static_cast<float>(live_steps1) * scale;
            }
        }
        if (measure & MEASURE_RELEASE) {
            if (player1) {
                logic.traj_release_alive_p1 = static_cast<float>(live_steps1) * scale;
                if (pair)
                    logic.traj_release_alive_p2 = static_cast<float>(live_steps2) * scale;
            }
            else {
                logic.traj_release_alive_p2 = static_cast<float>(live_steps1) * scale;
            }
        }
    }

    pairing = false;
    joinable = false;
    join_real = nullptr;
    if (manager)
        manager->m_performanceMode = performance;
    if (draw) {
        flush_path(play, fake, click);
        if (partner)
            flush_path(play, partner, click);
    }
    restore_live();
    park(fake);
    if (partner)
        park(partner);
    used_p1.clear();
    used_p2.clear();
    restore_players();
    GameToolbox::fast_srand(random_state);
    logic.tp_state = teleport_roll;
    logic.shake_state = shake_roll;
}

void Trajectory::refresh(PlayLayer* play) {
    if (!play || !play->m_started || !play->m_player1 || !node || !fake1 || !fake2)
        return;
    auto& logic = Logic::get();
    // do this or breaks traj on end
    if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
        if (node->isVisible())
            node->setVisible(false);
        park(fake1);
        park(fake2);
        if (cached || logic.death_next_p1 || logic.death_next_p2) {
            cached = false;
            logic.death_next_p1 = false;
            logic.death_next_p2 = false;
            Logic::get().sync_death_label(play);
        }
        return;
    }
    bool rendering = Logic::get().render_capturing();
    bool draw = logic.show_traj;
    bool need_predict = draw;
    if (!need_predict) {
        if (node->isVisible())
            node->setVisible(false);
        bool pause_kill = logic.pause_before_kill && logic.is_recording();
        if (!rendering && (logic.show_death_next || pause_kill)) {
            lookahead(play);
            Logic::get().check_advance_pause(play);
            Logic::get().sync_death_label(play);
            return;
        }
        if (cached || logic.death_next_p1 || logic.death_next_p2 || logic.traj_dies_p1 || logic.traj_dies_p2) {
            cached = false;
            logic.death_next_p1 = false;
            logic.death_next_p2 = false;
            logic.traj_alive_p1 = 0.f;
            logic.traj_alive_p2 = 0.f;
            logic.traj_dies_p1 = false;
            logic.traj_dies_p2 = false;
            logic.traj_hold_alive_p1 = 0.f;
            logic.traj_hold_alive_p2 = 0.f;
            logic.traj_release_alive_p1 = 0.f;
            logic.traj_release_alive_p2 = 0.f;
            Logic::get().sync_death_label(play);
        }
        return;
    }
    Logic::get().seat_overlay(node, play);
    if (play->m_objectLayer) {
        if (!fake1->getParent())
            play->m_objectLayer->addChild(fake1);
        if (!fake2->getParent())
            play->m_objectLayer->addChild(fake2);
    }
    if (rendering && !logic.show_traj) {
        node->setVisible(false);
        park(fake1);
        park(fake2);
        Logic::get().death_next_p1 = false;
        Logic::get().death_next_p2 = false;
        logic.traj_alive_p1 = 0.f;
        logic.traj_alive_p2 = 0.f;
        logic.traj_dies_p1 = false;
        logic.traj_dies_p2 = false;
        logic.traj_hold_alive_p1 = 0.f;
        logic.traj_hold_alive_p2 = 0.f;
        logic.traj_release_alive_p1 = 0.f;
        logic.traj_release_alive_p2 = 0.f;
        Logic::get().sync_death_label(play);
        return;
    }
    park(fake1);
    park(fake2);

    node->setVisible(draw);

    float saved_seconds = logic.traj_seconds;
    int step_limit = 0;

    Key key = make_key(play);
    key.hold = draw && logic.traj_hold;
    key.swift = draw && logic.traj_swift;
    key.release = draw && logic.traj_release;
    if (cached && key == last_key) {
        logic.traj_seconds = saved_seconds;
        Logic::get().check_advance_pause(play);
        return;
    }

    busy = true;
    phys::prepare_prediction_art();
    logic.predicting = true;
    Logic::get().sync_predict_hooks();
    node->clear();
    auto snap = [](PlayerObject* body) {
        return body ? std::pair{ body->m_jumpBuffered, body->m_holdingButtons } : std::pair{ false, gd::map<int, bool>{} };
    };
    auto p1_hold = snap(play->m_player1);
    auto p2_hold = snap(play->m_player2);
    bool ui1 = false, ui2 = false;
    int touch1 = -1, touch2 = -1;
    if (auto* ui = play->m_uiLayer) {
        ui1 = ui->m_p1Jumping;
        ui2 = ui->m_p2Jumping;
        touch1 = ui->m_p1TouchId;
        touch2 = ui->m_p2TouchId;
    }
    auto restore = [&] {
        auto put = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
            if (!body)
                return;
            body->m_jumpBuffered = hold.first;
            body->m_holdingButtons = hold.second;
        };
        put(play->m_player1, p1_hold);
        put(play->m_player2, p2_hold);
        if (auto* ui = play->m_uiLayer) {
            ui->m_p1Jumping = ui1;
            ui->m_p2Jumping = ui2;
            ui->m_p1TouchId = touch1;
            ui->m_p2TouchId = touch2;
        }
    };

    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    auto live = [&](bool player2) {
        if (logic.plays_player(player2))
            return false;
        return true;
    };
    logic.death_next_p1 = false;
    logic.death_next_p2 = false;
    logic.traj_alive_p1 = 0.f;
    logic.traj_alive_p2 = 0.f;
    logic.traj_dies_p1 = false;
    logic.traj_dies_p2 = false;
    logic.traj_hold_alive_p1 = 0.f;
    logic.traj_hold_alive_p2 = 0.f;
    logic.traj_release_alive_p1 = 0.f;
    logic.traj_release_alive_p2 = 0.f;
    auto jump_held = [](PlayerObject* body) {
        if (!body)
            return false;
        int jump = static_cast<int>(PlayerButton::Jump);
        auto it = body->m_holdingButtons.find(jump);
        return it != body->m_holdingButtons.end() && it->second;
    };
    auto actual = [&](bool player1) {
        if (logic.plays_player(!player1))
            return Click::Replay;
        PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
        if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart) {
            return Logic::get().physical_hold(play, !player1) ? Click::Hold : Click::Release;
        }
        return jump_held(body) ? Click::Hold : Click::Release;
    };
    constexpr int MEASURE_LIVE = 1, MEASURE_HOLD = 2, MEASURE_RELEASE = 4;
    auto run = [&](bool player1, Click click, bool both, bool show, int measure) {
        simulate(play, player1, click, both, show, step_limit, measure);
    };
    auto modes = [&](bool player1, bool both) {
        auto act = actual(player1);
        auto fire = [&](Click click, bool show) {
            int measure = 0;
            if (click == act)
                measure |= MEASURE_LIVE;
            if (logic.pause_at_traj_length && click == Click::Hold)
                measure |= MEASURE_HOLD;
            if (logic.pause_at_traj_length && click == Click::Release)
                measure |= MEASURE_RELEASE;
            if (!show && measure == 0)
                return;
            run(player1, click, both, show, measure);
        };
        if (play->m_playerDied || rendering) {
            fire(act, draw);
            return;
        }
        bool show_hold = draw && logic.traj_hold, show_swift = draw && logic.traj_swift;
        bool show_release = draw && logic.traj_release;
        bool need_hold = show_hold || logic.pause_at_traj_length;
        bool need_release = show_release || logic.pause_at_traj_length;
        if (need_hold)
            fire(Click::Hold, show_hold);
        if (show_swift)
            fire(Click::Swift, true);
        if (need_release)
            fire(Click::Release, show_release);
        bool covered = (act == Click::Hold && need_hold) || (act == Click::Swift && show_swift) || (act == Click::Release && need_release);
        if (!covered)
            fire(act, draw);
    };
    bool hide1 = Logic::get().traj_player_hidden(false), hide2 = Logic::get().traj_player_hidden(true);
    bool linked = dual && !twoPlayer && !hide1 && !hide2;
    if (!hide1) {
        if (!rendering && !play->m_playerDied && logic.plays_player(false))
            run(true, Click::Replay, linked, draw, MEASURE_LIVE);
        if (live(false) || rendering || play->m_playerDied)
            modes(true, linked);
    }
    bool second = dual && !hide2 && !linked && (twoPlayer || live(true) || logic.plays_player(true) || play->m_playerDied);
    if (second) {
        if (!rendering && !play->m_playerDied && logic.plays_player(true))
            run(false, Click::Replay, false, draw, MEASURE_LIVE);
        if (live(true) || rendering || play->m_playerDied)
            modes(false, false);
    }
    restore();
    Logic::get().sync_death_label(play);
    Logic::get().check_advance_pause(play);

    busy = false;
    logic.predicting = false;
    Logic::get().sync_predict_hooks();
    cached = true;
    last_key = key;
    logic.traj_seconds = saved_seconds;
}

Trajectory::Click Trajectory::next_click(PlayLayer* play, bool player1) const {
    auto const& logic = Logic::get();
    PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
    if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart)
        return Logic::get().physical_hold(play, !player1) ? Click::Hold : Click::Release;
    int jump = static_cast<int>(PlayerButton::Jump);
    for (auto it = play->m_queuedButtons.rbegin(); it != play->m_queuedButtons.rend(); ++it) {
        int button = static_cast<int>(it->m_button);
        if ((button == 0 || button == jump) && it->m_isPlayer2 != player1)
            return it->m_isPush ? Click::Hold : Click::Release;
    }
    if (!body)
        return Click::Release;
    auto found = body->m_holdingButtons.find(jump);
    return found != body->m_holdingButtons.end() && found->second ? Click::Hold : Click::Release;
}

template <class F>
void Trajectory::predicting(PlayLayer* play, F&& predict) {
    auto& logic = Logic::get();
    if (play->m_objectLayer) {
        if (!fake1->getParent())
            play->m_objectLayer->addChild(fake1);
        if (!fake2->getParent())
            play->m_objectLayer->addChild(fake2);
    }
    park(fake1);
    park(fake2);
    busy = true;
    phys::prepare_prediction_art();
    logic.predicting = true;
    Logic::get().sync_predict_hooks();
    auto snap = [](PlayerObject* body) {
        return body ? std::pair{ body->m_jumpBuffered, body->m_holdingButtons } : std::pair{ false, gd::map<int, bool>{} };
    };
    auto p1_hold = snap(play->m_player1);
    auto p2_hold = snap(play->m_player2);
    predict();
    auto put = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
        if (!body)
            return;
        body->m_jumpBuffered = hold.first;
        body->m_holdingButtons = hold.second;
    };
    put(play->m_player1, p1_hold);
    put(play->m_player2, p2_hold);
    busy = false;
    logic.predicting = false;
    Logic::get().sync_predict_hooks();
}

int Trajectory::flip_survival(PlayLayer* play, bool player1, int& full, std::vector<CCPoint>* path, int limit) {
    full = 0;
    if (!play || !play->m_started || play->m_playerDied || !node || !fake1 || !fake2)
        return 0;
    auto& logic = Logic::get();
    constexpr int MEASURE_HOLD = 2, MEASURE_RELEASE = 4;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool linked = player1 && play->m_gameState.m_isDualMode && play->m_player2 && !twoPlayer;
    Click flip = next_click(play, player1) == Click::Hold ? Click::Release : Click::Hold;
    float kept[4] = { logic.traj_hold_alive_p1, logic.traj_hold_alive_p2, logic.traj_release_alive_p1, logic.traj_release_alive_p2 };
    measured[0] = measured[1] = measured_full = 0;
    path_sink = path;
    predicting(play, [&] {
        simulate(play, player1, flip, linked, false, limit, flip == Click::Hold ? MEASURE_HOLD : MEASURE_RELEASE);
    });
    path_sink = nullptr;
    logic.traj_hold_alive_p1 = kept[0];
    logic.traj_hold_alive_p2 = kept[1];
    logic.traj_release_alive_p1 = kept[2];
    logic.traj_release_alive_p2 = kept[3];
    cached = false;
    full = measured_full;
    return std::min(measured[0], measured[1]);
}

void Trajectory::autoclick(PlayLayer* play) {
    // TODO
}

void Trajectory::lookahead(PlayLayer* play) {
    auto& logic = Logic::get();
    logic.death_next_p1 = false;
    logic.death_next_p2 = false;
    if (!play || !play->m_started || play->m_playerDied || !play->m_player1 || !node || !fake1 || !fake2)
        return;
    if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted)
        return;
    auto actual = [&](bool player1) {
        return next_click(play, player1);
    };
    auto watched = [&](bool player2) {
        return logic.is_recording() ? logic.records_player(player2) : !logic.plays_player(player2);
    };
    constexpr int MEASURE_LIVE = 1;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    bool linked = dual && !twoPlayer;
    predicting(play, [&] {
        if (watched(false) && !logic.plays_player(false))
            simulate(play, true, actual(true), linked, false, 1, MEASURE_LIVE);
        if (dual && !linked && watched(true) && !logic.plays_player(true))
            simulate(play, false, actual(false), false, false, 1, MEASURE_LIVE);
    });
    logic.traj_alive_p1 = logic.traj_alive_p2 = 0.f;
    logic.traj_dies_p1 = logic.traj_dies_p2 = false;
    cached = false;
}

bool Logic::is_traj_player(CCObject* object) {
    auto const& trajectory = Trajectory::get();
    if (!object)
        return false;
    return (trajectory.fake1 && object == static_cast<CCObject*>(trajectory.fake1)) ||
           (trajectory.fake2 && object == static_cast<CCObject*>(trajectory.fake2));
}

void Logic::reset_traj() {
    Trajectory::get().invalidate();
}
