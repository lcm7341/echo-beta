// the on screen labels (frame, time, percent, cps, position, velocity ...)
// auto position stacks the visible ones from a corner
#include "Logic/build_info.hpp"
#include "Logic/perf.hpp"
#include "Logic/logic.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/label_types.hpp"
#include "Hack/script_labels.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/OBB2D.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <psapi.h>
#endif
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

#define FRAME_TAG 60417
#define CPS_TAG 60418
#define PERCENT_TAG 60419
#define TIME_TAG 60420
#define RECORD_TAG 60421
#define OLD_DEBUG_TAG 60423
#define POS_TAG 60424
#define VELOCITY_TAG 60425
#define ALIVE_TAG 60426
#define MAX_CPS_TAG 60427
#define GRAVITY_TAG 60428
#define DEATH_TAG 60429
#define UTILITY_TAG 60430
#define SAFE_MODE_TAG 60431
#define FPS_TAG 60448
#define DEBUG_STATS_TAG 60449
#define SPEED_TAG 60450
#define SSB_TAG 60451

static LabelInfo const LABELS[] = {
    { "Show Recording Label", "recording", &Logic::show_recording, &Logic::recording_label, RECORD_TAG },
    { "Show CPS", "cps", &Logic::show_cps, &Logic::cps_label, CPS_TAG },
    { "Show Frame", "frame", &Logic::show_frame, &Logic::frame_label, FRAME_TAG },
    { "Show Time", "time", &Logic::show_time, &Logic::time_label, TIME_TAG },
    { "Show Position", "position", &Logic::show_pos, &Logic::pos_label, POS_TAG },
    { "Show Velocity", "velocity", &Logic::show_velocity, &Logic::velocity_label, VELOCITY_TAG },
    { "Show Alive", "alive", &Logic::show_alive, &Logic::alive_label, ALIVE_TAG },
    { "Show Max CPS", "max_cps", &Logic::show_max_cps, &Logic::max_cps_label, MAX_CPS_TAG },
    { "Show Gravity", "gravity", &Logic::show_gravity, &Logic::gravity_label, GRAVITY_TAG },
    { "Utility Labels", "utilities", &Logic::show_utilities, &Logic::utility_label, UTILITY_TAG },
    { "Safe Mode Cheats", "safe_mode", &Logic::show_safe_mode, &Logic::safe_mode_label, SAFE_MODE_TAG },
    { "Death Next Frame", "death_next", &Logic::show_death_next, &Logic::death_label, DEATH_TAG },
    { "Show Percent", "percent", &Logic::show_percent, &Logic::percent_label, PERCENT_TAG },
    { "Show FPS", "fps", &Logic::show_fps, &Logic::fps_label, FPS_TAG },
    { "Debug Stats", "debug", &Logic::show_debug, &Logic::debug_label, DEBUG_STATS_TAG },
    { "Show Speed", "speed", &Logic::show_speed, &Logic::speed_label, SPEED_TAG },
    { "Show SSB", "ssb", &Logic::show_ssb, &Logic::ssb_label, SSB_TAG },
    { "Running Scripts", "scripts", &Logic::show_scripts, &Logic::scripts_label, SCRIPTS_TAG },
};

struct ScriptLabel {
    int owner = 0;
    int tag = 0;
    std::string name, text;
    ScriptLabels::Look look;
};

struct LabelOverride {
    int owner = 0;
    std::string text;
};

static std::vector<ScriptLabel> g_script_labels;
static std::unordered_map<int, LabelOverride> g_overrides;
static std::vector<int> g_dropped_tags;
static std::string g_running_names;

static bool labels_shown() {
    auto& logic = Logic::get();
    return !(logic.render_hide_labels && logic.render_capturing());
}

static ScriptLabel* script_label_named(int owner, std::string const& name) {
    for (auto& label : g_script_labels) {
        if (label.owner == owner && label.name == name) return &label;
    }
    return nullptr;
}

