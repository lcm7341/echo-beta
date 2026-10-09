#pragma once
#ifdef GEODE_IS_WINDOWS
#include <cstdint>
class PlayerObject;
namespace cocos2d { class CCSprite; }
namespace echo::dash {
// Check after all mods have loaded, once per displayed frame; restore patches on conflicts.
void prepare();
// True when native stopDashing would build this player's end effect and Echo can take it over.
bool endEffectSkippable(PlayerObject* player);
// How native stopDashing treats its end effect (two sprites and their actions).
// Every player and physics write in stopDashing runs in all modes.
enum class EndEffect : std::uint8_t {
    Native = 0,        // the game builds it
    Skip = 1,          // nothing is built
    Record = 2,        // the game builds it and Echo records a copy (comparison only)
    RecordOnly = 3,    // Echo records it and draws it on the GPU; the game builds nothing
};
void setEndEffect(EndEffect mode);
// Ready to record and draw end effects on the GPU. Creates GL resources on first use.
bool gpuEffectsReady();
// GPU effects can share CPU state for consecutive bit-identical effects.
bool mergeReady();
// Whether this dash end effect should go to the GPU path: only when it is ready, the GL renderer
// is real hardware, and effects come often enough that sprites cost more (dash_effects.cpp).
bool preferGpu();
// While set, a recorded effect may join the previous identical record (above 240 TPS).
void setMerge(bool value);
// Each stopDashing call owns its own pairing; an old GPU-only record is never a native twin.
void resetPendingPair();
// Pairs the native sprites just built with the record made for them (comparison only).
void pairNative(cocos2d::CCSprite* root, cocos2d::CCSprite* outline);
}
// Called by the stopDashing cave where the game would build the end effect.
extern "C" void echoRecordDashEnd(PlayerObject* player) noexcept;
// Comparison switches and counters, set and read by external diagnostics.
extern volatile bool echoDashCompare;
extern volatile bool echoDashShowGpu;
extern volatile std::uint64_t echoDashCompared, echoDashMismatches, echoDashLifeMismatches;
#endif
