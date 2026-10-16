// shows where the player will go
//
// 2 fake players get the real players state copied on & run physics steps, once as if the button is held & once released
// each path is drawn as lines w/ a marker where it dies
// predicting must NEVER touch the real attempt: triggers dont fire for real, deaths stay on the fake, the random state is put back after
// the same thing answers "does the next step kill me" (pause before kill, the autoclicker) & "how long do i live if i flip now" (find best frame)
#include "Logic/perf.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/level_forecast.hpp"
#include "Hack/trajectory_path.hpp"
#include "Hack/orbs.hpp"
#include "Logic/game_state_copy.hpp"
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
#include <memory>
#include <utility>
#include <vector>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;


void Trajectory::park_fake(PlayerObject* player) {
    if (!player) return;
    player->setVisible(false);
    player->m_isHidden = true;
    player->stopAllActions();
    player->unscheduleUpdate();
    // if i leave the fake here the real player hits it next step xd
    player->setPosition({ -100000.f, -100000.f });
    player->m_position = CCPoint{ -100000.f, -100000.f };
    if (auto* play = PlayLayer::get()) play->removeObjectFromSection(player);
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
    if (player->m_iconSprite) player->m_iconSprite->setVisible(false);
    if (player->m_iconSpriteSecondary) player->m_iconSpriteSecondary->setVisible(false);
    if (player->m_iconSpriteWhitener) player->m_iconSpriteWhitener->setVisible(false);
    player->removeFromParentAndCleanup(false);
    park_fake(player);
    return player;
}

void Trajectory::attach(PlayLayer* play) {
    detach();
    if (!play || !play->m_objectLayer) return;
    auto make_node = [&](char const* id) {
        auto* made = CCDrawNode::create();
        made->retain();
        made->m_bUseArea = false;
        made->setID(id);
        made->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
        Logic::get().attach_overlay(made, play);
        return made;
    };
    draw_node = make_node("trajectory"_spr);
    back_draw_node = make_node("trajectory-next"_spr);
    back_draw_node->setVisible(false);
    fake1 = spawn_fake(play, "trajectory-player-1"_spr);
    fake2 = spawn_fake(play, "trajectory-player-2"_spr);
}

void Trajectory::sync_seat(PlayLayer* play) {
    if (!draw_node || !play) return;
    Logic::get().attach_overlay(draw_node, play);
    if (back_draw_node) Logic::get().attach_overlay(back_draw_node, play);
}

void Trajectory::invalidate() {
    drawing_valid = false;
    cancel_job();
}

void Trajectory::detach() {
    cancel_job();
    auto drop = [](auto*& object) {
        if (!object) return;
        object->stopAllActions();
        object->unscheduleAllSelectors();
        object->removeFromParentAndCleanup(true);
        object->release();
        object = nullptr;
    };
    drop(fake1);
    drop(fake2);
    drop(draw_node);
    drop(back_draw_node);
    activated_by_fake1.clear();
    activated_by_fake2.clear();
    simulating = false;
    drawing_valid = false;
    clicked_frame = -1;
    real_step_delta = 0.f;
    Logic::get().predicting = false;
    Logic::get().sync_predict_hooks();
}

bool Trajectory::mark_dead(PlayerObject* player) {
    if (!player) return false;
    if (player == fake1) dead_p1 = true;
    else if (player == fake2) dead_p2 = true;
    else return false;
    return true;
}

bool Trajectory::mark_predict_death(PlayerObject* player) {
    if (mark_dead(player)) return true;
    if (colliding_fake && mark_dead(colliding_fake)) return true;
    auto* play = PlayLayer::get();
    if (!play || !player) return false;
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
    if (!object || object->m_isMultiActivate) return false;
    if (player == fake1) return activated_by_fake1.contains(object);
    if (player == fake2) return activated_by_fake2.contains(object);
    return false;
}

bool Trajectory::realActivated(PlayerObject* player, EnhancedGameObject* object) const {
    if (!object) return false;
    auto* layer = GJBaseGameLayer::get();
    PlayerObject* real = player;
    if (layer && player == fake1) real = layer->m_player1;
    else if (layer && player == fake2) real = layer->m_player2;
    return phys::hasBeenActivatedByPlayer(real, object);
}

void Trajectory::rememberActivation(EnhancedGameObject* object, PlayerObject* player) {
    if (player == fake1) activated_by_fake1.insert(object);
    else if (player == fake2) activated_by_fake2.insert(object);
}

ccColor4F Trajectory::path_color(PlayerObject* fake, ClickType click) const {
    auto const& logic = Logic::get();
    float const* rgba = logic.traj_color;
    switch (click) {
    case ClickType::Swift:
        rgba = logic.traj_swift_color;
        break;
    case ClickType::Release:
        rgba = logic.traj_release_color;
        break;
    case ClickType::Hold:
    case ClickType::Replay:
        break;
    }
    if (fake == fake2)
        return { 1.f - rgba[0], 1.f - rgba[1], 1.f - rgba[2], rgba[3] };
    return { rgba[0], rgba[1], rgba[2], rgba[3] };
}

float Trajectory::line_width(PlayLayer* play) const {
    return Logic::get().traj_width / std::max(play->m_gameState.m_cameraZoom, 0.05f);
}