static int free_script_tag() {
    for (int tag = SCRIPT_FIRST_TAG; tag <= SCRIPT_LAST_TAG; ++tag) {
        bool used = false;
        for (auto const& label : g_script_labels)
            used = used || label.tag == tag;
        if (!used) return tag;
    }
    return 0;
}

static void drop_script_label(size_t index) {
    g_dropped_tags.push_back(g_script_labels[index].tag);
    g_script_labels.erase(g_script_labels.begin() + (long)index);
}

namespace ScriptLabels {
    bool set(int owner, std::string const& name, std::string const& text, Look const* look) {
        if (auto* label = script_label_named(owner, name)) {
            label->text = text;
            if (look) label->look = *look;
            return true;
        }
        int tag = free_script_tag();
        if (!tag) return false;
        ScriptLabel label;
        label.owner = owner;
        label.tag = tag;
        label.name = name;
        label.text = text;
        if (look) label.look = *look;
        if (!label.look.placed) label.look.place.y -= 12.f * (float)(g_script_labels.size());
        g_script_labels.push_back(std::move(label));
        return true;
    }

    bool remove(int owner, std::string const& name) {
        for (size_t i = 0; i < g_script_labels.size(); ++i) {
            if (g_script_labels[i].owner != owner || g_script_labels[i].name != name) continue;
            drop_script_label(i);
            return true;
        }
        return false;
    }

    void clear(int owner) {
        for (size_t i = g_script_labels.size(); i-- > 0;) {
            if (g_script_labels[i].owner == owner) drop_script_label(i);
        }
        std::erase_if(g_overrides, [owner](auto const& entry) {
            return entry.second.owner == owner;
        });
    }

    void clear_all() {
        for (size_t i = g_script_labels.size(); i-- > 0;)
            drop_script_label(i);
        g_overrides.clear();
    }

    bool set_override(int owner, std::string const& key, std::string const* text) {
        auto* info = label_info(key);
        if (!info) return false;
        if (text) g_overrides[info->tag] = { owner, *text };
        else g_overrides.erase(info->tag);
        return true;
    }

    std::string const* override_text(int tag) {
        auto found = g_overrides.find(tag);
        return found == g_overrides.end() ? nullptr : &found->second.text;
    }

    void set_running_scripts(std::vector<std::string> const& names) {
        g_running_names.clear();
        for (auto const& name : names)
            g_running_names += (g_running_names.empty() ? "" : "\n") + name;
    }
}

