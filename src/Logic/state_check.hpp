#pragma once

#include <cstdint>
#include <string>

class GJGameState;

// a backstep save had a broken map & gd crashed copying it
// ts checks first
namespace echo::state_check {
	std::string broken(GJGameState const& state);
}
