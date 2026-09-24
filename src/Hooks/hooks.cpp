// where echo hooks the game
//
// PlayLayer: level start, reset, completion, pause & checkpoints. reset is where the seed goes in & the macro rewinds
// GJBaseGameLayer: update (handed to run_frame), processCommands (1 physics step, echo::step::begin / end wrap it), handleButton (recording)
// PlayerObject: buttons, death, rotation, trail & particles
// most trajectory hooks do NOTHING unless a prediction is running
// visual tilt: ship / ufo / wave / swing turn too little per step at high tps to look right, so a separate visual angle is kept & drawn.
//   physics & hitboxes still use the real one
// the step count patches & silicates reset patches are at the bottom
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
#include "Hooks/step.hpp"
#include "GUI/gui.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/collisions.hpp"
#include "Physics/gjbasegamelayer.hpp"
#include "Physics/gravity.hpp"
#include "Physics/player.hpp"
#include "Physics/prediction_art.hpp"
#include <unordered_set>
#include <Geode/binding/PauseLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameToolbox.hpp>
#include <Geode/modify/UILayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <vector>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PointNode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <cmath>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RandTriggerGameObject.hpp>
#include <Geode/binding/RandTriggerGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/Geode.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <safetyhook.hpp>
#include <windows.h>
#include "Logic/input.hpp"
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/binding/PointNode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <imgui-cocos.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/AppDelegate.hpp>

namespace chrono = std::chrono;

using namespace geode::prelude;

static bool g_nativeReset = false;
static bool g_seedApplied = false;
void apply_lvl_seed();

namespace {
    std::vector<Hook*>& predict_hooks() {
        static std::vector<Hook*> hooks;
        return hooks;
    }

    bool predict_hooks_on = false;

    void hold_for_predict(auto& self, char const* name) {
        auto hook = self.getHook(name);
        if (!hook) {
            return;
        }
        hook.unwrap()->setAutoEnable(false);
        predict_hooks().push_back(hook.unwrap());
    }
}

void Logic::sync_predict_hooks() {
    auto const& logic = Logic::get();
    bool want = logic.show_traj || logic.pause_before_kill || logic.traj_autoclicker || logic.show_death_next || logic.predicting;
    if (want == predict_hooks_on)
        return;
    for (auto* hook : predict_hooks()) {
        auto res = want ? hook->enable() : hook->disable();
    }
    predict_hooks_on = want;
}

namespace {
    constexpr int DASH_SPIN_TAG = 12;
    bool in_orb_button = false;
    // the swifts own release & press. these must NOT start another one
    bool orb_swift = false;

    class TickActionManager : public CCActionManager {
    public:
        void tick(float dt) {
            this->update(dt);
        }
    };

    TickActionManager* g_dashSpin = nullptr;
    bool g_tickPending = true;
    int g_halfPasses = 0;

    bool dash_spin_enabled() {
        auto& logic = Logic::get();
        return !logic.predicting && !logic.defer_physics();
    }

    float dash_tick_delta() {
        float tps = static_cast<float>(Logic::get().sample_rate());
        if (!std::isfinite(tps) || tps <= 0.f)
            tps = 240.f;
        float warp = 1.f;
        if (auto* layer = GJBaseGameLayer::get())
            warp = layer->m_gameState.m_timeWarp;
        if (!std::isfinite(warp))
            warp = 1.f;
        return 1.f / tps / std::max(warp, 1.f);
    }

    TickActionManager* dash_spin_manager() {
        if (!g_dashSpin)
            g_dashSpin = new TickActionManager();
        return g_dashSpin;
    }

    void dash_spin_pre() {
        if (!g_tickPending)
            return;
        g_tickPending = false;
        if (g_dashSpin && dash_spin_enabled())
            g_dashSpin->tick(dash_tick_delta());
    }

    void dash_spin_post(bool isHalfTick) {
        if (!isHalfTick) {
            g_halfPasses = 0;
            g_tickPending = true;
            return;
        }
        if (++g_halfPasses >= 2) {
            g_halfPasses = 0;
            g_tickPending = true;
        }
    }

    void dash_spin_reset() {
        g_tickPending = true;
        g_halfPasses = 0;
    }

    bool can_touch(void const* pointer, size_t bytes, bool write) {
        if (!pointer || bytes == 0)
            return false;
        auto address = reinterpret_cast<uintptr_t>(pointer);
        if ((address & 7) != 0 || address < 0x10000 || address > 0x00007fffffffffffull) // NOT a real pointer
            return false;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(pointer, &info, sizeof(info)) == 0)
            return false;
        if (info.State != MEM_COMMIT)
            return false;
        if ((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        auto start = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (address < start || address + bytes > start + info.RegionSize)
            return false;
        if (!write)
            return true;
        auto protect = info.Protect & 0xff;
        return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    }

    bool heap_block_ok(void* begin, void* cap) {
        if (!begin)
            return true;
        if (!can_touch(begin, sizeof(void*), false))
            return false;
        auto start = reinterpret_cast<uintptr_t>(begin);
        auto stop = reinterpret_cast<uintptr_t>(cap);
        if (stop < start)
            return false;
        auto bytes = (stop - start) & ~uintptr_t{ 7 };
        if (bytes >= 0x40000000ull)
            return false;
        if (bytes < 0x1000)
            return true;
        auto* header_slot = reinterpret_cast<uintptr_t*>(start - 8);
        if (!can_touch(header_slot, sizeof(uintptr_t), false))
            return false;
        auto header = *header_slot;
        if (header == 0 || (header & 7) != 0 || header >= start)
            return false;
        auto gap = start - header;
        return gap >= 8 && gap - 8 <= 0x1f;
    }

    void zero_vec(SectionVec* vec) {
        vec->begin = nullptr;
        vec->end = nullptr;
        vec->cap = nullptr;
    }

    ptrdiff_t vec_count(SectionVec* vec) {
        if (!vec->begin || !vec->cap || vec->end < vec->begin || vec->cap < vec->begin)
            return 0;
        auto capacity = vec->cap - vec->begin;
        auto amt = vec->end - vec->begin;
        if (amt > capacity)
            amt = capacity;
        if (amt > 0x100000)
            return -1;
        return amt;
    }

    void drop_grid(SectionVec* outer) {
        if (!can_touch(outer, sizeof(SectionVec), true))
            return;
        if (!outer->begin || !heap_block_ok(outer->begin, outer->cap) || !can_touch(outer->begin, sizeof(uintptr_t), true)) {
            zero_vec(outer);
            return;
        }
        auto columns = vec_count(outer);
        if (columns < 0) {
            zero_vec(outer);
            return;
        }
        for (ptrdiff_t column_index = 0; column_index < columns; ++column_index) {
            auto* column = reinterpret_cast<SectionVec*>(outer->begin[column_index]);
            if (!column)
                continue;
            if (!can_touch(column, sizeof(SectionVec), true) || !column->begin || !heap_block_ok(column->begin, column->cap) || !can_touch(column->begin, sizeof(uintptr_t), true)) {
                outer->begin[column_index] = 0;
                continue;
            }
            auto rows = vec_count(column);
            if (rows < 0) {
                outer->begin[column_index] = 0;
                continue;
            }
            for (ptrdiff_t row_index = 0; row_index < rows; ++row_index) {
                auto* bucket = reinterpret_cast<SectionVec*>(column->begin[row_index]);
                if (!bucket)
                    continue;
                if (!can_touch(bucket, sizeof(SectionVec), true) || !heap_block_ok(bucket->begin, bucket->cap)) {
                    column->begin[row_index] = 0;
                }
            }
        }
    }

    void drop_broken_sections(GJBaseGameLayer* layer) {
        auto* base = reinterpret_cast<uint8_t*>(layer);
        drop_grid(reinterpret_cast<SectionVec*>(base + 0x3598)); // the 3 grids ~GJBaseGameLayer frees
        drop_grid(reinterpret_cast<SectionVec*>(base + 0x35b0));
        drop_grid(reinterpret_cast<SectionVec*>(base + 0x35c8));
    }
}

static bool menu_holds_mouse() {
    auto& imgui = ImGuiCocos::get();
    if (!imgui.isInitialized() || !imgui.isVisible())
        return false;
    return ImGui::GetIO().WantCaptureMouse || ImGui::IsAnyItemActive();
}

static void keep_menu_cursor() {
    if (!menu_holds_mouse())
        return;
    auto* view = CCEGLView::get();
    if (!view)
        return;
    if (view->m_bCursorLocked)
        view->toggleLockCursor(false);
    if (!view->m_pMainWindow)
        return;
    auto hwnd = *reinterpret_cast<HWND*>(reinterpret_cast<uintptr_t>(view->m_pMainWindow) + 0x370); // tf
    POINT point;
    if (!hwnd || !GetCursorPos(&point) || !ScreenToClient(hwnd, &point))
        return;
    float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
    if (view->m_fFrameZoomFactor != 0.f) {
        x /= view->m_fFrameZoomFactor;
        y /= view->m_fFrameZoomFactor;
    }
    if (view->m_bInRetinaMonitor && view->m_nRetinaFactor == 1) {
        x *= 2.f;
        y *= 2.f;
    }
    view->m_fMouseX = x;
    view->m_fMouseY = y;
}

// visual tilt for ship ufo wave swing
// physics uses the real rotation NOT ts
std::unordered_map<PlayerObject*, float>& tilt_fix() {
    static std::unordered_map<PlayerObject*, float> shown;
    return shown;
}

std::unordered_map<PlayerObject*, float>& tilt_held() {
    static std::unordered_map<PlayerObject*, float> held;
    return held;
}

std::unordered_map<PlayerObject*, float>& tilt_left() {
    static std::unordered_map<PlayerObject*, float> left;
    return left;
}

bool tilt_mode(PlayerObject* player) {
    return player->m_isShip || player->m_isBird || player->m_isDart || player->m_isSwing;
}

bool tilt_applies(PlayerObject* player) {
    return Logic::get().fix_tps_rotation && tilt_mode(player) && !Trajectory::get().isFake(player);
}

float visual_tilt(PlayerObject* player) {
    float now = player->getRotation();
    auto left = tilt_left().find(player);
    auto shown = tilt_fix().find(player);
    bool fresh = left == tilt_left().end() || left->second != now || shown == tilt_fix().end();
    return fresh ? now : shown->second;
}

template <class Gameplay, class Visual>
void turn_both(PlayerObject* player, Gameplay&& gameplay_turn, Visual&& visual_turn) {
    float visual = visual_tilt(player);
    gameplay_turn();
    float gameplay = player->getRotation();
    player->setRotation(visual);
    visual_turn();
    tilt_fix()[player] = player->getRotation();
    tilt_left()[player] = gameplay;
    player->setRotation(gameplay);
}

void restore_tilt(PlayerObject* player) {
    if (!player)
        return;
    auto held = tilt_held().find(player);
    if (held == tilt_held().end())
        return;
    player->setRotation(held->second);
    tilt_held().erase(held);
}

void present_tilt(PlayerObject* player) {
    if (!player || !tilt_applies(player) || !tilt_fix().contains(player))
        return;
    float visual = visual_tilt(player);
    tilt_held()[player] = player->getRotation();
    player->setRotation(visual);
}

void clear_tilt() {
    tilt_fix().clear();
    tilt_held().clear();
    tilt_left().clear();
}

namespace echo::rotation {
    float gameplay(PlayerObject* player) {
        if (!player)
            return 0.f;
        auto held = tilt_held().find(player);
        return held == tilt_held().end() ? player->getRotation() : held->second;
    }