static std::vector<std::string> build_font_names() {
    std::vector<std::string> names{ "Pusab", "Gold Pusab", "Chat" };
    for (int i = 1; i <= 59; ++i)
        names.push_back(fmt::format("Font {:02}", i));
    return names;
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

std::vector<LabelInfo const*> ordered_labels(Logic const& value) {
    std::vector<LabelInfo const*> out;
    for (auto const& key : value.label_order) {
        auto* info = label_info(key);
        if (info && std::ranges::find(out, info) == out.end()) out.push_back(info);
    }
    for (auto const& info : LABELS) {
        if (std::ranges::find(out, &info) == out.end()) out.push_back(&info);
    }
    return out;
}

void move_label(Logic& value, LabelInfo const& label, int delta) {
    auto order = ordered_labels(value);
    auto at = std::ranges::find(order, &label) - order.begin();
    auto to = at + delta;
    if (to < 0 || to >= (std::ptrdiff_t)order.size()) return;
    std::swap(order[(size_t)at], order[(size_t)to]);
    value.label_order.clear();
    for (auto* info : order)
        value.label_order.emplace_back(info->key);
}

std::vector<std::string> const& label_font_names() {
    static auto const names = build_font_names();
    return names;
}

std::string label_font_file(int index) {
    if (index <= 0) return "bigFont.fnt";
    if (index == 1) return "goldFont.fnt";
    if (index == 2) return "chatFont.fnt";
    return fmt::format("gjFont{:02}.fnt", std::clamp(index - 2, 1, 59));
}

int label_font_index(std::string const& file) {
    for (int i = 0; i < (int)(label_font_names().size()); ++i) {
        if (label_font_file(i) == file) return i;
    }
    return 0;
}

void Labels::sync_label(PlayLayer* play, int tag, bool visible, LabelPlacement const& place, std::string const& shown_text, ccColor3B color) {
    auto* label = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(tag));
    std::string const* over = ScriptLabels::override_text(tag);
    std::string const& text = over ? *over : shown_text;
    if (over) visible = labels_shown();
    if (!visible) {
        if (label) label->removeFromParent();
        return;
    }
    auto const& logic = Logic::get();
    if (!label) {
        label = CCLabelBMFont::create(text.c_str(), font.c_str());
        if (!label) return;
        label->setAnchorPoint({ 0.f, 0.5f });
        alignments[tag] = kCCTextAlignmentLeft;
        play->addChild(label, 999, tag);
    }
    // cocos rebuilds the whole label even if unchanged .......... w optimization?
    if (text != label->getString()) label->setString(text.c_str());
    if (!logic.labels_auto) {
        if (label->getAnchorPoint() != CCPoint{ 0.f, 0.5f })
            label->setAnchorPoint({ 0.f, 0.5f });
        auto& alignment = alignments[tag];
        if (alignment != kCCTextAlignmentLeft) {
            alignment = kCCTextAlignmentLeft;
            label->setAlignment(alignment);
        }
        auto position = label->getPosition();
        if (position.x != place.x || position.y != place.y)
            label->setPosition({ place.x, place.y });
    }
    float scale = place.scale * logic.label_scale;
    if (label->getScaleX() != scale || label->getScaleY() != scale) label->setScale(scale);
    auto opacity = (GLubyte)std::lround(std::clamp(place.opacity, 0, 100) * 2.55f);
    if (label->getOpacity() != opacity) label->setOpacity(opacity);
    auto current_color = label->getColor();
    if (current_color.r != color.r || current_color.g != color.g || current_color.b != color.b) label->setColor(color);
}

#ifdef GEODE_IS_IOS
float ios_corner_ratio();
#endif

void Labels::layout(PlayLayer* play) {
    auto const& logic = Logic::get();
    if (!logic.labels_auto || !play) return;
    auto win = CCDirector::get()->getWinSize();
    bool right = logic.label_corner == 1 || logic.label_corner == 3, bottom = logic.label_corner >= 2;
    const float MARGIN = 3.f;
    const float GAP = 2.f;
    auto order = ordered_labels(logic);
    float radius = 0.f;
#ifdef GEODE_IS_IOS
    // 1.15: apples corners are not circles belief it or no
    if (!Logic::get().render_active())
        radius = ios_corner_ratio() * std::min(win.width, win.height) * 1.15f;
#endif
    auto corner_inset = [radius](float from_edge) {
        if (from_edge >= radius) return 0.f;
        float up = radius - from_edge;
        return radius - std::sqrt(std::max(0.f, radius * radius - up * up));
    };
    float stacked_height = 0.f;
    auto stack = [&](int tag) {
        auto* label = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(tag));
        if (!label || !label->isVisible()) return;
        CCPoint anchor{ right ? 1.f : 0.f, bottom ? 0.f : 1.f };
        if (label->getAnchorPoint() != anchor) label->setAnchorPoint(anchor);
        auto& alignment = alignments[tag];
        auto wanted = right ? kCCTextAlignmentRight : kCCTextAlignmentLeft;
        if (alignment != wanted) {
            alignment = wanted;
            label->setAlignment(wanted);
        }
        float height = label->getScaledContentSize().height;
        float edge_inset = MARGIN + corner_inset(MARGIN + stacked_height);
        CCPoint position{ right ? win.width - edge_inset : edge_inset, bottom ? MARGIN + stacked_height : win.height - MARGIN - stacked_height };
        if (label->getPosition() != position) label->setPosition(position);
        stacked_height += height + GAP;
    };
    std::vector<int> tags;
    for (auto* info : order)
        tags.push_back(info->tag);
    for (auto const& label : g_script_labels)
        tags.push_back(label.tag);
    if (bottom) std::ranges::reverse(tags);
    for (int tag : tags)
        stack(tag);
}