Trajectory::CacheKey Trajectory::make_cache_key(PlayLayer* play) const {
    auto const& logic = Logic::get();
    CacheKey key;
    key.frame = Logic::get().get_frame(play);
    auto fill_player_state = [](PlayerObject* player, float* out) {
        if (!player) return;
        out[0] = player->m_position.x;
        out[1] = player->m_position.y;
        out[2] = player->getRotation();
        out[3] = (float)player->m_yVelocity;
        out[4] = (float)player->m_platformerXVelocity;
        out[5] = (float)((player->m_holdingLeft ? 1 : 0) | (player->m_holdingRight ? 2 : 0) | (player->m_leftPressedFirst ? 4 : 0));
    };
    fill_player_state(play->m_player1, key.player1_state);
    fill_player_state(play->m_player2, key.player2_state);
    key.dual = play->m_gameState.m_isDualMode;
    auto jump_held = [](PlayerObject* body) {
        if (!body) return false;
        int jump = (int)PlayerButton::Jump;
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
    key.black_orbs = key.dash_orbs && logic.black_orb_assist();
    key.pad_black = logic.pad_black_orb;
    key.pad_blue = logic.pad_blue_orb;
    key.pause_kill = logic.pause_before_kill && logic.is_recording() && logic.show_traj;
    key.pause_length = logic.pause_at_traj_length;
    key.pause_ms = logic.pause_traj_ms;
    key.hide_p1 = Logic::get().traj_player_hidden(false);
    key.hide_p2 = Logic::get().traj_player_hidden(true);
    key.orbs = Orbs::orb_spots(play, play->m_player1);
    if (play->m_player2) {
        auto spots = Orbs::orb_spots(play, play->m_player2);
        key.orbs.insert(key.orbs.end(), spots.begin(), spots.end());
    }
    key.seconds = logic.traj_seconds;
    key.width = logic.traj_width;
    key.zoom = play->m_gameState.m_cameraZoom;
    std::copy(std::begin(logic.traj_color), std::end(logic.traj_color), key.color);
    std::copy(std::begin(logic.traj_swift_color), std::end(logic.traj_swift_color), key.swift_color);
    std::copy(std::begin(logic.traj_release_color), std::end(logic.traj_release_color), key.release_color);
    return key;
}

// w silly cat skid
void Trajectory::copy_real_into_fake(PlayerObject* fake, PlayerObject* real) {
    // mod 0 crashes on windows
    auto own_streak_type = fake->m_shipStreakType;
    fake->copyAttributes(real);
    SavedPlayerCheckpoint checkpoint = SavedPlayerCheckpoint::create(real);
    checkpoint.apply(fake);
    fake->m_shipStreakType = own_streak_type;
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
    if (state.m_isDualMode == on || !fake1 || !fake2) return;
    bool twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
    auto with_fakes = [&](auto&& action) {
        PlayerObject* real1 = layer->m_player1;
        PlayerObject* real2 = layer->m_player2;
        layer->m_player1 = fake1;
        layer->m_player2 = fake2;
        action();
        layer->m_player1 = real1;
        layer->m_player2 = real2;
    };
    if (on) {
        if (player != fake1 || running_pair) return;
        state.m_isDualMode = true;
        fake1->m_unkA99 = true;
        if (dual_partner_real) copy_real_into_fake(fake2, dual_partner_real);
        fake2->m_unkA99 = true;
        fake2->resetObject();
        fake2->copyAttributes(fake1);
        bool unlinked = state.m_unlinkDualGravity;
        fake2->flipGravity(unlinked ? fake1->m_isUpsideDown : !fake1->m_isUpsideDown, true);
        fake2->setYVelocity(unlinked ? fake1->m_yVelocity : -fake1->m_yVelocity, 0);
        fake2->m_isOnGround = false;
        fake2->m_isOnGround2 = false;
        if (twoPlayer) {
            if (!fake2->m_inputsLocked) fake2->m_holdingButtons[(int)PlayerButton::Jump] = false;
            if (!fake2->m_controlsDisabled) {
                fake2->m_jumpBuffered = false;
                fake2->m_stateRingJump = false;
                fake2->m_touchedPad = true;
                if (fake2->m_isDashing) fake2->stopDashing();
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
        dual_started = can_start_dual;
        return;
    }
    state.m_isDualMode = false;
    fake1->m_unkA99 = false;
    fake2->m_unkA99 = false;
    if (player == fake2) fake1->copyAttributes(fake2);
    fake2->releaseAllButtons();
    state.m_lastActivatedPortal2 = nullptr;
    int mode = 6;
    if (fake1->m_isShip || fake1->m_isBird || fake1->m_isDart || fake1->m_isSwing) mode = 5;
    else if (fake1->m_isBall) mode = 16;
    else if (fake1->m_isSpider) mode = 33;
    with_fakes([&] {
        layer->updateDualGround(player, mode, false, 0.f);
    });
    dual_ended = true;
}

void Trajectory::press_fake(PlayLayer* play, PlayerObject* fake, ClickType click) {
    auto& logic = Logic::get();
    bool stash_orbs = Orbs::click_orb_assist(fake == fake2);
    if (stash_orbs) logic.suppress_rings = true;
    switch (click) {
    case ClickType::Replay:
        break;
    case ClickType::Hold:
        fake->pushButton(PlayerButton::Jump);
        break;
    case ClickType::Swift:
        fake->releaseButton(PlayerButton::Jump);
        fake->m_jumpBuffered = false;
        fake->pushButton(PlayerButton::Jump);
        fake->releaseButton(PlayerButton::Jump);
        break;
    case ClickType::Release:
        fake->releaseButton(PlayerButton::Jump);
        fake->m_jumpBuffered = false;
        break;
    }
    if (stash_orbs) logic.suppress_rings = false;
}

bool Trajectory::step_fake(PlayLayer* play, PlayerObject* fake, float step_delta, bool draw, ClickType click) {
    CCPoint from = fake->getPosition();

    // w nat
    fake->m_totalTime += Logic::get().physics_step();
    auto clear_log = [&](CCDictionary* log) {
        if (!log) return;
        auto is_real_log = [&](PlayerObject* body) {
            if (!body) return false;
            return log == body->m_collisionLogTop || log == body->m_collisionLogBottom || log == body->m_collisionLogLeft || log == body->m_collisionLogRight;
        };
        if (is_real_log(play->m_player1) || is_real_log(play->m_player2)) return;
        log->removeAllObjects();
    };
    clear_log(fake->m_collisionLogTop);
    clear_log(fake->m_collisionLogBottom);
    clear_log(fake->m_collisionLogLeft);
    clear_log(fake->m_collisionLogRight);
    fake->m_playEffects = false;
    bool live_was_dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    // a fire sprite w a type that has no frames must never reach update()
    if (int streak_type = (int)(fake->m_shipStreakType); fake->m_shipStreak && (streak_type < 2 || streak_type > 6))
        fake->m_shipStreakType = (ShipStreak)2;
    fake->update(step_delta);
    fake->m_unkUnused3 = fake->getRotation();
    fake->updateRotation(step_delta);
    fake->m_shipRotation = fake->getPosition();
    PlayerObject*& player_slot = (fake == fake2) ? play->m_player2 : play->m_player1;
    PlayerObject* saved_slot = player_slot;
    if (player_slot) player_slot = fake;
    colliding_fake = fake;
    int hit = play->checkCollisions(fake, step_delta, false);
    colliding_fake = nullptr;
    if (player_slot) player_slot = saved_slot;
    (void)hit;
    if (fake->m_isDead) mark_dead(fake);
    bool live_now_dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    if (!live_was_dead && live_now_dead) mark_dead(fake);
    phys::checkSpawnObjects(play, fake);

    bool dead = (fake == fake1 ? dead_p1 : dead_p2) || fake->m_isDead;
    if (dead) {
        if (fake == fake1) dead_p1 = true;
        else if (fake == fake2) dead_p2 = true;
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
        auto& path = unflushed_paths[fake == fake2];
        if (path.empty()) path.push_back(from);
        path.push_back(fake->getPosition());
    }
    return false;
}

void Trajectory::flush_path(PlayLayer* play, PlayerObject* fake, ClickType click) {
    auto& path = unflushed_paths[fake == fake2];
    float width = line_width(play);
    auto color = path_color(fake, click);
    echo::trajectory::draw_path(path, [&](CCPoint a, CCPoint b) {
        draw_node->drawSegment(a, b, width, color);
    });
    path.clear();
}

void Trajectory::draw_death(PlayerObject* fake) {
    auto* play = PlayLayer::get();
    if (!play) return;
    auto const& logic = Logic::get();
    float width = line_width(play);
    auto to_color = [](float const* c) {
        return ccColor4F{ c[0], c[1], c[2], c[3] };
    };
    draw_node->drawRect(fake->getObjectRect(), { 0.f, 0.f, 0.f, 0.f }, width, to_color(logic.hitbox_outer_color), BorderAlignment::Inside);
    draw_node->drawRect(fake->getObjectRect(0.3f, 0.3f), { 0.f, 0.f, 0.f, 0.f }, width, to_color(logic.hitbox_inner_color), BorderAlignment::Inside);
}

void Trajectory::simulate(PlayLayer* play, bool player1, ClickType click, bool both, bool draw, int limit, int measure) {
    auto& logic = Logic::get();
    const int MEASURE_LIVE = 1, MEASURE_HOLD = 2, MEASURE_RELEASE = 4;
    bool measuring = measure != 0;
    PlayerObject* fake = player1 ? fake1 : fake2;
    PlayerObject* real = player1 ? play->m_player1 : play->m_player2;
    PlayerObject* partner = otherFake(fake);
    PlayerObject* partner_real = player1 ? play->m_player2 : play->m_player1;
    if (!fake || !real) return;
    if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) return;
    auto random_state = GameToolbox::getfast_srand();
    auto teleport_roll = logic.teleport_random;
    auto shake_roll = logic.shake_random;
    bool with_partner = both && play->m_gameState.m_isDualMode && partner && partner_real;
    auto* manager = GameManager::get();
    bool saved_performance_mode = manager && manager->m_performanceMode;
    if (manager) manager->m_performanceMode = true;

    auto player1_checkpoint = SavedPlayerCheckpoint::create(play->m_player1);
    auto player2_checkpoint = play->m_player2 ? SavedPlayerCheckpoint::create(play->m_player2) : SavedPlayerCheckpoint();
    bool has_player2 = play->m_player2 != nullptr;
    bool died = play->m_playerDied;
    auto queued = play->m_queuedButtons;
    auto recorded = play->m_queuedRecordedButtons;
    int recorded_size = play->m_queuedRecordedButtonsSize;
    play->m_playerDied = false;
    auto logs_of = [](PlayerObject* body) {
        return body ? std::array{ body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight } : std::array<CCDictionary*, 4>{};
    };

    struct PlayerTrails {
        HardStreak* wave = nullptr;
        CCMotionStreak* regular = nullptr;
        GhostTrailEffect* ghost = nullptr;
    };

    auto trails_of = [](PlayerObject* body) {
        if (!body)
            return PlayerTrails{};
        return PlayerTrails{ body->m_waveTrail, body->m_regularTrail, body->m_ghostTrail };
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
        auto restore_body = [](PlayerObject* body, SavedPlayerCheckpoint& checkpoint, std::array<CCDictionary*, 4> const& logs, PlayerTrails const& trails) {
            if (!body) return;
            checkpoint.apply(body);
            body->m_collisionLogTop = logs[0];
            body->m_collisionLogBottom = logs[1];
            body->m_collisionLogLeft = logs[2];
            body->m_collisionLogRight = logs[3];
            body->m_waveTrail = trails.wave;
            body->m_regularTrail = trails.regular;
            body->m_ghostTrail = trails.ghost;
        };
        restore_body(play->m_player1, player1_checkpoint, logs1, trails1);
        if (has_player2) restore_body(play->m_player2, player2_checkpoint, logs2, trails2);
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

    SavedGameState saved_state(play->m_gameState);
    Orbs::reset_prediction_pads();
    copy_real_into_fake(fake, real);
    if (with_partner) copy_real_into_fake(partner, partner_real);
    fake->m_controlsDisabled = false;
    if (with_partner && partner) partner->m_controlsDisabled = false;
    dead_p1 = false;
    dead_p2 = false;

    std::array<PlayerObject*, 2> bodies = { fake, with_partner ? partner : nullptr };
    auto each_body = [&](auto&& action) {
        for (auto* body : bodies) {
            if (body) action(body);
        }
    };

    bool macro_plays = logic.plays_player(!player1);
    bool uses_autoclicker = !macro_plays && logic.autoclicker && logic.records_player(!player1) && (player1 ? logic.autoclick_p1 : logic.autoclick_p2);
    auto jump_held = [](PlayerObject* body) {
        if (!body) return false;
        int jump = (int)PlayerButton::Jump;
        auto it = body->m_holdingButtons.find(jump);
        return it != body->m_holdingButtons.end() && it->second;
    };
    bool physical_key_held = Logic::get().physical_hold(play, !player1);
    bool started_held = jump_held(real) || (logic.maintain_gravity && physical_key_held && real->m_isDart);
    AutoClick autoclick_state = logic.auto_click;
    if (uses_autoclicker) {
        bool stash_orbs = Orbs::click_orb_assist(!player1) || (with_partner && Orbs::click_orb_assist(player1));
        if (stash_orbs) logic.suppress_rings = true;
        each_body([&](PlayerObject* body) {
            if (autoclick_state.pressed) body->pushButton(PlayerButton::Jump);
            else body->releaseButton(PlayerButton::Jump);
        });
        if (stash_orbs) logic.suppress_rings = false;
    }
    else {
        each_body([&](PlayerObject* body) {
            body->m_controlsDisabled = false;
            body->m_isLocked = false;
            bool already_held = jump_held(body);
            if (!already_held && logic.maintain_gravity && physical_key_held && body->m_isDart && click == ClickType::Hold) {
                already_held = true;
            }
            if (click == ClickType::Hold && already_held) return;
            if (click != ClickType::Release && click != ClickType::Replay) body->m_jumpBuffered = false;
            press_fake(play, body, click);
        });
    }

    float warp = play->m_gameState.m_timeWarp;
    float step_delta = real_step_delta > 0.f ? real_step_delta : logic.physics_step() * 60.f * std::min(warp, 1.f);
    // a macro can change tps part way so each step gets its own length (NOT the last real frames)
    bool varying_tps = logic.is_playing() && !logic.rates.empty();
    float step_seconds = logic.physics_step();
    if (varying_tps) {
        step_seconds = 1.f / std::max(logic.rate_at(logic.get_frame(play)), 1.f);
        step_delta = step_seconds * 60.f * std::min(warp, 1.f);
    }
    int steps = echo::trajectory::prediction_steps(logic.traj_seconds, logic.sample_rate(), warp);
    if (limit > 0) steps = std::min(steps, limit);
    perf_count_traj_steps(steps);
    bool moving_objects = logic.traj_moving_objects && !death_label_only;
    if (moving_objects) forecast::prepare(play, steps, step_delta / 60.f);
    else forecast::invalidate();
    if (draw) {
        unflushed_paths[fake == fake2].reserve(steps + 1);
        if (partner) unflushed_paths[partner == fake2].reserve(steps + 1);
    }
    int frame = logic.get_frame(play);
    size_t input_cursor = logic.replay_pos;
    const int HOLD_BUTTONS[] = { (int)PlayerButton::Jump, (int)PlayerButton::Left, (int)PlayerButton::Right };
    std::array<std::array<bool, 3>, 2> macro_held{};
    std::array<int, 2> last_input_frame = { -1, -1 };
    if (logic.is_playing()) {
        for (int side = 0; side < 2; ++side) {
            if (!logic.plays_player(side == 1)) continue;
            for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b)
                macro_held[side][b] = Logic::get().held_at(side == 1, frame, HOLD_BUTTONS[b]);
        }
        for (size_t i = logic.inputs.size(); i-- > 0 && (last_input_frame[0] < 0 || last_input_frame[1] < 0);) {
            int& last = last_input_frame[logic.inputs[i].player2 ? 1 : 0];
            if (last < 0) last = logic.inputs[i].number;
        }
    }
    bool fake_done = false, partner_done = !with_partner;
    int fake_alive_steps = 0, partner_alive_steps = 0;
    auto orb_poses = Orbs::capture(play, real);
    if (with_partner) {
        auto partner_orb_poses = Orbs::capture(play, partner_real);
        orb_poses.insert(orb_poses.end(), partner_orb_poses.begin(), partner_orb_poses.end());
    }
    auto restore_orbs = [&] {
        Orbs::restore(orb_poses);
    };
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool fake_left_dual = false, partner_left_dual = false;
    running_pair = with_partner;
    dual_partner_real = partner_real;
    can_start_dual = player1 && !with_partner && partner == fake2 && partner_real && !Logic::get().traj_player_hidden(true);
    dual_started = false;
    dual_ended = false;
    auto apply_dual_portal_changes = [&] {
        if (std::exchange(dual_started, false)) {
            with_partner = true;
            running_pair = true;
            partner_done = false;
            partner->m_controlsDisabled = false;
            partner->m_isLocked = false;
            if (!twoPlayer) bodies[1] = partner;
            if (draw) unflushed_paths[partner == fake2].reserve(steps + 1);
        }
        if (std::exchange(dual_ended, false)) {
            running_pair = false;
            if (fake == fake2) {
                fake_left_dual = !fake_done;
                fake_done = true;
            }
            else if (with_partner) {
                partner_left_dual = !partner_done;
                partner_done = true;
                bodies[1] = nullptr;
            }
        }
    };
    std::array<std::unordered_set<int>, 2> orb_clicks;

    struct ObjectUseFlags {
        EnhancedGameObject* object;
        bool p1, p2;
    };

    std::vector<ObjectUseFlags> orb_use_flags;
    for (auto const& pose : orb_poses) {
        auto* ring = static_cast<EnhancedGameObject*>(pose.object);
        if (ring)
            orb_use_flags.push_back({ ring, ring->m_activatedByPlayer1, ring->m_activatedByPlayer2 });
    }
    auto copy_touching_rings = [](PlayerObject* player) {
        std::vector<CCObject*> rings;
        if (!player || !player->m_touchingRings) return rings;
        unsigned int count = player->m_touchingRings->count();
        rings.reserve(count);
        for (unsigned int i = 0; i < count; ++i)
            rings.push_back(player->m_touchingRings->objectAtIndex(i));
        return rings;
    };
    auto restore_touching_rings = [](PlayerObject* player, std::vector<CCObject*> const& rings) {
        if (!player || !player->m_touchingRings) return;
        player->m_touchingRings->removeAllObjects();
        for (auto* ring : rings) {
            if (ring) player->m_touchingRings->addObject(ring);
        }
    };
    auto rings1 = copy_touching_rings(play->m_player1);
    auto rings2 = copy_touching_rings(play->m_player2);
    auto restore_live_orbs = [&] {
        restore_orbs();
        for (auto const& flag : orb_use_flags) {
            if (!flag.object) continue;
            flag.object->m_activatedByPlayer1 = flag.p1;
            flag.object->m_activatedByPlayer2 = flag.p2;
        }
        restore_touching_rings(play->m_player1, rings1);
        restore_touching_rings(play->m_player2, rings2);
    };

    std::unique_ptr<SavedGameState> sim_state;
    decltype(random_state) sim_random{};
    decltype(teleport_roll) sim_teleport{};
    decltype(shake_roll) sim_shake{};
    std::vector<ObjectUseFlags> sim_orb_use_flags;
    std::vector<CCObject*> sim_rings1, sim_rings2;
    SavedPlayerCheckpoint sim_first, sim_second;
    auto sim_queued = queued;
    auto sim_recorded = recorded;
    int sim_recorded_size = recorded_size;
    CCPoint sim_layer_pos = layer_pos;
    float sim_scale_x = layer_scale_x, sim_scale_y = layer_scale_y, sim_rotation = layer_rotation;
    SliceStateHooks world_hooks{
        [&] {
            sim_state = std::make_unique<SavedGameState>(play->m_gameState);
            sim_random = GameToolbox::getfast_srand();
            sim_teleport = logic.teleport_random;
            sim_shake = logic.shake_random;
            sim_orb_use_flags.clear();
            for (auto const& flag : orb_use_flags) {
                if (flag.object)
                    sim_orb_use_flags.push_back({ flag.object, flag.object->m_activatedByPlayer1, flag.object->m_activatedByPlayer2 });
            }
            sim_rings1 = copy_touching_rings(play->m_player1);
            sim_rings2 = copy_touching_rings(play->m_player2);
            sim_first = SavedPlayerCheckpoint::create(play->m_player1);
            if (has_player2) sim_second = SavedPlayerCheckpoint::create(play->m_player2);
            sim_queued = play->m_queuedButtons;
            sim_recorded = play->m_queuedRecordedButtons;
            sim_recorded_size = play->m_queuedRecordedButtonsSize;
            if (layer) {
                sim_layer_pos = layer->getPosition();
                sim_scale_x = layer->getScaleX();
                sim_scale_y = layer->getScaleY();
                sim_rotation = layer->getRotation();
            }
        },
        [&] {
            forecast::suspend(play);
            restore_live_orbs();
            saved_state.restore(play->m_gameState);
            restore_players();
            GameToolbox::fast_srand(random_state);
            logic.teleport_random = teleport_roll;
            logic.shake_random = shake_roll;
            if (manager) manager->m_performanceMode = saved_performance_mode;
        },
        [&] {
            queued = play->m_queuedButtons;
            recorded = play->m_queuedRecordedButtons;
            recorded_size = play->m_queuedRecordedButtonsSize;
            random_state = GameToolbox::getfast_srand();
            teleport_roll = logic.teleport_random;
            shake_roll = logic.shake_random;
            if (manager) saved_performance_mode = manager->m_performanceMode;
            if (manager) manager->m_performanceMode = true;
            if (sim_state) {
                sim_state->restore(play->m_gameState);
                sim_state.reset();
            }
            GameToolbox::fast_srand(sim_random);
            logic.teleport_random = sim_teleport;
            logic.shake_random = sim_shake;
            for (auto const& flag : sim_orb_use_flags) {
                flag.object->m_activatedByPlayer1 = flag.p1;
                flag.object->m_activatedByPlayer2 = flag.p2;
            }
            restore_touching_rings(play->m_player1, sim_rings1);
            restore_touching_rings(play->m_player2, sim_rings2);
            auto restore_sim_body = [](PlayerObject* body, SavedPlayerCheckpoint& checkpoint, std::array<CCDictionary*, 4> const& logs, PlayerTrails const& trails) {
                if (!body) return;
                checkpoint.apply(body);
                body->m_collisionLogTop = logs[0];
                body->m_collisionLogBottom = logs[1];
                body->m_collisionLogLeft = logs[2];
                body->m_collisionLogRight = logs[3];
                body->m_waveTrail = trails.wave;
                body->m_regularTrail = trails.regular;
                body->m_ghostTrail = trails.ghost;
            };
            restore_sim_body(play->m_player1, sim_first, logs1, trails1);
            if (has_player2) restore_sim_body(play->m_player2, sim_second, logs2, trails2);
            play->m_playerDied = false;
            play->m_queuedButtons = sim_queued;
            play->m_queuedRecordedButtons = sim_recorded;
            play->m_queuedRecordedButtonsSize = sim_recorded_size;
            if (layer) {
                layer->setPosition(sim_layer_pos);
                layer->setScaleX(sim_scale_x);
                layer->setScaleY(sim_scale_y);
                layer->setRotation(sim_rotation);
            }
        }
    };
    slice_state_hooks.push_back(&world_hooks);
    struct PopHook {
        std::vector<SliceStateHooks*>& hooks;
        ~PopHook() {
            hooks.pop_back();
        }
    } pop_world{ slice_state_hooks };

    std::vector<CCPoint> player_path;
    bool record_player_path = (measure & MEASURE_LIVE) && fake == fake1 && steps >= 30 && !died;
    if (record_player_path) {
        player_path.reserve((size_t)steps + 1);
        player_path.push_back(fake->getPosition());
    }
    for (int i = 0; i < steps && !(fake_done && partner_done); ++i) {
        yield_if_slice_over();
        if (job_cancel_requested) break;

        if (varying_tps) {
            step_seconds = 1.f / std::max(logic.rate_at(frame + i), 1.f);
            step_delta = step_seconds * 60.f * std::min(warp, 1.f);
        }
        {
            const float FORECAST_REACH = 360.f;
            auto at = fake->getPosition();
            float left = at.x, right = at.x, bottom = at.y, top = at.y;
            if (with_partner && partner) {
                auto other = partner->getPosition();
                left = std::min(left, other.x);
                right = std::max(right, other.x);
                bottom = std::min(bottom, other.y);
                top = std::max(top, other.y);
            }
            if (moving_objects)
                forecast::apply(play, i + 1, { left - FORECAST_REACH, bottom - FORECAST_REACH, right - left + 2.f * FORECAST_REACH, top - bottom + 2.f * FORECAST_REACH }, fake == fake1 ? at.y : NAN);
        }
        std::array<bool, 2> orb_pressed{};
        if (macro_plays || (with_partner && logic.is_playing())) {
            int at = frame + i + 1;
            bool inputs_shared = with_partner && logic.records_player(false) == logic.records_player(true) && (!logic.is_playing() || logic.play_p1 == logic.play_p2);
            auto send_input = [&](PlayerObject* target, bool player2, PlayerButton button, bool holding) {
                auto press_or_release = [&](PlayerObject* body, bool icon2) {
                    if (!body) return;
                    if (holding) {
                        body->pushButton(button);
                        if (button == PlayerButton::Jump) orb_pressed[icon2 ? 1 : 0] = true;
                    }
                    else body->releaseButton(button);
                };
                press_or_release(target, player2);
                if (inputs_shared) press_or_release(otherFake(target), !player2);
            };
            std::array<std::array<bool, 3>, 2> sent_this_step{};
            bool two_player_lvl = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
            while (input_cursor < logic.inputs.size() && logic.inputs[input_cursor].number <= at) {
                auto const& input = logic.inputs[input_cursor++];
                int raw_button = input.button == 0 ? 1 : input.button;
                // outside 2 player levels gd gives p2 inputs to p1 (silicate macros can be all p2)
                bool to_p2 = input.player2 && two_player_lvl;
                send_input(to_p2 ? fake2 : fake1, to_p2, (PlayerButton)raw_button, input.holding);
                int side = to_p2 ? 1 : 0;
                for (int b = 0; b < 3; ++b) {
                    if (HOLD_BUTTONS[b] != raw_button) continue;
                    macro_held[side][b] = input.holding;
                    sent_this_step[side][b] = true;
                }
            }
            for (int side = 0; side < (two_player_lvl ? 2 : 1); ++side) {
                if (!logic.plays_player(side == 1)) continue;
                if (!logic.ignore_inputs && at > last_input_frame[side]) continue;
                PlayerObject* body = side == 1 ? fake2 : fake1;
                if (body != fake && !(with_partner && body == partner)) continue;
                for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b) {
                    if (sent_this_step[side][b]) continue;
                    if (b > 0 && body->m_controlsDisabled) continue;
                    if (Logic::button_held(body, HOLD_BUTTONS[b]) == macro_held[side][b]) continue;
                    send_input(body, side == 1, (PlayerButton)HOLD_BUTTONS[b], macro_held[side][b]);
                }
            }
        }

        bool pressed_now = (i == 0 && (click == ClickType::Swift || (click == ClickType::Hold && !started_held)));
        if (uses_autoclicker) {
            if (autoclick_state.pressed) ++autoclick_state.since_press;
            else ++autoclick_state.since_release;
            bool flip = autoclick_state.pressed ? autoclick_state.since_press >= autoclick_state.press_interval : autoclick_state.since_release >= autoclick_state.release_interval;
            if (flip) {
                autoclick_state.pressed = !autoclick_state.pressed;
                autoclick_state.since_press = 0;
                autoclick_state.since_release = 0;
                pressed_now = autoclick_state.pressed;
                bool stash_orbs = Orbs::click_orb_assist(!player1) || (with_partner && Orbs::click_orb_assist(player1));
                if (stash_orbs) logic.suppress_rings = true;
                each_body([&](PlayerObject* body) {
                    if (autoclick_state.pressed) body->pushButton(PlayerButton::Jump);
                    else body->releaseButton(PlayerButton::Jump);
                });
                if (stash_orbs) logic.suppress_rings = false;
                if (autoclick_state.pressed && autoclick_state.press_interval <= 0) {
                    autoclick_state.pressed = false;
                    pressed_now = false;
                    each_body([&](PlayerObject* body) {
                        body->releaseButton(PlayerButton::Jump);
                    });
                }
            }
        }

        auto keep_gravity_hold = [&](PlayerObject* body) {
            bool icon2 = body == fake2;
            if (!body->m_isDart || logic.plays_player(icon2) || !logic.maintain_gravity || !logic.records_player(icon2))
                return;
            bool physically_held = uses_autoclicker ? autoclick_state.pressed : click == ClickType::Hold;
            bool stash_orbs = Orbs::click_orb_assist(icon2);
            if (stash_orbs) logic.suppress_rings = true;
            for (int step = 0; step < 8 && !body->m_controlsDisabled; ++step) {
                if (body->m_isUpsideDown != (physically_held == body->m_jumpBuffered)) break;
                bool before_flip = body->m_isUpsideDown, before_buffer = body->m_jumpBuffered;
                if (physically_held != body->m_isUpsideDown) {
                    body->pushButton(PlayerButton::Jump);
                }
                else body->releaseButton(PlayerButton::Jump);
                if (body->m_isUpsideDown == before_flip && body->m_jumpBuffered == before_buffer)
                    break;
            }
            if (stash_orbs) logic.suppress_rings = false;
        };
        each_body(keep_gravity_hold);
        each_body([&](PlayerObject* body) {
            bool icon2 = body == fake2;
            bool fresh_press = orb_pressed[icon2 ? 1 : 0] || (!logic.plays_player(icon2) && pressed_now);
            Orbs::click_fake(play, body, icon2, orb_clicks[icon2 ? 1 : 0], fresh_press);
            Orbs::click_fake_dash(body, icon2);
            Orbs::apply_prediction_pads(play, body, icon2);
        });
        each_body(keep_gravity_hold);

        // w skidicate
        auto& state = play->m_gameState;
        float dt = varying_tps ? step_seconds : logic.physics_step();
        state.m_totalTime += dt;
        state.m_unkDouble3 += dt / state.m_timeWarp;
        state.m_currentProgress++;
        state.m_unkUint5 += (int)roundf(state.m_timeWarp * 1000.0f);
        float time_mod = *reinterpret_cast<float*>(&state.m_timeModRelated);
        if (time_mod != 0.0f) {
            state.m_timeModRelated = 0;
            state.m_timeModRelated2 = 0;
            each_body([&](PlayerObject* body) {
                body->updateTimeMod(time_mod, true);
            });
        }

        if (!fake_done) fake->resetTouchedRings(false);
        if (!partner_done && partner) partner->resetTouchedRings(false);
        bool fake_was_done = fake_done, partner_was_done = partner_done;
        if (!fake_done) fake_done = step_fake(play, fake, step_delta, draw, click);
        if (record_player_path && !fake_done) player_path.push_back(fake->getPosition());
        if (survival_path_out && !fake_done) survival_path_out->push_back(fake->getPosition());
        apply_dual_portal_changes();
        if (!partner_done) partner_done = step_fake(play, partner, step_delta, draw, click);
        apply_dual_portal_changes();
        restore_orbs();
        if ((measure & MEASURE_LIVE) && i == 0 && !died) {
            if (player1) {
                if (!fake_was_done && fake_done && !fake_left_dual) logic.death_next_p1 = true;
                if (with_partner && !partner_was_done && partner_done && !partner_left_dual) logic.death_next_p2 = true;
            }
            else if (!fake_was_done && fake_done && !fake_left_dual) {
                logic.death_next_p2 = true;
            }
        }
        if (measuring) {
            if (!fake_was_done && !fake_done) ++fake_alive_steps;
            if (with_partner && !partner_was_done && !partner_done) ++partner_alive_steps;
        }
    }

    if (measuring) {
        measured_alive_steps[0] = fake_done && !fake_left_dual ? fake_alive_steps : steps;
        measured_alive_steps[1] = with_partner && partner_done && !partner_left_dual ? partner_alive_steps : steps;
        measured_total_steps = steps;
        float steps_to_seconds = std::clamp(warp, 0.01f, 1.f) / (float)logic.sample_rate();
        auto store_alive_times = [&](float& alive1, bool& dies1, float& alive2, bool& dies2) {
            if (player1) {
                alive1 = (float)fake_alive_steps * steps_to_seconds;
                dies1 = fake_done && !fake_left_dual;
                if (with_partner) {
                    alive2 = (float)partner_alive_steps * steps_to_seconds;
                    dies2 = partner_done && !partner_left_dual;
                }
            }
            else {
                alive2 = (float)fake_alive_steps * steps_to_seconds;
                dies2 = fake_done && !fake_left_dual;
            }
        };
        if (measure & MEASURE_LIVE)
            store_alive_times(logic.traj_alive_p1, logic.traj_dies_p1, logic.traj_alive_p2, logic.traj_dies_p2);
        if (measure & MEASURE_HOLD) {
            if (player1) {
                logic.traj_hold_alive_p1 = (float)fake_alive_steps * steps_to_seconds;
                if (with_partner) logic.traj_hold_alive_p2 = (float)partner_alive_steps * steps_to_seconds;
            }
            else {
                logic.traj_hold_alive_p2 = (float)fake_alive_steps * steps_to_seconds;
            }
        }
        if (measure & MEASURE_RELEASE) {
            if (player1) {
                logic.traj_release_alive_p1 = (float)fake_alive_steps * steps_to_seconds;
                if (with_partner) logic.traj_release_alive_p2 = (float)partner_alive_steps * steps_to_seconds;
            }
            else {
                logic.traj_release_alive_p2 = (float)fake_alive_steps * steps_to_seconds;
            }
        }
    }

    if (record_player_path) forecast::note_player_path(std::move(player_path), real->m_lastPosition);
    forecast::rewind(play);
    running_pair = false;
    can_start_dual = false;
    dual_partner_real = nullptr;
    if (manager) manager->m_performanceMode = saved_performance_mode;
    if (draw) {
        flush_path(play, fake, click);
        if (partner) flush_path(play, partner, click);
    }
    restore_live_orbs();
    park_fake(fake);
    if (partner) park_fake(partner);
    activated_by_fake1.clear();
    activated_by_fake2.clear();
    saved_state.restore(play->m_gameState);
    restore_players();
    GameToolbox::fast_srand(random_state);
    logic.teleport_random = teleport_roll;
    logic.shake_random = shake_roll;
}

void Trajectory::refresh_now(PlayLayer* play) {
    if (!play || !play->m_started || !play->m_player1 || !draw_node || !fake1 || !fake2) return;
    auto& logic = Logic::get();
    if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
        if (draw_node->isVisible()) draw_node->setVisible(false);
        park_fake(fake1);
        park_fake(fake2);
        if (drawing_valid || logic.death_next_p1 || logic.death_next_p2) {
            drawing_valid = false;
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
        if (draw_node->isVisible()) draw_node->setVisible(false);
        bool pause_kill = logic.pause_before_kill && logic.is_recording();
        if (!rendering && ((logic.show_death_next && !(logic.is_playing() && !logic.is_recording())) || pause_kill)) {
            death_label_only = !pause_kill;
            lookahead(play);
            death_label_only = false;
            Logic::get().check_advance_pause(play);
            Logic::get().sync_death_label(play);
            return;
        }
        if (drawing_valid || logic.death_next_p1 || logic.death_next_p2 || logic.traj_dies_p1 || logic.traj_dies_p2) {
            drawing_valid = false;
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
    Logic::get().attach_overlay(draw_node, play);
    if (play->m_objectLayer) {
        if (!fake1->getParent()) play->m_objectLayer->addChild(fake1);
        if (!fake2->getParent()) play->m_objectLayer->addChild(fake2);
    }
    if (rendering && !logic.show_traj) {
        draw_node->setVisible(false);
        park_fake(fake1);
        park_fake(fake2);
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
    park_fake(fake1);
    park_fake(fake2);

    draw_node->setVisible(draw);

    float saved_seconds = logic.traj_seconds;
    int step_limit = 0;

    CacheKey key = make_cache_key(play);
    key.hold = draw && logic.traj_hold;
    key.swift = draw && logic.traj_swift;
    key.release = draw && logic.traj_release;
    if (drawing_valid && key == last_cache_key) {
        logic.traj_seconds = saved_seconds;
        Logic::get().check_advance_pause(play);
        return;
    }

    simulating = true;
    phys::prepare_prediction_art();
    logic.predicting = true;
    Logic::get().sync_predict_hooks();
    if (inside_job && !job_swapped_nodes && back_draw_node) {
        std::swap(draw_node, back_draw_node);
        job_swapped_nodes = true;
        draw_node->setVisible(false);
    }
    draw_node->clear();
    auto save_hold = [](PlayerObject* body) {
        return body ? std::pair{ body->m_jumpBuffered, body->m_holdingButtons } : std::pair{ false, gd::map<int, bool>{} };
    };
    auto copy_hold_into = [](std::pair<bool, gd::map<int, bool>>& into, PlayerObject* body) {
        into.first = body ? body->m_jumpBuffered : false;
        if (body) into.second = body->m_holdingButtons;
        else into.second.clear();
    };
    auto p1_hold = save_hold(play->m_player1);
    auto p2_hold = save_hold(play->m_player2);
    bool p1_ui_jumping = false, p2_ui_jumping = false;
    int p1_touch_id = -1, p2_touch_id = -1;
    if (auto* ui = play->m_uiLayer) {
        p1_ui_jumping = ui->m_p1Jumping;
        p2_ui_jumping = ui->m_p2Jumping;
        p1_touch_id = ui->m_p1TouchId;
        p2_touch_id = ui->m_p2TouchId;
    }
    auto restore_holds = [&] {
        auto restore_hold = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
            if (!body) return;
            body->m_jumpBuffered = hold.first;
            body->m_holdingButtons = hold.second;
        };
        restore_hold(play->m_player1, p1_hold);
        restore_hold(play->m_player2, p2_hold);
        if (auto* ui = play->m_uiLayer) {
            ui->m_p1Jumping = p1_ui_jumping;
            ui->m_p2Jumping = p2_ui_jumping;
            ui->m_p1TouchId = p1_touch_id;
            ui->m_p2TouchId = p2_touch_id;
        }
    };

    std::pair<bool, gd::map<int, bool>> sim_hold1, sim_hold2;
    bool sim_p1_ui_jumping = false, sim_p2_ui_jumping = false;
    int sim_p1_touch_id = -1, sim_p2_touch_id = -1;
    SliceStateHooks hold_hooks{
        [&] {
            copy_hold_into(sim_hold1, play->m_player1);
            copy_hold_into(sim_hold2, play->m_player2);
            if (auto* ui = play->m_uiLayer) {
                sim_p1_ui_jumping = ui->m_p1Jumping;
                sim_p2_ui_jumping = ui->m_p2Jumping;
                sim_p1_touch_id = ui->m_p1TouchId;
                sim_p2_touch_id = ui->m_p2TouchId;
            }
        },
        [&] {
            restore_holds();
            simulating = false;
            logic.predicting = false;
            Logic::get().sync_predict_hooks();
        },
        [&] {
            copy_hold_into(p1_hold, play->m_player1);
            copy_hold_into(p2_hold, play->m_player2);
            if (auto* ui = play->m_uiLayer) {
                p1_ui_jumping = ui->m_p1Jumping;
                p2_ui_jumping = ui->m_p2Jumping;
                p1_touch_id = ui->m_p1TouchId;
                p2_touch_id = ui->m_p2TouchId;
            }
            simulating = true;
            logic.predicting = true;
            Logic::get().sync_predict_hooks();
            auto restore_hold = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
                if (!body) return;
                body->m_jumpBuffered = hold.first;
                body->m_holdingButtons = hold.second;
            };
            restore_hold(play->m_player1, sim_hold1);
            restore_hold(play->m_player2, sim_hold2);
            if (auto* ui = play->m_uiLayer) {
                ui->m_p1Jumping = sim_p1_ui_jumping;
                ui->m_p2Jumping = sim_p2_ui_jumping;
                ui->m_p1TouchId = sim_p1_touch_id;
                ui->m_p2TouchId = sim_p2_touch_id;
            }
        }
    };
    slice_state_hooks.push_back(&hold_hooks);
    struct PopHook {
        std::vector<SliceStateHooks*>& hooks;
        ~PopHook() {
            hooks.pop_back();
        }
    } pop_holds{ slice_state_hooks };

    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    auto is_live = [&](bool player2) {
        if (logic.plays_player(player2)) return false;
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
        if (!body) return false;
        int jump = (int)PlayerButton::Jump;
        auto it = body->m_holdingButtons.find(jump);
        return it != body->m_holdingButtons.end() && it->second;
    };
    auto actual_click = [&](bool player1) {
        if (logic.plays_player(!player1)) return ClickType::Replay;
        PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
        if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart) {
            return Logic::get().physical_hold(play, !player1) ? ClickType::Hold : ClickType::Release;
        }
        return jump_held(body) ? ClickType::Hold : ClickType::Release;
    };
    const int MEASURE_LIVE = 1, MEASURE_HOLD = 2, MEASURE_RELEASE = 4;
    auto run = [&](bool player1, ClickType click, bool both, bool show, int measure) {
        if (job_cancel_requested) return;
        simulate(play, player1, click, both, show, step_limit, measure);
    };
    auto run_click_types = [&](bool player1, bool both) {
        auto actual = actual_click(player1);
        auto run_click = [&](ClickType click, bool show) {
            int measure = 0;
            if (click == actual) measure |= MEASURE_LIVE;
            if (logic.pause_at_traj_length && click == ClickType::Hold) measure |= MEASURE_HOLD;
            if (logic.pause_at_traj_length && click == ClickType::Release) measure |= MEASURE_RELEASE;
            if (!show && measure == 0) return;
            run(player1, click, both, show, measure);
        };
        if (play->m_playerDied || rendering) {
            run_click(actual, draw);
            return;
        }
        bool show_hold = draw && logic.traj_hold, show_swift = draw && logic.traj_swift;
        bool show_release = draw && logic.traj_release;
        bool need_hold = show_hold || logic.pause_at_traj_length;
        bool need_release = show_release || logic.pause_at_traj_length;
        if (need_hold) run_click(ClickType::Hold, show_hold);
        if (show_swift) run_click(ClickType::Swift, true);
        if (need_release) run_click(ClickType::Release, show_release);
        bool covered = (actual == ClickType::Hold && need_hold) || (actual == ClickType::Swift && show_swift) || (actual == ClickType::Release && need_release);
        if (!covered) run_click(actual, draw);
    };
    bool hide1 = Logic::get().traj_player_hidden(false), hide2 = Logic::get().traj_player_hidden(true);
    bool linked = dual && !twoPlayer && !hide1 && !hide2;
    if (!hide1) {
        if (!rendering && !play->m_playerDied && logic.plays_player(false))
            run(true, ClickType::Replay, linked, draw, MEASURE_LIVE);
        if (is_live(false) || rendering || play->m_playerDied) run_click_types(true, linked);
    }
    bool draw_player2 = dual && !hide2 && !linked && (twoPlayer || is_live(true) || logic.plays_player(true) || play->m_playerDied);
    if (draw_player2) {
        if (!rendering && !play->m_playerDied && logic.plays_player(true))
            run(false, ClickType::Replay, false, draw, MEASURE_LIVE);
        if (is_live(true) || rendering || play->m_playerDied) run_click_types(false, false);
    }
    restore_holds();
    Logic::get().sync_death_label(play);
    Logic::get().check_advance_pause(play);

    simulating = false;
    logic.predicting = false;
    Logic::get().sync_predict_hooks();
    drawing_valid = !job_cancel_requested;
    last_cache_key = key;
    logic.traj_seconds = saved_seconds;
}

void Trajectory::refresh(PlayLayer* play) {
    PerfScope traj_time(PERF_TRAJECTORY);
#ifdef GEODE_IS_WINDOWS
    if (inside_job) return;
    if (back_draw_node && play) Logic::get().attach_overlay(back_draw_node, play);
    if (job_running) {
        if (play == job_play && want_slicing(play)) {
            run_slice();
            return;
        }
        cancel_job();
    }
    if (want_slicing(play)) {
        if (!main_fiber)
            main_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
        if (main_fiber && !job_fiber)
            job_fiber = CreateFiber(4u << 20, reinterpret_cast<LPFIBER_START_ROUTINE>(&Trajectory::job_fiber_main), this);
        if (job_fiber) {
            job_running = true;
            job_done = job_cancel_requested = job_swapped_nodes = false;
            job_play = play;
            job_frame = Logic::get().step_frame;
            job_last_drawn_frame = 0;
            run_slice();
            return;
        }
    }
#endif
    refresh_now(play);
}

bool Trajectory::want_slicing(PlayLayer* play) const {
#ifdef GEODE_IS_WINDOWS
    auto const& logic = Logic::get();
    if (!(play && draw_node && back_draw_node && logic.lock_delta && logic.show_traj && !Logic::get().render_capturing() && !logic.frame_advance &&
            !logic.traj_autoclicker && !(logic.pause_before_kill && logic.is_recording()) && !play->m_playerDied && !survival_path_out))
        return false;
    return true;
#else
    (void)play;
    return false;
#endif
}

void Trajectory::job_fiber_main(void* self) {
#ifdef GEODE_IS_WINDOWS
    auto* trajectory = static_cast<Trajectory*>(self);
    for (;;) {
        trajectory->inside_job = true;
        trajectory->refresh_now(trajectory->job_play);
        trajectory->inside_job = false;
        trajectory->job_done = true;
        SwitchToFiber(trajectory->main_fiber);
    }
#else
    (void)self;
#endif
}

void Trajectory::yield_if_slice_over() {
#ifdef GEODE_IS_WINDOWS
    if (!inside_job || job_cancel_requested) return;
    if ((++slice_point_count & 15) != 0) return;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - slice_start).count() < slice_budget_seconds)
        return;
    for (auto* hook : slice_state_hooks)
        hook->save();
    for (auto it = slice_state_hooks.rbegin(); it != slice_state_hooks.rend(); ++it)
        (*it)->pause();
    inside_job = false;
    SwitchToFiber(main_fiber);
    inside_job = true;
    for (auto* hook : slice_state_hooks)
        hook->resume();
