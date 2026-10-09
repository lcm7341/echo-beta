#pragma once
#include <cstdint>

namespace echo::dash {
// Windows CCScaleTo, stackable CCMoveBy and CCFadeTo arithmetic, in native
// action order. The sequence starts its scale action on its first update.
struct Timeline {
    float endX{}, endY{}, startX{}, startY{}, deltaX{}, deltaY{};
    float startPX{}, startPY{}, previousX{}, previousY{};
    std::uint8_t opacity{};
    bool scaleStarted = false, removed = false;

    template<class Target> void start(Target& target, float x, float y) {
        endX = x; endY = y;
        startPX = previousX = target.positionX();
        startPY = previousY = target.positionY();
        opacity = target.opacity();
        scaleStarted = removed = false;
    }
    template<class Target> void update(float time, Target& target) {
        if (removed) return;
        if (!scaleStarted) {
            startX = target.scaleX(); startY = target.scaleY();
            deltaX = endX - startX; deltaY = endY - startY;
            scaleStarted = true;
        }
        target.scale(startX + deltaX * time, startY + deltaY * time);
        // The native sequence's callback cleans up the other root actions
        // before the action manager can update move/fade on this last tick.
        if (time >= 1.f) { removed = true; target.remove(); return; }
        float differenceX = target.positionX() - previousX;
        float differenceY = target.positionY() - previousY;
        startPX = startPX + differenceX;
        startPY = startPY + differenceY;
        float x = startPX + -20.f * time;
        float y = startPY + 0.f * time;
        target.position(x, y);
        previousX = x; previousY = y;
        target.opacity(static_cast<std::uint8_t>(opacity + (0.f - opacity) * time));
    }
};
}
