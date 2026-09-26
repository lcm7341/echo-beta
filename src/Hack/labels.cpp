// the on screen labels (frame, time, percent, cps, position, velocity ...)
// auto position stacks the visible ones from a corner
#include "Logic/logic.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/label_types.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

class Labels {
public:
    static Labels& get() {
        static Labels value;
        return value;
    }

    void sync_label(PlayLayer* play, int tag, bool visible, LabelPlacement const& place, std::string const& text, ccColor3B color);
    void layout(PlayLayer* play);
    std::string format_clock(double seconds);
    std::string font;
    std::unordered_map<int, CCTextAlignment> alignments;
};

constexpr int FRAME_TAG = 0xEC01, CPS_TAG = 0xEC02, PERCENT_TAG = 0xEC03, TIME_TAG = 0xEC04;
constexpr int RECORD_TAG = 0xEC05, DEBUG_TAG = 0xEC07, POS_TAG = 0xEC08, VELOCITY_TAG = 0xEC09;
constexpr int ALIVE_TAG = 0xEC0A, MAX_CPS_TAG = 0xEC0B, GRAVITY_TAG = 0xEC0C, DEATH_TAG = 0xEC0D;
constexpr int UTILITY_TAG = 0xEC0E, SAFE_MODE_TAG = 0xEC0F;

namespace {
    LabelInfo const LABELS[] = {
        { "Show Recording Label", "recording", &Session::show_recording, &Session::recording_label, RECORD_TAG },
        { "Show CPS", "cps", &Session::show_cps, &Session::cps_label, CPS_TAG },
        { "Show Frame", "frame", &Session::show_frame, &Session::frame_label, FRAME_TAG },
        { "Show Time", "time", &Session::show_time, &Session::time_label, TIME_TAG },
        { "Show Position", "position", &Session::show_pos, &Session::pos_label, POS_TAG },
        { "Show Velocity", "velocity", &Session::show_velocity, &Session::velocity_label, VELOCITY_TAG },
        { "Show Alive", "alive", &Session::show_alive, &Session::alive_label, ALIVE_TAG },
        { "Show Max CPS", "max_cps", &Session::show_max_cps, &Session::max_cps_label, MAX_CPS_TAG },
        { "Show Gravity", "gravity", &Session::show_gravity, &Session::gravity_label, GRAVITY_TAG },
        { "Utility Labels", "utilities", &Session::show_utilities, &Session::utility_label, UTILITY_TAG },
        { "Safe Mode Cheats", "safe_mode", &Session::show_safe_mode, &Session::safe_mode_label, SAFE_MODE_TAG },
        { "Death Next Frame", "death_next", &Session::show_death_next, &Session::death_label, DEATH_TAG },
        { "Show Percent", "percent", &Session::show_percent, &Session::percent_label, PERCENT_TAG },
    };

    std::vector<std::string> build_font_names() {
        std::vector<std::string> names{ "Pusab", "Gold Pusab", "Chat" };
        for (int i = 1; i <= 59; ++i)
            names.push_back(fmt::format("Font {:02}", i));
        return names;
    }
}

std::span<LabelInfo const> label_infos() {
    return LABELS;
}

LabelInfo const* label_info(std::string const& key) {
    auto it = std::ranges::find(LABELS, key, [](LabelInfo const& info) {
        return std::string_view(info.key);
    });
    return it == std::end(LABELS) ? nullptr : &*it;
}

std::vector<LabelInfo const*> ordered_labels(Session const& value) {
    log::debug("ordered labels");
    std::vector<LabelInfo const*> out;
    for (auto const& key : value.label_order) {
        auto* info = label_info(key);
        if (info && std::ranges::find(out, info) == out.end()) {
            out.push_back(info);
        }
    }
    for (auto const& info : LABELS) {
        if (std::ranges::find(out, &info) == out.end()) {
            out.push_back(&info);
        }
    }
    return out;
}

void move_label(Session& value, LabelInfo const& label, int delta) {
    auto order = ordered_labels(value);
    auto at = std::ranges::find(order, &label) - order.begin();
    auto to = at + delta;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(order.size()))
        return;
    std::swap(order[static_cast<size_t>(at)], order[static_cast<size_t>(to)]);
    value.label_order.clear();
    for (auto* info : order)
        value.label_order.emplace_back(info->key);
}

