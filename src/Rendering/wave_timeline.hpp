#pragma once
#include <cmath>

namespace echo::waves {
using Power = float (*)(float, float);

// Match CCActionTween's to - delta * (1 - t), rather than the numerically
// different from + delta * t. Android's native instruction is a fused subtract.
template<bool fused>
float tween(float from, float to, float time) {
    float delta = to - from;
    float left = 1.f - time;
    if constexpr (fused) return std::fma(-delta, left, to);
    else { volatile float product = delta * left; return to - product; }
}

// The native graph is radius + opacity in parallel, followed by removal.
// Fade-in opacity has two halves; preserve its completion callback when
// crossing the midpoint, including a step that jumps straight to the end.
class Timeline {
    float fromRadius = 0.f, toRadius = 0.f;
    bool fadeIn = false, easeOut = false;
    int opacityPhase = -1;
    bool finished = false;
public:
    void reset(float from, float to, bool fade, bool ease) {
        fromRadius = from; toRadius = to; fadeIn = fade; easeOut = ease;
        opacityPhase = -1; finished = false;
    }
    template<bool fused, class Target>
    void update(float time, Power power, Target& target) {
        if (finished) return;
        float radiusTime = !fadeIn && easeOut ? power(time, 0.5f) : time;
        target.radius(tween<fused>(fromRadius, toRadius, radiusTime));
        if (!fadeIn) target.opacity(tween<fused>(255.f, 0.f, radiusTime));
        else {
            int phase = time < 0.5f ? 0 : 1;
            if (phase == 1 && opacityPhase != 1)
                target.opacity(tween<fused>(0.f, 255.f, 1.f));
            else if (phase == 0 && opacityPhase == 1)
                target.opacity(tween<fused>(255.f, 0.f, 0.f));
            float local = phase ? (time - 0.5f) / 0.5f : time / 0.5f;
            target.opacity(phase ? tween<fused>(255.f, 0.f, local)
                                 : tween<fused>(0.f, 255.f, local));
            opacityPhase = phase;
        }
        if (time >= 1.f) {
            finished = true;
            target.remove();
        }
    }
};
}
