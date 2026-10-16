#pragma once

class PlayerObject;

float gameplay_rotation(PlayerObject* player);
float visual_rotation(PlayerObject* player);
void restore_rotation(PlayerObject* player, float gameplay, float visual);
