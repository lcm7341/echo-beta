// safe mode: an attempt that used any hack saves no progress
// active_cheats() is whats on right now, attempt_cheats() is what was on at ANY point in ts attempt
// it works by holding gds own test mode flag from completion until the next reset, so gd itself skips the save
#include "Logic/logic.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>

using namespace geode::prelude;

namespace {
    bool g_cheated = false;
    bool g_applied = false;
    bool g_savedTestMode = false;
    std::vector<std::string_view> g_used;
}

std::vector<std::string_view> Logic::active_cheats() const {
    auto const& value = *this;
    std::vector<std::string_view> out;
    auto add = [&](bool on, std::string_view name) {
        if (on)
            out.push_back(name);
    };
    add(value.noclip_p1 || value.noclip_p2, "Noclip");
    add(value.speedhack != 1.f, "Speedhack");
    add(value.sample_rate() != static_cast<double>(VANILLA_TPS), "TPS");
    add(value.frame_advance, "Frame Advance");
    add(value.pause_before_kill, "Prevent Death");
    add(value.show_death_next, "Death Next Frame");
    add(value.backstep, "Backstep");
    add(value.unrounded_physics(), "Velocity Unrounding");
    add(value.pause_at_traj_length, "Pause At Trajectory Length");
    add(value.swap_players, "Swap Player Inputs");
    add(value.inverse_p1 || value.inverse_p2, "Inverse Inputs");
    add(value.maintain_gravity, "Maintain Gravity");
    add(value.same_gravity, "Same Gravity");
    add(value.no_mirror_portals, "No Mirror Portals");
    add(value.seed_override, "Seed Override");
    add(value.is_playing(), "Macro Playback");
    add(value.autoclicker, "Autoclicker");
    add(value.traj_autoclicker, "Trajectory Autoclicker");
    add(value.click_both, "Click Both");
    add(value.follow_mode != 0, "Follow Player");
    add(value.click_dash_orbs, "Click Dash Orbs");
    add(value.click_black_orbs, "Click Black Orbs");
    add(value.pad_black_orb, "Auto Black Orb on Pad");
    add(value.pad_blue_orb, "Auto Blue Orb on Pad");
    add(value.easy_black_ufo, "Easy Black Orb UFO");
    add(value.auto_swift, "Auto Swift");
    add(value.extra_clicks, "Extra Clicks");
    add(value.straight_fly(), "Auto Straight Fly");
    add(value.straight_ufo(), "Auto Straight UFO");
    add(value.show_traj, "Trajectory");
    add(value.show_hitboxes, "Hitboxes");
    add(value.layout_mode, "Layout");
    add(value.hide_p1 || value.hide_p2, "Hide Player");
    return out;
}

std::vector<std::string_view> const& Logic::attempt_cheats() const {
    return g_used;
}

bool Logic::attempt_cheated() const {
    return g_cheated;
}

void Logic::update_safe_mode(PlayLayer* play) {
    if (!play)
        return;
    if (!safeMode)
        return;
    for (auto name : active_cheats()) {
        g_cheated = true;
        if (std::ranges::find(g_used, name) == g_used.end())
            g_used.push_back(name);
    }
}

void Logic::hold_safe_mode_test_mode(PlayLayer* play) {
    if (!play || !g_cheated || g_applied)
        return;
    g_savedTestMode = play->m_isTestMode;
    play->m_isTestMode = true;
    g_applied = true;
}

void Logic::end_safe_mode_attempt(PlayLayer* play) {
    g_cheated = false;
    g_used.clear();
    if (g_applied && play)
        play->m_isTestMode = g_savedTestMode;
    g_applied = false;
}

void Logic::reset_safe_mode() {
    g_cheated = false;
    g_used.clear();
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