#endif
}

void Trajectory::run_slice() {
#ifdef GEODE_IS_WINDOWS
    if (!job_running) return;
    auto* director = CCDirector::sharedDirector();
    unsigned drawn = director->getTotalFrames();
    if (drawn != 0 && drawn == job_last_drawn_frame) return;
    if (job_play != PlayLayer::get() || job_frame != Logic::get().step_frame) {
        abandon_job();
        return;
    }
    job_last_drawn_frame = drawn;
    auto& logic = Logic::get();
    double interval = director->getAnimationInterval();
    slice_start = std::chrono::steady_clock::now();
    double used = std::chrono::duration<double>(slice_start - logic.frame_start).count();
    slice_budget_seconds = interval - used - logic.draw_time;
    slice_point_count = 0;
    SwitchToFiber(job_fiber);
    if (job_done) {
        finish_job();
        return;
    }
    if (job_swapped_nodes) {
        draw_node->setVisible(false);
        back_draw_node->setVisible(Logic::get().show_traj);
    }
#endif
}

void Trajectory::finish_job() {
#ifdef GEODE_IS_WINDOWS
    job_running = false;
    if (job_swapped_nodes) {
        if (job_cancel_requested) std::swap(draw_node, back_draw_node);
        back_draw_node->clear();
        back_draw_node->setVisible(false);
        draw_node->setVisible(Logic::get().show_traj);
    }
    job_swapped_nodes = job_cancel_requested = job_done = false;
    job_play = nullptr;
#endif
}

