// playback & everything around an attempt
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"

void Logic::sync_time_scale() {
    // TODO
}

void Logic::sync_playback_fps() {
    auto& logic = Logic::get();
    auto* director = CCDirector::sharedDirector();
    if (logic.playback_fps >= 1.f) {
        if (!logic.playback_fps_held) {
            logic.held_interval = director->getAnimationInterval();
        }
        logic.playback_fps_held = true;
        director->setAnimationInterval(1.f / (logic.playback_fps * logic.speedhack));
        return;
    }
    if (!logic.playback_fps_held)
        return;
    logic.playback_fps_held = false;
    director->setAnimationInterval(logic.held_interval > 0.f ? logic.held_interval : 1.f / 60.f);
}