    float visual(PlayerObject* player) {
        if (!player)
            return 0.f;
        return tilt_applies(player) ? visual_tilt(player) : gameplay(player);
    }

    void restore(PlayerObject* player, float gameplay_angle, float visual_angle) {
        if (!player)
            return;
        player->setRotation(gameplay_angle);
        if (tilt_applies(player)) {
            tilt_fix()[player] = visual_angle;
            tilt_left()[player] = gameplay_angle;
        }
    }
}

void check_physics_step_patch();
void echo_guard_step_site();

class $modify(EchoPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        check_physics_step_patch();
        // the macro says if ldm was on. has to be set BEFORE the level builds its objects
        if (auto& macro = Logic::get(); level && macro.macro_ldm_known && !macro.inputs.empty() && level->m_lowDetailModeToggled != macro.macro_ldm) {
            level->m_lowDetailModeToggled = macro.macro_ldm;
            Notification::create(macro.macro_ldm ? "Low Detail Mode turned on for this macro" : "Low Detail Mode turned off for this macro", NotificationIcon::Success)->show();
        }
        Logic::get().clear_rand_triggers();
        clear_tilt();
        if (Trajectory::get().attached())
            Trajectory::get().detach();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        if (Logic::get().practice_music_hack)
            m_practiceMusicSync = true;
        Trajectory::get().attach(this);
        Logic::get().on_play_init(this);
        phys::prepare_prediction_art();
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        Logic::get().after_frame(this, dt);
    }

    // TODO ts rebuilds the label every frame

    void updateVisibility(float dt) {
        PlayLayer::updateVisibility(dt);
    }

    void addObject(GameObject* object) {
        PlayLayer::addObject(object);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (Logic::get().render_queued) {
            Logic::get().render_queued = false;
        }
    }

    void updateColor(ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, ccHSVValue& copyHSV, int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
        PlayLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
    }

    void resetLevel() {
        restore_tilt(m_player1);
        restore_tilt(m_player2);
        clear_tilt();
        auto& logic = Logic::get();
        if (!logic.backstepping) {
            Logic::get().stop_best_frame();
        }
        logic.carry = 0.f;
        logic.respawn_frames = 0;
        logic.resetting = true;
        logic.restored_checkpoint = nullptr;
        Logic::get().sync_checkpoints(this);
        Logic::get().capture_restart_holds(this);
        Logic::get().keep_lvl_seed = Logic::get().get_latest_checkpoint(this);
        bool roll_seed = !Logic::get().keep_lvl_seed && !logic.seed_override && !logic.is_playing();
        if (!roll_seed)
            Logic::get().plant_lvl_seed(this);
        g_nativeReset = true;
        g_seedApplied = false;
        PlayLayer::resetLevel();
        g_nativeReset = false;
        if (Logic::get().keep_lvl_seed)
            Logic::get().restore_checkpoint_random(this);
        else if (!roll_seed)
            Logic::get().plant_lvl_seed(this);
        Logic::get().keep_lvl_seed = false;
        logic.resetting = false;
        logic.present = true;
        Logic::get().onReset(this);
        dash_spin_reset();
        Logic::get().cancel_steps();
        Logic::get().sync_time_scale();
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        Logic::get().restored_checkpoint = checkpoint;
        PlayLayer::loadFromCheckpoint(checkpoint);
        Logic::get().restore_checkpoint_random(this);
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        restore_tilt(m_player1);
        restore_tilt(m_player2);
        PlayLayer::storeCheckpoint(checkpoint);
        Logic::get().sync_checkpoints(this);
        if (checkpoint && m_checkpointArray && m_checkpointArray->containsObject(checkpoint))
            Logic::get().save_checkpoint(this, checkpoint);
    }

    void removeCheckpoint(bool first) {
        PlayLayer::removeCheckpoint(first);
        Logic::get().sync_checkpoints(this);
    }

