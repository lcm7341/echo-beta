// where echo hooks the game
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
    // TODO
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
    // TODO
    return {};
}

static void keep_menu_cursor() {
    // TODO
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
    // todo
    return {};
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
    // port this
}

void present_tilt(PlayerObject* player) {
    // TODO
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
