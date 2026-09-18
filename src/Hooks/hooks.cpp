// where echo hooks the game
//
// PlayLayer: level start, reset, completion, pause & checkpoints. reset is where the seed goes in & the macro rewinds
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
#include "Hooks/step.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/collisions.hpp"
#include "Physics/gjbasegamelayer.hpp"
#include "Physics/gravity.hpp"
#include "Physics/player.hpp"
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
    if (!imgui.isInitialized() || !imgui.isVisible()) {
        return false;
    }
    return ImGui::GetIO().WantCaptureMouse || ImGui::IsAnyItemActive();
}

static void keep_menu_cursor() {
    if (!menu_holds_mouse())
        return;
    auto* view = CCEGLView::get();
    if (!view)
        return;
    if (view->m_bCursorLocked) {
        view->toggleLockCursor(false);
    }
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
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        if (Logic::get().practice_music_hack)
            m_practiceMusicSync = true;
        Logic::get().on_play_init(this);
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
        }
        logic.carry = 0.f;
        logic.respawn_frames = 0;
        logic.resetting = true;
        logic.restored_checkpoint = nullptr;
        Logic::get().sync_checkpoints(this);
        Logic::get().save_holds(this);
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

}

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
        if (auto& logic = Logic::get(); logic.defer_player_art && !Trajectory::get().isFake(this)) {
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

    void playDeathEffect() {
        PlayerObject::playDeathEffect();
    }

    void playerDestroyed(bool noEffects) {
        auto* play = PlayLayer::get();
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
        PlayerObject::playBumpEffect(objectType, object);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void playSpawnEffect() {
        PlayerObject::playSpawnEffect();
    }

    void spawnCircle() {
        PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(ccColor3B color, float startRadius) {
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void spawnScaleCircle() {
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

// skidded from silicate (GPL-3.0, gd 2.2081)
// swaps gds step count math in GJBaseGameLayer::update for a load of my count
// HAS to be a raw patch, a midhook saves every register & that changes the ship delta
namespace {
    Patch* g_step_patch = nullptr;
    std::vector<uint8_t> g_step_bytes;
}

void install_physics_step_patch() {
    // TODO
}

// silicate patches the SAME spot with its own count so mine gets ignored (crash at high tps)
// its disable bot button can also put gds old math back
void check_physics_step_patch() {
    // TODO
}

int vanilla_physics_steps(float dt, float timeWarp) {
    // TODO
    return {};
}

void apply_lvl_seed() {
    // TODO
}

safetyhook::MidHook g_levelSeed;

void on_lvl_seed(safetyhook::Context&) {
    apply_lvl_seed();
}

// the mid hooks are NOT geode patches so disable bot has to take them out itself
void echo_raw_hooks(bool on) {
    // todo
}

// silicate saved MY step patch as its "original" (it loaded after me). when it turns itself off it puts mine back
// even if im disabled, & then the game reads my step count forever. so while im off i keep that spot gds own
void echo_guard_step_site() {
    // later
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
