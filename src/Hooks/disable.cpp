#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "Hooks/step.hpp"
#include "Logic/logic.hpp"
#include <fstream>
#include <vector>

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
void echo_raw_hooks(bool on);
#endif

namespace {
    bool& off() {
        static bool value = Mod::get()->getSavedValue<bool>("bot-disabled", false);
        return value;
    }

    std::vector<Patch*> g_patches;
    std::vector<Hook*> g_hooks;

    void apply(bool disable) {
        if (disable) {
            for (auto* patch : Mod::get()->getPatches()) {
                if (!patch->isEnabled())
                    continue;
                (void)patch->disable();
                g_patches.push_back(patch);
            }
            for (auto* hook : Mod::get()->getHooks()) {
                auto name = std::string(hook->getDisplayName());
                bool menu = name.starts_with("cocos2d::") && name != "cocos2d::CCScheduler::update";
                if (menu || name == "PlayerObject::update" || !hook->isEnabled())
                    continue;
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
        echo_raw_hooks(!disable);
#endif
    }
}

bool Session::bot_disabled() {
    return off();
}

void Session::set_bot_disabled(bool on) {
    if (on == off())
        return;
    auto& logic = Logic::get();
    // half a level played by echo & half not is a mess. same rule silicate has
    if (PlayLayer::get()) {
        logic.message = "Leave the level first";
        Notification::create("Leave the level first", NotificationIcon::Error)->show();
        return;
    }
    if (on)
        logic.state = RunState::Idle;
    off() = on;
    apply(on);
    Mod::get()->setSavedValue("bot-disabled", on);
    (void)Mod::get()->saveData();
    logic.message = on ? "Echo is off" : "Echo is back on";
}

void Session::repair_player_order(PlayLayer* play) {
    static uint64_t checked = ~0ull;
    static Frame const* owner = nullptr;
    if (!play || (play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode))
        return;
    if (checked == inputs_revision && owner == inputs.data())
        return;
    int jump = static_cast<int>(PlayerButton::Jump);
    auto button = [&](Frame const& input) {
        return input.button == 0 ? jump : static_cast<int>(input.button);
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
        if (next >= inputs.size() || inputs[next].holding)
            continue;
        std::swap(a, b);
        if (poses.size() == inputs.size())
            std::swap(poses[i], poses[i + 1]);
        if (input_ids.size() == inputs.size())
            std::swap(input_ids[i], input_ids[i + 1]);
        changed = true;
        ++i;
    }
    if (changed)
        touch_inputs();
    checked = inputs_revision;
    owner = inputs.data();
}

void Session::set_macro_ldm(bool on) {
    auto& logic = Logic::get();
    logic.macro_ldm = on;
    logic.macro_ldm_known = true;
    auto* play = PlayLayer::get();
    if (play && play->m_level && play->m_level->m_lowDetailModeToggled != on)
        logic.message = "Leave the level and enter it again to apply Low Detail Mode";
    Logic::get().push_runtime_settings();
}

$on_mod(Loaded) {
    if (off())
        apply(true);
}

// TODO remove. TEMPORARY, like step_trace.cpp. same file layout so the 2 can be compared
namespace {
    struct PlainStep {
        std::int32_t frame;
        float x, y;
        float rotation;
        double yVelocity;
        double levelTime;
        std::uint8_t held;
        std::uint8_t dead;
        std::uint16_t batch;
        std::uint32_t pad;
    };

    static_assert(sizeof(PlainStep) == 40);
    std::vector<PlainStep> g_plain;
    bool g_plain_written = false;
}

namespace echo::step {
    void trace_plain(PlayerObject* player) {
        auto* play = PlayLayer::get();
        if (!Logic::get().trace_logging || !play || player != play->m_player1)
            return;
        int frame = static_cast<int>(play->m_gameState.m_currentProgress);
        if (frame <= 1 && !play->m_playerDied) {
            g_plain.clear();
            g_plain_written = false;
        }
        if (g_plain_written || g_plain.size() >= 400000)
            return;
        g_plain.push_back({ frame, player->getPositionX(), player->getPositionY(), player->getRotation(), player->m_yVelocity,
            play->m_gameState.m_levelTime, Logic::button_held(player, static_cast<int>(PlayerButton::Jump)), play->m_playerDied || player->m_isDead, 0, 0 });
        bool dead = play->m_playerDied || player->m_isDead;
        if (!dead && g_plain.size() % 4000 != 0)
            return;
        if (dead)
            g_plain_written = true;
        std::ofstream out(Logic::get().save_root() / "logs" / "steps.bin", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<char const*>(g_plain.data()), static_cast<std::streamsize>(g_plain.size() * sizeof(PlainStep)));
    }
}
