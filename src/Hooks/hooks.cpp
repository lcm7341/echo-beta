#include "Physics/object_counters.hpp"
#include "Logic/logic.hpp"
#include "Logic/perf.hpp"
#include "Hooks/rotation.hpp"
#include "Hooks/step.hpp"
#include "Hooks/opt.hpp"
#include "Hooks/shown_camera.hpp"
#include <bit>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "Hack/orbs.hpp"
#include "GUI/gui.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/level_forecast.hpp"
#include "Physics/collisions.hpp"
#include "Physics/gjbasegamelayer.hpp"
#include "Physics/gravity.hpp"
#include "Physics/player.hpp"
#include "Physics/prediction_art.hpp"
#include "Rendering/effect_frame.hpp"
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
#include <limits>
#include <vector>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/RingObject.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PointNode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/RandTriggerGameObject.hpp>
#include <Geode/binding/RandTriggerGameObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EnhancedGameObject.hpp>
#include <Geode/modify/RingObject.hpp>
#include <Geode/modify/CCCircleWave.hpp>
#include <Geode/modify/CCActionManager.hpp>
#include <Geode/modify/GJActionManager.hpp>
#include <Geode/Geode.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#ifdef GEODE_IS_WINDOWS
#include <safetyhook.hpp>
#include <windows.h>
#endif
#include "Logic/input.hpp"
#include <Geode/modify/ShaderLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/CCDrawNode.hpp>
#include <fstream>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCSprite.hpp>
#include <Geode/modify/CCNode.hpp>
#include <imgui-cocos.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include <Geode/modify/AppDelegate.hpp>

namespace chrono = std::chrono;

using namespace geode::prelude;

static bool g_native_reset = false;
static bool g_seed_applied = false;
void apply_lvl_seed();

static std::vector<Hook*>& predict_hooks() {
    static std::vector<Hook*> hooks;
    return hooks;
}

static bool predict_hooks_on = false;

static void hold_for_predict(auto& self, const char* name) {
    auto hook = self.getHook(name);
    if (!hook) {
        return;
    }
    hook.unwrap()->setAutoEnable(false);
    predict_hooks().push_back(hook.unwrap());
}

void echo_forecast_section_hook(bool on);

void Logic::sync_predict_hooks() {
    bool watching = is_playing() && !is_recording();
    bool want = show_traj || pause_before_kill || traj_autoclicker || (show_death_next && !watching) || predicting;
    bool forecast_users = show_traj || pause_before_kill || traj_autoclicker || best_frame_running;
    echo_forecast_section_hook(forecast_users && traj_moving_objects);
    echo_touch_hook((backstep && !render_capturing()) || is_recording());
    if (want == predict_hooks_on)
        return;
    for (auto* hook : predict_hooks())
        (void)(want ? hook->enable() : hook->disable());
    predict_hooks_on = want;
}

static unsigned long long g_updates_run = 0;

void echo_forget_camera(GJBaseGameLayer* layer);

constexpr int DASH_SPIN_TAG = 12;
static EffectFrame g_effect_frame;
static int g_stacked_waves = 0;
static bool quiet_orb = false;
// waves gd made for a dud ahh orb!
static std::vector<CCCircleWave*> g_dud_waves;
struct BoomStack {
    std::vector<CCPoint> offsets;
    std::vector<int> weights;
    int base = 1;
};
static std::unordered_map<CCSprite*, BoomStack> g_boom_stacks;
static Hook* g_stack_visit_hook = nullptr;
struct BoomFirst {
    CCSprite* sprite = nullptr;
    CCPoint at;
};
static std::map<std::pair<GameObject*, PlayerObject*>, BoomFirst> g_boom_first;
static std::unordered_map<PlayerObject*, BoomFirst> g_after_first;
static bool g_boom_capture = false;
static CCSprite* g_boom_captured = nullptr;
constexpr float BOOM_SPACING = 0.5f;

class $modify(EchoBoomSprite, CCSprite) {
    static CCSprite* createWithSpriteFrameName(const char* name) {
        auto* made = CCSprite::createWithSpriteFrameName(name);
        if (g_boom_capture && !g_boom_captured)
            g_boom_captured = made;
        return made;
    }
};

class $modify(EchoStackNode, CCNode) {
    static void onModify(auto& self) {
        if (auto hook = self.getHook("cocos2d::CCNode::visit"); hook) {
            hook.unwrap()->setAutoEnable(false);
            g_stack_visit_hook = hook.unwrap();
        }
    }

    void visit() {
        if (g_boom_stacks.empty()) {
            CCNode::visit();
            return;
        }
        auto it = g_boom_stacks.find(reinterpret_cast<CCSprite*>(this));
        if (it == g_boom_stacks.end() || !isVisible()) {
            CCNode::visit();
            return;
        }
        // offsets are in the parents space so a plain translate works. whole visit so the outline comes too
        const auto& stack = it->second;
        auto* sprite = it->first;
        GLubyte own = sprite->getOpacity();
        std::vector<std::pair<CCRGBAProtocol*, GLubyte>> kids;
        for (auto* child : CCArrayExt<CCNode*>(getChildren())) {
            if (auto* rgba = typeinfo_cast<CCRGBAProtocol*>(child))
                kids.push_back({ rgba, rgba->getOpacity() });
        }
        auto weigh = [&](int weight) {
            auto scaled = [&](GLubyte opacity) {
                return (GLubyte)std::min(255, (int)opacity * std::max(weight, 1));
            };
            sprite->setOpacity(scaled(own));
            for (auto& [kid, opacity] : kids)
                kid->setOpacity(scaled(opacity));
        };
        int total = stack.base;
        for (int weight : stack.weights)
            total += weight;
        int share = (total + (int)stack.offsets.size()) / ((int)stack.offsets.size() + 1);
        for (size_t at = 0; at < stack.offsets.size(); ++at) {
            weigh(share);
            kmGLPushMatrix();
            kmGLTranslatef(stack.offsets[at].x, stack.offsets[at].y, 0.f);
            CCNode::visit();
            kmGLPopMatrix();
        }
        weigh(share);
        CCNode::visit();
        sprite->setOpacity(own);
        for (auto& [kid, opacity] : kids)
            kid->setOpacity(opacity);
    }
};

static CCNode* g_wave_being_created = nullptr;
static CCAction* g_new_wave_tween = nullptr;
struct WaveBornThisFrame {
    CCCircleWave* wave;
    int step;
};
static std::vector<WaveBornThisFrame> g_waves_born_this_frame;

struct ActionElapsedSetter : CCActionInterval {
    static void set_elapsed(CCActionInterval* action, float elapsed) {
        auto* clock = static_cast<ActionElapsedSetter*>(action);
        clock->m_bFirstTick = false;
        clock->m_elapsed = elapsed;
    }
};

static float physics_step_seconds() {
    return (float)(1.0 / std::max(Logic::get().sample_rate(), 1.0));
}

static float action_progress_at(CCActionInterval* timed, float elapsed) {
    return std::clamp(elapsed / std::max(timed->getDuration(), FLT_EPSILON), 0.f, 0.9999f);
}

class $modify(EchoCaptureWaveTween, CCActionManager) {
    void addAction(CCAction* action, CCNode* target, bool paused) {
        if (g_wave_being_created && target == g_wave_being_created && !g_new_wave_tween)
            g_new_wave_tween = action;
        CCActionManager::addAction(action, target, paused);
    }
};

class $modify(EchoCircleWave, CCCircleWave) {
    struct MergedHit {
        int step = 0;
        CCPoint offset;
        int weight = 1;
    };

    struct Fields {
        int hits = 1;
        int born = 0, last = 0;
        bool stacked = false, ages_by_step = false;
        CCAction* tween_action = nullptr;
        std::vector<MergedHit> merged_hits;

        ~Fields() {
            if (stacked)
                --g_stacked_waves;
            if (tween_action)
                tween_action->release();
        }
    };

    bool init(float startRadius, float endRadius, float duration, bool fadeIn, bool easeOut) {
        if (quiet_orb && !g_effect_frame.open) {
            this->retain();
            g_dud_waves.push_back(this);
            return true;
        }
        g_wave_being_created = this;
        g_new_wave_tween = nullptr;
        bool ok = CCCircleWave::init(startRadius, endRadius, duration, fadeIn, easeOut);
        g_wave_being_created = nullptr;
        auto* captured = g_new_wave_tween;
        g_new_wave_tween = nullptr;
        if (!ok)
            return false;
        auto* fields = m_fields.self();
        int step = Logic::get().step_frame;
        fields->born = fields->last = step;
        if (typeinfo_cast<CCActionInterval*>(captured)) {
            captured->retain();
            fields->tween_action = captured;
            fields->ages_by_step = Logic::get().render_capturing();
            if (fields->ages_by_step && g_effect_frame.on) {
                this->retain();
                g_waves_born_this_frame.push_back({ this, step });
            }
        }
        if (auto* seen = g_effect_frame.open; seen && seen->wave_amt < (int)seen->waves.size()) {
            this->retain();
            seen->waves[seen->wave_amt++] = this;
        }
        return true;
    }

    void advance_tween_by(float extra) {
        auto* timed = typeinfo_cast<CCActionInterval*>(m_fields->tween_action);
        if (!timed || timed->isDone() || !getParent())
            return;
        float elapsed = std::min(extra, timed->getDuration() * 0.9999f);
        ActionElapsedSetter::set_elapsed(timed, elapsed);
        timed->update(action_progress_at(timed, elapsed));
    }

    void record_merged_hit(const CCPoint* offset) {
        auto* fields = m_fields.self();
        int step = Logic::get().step_frame;
        fields->last = step;
        CCPoint at = offset ? *offset : CCPointZero;
        bool same = !fields->merged_hits.empty() && (!fields->ages_by_step || fields->merged_hits.back().step == step) && ccpDistance(at, fields->merged_hits.back().offset) < 0.5f;
        if (same || fields->merged_hits.size() >= 512)
            ++fields->merged_hits.back().weight;
        else
            fields->merged_hits.push_back({ step, at, 1 });
        if (++fields->hits == 2) {
            fields->stacked = true;
            ++g_stacked_waves;
        }
    }

    void draw_as_stacked_copies(int weight, const CCPoint& offset) {
        float alpha = m_opacity * m_opacityMod;
        float stacked = m_blendAdditive ? std::min(alpha * (float)weight, 1.f) : 1.f - std::pow(1.f - std::clamp(alpha, 0.f, 1.f), (float)weight);
        if (m_opacityMod > 0.f)
            m_opacity = stacked / m_opacityMod;
        bool moved = offset.x != 0.f || offset.y != 0.f;
        if (moved) {
            kmGLPushMatrix();
            kmGLTranslatef(offset.x, offset.y, 0.f);
        }
        CCCircleWave::draw();
        if (moved)
            kmGLPopMatrix();
    }

    void draw() {
        auto* fields = m_fields.self();
        if (g_stacked_waves <= 0 || fields->hits <= 1) {
            CCCircleWave::draw();
            return;
        }
        float old_radius = m_radius, old_opacity = m_opacity;
        auto* timed = fields->ages_by_step ? typeinfo_cast<CCActionInterval*>(fields->tween_action) : nullptr;
        if (timed && timed->isDone())
            timed = nullptr;
        float elapsed = timed ? timed->getElapsed() : 0.f;
        float step_time = physics_step_seconds();
        auto pose_at_step = [&](int step) {
            if (!timed)
                return;
            timed->update(action_progress_at(timed, elapsed - (float)(step - fields->born) * step_time));
        };
        constexpr std::size_t MOST = 64;
        std::size_t total = fields->merged_hits.size() + 1;
        std::size_t groups = std::min(total, MOST);
        for (std::size_t g = 0; g < groups; ++g) {
            std::size_t from = g * total / groups, to = (g + 1) * total / groups;
            int weight = 0;
            for (std::size_t i = from; i < to; ++i)
                weight += i == 0 ? 1 : fields->merged_hits[i - 1].weight;
            std::size_t mid = (from + to - 1) / 2;
            int step = mid == 0 ? fields->born : fields->merged_hits[mid - 1].step;
            CCPoint offset = mid == 0 ? CCPointZero : fields->merged_hits[mid - 1].offset;
            pose_at_step(step);
            if (!timed) {
                m_radius = old_radius;
                m_opacity = old_opacity;
            }
            draw_as_stacked_copies(weight, offset);
        }
        if (timed)
            timed->update(action_progress_at(timed, elapsed));
        m_radius = old_radius;
        m_opacity = old_opacity;
    }
};

static EffectSeen* effect_slot(const EffectKey& key, const CCPoint* at, bool& fresh) {
    const auto& logic = Logic::get();
    fresh = false;
    if (!opt_effects || logic.predicting || logic.refresh_only || logic.backstepping)
        return nullptr;
    auto* seen = g_effect_frame.find(key, fresh);
    if (!seen || !fresh)
        return seen;
    g_effect_frame.open = seen;
    if (at) {
        seen->first_x = at->x;
        seen->first_y = at->y;
        seen->has_first = true;
    }
    return seen;
}

static void count_repeat(EffectSeen* seen, const CCPoint* at) {
    CCPoint offset;
    const CCPoint* moved = nullptr;
    if (at && seen->has_first) {
        offset = *at - CCPoint{ seen->first_x, seen->first_y };
        moved = &offset;
    }
    for (int wave = 0; wave < seen->wave_amt; ++wave)
        static_cast<EchoCircleWave*>(static_cast<CCCircleWave*>(seen->waves[wave]))->record_merged_hit(moved);
}

// true = already played ts shown frame. the 1st time needs an EffectScope around the call
static bool repeated_effect(const EffectKey& key, const CCPoint* at = nullptr) {
    bool fresh = false;
    auto* seen = effect_slot(key, at, fresh);
    if (!seen || fresh)
        return false;
    count_repeat(seen, at);
    return true;
}