void ScriptLabels::sync(PlayLayer* play, bool shown) {
    for (int tag : g_dropped_tags) {
        if (auto* gone = play->getChildByTag(tag)) gone->removeFromParent();
    }
    g_dropped_tags.clear();
    for (auto const& label : g_script_labels)
        Labels::get().sync_label(play, label.tag, shown && !label.text.empty(), label.look.place, label.text, label.look.color);
    auto const& logic = Logic::get();
    bool scripts_shown = shown && logic.show_scripts && !g_running_names.empty();
    Labels::get().sync_label(play, SCRIPTS_TAG, scripts_shown, logic.scripts_label, logic.scripts_label.prefix ? "Scripts:\n" + g_running_names : g_running_names, { 120, 230, 160 });
}

std::string Labels::format_clock(double seconds) {
    int total = std::max(0, (int)(seconds));
    int hours = total / 3600;
    int minutes = (total % 3600) / 60;
    int secs = total % 60;
    return fmt::format("{:02}:{:02}:{:02}", hours, minutes, secs);
}

static std::string ram_text() {
#ifdef GEODE_IS_WINDOWS
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)))
        return fmt::format("  RAM {}MB", info.WorkingSetSize / (1024 * 1024));
#endif
    return {};
}

static void sync_perf_labels(PlayLayer* play, bool show_labels) {
    auto const& logic = Logic::get();
    auto const& stats = perf_summary();
    bool fps_on = show_labels && logic.show_fps;
    bool debug_on = show_labels && logic.show_debug;
    Labels::get().sync_label(play, FPS_TAG, fps_on, logic.fps_label, logic.fps_label.prefix ? fmt::format("FPS: {:.0f}", stats.fps) : fmt::format("{:.0f} fps", stats.fps), { 255, 255, 255 });
    if (!debug_on) {
        Labels::get().sync_label(play, DEBUG_STATS_TAG, false, logic.debug_label, "", { 255, 255, 255 });
        return;
    }
    static std::string text;
    static auto last_built = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (!text.empty() && now - last_built < std::chrono::milliseconds(250)) {
        Labels::get().sync_label(play, DEBUG_STATS_TAG, true, logic.debug_label, text, { 255, 255, 255 });
        return;
    }
    last_built = now;
    text.clear();
    text += fmt::format("{:.1f} fps  avg {:.2f}ms  median {:.2f}  1% {:.2f}  max {:.2f}  hitches/s {}\n", stats.fps, stats.avg_ms, stats.median_ms, stats.low1_ms, stats.max_ms, stats.hitches);
    if (!stats.last_hitch.empty()) text += "last hitch: " + stats.last_hitch + "\n";
    text += fmt::format("avg ms  phys {:.2f}  bstep {:.2f}  traj {:.2f}  auto {:.2f}  hbox {:.2f}  lbl {:.2f}  draw+wait {:.2f}\n", stats.slot_ms[PERF_PHYSICS], stats.slot_ms[PERF_BACKSTEP],
        stats.slot_ms[PERF_TRAJECTORY], stats.slot_ms[PERF_AUTOCLICK], stats.slot_ms[PERF_HITBOXES], stats.slot_ms[PERF_LABELS], stats.draw_ms);
    text += fmt::format("max ms  phys {:.2f}  bstep {:.2f}  traj {:.2f}  auto {:.2f}  hbox {:.2f}  lbl {:.2f}\n", stats.slot_max_ms[PERF_PHYSICS], stats.slot_max_ms[PERF_BACKSTEP],
        stats.slot_max_ms[PERF_TRAJECTORY], stats.slot_max_ms[PERF_AUTOCLICK], stats.slot_max_ms[PERF_HITBOXES], stats.slot_max_ms[PERF_LABELS]);
    text += fmt::format("tps {:.1f} real / {:g} set  steps/frame {:.2f}  level speed {:.3f}x (hack {:g}x)\n", stats.steps_per_sec, logic.sample_rate(), stats.steps_per_frame,
        stats.level_speed, logic.speedhack);
    text += fmt::format("dt gd {:.2f}ms used {:.2f}ms  cap {:.2f}ms  carry {:.3f}ms  lock fps {}  real time {}\n", stats.gd_dt_ms, stats.used_dt_ms,
        CCDirector::sharedDirector()->getAnimationInterval() * 1000.0, logic.leftover_time * 1000.0, logic.lock_delta ? "on" : "off", logic.real_time_mode ? "on" : "off");
    if (logic.backstep)
        text += fmt::format("backstep  saves/s {}  gap {} steps  save {:.2f}ms avg  held {}  last: gd {:.2f} extra {:.2f} objects {:.2f} trim {:.2f}\n",
            stats.saves_per_sec, logic.backstep_interval(), logic.backstep_save_sec * 1000.0, logic.backsteps.size(), stats.save_gd_ms, stats.save_extra_ms,
            stats.save_objects_ms, stats.save_trim_ms);
    if (stats.traj_steps_per_frame > 0.0 || logic.show_traj || logic.traj_autoclicker || logic.pause_before_kill)
        text += fmt::format("trajectory  {:.0f} predicted steps/frame  moving objects {}\n", stats.traj_steps_per_frame, logic.traj_moving_objects ? "on" : "off");
    double level_ms = play->m_gameState.m_levelTime * 1000.0;
    double music_ms = FMODAudioEngine::get() ? FMODAudioEngine::get()->getMusicTimeMS(0) : 0.0;
    text += fmt::format("build {}  frame {}  objects {}  music - level {:.0f}ms{}", build_id(), logic.step_frame, play->m_objects ? play->m_objects->count() : 0,
        music_ms - level_ms, ram_text());
    Labels::get().sync_label(play, DEBUG_STATS_TAG, true, logic.debug_label, text, { 255, 255, 255 });
}

