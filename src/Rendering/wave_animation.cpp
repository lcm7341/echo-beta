// orb rings (CCCircleWave) at high tps
// every orb press makes a ring. presses on the same orb on the same frame make rings that stay identical for their whole life
// merge_fresh() finds those (same parent, spot, color, size & timing) & keeps 1, counted as many, so it is drawn as bright as the stack was
// WaveAnimation takes over the rings own update so that 1 ring can stand in for the count
#include <Geode/Geode.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include "circle_signatures.hpp"
#include "wave_timeline.hpp"
#include "wave_merge.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;
namespace {
    bool compact_animation = false;

    struct Params {
        float from, to, duration;
        bool fadeIn, easeOut;
    };

    std::vector<CCCircleWave*> fresh;
    std::unordered_map<CCCircleWave*, Params> params;
    std::unordered_map<CCCircleWave*, float> counts;

    void forget(CCCircleWave* wave) {
        params.erase(wave);
        counts.erase(wave);
    }

    echo::waves::Power native_power = nullptr;
    constexpr bool fused_tween = false;

    class WaveAnimation final : public CCActionInterval {
        echo::waves::Timeline timeline;
        float from_radius = 0.f, to_radius = 0.f;
        bool fadeIn = false, easeOut = false;
        CCCircleWave* removal_target = nullptr;

    public:
        ~WaveAnimation() override {
            if (!removal_target)
                return;
            forget(removal_target);
            removal_target->release();
        }

        bool init(float duration, float from, float to, bool fade, bool ease, CCCircleWave* removal) {
            if (!CCActionInterval::initWithDuration(duration))
                return false;
            if (removal)
                removal->retain();
            if (removal_target)
                removal_target->release();
            removal_target = removal;
            from_radius = from;
            to_radius = to;
            fadeIn = fade;
            easeOut = ease;
            timeline.reset(from, to, fade, ease);
            return true;
        }

        void startWithTarget(CCNode* target) override {
            CCActionInterval::startWithTarget(target);
            timeline.reset(from_radius, to_radius, fadeIn, easeOut);
        }

        void update(float time) override {
            struct Target {
                CCNode* wave;
                CCCircleWave* removal;

                void radius(float value) {
                    wave->updateTweenAction(value, "radius");
                }

                void opacity(float value) {
                    wave->updateTweenAction(value, "opacity");
                }

                void remove() {
                    forget(removal);
                    removal->removeMeAndCleanup();
                }
            } target{ m_pTarget, removal_target };

            timeline.update<fused_tween>(time, native_power, target);
        }

        CCActionInterval* reverse() override {
            CCActionInterval* radius = CCActionTween::create(getDuration(), "radius", from_radius, to_radius);
            auto* removal = CCCallFunc::create(removal_target, callfunc_selector(CCCircleWave::removeMeAndCleanup));
            CCActionInterval* graph;
            if (fadeIn) {
                float half = getDuration() * 0.5f;
                auto* up = CCActionTween::create(half, "opacity", 0.f, 255.f);
                auto* down = CCActionTween::create(half, "opacity", 255.f, 0.f);
                graph = CCSpawn::create(radius, CCSequence::create(up, down, removal, nullptr), nullptr);
            }
            else {
                CCActionInterval* opacity = CCActionTween::create(getDuration(), "opacity", 255.f, 0.f);
                if (easeOut) {
                    radius = CCEaseOut::create(radius, 2.f);
                    opacity = CCEaseOut::create(opacity, 2.f);
                }
                graph = CCSequence::create(CCSpawn::create(radius, opacity, nullptr), removal, nullptr);
            }
            return graph->reverse();
        }

        CCObject* copyWithZone(CCZone* zone) override {
            auto* copy = zone && zone->m_pCopyObject ? static_cast<WaveAnimation*>(zone->m_pCopyObject) : new WaveAnimation;
            if (!copy->init(getDuration(), from_radius, to_radius, fadeIn, easeOut, removal_target)) {
                if (!zone || !zone->m_pCopyObject)
                    delete copy;
                return nullptr;
            }
            return copy;
        }
    };
}