void Trajectory::cancel_job() {
#ifdef GEODE_IS_WINDOWS
    if (!job_running || inside_job) return;
    if (job_play != PlayLayer::get() || job_frame != Logic::get().step_frame) {
        abandon_job();
        return;
    }
    job_cancel_requested = true;
    while (!job_done) {
        slice_start = std::chrono::steady_clock::now();
        slice_budget_seconds = 1e9;
        SwitchToFiber(job_fiber);
    }
    finish_job();
#endif
}

void Trajectory::abandon_job() {
#ifdef GEODE_IS_WINDOWS
    if (job_fiber) DeleteFiber(job_fiber);
    job_fiber = nullptr;
    job_running = job_done = job_cancel_requested = inside_job = false;
    slice_state_hooks.clear();
    simulating = false;
    Logic::get().predicting = false;
    Logic::get().sync_predict_hooks();
    activated_by_fake1.clear();
    activated_by_fake2.clear();
    unflushed_paths[0].clear();
    unflushed_paths[1].clear();
    drawing_valid = false;
    if (job_swapped_nodes) {
        std::swap(draw_node, back_draw_node);
        back_draw_node->clear();
        back_draw_node->setVisible(false);
        job_swapped_nodes = false;
    }
    job_play = nullptr;
#endif
}

