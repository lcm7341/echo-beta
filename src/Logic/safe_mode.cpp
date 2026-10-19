#include "Logic/logic.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

static bool g_cheated = false;
static bool g_applied = false;
static bool g_saved_test_mode = false;
static std::vector<std::string_view> g_used_cheats;

std::vector<std::string_view> Logic::active_cheats() const {
    std::vector<std::string_view> out;
    auto add = [&](bool on, std::string_view name) {
        if (on) out.push_back(name);
    };
    add(noclip_p1 || noclip_p2, "Noclip");
    add(speedhack != 1.f, "Speedhack");
    add(sample_rate() != (double)VANILLA_TPS, "TPS");
    add(frame_advance, "Frame Advance");
    add(pause_before_kill, "Prevent Death");
    add(show_death_next, "Death Next Frame");
    add(backstep, "Backstep");
    add(unrounded_physics(), "Velocity Unrounding");
    add(pause_at_traj_length, "Pause At Trajectory Length");
    add(swap_players, "Swap Player Inputs");
    add(inverse_p1 || inverse_p2, "Inverse Inputs");
    add(maintain_gravity, "Maintain Gravity");
    add(same_gravity, "Same Gravity");
    add(no_mirror_portals, "No Mirror Portals");
    add(seed_override, "Seed Override");
    add(is_playing(), "Macro Playback");
    add(autoclicker, "Autoclicker");
    add(traj_autoclicker, "Trajectory Autoclicker");
    add(click_both, "Click Both");
    add(follow_mode != 0, "Follow Player");
    add(click_dash_orbs, "Click Dash Orbs");
    add(click_black_orbs, "Click Black Orbs");
    add(pad_black_orb, "Auto Black Orb on Pad");
    add(pad_blue_orb, "Auto Blue Orb on Pad");
    add(easy_black_ufo, "Easy Black Orb UFO");
    add(auto_swift, "Auto Swift");
    add(extra_clicks, "Extra Clicks");
    add(straight_fly(), "Auto Straight Fly");
    add(straight_ufo(), "Auto Straight UFO");
    add(show_traj, "Trajectory");
    add(show_hitboxes, "Hitboxes");
    add(layout_mode, "Layout");
    add(hide_p1 || hide_p2, "Hide Player");
    add(script_changed_game, "Full Game Access");
    return out;
}

const std::vector<std::string_view>& Logic::attempt_cheats() const {
    return g_used_cheats;
}

bool Logic::attempt_cheated() const {
    return g_cheated;
}

void Logic::update_safe_mode(PlayLayer* play) {
    if (!play) return;
    if (!safe_mode) return;
    for (auto name : active_cheats()) {
        g_cheated = true;
        if (std::ranges::find(g_used_cheats, name) == g_used_cheats.end()) g_used_cheats.push_back(name);
    }
}

void Logic::hold_safe_mode_test_mode(PlayLayer* play) {
    if (!play || !g_cheated || g_applied) return;
    g_saved_test_mode = play->m_isTestMode;
    play->m_isTestMode = true;
    g_applied = true;
}

void Logic::end_safe_mode_attempt(PlayLayer* play) {
    g_cheated = false;
    g_used_cheats.clear();
    if (g_applied && play) play->m_isTestMode = g_saved_test_mode;
    g_applied = false;
}

void Logic::reset_safe_mode() {
    g_cheated = false;
    g_used_cheats.clear();
    g_applied = false;
}

class $modify(SafeModePlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        Logic::get().update_safe_mode(this);
        if (!Logic::get().attempt_cheated() || m_isTestMode) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        m_isTestMode = true;
        PlayLayer::destroyPlayer(player, object);
        m_isTestMode = false;
    }
};