    void removeAllCheckpoints() {
        PlayLayer::removeAllCheckpoints();
        Logic::get().sync_checkpoints(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        if (Trajectory::get().mark_dead(player) || m_player1->m_isDead || m_player2->m_isDead)
            return;
        if (Logic::get().should_noclip(this, player)) {
            if (player == m_player1)
                Logic::get().noclip_dead_p1 = true;
            else if (player == m_player2)
                Logic::get().noclip_dead_p2 = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        if (m_playerDied)
            Logic::get().sync_time_scale();
    }

    void delayedResetLevel() {
        PlayLayer::delayedResetLevel();
        m_resumeTimer = 0;
        Logic::get().clear_stuck_death(this);
    }

    void fullReset() {
        PlayLayer::fullReset();
        m_resumeTimer = 0;
        Logic::get().clear_stuck_death(this);
    }

    void resetLevelFromStart() {
        PlayLayer::resetLevelFromStart();
        m_resumeTimer = 0;
        Logic::get().clear_stuck_death(this);
    }

    void showNewBest(bool newReward, int orbs, int diamonds, bool demonKey, bool noRetry, bool noTitle) {
        auto& logic = Logic::get();
        if (logic.predicting || logic.showed_new_best)
            return;
        logic.showed_new_best = true;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }

    void checkForEnd() {
        if (Logic::get().predicting)
            return;
        PlayLayer::checkForEnd();
    }

    void levelComplete() {
        if (Logic::get().predicting)
            return;
        Logic::get().on_complete(this);
        PlayLayer::levelComplete();
    }

    void showCompleteEffect() {
        PlayLayer::showCompleteEffect();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (Logic::get().predicting)
            return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void playPlatformerEndAnimationToPos(CCPoint position, bool instant) {
        if (Logic::get().predicting)
            return;
        PlayLayer::playPlatformerEndAnimationToPos(position, instant);
    }

    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
        if (unfocused)
            Logic::get().on_focus_change();
    }

    void resume() {
        PlayLayer::resume();
        Logic::get().resume_attempt(this);
    }

    void resumeAndRestart(bool fromStart) {
        PlayLayer::resumeAndRestart(fromStart);
        Logic::get().resume_attempt(this);
    }

    void onQuit() {
        restore_tilt(m_player1);
        restore_tilt(m_player2);
        clear_tilt();
        Logic::get().onQuit(this);
        Trajectory::get().detach();
        PlayLayer::onQuit();
    }
};

class $modify(EchoEditor, LevelEditorLayer) {
    void onPlaytest() {
        Logic::get().begin_editor_playtest();
        LevelEditorLayer::onPlaytest();
    }

    void onStopPlaytest() {
        LevelEditorLayer::onStopPlaytest();
        Logic::get().end_editor_playtest(this);
    }

    void updateEditor(float dt) {
        LevelEditorLayer::updateEditor(dt);
        if (Logic::get().show_hitboxes && !Logic::get().editor_testing) {
        }
    }

    void postUpdate(float dt) {
        LevelEditorLayer::postUpdate(dt);
        if (Logic::get().hide_p1 && m_player1)
            m_player1->setVisible(false);
        if (Logic::get().hide_p2 && m_player2)
            m_player2->setVisible(false);
    }

    void updateDebugDraw() {
        LevelEditorLayer::updateDebugDraw();
    }
};

class $modify(EchoPauseLayer, PauseLayer) {
    void onRestart(CCObject* sender) {
        PauseLayer::onRestart(sender);
        if (auto* play = PlayLayer::get())
            Logic::get().resume_attempt(play);
    }

    void onRestartFull(CCObject* sender) {
        PauseLayer::onRestartFull(sender);
        if (auto* play = PlayLayer::get())
            Logic::get().resume_attempt(play);
    }
};

int vanilla_physics_steps(float dt, float timeWarp);

namespace echo::step {
    GJBaseGameLayer* begin(GJBaseGameLayer* self) {
        auto& logic = Logic::get();
        dash_spin_pre();
        auto* play = Logic::as_play(self);
        auto* editor = Logic::as_editor(self);
        GJBaseGameLayer* layer = play;
        if (!layer && editor && logic.editor_testing)
            layer = editor;
        if (layer && !logic.predicting)
            Logic::get().note_step();
        if (play && !logic.predicting) {
            Logic::get().keep_variance(play);
            logic.noclip_dead_p1 = false;
            logic.noclip_dead_p2 = false;
        }
        if (self->m_resumeTimer > 0 && self->m_resumeTimer <= 8)
            self->m_resumeTimer = 0;
        if (play && !self->m_playerDied && self->m_resumeTimer <= 0) {
        }
        if (layer && !self->m_playerDied && self->m_resumeTimer <= 0)
            Logic::get().run_follow(layer);
        return layer;
    }

    void end(GJBaseGameLayer* self, GJBaseGameLayer* layer, bool isHalfTick) {
        auto& logic = Logic::get();
        auto* play = Logic::as_play(self);

        if (!layer) {
            dash_spin_post(isHalfTick);
            return;
        }
        if (self->m_resumeTimer > 0) {
            dash_spin_post(isHalfTick);
            return;
        }

        if (!self->m_playerDied) {
            logic.step_frame++;
            if (play && !logic.progress_base_set) {
                logic.progress_base = static_cast<int>(self->m_gameState.m_currentProgress) - logic.step_frame;
                logic.progress_base_set = true;
            }
            Logic::get().apply_rate(logic.step_frame);
        }
        if (play)
            trace(self);
        if (logic.ignore_record_frames > 0)
            --logic.ignore_record_frames;

        logic.buttons_prepared = false;
        if (play)
            Logic::get().tick_autoclicker(play);
        dash_spin_post(isHalfTick);
    }
}

class $modify(EchoGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        hold_for_predict(self, "GJBaseGameLayer::gameEventTriggered");
        hold_for_predict(self, "GJBaseGameLayer::updateObjectSection");
        hold_for_predict(self, "GJBaseGameLayer::collisionCheckObjects");
        hold_for_predict(self, "GJBaseGameLayer::flipGravity");
        hold_for_predict(self, "GJBaseGameLayer::teleportPlayer");
        hold_for_predict(self, "GJBaseGameLayer::destroyObject");
        hold_for_predict(self, "GJBaseGameLayer::pickupItem");
        (void)self.setHookPriority("GJBaseGameLayer::update", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::GJBaseGameLayer", Priority::Last);
        (void)self.setHookPriorityAfter("GJBaseGameLayer::GJBaseGameLayer", "smjs.object-collab");
    }

    void destructor() {
        drop_broken_sections(this);
        reinterpret_cast<void (*)(GJBaseGameLayer*)>(geode::base::get() + 0x2057a0)(this); // the real ~GJBaseGameLayer
    }

    void update(float dt) {
        auto& logic = Logic::get();
        if (logic.skip_physics)
            return;
        // death HAS to reach the original or auto retry freezes
        if (auto* play = Logic::as_play(this); play && play->m_playerDied) {
            logic.tick_steps = static_cast<uint32_t>(vanilla_physics_steps(dt, m_gameState.m_timeWarp));
            GJBaseGameLayer::update(dt);
            return;
        }
        auto* play = Logic::as_play(this);
        auto* editor = Logic::as_editor(this);
        GJBaseGameLayer* layer = play;
        if (!layer && editor && logic.editor_testing)
            layer = editor;
        bool locked = Logic::get().lock_delta_active();
        if (logic.refresh_only || !layer || Logic::get().defer_physics()) {
            logic.tick_steps = static_cast<uint32_t>(vanilla_physics_steps(dt, m_gameState.m_timeWarp));
            GJBaseGameLayer::update(dt);
            return;
        }
        if (logic.disable_shakes || logic.layout_mode)
            m_skipCameraShake = true;
        if (!locked && logic.steps_left == 0) {
            logic.tick_steps = static_cast<uint32_t>(std::max(Logic::get().count_unlocked_steps(layer, dt), 0));
            if (logic.tick_steps == 0)
                return;
        }
        else if (!locked) {
            logic.tick_steps = static_cast<uint32_t>(std::max(logic.steps_left, 0));
        }
        else {
            logic.tick_steps = 1;
        }
        GJBaseGameLayer::update(dt);
    }

    double getModifiedDelta(float dt) {
        double original = GJBaseGameLayer::getModifiedDelta(dt);
        if (auto* play = Logic::as_play(this); play && play->m_playerDied)
            return original;
        bool gameplay = Logic::as_play(this) || (Logic::get().editor_testing && Logic::as_editor(this));
        if (!gameplay)
            return original;
        if (auto* editor = Logic::as_editor(this); editor && editor->m_playbackActive)
            return original;
        auto const& logic = Logic::get();
        if (logic.refresh_only)
            return 0.0;
        if (Logic::get().defer_physics())
            return original;
        float warp = std::min(m_gameState.m_timeWarp, 1.f);
        float step = logic.physics_step();
        if (!std::isfinite(warp) || warp < 0.f || !std::isfinite(step) || !(step > 0.f))
            return 0.0;
        return static_cast<double>(step) * static_cast<double>(warp) * static_cast<double>(logic.tick_steps);
    }

    // advanced follow can throw stuff to like 1e12 xd
    template <class Point>
    static bool sectionable(Point const& pos) {
        constexpr float LIMIT = 1e7f;
        return std::isfinite(pos.x) && std::isfinite(pos.y) && std::fabs(pos.x) < LIMIT && std::fabs(pos.y) < LIMIT;
    }

    void addToSection(GameObject* object) {
        if (!object)
            return;
        auto const pos = object->getPosition();
        if (!sectionable(pos))
            return;
        if (Logic::get().is_traj_player(object))
            return;
        GJBaseGameLayer::addToSection(object);
    }

    void updateObjectSection(GameObject* object) {
        if (!object)
            return;
        auto const pos = object->getPosition();
        if (!sectionable(pos))
            return;
        if (Logic::get().is_traj_player(object))
            return;
        GJBaseGameLayer::updateObjectSection(object);
    }

    // checkpoint restore moves an object without fixing its section
    // then gd removes it from slot -1 & crashes
    void removeObjectFromSection(GameObject* object) {
        if (!object)
            return;
        auto* raw = reinterpret_cast<uint8_t*>(object);
        auto section_x = reinterpret_cast<int*>(raw + 0x278); // GameObject offsets (windows 2.2081)
        auto section_y = reinterpret_cast<int*>(raw + 0x27c);
        auto slot = reinterpret_cast<int*>(raw + 0x270);
        auto* object_id = reinterpret_cast<int*>(raw + 0x40c);
        auto* block_slot = reinterpret_cast<int*>(raw + 0x274);
        auto forget = [&] {
            *section_x = -1;
            *section_y = -1;
            *slot = -1;
        };
        auto pointer_ok = [](void* pointer) {
            auto bits = reinterpret_cast<uintptr_t>(pointer);
            return bits > 0x10000 && (bits & 7) == 0;
        };
        auto* layer_base = reinterpret_cast<uint8_t*>(this);
        auto* objectType = reinterpret_cast<int*>(raw + 0x3a0);
        bool block = *object_id == 0x718;
        bool has_second = block || *objectType != 7;
        bool moving = raw[0x280] != 0; // gd moves ts one every step
        std::size_t list_field = block ? 0x3600 : 0x35e0, list_count = block ? 0x3618 : 0x35f8;
        std::size_t grid_field = block ? 0x35c8 : 0x35b0, grid_sizes = block ? 0x3670 : 0x3658;
        auto second_list = [&](GameObject***& begin_field, GameObject***& end_field, int*& count) {
            begin_field = nullptr;
            end_field = nullptr;
            count = nullptr;
            if (!has_second)
                return;
            if (moving) {
                begin_field = reinterpret_cast<GameObject***>(layer_base + list_field);
                end_field = reinterpret_cast<GameObject***>(layer_base + list_field + sizeof(void*));
                count = reinterpret_cast<int*>(layer_base + list_count);
                return;
            }
            if (*section_x < 0 || *section_y < 0)
                return;
            auto* columns = *reinterpret_cast<uint8_t***>(layer_base + grid_field);
            auto* columns_end = *reinterpret_cast<uint8_t***>(layer_base + grid_field + sizeof(void*));
            auto* size_columns = *reinterpret_cast<uint8_t***>(layer_base + grid_sizes);
            auto* size_columns_end = *reinterpret_cast<uint8_t***>(layer_base + grid_sizes + sizeof(void*));
            if (!columns || !size_columns || *section_x >= columns_end - columns || *section_x >= size_columns_end - size_columns || !columns[*section_x] || !size_columns[*section_x])
                return;
            auto* column = columns[*section_x];
            auto* rows = *reinterpret_cast<uint8_t***>(column);
            auto* rows_end = *reinterpret_cast<uint8_t***>(column + sizeof(void*));
            auto* size_column = size_columns[*section_x];
            auto* size_rows = *reinterpret_cast<int**>(size_column);
            auto* size_rows_end = *reinterpret_cast<int**>(size_column + sizeof(void*));
            if (!rows || !size_rows || *section_y >= rows_end - rows || *section_y >= size_rows_end - size_rows || !rows[*section_y])
                return;
            auto* bucket = rows[*section_y];
            begin_field = reinterpret_cast<GameObject***>(bucket);
            end_field = reinterpret_cast<GameObject***>(bucket + sizeof(void*));
            count = &size_rows[*section_y];
        };
        auto second_ok = [&] {
            if (!has_second)
                return true;
            GameObject*** begin_field;
            GameObject*** end_field;
            int* amt;
            second_list(begin_field, end_field, amt);
            if (!begin_field || !*begin_field || !amt)
                return false;
            int stored = *end_field > *begin_field ? static_cast<int>(*end_field - *begin_field) : 0;
            int used = *amt;
            return used >= 1 && used <= stored && *block_slot >= 0 && *block_slot < used && (*begin_field)[*block_slot] == object && pointer_ok((*begin_field)[used - 1]);
        };
        if (*object_id != 0x718 && *section_x >= 0 && *section_y >= 0 && *slot >= 0) {
            auto* base = reinterpret_cast<uint8_t*>(this);
            auto* columns = *reinterpret_cast<uint8_t***>(base + 0x3598); // the section grid
            auto* columns_end = *reinterpret_cast<uint8_t***>(base + 0x3598 + sizeof(void*));
            auto* size_columns = *reinterpret_cast<uint8_t***>(base + 0x3640); // how many are in each section
            auto* size_columns_end = *reinterpret_cast<uint8_t***>(base + 0x3640 + sizeof(void*));
            if (columns && size_columns && *section_x < columns_end - columns && *section_x < size_columns_end - size_columns &&
                columns[*section_x] && size_columns[*section_x]) {
                auto* column = columns[*section_x];
                auto* rows = *reinterpret_cast<uint8_t***>(column);
                auto* rows_end = *reinterpret_cast<uint8_t***>(column + sizeof(void*));
                auto* size_column = size_columns[*section_x];
                auto* size_rows = *reinterpret_cast<int**>(size_column);
                auto* size_rows_end = *reinterpret_cast<int**>(size_column + sizeof(void*));
                if (rows && size_rows && *section_y < rows_end - rows && *section_y < size_rows_end - size_rows && rows[*section_y]) {
                    auto* bucket = rows[*section_y];
                    auto* objects = *reinterpret_cast<GameObject***>(bucket);
                    auto* objects_end = *reinterpret_cast<GameObject***>(bucket + sizeof(void*));
                    int stored = objects && objects_end > objects ? static_cast<int>(objects_end - objects) : 0;
                    int used = size_rows[*section_y];
                    if (used >= 1 && used <= stored && *slot < used && objects[*slot] == object && pointer_ok(objects[used - 1]) && second_ok()) {
                        GJBaseGameLayer::removeObjectFromSection(object);
                        return;
                    }
                }
            }
        }
        auto compact = [&](GameObject*** begin_field, GameObject*** end_field, int* count, bool keep) {
            if (!begin_field || !end_field) {
                *block_slot = -1;
                return;
            }
            auto* begin = *begin_field;
            auto* end = *end_field;
            int size = begin && end > begin ? static_cast<int>(end - begin) : 0;
            int used = count ? *count : size;
            if (used < 0)
                used = 0;
            if (used > size)
                used = size;
            int write = 0;
            int it = -1;
            bool kept = false;
            if (begin) {
                for (int i = 0; i < used; ++i) {
                    auto* item = begin[i];
                    if (!pointer_ok(item))
                        continue;
                    if (item == object) {
                        if (!keep || kept)
                            continue;
                        kept = true;
                    }
                    if (write != i)
                        begin[write] = item;
                    auto* idx = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(item) + 0x274);
                    if (*idx != write)
                        *idx = write;
                    if (item == object)
                        it = write;
                    ++write;
                }
            }
            if (begin)
                *end_field = begin + write;
            if (count)
                *count = write;
            *block_slot = it;
        };
        auto* base = reinterpret_cast<uint8_t*>(this);
        auto repair_blocks = [&](bool keep) {
            if (!has_second)
                return;
            GameObject*** begin_field;
            GameObject*** end_field;
            int* amt;
            second_list(begin_field, end_field, amt);
            compact(begin_field, end_field, amt, keep);
        };
        bool main_ok = *section_x >= 0 && *section_y >= 0 && *slot >= 0;
        if (main_ok) {
            auto* columns = *reinterpret_cast<uint8_t***>(base + 0x3598);
            auto* columns_end = *reinterpret_cast<uint8_t***>(base + 0x3598 + sizeof(void*));
            auto* size_columns = *reinterpret_cast<uint8_t***>(base + 0x3640);
            auto* size_columns_end = *reinterpret_cast<uint8_t***>(base + 0x3640 + sizeof(void*));
            main_ok = columns && size_columns && *section_x < columns_end - columns && *section_x < size_columns_end - size_columns && columns[*section_x] && size_columns[*section_x];
            if (main_ok) {
                auto* column = columns[*section_x];
                auto* rows = *reinterpret_cast<uint8_t***>(column);
                auto* rows_end = *reinterpret_cast<uint8_t***>(column + sizeof(void*));
                auto* size_column = size_columns[*section_x];
                auto* size_rows = *reinterpret_cast<int**>(size_column);
                auto* size_rows_end = *reinterpret_cast<int**>(size_column + sizeof(void*));
                main_ok = rows && size_rows && *section_y < rows_end - rows && *section_y < size_rows_end - size_rows && rows[*section_y];
                if (main_ok) {
                    auto* bucket = rows[*section_y];
                    auto* objects = *reinterpret_cast<GameObject***>(bucket);
                    auto* objects_end = *reinterpret_cast<GameObject***>(bucket + sizeof(void*));
                    auto* amt = &size_rows[*section_y];
                    int stored = objects && objects_end > objects ? static_cast<int>(objects_end - objects) : 0;
                    int used = *amt;
                    if (used < 0)
                        used = 0;
                    if (used > stored)
                        used = stored;
                    int write = 0;
                    for (int i = 0; i < used; ++i) {
                        auto* item = objects[i];
                        if (!pointer_ok(item))
                            continue;
                        if (write != i)
                            objects[write] = item;
                        auto* idx = reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(item) + 0x270);
                        if (*idx != write)
                            *idx = write;
                        ++write;
                    }
                    *amt = write;
                    main_ok = write > 0 && *slot >= 0 && *slot < write && objects[*slot] == object && pointer_ok(objects[write - 1]);
                }
            }
        }
        repair_blocks(main_ok);
        if (!main_ok) {
            forget();
            return;
        }
        int saved_id = *object_id, saved_type = *objectType;
        bool spoof = has_second && *block_slot < 0;
        if (spoof) {
            *object_id = 0;
            *objectType = 7;
        }
        GJBaseGameLayer::removeObjectFromSection(object);
        if (spoof) {
            *object_id = saved_id;
            *objectType = saved_type;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& logic = Logic::get();
        if (logic.refresh_only)
            return;
        if (logic.predicting)
            return;
        auto* layer = echo::step::begin(this);
        logic.in_step = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        logic.in_step = false;
        echo::step::end(this, layer, isHalfTick);
    }

    void resetLevelVariables() {
        GJBaseGameLayer::resetLevelVariables();
        m_queuedButtons.clear();
        if (Logic::get().editor_testing && Logic::as_editor(this)) {
            auto& logic = Logic::get();
            logic.step_frame = 0;
            logic.carry = 0.f;
            logic.buttons_prepared = false;
            logic.follow_log.clear();
            if (logic.is_playing())
                Logic::get().set_replay_pos(nullptr);
        }
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int length, float dt) {
        if (!Trajectory::get().isFake(player)) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, length, dt);
            return;
        }
        phys::collisionCheckObjects(this, player, objects, length, dt);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (Trajectory::get().isFake(player)) {
            phys::flipGravity(player, flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (Trajectory::get().isFake(player)) {
            phys::teleportPlayer(this, object, player);
            return;
        }
        GJBaseGameLayer::teleportPlayer(object, player);
    }

    void destroyObject(GameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::destroyObject(object);
    }

    void pickupItem(EffectGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::pickupItem(object);
    }

    void processQueuedButtons(float dt, bool clearInputQueue) {
        if (Logic::get().predicting)
            return;
        if (auto* play = Logic::as_play(this)) {
            Logic::get().play_macro(play, this, dt, clearInputQueue);
            return;
        }
        if (Logic::get().editor_testing && Logic::as_editor(this)) {
            Logic::get().play_macro(this, this, dt, clearInputQueue);
            return;
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }

    void handleButton(bool down, int button, bool player1) {
        if (Logic::get().predicting)
            return;
        auto& value = Logic::get();
        // flip 2 player controls: i do the swap myself & tell gd not to (see the native call)
        // so a macro always holds the REAL player & records / plays the same with the option on or off
        bool flips = value.flip_controls() && !m_isPlatformer && !m_useReplay;
        bool mine = !value.applying_playback && !value.routing_input && !value.restoring_holds && !value.maintaining_gravity && !value.injecting && !value.follow_injecting && !orb_swift;
        if (flips && mine && m_levelSettings && m_levelSettings->m_twoPlayerMode)
            player1 = !player1;
        echo::step::trace_button('H', down, player1);
        auto* live = Logic::as_play(this);
        int jump_early = static_cast<int>(PlayerButton::Jump);
        bool jump_button_early = button == 0 || button == jump_early;
        if (value.suppress_auto_hold && jump_button_early && !value.applying_playback && !value.restoring_holds && !value.maintaining_gravity) {
            if (!down) {
                value.suppress_auto_hold = false;
            }
            else {
                if (auto* ui = m_uiLayer) {
                    if (player1) {
                        ui->m_p1Jumping = false;
                        ui->m_p1TouchId = -1;
                    }
                    else {
                        ui->m_p2Jumping = false;
                        ui->m_p2TouchId = -1;
                    }
                }
                return;
            }
        }
        if (!down && !player1 && value.is_recording() && !value.applying_playback &&
            !value.restoring_holds && !value.maintaining_gravity &&
            !m_gameState.m_isDualMode && !(m_levelSettings && m_levelSettings->m_twoPlayerMode)) {
            int key = button == 0 ? static_cast<int>(PlayerButton::Jump) : button;
            if (!Logic::get().held_at(value, true, value.step_frame + 1, key) &&
                Logic::get().held_at(value, false, value.step_frame + 1, key))
                player1 = true;
        }
        {
            int jump_key = static_cast<int>(PlayerButton::Jump);
            bool move_button = button == 0 || button == jump_key || button == static_cast<int>(PlayerButton::Left) || button == static_cast<int>(PlayerButton::Right);
            if (move_button && !value.follow_injecting && !value.restoring_holds && !value.maintaining_gravity && !value.injecting && Logic::get().follow_active(this)) {
                bool leader1 = value.follow_mode == 1;
                if (!value.applying_playback) {
                    player1 = leader1;
                    value.note_follow_press(this, down, button == 0 ? jump_key : button);
                }
                else if (value.backstep_target >= 0 && player1 == leader1) {
                    value.note_follow_press(this, down, button == 0 ? jump_key : button);
                }
            }
        }
        bool opened_button = !in_orb_button;
        in_orb_button = true;
        bool steered = false;
        if (!value.applying_playback && !value.routing_input && !value.restoring_holds && !value.maintaining_gravity) {
            steered = Logic::get().steer_player(player1);
            bool other_played = value.plays_player(player1);
            if (!steered && value.click_both && !other_played && !value.plays_player(!player1) && (button == 0 || button == static_cast<int>(PlayerButton::Jump) || button == static_cast<int>(PlayerButton::Left) || button == static_cast<int>(PlayerButton::Right))) {
                value.routing_input = true;
                handleButton(down, button, !player1);
                value.routing_input = false;
            }
        }
        int jump = static_cast<int>(PlayerButton::Jump);
        int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
        bool jump_button = button == 0 || button == jump;
        int swifts = 0;
        if (down && jump_button && !orb_swift && !value.applying_playback && !value.injecting && !value.restoring_holds && !value.maintaining_gravity) {
            PlayerObject* body = player1 ? m_player1 : m_player2;
        }
        if ((button == 0 || button == jump) && !orb_swift && !value.applying_playback && !value.maintaining_gravity && !value.restoring_holds && !value.resetting && !down) {
            value.restart_hold_captured = false;
            value.death_jump_p1 = value.death_jump_p2 = false;
        }
        if (button == jump && !value.suppress_click_hitbox && !value.maintaining_gravity && !value.restoring_holds) {
            Trajectory::get().note_button(player1, down);
            if (auto* play = Logic::as_play(this)) {
                bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
                if (twoPlayer) {
                }
                else if (!Logic::get().routing_input) {
                }
            }
        }
        if (auto* play = Logic::as_play(this)) {
            auto& logic = Logic::get();
            // mirror clicks are NOT p2
            bool ignored_p2 = !player1 && play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode && !m_gameState.m_isDualMode;
            if (!logic.injecting && !orb_swift && !logic.routing_input && !ignored_p2)
                Logic::get().note_click(play, down, player1, button);
            if (!logic.applying_playback && logic.is_recording() && !logic.injecting && !logic.restoring_holds) {
                Logic::get().record_input(play, down, player1, button);
            }
        }
        else if (Logic::get().editor_testing && Logic::as_editor(this)) {
            auto& logic = Logic::get();
            if (!logic.applying_playback && logic.is_recording() && !logic.injecting && !logic.restoring_holds) {
                Logic::get().record_input(this, down, player1, button);
            }
        }
        bool one_replay = value.applying_playback && value.play_p1 != value.play_p2;
        bool icon2 = !player1;
        if (value.maintaining_gravity || steered || one_replay) {
            if (steered)
                Logic::get().set_routed_hold(icon2, button, down);
            Logic::get().play_input(this, icon2, down, button);
        }
        else {
            auto* manager = GameManager::get();
            if (flips && manager)
                manager->setGameVariable("0010", false);
            GJBaseGameLayer::handleButton(down, button, player1);
            if (flips && manager)
                manager->setGameVariable("0010", true);
        }
        if ((button == 0 || button == jump || button == left || button == right) && !orb_swift && !value.maintaining_gravity && !value.suppress_click_hitbox && !value.restoring_holds) {
        }
        if (swifts > 0) {
            orb_swift = true;
            bool was_routing = value.routing_input;
            value.routing_input = true;
            for (int i = 0; i < swifts; ++i) {
                handleButton(false, jump, player1);
                handleButton(true, jump, player1);
            }
            value.routing_input = was_routing;
            orb_swift = false;
        }
        if (opened_button)
            in_orb_button = false;
    }

    void activateSFXTrigger(SFXTriggerGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::activateSFXTrigger(object);
    }

    void activateSFXEditTrigger(SFXTriggerGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::activateSFXEditTrigger(object);
    }

    void activateSongTrigger(SongTriggerGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::activateSongTrigger(object);
    }

    void activateSongEditTrigger(SongTriggerGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::activateSongEditTrigger(object);
    }

    void activatedAudioTrigger(SFXTriggerGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::activatedAudioTrigger(object);
    }

    void shakeCamera(float duration, float strength, float interval) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::shakeCamera(duration, strength, interval);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void checkpointActivated(CheckpointGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::checkpointActivated(object);
    }

    CCParticleSystemQuad* spawnParticle(char const* plist, int zOrder, tCCPositionType positionType, CCPoint position) {
        if (Logic::get().predicting)
            return nullptr;
        return GJBaseGameLayer::spawnParticle(plist, zOrder, positionType, position);
    }

    void spawnParticleTrigger(int particleID, CCPoint position, float rotation, float scale) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::spawnParticleTrigger(particleID, position, rotation, scale);
    }

    void createBackground(int background) {
        GJBaseGameLayer::createBackground(background);
    }

    void createMiddleground(int middleground) {
        if (m_middleground) {
            m_middleground->removeFromParent();
            m_middleground = nullptr;
        }
    }

    void createGroundLayer(int ground, int line) {
        GJBaseGameLayer::createGroundLayer(18, 2);
    }

    void updateColor(ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, ccHSVValue& copyHSV, int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
        GJBaseGameLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
    }

    void triggerGradientCommand(GradientTriggerObject* object) {
        GJBaseGameLayer::triggerGradientCommand(object);
    }
};

class $modify(EchoUILayer, UILayer) {

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        if (Logic::get().suppress_auto_hold)
            return false;
        return UILayer::ccTouchBegan(touch, event);
    }