Trajectory::ClickType Trajectory::next_click(PlayLayer* play, bool player1) const {
    auto const& logic = Logic::get();
    PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
    if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart)
        return Logic::get().physical_hold(play, !player1) ? ClickType::Hold : ClickType::Release;
    int jump = (int)PlayerButton::Jump;
    for (auto it = play->m_queuedButtons.rbegin(); it != play->m_queuedButtons.rend(); ++it) {
        int button = (int)it->m_button;
        if ((button == 0 || button == jump) && it->m_isPlayer2 != player1)
            return it->m_isPush ? ClickType::Hold : ClickType::Release;
    }
    if (!body) return ClickType::Release;
    auto found = body->m_holdingButtons.find(jump);
    return found != body->m_holdingButtons.end() && found->second ? ClickType::Hold : ClickType::Release;
}

template <class F>
void Trajectory::run_prediction(PlayLayer* play, F&& predict) {
    cancel_job();
    auto& logic = Logic::get();
    if (play->m_objectLayer) {
        if (!fake1->getParent()) play->m_objectLayer->addChild(fake1);
        if (!fake2->getParent()) play->m_objectLayer->addChild(fake2);
    }
    park_fake(fake1);
    park_fake(fake2);
    simulating = true;
    phys::prepare_prediction_art();
    logic.predicting = true;
    Logic::get().sync_predict_hooks();
    auto save_hold = [](PlayerObject* body) {
        return body ? std::pair{ body->m_jumpBuffered, body->m_holdingButtons } : std::pair{ false, gd::map<int, bool>{} };
    };
    auto p1_hold = save_hold(play->m_player1);
    auto p2_hold = save_hold(play->m_player2);
    predict();
    auto restore_hold = [](PlayerObject* body, std::pair<bool, gd::map<int, bool>> const& hold) {
        if (!body) return;
        body->m_jumpBuffered = hold.first;
        body->m_holdingButtons = hold.second;
    };
    restore_hold(play->m_player1, p1_hold);
    restore_hold(play->m_player2, p2_hold);
    simulating = false;
    logic.predicting = false;
    Logic::get().sync_predict_hooks();
}