std::vector<std::string> const& label_font_names() {
    static auto const names = build_font_names();
    return names;
}

std::string label_font_file(int index) {
    // TODO
    return {};
}

int label_font_index(std::string const& file) {
    // later
    return {};
}

void Labels::sync_label(PlayLayer* play, int tag, bool visible, LabelPlacement const& place, std::string const& text, ccColor3B color) {
    auto* label = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(tag));
    if (!visible) {
        if (label) {
            label->removeFromParent();
        }
        return;
    }
    auto const& logic = Logic::get();
    if (!label) {
        label = CCLabelBMFont::create(text.c_str(), font.c_str());
        if (!label)
            return;
        label->setAnchorPoint({ 0.f, 0.5f });
        alignments[tag] = kCCTextAlignmentLeft;
        play->addChild(label, 999, tag);
    }
    if (!logic.labels_auto) {
        if (label->getAnchorPoint() != CCPoint{ 0.f, 0.5f })
            label->setAnchorPoint({ 0.f, 0.5f });
        auto& alignment = alignments[tag];
        if (alignment != kCCTextAlignmentLeft) {
            alignment = kCCTextAlignmentLeft;
            label->setAlignment(alignment);
        }
        auto position = label->getPosition();
        if (position.x != place.x || position.y != place.y) {
            label->setPosition({ place.x, place.y });
        }
    }
    float scale = place.scale * logic.labelScale;
    if (label->getScaleX() != scale || label->getScaleY() != scale) {
        label->setScale(scale);
    }
    auto opacity = static_cast<GLubyte>(std::lround(std::clamp(place.opacity, 0, 100) * 2.55f));
    if (label->getOpacity() != opacity) {
        label->setOpacity(opacity);
    }
    auto cur = label->getColor();
    if (cur.r != color.r || cur.g != color.g || cur.b != color.b) {
        label->setColor(color);
    }
}

void Labels::layout(PlayLayer* play) {
    // later
}

std::string Labels::format_clock(double seconds) {
    int total = std::max(0, (int)(seconds));
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int secs = total % 60;
    return fmt::format("{:02}:{:02}:{:02}", hours, minutes, secs);
}