    void handleKeypress(enumKeyCodes key, bool down, double timestamp) {
        Input::note_key(key, down);
        if (Logic::get().steer_key(m_gameLayer, key, down, timestamp))
            return;
        UILayer::handleKeypress(key, down, timestamp);
    }

    void visit() {
        UILayer::visit();
    }
};

class $modify(EchoPlayerObject, PlayerObject) {
    struct Fields {
        Ref<CCNode> tick_spin;
        int64_t dash_effect_step = std::numeric_limits<int64_t>::min();
    };

    void stop_tick_spin() {
        if (auto* node = m_fields->tick_spin.data())
            dash_spin_manager()->removeAllActionsFromTarget(node);
    }

    void updateDashArt() {
        PlayerObject::updateDashArt();
        stop_tick_spin();
        if (Trajectory::get().isFake(this) || !dash_spin_enabled() || !m_isDashing || !m_iconSprite)
            return;
        auto* spin = m_iconSprite->getActionByTag(DASH_SPIN_TAG);
        if (!spin)
            return;
        if (!m_fields->tick_spin)
            m_fields->tick_spin = CCNode::create();
        auto* node = m_fields->tick_spin.data();
        node->setRotation(m_iconSprite->getRotation());
        auto* copy = static_cast<CCAction*>(spin->copy());
        copy->autorelease();
        dash_spin_manager()->addAction(copy, node, false);
    }