int Trajectory::flip_survival(PlayLayer* play, bool player1, int& full, std::vector<CCPoint>* path, int limit) {
    full = 0;
    if (!play || !play->m_started || play->m_playerDied || !draw_node || !fake1 || !fake2) return 0;
    auto& logic = Logic::get();
    const int MEASURE_HOLD = 2, MEASURE_RELEASE = 4;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool linked = player1 && play->m_gameState.m_isDualMode && play->m_player2 && !twoPlayer;
    ClickType flip = next_click(play, player1) == ClickType::Hold ? ClickType::Release : ClickType::Hold;
    float saved_alive[4] = { logic.traj_hold_alive_p1, logic.traj_hold_alive_p2, logic.traj_release_alive_p1, logic.traj_release_alive_p2 };
    measured_alive_steps[0] = measured_alive_steps[1] = measured_total_steps = 0;
    survival_path_out = path;
    run_prediction(play, [&] {
        simulate(play, player1, flip, linked, false, limit, flip == ClickType::Hold ? MEASURE_HOLD : MEASURE_RELEASE);
    });
    survival_path_out = nullptr;
    logic.traj_hold_alive_p1 = saved_alive[0];
    logic.traj_hold_alive_p2 = saved_alive[1];
    logic.traj_release_alive_p1 = saved_alive[2];
    logic.traj_release_alive_p2 = saved_alive[3];
    drawing_valid = false;
    full = measured_total_steps;
    return std::min(measured_alive_steps[0], measured_alive_steps[1]);
}

