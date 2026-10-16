#include "Logic/logic.hpp"
#include <Geode/Geode.hpp>
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
};

static std::vector<ScreenFlip>& screen_flips() {
    static std::vector<ScreenFlip> flips;
    return flips;
}

static std::vector<Ref<GameObject>>& slid() {
    static std::vector<Ref<GameObject>> objects;
    return objects;
}

#if defined(GEODE_IS_WINDOWS)
#define ECHO_NO_MIRROR_DRAW
constexpr std::ptrdiff_t PLAIN_DRAW = 0x31f4;
constexpr std::ptrdiff_t MIRROR_DIRECTION = 0x3204;
#elif defined(GEODE_IS_MACOS)
#define ECHO_NO_MIRROR_DRAW
constexpr std::ptrdiff_t PLAIN_DRAW = 0x305c;
constexpr std::ptrdiff_t MIRROR_DIRECTION = 0x306c;
#elif defined(GEODE_IS_IOS)
#define ECHO_NO_MIRROR_DRAW
constexpr std::ptrdiff_t PLAIN_DRAW = 0x3084;
constexpr std::ptrdiff_t MIRROR_DIRECTION = 0x3094;
#endif

#ifdef ECHO_NO_MIRROR_DRAW
static float& layer_float(GJBaseGameLayer* layer, std::ptrdiff_t offset) {
    return *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(layer) + offset);
}

static float& mirror_direction(GJBaseGameLayer* layer) {
    return layer_float(layer, MIRROR_DIRECTION);
}

static bool offsets_player(GJBaseGameLayer* layer, PlayerObject* body) {
    if (!body)
        return false;
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
    void visit() {
#ifdef ECHO_NO_MIRROR_DRAW
        float progress = m_gameState.m_levelFlipProgress;
        if (!Logic::get().no_mirror_portals || !Logic::as_play(this) || !(progress > 0.f && progress < 1.f)) {
            screen_flips().clear();
            GJBaseGameLayer::visit();
            return;
        }
        bool plain = layer_float(this, PLAIN_DRAW) < 1.f;
        bool flipped = !plain && m_player1 && (mirror_direction(this) == 1.f ? progress >= .5f : progress <= .5f);
        std::vector<std::pair<PlayerObject*, CCPoint>> players;
        for (auto* body : { m_player1, m_player2 }) {
            if (!offsets_player(this, body))
                continue;
            players.emplace_back(body, body->getPosition());
            body->setPosition(body->m_position);
        }
        auto& flips = screen_flips();
        for (auto& flip : flips) {
            if (flip.object->getPosition() == flip.after)
                flip.object->setPosition(flip.before);
        }
        if (flipped)
            flipObjects();
        GJBaseGameLayer::visit();
        if (flipped)
            flipObjects();
        for (auto& flip : flips) {
            if (flip.object->getPosition() == flip.before)
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
        if (!Logic::get().no_mirror_portals || !Logic::as_play(this) || !m_objectLayer)
            return;
#ifdef ECHO_NO_MIRROR_DRAW
        if (mirror_direction(this) != -1.f)
            return;
#else
        if (!(m_objectLayer->getScaleX() < 0.f))
            return;
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
        screen_flips().clear();
        std::vector<Ref<GameObject>> before;
        before.swap(slid());
        PlayLayer::updateVisibility(dt);
        if (before.empty())
            return;
        std::unordered_set<GameObject*> still;
        for (const auto& object : slid())
            still.insert(object.data());
        for (const auto& object : before) {
            if (!still.contains(object.data()))
                object->setPosition(object->getRealPosition());
        }
    }

    void screenFlipObject(GameObject* object) {
        if (object)
            slid().push_back(object);
        if (!Logic::get().no_mirror_portals || !object) {
            PlayLayer::screenFlipObject(object);
            return;
        }
        auto before = object->getPosition();
        PlayLayer::screenFlipObject(object);
        auto after = object->getPosition();
        if (after != before)
            screen_flips().push_back({ object, before, after });
    }
};