static bool spread_effect(const EffectKey& key) {
    constexpr int PER_FRAME = 16;
    auto& logic = Logic::get();
    if (!opt_effects || logic.predicting || logic.refresh_only || logic.backstepping)
        return false;
    bool fresh = false;
    auto* seen = g_effect_frame.find(key, fresh);
    if (!seen)
        return false;
    double interval = 1.0 / std::max(logic.render_fps, 1);
    if (!logic.render_capturing()) {
        auto* director = CCDirector::sharedDirector();
        interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
        interval *= std::max(logic.speedhack, 0.01f);
    }
    int gap = std::max((int)(logic.sample_rate() * interval / PER_FRAME), 1);
    int step = logic.step_frame;
    if (fresh || (seen->plays < PER_FRAME && step - seen->last_step >= gap)) {
        seen->last_step = step;
        ++seen->plays;
        return false;
    }
    return true;
}

static void drop_dud_waves() {
    for (auto* wave : g_dud_waves) {
        wave->removeMeAndCleanup();
        wave->release();
    }
    g_dud_waves.clear();
}

struct EffectScope {
    EffectSeen* before = g_effect_frame.open;
    ~EffectScope() {
        g_effect_frame.open = before;
    }
};

// the layer gd adds the dash afterimage to: GameManager+0x1f8 (the game layer) then +0xeb8, from stopDashing on m1
static CCNode* effect_layer() {
#ifdef GEODE_IS_ARM_MAC
    auto* game = GJBaseGameLayer::get();
    auto* gm = GameManager::get();
    if (!game || !gm)
        return nullptr;
    if (*reinterpret_cast<GJBaseGameLayer**>(reinterpret_cast<char*>(gm) + 0x1f8) != game)
        return nullptr;
    auto* layer = *reinterpret_cast<CCNode**>(reinterpret_cast<char*>(game) + 0xeb8);
    for (auto* up = layer ? layer->getParent() : nullptr; up; up = up->getParent()) {
        if (up == game)
            return layer;
    }
    return nullptr;
#else
    return nullptr;
#endif
}

template <class F>
static CCSprite* added_effect(F&& run) {
    auto* layer = effect_layer();
    unsigned before = layer ? layer->getChildrenCount() : 0;
    run();
    if (!layer || layer->getChildrenCount() <= before)
        return nullptr;
    return typeinfo_cast<CCSprite*>(static_cast<CCObject*>(layer->getChildren()->objectAtIndex(layer->getChildrenCount() - 1)));
}

static void stack_offset(const BoomFirst& first, CCPoint now) {
    auto offset = now - first.at;
    auto& stack = g_boom_stacks[first.sprite];
    CCPoint last = stack.offsets.empty() ? CCPointZero : stack.offsets.back();
    if (ccpDistance(offset, last) >= BOOM_SPACING && stack.offsets.size() < 2048) {
        stack.offsets.push_back(offset);
        stack.weights.push_back(1);
    }
    else if (stack.weights.empty())
        ++stack.base;
    else
        ++stack.weights.back();
}

static void keep_first(CCSprite* sprite) {
    if (g_boom_stacks.emplace(sprite, BoomStack{}).second)
        sprite->retain();
    if (g_stack_visit_hook && !g_stack_visit_hook->isEnabled())
        (void)g_stack_visit_hook->enable();
}

static void forget_booms() {
    g_boom_first.clear();
    g_after_first.clear();
    for (auto it = g_boom_stacks.begin(); it != g_boom_stacks.end();) {
        if (!it->first->getParent()) {
            it->first->release();
            it = g_boom_stacks.erase(it);
        }
        else
            ++it;
    }
    if (g_boom_stacks.empty() && g_stack_visit_hook && g_stack_visit_hook->isEnabled())
        (void)g_stack_visit_hook->disable();
}

static void age_waves_born_this_frame() {
    if (g_waves_born_this_frame.empty())
        return;
    int last = Logic::get().step_frame - 1;
    float step_time = physics_step_seconds();
    for (const auto& [wave, born] : g_waves_born_this_frame) {
        static_cast<EchoCircleWave*>(wave)->advance_tween_by((float)std::max(last - born, 0) * step_time);
        wave->release();
    }
    g_waves_born_this_frame.clear();
}

static void forget_effects() {
    forget_booms();
    for (std::size_t at = 0; at < g_effect_frame.amt; ++at) {
        auto& seen = g_effect_frame.seen[at];
        for (int wave = 0; wave < seen.wave_amt; ++wave)
            static_cast<CCCircleWave*>(seen.waves[wave])->release();
        seen.wave_amt = 0;
    }
    g_effect_frame.amt = 0;
    g_effect_frame.open = nullptr;
}

static void begin_effects() {
    forget_effects();
    g_effect_frame.began = Logic::get().step_frame;
    g_effect_frame.on = true;
}

static void end_effects() {
    age_waves_born_this_frame();
    forget_effects();
    g_effect_frame.on = false;
}

static bool in_orb_button = false;
// the swifts own release & press. these must NOT start another one
static bool orb_swift = false;

class TickActionManager : public CCActionManager {
public:
    void tick(float dt) {
        this->update(dt);
    }
};

static TickActionManager* g_dash_spin = nullptr;
static bool g_tick_pending = true;
static int g_half_passes = 0;

static bool dash_spin_enabled() {
    auto& logic = Logic::get();
    return !logic.predicting && !logic.defer_physics();
}

static float dash_tick_delta() {
    float tps = (float)Logic::get().sample_rate();
    if (!std::isfinite(tps) || tps <= 0.f)
        tps = 240.f;
    float warp = 1.f;
    if (auto* layer = GJBaseGameLayer::get())
        warp = layer->m_gameState.m_timeWarp;
    if (!std::isfinite(warp))
        warp = 1.f;
    return 1.f / tps / std::max(warp, 1.f);
}

static TickActionManager* dash_spin_manager() {
    if (!g_dash_spin)
        g_dash_spin = new TickActionManager();
    return g_dash_spin;
}

static void dash_spin_pre() {
    if (!g_tick_pending)
        return;
    g_tick_pending = false;
    if (g_dash_spin && dash_spin_enabled())
        g_dash_spin->tick(dash_tick_delta());
}

static void dash_spin_post(bool is_half_tick) {
    if (!is_half_tick) {
        g_half_passes = 0;
        g_tick_pending = true;
        return;
    }
    if (++g_half_passes >= 2) {
        g_half_passes = 0;
        g_tick_pending = true;
    }
}

static void dash_spin_reset() {
    g_tick_pending = true;
    g_half_passes = 0;
}

#ifdef GEODE_IS_WINDOWS
// msvc delete crashes on ts when leaving a lvl
struct SectionVec {
    uintptr_t* begin;
    uintptr_t* end;
    uintptr_t* cap;
};

