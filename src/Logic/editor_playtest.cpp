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

void Logic::begin_editor_playtest() {
    auto& logic = Logic::get();
    logic.editor_testing = true;
    logic.editor_paused = false;
    logic.trail_p1.clear();
    logic.trail_p2.clear();
    logic.step_frame = 0;
    logic.progress_base_set = false;
    logic.carry = 0.f;
    logic.respawn_frames = 0;
    logic.follow_log.clear();
    logic.buttons_prepared = false;
    logic.replay_clicks_left = 0;
    Logic::get().clear_click_hitbox();
    if (logic.is_recording() && !logic.is_playing())
        Logic::get().trim_fresh_attempt(nullptr, logic);
    if (logic.is_playing())
        Logic::get().set_replay_pos(nullptr);
    Logic::get().sync_time_scale();
}

void Logic::end_editor_playtest(GJBaseGameLayer* editor) {
    (void)editor;
    auto& logic = Logic::get();
    logic.editor_testing = false;
    logic.editor_paused = false;
    logic.carry = 0.f;
    logic.steps_left = 0;
    Logic::get().sync_time_scale();
}
