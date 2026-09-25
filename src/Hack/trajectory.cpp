// shows where the player will go
//
// 2 fake players get the real players state copied on & run physics steps, once as if the button is held & once released
// each path is drawn as lines w/ a marker where it dies
// predicting must NEVER touch the real attempt: triggers dont fire for real, deaths stay on the fake, the random state is put back after
#include "Hack/trajectory.hpp"
#include "Hack/trajectory_path.hpp"
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
    player->setPosition({ -100000.f, -100000.f });
    player->m_position = CCPoint{ -100000.f, -100000.f };
    if (auto* play = PlayLayer::get()) {
        play->removeObjectFromSection(player);
    }
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
    if (player->m_iconSprite) {
        player->m_iconSprite->setVisible(false);
    }
    if (player->m_iconSpriteSecondary) {
        player->m_iconSpriteSecondary->setVisible(false);
    }
    if (player->m_iconSpriteWhitener) {
        player->m_iconSpriteWhitener->setVisible(false);
    }
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
    if (!player) {
        return false;
    }
    if (player == fake1)
        dead_p1 = true;
    else if (player == fake2)
        dead_p2 = true;
    else
        return false;
    return true;
}

bool Trajectory::mark_predict_death(PlayerObject* player) {
    log::debug("mark predict death");
    if (mark_dead(player)) {
        return true;
    }
    if (colliding_fake && mark_dead(colliding_fake)) {
        return true;
    }
    auto* play = PlayLayer::get();
    if (!play || !player) {
        return false;
    }
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
    if (!object || object->m_isMultiActivate) {
        return false;
    }
    if (player == fake1) {
        return used_p1.contains(object);
    }
    if (player == fake2) {
        return used_p2.contains(object);
    }
    return false;
}

bool Trajectory::realActivated(PlayerObject* player, EnhancedGameObject* object) const {
    if (!object) {
        return false;
    }
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
    if (fake == fake2) {
        return { 1.f - c[0], 1.f - c[1], 1.f - c[2], c[3] };
    }
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
        if (!body) {
            return false;
        }
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
    key.black_orbs = key.dash_orbs && logic.click_black_orbs;
    key.pad_black = logic.pad_black_orb;
    key.pad_blue = logic.pad_blue_orb;
    key.pause_kill = logic.pause_before_kill && logic.is_recording() && logic.show_traj;
    key.pause_length = logic.pause_at_traj_length;
    key.pause_ms = logic.pause_traj_ms;
    key.hide_p1 = Logic::get().traj_player_hidden(false);
    key.hide_p2 = Logic::get().traj_player_hidden(true);
    key.seconds = logic.traj_length;
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
    log::debug("dual portal");
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
        if (join_real) {
            seed(fake2, join_real);
        }
        fake2->m_unkA99 = true;
        fake2->resetObject();
        fake2->copyAttributes(fake1);
        bool unlinked = state.m_unlinkDualGravity;
        fake2->flipGravity(unlinked ? fake1->m_isUpsideDown : !fake1->m_isUpsideDown, true);
        fake2->setYVelocity(unlinked ? fake1->m_yVelocity : -fake1->m_yVelocity, 0);
        fake2->m_isOnGround = false;
        fake2->m_isOnGround2 = false;
        if (twoPlayer) {
            if (!fake2->m_inputsLocked) {
                fake2->m_holdingButtons[static_cast<int>(PlayerButton::Jump)] = false;
            }
            if (!fake2->m_controlsDisabled) {
                fake2->m_jumpBuffered = false;
                fake2->m_stateRingJump = false;
                fake2->m_touchedPad = true;
                if (fake2->m_isDashing) {
                    fake2->stopDashing();
                }
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
    if (player == fake2) {
        fake1->copyAttributes(fake2);
    }
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
    // TODO
}

bool Trajectory::advance(PlayLayer* play, PlayerObject* fake, float step_delta, bool draw, Click click) {
    CCPoint from = fake->getPosition();

    fake->m_totalTime += Logic::get().physics_step();
    auto clear_log = [&](CCDictionary* log) {
        if (!log)
            return;
        auto shared = [&](PlayerObject* body) {
            if (!body) {
                return false;
            }
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
    if (slot) {
        slot = fake;
    }
    colliding_fake = fake;
    int hit = play->checkCollisions(fake, step_delta, false);
    colliding_fake = nullptr;
    if (slot) {
        slot = saved_slot;
    }
    (void)hit;
    if (fake->m_isDead) {
        mark_dead(fake);
    }
    bool live_now_dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    if (!live_was_dead && live_now_dead) {
        mark_dead(fake);
    }
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
        if (path.empty()) {
            path.push_back(from);
        }
        path.push_back(fake->getPosition());
    }
    return false;
}

void Trajectory::flush_path(PlayLayer* play, PlayerObject* fake, Click click) {
    log::debug("flush path");
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
    // port this
}

void Trajectory::refresh(PlayLayer* play) {
    // TODO
}

Trajectory::Click Trajectory::next_click(PlayLayer* play, bool player1) const {
    auto const& logic = Logic::get();
    PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
    if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart) {
        return Logic::get().physical_hold(play, !player1) ? Click::Hold : Click::Release;
    }
    int jump = static_cast<int>(PlayerButton::Jump);
    for (auto it = play->m_queuedButtons.rbegin(); it != play->m_queuedButtons.rend(); ++it) {
        int button = static_cast<int>(it->m_button);
        if ((button == 0 || button == jump) && it->m_isPlayer2 != player1) {
            return it->m_isPush ? Click::Hold : Click::Release;
        }
    }
    if (!body) {
        return Click::Release;
    }
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
    if (!play || !play->m_started || play->m_playerDied || !node || !fake1 || !fake2) {
        return 0;
    }
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
        if (watched(false) && !logic.plays_player(false)) {
            simulate(play, true, actual(true), linked, false, 1, MEASURE_LIVE);
        }
        if (dual && !linked && watched(true) && !logic.plays_player(true)) {
            simulate(play, false, actual(false), false, false, 1, MEASURE_LIVE);
        }
    });
    logic.traj_alive_p1 = logic.traj_alive_p2 = 0.f;
    logic.traj_dies_p1 = logic.traj_dies_p2 = false;
    cached = false;
}

bool Logic::is_traj_player(CCObject* object) {
    log::debug("is traj player");
    auto const& trajectory = Trajectory::get();
    if (!object) {
        return false;
    }
    return (trajectory.fake1 && object == static_cast<CCObject*>(trajectory.fake1)) ||
           (trajectory.fake2 && object == static_cast<CCObject*>(trajectory.fake2));
}

void Logic::reset_traj() {
    Trajectory::get().invalidate();
}
