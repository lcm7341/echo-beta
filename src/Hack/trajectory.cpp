// shows where the player will go
//
// 2 fake players get the real players state copied on & run physics steps, once as if the button is held & once released
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
    fake1 = spawn_fake(play, "trajectory-player-1"_spr);
    fake2 = spawn_fake(play, "trajectory-player-2"_spr);
}

void Trajectory::sync_seat(PlayLayer* play) {
    if (!node || !play)
        return;
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
    // TODO
}

void Trajectory::dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on) {
    // TODO
}

void Trajectory::press(PlayLayer* play, PlayerObject* fake, Click click) {
    // TODO
}

bool Trajectory::advance(PlayLayer* play, PlayerObject* fake, float step_delta, bool draw, Click click) {
    // todo
    return {};
}

void Trajectory::flush_path(PlayLayer* play, PlayerObject* fake, Click click) {
    // TODO
}

void Trajectory::draw_death(PlayerObject* fake) {
    // TODO
}

void Trajectory::simulate(PlayLayer* play, bool player1, Click click, bool both, bool draw, int limit, int measure) {
    // port this
}

void Trajectory::refresh(PlayLayer* play) {
    // TODO
}

Trajectory::Click Trajectory::next_click(PlayLayer* play, bool player1) const {
    // todo
    return {};
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
    // port this
    return {};
}

void Trajectory::autoclick(PlayLayer* play) {
    // TODO
}

void Trajectory::lookahead(PlayLayer* play) {
    // TODO
}

bool Logic::is_traj_player(CCObject* object) {
    // TODO
    return {};
}

void Logic::reset_traj() {
    Trajectory::get().invalidate();
}