int Trajectory::click_survival(PlayLayer* play, bool player1, ClickType click, int& full, int limit) {
    full = 0;
    if (!play || !play->m_started || play->m_playerDied || !draw_node || !fake1 || !fake2) return 0;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool linked = player1 && play->m_gameState.m_isDualMode && play->m_player2 && !twoPlayer;
    measured_alive_steps[0] = measured_alive_steps[1] = measured_total_steps = 0;
    run_prediction(play, [&] {
        simulate(play, player1, click, linked, false, limit, 8);
    });
    drawing_valid = false;
    full = measured_total_steps;
    return std::min(measured_alive_steps[0], measured_alive_steps[1]);
}

void Trajectory::autoclick(PlayLayer* play) {
    PerfScope click_time(PERF_AUTOCLICK);
    auto& logic = Logic::get();
    if (!logic.traj_autoclicker || !play || play->m_isPaused || play->m_playerDied) return;
    if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) return;
    if (logic.step_frame < clicked_frame) clicked_frame = -1;
    if (logic.step_frame == clicked_frame) return;
    std::memset(logic.assist_clicks, 0, sizeof(logic.assist_clicks));
    lookahead(play);
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    bool flip1 = logic.death_next_p1 || (dual && !twoPlayer && logic.death_next_p2);
    bool flip2 = dual && twoPlayer && logic.death_next_p2;
    int lookahead_steps = std::max(logic.traj_click_lookahead, 1);
    int current_lives[2] = { 0, 0 };
    auto is_watched = [&](bool player2) {
        return logic.is_recording() ? logic.records_player(player2) : !logic.plays_player(player2);
    };
    auto dies_within_lookahead = [&](bool player1) {
        int full = 0;
        int lives = click_survival(play, player1, next_click(play, player1), full, lookahead_steps * 2);
        if (full <= 0) return false;
        if (lives >= full || lives >= lookahead_steps) return false;
        current_lives[player1 ? 0 : 1] = lives;
        return true;
    };
    if (!flip1 && is_watched(false) && !logic.plays_player(false)) {
        bool dies_soon = dies_within_lookahead(true);
        if (lookahead_steps > 1) flip1 = dies_soon;
    }
    if (!flip2 && dual && twoPlayer && is_watched(true) && !logic.plays_player(true)) {
        bool dies_soon = dies_within_lookahead(false);
        if (lookahead_steps > 1) flip2 = dies_soon;
    }
    if (!flip1 && !flip2) return;
    int jump = (int)PlayerButton::Jump;
    bool linked = dual && !twoPlayer;
    auto flip_click = [&](bool player1) {
        bool held = next_click(play, player1) == ClickType::Hold;
        auto* body = player1 ? play->m_player1 : play->m_player2;
        int current = current_lives[player1 ? 0 : 1];
        int horizon = current > 0 ? lookahead_steps * 2 : 0;
        int flip_full = 0, swift_full = 0;
        int flip_lives = click_survival(play, player1, held ? ClickType::Release : ClickType::Hold, flip_full, horizon);
        int swift_lives = click_survival(play, player1, ClickType::Swift, swift_full, horizon);
        if (current > 0 && std::max(flip_lives, swift_lives) <= current) return;
        if (logic.maintain_gravity && body && body->m_isDart && logic.records_player(!player1)) {
            bool& flipped = player1 ? logic.traj_flip_p1 : logic.traj_flip_p2;
            if (!flipped) {
                Logic::get().physical_hold(play, !player1);
                (player1 ? logic.traj_flip_hold_p1 : logic.traj_flip_hold_p2) = player1 ? logic.jump_held_p1 : logic.jump_held_p2;
            }
            flipped ^= true;
            return;
        }
        auto queue_click = [&](bool down) {
            ++logic.assist_clicks[player1 ? 0 : 1][down ? 1 : 0];
            play->queueButton(jump, down, !player1, Logic::get().click_stamp());
        };
        if (swift_lives > flip_lives) {
            if (held) queue_click(false);
            queue_click(true);
            queue_click(false);
            return;
        }
        queue_click(!held);
    };
    if (flip1) flip_click(true);
    if (flip2) flip_click(false);
    clicked_frame = logic.step_frame;
    lookahead(play);
}

