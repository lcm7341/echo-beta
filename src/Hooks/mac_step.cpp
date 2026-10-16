#include <Geode/Geode.hpp>
#ifdef GEODE_IS_MACOS
#include "Hooks/step.hpp"
#include "Logic/logic.hpp"
#include "Logic/mac_refresh_clock.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cstddef>
#include <cstdint>

using namespace geode::prelude;

// mac inlines processCommands into update so hooking it does NOTHING
// a step is found from the calls gd makes around it:
//   updateSpawnTriggers (on the real effect manager, not the forecast one) arms the layer
//   applyTimeWarp comes right after -> the last step ends & a new one begins
//   p1's resetTouchedRings is the last thing in a step -> the step ends
//   update returning ends whatever is still open
// step_begin / step_end are the same calls the processCommands hook makes on the other platforms
// a step that shouldnt run (refresh only, predicting) gets its processQueuedButtons skipped
// HALF_TICK_OFFSET is a raw GJBaseGameLayer field, 2.2081
const std::ptrdiff_t HALF_TICK_OFFSET = 0x3568;

struct NativeStep {
    int update_depth = 0, apply_depth = 0;
    GJBaseGameLayer* armed = nullptr;
    GJBaseGameLayer* open = nullptr;
    GJBaseGameLayer* layer = nullptr;
    bool ran = false;
    bool half_tick = false;
};

static NativeStep g_step;

static void begin_step(GJBaseGameLayer* self) {
    auto& logic = Logic::get();
    g_step.open = self;
    g_step.half_tick = *(reinterpret_cast<const unsigned char*>(self) + HALF_TICK_OFFSET) != 0;
    g_step.ran = !logic.refreshing_visuals && !logic.predicting;
    g_step.layer = nullptr;
    if (!g_step.ran) return;
    g_step.layer = step_begin(self);
    logic.in_step = true;
}

static void end_step() {
    auto* self = g_step.open;
    g_step.open = nullptr;
    if (!self || !g_step.ran) return;
    Logic::get().in_step = false;
    step_end(self, g_step.layer, g_step.half_tick);
}

static void after_apply_time_warp(GJBaseGameLayer* self) {
    if (--g_step.apply_depth > 0 || g_step.armed != self) return;
    g_step.armed = nullptr;
    end_step();
    begin_step(self);
}

void suggest_sdl_pacing() {
    static bool checked = false;
    if (checked) return;
    checked = true;
    auto* sdl = Loader::get()->getLoadedMod("zmx.sdl");
    if (!sdl || window_max_refresh() <= 61.0) return;
    if (!sdl->hasSetting("disable-vsync") || !sdl->hasSetting("uncap-framerate")) return;
    if (sdl->getSettingValue<bool>("disable-vsync") && sdl->getSettingValue<bool>("uncap-framerate"))
        return;
    Logic::get().notify("Turn on the SDL Mod's \"Disable VSync\" and \"Disable Framerate Cap\" for better performance.", true);
}

class $modify(EchoMacStepLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::Last);
        (void)self.setHookPriority("GJBaseGameLayer::processQueuedButtons", Priority::First);
    }

    void update(float dt) {
        ++g_step.update_depth;
        GJBaseGameLayer::update(dt);
        if (--g_step.update_depth > 0) return;
        end_step();
        g_step.armed = nullptr;
        g_step.apply_depth = 0;
    }

    void applyTimeWarp(float timeWarp) {
        ++g_step.apply_depth;
        GJBaseGameLayer::applyTimeWarp(timeWarp);
        after_apply_time_warp(this);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        if (g_step.open == this && !g_step.ran) return;
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }
};

class $modify(EchoMacStepEffects, GJEffectManager) {
    void updateSpawnTriggers(float dt) {
        GJEffectManager::updateSpawnTriggers(dt);
        if (g_step.update_depth == 0) return;
        // only the real layer NOT the forecast one
        auto* layer = GJBaseGameLayer::get();
        if (layer && layer->m_effectManager == this) g_step.armed = layer;
    }
};

class $modify(EchoMacStepPlayer, PlayerObject) {
    void resetTouchedRings(bool removeAll) {
        if (g_step.open && g_step.update_depth > 0 && this == g_step.open->m_player1) end_step();
        PlayerObject::resetTouchedRings(removeAll);
    }
};
#endif
