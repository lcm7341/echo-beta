#pragma once

#include <cstdint>
#include <string>

class GJGameState;

// Safeguard for a backstep crash: a backstep save's GJGameState held a map whose head node had
// been overwritten (root pointer 0x14e4bf), and releasing the save crashed GD's destructor. This
// walks every node-based container in a game state without trusting its pointers.
namespace echo::state_check {
// Empty when the state's containers are intact, otherwise the first broken member.
std::string broken(GJGameState const& state);
}
