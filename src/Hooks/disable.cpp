#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "Hooks/step.hpp"
#include "Logic/logic.hpp"
#include <fstream>
#include <vector>

using namespace geode::prelude;

static bool& disabled_flag() {
    static bool value = Mod::get()->getSavedValue<bool>("bot-disabled", false);
    return value;
}

static std::vector<Patch*> g_patches;
static std::vector<Hook*> g_hooks;
Hook* step_count_hook = nullptr;

static void apply_disabled(bool disable) {
    if (disable) {
        for (auto* patch : Mod::get()->getPatches()) {
            if (!patch->isEnabled()) continue;
            (void)patch->disable();
            g_patches.push_back(patch);
        }
        for (auto* hook : Mod::get()->getHooks()) {
            auto name = std::string(hook->getDisplayName());
            bool menu = name.starts_with("cocos2d::") && name != "cocos2d::CCScheduler::update";
            if (menu || name == "PlayerObject::update" || hook == step_count_hook || !hook->isEnabled()) continue;
            (void)hook->disable();
            g_hooks.push_back(hook);
        }
    }
    else {
        for (auto* patch : g_patches)
            (void)patch->enable();
        for (auto* hook : g_hooks)
            (void)hook->enable();
        g_patches.clear();
        g_hooks.clear();
    }
#ifdef GEODE_IS_WINDOWS
    enable_raw_hooks(!disable);
#endif
}

bool Logic::bot_disabled() {
    return disabled_flag();
}

void Logic::set_bot_disabled(bool on) {
    if (on == disabled_flag()) return;
    auto& logic = Logic::get();
    // half a level played by echo & half not is a mess. same rule silicate has
    if (PlayLayer::get() || logic.editor_testing) {
        logic.message = "Leave the level first";
        Notification::create("Leave the level first", NotificationIcon::Error)->show();
        return;
    }
    if (on) logic.state = RunState::Idle;
    disabled_flag() = on;
    apply_disabled(on);
    Mod::get()->setSavedValue("bot-disabled", on);
    (void)Mod::get()->saveData();
    logic.message = on ? "Echo is off" : "Echo is back on";
}

void Logic::repair_player_order(PlayLayer* play) {
    static unsigned long long checked = ~0ull;
    static const Frame* owner = nullptr;
    if (!play || (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode)) return;
    if (checked == inputs_version && owner == inputs.data()) return;
    auto button = [&](const Frame& input) {
        return input.button == 0 ? JUMP : (int)input.button;
    };
    bool changed = false;
    for (size_t i = 0; i + 1 < inputs.size(); ++i) {
        auto& a = inputs[i];
        auto& b = inputs[i + 1];
        if (a.number != b.number || a.player2 == b.player2 || button(a) != button(b) || !a.holding || b.holding)
            continue;
        size_t next = i + 2;
        while (next < inputs.size() && button(inputs[next]) != button(a))
            ++next;
        if (next >= inputs.size() || inputs[next].holding) continue;
        std::swap(a, b);
        if (poses.size() == inputs.size()) std::swap(poses[i], poses[i + 1]);
        if (input_ids.size() == inputs.size()) std::swap(input_ids[i], input_ids[i + 1]);
        changed = true;
        ++i;
    }
    if (changed) mark_inputs_changed();
    checked = inputs_version;
    owner = inputs.data();
}

void Logic::set_macro_ldm(bool on) {
    macro_ldm = on;
    macro_ldm_known = true;
    auto* play = PlayLayer::get();
    if (play && play->m_level && play->m_level->m_lowDetailModeToggled != on)
        message = "Leave the level and enter it again to apply Low Detail Mode";
    push_runtime_settings();
}

$on_mod(Loaded) {
    if (disabled_flag()) apply_disabled(true);
}

// TODO remove. TEMPORARY, like step_trace.cpp. same file layout so the 2 can be compared
struct PlainStep {
    int frame;
    float x, y;
    float rotation;
    double y_velocity;
    double level_time;
    unsigned char held;
    unsigned char dead;
    unsigned short batch;
    unsigned pad;
};

static_assert(sizeof(PlainStep) == 40);
static std::vector<PlainStep> g_plain;
static bool g_plain_written = false;

void trace_plain(PlayerObject* player) {
    auto* play = PlayLayer::get();
    if (!Logic::get().trace_logging || !play || player != play->m_player1) return;
    int frame = (int)play->m_gameState.m_currentProgress;
    if (frame <= 1 && !play->m_playerDied) {
        g_plain.clear();
        g_plain_written = false;
    }
    if (g_plain_written || g_plain.size() >= 400000) return;
    g_plain.push_back({ frame, player->getPositionX(), player->getPositionY(), player->getRotation(), player->m_yVelocity,
        play->m_gameState.m_levelTime, Logic::button_held(player, JUMP), play->m_playerDied || player->m_isDead, 0, 0 });
    bool dead = play->m_playerDied || player->m_isDead;
    if (!dead && g_plain.size() % 4000 != 0) return;
    if (dead) g_plain_written = true;
    std::ofstream out(Logic::get().save_root() / "logs" / "steps.bin", std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(g_plain.data()), (std::streamsize)(g_plain.size() * sizeof(PlainStep)));
}
