#pragma once

class PlayerObject;

namespace echo::rotation {
    float gameplay(PlayerObject* player);
    float visual(PlayerObject* player);
    void restore(PlayerObject* player, float gameplay, float visual);
}