class $modify(EchoWaveAnimation, CCCircleWave) {
    bool init(float from, float to, float duration, bool fadeIn, bool easeOut) {
        if (!compact_animation || !std::isfinite(from) || !std::isfinite(to) ||
            !std::isfinite(to - from) || !std::isfinite(duration) || duration < 0.0001f)
            return CCCircleWave::init(from, to, duration, fadeIn, easeOut);
        auto* action = new WaveAnimation;
        if (!action->init(duration, from, to, fadeIn, easeOut, this)) {
            delete action;
            return CCCircleWave::init(from, to, duration, fadeIn, easeOut);
        }
        m_target = nullptr;
        m_radius = from;
        m_opacity = fadeIn ? 0.f : 255.f;
        m_color = { 255, 255, 255 };
        m_circleMode = CircleMode::Filled;
        m_lineWidth = 2;
        m_opacityMod = 1.f;
        m_blendAdditive = true;
        action->autorelease();
        CCDirector::sharedDirector()->getActionManager()->addAction(action, this, false);
        params[this] = { from, to, duration, fadeIn, easeOut };
        retain();
        fresh.push_back(this);
        return true;
    }
};

namespace {
    bool same(float a, float b) {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    bool identical(CCCircleWave* a, CCCircleWave* b) {
        auto pa = params.find(a), pb = params.find(b);
        if (pa == params.end() || pb == params.end())
            return false;
        auto const& x = pa->second;
        auto const& y = pb->second;
        if (!same(x.from, y.from) || !same(x.to, y.to) || !same(x.duration, y.duration) ||
            x.fadeIn != y.fadeIn || x.easeOut != y.easeOut)
            return false;
        auto point_same = [](CCPoint p, CCPoint q) {
            return same(p.x, q.x) && same(p.y, q.y);
        };
        return a->getParent() == b->getParent() && a->getZOrder() == b->getZOrder() &&
               point_same(a->getPosition(), b->getPosition()) && point_same(a->m_position, b->m_position) &&
               same(a->getScaleX(), b->getScaleX()) && same(a->getScaleY(), b->getScaleY()) &&
               same(a->getRotationX(), b->getRotationX()) && same(a->getRotationY(), b->getRotationY()) &&
               a->isVisible() == b->isVisible() && same(a->m_radius, b->m_radius) && same(a->m_width, b->m_width) &&
               same(a->m_opacity, b->m_opacity) && same(a->m_opacityMod, b->m_opacityMod) &&
               a->m_color.r == b->m_color.r && a->m_color.g == b->m_color.g && a->m_color.b == b->m_color.b &&
               a->m_circleMode == b->m_circleMode && a->m_lineWidth == b->m_lineWidth &&
               a->m_blendAdditive == b->m_blendAdditive && a->m_delegate == b->m_delegate &&
               (a->m_target == b->m_target || (a->m_target && b->m_target &&
                                                  point_same(a->m_target->getPosition(), b->m_target->getPosition())));
    }

    std::uint64_t signature(CCCircleWave* wave) {
        std::uint64_t hash = 14695981039346656037ull;
        auto mix = [&](std::uint64_t value) {
            hash = (hash ^ value) * 1099511628211ull;
        };
        auto const& x = params[wave];
        mix(std::bit_cast<std::uint32_t>(x.from));
        mix(std::bit_cast<std::uint32_t>(x.to));
        mix(std::bit_cast<std::uint32_t>(x.duration));
        mix(x.fadeIn * 2 + x.easeOut);
        mix(reinterpret_cast<std::uintptr_t>(wave->getParent()));
        mix(static_cast<std::uint32_t>(wave->getZOrder()));
        mix(std::bit_cast<std::uint32_t>(wave->m_position.x));
        mix(std::bit_cast<std::uint32_t>(wave->m_position.y));
        mix(std::bit_cast<std::uint32_t>(wave->m_radius));
        mix(std::bit_cast<std::uint32_t>(wave->m_opacity));
        mix(static_cast<std::uint32_t>(wave->m_circleMode));
        mix(wave->m_color.r | wave->m_color.g << 8 | wave->m_color.b << 16);
        return hash;
    }
}

namespace echo::waves {
    void merge_fresh() {
        if (fresh.empty())
            return;
        auto* manager = CCDirector::sharedDirector()->getActionManager();
        static std::unordered_map<std::uint64_t, std::vector<CCCircleWave*>> keepers;
        keepers.clear();
        for (auto* wave : fresh) {
            bool candidate = wave->getParent() && wave->m_blendAdditive && params.contains(wave) &&
                             manager->numberOfRunningActionsInTarget(wave) == 1;
            if (!candidate)
                continue;
            auto& group = keepers[signature(wave)];
            CCCircleWave* keeper = nullptr;
            for (auto* existing : group) {
                if (identical(existing, wave)) {
                    keeper = existing;
                    break;
                }
            }
            if (keeper) {
                float add = counts.contains(wave) ? counts[wave] : 1.f;
                float& total = counts.try_emplace(keeper, 1.f).first->second;
                total += add;
                forget(wave);
                wave->removeMeAndCleanup();
                continue;
            }
            group.push_back(wave);
        }
        for (auto* wave : fresh)
            wave->release();
        fresh.clear();
    }

