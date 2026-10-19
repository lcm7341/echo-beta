#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace geode::prelude;

// this HAS to be visual only
// skipping toggleFlipped broke stuff
struct ScreenFlip {
    Ref<GameObject> object;
    CCPoint before, after;
    bool mirrored = false, swapped = false;
};

static std::vector<ScreenFlip> g_screen_flips;

static std::vector<Ref<GameObject>> g_slid_objects;

#if defined(GEODE_IS_WINDOWS)
#define ECHO_NO_MIRROR_DRAW
const std::ptrdiff_t PLAIN_DRAW = 0x31f4;
const std::ptrdiff_t MIRROR_DIRECTION = 0x3204;
#elif defined(GEODE_IS_MACOS)
#define ECHO_NO_MIRROR_DRAW
const std::ptrdiff_t PLAIN_DRAW = 0x305c;
const std::ptrdiff_t MIRROR_DIRECTION = 0x306c;
#elif defined(GEODE_IS_IOS)
#define ECHO_NO_MIRROR_DRAW
const std::ptrdiff_t PLAIN_DRAW = 0x3084;
const std::ptrdiff_t MIRROR_DIRECTION = 0x3094;
#endif

#ifdef ECHO_NO_MIRROR_DRAW
static float& layer_float(GJBaseGameLayer* layer, std::ptrdiff_t offset) {
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(layer) + offset);
}

static float& mirror_direction(GJBaseGameLayer* layer) {
    return layer_float(layer, MIRROR_DIRECTION);
}

static bool mid_flip(GJBaseGameLayer* layer) {
    float progress = layer->m_gameState.m_levelFlipProgress;
    return progress > 0.f && progress < 1.f;
}

static bool in_mirrored_half(GJBaseGameLayer* layer) {
    float progress = layer->m_gameState.m_levelFlipProgress;
    return mirror_direction(layer) == 1.f ? progress >= .5f : progress <= .5f;
}

static void mirror_object(GameObject* object) {
    const float quarter_turn = 90.f, three_quarter_turn = 270.f;
    const int degrees_per_quarter = 90;
    float turn = std::abs(object->getRotation());
    if (turn == quarter_turn || turn == three_quarter_turn) object->setFlipY(!object->isFlipY());
    else object->setFlipX(!object->isFlipX());
    if ((int)turn % degrees_per_quarter != 0) object->setRotation(-object->getRotation());
}

static void unslide_wave(CCCircleWave* wave) {
    if (!wave->m_target) return;
    for (const auto& flip : g_screen_flips) {
        if (flip.object.data() != wave->m_target || wave->m_target->getPosition() != flip.after) continue;
        wave->setPosition(flip.before);
        return;
    }
}

static bool offsets_player(GJBaseGameLayer* layer, PlayerObject* body) {
    if (!body) return false;
#ifdef GEODE_IS_WINDOWS
    if (reinterpret_cast<unsigned char*>(body)[0xa2a] != 0)
        return false;                                                                      // why
    return body == layer->m_player1 || reinterpret_cast<unsigned char*>(layer)[0x422] != 0; // tf
#else
    return body == layer->m_player1 || layer->m_gameState.m_isDualMode;
#endif
}
#endif