    void update(float dt) {
        if (Trajectory::get().isFake(this) && !Logic::get().predicting)
            return;
        if (Logic::get().suppress_auto_hold && !Trajectory::get().isFake(this)) {
            auto& logic = Logic::get();
            auto it = m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
            if (it != m_holdingButtons.end() && it->second)
                PlayerObject::releaseButton(PlayerButton::Jump);
            m_jumpBuffered = false;
            bool keyDown = Input::jump_key_down(), mouseDown = Input::mouseDown(), ui_down = false;
            if (auto* layer = GJBaseGameLayer::get()) {
                if (auto* ui = layer->m_uiLayer) {
                    ui_down = ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
                }
            }
            if (!keyDown && !mouseDown && !ui_down)
                logic.suppress_auto_hold = false;
        }
        Logic::get().reinforce_routed_hold(this);
        PlayerObject::update(dt);
        if (!Trajectory::get().isFake(this))
            Trajectory::get().set_delta(dt);
        if (m_isSwing && tilt_applies(this) && tilt_fix().contains(this) && !tilt_held().contains(this)) {
            float turn = visual_tilt(this) - getRotation();
            for (auto* burst : std::array{ m_swingBurstParticles1, m_swingBurstParticles2 }) {
                if (burst)
                    burst->setAngle(burst->getAngle() - turn);
            }
        }
    }