void Trajectory::lookahead(PlayLayer* play) {
    PerfScope look_time(PERF_AUTOCLICK);
    auto& logic = Logic::get();
    logic.death_next_p1 = false;
    logic.death_next_p2 = false;
    if (!play || !play->m_started || play->m_playerDied || !play->m_player1 || !draw_node || !fake1 || !fake2)
        return;
    if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) return;
    auto actual_click = [&](bool player1) {
        return next_click(play, player1);
    };
    auto is_watched = [&](bool player2) {
        return logic.is_recording() ? logic.records_player(player2) : !logic.plays_player(player2);
    };
    const int MEASURE_LIVE = 1;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    bool linked = dual && !twoPlayer;
    run_prediction(play, [&] {
        if (is_watched(false) && !logic.plays_player(false))
            simulate(play, true, actual_click(true), linked, false, 1, MEASURE_LIVE);
        if (dual && !linked && is_watched(true) && !logic.plays_player(true))
            simulate(play, false, actual_click(false), false, false, 1, MEASURE_LIVE);
    });
    logic.traj_alive_p1 = logic.traj_alive_p2 = 0.f;
    logic.traj_dies_p1 = logic.traj_dies_p2 = false;
    drawing_valid = false;
}

bool Logic::is_traj_player(CCObject* object) {
    auto const& trajectory = Trajectory::get();
    if (!object) return false;
    return (trajectory.fake1 && object == static_cast<CCObject*>(trajectory.fake1)) ||
           (trajectory.fake2 && object == static_cast<CCObject*>(trajectory.fake2));
}

void Logic::reset_traj() {
    Trajectory::get().invalidate();
}
