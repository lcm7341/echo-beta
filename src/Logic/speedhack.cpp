// playback & everything around an attempt
//
// play_macro runs at the start of every physics step: the inputs stored for that step go into gds button queue,
//   so gd applies them exactly like a real press
// resets: held buttons, the queue, the seed, click counters & labels all go back to a known state. a key still down from
//   before the reset would fire again & look like a new press, so it is swallowed until its really let go
// holds across death & respawn are remembered & put back silently. no new press = no jump & no flipped gravity
// practice checkpoints, maintain / same gravity, follow player & the seed handling are in here too
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"

void Logic::sync_time_scale() {
    auto* scheduler = CCDirector::sharedDirector()->getScheduler();
    if (!scheduler)
        return;
    auto const& logic = Logic::get();
    float speed = std::max(logic.speedhack, 0.01f);
    if (!std::isfinite(speed) || speed < 0.01f) {
        speed = 1.f;
    }
    if (auto* play = PlayLayer::get(); play && play->m_playerDied) {
        scheduler->setTimeScale(speed);
        return;
    }
    if (PlayLayer::get() || logic.editor_testing)
        scheduler->setTimeScale(1.f);
    else
        scheduler->setTimeScale(speed);
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