void Logic::draw_overlay(PlayLayer* play) {
    log::debug("draw overlay");
    auto const& logic = Logic::get();
    auto& labels = Labels::get();
    auto font = label_font_file(label_font_index(logic.labelFont));
    if (labels.font != font) {
        labels.font = font;
        for (auto const& info : label_infos()) {
            if (auto* old = play->getChildByTag(info.tag)) {
                old->removeFromParent();
            }
        }
    }
    bool show_labels = !(logic.render_hide_labels && Logic::get().render_capturing());
    Labels::get().sync_label(play, FRAME_TAG, show_labels && logic.show_frame, logic.frame_label, fmt::format("Frame: {}", Logic::get().get_frame(play) + 1), { 255, 255, 255 });

    unsigned p1 = Logic::get().count_presses_in_last_second(false);
    bool two_dual = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode && play->m_gameState.m_isDualMode;
    unsigned p2 = two_dual ? Logic::get().count_presses_in_last_second(true) : 0;
    ccColor3B cps_color{ 255, 255, 255 };
    if (logic.currently_pressing)
        cps_color = { 0, 255, 77 };
    else if (logic.cps_breaking)
        cps_color = { 255, 0, 0 };
    else if (logic.over_max_cps)
        cps_color = { 255, 72, 0 };
    if (two_dual)
        Labels::get().sync_label(play, CPS_TAG, show_labels && logic.show_cps, logic.cps_label, fmt::format("CPS: {}/{}", p1, p2), cps_color);
    else
        Labels::get().sync_label(play, CPS_TAG, show_labels && logic.show_cps, logic.cps_label, fmt::format("CPS: {}", p1), cps_color);

    static PlayLayer* peak_lvl = nullptr;
    static unsigned peak_p1 = 0, peak_p2 = 0;
    if (peak_lvl != play) {
        peak_lvl = play;
        peak_p1 = peak_p2 = 0;
    }
    peak_p1 = std::max(peak_p1, p1);
    peak_p2 = std::max(peak_p2, p2);
    std::string max_cps_text = logic.highest_cps;
    {
        auto colon = max_cps_text.find(": ");
        bool counted = colon != std::string::npos;
        int macro_best = counted ? std::atoi(max_cps_text.c_str() + colon + 2) : 0;
        unsigned live = std::max(peak_p1, peak_p2);
        if ((counted || max_cps_text.empty()) && (int)(live) > macro_best) {
            max_cps_text = fmt::format("{}: {}", peak_p2 > peak_p1 ? "P2" : "P1", live);
        }
    }

    std::string percent = "0%";
    if (logic.show_percent && play->m_player1 && play->m_endPortal) {
        float end_x = play->m_endPortal->getPositionX();
        float amount = end_x == 0.f ? 0.f : std::min(100.f, play->m_player1->getPositionX() / end_x * 100.f);
        if (logic.percent_accuracy <= 0)
            percent = fmt::format("{}%", (int)(amount));
        else
            percent = fmt::format("{:.{}f}%", amount, logic.percent_accuracy);
    }
    Labels::get().sync_label(play, PERCENT_TAG, show_labels && logic.show_percent, logic.percent_label, percent, { 255, 255, 255 });
    Labels::get().sync_label(
        play,
        TIME_TAG,
        show_labels && logic.show_time,
        logic.time_label,
        Labels::get().format_clock(play->m_gameState.m_levelTime),
        { 255, 255, 255 });

    bool recording_shown = show_labels && ((logic).is_recording() || (logic.backstep_target >= 0 && logic.backstep_state != RunState::Playing && logic.backstep_state != RunState::Idle)) && logic.show_recording;
    auto [holds, releases] = recording_shown ? Logic::get().count_holds(logic) : std::pair<int, int>{ 0, 0 };
    Labels::get().sync_label(
        play,
        RECORD_TAG,
        recording_shown,
        logic.recording_label,
        fmt::format("Recording: {}/{}", holds, releases),
        { 255, 255, 255 });

    if (auto* debug = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(DEBUG_TAG))) {
        debug->removeFromParent();
    }

    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    auto body_text = [&](auto&& text_for) {
        std::string text = text_for(play->m_player1, false);
        if (dual) {
            text += "   " + text_for(play->m_player2, true);
        }
        return text;
    };
    auto named = [&](bool second, std::string text) {
        if (!dual) {
            return text;
        }
        return std::string(second ? "P2 " : "P1 ") + text;
    };
    Labels::get().sync_label(play, POS_TAG, show_labels && logic.show_pos, logic.pos_label, body_text([&](PlayerObject* body, bool second) {
        if (!body) {
            return named(second, "--");
        }
        auto pos = body->getPosition();
        return named(second, fmt::format("{:.2f}, {:.2f}", pos.x, pos.y));
    }),
        { 255, 255, 255 });
    Labels::get().sync_label(play, VELOCITY_TAG, show_labels && logic.show_velocity, logic.velocity_label, body_text([&](PlayerObject* body, bool second) {
        if (!body) {
            return named(second, "--");
        }
        float forward = body->m_playerSpeed * (float)(body->m_speedMultiplier) * 5.77f;
        if (body->m_isGoingLeft)
            forward = -forward;
        if (body->m_isPlatformer)
            forward = (float)(body->m_platformerXVelocity);
        float vx = body->m_isSideways ? (float)(body->m_yVelocity) : forward;
        float vy = body->m_isSideways ? forward : (float)(body->m_yVelocity);
        return named(second, fmt::format("{:.2f}, {:.2f}", vx, vy));
    }),
        { 255, 255, 255 });
    auto clipped = [&](bool second) {
        return second ? logic.noclip_p2 && logic.noclip_dead_p2 : logic.noclip_p1 && logic.noclip_dead_p1;
    };
    bool dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (dual && play->m_player2 && play->m_player2->m_isDead) || clipped(false) || (dual && clipped(true));
    Labels::get().sync_label(play, ALIVE_TAG, show_labels && logic.show_alive, logic.alive_label, body_text([&](PlayerObject* body, bool second) {
        bool gone = play->m_playerDied || (body && body->m_isDead) || clipped(second);
        return named(second, gone ? "Dead" : "Alive");
    }),
        dead ? ccColor3B{ 255, 60, 60 } : ccColor3B{ 80, 255, 120 });
    Labels::get().sync_label(play, MAX_CPS_TAG, show_labels && logic.show_max_cps, logic.max_cps_label, fmt::format("Max CPS: {}", max_cps_text), { 255, 255, 255 });
    Labels::get().sync_label(play, GRAVITY_TAG, show_labels && logic.show_gravity, logic.gravity_label, body_text([&](PlayerObject* body, bool second) {
        bool flipped = body && body->m_isUpsideDown;
        return named(second, flipped ? "Flipped" : "Normal");
    }),
        { 255, 255, 255 });
    std::string utilities;
    if (show_labels && logic.show_utilities) {
        auto add = [&](bool on, std::string_view name) {
            if (!on)
                return;
            if (!utilities.empty()) {
                utilities += '\n';
            }
            utilities += name;
        };
        auto pair = [](bool p1, bool p2, std::string_view name) {
            if (p1 && p2) {
                return std::string(name);
            }
            return std::string(name) + (p1 ? " P1" : " P2");
        };
        if (logic.noclip_p1 || logic.noclip_p2) {
            add(true, pair(logic.noclip_p1, logic.noclip_p2, "Noclip"));
        }
        add(logic.maintain_gravity, "Maintain Gravity");
        add(logic.same_gravity, "Same Gravity");
        add(logic.pause_before_kill, "Prevent Death");
        add(logic.pause_at_traj_length, "Pause At Trajectory Length");
        add(logic.autoclicker, "Autoclicker");
        add(logic.traj_autoclicker, "Trajectory Autoclicker");
        add(logic.click_both, "Click Both");
        add(logic.follow_mode != 0, "Follow Player");
        add(logic.swap_players, "Swap Player Inputs");
        if (logic.inverse_p1 || logic.inverse_p2) {
            add(true, pair(logic.inverse_p1, logic.inverse_p2, "Inverse Inputs"));
        }
        add(logic.click_dash_orbs, "Click Dash Orbs");
        add(logic.click_black_orbs, "Click Black Orbs");
        add(logic.pad_black_orb, "Auto Black Orb on Pad");
        add(logic.pad_blue_orb, "Auto Blue Orb on Pad");
        add(logic.speedhack != 1.f, fmt::format("Speedhack {:g}x", logic.speedhack));
        add(logic.sample_rate() != static_cast<double>(VANILLA_TPS), fmt::format("{:g} TPS", logic.sample_rate()));
        add(logic.frame_advance, "Frame Advance");
        add(logic.backstep, "Backstep");
        add(logic.unrounded_physics(), "Velocity Unrounding");
        add(logic.layout_mode, "Layout");
        if (logic.hide_p1 || logic.hide_p2) {
            add(true, pair(logic.hide_p1, logic.hide_p2, "Hide Player"));
        }
        add(logic.no_mirror_portals, "No Mirror Portals");
        add(logic.seed_override, "Seed Override");
        add(logic.safeMode, "Safe Mode");
    }
    Labels::get().sync_label(play, UTILITY_TAG, !utilities.empty(), logic.utility_label, utilities, { 255, 200, 80 });
    std::string blocked;
    Labels::get().sync_label(play, SAFE_MODE_TAG, !blocked.empty(), logic.safe_mode_label, blocked, { 255, 60, 60 });
    for (int tag = 0xEC10; tag <= 0xEC1F; ++tag) {
        if (auto* leftover = play->getChildByTag(tag)) {
            leftover->removeFromParent();
        }
    }
    Logic::get().sync_death_label(play);
}

void Logic::sync_death_label(PlayLayer* play) {
    if (!play)
        return;
    auto const& logic = Logic::get();
    bool dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    bool show = !dead && logic.show_death_next && (logic.death_next_p1 || logic.death_next_p2);
    std::string text = "Death next frame";
    if (logic.death_next_p1 && logic.death_next_p2)
        text = "Death next frame";
    else if (play->m_gameState.m_isDualMode && logic.death_next_p1)
        text = "P1 dies next frame";
    else if (play->m_gameState.m_isDualMode && logic.death_next_p2)
        text = "P2 dies next frame";
    Labels::get().sync_label(play, DEATH_TAG, show, logic.death_label, text, { 255, 40, 40 });
    Labels::get().layout(play);
}