static bool can_touch(const void* pointer, size_t bytes, bool write) {
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
    auto protect = info.Protect & 255;
    return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

static bool heap_block_ok(void* begin, void* cap) {
    if (!begin)
        return true;
    if (!can_touch(begin, sizeof(void*), false))
        return false;
    auto start = reinterpret_cast<uintptr_t>(begin);
    auto stop = reinterpret_cast<uintptr_t>(cap);
    if (stop < start)
        return false;
    auto bytes = (stop - start) & ~uintptr_t{ 7 };
    if (bytes >= 1073741824ull)
        return false;
    if (bytes < 4096)
        return true;
    auto* header_slot = reinterpret_cast<uintptr_t*>(start - 8);
    if (!can_touch(header_slot, sizeof(uintptr_t), false))
        return false;
    auto header = *header_slot;
    if (header == 0 || (header & 7) != 0 || header >= start)
        return false;
    auto gap = start - header;
    return gap >= 8 && gap - 8 <= 31;
}

static void zero_vec(SectionVec* vec) {
    vec->begin = nullptr;
    vec->end = nullptr;
    vec->cap = nullptr;
}

static ptrdiff_t vec_count(SectionVec* vec) {
    if (!vec->begin || !vec->cap || vec->end < vec->begin || vec->cap < vec->begin)
        return 0;
    auto capacity = vec->cap - vec->begin;
    auto amt = vec->end - vec->begin;
    if (amt > capacity)
        amt = capacity;
    if (amt > 1048576)
        return -1;
    return amt;
}

static void drop_grid(SectionVec* outer) {
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

static void drop_broken_sections(GJBaseGameLayer* layer) {
    auto* base = reinterpret_cast<unsigned char*>(layer);
    drop_grid(reinterpret_cast<SectionVec*>(base + 0x3598)); // the 3 grids ~GJBaseGameLayer frees
    drop_grid(reinterpret_cast<SectionVec*>(base + 0x35b0));
    drop_grid(reinterpret_cast<SectionVec*>(base + 0x35c8));
}
#endif

static bool menu_holds_mouse() {
    auto& imgui = ImGuiCocos::get();
    if (!imgui.isInitialized() || !imgui.isVisible())
        return false;
    return ImGui::GetIO().WantCaptureMouse || ImGui::IsAnyItemActive();
}

static void keep_menu_cursor() {
#ifdef GEODE_IS_WINDOWS
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
    float x = (float)point.x, y = (float)point.y;
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
#endif
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

float gameplay_rotation(PlayerObject* player) {
    if (!player)
        return 0.f;
    auto held = tilt_held().find(player);
    return held == tilt_held().end() ? player->getRotation() : held->second;
}

float visual_rotation(PlayerObject* player) {
    if (!player)
        return 0.f;
    return tilt_applies(player) ? visual_tilt(player) : gameplay_rotation(player);
}

void restore_rotation(PlayerObject* player, float gameplay_angle, float visual_angle) {
    if (!player)
        return;
    // DONT put the dead attempts angle back
    tilt_held().erase(player);
    tilt_fix().erase(player);
    tilt_left().erase(player);
    player->setRotation(gameplay_angle);
    if (tilt_applies(player)) {
        tilt_fix()[player] = visual_angle;
        tilt_left()[player] = gameplay_angle;
    }
}

#ifdef GEODE_IS_WINDOWS
void check_physics_step_patch();
void echo_guard_step_site();
#endif

class $modify(EchoPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
#ifdef GEODE_IS_WINDOWS
        check_physics_step_patch();
#endif
        if (auto& macro = Logic::get(); level && macro.macro_ldm_known && !macro.inputs.empty() && level->m_lowDetailModeToggled != macro.macro_ldm) {
            level->m_lowDetailModeToggled = macro.macro_ldm;
            Notification::create(macro.macro_ldm ? "Low Detail Mode turned on for this macro" : "Low Detail Mode turned off for this macro", NotificationIcon::Success)->show();
        }
        Logic::get().clear_rand_triggers();
        clear_tilt();
        if (Trajectory::get().attached())
            Trajectory::get().detach();
        Logic::get().reset_safe_mode();
        Logic::get().clear_backsteps();
        reseed_variance();
        Logic::get().clear_layout_hidden();
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;
        if (Logic::get().practice_music_hack)
            m_practiceMusicSync = true;
        Logic::get().prepare_lvl_view(this);
        Trajectory::get().attach(this);
        Logic::get().on_play_init(this);
        phys::prepare_prediction_art();
        return true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        Logic::get().after_frame(this, dt);
        Logic::get().update_safe_mode(this);
    }

    // updateCamera calls ts every frame & it rebuilds the whole label :/
    void flipArt(bool flip) {
        flip = flip && !Logic::get().no_mirror_portals;
        if (m_attemptLabel && m_attemptLabel->getScaleX() == (flip ? -1.f : 1.f))
            return;
        PlayLayer::flipArt(flip);
    }

    void updateVisibility(float dt) {
        PlayLayer::updateVisibility(dt);
        Logic::get().paint_layout_colors(this);
        Logic::get().draw_layout_art(this);
    }

    void addObject(GameObject* object) {
        // no effects. mirror portals keep theirs, w/out it the flip happens with no warning at all
        if (Logic::get().no_effects && object && object->m_objectType != GameObjectType::InverseMirrorPortal && object->m_objectType != GameObjectType::NormalMirrorPortal)
            object->m_hasNoEffects = true;
        if (!Logic::get().layout_active()) {
            PlayLayer::addObject(object);
            return;
        }
        Logic::get().style_layout_object(object);
        PlayLayer::addObject(object);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (Logic::get().render_queued) {
            Logic::get().render_queued = false;
            Logic::get().start_render();
        }
    }

    void updateColor(ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, ccHSVValue& copyHSV, int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
        if (Logic::get().layout_active())
            return;
        PlayLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
    }

    void resetLevel() {
        Trajectory::get().cancel_job();
        forget_effects();
        g_effect_frame.began = Logic::get().step_frame;
        restore_tilt(m_player1);
        restore_tilt(m_player2);
        clear_tilt();
        auto& logic = Logic::get();
        if (!logic.backstepping) {
            Logic::get().finish_backstep(this);
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
        Logic::get().end_safe_mode_attempt(this);
        forecast::invalidate();
        reseed_variance();
        g_native_reset = true;
        g_seed_applied = false;
        PlayLayer::resetLevel();
        g_native_reset = false;
        Logic::get().update_safe_mode(this);
        if (Logic::get().keep_lvl_seed)
            Logic::get().restore_checkpoint_random(this);
        else if (!roll_seed)
            Logic::get().plant_lvl_seed(this);
#if defined(GEODE_IS_MACOS)
        else if (!g_seed_applied)
            Logic::get().roll_live_seed(this);
#elif !defined(GEODE_IS_WINDOWS)
        else
            Logic::get().roll_live_seed(this);
#endif
        Logic::get().keep_lvl_seed = false;
        logic.resetting = false;
        logic.present = true;
        Logic::get().on_reset(this);
        if (!logic.backstepping)
            Logic::get().trim_backsteps(this);
        dash_spin_reset();
        Logic::get().cancel_steps();
        if (!logic.in_scheduler && !logic.backstepping)
            Logic::get().draw_hitboxes(this);
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
        // predicting must NEVER kill the real attempt
        if (Logic::get().predicting) {
            Trajectory::get().mark_predict_death(player);
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
        if (logic.predicting || logic.showed_new_best || logic.hide_new_best)
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
        Logic::get().update_safe_mode(this);
        Logic::get().hold_safe_mode_test_mode(this);
        Logic::get().on_complete(this);
        PlayLayer::levelComplete();
    }

    void showCompleteEffect() {
        if (Logic::get().render_capturing())
            Logic::get().hold_render_effects();
        PlayLayer::showCompleteEffect();
        if (Logic::get().render_capturing())
            Logic::get().hold_render_effects();
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

    void updateAttemptTime(float dt) {
        auto& logic = Logic::get();
        if (!logic.predicting && !logic.refresh_only && !m_playerDied && m_resumeTimer <= 8) {
            Logic::get().step_object_states(this);
            Logic::get().save_backstep(this);
        }
        PlayLayer::updateAttemptTime(dt);
    }

    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
        trace_flush();
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
        echo_forget_camera(this);
        restore_tilt(m_player1);
        restore_tilt(m_player2);
        clear_tilt();
        trace_flush();
        Logic::get().on_quit(this);
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
            Logic::get().draw_hitboxes(this);
        }
    }

    void postUpdate(float dt) {
        LevelEditorLayer::postUpdate(dt);
        if (Logic::get().hide_p1 && m_player1)
            m_player1->setVisible(false);
        if (Logic::get().hide_p2 && m_player2)
            m_player2->setVisible(false);
        Logic::get().flush_click_hitboxes(this);
        Logic::get().draw_hitboxes(this);
    }

    void updateDebugDraw() {
        LevelEditorLayer::updateDebugDraw();
        Logic::get().draw_hitboxes(this);
    }
};

class $modify(EchoPauseLayer, PauseLayer) {
    void customSetup() {
        Logic::get().pause_known = false;
        PauseLayer::customSetup();
    }

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

int vanilla_physics_steps(float dt, float time_warp);

#ifndef GEODE_IS_WINDOWS
bool plausible_object(const GameObject* object) {
    auto bits = reinterpret_cast<uintptr_t>(object) & 0x00ffffffffffffffull;   // drop the tag android puts in the top byte
    return bits >= 0x10000 && bits < 0x0000800000000000ull && (bits & 7) == 0; // looks like a real pointer
}

bool section_holds(GJBaseGameLayer* layer, GameObject* object) {
    auto in_range = [](const auto& list, int index) {
        return index >= 0 && (size_t)index < list.size();
    };
    int x = object->m_outerSectionIndex, y = object->m_middleSectionIndex;
    int slot = object->m_someOtherIndex;
    if (!in_range(layer->m_sections, x) || !in_range(layer->m_sectionSizes, x))
        return false;
    auto* column = layer->m_sections[x];
    auto* sizes = layer->m_sectionSizes[x];
    if (!column || !sizes || !in_range(*column, y) || !in_range(*sizes, y) || !(*column)[y])
        return false;
    const auto& bucket = *(*column)[y];
    int amt = (*sizes)[y];
    return amt > 0 && (size_t)amt <= bucket.size() && slot >= 0 && slot < amt && bucket[slot] == object && plausible_object(bucket[amt - 1]);
}
#endif

GJBaseGameLayer* step_begin(GJBaseGameLayer* self) {
    auto& logic = Logic::get();
    if (!logic.predicting)
        Orbs::begin_step();
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
    if (layer && !self->m_playerDied && self->m_resumeTimer <= 0)
        Logic::get().run_follow(layer);
    if (play && !self->m_playerDied && self->m_resumeTimer <= 0 && !logic.predicting)
        Logic::get().tick_straight_ufo(play);
    return layer;
}

void step_end(GJBaseGameLayer* self, GJBaseGameLayer* layer, bool is_half_tick) {
    auto& logic = Logic::get();
    auto* play = Logic::as_play(self);
    if (play)
        forecast::note_live_step();

    if (!layer) {
        dash_spin_post(is_half_tick);
        return;
    }
    if (self->m_resumeTimer > 0) {
        dash_spin_post(is_half_tick);
        return;
    }

    if (!self->m_playerDied) {
        logic.step_frame++;
        if (play && !logic.progress_base_set) {
            logic.progress_base = (int)self->m_gameState.m_currentProgress - logic.step_frame;
            logic.progress_base_set = true;
        }
        Logic::get().apply_rate(logic.step_frame);
        if (!logic.hitbox_trail_visual)
            Logic::get().sample_hitbox_trail(layer);
    }
    if (logic.macrobuff_tint && !play)
        Logic::get().record_macrobuff_player_hitboxes(layer, self->m_playerDied);
    if (play)
        trace_step(self);
    if (logic.ignore_record_frames > 0)
        --logic.ignore_record_frames;

    logic.buttons_prepared = false;
    if (play) {
        Logic::get().tick_autoclicker(play);
        Logic::get().tick_straight(play);
    }
    dash_spin_post(is_half_tick);
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

#ifdef GEODE_IS_WINDOWS
    void destructor() {
        drop_broken_sections(this);
        reinterpret_cast<void (*)(GJBaseGameLayer*)>(geode::base::get() + 0x2057a0)(this); // the real ~GJBaseGameLayer
    }
#endif

    void update(float dt) {
        auto& logic = Logic::get();
        if (logic.skip_physics)
            return;
        // death HAS to reach the original or auto retry freezes
        if (auto* play = Logic::as_play(this); play && play->m_playerDied) {
            logic.steps_this_tick = (unsigned)vanilla_physics_steps(dt, m_gameState.m_timeWarp);
            ++g_updates_run;
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
            logic.steps_this_tick = (unsigned)vanilla_physics_steps(dt, m_gameState.m_timeWarp);
            ++g_updates_run;
            GJBaseGameLayer::update(dt);
            return;
        }
        if (logic.disable_shakes || logic.layout_mode)
            m_skipCameraShake = true;
        if (!locked && logic.steps_left == 0) {
            logic.steps_this_tick = (unsigned)std::max(Logic::get().count_unlocked_steps(layer, dt), 0);
            if (logic.steps_this_tick == 0)
                return;
        }
        else if (!locked) {
            logic.steps_this_tick = (unsigned)std::max(logic.steps_left, 0);
        }
        else {
            logic.steps_this_tick = (unsigned)std::max(logic.steps_left, 0);
        }
        ++g_updates_run;
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
        const auto& logic = Logic::get();
        if (logic.refresh_only)
            return 0.0;
        if (Logic::get().defer_physics())
            return original;
        float warp = std::min(m_gameState.m_timeWarp, 1.f);
        double rate = logic.sample_rate();
        if (!std::isfinite(warp) || warp < 0.f || !std::isfinite(rate) || !(rate > 0.0))
            return 0.0;
        // the length of ONE step is worked out in double & only then rounded to a float, the way silicate does it for a single step.
        // rounding 1/tps to a float first & multiplying by the warp after is 1 bit off under a time warp, & that adds up over a level.
        // the batch is that float times the count, so gd dividing it back out gets the exact same step every time
        float step = (float)((1.0 / rate) * (double)warp);
        if (!std::isfinite(step) || !(step > 0.f))
            return 0.0;
        return (double)step * (double)logic.steps_this_tick;
    }

    // advanced follow can throw stuff to like 1e12 xd
    template <class Point>
    static bool sectionable(const Point& pos) {
        constexpr float LIMIT = 1e7f;
        return std::isfinite(pos.x) && std::isfinite(pos.y) && std::fabs(pos.x) < LIMIT && std::fabs(pos.y) < LIMIT;
    }

    void addToSection(GameObject* object) {
        if (!object)
            return;
        const auto pos = object->getPosition();
        if (!sectionable(pos))
            return;
        if (Logic::get().is_traj_player(object))
            return;
        GJBaseGameLayer::addToSection(object);
    }

    void updateObjectSection(GameObject* object) {
        if (!object)
            return;
        const auto pos = object->getPosition();
        if (!sectionable(pos))
            return;
        if (Logic::get().is_traj_player(object))
            return;
        GJBaseGameLayer::updateObjectSection(object);
    }

#ifdef GEODE_IS_WINDOWS
    // checkpoint restore moves an object without fixing its section
    // then gd removes it from slot -1 & crashes
    void removeObjectFromSection(GameObject* object) {
        if (!object)
            return;
        auto* raw = reinterpret_cast<unsigned char*>(object);
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
        auto* layer_base = reinterpret_cast<unsigned char*>(this);
        auto* object_type = reinterpret_cast<int*>(raw + 0x3a0);
        bool block = *object_id == 1816;
        bool has_second = block || *object_type != 7;
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
            auto* columns = *reinterpret_cast<unsigned char***>(layer_base + grid_field);
            auto* columns_end = *reinterpret_cast<unsigned char***>(layer_base + grid_field + sizeof(void*));
            auto* size_columns = *reinterpret_cast<unsigned char***>(layer_base + grid_sizes);
            auto* size_columns_end = *reinterpret_cast<unsigned char***>(layer_base + grid_sizes + sizeof(void*));
            if (!columns || !size_columns || *section_x >= columns_end - columns || *section_x >= size_columns_end - size_columns || !columns[*section_x] || !size_columns[*section_x])
                return;
            auto* column = columns[*section_x];
            auto* rows = *reinterpret_cast<unsigned char***>(column);
            auto* rows_end = *reinterpret_cast<unsigned char***>(column + sizeof(void*));
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
            int stored = *end_field > *begin_field ? (int)(*end_field - *begin_field) : 0;
            int used = *amt;
            return used >= 1 && used <= stored && *block_slot >= 0 && *block_slot < used && (*begin_field)[*block_slot] == object && pointer_ok((*begin_field)[used - 1]);
        };
        if (*object_id != 1816 && *section_x >= 0 && *section_y >= 0 && *slot >= 0) {
            auto* base = reinterpret_cast<unsigned char*>(this);
            auto* columns = *reinterpret_cast<unsigned char***>(base + 0x3598); // the section grid
            auto* columns_end = *reinterpret_cast<unsigned char***>(base + 0x3598 + sizeof(void*));
            auto* size_columns = *reinterpret_cast<unsigned char***>(base + 0x3640); // how many are in each section
            auto* size_columns_end = *reinterpret_cast<unsigned char***>(base + 0x3640 + sizeof(void*));
            if (columns && size_columns && *section_x < columns_end - columns && *section_x < size_columns_end - size_columns &&
                columns[*section_x] && size_columns[*section_x]) {
                auto* column = columns[*section_x];
                auto* rows = *reinterpret_cast<unsigned char***>(column);
                auto* rows_end = *reinterpret_cast<unsigned char***>(column + sizeof(void*));
                auto* size_column = size_columns[*section_x];
                auto* size_rows = *reinterpret_cast<int**>(size_column);
                auto* size_rows_end = *reinterpret_cast<int**>(size_column + sizeof(void*));
                if (rows && size_rows && *section_y < rows_end - rows && *section_y < size_rows_end - size_rows && rows[*section_y]) {
                    auto* bucket = rows[*section_y];
                    auto* objects = *reinterpret_cast<GameObject***>(bucket);
                    auto* objects_end = *reinterpret_cast<GameObject***>(bucket + sizeof(void*));
                    int stored = objects && objects_end > objects ? (int)(objects_end - objects) : 0;
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
            int size = begin && end > begin ? (int)(end - begin) : 0;
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
                    auto* idx = reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(item) + 0x274);
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
        auto* base = reinterpret_cast<unsigned char*>(this);
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
            auto* columns = *reinterpret_cast<unsigned char***>(base + 0x3598);
            auto* columns_end = *reinterpret_cast<unsigned char***>(base + 0x3598 + sizeof(void*));
            auto* size_columns = *reinterpret_cast<unsigned char***>(base + 0x3640);
            auto* size_columns_end = *reinterpret_cast<unsigned char***>(base + 0x3640 + sizeof(void*));
            main_ok = columns && size_columns && *section_x < columns_end - columns && *section_x < size_columns_end - size_columns && columns[*section_x] && size_columns[*section_x];
            if (main_ok) {
                auto* column = columns[*section_x];
                auto* rows = *reinterpret_cast<unsigned char***>(column);
                auto* rows_end = *reinterpret_cast<unsigned char***>(column + sizeof(void*));
                auto* size_column = size_columns[*section_x];
                auto* size_rows = *reinterpret_cast<int**>(size_column);
                auto* size_rows_end = *reinterpret_cast<int**>(size_column + sizeof(void*));
                main_ok = rows && size_rows && *section_y < rows_end - rows && *section_y < size_rows_end - size_rows && rows[*section_y];
                if (main_ok) {
                    auto* bucket = rows[*section_y];
                    auto* objects = *reinterpret_cast<GameObject***>(bucket);
                    auto* objects_end = *reinterpret_cast<GameObject***>(bucket + sizeof(void*));
                    auto* amt = &size_rows[*section_y];
                    int stored = objects && objects_end > objects ? (int)(objects_end - objects) : 0;
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
                        auto* idx = reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(item) + 0x270);
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
        int saved_id = *object_id, saved_type = *object_type;
        bool spoof = has_second && *block_slot < 0;
        if (spoof) {
            *object_id = 0;
            *object_type = 7;
        }
        GJBaseGameLayer::removeObjectFromSection(object);
        if (spoof) {
            *object_id = saved_id;
            *object_type = saved_type;
        }
    }

#else
    void removeObjectFromSection(GameObject* object) {
        if (!object)
            return;
        if (section_holds(this, object)) {
            GJBaseGameLayer::removeObjectFromSection(object);
            return;
        }
        object->m_someOtherIndex = -1;
        object->m_outerSectionIndex = -1;
        object->m_middleSectionIndex = -1;
    }
#endif

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto& logic = Logic::get();
        if (logic.refresh_only)
            return;
        if (logic.predicting)
            return;
        auto* layer = step_begin(this);
        logic.in_step = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        logic.in_step = false;
        step_end(this, layer, isHalfTick);
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
        Logic::get().flatten_click_times(this);
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
        bool flips = value.flip_controls() && !m_isPlatformer && !m_useReplay; // same checks gd does (handleButton, ios 0x1fabbc)
        bool mine = !value.applying_playback && !value.routing_input && !value.restoring_holds && !value.maintaining_gravity && !value.injecting && !value.follow_injecting && !orb_swift;
        struct AssistScope {
            bool before = Logic::get().assist_input;
            ~AssistScope() { Logic::get().assist_input = before; }
        } assist_scope;
        if (mine && (button == 0 || button == JUMP)) {
            for (auto* counts : { &value.assist_clicks, &value.restored_clicks }) {
                int& left = (*counts)[player1 ? 0 : 1][down ? 1 : 0];
                if (left > 0) {
                    --left;
                    value.assist_input = true;
                    mine = false;
                    break;
                }
            }
        }
        if (flips && mine && m_levelSettings && m_levelSettings->m_twoPlayerMode)
            player1 = !player1;
        bool auto_swift = value.auto_swift && mine && (button == 0 || button == JUMP) && !(value.is_playing() && !value.is_recording());
        if (auto_swift && !down)
            return;
        if (tracing())
            trace_event(fmt::format("H frame={} down={} p1={} button={} playback={} swift={} inject={} routing={} restoring={} gravity={} held_p1={} held_p2={}\n",
                value.step_frame + 1, down, player1, button, value.applying_playback, orb_swift, value.injecting, value.routing_input, value.restoring_holds, value.maintaining_gravity,
                m_player1 && Logic::button_held(m_player1, JUMP), m_player2 && Logic::button_held(m_player2, JUMP)));
        auto* live = Logic::as_play(this);
        if (!down && ((live && live->m_isPaused) || Logic::get().pause_menu_open()) && (value.is_playing() || value.is_recording())) {
            if (tracing())
                trace_event(fmt::format("DROP frame={} release p1={} paused={} pause_menu={} pause_known={}\n",
                    value.step_frame + 1, player1, live && live->m_isPaused, Logic::get().pause_menu_open(), value.pause_known));
            return;
        }
        bool jump_button_early = button == 0 || button == JUMP;
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
            int key = button == 0 ? JUMP : button;
            if (!Logic::get().held_at(true, value.step_frame + 1, key) &&
                Logic::get().held_at(false, value.step_frame + 1, key))
                player1 = true;
        }
        {
            bool move_button = button == 0 || button == JUMP || button == LEFT || button == RIGHT;
            if (move_button && !value.follow_injecting && !value.restoring_holds && !value.maintaining_gravity && !value.injecting && Logic::get().follow_active(this)) {
                bool leader1 = value.follow_mode == 1;
                if (!value.applying_playback) {
                    player1 = leader1;
                    value.note_follow_press(this, down, button == 0 ? JUMP : button);
                }
                else if (value.backstep_target >= 0 && player1 == leader1) {
                    value.note_follow_press(this, down, button == 0 ? JUMP : button);
                }
            }
        }
        bool opened_button = !in_orb_button;
        in_orb_button = true;
        bool steered = false;
        if (!value.applying_playback && !value.routing_input && !value.restoring_holds && !value.maintaining_gravity) {
            steered = Logic::get().steer_player(player1);
            bool other_played = value.plays_player(player1);
            if (!steered && value.click_both && !value.assist_input && !other_played && !value.plays_player(!player1) && (button == 0 || button == JUMP || button == LEFT || button == RIGHT)) {
                value.routing_input = true;
                handleButton(down, button, !player1);
                value.routing_input = false;
            }
        }
        bool jump_button = button == 0 || button == JUMP;
        bool my_click = !value.applying_playback && !value.restoring_holds && !value.maintaining_gravity && !value.injecting &&
                        !value.follow_injecting && !orb_swift && !value.assist_input;
        if (my_click && jump_button) {
            bool two = m_levelSettings && m_levelSettings->m_twoPlayerMode;
            if (!player1 && two ? value.inverse_p2 : value.inverse_p1)
                down = !down;
        }
        int swifts = 0;
        if (down && jump_button && !orb_swift && !value.applying_playback && !value.injecting && !value.restoring_holds && !value.maintaining_gravity) {
            PlayerObject* body = player1 ? m_player1 : m_player2;
            swifts = Orbs::claim_click(this, body, !player1);
            if (value.extra_clicks && !value.routing_input && !value.follow_injecting && !(value.is_playing() && !value.is_recording()))
                swifts += std::clamp(value.extra_click_count, 1, 16);
        }
        if (swifts > 0)
            Orbs::arm_tool(true);
        if ((button == 0 || button == JUMP) && !orb_swift && !value.applying_playback && !value.maintaining_gravity && !value.restoring_holds && !value.resetting && !down) {
            value.restart_hold_captured = false;
            value.death_jump_p1 = value.death_jump_p2 = false;
        }
        if (button == JUMP && !value.suppress_click_hitbox && !value.maintaining_gravity && !value.restoring_holds) {
            Trajectory::get().note_button(player1, down);
            if (auto* play = Logic::as_play(this)) {
                bool two_player = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
                if (two_player) {
                    if (player1 || m_gameState.m_isDualMode)
                        Logic::get().play_click(play, down, player1);
                }
                else if (!Logic::get().routing_input) {
                    Logic::get().play_click(play, down, true);
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
                Logic::get().record_input(play, down, player1, button, orb_swift || Logic::record_every_input());
            }
        }
        else if (Logic::get().editor_testing && Logic::as_editor(this)) {
            auto& logic = Logic::get();
            if (!logic.applying_playback && logic.is_recording() && !logic.injecting && !logic.restoring_holds) {
                Logic::get().record_input(this, down, player1, button, orb_swift || Logic::record_every_input());
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
#ifdef GEODE_IS_IOS
            if (value.applying_playback || orb_swift)
                m_allowedButtons.clear();
#endif
#ifdef GEODE_IS_ANDROID64
            // android has the same list, in isButtonAllowed (0x88370c in the arm64 lib off my phone)
            if (value.applying_playback || orb_swift)
                m_allowedButtons.clear();
#endif
            auto* manager = GameManager::get();
            if (flips && manager)
                manager->setGameVariable("0010", false);
            GJBaseGameLayer::handleButton(down, button, player1);
            if (flips && manager)
                manager->setGameVariable("0010", true);
        }
        if ((button == 0 || button == JUMP || button == LEFT || button == RIGHT) && !orb_swift && !value.maintaining_gravity && !value.suppress_click_hitbox && !value.restoring_holds) {
            Logic::get().note_click_hitbox(this, player1, down);
        }
        if (swifts > 0) {
            Orbs::arm_tool(false);
            orb_swift = true;
            bool was_routing = value.routing_input;
            value.routing_input = true;
            for (int i = 0; i < swifts; ++i) {
                handleButton(false, JUMP, player1);
                handleButton(true, JUMP, player1);
            }
            value.routing_input = was_routing;
            orb_swift = false;
        }
        if (auto_swift && down) {
            orb_swift = true;
            bool was_routing = value.routing_input;
            value.routing_input = true;
            handleButton(false, JUMP, player1);
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

// not on ios it crashes there
#ifndef GEODE_IS_IOS
    void activatedAudioTrigger(SFXTriggerGameObject* object) {
        if (Logic::get().predicting)
            return;
        GJBaseGameLayer::activatedAudioTrigger(object);
    }
#endif

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

    CCParticleSystemQuad* spawnParticle(const char* plist, int zOrder, tCCPositionType positionType, CCPoint position) {
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
        if (Logic::get().layout_active() && !Logic::get().layout_use_regular_bg)
            background = 13;
        GJBaseGameLayer::createBackground(background);
    }

    void createMiddleground(int middleground) {
        if (!Logic::get().layout_active() || !Logic::get().layout_use_regular_bg) {
            GJBaseGameLayer::createMiddleground(middleground);
            return;
        }
        if (m_middleground) {
            m_middleground->removeFromParent();
            m_middleground = nullptr;
        }
    }

    void createGroundLayer(int ground, int line) {
        if (!Logic::get().layout_active() || !Logic::get().layout_use_regular_bg) {
            GJBaseGameLayer::createGroundLayer(ground, line);
            return;
        }
        GJBaseGameLayer::createGroundLayer(18, 2);
    }

    void updateColor(ccColor3B& color, float fadeTime, int colorID, bool blending, float opacity, ccHSVValue& copyHSV, int colorIDToCopy, bool copyOpacity, EffectGameObject* callerObject, int unk1, int unk2) {
        if (Logic::get().layout_active())
            return;
        GJBaseGameLayer::updateColor(color, fadeTime, colorID, blending, opacity, copyHSV, colorIDToCopy, copyOpacity, callerObject, unk1, unk2);
    }

    void triggerGradientCommand(GradientTriggerObject* object) {
        if (Logic::get().layout_active())
            return;
        GJBaseGameLayer::triggerGradientCommand(object);
    }
};

class $modify(EchoUILayer, UILayer) {
#ifdef GEODE_IS_MACOS
    void resetAllButtons() {
        const auto& logic = Logic::get();
        if (logic.resetting || logic.is_playing() || logic.is_recording())
            return;
        UILayer::resetAllButtons();
    }
#endif

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        if (Logic::get().pause_menu_open())
            return false;
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
        long long dash_effect_step = std::numeric_limits<long long>::min();
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
        if (Logic::bot_disabled()) {
            PlayerObject::update(dt);
            trace_plain(this);
            return;
        }
        if (Trajectory::get().isFake(this) && !Logic::get().predicting)
            return;
        if (!Trajectory::get().isFake(this))
            Logic::get().capture_pending_click(this);
        if (Logic::get().suppress_auto_hold && !Trajectory::get().isFake(this)) {
            auto& logic = Logic::get();
            auto it = m_holdingButtons.find(JUMP);
            if (it != m_holdingButtons.end() && it->second)
                PlayerObject::releaseButton(PlayerButton::Jump);
            m_jumpBuffered = false;
            bool key_down = Input::jump_key_down(), mouse_down = Input::mouse_down(), ui_down = false;
            if (auto* layer = GJBaseGameLayer::get()) {
                if (auto* ui = layer->m_uiLayer) {
                    ui_down = ui->m_p1Jumping || ui->m_p2Jumping || ui->m_p1TouchId != -1 || ui->m_p2TouchId != -1;
                }
            }
            if (!key_down && !mouse_down && !ui_down)
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

#ifndef GEODE_IS_IOS
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
#endif

    void setPosition(const CCPoint& position) {
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

// not on ios the function is too short to hook :/
#ifndef GEODE_IS_IOS
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
#endif

    void ringJump(RingObject* ring, bool skipCheck) {
        if (Orbs::blockHoldRing(this, ring))
            return;
        if (Trajectory::get().isFake(this)) {
            phys::ringJump(this, ring);
            return;
        }
        struct RingNote {
            PlayerObject* player; RingObject* ring; bool on, dashing, flipped; double vel;
            RingNote(PlayerObject* p, RingObject* r) : player(p), ring(r), on(tracing()), dashing(p->m_isDashing), flipped(p->m_isUpsideDown), vel(p->m_yVelocity) {}
            ~RingNote() {
                if (!on || !ring || (dashing == player->m_isDashing && flipped == player->m_isUpsideDown && vel == player->m_yVelocity))
                    return;
                auto* layer = GJBaseGameLayer::get();
                trace_event(fmt::format("ORB frame={} p2={} id={} uid={} type={} dashing={}->{} upside_down={}->{} yvel={:.4f}->{:.4f} held={} x={:.3f} y={:.4f}\n",
                    Logic::get().step_frame + 1, layer && player == layer->m_player2, ring->m_objectID, ring->m_uniqueID, (int)ring->m_objectType,
                    dashing, player->m_isDashing, flipped, player->m_isUpsideDown, vel, player->m_yVelocity,
                    Logic::button_held(player, JUMP), player->getPositionX(), player->getPositionY()));
            }
        } ring_note{ this, ring };
        bool hidden = Logic::get().player_hidden(this);
        EffectScope scope;
        auto ring_at = ring ? ring->getPosition() : CCPointZero;
        bool fresh = false;
        auto* seen = ring && m_playEffects && !hidden ? effect_slot({ this, 3, ring->m_uniqueID }, &ring_at, fresh) : nullptr;
        if (!hidden && (!seen || fresh)) {
            PlayerObject::ringJump(ring, skipCheck);
            // touching it calls ts every step w/o jumping, those cant take the slot
            if (seen && seen->wave_amt == 0)
                seen->key = {};
            return;
        }
        bool was_quiet = std::exchange(quiet_orb, true);
        auto duds = g_dud_waves.size();
        bool dashing = m_isDashing, flipped = m_isUpsideDown;
        double vel = m_yVelocity;
        PlayerObject::ringJump(ring, skipCheck);
        quiet_orb = was_quiet;
        bool jumped = g_dud_waves.size() > duds || dashing != m_isDashing || flipped != m_isUpsideDown || vel != m_yVelocity;
        if (seen && jumped)
            count_repeat(seen, &ring_at);
        drop_dud_waves();
    }

    void bumpPlayer(float force, int objectType, bool playEffect, GameObject* object) {
        if (Trajectory::get().isFake(this)) {
            if (Logic::get().predicting) {
                if (auto* play = PlayLayer::get())
                    Orbs::notePredictionPad(play, this, object);
            }
            phys::bumpPlayer(this, force, objectType, playEffect, object);
            return;
        }
        if (auto* play = PlayLayer::get())
            Orbs::click_pads(play, this, objectType, object);
        PlayerObject::bumpPlayer(force, objectType, playEffect, object);
    }

    void propellPlayer(float force, bool noEffects, int objectType) {
        if (Trajectory::get().isFake(this)) {
            if (Logic::get().predicting) {
                if (auto* play = PlayLayer::get())
                    Orbs::notePredictionPad(play, this, m_lastActivatedPortal);
            }
            phys::propellPlayer(this, force, noEffects, objectType);
            return;
        }
        if (auto* play = PlayLayer::get())
            Orbs::click_pads(play, this, objectType, m_lastActivatedPortal);
        PlayerObject::propellPlayer(force, noEffects, objectType);
    }

    void startDashing(DashRingObject* ring) {
        if (Trajectory::get().isFake(this)) {
            phys::startDashing(this, ring);
            return;
        }
        if (tracing()) {
            auto* layer = GJBaseGameLayer::get();
            trace_event(fmt::format("DASH_START frame={} p2={} id={} uid={} was_dashing={} x={:.3f} y={:.4f}\n", Logic::get().step_frame + 1,
                layer && this == layer->m_player2, ring ? ring->m_objectID : 0, ring ? ring->m_uniqueID : 0, m_isDashing, getPositionX(), getPositionY()));
        }
        bool loud = m_playEffects;
        EffectScope scope;
        bool repeat = loud && ring && spread_effect({ this, 0, ring->m_uniqueID });
        if (repeat && ring && !m_isDashing) {
            if (auto first = g_boom_first.find({ ring, this }); first != g_boom_first.end() && first->second.sprite)
                stack_offset(first->second, ring->getPosition());
        }
        if (Logic::get().no_effects || repeat || Logic::get().player_hidden(this))
            m_playEffects = false;
        bool capture = !repeat && m_playEffects && ring && !m_isDashing && g_effect_frame.on && opt_effects;
        g_boom_capture = capture;
        g_boom_captured = nullptr;
        PlayerObject::startDashing(ring);
        g_boom_capture = false;
        if (capture && g_boom_captured && !g_boom_captured->getBatchNode()) {
            g_boom_first[{ ring, this }] = { g_boom_captured, ring->getPosition() };
            keep_first(g_boom_captured);
        }
        g_boom_captured = nullptr;
        m_playEffects = loud;
        if (auto* play = PlayLayer::get())
            Orbs::note_dash(play, this, ring);
    }

    void stopDashing() {
        if (Trajectory::get().isFake(this)) {
            phys::stopDashing(this);
            return;
        }
        bool was_dashing = m_isDashing;
        auto* node = m_fields->tick_spin.data();
        bool use_tick = dash_spin_enabled() && was_dashing && m_iconSprite && node && dash_spin_manager()->numberOfRunningActionsInTarget(node) > 0;
        float visual_spin = m_iconSprite ? m_iconSprite->getRotation() : 0.f;
        float tick_spin = use_tick ? node->getRotation() : visual_spin;
        if (use_tick)
            m_iconSprite->setRotation(node->getRotation());
        stop_tick_spin();
        struct DashNote {
            PlayerObject* player; bool was; float visual, tick; bool used;
            ~DashNote() {
                if (!was || Logic::get().predicting)
                    return;
                auto* layer = GJBaseGameLayer::get();
                trace_event(fmt::format("DASH_END frame={} p2={} visual_spin={:.4f} tick_spin={:.4f} used_tick={} rot_after={:.4f} yvel={:.4f} x={:.3f} y={:.4f}\n",
                    Logic::get().step_frame + 1, layer && player == layer->m_player2, visual, tick, used, player->getRotation(), player->m_yVelocity, player->getPositionX(), player->getPositionY()));
            }
        } dash_note{ this, was_dashing, visual_spin, tick_spin, use_tick };
        bool reduced = m_maybeReducedEffects;
        EffectScope scope;
        bool repeat = was_dashing && !reduced && spread_effect({ this, 1, 0 });
        if (repeat) {
            if (auto first = g_after_first.find(this); first != g_after_first.end() && first->second.sprite)
                stack_offset(first->second, getPosition());
        }
        if (Logic::get().no_effects || repeat)
            m_maybeReducedEffects = true;
        bool capture = was_dashing && !repeat && !m_maybeReducedEffects && g_effect_frame.on && opt_effects && !Logic::get().no_effects;
        auto at = getPosition();
        g_boom_captured = nullptr;
        if (capture)
            g_boom_captured = added_effect([&] { PlayerObject::stopDashing(); });
        else
            PlayerObject::stopDashing();
        if (capture && g_boom_captured && !g_boom_captured->getBatchNode()) {
            g_after_first[this] = { g_boom_captured, at };
            keep_first(g_boom_captured);
        }
        g_boom_captured = nullptr;
        m_maybeReducedEffects = reduced;
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
        if (play && !Trajectory::get().isFake(this) && (logic.is_playing() || logic.is_recording()) && (play->m_isPaused || Logic::get().pause_menu_open())) {
            if (tracing())
                trace_event(fmt::format("DROP frame={} releaseButton p2={} paused={} pause_menu={}\n",
                    logic.step_frame + 1, this == play->m_player2, play->m_isPaused, Logic::get().pause_menu_open()));
            return;
        }
        if (!Trajectory::get().isFake(this))
            if (tracing()) {
                auto* layer = GJBaseGameLayer::get();
                const auto& value = Logic::get();
                trace_event(fmt::format("R frame={} p2={} button={} upside_down={} buffered={} maintaining={} playback={} restoring={} injecting={} physical={}\n",
                    value.step_frame + 1, layer && this == layer->m_player2, (int)button, m_isUpsideDown, m_jumpBuffered, value.maintaining_gravity,
                    value.applying_playback, value.restoring_holds, value.injecting, layer ? Logic::get().physical_hold(layer, layer && this == layer->m_player2) : false));
            }
        note_partner();
        PlayerObject::releaseButton(button);
    }

    bool pushButton(PlayerButton button) {
        auto& logic = Logic::get();
        if (!Trajectory::get().isFake(this))
            if (tracing()) {
                auto* layer = GJBaseGameLayer::get();
                const auto& value = Logic::get();
                trace_event(fmt::format("P frame={} p2={} button={} upside_down={} buffered={} maintaining={} playback={} restoring={} injecting={} physical={}\n",
                    value.step_frame + 1, layer && this == layer->m_player2, (int)button, m_isUpsideDown, m_jumpBuffered, value.maintaining_gravity,
                    value.applying_playback, value.restoring_holds, value.injecting, layer ? Logic::get().physical_hold(layer, layer && this == layer->m_player2) : false));
            }
        if (logic.suppress_auto_hold && button == PlayerButton::Jump && !Trajectory::get().isFake(this) && !logic.maintaining_gravity && !logic.applying_playback && !logic.injecting) {
            return false;
        }
        note_partner();
        bool track = button == PlayerButton::Jump && !Trajectory::get().isFake(this);
        if (track)
            Orbs::begin_push(this);
        auto finish = [&](bool pressed) {
            if (track)
                Orbs::end_push(this);
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
                swifts = Orbs::claim_click(layer, this, !player1);
            }
        }
        if (swifts > 0)
            Orbs::arm_tool(true);
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
        Orbs::arm_tool(false);
        orb_swift = true;
        for (int i = 0; i < swifts; ++i) {
            releaseButton(PlayerButton::Jump);
            if (layer && logic.is_recording())
                Logic::get().record_input(layer, false, player1, JUMP, true);
            pushButton(PlayerButton::Jump);
            if (layer && logic.is_recording())
                Logic::get().record_input(layer, true, player1, JUMP, true);
        }
        orb_swift = false;
        return pressed;
    }

    void releaseAllButtons() {
        auto* layer = GJBaseGameLayer::get();
        auto* play = Logic::as_play(layer);
        auto& logic = Logic::get();
#ifdef GEODE_IS_MACOS
        if (logic.resetting && play)
            return;
#endif
        if (logic.resetting || logic.suppress_auto_hold || !play) {
            PlayerObject::releaseAllButtons();
            return;
        }
        if ((logic.is_playing() || logic.is_recording()) && (play->m_isPaused || Logic::get().pause_menu_open()))
            return;
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
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this) || Logic::get().no_effects)
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
        if (Logic::get().predicting || Logic::get().is_traj_player(this) || Logic::get().player_hidden(this) || Logic::get().no_effects)
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

#ifdef GEODE_IS_IOS
constexpr std::uintptr_t ROTATE_BODY = 0x21d4a4;

static void ios_rotate_body(PlayerObject* self, float dt, float rotation) {
    auto original = reinterpret_cast<void (*)(PlayerObject*, float, float)>(geode::base::get() + ROTATE_BODY);
    restore_tilt(self);
    if (!tilt_applies(self)) {
        original(self, dt, rotation);
        return;
    }
    auto turn = [&] {
        original(self, dt, rotation);
    };
    turn_both(self, turn, turn);
}

$execute {
    auto hooked = GEODE_MOD_STATIC_HOOK(0x21d4a4, &ios_rotate_body, PlayerObject::updateRotation_body);
}
#endif

class $modify(EchoSchedulerDepth, CCScheduler) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("cocos2d::CCScheduler::update", Priority::First);
    }

    void update(float dt) {
        auto& logic = Logic::get();
        bool outer = logic.scheduler_depth == 0;
        auto began = outer ? chrono::steady_clock::now() : chrono::steady_clock::time_point{};
        ++logic.scheduler_depth;
        CCScheduler::update(dt);
        --logic.scheduler_depth;
        if (outer)
            logic.scheduler_time = chrono::duration<double>(chrono::steady_clock::now() - began).count();
    }
};

class $modify(EchoScheduler, CCScheduler) {
    void update(float dt) {
        struct EffectsFrame {
            bool outer;
            EffectsFrame() : outer(Logic::get().scheduler_depth <= 1 && !Logic::get().in_scheduler && !Logic::get().refresh_only) {
                if (outer)
                    begin_effects();
            }
            ~EffectsFrame() {
                if (outer)
                    end_effects();
            }
        } effects_frame;
        bool timed_frame = effects_frame.outer;
        if (timed_frame)
            perf_frame_start();
        struct LogicDone {
            bool on;
            ~LogicDone() {
                if (on)
                    perf_logic_done();
            }
        } logic_done{ timed_frame };
        Logic::get().sync_predict_hooks();
        keep_menu_cursor();
        auto& logic = Logic::get();
        if (logic.refresh_only) {
            CCScheduler::update(dt);
            return;
        }
        auto* play = PlayLayer::get();
        if (logic.editor_testing && !LevelEditorLayer::get()) {
            logic.editor_testing = false;
            logic.editor_paused = false;
        }
        GJBaseGameLayer* layer = play;
        if (!layer && logic.editor_testing)
            layer = LevelEditorLayer::get();
        if (layer) {
            restore_tilt(layer->m_player1);
            restore_tilt(layer->m_player2);
        }
        bool pause_menu = Logic::get().pause_menu_open();
        struct PauseKnown {
            PauseKnown(bool open) {
                Logic::get().pause_cached = open;
                Logic::get().pause_known = true;
            }
            ~PauseKnown() {
                Logic::get().pause_known = false;
            }
        } pause_known{ pause_menu };
        static bool was_paused = false;
        if (pause_menu && play) {
            play->m_isPaused = true;
            if (!was_paused)
                GUI::get().release_stuck_mouse();
        }
        was_paused = pause_menu && play;
        if (logic.resume_pending && play && !pause_menu) {
            logic.resume_pending = false;
            play->m_isPaused = false;
            play->m_resumeTimer = 0;
            play->m_extraDelta = 0.0;
            logic.carry = 0.f;
            if (auto* director = CCDirector::sharedDirector())
                director->setNextDeltaTimeZero(false);
            Logic::get().retain_playback_hold(play);
        }
        if (Logic::get().render_capturing() && logic.frame_advance) {
            logic.frame_advance = false;
            logic.holding_advance = false;
            logic.advance_queued = false;
        }
        Logic::get().poll_advance_hold();
        Logic::get().poll_back_hold();
        Logic::get().poll_hold_block();
        Logic::get().poll_autosave();
        Logic::get().sync_unrounding();
        Logic::get().sync_time_scale();
        Logic::get().sync_playback_fps();
        Logic::get().sync_music_pitch();

        if (play && !pause_menu && play->m_tryPlaceCheckpoint && play->m_started && play->m_player1 && !play->m_playerDied) {
            play->m_tryPlaceCheckpoint = false;
            play->markCheckpoint();
        }

        logic.save_shown_step = logic.frame_advance && logic.backstep;
        int frames_before = logic.step_frame;
        logic.in_scheduler = true;
        {
            PerfScope physics_time(PERF_PHYSICS);
            Logic::get().run_frame(layer, dt, [this](float delta) {
                CCScheduler::update(delta);
            });
        }
        logic.in_scheduler = false;
        if (play)
            perf_note_steps(logic.step_frame, play->m_gameState.m_levelTime);
        logic.save_shown_step = false;
        if (layer) {
            bool paused = logic.editor_paused;
            bool dead = false;
            if (auto* live = Logic::as_play(layer)) {
                paused = paused || live->m_isPaused;
                dead = live->m_playerDied;
            }
            if (!paused) {
                if (logic.hitbox_trail_visual && !dead && logic.step_frame != frames_before) {
                    ++logic.visual_frame;
                    Logic::get().sample_hitbox_trail(layer);
                }
            }
            if (auto* live = Logic::as_play(layer); live && !paused && !dead)
                Logic::get().note_shown_camera(live, logic.step_frame > frames_before);
            PerfScope hitbox_time(PERF_HITBOXES);
            Logic::get().draw_hitboxes(layer);
        }
        else if (auto* editor = LevelEditorLayer::get(); editor && logic.show_hitboxes) {
            Logic::get().draw_hitboxes(editor);
        }
        Logic::get().update_macrobuff_tint(layer ? nullptr : LevelEditorLayer::get());
        if (logic.refresh_queued)
            Logic::get().flush_visual_refresh();
        Logic::get().flush_clicks();
        if (layer) {
            present_tilt(layer->m_player1);
            present_tilt(layer->m_player2);
        }
    }
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
        if (Logic::get().predicting || Logic::get().no_effects)
            return;
        GameObject::playShineEffect();
    }

    void addGlow(gd::string frame) {
        GameObject::addGlow(std::move(frame));
        Logic::get().style_layout_glow(this);
    }
};

#ifndef GEODE_IS_WINDOWS
class $modify(EchoActionManager, GJActionManager) {
    void updateInternalActions(float dt, bool remove) {
        if (m_internalActions && m_internalActions->count() == 0)
            return;
        GJActionManager::updateInternalActions(dt, remove);
    }
};
#endif

class $modify(EchoEffectObject, EffectGameObject) {
    void playTriggerEffect() {
        if (quiet_orb)
            return;
        EffectGameObject::playTriggerEffect();
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, const gd::vector<int>* remapKeys) {
        if (Logic::get().predicting && !forecast::firing())
            return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }

    void triggerActivated(float xPosition) {
        if (Logic::get().predicting)
            return;
        EffectGameObject::triggerActivated(xPosition);
    }
};

class $modify(EchoRandTrigger, RandTriggerGameObject) {
    struct Fields {
        unsigned long long state = std::numeric_limits<unsigned long long>::max();
    };

    void customObjectSetup(gd::vector<gd::string>& values, gd::vector<void*>& exists) {
        RandTriggerGameObject::customObjectSetup(values, exists);
        Logic::get().note_rand_trigger(m_uniqueID, &m_fields->state);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, const gd::vector<int>* remapKeys) {
        // predicting must NOT roll the real random
        if (Logic::get().predicting && !forecast::firing())
            return;
        if (m_objectID != 2068) {
            if (!Logic::get().predicting)
                trace_note(fmt::format("N frame={} uid={} rand={}\n", Logic::get().step_frame, m_uniqueID, Logic::get().game_random_state()));
            RandTriggerGameObject::triggerObject(layer, uniqueID, remapKeys);
            return;
        }
        if (m_fields->state == std::numeric_limits<unsigned long long>::max()) {
            m_fields->state = Logic::get().lvl_seed ^ ((unsigned long long)m_uniqueID * 2137ull);
        }
        unsigned long long roll = m_fields->state;
        m_fields->state = 214013ull * roll + 2531011ull; // msvc rand()
        int sum = 0;
        for (const auto& chance : m_chanceObjects)
            sum += chance.m_chance;
        // silicate divides by a double 32767.0, so the product is a double & only the roundf arg is a float. all float can round the other way
        int threshold = (int)(roundf((float)((double)((float)((roll >> 16) & 32767)) / 32767.0 * (double)((float)sum))));
        int running = 0;
        int group = 0;
        for (size_t i = 0; i < m_chanceObjects.size(); ++i) {
            running += m_chanceObjects[i].m_chance;
            group = m_chanceObjects[i].m_groupID;
            if (running >= threshold)
                break;
        }
        gd::vector<int> keys;
        if (remapKeys)
            keys = *remapKeys;
        if (!Logic::get().predicting)
            trace_note(fmt::format("A frame={} uid={} roll={} sum={} threshold={} group={}\n", Logic::get().step_frame, m_uniqueID, roll, sum, threshold, group));
        layer->spawnGroup(group, false, 0.0, keys, m_uniqueID, m_controlID);
    }
};

class $modify(EchoEnhancedObject, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* player) {
        if (Trajectory::get().isFake(player)) {
            phys::activateForTrajectory(static_cast<EffectGameObject*>(static_cast<GameObject*>(this)), player);
            return;
        }
        if (Logic::get().trace_logging && !Logic::get().predicting)
            trace_note(fmt::format("ACT frame={} uid={} id={} x={:.3f} y={:.3f} p2={} was={}{} off={} state={}\n", Logic::get().step_frame, m_uniqueID, m_objectID,
                m_positionX, m_positionY, player && player->m_isSecondPlayer, m_activatedByPlayer1 ? 1 : 0, m_activatedByPlayer2 ? 1 : 0, m_isGroupDisabled,
                (int)Logic::get().state));
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
        CCPoint spot = getPosition();
        EffectScope scope;
        if (repeated_effect({ getParent(), 2, m_objectID, spot.x, spot.y }))
            return;
        RingObject::spawnCircle();
    }
};

#ifdef GEODE_IS_WINDOWS
// skidded from silicate (GPL-3.0, gd 2.2081)
// swaps gds step count math in GJBaseGameLayer::update for a load of my count
// HAS to be a raw patch, a midhook saves every register & that changes the ship delta
static Patch* g_step_patch = nullptr;
static std::vector<unsigned char> g_step_bytes;

void install_physics_step_patch() {
    std::vector<unsigned char> bytes = {
        0x49, 0xBB, // movabs r11, <count ptr>
        0, 0, 0, 0, 0, 0, 0, 0,
        0x45, 0x8B, 0x1B,            // mov r11d, [r11]
        0xE9, 0x31, 0x00, 0x00, 0x00 // jmp over the old math
    };
    auto* amt = &Logic::get().steps_this_tick;
    std::memcpy(bytes.data() + 2, &amt, sizeof(amt));
    auto address = reinterpret_cast<void*>(geode::base::get() + 0x237A5B); // in GJBaseGameLayer::update
    if (auto res = geode::Mod::get()->patch(address, bytes); res)
        g_step_patch = res.unwrap();
    g_step_bytes = bytes;
}

// silicate patches the SAME spot with its own count so mine gets ignored (crash at high tps)
// its disable bot button can also put gds old math back
void check_physics_step_patch() {
    auto* at = reinterpret_cast<unsigned char*>(geode::base::get() + 0x237A5B);
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
    if (g_step_patch) {
        (void)g_step_patch->disable();
        (void)g_step_patch->enable();
    }
}
#endif

int vanilla_physics_steps(float dt, float time_warp) {
    float warp = std::min(time_warp, 1.f);
    if (!std::isfinite(warp) || !(warp > 0.f))
        warp = 1.f;
    if (!std::isfinite(dt) || dt < 0.f)
        dt = 0.f;
    int steps = (int)std::lround((double)dt * 60.0 / (double)warp * 4.0);
    return std::max(steps, 1);
}

#ifdef GEODE_IS_ANDROID64
// android version of the step patch, checked on the arm64 lib off my phone
// only the count math changes, timewarp stays in s1 & the 3 delta divisions at 0x89da30 still run
void install_android_physics_step_patch() {
    constexpr std::array<unsigned, 10> original = {
        0x1e62100a, 0x1e6a096a, 0x1e624140, 0x1e264009, 0x1e2e1000,
        0x1e202130, 0x5400758d, 0x1e380120, 0x1e620000, 0xb900e3a0
    };
    auto* address = reinterpret_cast<void*>(geode::base::get() + 0x89d9f8); // in GJBaseGameLayer::update
    if (std::memcmp(address, original.data(), sizeof(original)) != 0) {
        return;
    }
    // ldr x0, <ptr>; ldr w0, [x0]; cmp w0, #1; csinc w0, w0, wzr, ge; scvtf d0, w0; str w0, [x29, #0xe0]; b 0x89da20; nop; <ptr>
    std::array<unsigned, 10> words = {
        0x58000100, 0xb9400000, 0x7100041f, 0x1a9fa400,
        0x1e620000, 0xb900e3a0, 0x14000004, 0xd503201f, 0, 0
    };
    auto* amt = &Logic::get().steps_this_tick;
    static_assert(sizeof(amt) == 8);
    std::memcpy(words.data() + 8, &amt, sizeof(amt));
    std::vector<unsigned char> bytes(sizeof(words));
    std::memcpy(bytes.data(), words.data(), sizeof(words));
    if (!Mod::get()->patch(address, bytes)) {
        return;
    }
    step_patched = true;
}

// silicate's windows patches (GPL-3.0, bottom of this file) found again in the arm64 android lib
// each checks the original instructions first & skips w/ a warning if they dont match
void install_android_silicate_patches() {
    constexpr unsigned nop = 0xd503201f; // arm64 nop
    auto patch = [](uintptr_t offset, std::initializer_list<unsigned> original, std::initializer_list<unsigned> replacement, const char* name) {
        auto* address = reinterpret_cast<void*>(geode::base::get() + offset);
        if (std::memcmp(address, original.begin(), original.size() * 4) != 0) {
            return;
        }
        std::vector<unsigned char> bytes(replacement.size() * 4);
        std::memcpy(bytes.data(), replacement.begin(), bytes.size());
        (void)Mod::get()->patch(address, bytes);
    };
    patch(0x88abe0, { 0x97f6c0d7 }, { nop }, "GJBaseGameLayer::resetLevelVariables P1");
    patch(0x88abe8, { 0x97f6c0d5 }, { nop }, "GJBaseGameLayer::resetLevelVariables P2");
    patch(0x5c6f5c, { 0xb9024a61, 0xb9024e61, 0x97fffefa }, { nop, nop, nop }, "UILayer::handleKeypress");
    // resetLevel skips the GameManager check, removeReleasedButtons & the held check. always queues the jump
    // fmov d0, xzr (d0 is queueButton's timestamp); b 0x5e571c
    patch(0x5e56e4, { 0x94002767, 0x394ccc00 }, { 0x9e6703e0, 0x1400000d }, "PlayLayer::resetLevel");
    patch(0x5d27e0, { 0x9401a1d7 }, { nop }, "PlayLayer::resumeAndRestart");
}
#endif

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
        logic.shake_random = logic.lvl_seed & 32767;
        logic.teleport_random = logic.lvl_seed & 32767;
        Logic::get().apply_attempt_variance(play, logic.lvl_seed);
        Logic::get().reseed_rand_triggers(logic.lvl_seed);
        return;
    }
    unsigned long long state = Logic::get().game_random_state();
    logic.lvl_seed = state;
    logic.lvl_seed_known = true;
    logic.shake_random = state & 32767;
    logic.teleport_random = state & 32767;
    Logic::get().reseed_rand_triggers(state);
}

#ifdef GEODE_IS_WINDOWS
safetyhook::MidHook g_level_seed;

void on_lvl_seed(safetyhook::Context&) {
    apply_lvl_seed();
}
#endif

#ifdef GEODE_IS_MACOS
class $modify(EchoSeedToolbox, GameToolbox) {
    static void fast_srand(unsigned long long seed) {
        GameToolbox::fast_srand(seed);
        if (!g_native_reset || g_seed_applied)
            return;
        g_seed_applied = true;
        apply_lvl_seed();
    }
};
#endif

#if defined(GEODE_IS_ANDROID) || defined(GEODE_IS_MACOS)
class $modify(EchoPortalTarget, GJBaseGameLayer) {
    TeleportPortalObject* getPortalTarget(TeleportPortalObject* portal) {
        auto* picked = GJBaseGameLayer::getPortalTarget(portal);
        if (!portal || portal->m_orangePortal || portal->m_targetGroupID <= 0)
            return picked;
        auto* group = getGroup(portal->m_targetGroupID);
        int amt = group ? (int)group->count() : 0;
        if (amt <= 1)
            return picked;
        auto& roll = Logic::get().teleport_random;
        roll = (unsigned long long)((int)((214013ull * roll + 2531011ull) >> 16) & 32767);
        float at = (float)roll / 32767.f;
        if (at == 1.f)
            at = 0.f; // why
        return static_cast<TeleportPortalObject*>(group->objectAtIndex((int)(at * amt)));
    }
};
#endif

#ifdef GEODE_IS_WINDOWS
// the mid hooks are NOT geode patches so disable bot has to take them out itself
void echo_raw_hooks(bool on) {
    static safetyhook::MidHook shake_a, shake_b, shake_c, shake_d;
    static safetyhook::MidHook teleport;
    if (!on) {
        g_level_seed = {};
        shake_a = {};
        shake_b = {};
        shake_c = {};
        shake_d = {};
        teleport = {};
        return;
    }
    // where resetLevel writes the attempts rand state (silicate's offset, 2.2081)
    // recording keeps what gd picked, playback puts the macros seed back
    g_level_seed = safetyhook::create_mid(geode::base::get() + 0x3b90fb, on_lvl_seed);

    // shake & teleport rolls use my own seeded lcg NOT gds rand, so nothing else rolling changes them
    auto shake_step = [](safetyhook::Context& ctx) {
        auto& state = Logic::get().shake_random;
        state = (unsigned long long)((int)((214013ull * state + 2531011ull) >> 16) & 32767);
        ctx.rax = (uintptr_t)state;
        ctx.rip += 6;
    };
    auto teleport_step = [](safetyhook::Context& ctx) {
        auto& state = Logic::get().teleport_random;
        state = (unsigned long long)((int)((214013ull * state + 2531011ull) >> 16) & 32767);
        ctx.rax = (uintptr_t)state;
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
    auto* at = reinterpret_cast<unsigned char*>(geode::base::get() + 0x237A5B);
    if (!g_step_patch || g_step_bytes.empty() || std::memcmp(at, g_step_bytes.data(), g_step_bytes.size()) != 0)
        return;
    (void)g_step_patch->enable();
    (void)g_step_patch->disable();
}
#endif

$execute {
#ifdef GEODE_IS_ANDROID64
    install_android_physics_step_patch();
    install_android_silicate_patches();
#endif
#if defined(GEODE_IS_WINDOWS)
    install_physics_step_patch();
    echo_raw_hooks(true);

    // skidded straight from silicate (GPL-3.0), these offsets + bytes are their findings for 2.2081
    // same 4 as the android ones above
    auto patch = [](uintptr_t offset, std::vector<unsigned char> bytes, const char* name) {
        (void)Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset), bytes);
    };
    patch(0x23B4EA, std::vector<unsigned char>(24, 0x90), "GJBaseGameLayer::resetLevelVariables");
    patch(0x4CD95C, std::vector<unsigned char>(19, 0x90), "UILayer::handleKeypress");
    patch(0x3B994C, { 0xEB, 0x5E }, "PlayLayer::resetLevel");
    patch(0x3BA508, std::vector<unsigned char>(5, 0x90), "PlayLayer::resumeAndRestart");
#endif
}

class $modify(EchoShaderLayer, ShaderLayer) {
    void performCalculations() {
        if (Logic::get().layout_active()) {
            m_state.m_usesShaders = false;
            return;
        }
        ShaderLayer::performCalculations();
    }
};

class $modify(EchoEffectManager, GJEffectManager) {
    void updateEffects(float dt) {
        if (Logic::get().layout_active()) {
            m_pulseEffectMap.clear();
            m_pulseEffectVector.clear();
            m_opacityEffectMap.clear();
        }
        GJEffectManager::updateEffects(dt);
        if (Logic::get().layout_active())
            Logic::get().paint_layout_channels(this);
    }

    float opacityModForGroup(int id) {
        if (Logic::get().layout_active())
            return 1.f;
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
        if (ccpDistanceSQ(last->m_point, point) < 1e-6f)
            return;
        HardStreak::addPoint(point);
    }
};

class $modify(EchoDrawNode, CCDrawNode) {
    void draw() {
        unsigned capacity = m_uBufferCapacity;
        if (m_bDirty && m_nBufferCount >= 0)
            m_uBufferCapacity = std::min(capacity, (unsigned)m_nBufferCount);
        CCDrawNode::draw();
        m_uBufferCapacity = capacity;
    }
};

class $modify(EchoDirector, CCDirector) {
    void drawScene() {
        GUI::get().sync_cursor();
        if (Logic::bot_disabled()) {
#ifdef GEODE_IS_WINDOWS
            echo_guard_step_site();
#endif
            CCDirector::drawScene();
            return;
        }
        auto& logic = Logic::get();
        logic.sync_render_pause_button();
#ifdef GEODE_IS_IOS
        struct DrawHidden {
            std::array<std::pair<CCNode*, bool>, 2> held{};

            DrawHidden() {
                auto* layer = GJBaseGameLayer::get();
                if (!layer)
                    return;
                PlayerObject* players[2] = { layer->m_player1, layer->m_player2 };
                bool hide[2] = { Logic::get().hide_p1, Logic::get().hide_p2 };
                for (int i = 0; i < 2; ++i) {
                    if (!hide[i] || !players[i])
                        continue;
                    held[i] = { players[i], players[i]->isVisible() };
                    players[i]->CCNode::setVisible(false);
                }
            }

            ~DrawHidden() {
                for (auto& [node, was] : held) {
                    if (node)
                        node->CCNode::setVisible(was);
                }
            }
        } hidden;
#endif
        if (auto* play = PlayLayer::get(); play && (logic.show_hitboxes || logic.show_traj)) {
            logic.sync_overlay_seats(play);
        }
        static bool menu_under_me = false;
        if (!logic.render_capturing()) {
            auto started = chrono::steady_clock::now();
            logic.frame_start = started;
            logic.scheduler_time = 0.0;
            int menu_frames = GUI::get().menu_frames();
            CCDirector::drawScene();
            logic.draw_time = std::max(0.0, chrono::duration<double>(chrono::steady_clock::now() - started).count() - logic.scheduler_time);
            if (GUI::get().menu_frames() != menu_frames)
                menu_under_me = true;
            logic.note_frame_work(chrono::duration<double>(chrono::steady_clock::now() - started).count());
            return;
        }
        auto started = chrono::steady_clock::now();
        auto advance = [&] {
            bool capture = logic.begin_render_frame();
            if (capture)
                logic.render_frame_game_step = logic.render_game_step();
            if (!m_bPaused && m_pScheduler)
                m_pScheduler->update(capture ? logic.render_frame_game_step : logic.render_paused() ? 0.f
                                                                                                    : logic.render_wall_delta());
            if (auto* play = PlayLayer::get(); play && capture && (logic.show_hitboxes || logic.show_traj)) {
                logic.sync_overlay_seats(play);
            }
            bool switched = m_pNextScene != nullptr;
            if (switched)
                setNextScene();
            return capture && logic.capture_frame() && !switched;
        };
        while (advance() && logic.render_batch_more(started)) {
        }
        static auto last_shown = chrono::steady_clock::now();
        auto shown = chrono::steady_clock::now();
        m_fDeltaTime = std::clamp(chrono::duration<float>(shown - last_shown).count(), 0.f, 0.25f);
        last_shown = shown;
        logic.show_render_overlay();
        if (menu_under_me)
            GUI::get().draw_menu_now();
        m_pobOpenGLView->swapBuffers();
    }
};

#ifdef GEODE_IS_WINDOWS
class $modify(EchoEGLView, CCEGLView) {
    void setFrameSize(float width, float height) {
        if (Logic::get().render_holds_view())
            return;
        CCEGLView::setFrameSize(width, height);
    }

    void onGLFWframebuffersize(GLFWwindow* window, int width, int height) {
        if (Logic::get().render_holds_view())
            return;
        CCEGLView::onGLFWframebuffersize(window, width, height);
    }

    void onGLFWWindowSizeFunCallback(GLFWwindow* window, int width, int height) {
        if (Logic::get().render_holds_view())
            return;
        CCEGLView::onGLFWWindowSizeFunCallback(window, width, height);
    }

    void toggleLockCursor(bool locked) {
        if (locked && menu_holds_mouse())
            locked = false;
        CCEGLView::toggleLockCursor(locked);
    }
};
#endif

// gds camera follow moves the same amt every call no matter the step length, so at a high tps it snapped.
// physics keeps gds camera every step (lvls read it, desideration died w/o that). the picture gets its own camera,
// run ~240 times a second on the real steps, & physics gets its values, tweens & layers back right after.
// frames are drawn w/ the pictures
struct CameraRegions {
    std::vector<std::pair<unsigned, unsigned>> regions; // offset in the layer, 4 byte words
    std::vector<unsigned> mask;
    std::vector<unsigned> before, shown, physics;
    std::size_t words = 0;

    void build(GJBaseGameLayer* layer) {
        auto base = reinterpret_cast<uintptr_t>(layer);
        auto& gs = layer->m_gameState;
        auto add = [&](const void* from, const void* to) {
            auto start = reinterpret_cast<uintptr_t>(from) - base, end = reinterpret_cast<uintptr_t>(to) - base;
            regions.push_back({ (unsigned)(start / 4), (unsigned)((end - start) / 4) });
        };
        add(&gs, &gs.m_spawnChannelRelated0);
        add(&gs.m_cameraPosition, &gs.m_tweenActions);
        add(&gs.m_cameraEdgeValue0, &gs.m_gameObjectPhysics);
        add(&gs.m_unkUint10, &gs.m_activatedObjectIDs);
        add(&gs.m_unkUint13, &gs.m_stateObjects);
        add(&layer->m_cameraFlip, &layer->m_updateGroundShadows);
        add(&layer->m_cameraUnzoomedHeightOffset, &layer->m_targetCameraHeightOffset + 1);
        words = 0;
        for (const auto& region : regions)
            words += region.second;
        mask.assign(words, 0);
        before.assign(words, 0);
        shown.assign(words, 0);
        physics.assign(words, 0);
    }

    template <class F>
    void each(GJBaseGameLayer* layer, F&& visit) {
        auto* raw = reinterpret_cast<unsigned*>(layer);
        std::size_t at = 0;
        for (const auto& [start, count] : regions)
            for (unsigned word = 0; word < count; ++word, ++at)
                visit(raw[start + word], at);
    }

    std::size_t index_of(const void* field, GJBaseGameLayer* layer) const {
        auto off = (unsigned)((reinterpret_cast<uintptr_t>(field) - reinterpret_cast<uintptr_t>(layer)) / 4);
        std::size_t at = 0;
        for (const auto& [start, count] : regions) {
            if (off >= start && off < start + count)
                return at + off - start;
            at += count;
        }
        return 0;
    }

    void save(GJBaseGameLayer* layer, std::vector<unsigned>& into) {
        each(layer, [&](unsigned& word, std::size_t at) { into[at] = word; });
    }

    void load(GJBaseGameLayer* layer, const std::vector<unsigned>& from) {
        each(layer, [&](unsigned& word, std::size_t at) { word = from[at]; });
    }

    void load_shown(GJBaseGameLayer* layer) {
        each(layer, [&](unsigned& word, std::size_t at) {
            if (mask[at])
                word = shown[at];
        });
    }

    void learn(GJBaseGameLayer* layer) {
        each(layer, [&](unsigned& word, std::size_t at) { mask[at] |= word != before[at]; });
    }
};

struct NodeSpot {
    Ref<CCNode> node;
    CCPoint position;
    float scale_x, scale_y, rotation_x, rotation_y, skew_x, skew_y;
};

static void keep_nodes(GJBaseGameLayer* layer, std::vector<NodeSpot>& spots) {
    spots.clear();
    auto keep = [&](CCNode* node) {
        if (node)
            spots.push_back({ node, node->getPosition(), node->getScaleX(), node->getScaleY(), node->getRotationX(), node->getRotationY(),
                node->getSkewX(), node->getSkewY() });
    };
    auto with_children = [&](CCNode* node) {
        keep(node);
        if (!node || !node->getChildren())
            return;
        for (auto* child : CCArrayExt<CCNode*>(node->getChildren())) {
            keep(child);
            if (!child || !child->getChildren())
                continue;
            for (auto* grandchild : CCArrayExt<CCNode*>(child->getChildren()))
                keep(grandchild);
        }
    };
    keep(layer->m_objectLayer);
    keep(layer->m_inShaderObjectLayer);
    keep(layer->m_aboveShaderObjectLayer);
    keep(layer->m_background);
    with_children(layer->m_groundLayer);
    with_children(layer->m_groundLayer2);
    with_children(layer->m_middleground);
}

static void put_back(const std::vector<NodeSpot>& spots) {
    for (const auto& spot : spots) {
        spot.node->setPosition(spot.position);
        spot.node->setScaleX(spot.scale_x);
        spot.node->setScaleY(spot.scale_y);
        spot.node->setRotationX(spot.rotation_x);
        spot.node->setRotationY(spot.rotation_y);
        spot.node->setSkewX(spot.skew_x);
        spot.node->setSkewY(spot.skew_y);
    }
}

struct CameraPace {
    GJBaseGameLayer* layer = nullptr;
    CameraRegions state;
    unsigned long long update = 0;
    int at = 0, since = 0;
    bool have_shown = false, shadow = false;
    CCPoint last1, last2;
    std::vector<NodeSpot> shown_nodes, physics_nodes;
    bool snap = false, have_cam = false;
    CCPoint cam_after;
    // gd moves the camera w/ tweens (cameraMoveX/Y are keys 1 & 2) from where it is rn. the picture keeps its own
    // copy of every key its calls touched, sharing gds made it jump onto gds camera
    std::unordered_map<int, GJValueTween> tweens;
    std::unordered_set<int> owned{ 1, 2 };
    int mirror_depth = 0;
    std::size_t camera_word = 0;
};
static CameraPace g_camera_pace;

using TweenMap = std::unordered_map<int, GJValueTween>;

static bool same_tween(const GJValueTween& a, const GJValueTween& b) {
    return a.m_fromValue == b.m_fromValue && a.m_toValue == b.m_toValue && a.m_duration == b.m_duration && a.m_deltaTime == b.m_deltaTime &&
           a.m_currentValue == b.m_currentValue && a.m_easingType == b.m_easingType && a.m_easingRate == b.m_easingRate &&
           a.m_finished == b.m_finished && a.m_disabled == b.m_disabled && a.m_uniqueID == b.m_uniqueID && a.m_controlID == b.m_controlID;
}

static TweenMap copy_tweens(GJBaseGameLayer* layer) {
    TweenMap out;
    for (const auto& [key, tween] : layer->m_gameState.m_tweenActions)
        out.emplace(key, tween);
    return out;
}

static void set_tween(GJBaseGameLayer* layer, int key, const GJValueTween* tween) {
    auto& map = layer->m_gameState.m_tweenActions;
    auto it = map.find(key);
    if (!tween) {
        if (it != map.end())
            map.erase(it);
    }
    else if (it != map.end())
        it->second = *tween;
    else
        map.emplace(key, *tween);
}

static void restore_tweens(GJBaseGameLayer* layer, const TweenMap& from) {
    std::vector<int> extra;
    for (const auto& [key, tween] : layer->m_gameState.m_tweenActions)
        if (!from.contains(key))
            extra.push_back(key);
    for (int key : extra)
        set_tween(layer, key, nullptr);
    for (const auto& [key, tween] : from)
        set_tween(layer, key, &tween);
}

// physics' map w/ the pictures own keys swapped in
static void install_tweens(GJBaseGameLayer* layer) {
    auto& pace = g_camera_pace;
    for (int key : pace.owned) {
        auto it = pace.tweens.find(key);
        set_tween(layer, key, it == pace.tweens.end() ? nullptr : &it->second);
    }
}

// anything a picture call started, changed or stopped is the pictures from now on
static void keep_tweens(GJBaseGameLayer* layer, const TweenMap& before) {
    auto& pace = g_camera_pace;
    const auto& map = layer->m_gameState.m_tweenActions;
    for (const auto& [key, tween] : map) {
        auto it = before.find(key);
        if (it == before.end() || !same_tween(it->second, tween))
            pace.owned.insert(key);
    }
    for (const auto& [key, tween] : before)
        if (map.find(key) == map.end())
            pace.owned.insert(key);
    for (int key : pace.owned) {
        if (auto it = map.find(key); it != map.end())
            pace.tweens[key] = it->second;
        else
            pace.tweens.erase(key);
    }
}

static int camera_every() {
    return std::max((int)std::lround(Logic::get().sample_rate() / VANILLA_TPS), 1);
}

static void run_shown_camera(GJBaseGameLayer* layer) {
    auto& pace = g_camera_pace;
    auto& state = pace.state;
    auto* p1 = layer->m_player1;
    auto* p2 = layer->m_player2;
    state.save(layer, state.physics);
    keep_nodes(layer, pace.physics_nodes);
    TweenMap physics_tweens = copy_tweens(layer);
    if (!pace.have_shown || pace.snap) {
        state.shown = state.physics;
        pace.shown_nodes = pace.physics_nodes;
        pace.tweens.clear();
        for (int key : pace.owned)
            if (auto it = physics_tweens.find(key); it != physics_tweens.end())
                pace.tweens.emplace(key, it->second);
        pace.have_shown = true;
        pace.snap = false;
        pace.since = 0;
        pace.last1 = p1->getPosition();
        pace.last2 = p2 ? p2->getPosition() : CCPoint{};
        return;
    }
    state.load_shown(layer);
    install_tweens(layer);
    float elapsed = (float)pace.since * Logic::get().physics_step();
    std::vector<int> finished;
    for (auto& [key, tween] : layer->m_gameState.m_tweenActions) {
        bool own = pace.owned.contains(key);
        if (own && !tween.m_finished && !tween.m_disabled)
            tween.step(elapsed);
        layer->m_gameState.updateTweenAction(tween.m_currentValue, key);
        if (own && tween.m_finished)
            finished.push_back(key);
    }
    for (int key : finished)
        set_tween(layer, key, nullptr);
    TweenMap installed = copy_tweens(layer);
    state.save(layer, state.before);
    auto real1 = p1->m_lastPosition;
    CCPoint real2 = p2 ? p2->m_lastPosition : CCPoint{};
    p1->m_lastPosition = pace.last1;
    if (p2)
        p2->m_lastPosition = pace.last2;
    float owed = (float)(60.0 / Logic::get().sample_rate() * std::max(pace.since, 1));
    pace.shadow = true;
    layer->updateCamera(owed);
    pace.shadow = false;
    state.learn(layer);
    keep_tweens(layer, installed);
    for (int key : finished)
        pace.tweens.erase(key);
    p1->m_lastPosition = real1;
    if (p2)
        p2->m_lastPosition = real2;
    pace.last1 = p1->getPosition();
    pace.last2 = p2 ? p2->getPosition() : CCPoint{};
    pace.since = 0;
    state.save(layer, state.shown);
    keep_nodes(layer, pace.shown_nodes);
    state.load(layer, state.physics);
    restore_tweens(layer, physics_tweens);
    put_back(pace.physics_nodes);
}

// a camera function physics called (portal, static camera, offset, zoom) runs again for the picture, so what it
// starts from the camera starts from the pictures. no events or dual ground on that 2nd call
template <class F>
static void mirror_camera(GJBaseGameLayer* layer, F&& call) {
    auto& pace = g_camera_pace;
    if (pace.shadow || pace.mirror_depth > 0 || pace.layer != layer || !pace.have_shown || camera_every() <= 1) {
        call();
        return;
    }
    auto& state = pace.state;
    ++pace.mirror_depth;
    call();
    state.save(layer, state.physics);
    TweenMap physics_tweens = copy_tweens(layer);
    std::vector<NodeSpot> nodes;
    keep_nodes(layer, nodes);
    state.load_shown(layer);
    install_tweens(layer);
    TweenMap installed = copy_tweens(layer);
    state.save(layer, state.before);
    pace.shadow = true;
    call();
    pace.shadow = false;
    keep_tweens(layer, installed);
    std::size_t at = 0;
    for (const auto& [start, count] : state.regions)
        for (unsigned w = 0; w < count; ++w, ++at)
            if (reinterpret_cast<unsigned*>(layer)[start + w] != state.physics[at])
                state.mask[at] = 1;
    state.save(layer, state.shown);
    state.load(layer, state.physics);
    restore_tweens(layer, physics_tweens);
    put_back(nodes);
    --pace.mirror_depth;
}

class $modify(EchoCameraPace, GJBaseGameLayer) {
    void updateCamera(float dt) {
        auto& pace = g_camera_pace;
        auto& logic = Logic::get();
        int every = camera_every();
        if (pace.shadow || dt == 0.f || !m_player1 || every <= 1 || !Logic::as_play(this)) {
            GJBaseGameLayer::updateCamera(dt);
            return;
        }
        if (pace.layer != this) {
            // a lvl that left w/o onQuit: its nodes are gone, releasing them crashed. let them leak
            new std::vector<NodeSpot>(std::move(pace.shown_nodes));
            new std::vector<NodeSpot>(std::move(pace.physics_nodes));
            pace = {};
            pace.layer = this;
            pace.state.build(this);
            pace.camera_word = pace.state.index_of(&m_gameState.m_cameraPosition, this);
        }
        if (logic.resetting || logic.refresh_only)
            pace.have_shown = false;
        if (pace.update != g_updates_run) {
            pace.update = g_updates_run;
            pace.at = 0;
        }
        ++pace.at;
        ++pace.since;
        // spread over the batch w/ the last 1 on its last step, or the shown frame lags behind
        int batch = (int)logic.steps_this_tick;
        bool runs_now;
        if (batch > 1) {
            int runs = std::clamp((int)std::lround((double)batch / every), 1, batch);
            runs_now = (pace.at * runs) / batch != ((pace.at - 1) * runs) / batch;
        }
        else
            runs_now = pace.since >= every;
        pace.state.save(this, pace.state.before);
        // following moves it a few units a call at most. way more = gd put it there on purpose
        constexpr float SNAP = 60.f;
        auto cam_before = m_gameState.m_cameraPosition;
        if (pace.have_cam && ccpDistance(cam_before, pace.cam_after) > SNAP)
            pace.snap = true;
        GJBaseGameLayer::updateCamera(dt);
        if (ccpDistance(m_gameState.m_cameraPosition, cam_before) > SNAP)
            pace.snap = true;
        pace.cam_after = m_gameState.m_cameraPosition;
        pace.have_cam = true;
        pace.state.learn(this);
        if (runs_now)
            run_shown_camera(this);
    }
};

void echo_forget_camera(GJBaseGameLayer* layer) {
    if (g_camera_pace.layer == layer)
        g_camera_pace = {};
}

using NodeShot = std::array<float, 8>;

static std::vector<NodeShot> shoot_nodes(const std::vector<NodeSpot>& spots) {
    std::vector<NodeShot> out;
    out.reserve(spots.size());
    for (const auto& spot : spots)
        out.push_back({ spot.position.x, spot.position.y, spot.scale_x, spot.scale_y, spot.rotation_x, spot.rotation_y, spot.skew_x, spot.skew_y });
    return out;
}

static void unshoot_nodes(const std::vector<NodeShot>& shots, std::vector<NodeSpot>& spots) {
    for (std::size_t i = 0; i < shots.size() && i < spots.size(); ++i) {
        const auto& from = shots[i];
        auto& spot = spots[i];
        spot.position = CCPoint{ from[0], from[1] };
        spot.scale_x = from[2];
        spot.scale_y = from[3];
        spot.rotation_x = from[4];
        spot.rotation_y = from[5];
        spot.skew_x = from[6];
        spot.skew_y = from[7];
    }
}

struct ShownCamera {
    std::vector<NodeShot> physics;
    bool picture = false;
    std::vector<unsigned> shown;
    std::vector<NodeShot> nodes;
    std::unordered_map<int, GJValueTween> tweens;
    std::unordered_set<int> owned;
    int since = 0;
    CCPoint last1, last2;
};

std::shared_ptr<ShownCamera> echo_save_shown_camera(GJBaseGameLayer* layer) {
    if (!layer)
        return nullptr;
    auto out = std::make_shared<ShownCamera>();
    std::vector<NodeSpot> spots;
    keep_nodes(layer, spots);
    out->physics = shoot_nodes(spots);
    const auto& pace = g_camera_pace;
    if (pace.layer != layer || !pace.have_shown || pace.snap)
        return out;
    out->picture = true;
    out->shown = pace.state.shown;
    out->nodes = shoot_nodes(pace.shown_nodes);
    out->tweens = pace.tweens;
    out->owned = pace.owned;
    out->since = pace.since;
    out->last1 = pace.last1;
    out->last2 = pace.last2;
    return out;
}

void echo_load_shown_camera(GJBaseGameLayer* layer, const ShownCamera* saved) {
    if (!layer)
        return;
    if (saved) {
        std::vector<NodeSpot> spots;
        keep_nodes(layer, spots);
        if (saved->physics.size() == spots.size()) {
            unshoot_nodes(saved->physics, spots);
            put_back(spots);
        }
    }
    auto& pace = g_camera_pace;
    if (pace.layer != layer)
        return;
    pace.snap = false;
    pace.have_cam = true;
    pace.cam_after = layer->m_gameState.m_cameraPosition;
    if (!saved || !saved->picture || saved->shown.size() != pace.state.shown.size() || saved->nodes.size() != pace.shown_nodes.size()) {
        pace.have_shown = false;
        return;
    }
    pace.state.shown = saved->shown;
    unshoot_nodes(saved->nodes, pace.shown_nodes);
    pace.tweens = saved->tweens;
    pace.owned = saved->owned;
    pace.since = saved->since;
    pace.last1 = saved->last1;
    pace.last2 = saved->last2;
    pace.have_shown = true;
}

class $modify(EchoCameraShow, GJBaseGameLayer) {
    void visit() {
        auto& pace = g_camera_pace;
        auto& logic = Logic::get();
        if (pace.layer != this || !pace.have_shown || !m_player1 || camera_every() <= 1 || logic.resetting || logic.refresh_only) {
            GJBaseGameLayer::visit();
            return;
        }
        if (pace.since > 0)
            run_shown_camera(this);
        std::vector<NodeSpot> now;
        keep_nodes(this, now);
        auto& state = pace.state;
        state.save(this, state.physics);
        state.load_shown(this);
        put_back(pace.shown_nodes);
        // enter effects come from the camera in updateVisibility, so the picture gets a pass w/ its camera & physics
        // gets its own back after drawing. dt 0 so nothing animates twice
        auto* play = Logic::as_play(this);
        bool visibility = play && ccpDistance(m_gameState.m_cameraPosition,
                                      CCPoint{ std::bit_cast<float>(state.physics[pace.camera_word]), std::bit_cast<float>(state.physics[pace.camera_word + 1]) }) > 0.5f;
        if (visibility) {
            pace.shadow = true;
            play->updateVisibility(0.f);
            pace.shadow = false;
        }
        GJBaseGameLayer::visit();
        state.load(this, state.physics);
        put_back(now);
        if (visibility)
            play->updateVisibility(0.f);
    }
};

class $modify(EchoCameraMirror, GJBaseGameLayer) {
#ifndef GEODE_IS_WINDOWS
    void updateCameraMode(EffectGameObject* obj, bool updateDual) {
        mirror_camera(this, [&] { GJBaseGameLayer::updateCameraMode(obj, updateDual); });
    }
#endif

    void updateStaticCameraPos(CCPoint pos, bool staticX, bool staticY, bool followOrSmoothEase, float time, int easingType, float easingRate) {
        mirror_camera(this, [&] { GJBaseGameLayer::updateStaticCameraPos(pos, staticX, staticY, followOrSmoothEase, time, easingType, easingRate); });
    }

    void updateStaticCameraPosToGroup(int centerID, bool updateX, bool updateY, bool followObject, float followEase, float duration, int easingType,
        float easingRate, bool smoothVelocity, float velocityMod) {
        mirror_camera(this, [&] {
            GJBaseGameLayer::updateStaticCameraPosToGroup(centerID, updateX, updateY, followObject, followEase, duration, easingType, easingRate, smoothVelocity, velocityMod);
        });
    }

    void exitStaticCamera(bool exitX, bool exitY, float time, int easingType, float easingRate, bool smoothVelocity, float smoothVelocityMod, bool exitInstant) {
        mirror_camera(this, [&] { GJBaseGameLayer::exitStaticCamera(exitX, exitY, time, easingType, easingRate, smoothVelocity, smoothVelocityMod, exitInstant); });
    }

    void resetStaticCamera(bool resetX, bool resetY) {
        mirror_camera(this, [&] { GJBaseGameLayer::resetStaticCamera(resetX, resetY); });
    }

    void updateCameraOffsetX(float offsetX, float duration, int easingType, float easingRate, int uniqueID, int controlID) {
        mirror_camera(this, [&] { GJBaseGameLayer::updateCameraOffsetX(offsetX, duration, easingType, easingRate, uniqueID, controlID); });
    }

    void updateCameraOffsetY(float offsetY, float duration, int easingType, float easingRate, int uniqueID, int controlID) {
        mirror_camera(this, [&] { GJBaseGameLayer::updateCameraOffsetY(offsetY, duration, easingType, easingRate, uniqueID, controlID); });
    }

    void updateZoom(float zoom, float duration, int easing, float rate, int uniqueID, int controlID) {
        mirror_camera(this, [&] { GJBaseGameLayer::updateZoom(zoom, duration, easing, rate, uniqueID, controlID); });
    }

    // the pictures 2nd call cant fire anything
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (g_camera_pace.shadow)
            return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void updateDualGround(PlayerObject* object, int mode, bool instant, float duration) {
        if (g_camera_pace.shadow)
            return;
        GJBaseGameLayer::updateDualGround(object, mode, instant, duration);
    }
};

class $modify(EchoAudioEngine, FMODAudioEngine) {
    void update(float dt) {
        if (Logic::get().render_blocks_engine())
            return;
        FMODAudioEngine::update(dt);
    }
};

FMOD_RESULT echo_mixer_update(FMOD::System* self) {
    if (Logic::get().render_blocks_mixer())
        return FMOD_OK;
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
