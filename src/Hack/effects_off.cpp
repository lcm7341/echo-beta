// no effects & hide endscreen (scarlet utils has both). every hook in here just skips a gd effect while its setting is on.
// the rest of no effects sits in hooks.cpp next to hooks that were already there: objects get m_hasNoEffects as the lvl loads
// (so the level has to be opened again after turning it on), & the portal circle, spider dash, shine & dash bursts are skipped
#include <Geode/Geode.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/ParticleGameObject.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "Logic/logic.hpp"

using namespace geode::prelude;

class $modify(EchoQuietLayer, GJBaseGameLayer) {
    void playExitDualEffect(PlayerObject* player) {
        auto& logic = Logic::get();
        if (logic.no_effects || (m_playerDied && logic.disable_death_effect)) return;
        GJBaseGameLayer::playExitDualEffect(player);
    }
};

class $modify(EchoQuietPlay, PlayLayer) {
    void playGravityEffect(bool flip) {
        if (Logic::get().no_effects) return;
        PlayLayer::playGravityEffect(flip);
    }
};

class $modify(EchoQuietTrigger, EffectGameObject) {
    void playTriggerEffect() {
        if (Logic::get().no_effects) return;
        EffectGameObject::playTriggerEffect();
    }
};

class $modify(EchoQuietWave, CCCircleWave) {
    void draw() {
        if (Logic::get().no_effects && PlayLayer::get()) return;
        CCCircleWave::draw();
    }
};

class $modify(EchoQuietParticle, ParticleGameObject) {
    void updateParticle() {
        ParticleGameObject::updateParticle();
        if (Logic::get().no_effects && PlayLayer::get()) this->setVisible(false);
    }
};

class $modify(EchoEndLayer, EndLevelLayer) {
    void showLayer(bool instant) {
        if (Logic::get().hide_endscreen) return;
        EndLevelLayer::showLayer(instant);
    }
};