    void updateShipRotation(float dt) {
        restore_tilt(this);
        if (!tilt_applies(this)) {
            tilt_fix().erase(this);
            PlayerObject::updateShipRotation(dt);
            return;
        }
        turn_both(this, [this, dt] {
            PlayerObject::updateShipRotation(dt);
        },
            [this, dt] {
                auto pos = getPosition();
                auto back = pos - m_shipRotation;
                float moved = back.x * back.x + back.y * back.y;
                float need = dt * 1.25f + 0.01f;
                auto saved = m_shipRotation;
                if (moved > 1e-8f && moved < need)
                    m_shipRotation = pos - back * std::sqrt(need / moved);
                PlayerObject::updateShipRotation(dt);
                m_shipRotation = saved;
            });
    }

    void updateRotation(float dt, float rotation) {
        restore_tilt(this);
        if (!tilt_applies(this)) {
            PlayerObject::updateRotation(dt, rotation);
            return;
        }
        auto turn = [this, dt, rotation] {
            PlayerObject::updateRotation(dt, rotation);
        };
        turn_both(this, turn, turn);
    }

    void setPosition(CCPoint const& position) {
        if (Trajectory::get().isFake(this) && phys::fast_prediction_pos()) {
            phys::pos_without_art(this, position);
            return;
        }
        if (auto& logic = Logic::get(); logic.defer_player_art && !Trajectory::get().isFake(this)) {
            phys::pos_without_art(this, position);
            logic.player_art_deferred = true;
            return;
        }
        if (!tilt_applies(this) || !tilt_fix().contains(this) || tilt_held().contains(this)) {
            PlayerObject::setPosition(position);
            return;
        }
        float gameplay = getRotation();
        setRotation(visual_tilt(this));
        PlayerObject::setPosition(position);
        setRotation(gameplay);
    }

    void setVisible(bool visible) {
        auto* layer = GJBaseGameLayer::get();
        if (layer && !Logic::get().is_traj_player(this)) {
            if (this == layer->m_player1 && Logic::get().hide_p1)
                visible = false;
            if (this == layer->m_player2 && Logic::get().hide_p2)
                visible = false;
        }
        PlayerObject::setVisible(visible);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (Trajectory::get().isFake(this)) {
            phys::ringJump(this, ring);
            return;
        }
        if (!Logic::get().player_hidden(this)) {
            PlayerObject::ringJump(ring, skipCheck);
            return;
        }
        bool effects = m_playEffects;
        m_playEffects = false;
        PlayerObject::ringJump(ring, skipCheck);
        m_playEffects = effects;
    }

    void bumpPlayer(float force, int objectType, bool playEffect, GameObject* object) {
        if (Trajectory::get().isFake(this)) {
            if (Logic::get().predicting) {
            }
            phys::bumpPlayer(this, force, objectType, playEffect, object);
            return;
        }
        PlayerObject::bumpPlayer(force, objectType, playEffect, object);
    }

    void propellPlayer(float force, bool noEffects, int objectType) {
        if (Trajectory::get().isFake(this)) {
            if (Logic::get().predicting) {
            }
            phys::propellPlayer(this, force, noEffects, objectType);
            return;
        }
        PlayerObject::propellPlayer(force, noEffects, objectType);
    }

    void startDashing(DashRingObject* ring) {
        if (Trajectory::get().isFake(this)) {
            phys::startDashing(this, ring);
            return;
        }
        CCNode* object_layer = nullptr;
        unsigned before = object_layer && object_layer->getChildren() ? object_layer->getChildren()->count() : 0;
    }

