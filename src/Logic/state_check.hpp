#pragma once

class GJGameState;

// a backstep save had a broken map & gd crashed copying it
// ts checks first
bool game_state_intact(const GJGameState& state);
