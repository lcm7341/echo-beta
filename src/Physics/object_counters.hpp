#pragma once
#include <cstdint>

// GD state that object creation consumes (src/Physics/object_counters.cpp).
namespace echo::objects {
// Next GameObject unique ID (GameObject::commonSetup), or null if the code is not the known build.
int* uniqueIdCounter();
// Generator for GameObject variance indexes (commonSetup and resetObject), or null.
std::uint64_t* varianceGenerator();
// Puts the variance generator at a fixed state, so every object gets the same variance index on
// every level load and reset.
void reseedVariance();
}