static std::string with_label_name(LabelPlacement const& place, char const* name, std::string text) {
    return place.prefix ? fmt::format("{}: {}", name, text) : text;
}

void Logic::draw_overlay(PlayLayer* play) {
    PerfScope label_time(PERF_LABELS);
    auto const& logic = Logic::get();
    auto& labels = Labels::get();
    auto font = label_font_file(label_font_index(logic.label_font));
    if (labels.font != font) {
        labels.font = font;
        for (auto const& info : label_infos()) {
            if (auto* old = play->getChildByTag(info.tag)) old->removeFromParent();
        }
    }
    bool show_labels = labels_shown();
    Labels::get().sync_label(play, FRAME_TAG, show_labels && logic.show_frame, logic.frame_label, with_label_name(logic.frame_label, "Frame", fmt::format("{}", Logic::get().get_frame(play) + 1)), { 255, 255, 255 });

    unsigned cps_p1 = Logic::get().count_presses_in_last_second(false);
    bool two_player_dual = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode && play->m_gameState.m_isDualMode;
    unsigned cps_p2 = two_player_dual ? Logic::get().count_presses_in_last_second(true) : 0;
    ccColor3B cps_color{ 255, 255, 255 };
    if (logic.currently_pressing)
        cps_color = { 0, 255, 77 };
    else if (logic.cps_breaking)
        cps_color = { 255, 0, 0 };
    else if (logic.over_max_cps)
        cps_color = { 255, 72, 0 };
    if (two_player_dual)
        Labels::get().sync_label(play, CPS_TAG, show_labels && logic.show_cps, logic.cps_label, with_label_name(logic.cps_label, "CPS", fmt::format("{}/{}", cps_p1, cps_p2)), cps_color);
    else
        Labels::get().sync_label(play, CPS_TAG, show_labels && logic.show_cps, logic.cps_label, with_label_name(logic.cps_label, "CPS", fmt::format("{}", cps_p1)), cps_color);

    static PlayLayer* peak_level = nullptr;
    static GJGameLevel* peak_game_level = nullptr;
    static unsigned peak_cps_p1 = 0, peak_cps_p2 = 0;
    if (peak_level != play || peak_game_level != play->m_level) {
        peak_level = play;
        peak_game_level = play->m_level;
        peak_cps_p1 = peak_cps_p2 = 0;
    }
    peak_cps_p1 = std::max(peak_cps_p1, cps_p1);
    peak_cps_p2 = std::max(peak_cps_p2, cps_p2);
    std::string max_cps_text = logic.highest_cps;
    {
        auto colon = max_cps_text.find(": ");
        bool counted = colon != std::string::npos;
        int macro_best = counted ? std::atoi(max_cps_text.c_str() + colon + 2) : 0;
        unsigned live_peak = std::max(peak_cps_p1, peak_cps_p2);
        if ((counted || max_cps_text.empty()) && (int)(live_peak) > macro_best)
            max_cps_text = fmt::format("{}: {}", peak_cps_p2 > peak_cps_p1 ? "P2" : "P1", live_peak);
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
    Labels::get().sync_label(play, PERCENT_TAG, show_labels && logic.show_percent, logic.percent_label, with_label_name(logic.percent_label, "Percent", percent), { 255, 255, 255 });
    Labels::get().sync_label(
        play,
        TIME_TAG,
        show_labels && logic.show_time,
        logic.time_label,
        with_label_name(logic.time_label, "Time", Labels::get().format_clock(play->m_gameState.m_levelTime)),
        { 255, 255, 255 });

    // backstep is playing but ehhh no its not
    bool recording_shown = show_labels && ((logic).is_recording() || (logic.backstep_target >= 0 && logic.backstep_state != RunState::Playing && logic.backstep_state != RunState::Idle)) && logic.show_recording;
    auto [holds, releases] = recording_shown ? Logic::get().count_holds() : std::pair<int, int>{ 0, 0 };
    Labels::get().sync_label(
        play,
        RECORD_TAG,
        recording_shown,
        logic.recording_label,
        with_label_name(logic.recording_label, "Recording", fmt::format("{}/{}", holds, releases)),
        { 255, 255, 255 });

    if (auto* old_debug_label = typeinfo_cast<CCLabelBMFont*>(play->getChildByTag(OLD_DEBUG_TAG)))
        old_debug_label->removeFromParent();

    bool dual = play->m_gameState.m_isDualMode && play->m_player2;
    auto text_per_player = [&](auto&& text_for) {
        std::string text = text_for(play->m_player1, false);
        if (dual) text += "   " + text_for(play->m_player2, true);
        return text;
    };
    auto with_player_tag = [&](bool second, std::string text) {
        if (!dual) return text;
        return std::string(second ? "P2 " : "P1 ") + text;
    };
    int decimals = std::clamp(logic.label_decimals, 0, 8);
    Labels::get().sync_label(play, POS_TAG, show_labels && logic.show_pos, logic.pos_label, with_label_name(logic.pos_label, "Position", text_per_player([&](PlayerObject* body, bool second) {
        if (!body) return with_player_tag(second, "--");
        auto pos = body->getPosition();
        return with_player_tag(second, fmt::format("{:.{}f}, {:.{}f}", pos.x, decimals, pos.y, decimals));
    })),
        { 255, 255, 255 });
    Labels::get().sync_label(play, VELOCITY_TAG, show_labels && logic.show_velocity, logic.velocity_label, with_label_name(logic.velocity_label, "Velocity", text_per_player([&](PlayerObject* body, bool second) {
        if (!body) return with_player_tag(second, "--");
        float forward = body->m_playerSpeed * (float)(body->m_speedMultiplier) * 5.77f;
        if (body->m_isGoingLeft) forward = -forward;
        if (body->m_isPlatformer) forward = (float)(body->m_platformerXVelocity);
        float vx = body->m_isSideways ? (float)(body->m_yVelocity) : forward;
        float vy = body->m_isSideways ? forward : (float)(body->m_yVelocity);
        return with_player_tag(second, fmt::format("{:.{}f}, {:.{}f}", vx, decimals, vy, decimals));
    })),
        { 255, 255, 255 });
    auto noclip_dead = [&](bool second) {
        return second ? logic.noclip_p2 && logic.noclip_dead_p2 : logic.noclip_p1 && logic.noclip_dead_p1;
    };
    bool dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (dual && play->m_player2 && play->m_player2->m_isDead) || noclip_dead(false) || (dual && noclip_dead(true));
    Labels::get().sync_label(play, ALIVE_TAG, show_labels && logic.show_alive, logic.alive_label, with_label_name(logic.alive_label, "Status", text_per_player([&](PlayerObject* body, bool second) {
        bool gone = play->m_playerDied || (body && body->m_isDead) || noclip_dead(second);
        return with_player_tag(second, gone ? "Dead" : "Alive");
    })),
        dead ? ccColor3B{ 255, 60, 60 } : ccColor3B{ 80, 255, 120 });
    Labels::get().sync_label(play, MAX_CPS_TAG, show_labels && logic.show_max_cps, logic.max_cps_label, with_label_name(logic.max_cps_label, "Max CPS", max_cps_text), { 255, 255, 255 });
    Labels::get().sync_label(play, GRAVITY_TAG, show_labels && logic.show_gravity, logic.gravity_label, with_label_name(logic.gravity_label, "Gravity", text_per_player([&](PlayerObject* body, bool second) {
        bool flipped = body && body->m_isUpsideDown;
        return with_player_tag(second, flipped ? "Flipped" : "Normal");
    })),
        { 255, 255, 255 });
    Labels::get().sync_label(play, SPEED_TAG, show_labels && logic.show_speed, logic.speed_label, with_label_name(logic.speed_label, "Speed", text_per_player([&](PlayerObject* body, bool second) {
        if (!body) return with_player_tag(second, "--");
        float speed = body->m_playerSpeed;
        char const* text = speed == 0.7f ? "0.5x" : speed == 1.1f ? "2x" : speed == 1.3f ? "3x" : speed == 1.6f ? "4x" : speed == 0.9f ? "1x" : nullptr;
        return with_player_tag(second, text ? std::string(text) : fmt::format("{:g}", speed));
    })),
        { 255, 255, 255 });
    Labels::get().sync_label(play, SSB_TAG, show_labels && logic.show_ssb, logic.ssb_label,
        with_label_name(logic.ssb_label, "SSB", fmt::format("{:.6f}x", Logic::get().scroll_speed_factor())), { 255, 255, 255 });
    // so were in copper
    std::string utilities;
    if (show_labels && logic.show_utilities) {
        auto add = [&](bool on, std::string_view name) {
            if (!on) return;
            if (!utilities.empty()) utilities += '\n';
            utilities += name;
        };
        auto with_player_suffix = [](bool p1, bool p2, std::string_view name) {
            if (p1 && p2) return std::string(name);
            return std::string(name) + (p1 ? " P1" : " P2");
        };
        if (logic.noclip_p1 || logic.noclip_p2)
            add(true, with_player_suffix(logic.noclip_p1, logic.noclip_p2, "Noclip"));
        add(logic.maintain_gravity, "Maintain Gravity");
        add(logic.same_gravity, "Same Gravity");
        add(logic.pause_before_kill, "Prevent Death");
        add(logic.pause_at_traj_length, "Pause At Trajectory Length");
        add(logic.autoclicker, "Autoclicker");
        add(logic.traj_autoclicker, "Trajectory Autoclicker");
        add(logic.click_both, "Click Both");
        add(logic.follow_mode != 0, "Follow Player");
        add(logic.swap_players, "Swap Player Inputs");
        if (logic.inverse_p1 || logic.inverse_p2)
            add(true, with_player_suffix(logic.inverse_p1, logic.inverse_p2, "Inverse Inputs"));
        add(logic.click_dash_orbs, "Click Dash Orbs");
        add(logic.click_black_orbs, "Click Black Orbs");
        add(logic.pad_black_orb, "Auto Black Orb on Pad");
        add(logic.pad_blue_orb, "Auto Blue Orb on Pad");
        add(logic.easy_black_ufo, "Easy Black Orb UFO");
        add(logic.auto_swift, "Auto Swift");
        add(logic.extra_clicks, fmt::format("Extra Clicks x{}", logic.extra_click_count));
        add(logic.straight_fly(), "Auto Straight Fly");
        add(logic.straight_ufo(), "Auto Straight UFO");
        add(logic.speedhack != 1.f, fmt::format("Speedhack {:g}x", logic.speedhack));
        add(logic.sample_rate() != (double)VANILLA_TPS, fmt::format("{:g} TPS", logic.sample_rate()));
        add(logic.frame_advance, "Frame Advance");
        add(logic.backstep, "Backstep");
        add(logic.unrounded_physics(), "Velocity Unrounding");
        add(logic.layout_mode, "Layout");
        if (logic.hide_p1 || logic.hide_p2)
            add(true, with_player_suffix(logic.hide_p1, logic.hide_p2, "Hide Player"));
        add(logic.no_mirror_portals, "No Mirror Portals");
        add(logic.seed_override, "Seed Override");
        add(logic.safe_mode, "Safe Mode");
    }
    Labels::get().sync_label(play, UTILITY_TAG, !utilities.empty(), logic.utility_label, utilities, { 255, 200, 80 });
    std::string cheats_text;
    if (show_labels && logic.show_safe_mode && logic.safe_mode) {
        auto names = Logic::get().attempt_cheats();
        for (auto name : Logic::get().active_cheats()) {
            if (std::ranges::find(names, name) == names.end()) names.push_back(name);
        }
        for (auto name : names) {
            cheats_text += cheats_text.empty() ? "Cheats: " : ", ";
            cheats_text += name;
        }
    }
    Labels::get().sync_label(play, SAFE_MODE_TAG, !cheats_text.empty(), logic.safe_mode_label, cheats_text, { 255, 60, 60 });
    sync_perf_labels(play, show_labels);
    ScriptLabels::sync(play, show_labels);
    for (int tag = 60432; tag <= 60447; ++tag) {
        if (auto* leftover = play->getChildByTag(tag)) leftover->removeFromParent();
    }
    Logic::get().sync_death_label(play);
}

void Logic::sync_death_label(PlayLayer* play) {
    if (!play) return;
    auto const& logic = Logic::get();
    bool dead = play->m_playerDied || (play->m_player1 && play->m_player1->m_isDead) || (play->m_player2 && play->m_player2->m_isDead);
    bool show = !dead && logic.show_death_next && (logic.death_next_p1 || logic.death_next_p2);
    if (logic.render_hide_labels && Logic::get().render_capturing()) show = false;
    std::string text = "Death next frame";
    if (logic.death_next_p1 && logic.death_next_p2) text = "Death next frame";
    else if (play->m_gameState.m_isDualMode && logic.death_next_p1) text = "P1 dies next frame";
    else if (play->m_gameState.m_isDualMode && logic.death_next_p2) text = "P2 dies next frame";
    Labels::get().sync_label(play, DEATH_TAG, show, logic.death_label, text, { 255, 40, 40 });
    Labels::get().layout(play);
}