    void incrementJumps() {
        if (Trajectory::get().drawing())
            return;
        PlayerObject::incrementJumps();
    }

    void note_partner() {
        auto* layer = GJBaseGameLayer::get();
        if (layer && this == layer->m_player2 && !Trajectory::get().isFake(this))
            ++Logic::get().partner_presses;
    }

    void releaseButton(PlayerButton button) {
        auto* play = PlayLayer::get();
        auto& logic = Logic::get();
        if (!Trajectory::get().isFake(this))
            echo::step::trace_button('R', false, static_cast<int>(button));
        note_partner();
        PlayerObject::releaseButton(button);
    }

    bool pushButton(PlayerButton button) {
        auto& logic = Logic::get();
        if (!Trajectory::get().isFake(this))
            echo::step::trace_button('P', true, static_cast<int>(button));
        if (logic.suppress_auto_hold && button == PlayerButton::Jump && !Trajectory::get().isFake(this) && !logic.maintaining_gravity && !logic.applying_playback && !logic.injecting) {
            return false;
        }
        note_partner();
        bool track = button == PlayerButton::Jump && !Trajectory::get().isFake(this);
        auto finish = [&](bool pressed) {
            return pressed;
        };
        // maintain gravity clicks here NOT thru handleButton
        GJBaseGameLayer* layer = nullptr;
        bool player1 = true;
        int swifts = 0;
        if (track && !in_orb_button && !orb_swift && logic.maintaining_gravity && !logic.applying_playback && !logic.injecting && !logic.restoring_holds && !logic.predicting && !logic.suppress_rings) {
            layer = GJBaseGameLayer::get();
            if (layer) {
                player1 = this != layer->m_player2;
            }
        }
        bool stash = logic.suppress_rings && button == PlayerButton::Jump && m_touchingRings && m_touchingRings->count() > 0;
        bool pressed = false;
        if (!stash) {
            pressed = finish(PlayerObject::pushButton(button));
        }
        else {
            std::vector<CCObject*> rings;
            rings.reserve(m_touchingRings->count());
            for (unsigned int i = 0; i < m_touchingRings->count(); ++i) {
                auto* ring = m_touchingRings->objectAtIndex(i);
                ring->retain();
                rings.push_back(ring);
            }
            m_touchingRings->removeAllObjects();
            pressed = PlayerObject::pushButton(button);
            for (auto* ring : rings) {
                m_touchingRings->addObject(ring);
                ring->release();
            }
            pressed = finish(pressed);
        }
        if (swifts <= 0)
            return pressed;
        orb_swift = true;
        int jump = static_cast<int>(PlayerButton::Jump);
        for (int i = 0; i < swifts; ++i) {
            releaseButton(PlayerButton::Jump);
            if (layer && logic.is_recording())
                Logic::get().record_input(layer, false, player1, jump);
            pushButton(PlayerButton::Jump);
            if (layer && logic.is_recording())
                Logic::get().record_input(layer, true, player1, jump);
        }
        orb_swift = false;
        return pressed;
    }

    void releaseAllButtons() {
        auto* layer = GJBaseGameLayer::get();
        auto* play = Logic::as_play(layer);
        auto& logic = Logic::get();
        if (logic.resetting || logic.suppress_auto_hold || !play) {
            PlayerObject::releaseAllButtons();
            return;
        }
        bool second = this == play->m_player2;
        bool only2 = logic.records_player(true) && !logic.records_player(false);
        bool only1 = logic.records_player(false) && !logic.records_player(true);
        if ((second && only2) || (!second && only1 && this == play->m_player1))
            return;
        PlayerObject::releaseAllButtons();
    }

    void playDeathEffect() {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this) || Logic::get().disable_death_effect)
            return;
        PlayerObject::playDeathEffect();
    }

    void playerDestroyed(bool noEffects) {
        auto* play = PlayLayer::get();
        if (Logic::get().predicting || Logic::get().is_traj_player(this)) {
            m_isDead = true;
            return;
        }
        if (play && Logic::get().should_noclip(play, this)) {
            m_isDead = false;
            bool other_dead = (play->m_player1 && play->m_player1 != this && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2 != this && play->m_player2->m_isDead);
            if (!other_dead)
                play->m_playerDied = false;
            return;
        }
        PlayerObject::playerDestroyed(noEffects);
    }

    void playBumpEffect(int objectType, GameObject* object) {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::playBumpEffect(objectType, object);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void playSpawnEffect() {
        if (Logic::get().predicting || Logic::get().backstepping || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::playSpawnEffect();
    }

    void spawnCircle() {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(ccColor3B color, float startRadius) {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void spawnScaleCircle() {
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this))
            return;
        PlayerObject::spawnScaleCircle();
    }

    void activateStreak() {
        if (Logic::get().player_hidden(this))
            return;
        PlayerObject::activateStreak();
    }

    void placeStreakPoint() {
        if (Trajectory::get().isFake(this)) {
            m_justPlacedStreak = false;
            return;
        }
        if (Logic::get().player_hidden(this))
            return;
        PlayerObject::placeStreakPoint();
    }
};

class $modify(EchoSchedulerDepth, CCScheduler) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCScheduler::update", Priority::First);
    }

    void update(float dt) {
        auto& logic = Logic::get();
        ++logic.scheduler_depth;
        CCScheduler::update(dt);
        --logic.scheduler_depth;
    }
};

class $modify(EchoScheduler, CCScheduler) {
};

class $modify(EchoGameObject, GameObject) {
    static void onModify(auto& self) {
        hold_for_predict(self, "GameObject::activateObject");
        hold_for_predict(self, "GameObject::deactivateObject");
        hold_for_predict(self, "GameObject::playShineEffect");
    }

    void activateObject() {
        if (Logic::get().predicting)
            return;
        GameObject::activateObject();
    }

    void deactivateObject(bool deactivate) {
        if (Logic::get().predicting)
            return;
        GameObject::deactivateObject(deactivate);
    }

    void playShineEffect() {
        if (Logic::get().predicting)
            return;
        GameObject::playShineEffect();
    }

    void addGlow(gd::string frame) {
        GameObject::addGlow(std::move(frame));
    }
};

class $modify(EchoEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }

    void triggerActivated(float xPosition) {
        if (Logic::get().predicting)
            return;
        EffectGameObject::triggerActivated(xPosition);
    }
};

class $modify(EchoEnhancedObject, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        if (Trajectory::get().isFake(player)) {
            phys::activateForTrajectory(static_cast<EffectGameObject*>(static_cast<GameObject*>(this)), player);
            return;
        }
        if (!player || !Logic::get().player_hidden(player)) {
            EnhancedGameObject::activatedByPlayer(player);
            return;
        }
        bool effects = player->m_playEffects;
        player->m_playEffects = false;
        EnhancedGameObject::activatedByPlayer(player);
        player->m_playEffects = effects;
    }
};

class $modify(EchoRingObject, RingObject) {
    void spawnCircle() {
        if (Logic::get().predicting)
            return;
        auto* play = PlayLayer::get();
        if (play) {
            auto caused = [&](PlayerObject* player) {
                return player && Logic::get().player_hidden(player) && player->m_touchingRings && player->m_touchingRings->containsObject(this);
            };
            if (caused(play->m_player1) || caused(play->m_player2))
                return;
        }
        RingObject::spawnCircle();
    }
};

// skidded from silicate (GPL-3.0, gd 2.2081)
// swaps gds step count math in GJBaseGameLayer::update for a load of my count
// HAS to be a raw patch, a midhook saves every register & that changes the ship delta
namespace {
    Patch* g_step_patch = nullptr;
    std::vector<uint8_t> g_step_bytes;
}

void install_physics_step_patch() {
    std::vector<uint8_t> bytes = {
        0x49, 0xBB, // movabs r11, <count ptr>
        0, 0, 0, 0, 0, 0, 0, 0,
        0x45, 0x8B, 0x1B,            // mov r11d, [r11]
        0xE9, 0x31, 0x00, 0x00, 0x00 // jmp over the old math
    };
    auto* amt = &Logic::get().tick_steps;
    std::memcpy(bytes.data() + 2, &amt, sizeof(amt));
    auto address = reinterpret_cast<void*>(geode::base::get() + 0x237A5B); // in GJBaseGameLayer::update
    if (auto res = geode::Mod::get()->patch(address, bytes); res)
        g_step_patch = res.unwrap();
    g_step_bytes = bytes;
}

