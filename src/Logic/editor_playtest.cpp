// playback & everything around an attempt
//
// play_macro runs at the start of every physics step: the inputs stored for that step go into gds button queue,
//   so gd applies them exactly like a real press
// resets: held buttons, the queue, the seed, click counters & labels all go back to a known state. a key still down from
//   before the reset would fire again & look like a new press, so it is swallowed until its really let go
#include "Logic/logic.hpp"
#include "Hooks/step.hpp"

void Logic::begin_editor_playtest() {
    // TODO
}

void Logic::end_editor_playtest(GJBaseGameLayer* editor) {
    // TODO
}
