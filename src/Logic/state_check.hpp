#pragma once

#include <cstdint>
#include <string>

class GJGameState;

// a backstep save had a broken map & gd crashed copying it
// ts checks first
std::string broken_state(const GJGameState& state);