class $modify(NoMirrorLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::visit", Priority::VeryLate);
    }

    void update(float dt) {
        GJBaseGameLayer::update(dt);
#ifdef ECHO_NO_MIRROR_DRAW
        if (!Logic::get().no_mirror_portals || !Logic::as_play(this) || !mid_flip(this)) return;
        for (auto* body : { m_player1, m_player2 }) {
            if (offsets_player(this, body)) body->setPosition(body->m_position);
        }
#endif
    }

    void visit() {
#ifdef ECHO_NO_MIRROR_DRAW
        if (!Logic::get().no_mirror_portals || !Logic::as_play(this) || !mid_flip(this)) {
            g_screen_flips.clear();
            GJBaseGameLayer::visit();
            return;
        }
        bool plain = layer_float(this, PLAIN_DRAW) < 1.f;
        bool flipped = !plain && m_player1 && in_mirrored_half(this);
        std::vector<std::pair<PlayerObject*, CCPoint>> players;
        for (auto* body : { m_player1, m_player2 }) {
            if (!offsets_player(this, body)) continue;
            players.emplace_back(body, body->getPosition());
            body->setPosition(body->m_position);
        }
        auto& flips = g_screen_flips;
        for (auto& flip : flips) {
            flip.swapped = flip.object->getPosition() == flip.after;
            if (!flip.swapped) continue;
            flip.object->setPosition(flip.before);
            if (flip.mirrored) mirror_object(flip.object);
        }
        if (flipped) flipObjects();
        GJBaseGameLayer::visit();
        if (flipped) flipObjects();
        for (auto& flip : flips) {
            if (!flip.swapped) continue;
            if (flip.mirrored) mirror_object(flip.object);
            flip.object->setPosition(flip.after);
        }
        for (auto& [body, position] : players)
            body->setPosition(position);
#else
        GJBaseGameLayer::visit();
#endif
    }

    void updateCamera(float dt) {
        GJBaseGameLayer::updateCamera(dt);
        if (!Logic::get().no_mirror_portals || !Logic::as_play(this) || !m_objectLayer) return;
#ifdef ECHO_NO_MIRROR_DRAW
        if (mirror_direction(this) != -1.f) return;
#else
        if (!(m_objectLayer->getScaleX() < 0.f)) return;
#endif
        float scale_x = m_objectLayer->getScaleX();
        auto position = m_objectLayer->getPosition();
        m_objectLayer->setScaleX(-scale_x);
        m_objectLayer->setPosition({ CCDirector::get()->getWinSize().width - position.x, position.y });
        updateExtraGameLayers();
    }

    void updateCameraBGArt(CCPoint position, float zoom) {
        if (!Logic::get().no_mirror_portals || !Logic::as_play(this)) {
            GJBaseGameLayer::updateCameraBGArt(position, zoom);
            return;
        }
        float progress = m_gameState.m_levelFlipProgress;
        m_gameState.m_levelFlipProgress = 0.f;
        GJBaseGameLayer::updateCameraBGArt(position, zoom);
        m_gameState.m_levelFlipProgress = progress;
    }
};

class $modify(NoMirrorObjects, PlayLayer) {
    void updateVisibility(float dt) {
        g_screen_flips.clear();
        std::vector<Ref<GameObject>> before;
        before.swap(g_slid_objects);
        PlayLayer::updateVisibility(dt);
        if (before.empty() || !(Logic::get().no_mirror_portals || Logic::get().layout_active()))
            return;
        std::unordered_set<GameObject*> still;
        for (const auto& object : g_slid_objects)
            still.insert(object.data());
        for (const auto& object : before) {
            if (!still.contains(object.data())) object->setPosition(object->getRealPosition());
        }
    }

    void screenFlipObject(GameObject* object) {
        if (object) g_slid_objects.push_back(object);
        if (!Logic::get().no_mirror_portals || !object) {
            PlayLayer::screenFlipObject(object);
            return;
        }
        auto before = object->getPosition();
        PlayLayer::screenFlipObject(object);
        auto after = object->getPosition();
#ifdef ECHO_NO_MIRROR_DRAW
        bool mirrored = in_mirrored_half(this);
#else
        bool mirrored = false;
#endif
        if (after != before || mirrored) g_screen_flips.push_back({ object, before, after, mirrored });
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
#ifdef ECHO_NO_MIRROR_DRAW
        if (!Logic::get().no_mirror_portals || g_screen_flips.empty() || !m_circleWaveArray) return;
        for (auto* wave : CCArrayExt<CCCircleWave*>(m_circleWaveArray)) unslide_wave(wave);
#endif
    }
};

class $modify(NoMirrorCircle, CCCircleWave) {
    void updatePosition(float dt) {
        CCCircleWave::updatePosition(dt);
#ifdef ECHO_NO_MIRROR_DRAW
        if (Logic::get().no_mirror_portals) unslide_wave(this);
#endif
    }
};