    bool count_duplicate(CCNode* parent, CCPoint position, ccColor3B color, float from, float to, float duration,
        bool fadeIn, bool easeOut, void* delegate) {
        if (fresh.empty())
            return false;
        auto point_same = [](CCPoint p, CCPoint q) {
            return same(p.x, q.x) && same(p.y, q.y);
        };
        auto* manager = CCDirector::sharedDirector()->getActionManager();
        int looked = 0;
        for (auto it = fresh.rbegin(); it != fresh.rend() && looked < 64; ++it, ++looked) {
            auto* wave = *it;
            auto found = params.find(wave);
            if (found == params.end())
                continue;
            auto const& x = found->second;
            if (!same(x.from, from) || !same(x.to, to) || !same(x.duration, duration) || x.fadeIn != fadeIn || x.easeOut != easeOut)
                continue;
            if (wave->getParent() != parent || wave->m_delegate != delegate || !wave->m_target)
                continue;
            if (!point_same(wave->m_target->getPosition(), position))
                continue;
            if (wave->m_color.r != color.r || wave->m_color.g != color.g || wave->m_color.b != color.b)
                continue;
            if (!same(wave->m_radius, from) || !same(wave->m_opacity, fadeIn ? 0.f : 255.f) || !same(wave->m_opacityMod, 1.f) ||
                wave->m_circleMode != CircleMode::Filled || wave->m_lineWidth != 2 || !wave->m_blendAdditive)
                continue;
            if (wave->getZOrder() != 0 || !wave->isVisible() || !same(wave->getScaleX(), 1.f) || !same(wave->getScaleY(), 1.f) ||
                !same(wave->getRotationX(), 0.f) || !same(wave->getRotationY(), 0.f))
                continue;
            if (manager->numberOfRunningActionsInTarget(wave) != 1)
                continue;
            counts.try_emplace(wave, 1.f).first->second += 1.f;
            return true;
        }
        return false;
    }

    float count_of(CCCircleWave* wave) {
        if (counts.empty())
            return 1.f;
        auto it = counts.find(wave);
        return it == counts.end() ? 1.f : it->second;
    }
}

$execute {
    using echo::circles::Signature;
    // { where, size, hash } of gd code
    constexpr Signature arm_init{ 0x5a66fc, 0x190, 0x60ce8f2e06fb6009 };
    constexpr Signature arm_tween{ 0xac9ba8, 0x74, 0xce8bc7c8df1db4b1 };
    constexpr Signature arm_ease{ 0xab69e0, 0x40, 0x9cd717e61dcb832c };
    constexpr Signature win_init{ 0x42940, 0x213, 0xf375e3ceb8ec56da };
    constexpr Signature win_tween{ 0x3d1a0, 0x79, 0xeed524abc5b5ff1f };
    constexpr Signature win_ease{ 0x2a4d0, 0x3d, 0xf3ef639193fee5fc };
    auto cocos_module = geode::base::getCocos();
    constexpr auto init_signature = win_init, tween_signature = win_tween, ease_signature = win_ease;
    auto matches = [](uintptr_t module, echo::circles::Signature const& signature) {
        return echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(module + signature.offset), signature.size) == signature.hash;
    };
    if (!matches(geode::base::get(), init_signature) || !matches(cocos_module, tween_signature) ||
        !matches(cocos_module, ease_signature)) {
        return;
    }
    native_power = *reinterpret_cast<echo::waves::Power*>(cocos_module + 0xe4b00); // powf
    compact_animation = true;
}