// silicate patches the SAME spot with its own count so mine gets ignored (crash at high tps)
// its disable bot button can also put gds old math back
void check_physics_step_patch() {
    auto* at = reinterpret_cast<uint8_t*>(geode::base::get() + 0x237A5B);
    if (g_step_bytes.empty() || std::memcmp(at, g_step_bytes.data(), g_step_bytes.size()) == 0)
        return;
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get())
            continue;
        for (auto* patch : mod->getPatches()) {
            if (!patch->isEnabled() || patch->getAddress() != reinterpret_cast<uintptr_t>(at))
                continue;
            Logic::get().message = fmt::format("Mod conflict: Disable {} in Geode!", mod->getName());
            Notification::create(Logic::get().message, NotificationIcon::Error)->show();
            return;
        }
    }
    // nobody owns it so take it back
    if (g_step_patch) {
        (void)g_step_patch->disable();
        (void)g_step_patch->enable();
    }
}

int vanilla_physics_steps(float dt, float timeWarp) {
    float warp = std::min(timeWarp, 1.f);
    if (!std::isfinite(warp) || !(warp > 0.f))
        warp = 1.f;
    if (!std::isfinite(dt) || dt < 0.f)
        dt = 0.f;
    int steps = static_cast<int>(std::lround(static_cast<double>(dt) * 60.0 / static_cast<double>(warp) * 4.0));
    return std::max(steps, 1);
}

void apply_lvl_seed() {
    auto* play = PlayLayer::get();
    if (!play)
        return;
    auto& logic = Logic::get();
    if (logic.keep_lvl_seed)
        return;
    if (!logic.is_playing() && !logic.seed_override) {
        Logic::get().roll_live_seed(play);
        return;
    }
    if (logic.seed_override) {
        logic.lvl_seed = logic.seed_override_value;
        logic.lvl_seed_known = true;
    }
    if (logic.lvl_seed_known) {
        Logic::get().set_game_random_state(logic.lvl_seed);
        logic.shake_state = logic.lvl_seed & 0x7FFF;
        logic.tp_state = logic.lvl_seed & 0x7FFF;
        Logic::get().apply_attempt_variance(play, logic.lvl_seed);
        Logic::get().seed_rand_triggers(logic.lvl_seed);
        return;
    }
    uint64_t state = Logic::get().game_random_state();
    logic.lvl_seed = state;
    logic.lvl_seed_known = true;
    logic.shake_state = state & 0x7FFF;
    logic.tp_state = state & 0x7FFF;
    Logic::get().seed_rand_triggers(state);
}

safetyhook::MidHook g_levelSeed;

void on_lvl_seed(safetyhook::Context&) {
    apply_lvl_seed();
}

// the mid hooks are NOT geode patches so disable bot has to take them out itself
void echo_raw_hooks(bool on) {
    static safetyhook::MidHook shake_a, shake_b, shake_c, shake_d;
    static safetyhook::MidHook teleport;
    if (!on) {
        g_levelSeed = {};
        shake_a = {};
        shake_b = {};
        shake_c = {};
        shake_d = {};
        teleport = {};
        return;
    }
    // where resetLevel writes the attempts rand state (silicate's offset, 2.2081)
    // recording keeps what gd picked, playback puts the macros seed back
    g_levelSeed = safetyhook::create_mid(geode::base::get() + 0x3b90fb, on_lvl_seed);

    // shake & teleport rolls use my own seeded lcg NOT gds rand, so nothing else rolling changes them
    auto shake_step = [](safetyhook::Context& ctx) {
        auto& state = Logic::get().shake_state;
        state = static_cast<uint64_t>(static_cast<int>((214013ull * state + 2531011ull) >> 16) & 0x7FFF);
        ctx.rax = static_cast<uintptr_t>(state);
        ctx.rip += 6;
    };
    auto teleport_step = [](safetyhook::Context& ctx) {
        auto& state = Logic::get().tp_state;
        state = static_cast<uint64_t>(static_cast<int>((214013ull * state + 2531011ull) >> 16) & 0x7FFF);
        ctx.rax = static_cast<uintptr_t>(state);
    };
    shake_a = safetyhook::create_mid(geode::base::get() + 0x23E173, shake_step); // the 4 rand calls for shake
    shake_b = safetyhook::create_mid(geode::base::get() + 0x23E1A1, shake_step);
    shake_c = safetyhook::create_mid(geode::base::get() + 0x23E1CB, shake_step);
    shake_d = safetyhook::create_mid(geode::base::get() + 0x23E1E9, shake_step);
    teleport = safetyhook::create_mid(geode::base::get() + 0x20FEDC, teleport_step); // in teleportPlayer
}

// silicate saved MY step patch as its "original" (it loaded after me). when it turns itself off it puts mine back
// even if im disabled, & then the game reads my step count forever. so while im off i keep that spot gds own
void echo_guard_step_site() {
    auto* at = reinterpret_cast<uint8_t*>(geode::base::get() + 0x237A5B);
    if (!g_step_patch || g_step_bytes.empty() || std::memcmp(at, g_step_bytes.data(), g_step_bytes.size()) != 0)
        return;
    (void)g_step_patch->enable();
    (void)g_step_patch->disable();
}

$execute {
    install_physics_step_patch();
    echo_raw_hooks(true);

    // skidded straight from silicate (GPL-3.0), these offsets + bytes are their findings for 2.2081
    auto patch = [](uintptr_t offset, std::vector<uint8_t> bytes, char const* name) {
        (void)Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset), bytes);
    };
    patch(0x23B4EA, std::vector<uint8_t>(24, 0x90), "GJBaseGameLayer::resetLevelVariables");
    patch(0x4CD95C, std::vector<uint8_t>(19, 0x90), "UILayer::handleKeypress");
    patch(0x3B994C, { 0xEB, 0x5E }, "PlayLayer::resetLevel");
    patch(0x3BA508, std::vector<uint8_t>(5, 0x90), "PlayLayer::resumeAndRestart");
}

class $modify(EchoShaderLayer, ShaderLayer) {
    void performCalculations() {
        ShaderLayer::performCalculations();
    }
};

class $modify(EchoEffectManager, GJEffectManager) {
    void updateEffects(float dt) {
        GJEffectManager::updateEffects(dt);
    }

    float opacityModForGroup(int id) {
        return GJEffectManager::opacityModForGroup(id);
    }
};

class $modify(EchoHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        auto* points = m_pointArray;
        auto* last = points && points->count() > 0 ? static_cast<PointNode*>(points->lastObject()) : nullptr;
        if (!last) {
            HardStreak::addPoint(point);
            return;
        }
        auto* layer = GJBaseGameLayer::get();
        float zoom = layer ? layer->m_gameState.m_cameraZoom : 1.f;
        if (!std::isfinite(zoom) || zoom < 0.05f)
            zoom = 1.f;
        float reach = 0.25f / zoom;
        if (ccpDistanceSQ(last->m_point, point) < reach * reach)
            return;
        HardStreak::addPoint(point);
    }
};

class $modify(EchoDirector, CCDirector) {
};

class $modify(EchoEGLView, CCEGLView) {
    void setFrameSize(float width, float height) {
        CCEGLView::setFrameSize(width, height);
    }

    void onGLFWframebuffersize(GLFWwindow* window, int width, int height) {
        CCEGLView::onGLFWframebuffersize(window, width, height);
    }

    void onGLFWWindowSizeFunCallback(GLFWwindow* window, int width, int height) {
        CCEGLView::onGLFWWindowSizeFunCallback(window, width, height);
    }

    void toggleLockCursor(bool locked) {
        if (locked && menu_holds_mouse())
            locked = false;
        CCEGLView::toggleLockCursor(locked);
    }
};

class $modify(EchoAudioEngine, FMODAudioEngine) {
    void update(float dt) {
        FMODAudioEngine::update(dt);
    }
};

FMOD_RESULT echo_mixer_update(FMOD::System* self) {
    return self->update();
}

$execute {
    (void)Mod::get()->hook(
        reinterpret_cast<void*>(geode::addresser::getNonVirtual(&FMOD::System::update)),
        &echo_mixer_update,
        "FMOD::System::update",
        tulip::hook::TulipConvention::Stdcall);
}

class $modify(EchoAppDelegate, AppDelegate) {
    void applicationDidEnterBackground() {
        Logic::get().save_settings_file();
        Logic::get().on_focus_change();
        AppDelegate::applicationDidEnterBackground();
    }

    void trySaveGame(bool force) {
        Logic::get().save_settings_file();
        AppDelegate::trySaveGame(force);
    }

    void applicationWillEnterForeground() {
        AppDelegate::applicationWillEnterForeground();
        Logic::get().on_focus_change();
    }
};
