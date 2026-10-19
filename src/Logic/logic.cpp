#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include "Hooks/opt.hpp"
#include "GUI/gui.hpp"
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/ModSettingsManager.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>

namespace fs = std::filesystem;

using namespace geode::prelude;

static bool saving_all_settings = false;

Logic& Logic::get() {
    static Logic logic;
    return logic;
}

const char* Logic::state_name(RunState state) {
    switch (state) {
    case RunState::Recording:
        return "Recording";
    case RunState::Playing:
        return "Playing";
    case RunState::RecordingAndPlaying:
        return "Record+Play";
    case RunState::Idle:
        return "Idle";
    }
    return "Idle";
}

fs::path Logic::save_root() {
    return Mod::get()->getSaveDir();
}

std::string Logic::read_folder_setting(const char* key) {
    auto value = Mod::get()->getSettingValue<fs::path>(key);
    return geode::utils::string::pathToString(value);
}

static std::string utf8_path_text(const std::string& text) {
    try {
        ReplayIO::utf8_path(text);
        return text;
    }
    catch (...) {
    }
    try {
        return geode::utils::string::pathToString(fs::path(text));
    }
    catch (...) {
        return {};
    }
}

matjson::Value Logic::placement_json(const LabelPlacement& placement) {
    matjson::Value json;
    json["x"] = placement.x;
    json["y"] = placement.y;
    json["scale"] = placement.scale;
    json["opacity"] = placement.opacity;
    json["prefix"] = placement.prefix;
    return json;
}

LabelPlacement Logic::read_placement(const matjson::Value& json, LabelPlacement fallback) {
    if (!json.isObject()) return fallback;
    if (auto number = json["x"].asDouble()) fallback.x = (float)number.unwrap();
    if (auto number = json["y"].asDouble()) fallback.y = (float)number.unwrap();
    if (auto number = json["scale"].asDouble()) fallback.scale = (float)number.unwrap();
    if (auto number = json["opacity"].asInt()) fallback.opacity = (int)number.unwrap();
    if (auto flag = json["prefix"].asBool()) fallback.prefix = flag.unwrap();
    return fallback;
}

void Logic::boot() {
    auto root = save_root();
    std::error_code error;
    for (const auto* folder : { "replays", "converted", "clickpacks", "renders", "logs" }) {
        fs::create_directories(root / folder, error);
    }
    auto packs = root / "clickpacks";
    bool packs_empty = true;
    if (fs::is_directory(packs)) {
        for (const auto& entry : fs::directory_iterator(packs, error)) {
            (void)entry;
            packs_empty = false;
            break;
        }
    }
    if (packs_empty) {
        for (const auto* folder : {
                 "example/clicks",
                 "example/releases",
                 "example/soft_clicks",
                 "example/soft_releases",
                 "example/hard_clicks",
                 "example/hard_releases",
                 "example/micro_clicks",
                 "example/micro_releases",
             }) {
            fs::create_directories(packs / folder, error);
        }
    }
#ifndef GEODE_IS_MOBILE
    auto migrate_key = [](const char* id, enumKeyCodes from, KeyboardModifier from_mods, enumKeyCodes to, KeyboardModifier to_mods) {
        auto cur = Mod::get()->getSettingValue<std::vector<Keybind>>(id);
        if (cur.size() != 1 || cur[0].key != from || cur[0].modifiers != from_mods) return;
        Mod::get()->setSettingValue<std::vector<Keybind>>(id, { Keybind(to, to_mods) });
    };
    const auto left_alt = KeyboardModifier::Alt;
    migrate_key("open-menu", KEY_Alt, KeyboardModifier::None, KEY_LeftMenu, left_alt);
    migrate_key("open-menu", KEY_LeftMenu, KeyboardModifier::None, KEY_LeftMenu, left_alt);
    migrate_key("toggle-advance", KEY_F6, KeyboardModifier::None, KEY_V, KeyboardModifier::None);
    migrate_key("step-frame", KEY_F7, KeyboardModifier::None, KEY_C, KeyboardModifier::None);
#endif
    if (std::fabs(Logic::read_float("macro-fps", 240.f) - 60.f) < 0.01f)
        Logic::write_float("macro-fps", 240.f);
    pull_settings();
    SettingChangedEventV3().listen([](std::string_view modID, std::string_view, std::shared_ptr<SettingV3>) {
                               if (saving_all_settings || modID != Mod::get()->getID())
                                   return ListenerResult::Propagate;
                               Logic::get().save_settings_file();
                               return ListenerResult::Propagate;
                           })
        .leak();
}

void Logic::pull_settings() {
    bool had_trail = false, trail_file = false;
    auto path = save_root() / "settings.json";
    if (fs::exists(path)) {
        std::ifstream file(path);
        auto parsed = matjson::Value::parse(file);
        if (parsed) {
            const auto& json = parsed.unwrap();
            if (auto number = json["macro_fps"].asDouble()) fps = (float)number.unwrap();
            if (auto number = json["speedhack"].asDouble()) speedhack = (float)number.unwrap();
            if (auto number = json["speedhack_memory"].asDouble())
                saved_speedhack = (float)number.unwrap();
            else if (std::abs(speedhack - 1.f) > 0.05f) saved_speedhack = speedhack;
            if (auto flag = json["real_time_mode"].asBool()) real_time_mode = flag.unwrap();
            if (auto flag = json["true_real_time"].asBool()) true_real_time = flag.unwrap();
            if (auto number = json["lowest_playback_fps"].asInt()) lowest_playback_fps = (int)number.unwrap();
            if (auto flag = json["prefetch_helper"].asBool()) prefetch_helper = flag.unwrap();
            if (auto flag = json["batched_effects"].asBool()) batched_effects = flag.unwrap();
            if (auto flag = json["lock_delta"].asBool()) lock_delta = flag.unwrap();
            if (auto flag = json["fix_tps_rotation"].asBool()) fix_tps_rotation = flag.unwrap();
            if (auto flag = json["save_debug"].asBool()) save_debug = flag.unwrap();
            if (auto flag = json["lock_display_fps"].asBool()) lock_display_fps = flag.unwrap();
            if (auto number = json["playback_fps"].asDouble())
                playback_fps = std::clamp((float)number.unwrap(), 0.f, 100000.f);
            else if (auto number = json["playback_fps"].asInt())
                playback_fps = std::clamp((float)number.unwrap(), 0.f, 100000.f);
            if (auto flag = json["ignore_inputs"].asBool()) ignore_inputs = flag.unwrap();
            if (auto flag = json["no_overwrite"].asBool()) no_overwrite = flag.unwrap();
            if (auto flag = json["playback_tps_changes"].asBool())
                playback_tps_changes = flag.unwrap();
            if (auto flag = json["trace_logging"].asBool()) trace_logging = flag.unwrap();
            if (auto flag = json["no_editor_keybinds"].asBool()) no_editor_keybinds = flag.unwrap();
            if (auto flag = json["opt_effects"].asBool()) optimize_effects = flag.unwrap();
            if (auto flag = json["record_p1"].asBool()) record_p1 = flag.unwrap();
            if (auto flag = json["record_p2"].asBool()) record_p2 = flag.unwrap();
            if (auto flag = json["play_p1"].asBool()) play_p1 = flag.unwrap();
            if (auto flag = json["play_p2"].asBool()) play_p2 = flag.unwrap();
            if (auto flag = json["click_both"].asBool()) click_both = flag.unwrap();
            if (auto number = json["follow_mode"].asInt())
                follow_mode = std::clamp((int)number.unwrap(), 0, 2);
            if (auto number = json["follow_delay"].asDouble())
                follow_delay = std::max((float)number.unwrap(), 0.f);
            if (auto flag = json["follow_delay_ms"].asBool()) follow_delay_ms = flag.unwrap();
            if (auto flag = json["maintain_gravity"].asBool()) maintain_gravity = flag.unwrap();
            if (auto flag = json["same_gravity"].asBool()) same_gravity = flag.unwrap();
            if (auto flag = json["hide_p1"].asBool()) hide_p1 = flag.unwrap();
            if (auto flag = json["hide_p2"].asBool()) hide_p2 = flag.unwrap();
            if (auto flag = json["hide_hitbox_p1"].asBool()) hide_hitbox_p1 = flag.unwrap();
            if (auto flag = json["hide_hitbox_p2"].asBool()) hide_hitbox_p2 = flag.unwrap();
            if (auto flag = json["hitboxes_follow_hide_player"].asBool())
                hitboxes_follow_hide_player = flag.unwrap();
            if (auto flag = json["hide_trajectory_p1"].asBool()) hide_traj_p1 = flag.unwrap();
            if (auto flag = json["hide_trajectory_p2"].asBool()) hide_traj_p2 = flag.unwrap();
            if (auto flag = json["trajectory_follow_hide_player"].asBool())
                traj_follow_hide_player = flag.unwrap();
            if (auto flag = json["disable_death_effect"].asBool())
                disable_death_effect = flag.unwrap();
            if (auto flag = json["safe_mode"].asBool()) safe_mode = flag.unwrap();
            if (auto flag = json["swap_players"].asBool()) swap_players = flag.unwrap();
            if (auto flag = json["inverse_p1"].asBool()) inverse_p1 = flag.unwrap();
            if (auto flag = json["inverse_p2"].asBool()) inverse_p2 = flag.unwrap();
            if (auto flag = json["noclip_p1"].asBool()) noclip_p1 = flag.unwrap();
            if (auto flag = json["noclip_memory_p1"].asBool()) noclip_memory_p1 = flag.unwrap();
            if (auto flag = json["noclip_memory_p2"].asBool()) noclip_memory_p2 = flag.unwrap();
            if (auto flag = json["noclip_p2"].asBool()) noclip_p2 = flag.unwrap();
            if (auto flag = json["show_hitboxes"].asBool()) show_hitboxes = flag.unwrap();
            if (auto flag = json["layout_mode"].asBool()) layout_mode = flag.unwrap();
            if (auto flag = json["layout_default_art"].asBool()) layout_default_art = flag.unwrap();
            if (auto flag = json["disable_shakes"].asBool()) disable_shakes = flag.unwrap();
            if (auto flag = json["no_mirror_portals"].asBool()) no_mirror_portals = flag.unwrap();
            if (auto flag = json["show_frame"].asBool()) show_frame = flag.unwrap();
            if (auto flag = json["show_cps"].asBool()) show_cps = flag.unwrap();
            if (auto flag = json["show_percent"].asBool()) show_percent = flag.unwrap();
            if (auto flag = json["show_time"].asBool()) show_time = flag.unwrap();
            if (auto flag = json["show_recording"].asBool()) show_recording = flag.unwrap();
            if (auto flag = json["show_position"].asBool()) show_pos = flag.unwrap();
            if (auto flag = json["show_fps"].asBool()) show_fps = flag.unwrap();
            if (auto flag = json["show_debug"].asBool()) show_debug = flag.unwrap();
            if (auto flag = json["show_speed"].asBool()) show_speed = flag.unwrap();
            if (auto flag = json["show_ssb"].asBool()) show_ssb = flag.unwrap();
            if (auto number = json["label_decimals"].asInt())
                label_decimals = std::clamp((int)number.unwrap(), 0, 8);
            if (auto flag = json["show_velocity"].asBool()) show_velocity = flag.unwrap();
            if (auto flag = json["show_alive"].asBool()) show_alive = flag.unwrap();
            if (auto flag = json["show_max_cps"].asBool()) show_max_cps = flag.unwrap();
            if (auto flag = json["show_gravity"].asBool()) show_gravity = flag.unwrap();
            if (auto flag = json["show_utilities"].asBool()) show_utilities = flag.unwrap();
            if (auto flag = json["show_safe_mode"].asBool()) show_safe_mode = flag.unwrap();
            if (auto flag = json["labels_auto"].asBool()) labels_auto = flag.unwrap();
            if (auto number = json["label_corner"].asInt())
                label_corner = std::clamp((int)number.unwrap(), 0, 3);
            if (auto number = json["label_scale"].asDouble())
                label_scale = std::clamp((float)number.unwrap(), .05f, 10.f);
            if (auto text = json["label_font"].asString()) label_font = text.unwrap();
            if (json["label_order"].isArray()) {
                label_order.clear();
                for (const auto& key : json["label_order"]) {
                    if (auto text = key.asString()) label_order.push_back(text.unwrap());
                }
            }
            if (auto flag = json["show_death_next"].asBool()) show_death_next = flag.unwrap();
            if (auto flag = json["click_dash_orbs"].asBool()) click_dash_orbs = flag.unwrap();
            if (auto flag = json["click_black_orbs"].asBool()) click_black_orbs = flag.unwrap();
            if (auto number = json["orb_click_count"].asInt())
                orb_click_count = std::max(1, (int)number.unwrap());
            if (auto flag = json["pad_black_orb"].asBool()) pad_black_orb = flag.unwrap();
            if (auto flag = json["pad_blue_orb"].asBool()) pad_blue_orb = flag.unwrap();
            if (auto flag = json["easy_black_ufo"].asBool()) easy_black_ufo = flag.unwrap();
            if (auto flag = json["auto_swift"].asBool()) auto_swift = flag.unwrap();
            if (auto flag = json["extra_clicks"].asBool()) extra_clicks = flag.unwrap();
            if (auto flag = json["straight_fly_p1"].asBool()) straight_fly_p1 = flag.unwrap();
            if (auto flag = json["straight_fly_p2"].asBool()) straight_fly_p2 = flag.unwrap();
            if (auto flag = json["straight_ufo_p1"].asBool()) straight_ufo_p1 = flag.unwrap();
            if (auto flag = json["straight_ufo_p2"].asBool()) straight_ufo_p2 = flag.unwrap();
            if (auto flag = json["hide_endscreen"].asBool()) hide_endscreen = flag.unwrap();
            if (auto flag = json["hide_new_best"].asBool()) hide_new_best = flag.unwrap();
            if (auto flag = json["no_effects"].asBool()) no_effects = flag.unwrap();
            if (auto number = json["straight_fly_threshold_p1"].asDouble())
                straight_fly_threshold_p1 = std::max((float)number.unwrap(), 0.f);
            if (auto number = json["straight_fly_threshold_p2"].asDouble())
                straight_fly_threshold_p2 = std::max((float)number.unwrap(), 0.f);
            if (auto number = json["straight_ufo_target_p1"].asDouble())
                straight_ufo_target_p1 = std::max((float)number.unwrap(), 0.f);
            if (auto number = json["straight_ufo_target_p2"].asDouble())
                straight_ufo_target_p2 = std::max((float)number.unwrap(), 0.f);
            if (auto number = json["straight_ufo_threshold_p1"].asDouble())
                straight_ufo_threshold_p1 = std::max((float)number.unwrap(), 0.f);
            if (auto number = json["straight_ufo_threshold_p2"].asDouble())
                straight_ufo_threshold_p2 = std::max((float)number.unwrap(), 0.f);
            if (auto number = json["extra_click_count"].asInt())
                extra_click_count = std::clamp((int)number.unwrap(), 1, 16);
            if (auto flag = json["autoclicker"].asBool()) autoclicker = flag.unwrap();
            if (auto flag = json["sync_audio"].asBool()) sync_audio = flag.unwrap();
            if (auto number = json["sync_audio_threshold"].asInt())
                sync_audio_threshold = std::clamp((int)number.unwrap(), 20, 2000);
            if (auto flag = json["audio_speedhack"].asBool()) audio_speedhack = flag.unwrap();
            if (auto flag = json["practice_music_hack"].asBool())
                practice_music_hack = flag.unwrap();
            if (auto flag = json["autoclick_p1"].asBool()) autoclick_p1 = flag.unwrap();
            if (auto flag = json["autoclick_p2"].asBool()) autoclick_p2 = flag.unwrap();
            if (auto flag = json["autoclick_auto_disable"].asBool())
                autoclick_auto_disable = flag.unwrap();
            if (auto flag = json["sequence_enabled"].asBool()) sequence_enabled = flag.unwrap();
            if (auto flag = json["sequence_random"].asBool()) sequence_random = flag.unwrap();
            if (auto number = json["max_cps"].asDouble()) max_cps = (float)number.unwrap();
            if (auto number = json["hitbox_thickness"].asDouble())
                hitbox_thickness = (float)number.unwrap();
            if (!json.contains("hitbox_thickness_thin") && hitbox_thickness == 1.f)
                hitbox_thickness = 0.3f;
            if (auto number = json["trail_length"].asInt())
                trail_length = std::max(0, (int)number.unwrap());
            if (auto flag = json["sort_automatically"].asBool()) sort_automatically = flag.unwrap();
            if (auto flag = json["keep_checkpoint_hold"].asBool())
                keep_checkpoint_hold = flag.unwrap();
            if (auto flag = json["immediate_restart"].asBool()) immediate_restart = flag.unwrap();
            if (auto flag = json["layout_regular_bg"].asBool())
                layout_use_regular_bg = flag.unwrap();
            if (auto number = json["render_width"].asInt())
                render_width = std::max(2, (int)number.unwrap());
            if (json["render_presets"].isArray()) {
                render_presets.clear();
                for (const auto& entry : json["render_presets"]) {
                    RenderPreset preset;
                    preset.name = entry["name"].asString().unwrapOr("Preset");
                    preset.width = std::max(2, (int)entry["width"].asInt().unwrapOr(1920));
                    preset.height = std::max(2, (int)entry["height"].asInt().unwrapOr(1080));
                    preset.fps = std::max(1, (int)entry["fps"].asInt().unwrapOr(60));
                    preset.bitrate = std::max(0, (int)entry["bitrate"].asInt().unwrapOr(20000000));
                    render_presets.push_back(std::move(preset));
                }
            }
            if (auto number = json["render_height"].asInt())
                render_height = std::max(2, (int)number.unwrap());
            if (auto number = json["render_fps"].asInt())
                render_fps = std::max(1, (int)number.unwrap());
            if (auto number = json["render_bitrate"].asInt()) {
                int bits = (int)number.unwrap();
                render_bitrate = bits <= 0 ? 0 : std::max(bits, 1000);
            }
            if (auto number = json["render_fade_in"].asDouble())
                render_fade_in = (float)number.unwrap();
            if (auto number = json["render_fade_out"].asDouble())
                render_fade_out = (float)number.unwrap();
            if (auto number = json["render_after_end"].asDouble())
                render_after_end = (float)number.unwrap();
            if (auto number = json["render_music"].asDouble())
                render_music = (float)number.unwrap();
            if (auto number = json["render_sfx"].asDouble()) render_sfx = (float)number.unwrap();
            if (auto flag = json["render_record_pause"].asBool())
                render_record_pause = flag.unwrap();
            if (auto flag = json["render_show_level"].asBool()) render_show_lvl = flag.unwrap();
            if (auto flag = json["render_screen_shape"].asBool())
                render_screen_shape = flag.unwrap();
            if (auto flag = json["seed_override"].asBool()) seed_override = flag.unwrap();
            if (json.contains("seed_override_value")) {
                if (auto text = json["seed_override_value"].asString())
                    seed_override_value = std::strtoull(text.unwrap().c_str(), nullptr, 10);
            }
            if (auto flag = json["render_real_time"].asBool()) render_real_time = flag.unwrap();
            if (auto flag = json["render_ssb"].asBool()) render_ssb_fix = flag.unwrap();
            if (auto flag = json["render_speedhack"].asBool()) render_speedhack = flag.unwrap();
            if (auto flag = json["render_color_fix"].asBool()) render_color_fix = flag.unwrap();
            if (auto flag = json["render_hide_labels"].asBool()) render_hide_labels = flag.unwrap();
            if (auto flag = json["render_hide_pause"].asBool()) render_hide_pause = flag.unwrap();
            if (auto flag = json["render_auto_exit"].asBool()) render_auto_exit = flag.unwrap();
            if (auto text = json["render_codec"].asString()) render_codec = text.unwrap();
            if (auto text = json["render_extension"].asString()) render_extension = text.unwrap();
            if (auto text = json["render_preset"].asString()) {
                auto preset = text.unwrap();
                if (preset == "ultrafast" || preset == "veryfast" || preset == "medium")
                    render_preset = preset;
            }
            if (auto text = json["render_args"].asString()) render_args = text.unwrap();
            if (auto text = json["render_name"].asString()) render_name = text.unwrap();
            if (auto flag = json["fill_hitboxes"].asBool()) fill_hitboxes = flag.unwrap();
            if (auto flag = json["hitbox_outline_only"].asBool())
                hitbox_outline_only = flag.unwrap();
            if (json.contains("hitbox_trail")) {
                if (auto flag = json["hitbox_trail"].asBool()) {
                    had_trail = true;
                    trail_file = flag.unwrap();
                    hitbox_trail = trail_file;
                }
            }
            if (auto flag = json["hitbox_trail_visual"].asBool())
                hitbox_trail_visual = flag.unwrap();
            if (auto flag = json["hitbox_player"].asBool()) hitbox_player = flag.unwrap();
            if (auto flag = json["hitbox_click"].asBool()) hitbox_click = flag.unwrap();
            if (auto flag = json["hitbox_rotated"].asBool()) hitbox_rotated = flag.unwrap();
            if (auto flag = json["hitbox_inner"].asBool()) hitbox_inner = flag.unwrap();
            if (auto flag = json["hitbox_circle"].asBool()) hitbox_circle = flag.unwrap();
            if (auto flag = json["hitbox_solid"].asBool()) hitbox_solid = flag.unwrap();
            if (auto flag = json["hitbox_passable"].asBool()) hitbox_passable = flag.unwrap();
            if (auto flag = json["macrobuff_tint"].asBool()) macrobuff_tint = flag.unwrap();
            if (auto flag = json["hitbox_slope"].asBool()) hitbox_slope = flag.unwrap();
            if (auto flag = json["hitbox_hazard"].asBool()) hitbox_hazard = flag.unwrap();
            if (auto flag = json["hitbox_interact"].asBool()) hitbox_interact = flag.unwrap();
            if (auto flag = json["hide_activated_objects"].asBool())
                hide_activated_objects = flag.unwrap();
            if (auto number = json["hitbox_fill_opacity"].asDouble())
                hitbox_fill_opacity = (float)number.unwrap();
            if (auto number = json["hitbox_trail_opacity"].asDouble())
                hitbox_trail_opacity = std::clamp((float)number.unwrap(), 0.f, 1.f);
            if (auto flag = json["show_trajectory"].asBool()) show_traj = flag.unwrap();
            if (auto flag = json["trajectory_hold"].asBool()) traj_hold = flag.unwrap();
            if (auto flag = json["trajectory_release"].asBool()) traj_release = flag.unwrap();
            if (auto flag = json["trajectory_swift"].asBool()) traj_swift = flag.unwrap();
            bool had_seconds = false;
            if (auto number = json["trajectory_seconds"].asDouble()) {
                traj_seconds = std::clamp((float)number.unwrap(), 0.05f, 4.f);
                had_seconds = true;
            }
            else if (auto frames = json["trajectory_length"].asInt()) {
                traj_seconds = std::clamp((float)frames.unwrap() / 240.f, 0.05f, 4.f);
            }
            if (auto number = json["trajectory_width"].asDouble())
                traj_width = (float)number.unwrap();
            if (!had_seconds && traj_width >= 1.4f) traj_width = 0.5f;
#ifndef GEODE_IS_MOBILE
            if (auto flag = json["docked_menu"].asBool()) docked_menu = flag.unwrap();
#endif
            if (auto number = json["floating_x"].asDouble()) floating_x = (float)number.unwrap();
            if (auto number = json["floating_y"].asDouble()) floating_y = (float)number.unwrap();
            if (auto number = json["frame_bar_x"].asDouble()) frame_bar_x = (float)number.unwrap();
            if (auto number = json["frame_bar_y"].asDouble()) frame_bar_y = (float)number.unwrap();
            if (auto flag = json["floating_hide_in_level"].asBool())
                floating_hide_in_lvl = flag.unwrap();
            if (auto number = json["floating_size"].asDouble())
                floating_size = std::clamp((float)number.unwrap(), 24.f, 72.f);
            if (auto number = json["menu_scale"].asDouble())
                menu_scale = std::clamp((float)number.unwrap(), 0.35f, 3.f);
#ifdef GEODE_IS_MOBILE
            if (!json.contains("menu_scale_phone")) menu_scale = std::min(menu_scale, .96f);
#endif
            if (auto number = json["menu_inner_scale"].asDouble())
                menu_inner_scale = std::clamp((float)number.unwrap(), 1.f, 1.5f);
            if (auto number = json["menu_scale_undocked"].asDouble())
                menu_scale_undocked = std::clamp((float)number.unwrap(), 0.35f, 3.f);
            if (auto number = json["menu_anim_speed"].asDouble())
                menu_anim_speed = std::clamp((float)number.unwrap(), 0.25f, 4.f);
            if (auto text = json["theme_name"].asString()) theme_name = text.unwrap();
            if (auto number = json["ui_style"].asInt()) ui_style = (int)number.unwrap();
            if (json["tiles"].isArray()) {
                tile_places.clear();
                for (const auto& entry : json["tiles"]) {
                    TilePlace place;
                    place.key = entry["key"].asString().unwrapOr("");
                    place.pinned = entry["pinned"].asBool().unwrapOr(false);
                    place.hidden = entry["hidden"].asBool().unwrapOr(false);
                    place.x = (float)entry["x"].asDouble().unwrapOr(0.0);
                    place.y = (float)entry["y"].asDouble().unwrapOr(0.0);
                    if (!place.key.empty()) tile_places.push_back(std::move(place));
                }
            }
            if (json["open_pages"].isArray()) {
                open_pages.clear();
                for (const auto& entry : json["open_pages"]) {
                    if (auto name = entry.asString()) open_pages.push_back(name.unwrap());
                }
            }
            if (json["tab_order"].isArray() && json["tab_order"].size() > 0) {
                std::vector<std::string> order;
                for (const auto& entry : json["tab_order"]) {
                    if (auto name = entry.asString()) order.push_back(name.unwrap());
                }
                if (!order.empty()) tab_order = std::move(order);
            }
            auto read_color = [&](const char* key, float* color, int count) {
                if (!json[key].isArray() || (int)json[key].size() < count) return;
                for (int i = 0; i < count; ++i) {
                    if (auto channel = json[key][i].asDouble()) color[i] = (float)channel.unwrap();
                }
            };
            read_color("hitbox_outer_color", hitbox_outer_color, 4);
            read_color("hitbox_rotated_color", hitbox_rotated_color, 4);
            read_color("hitbox_inner_color", hitbox_inner_color, 4);
            read_color("hitbox_circle_color", hitbox_circle_color, 4);
            read_color("hitbox_solid_color", hitbox_solid_color, 4);
            read_color("hitbox_passable_color", hitbox_passable_color, 4);
            read_color("hitbox_slope_color", hitbox_slope_color, 4);
            read_color("hitbox_hazard_color", hitbox_hazard_color, 4);
            read_color("hitbox_trail_color", hitbox_trail_color, 4);
            read_color("hitbox_interact_color", hitbox_interact_color, 4);
            read_color("hitbox_click_color", hitbox_click_color, 4);
            read_color("hitbox_release_color", hitbox_release_color, 4);
            if (json.contains("trajectory_color")) read_color("trajectory_color", traj_color, 4);
            if (json.contains("trajectory_swift_color"))
                read_color("trajectory_swift_color", traj_swift_color, 4);
            if (json.contains("trajectory_release_color"))
                read_color("trajectory_release_color", traj_release_color, 4);
            read_color("editor_p1", editor_p1, 4);
            read_color("editor_p2", editor_p2, 4);
            read_color("editor_sel_p1", editor_sel_p1, 4);
            read_color("editor_sel_p2", editor_sel_p2, 4);
            if (auto text = json["rename_format"].asString()) rename_format = text.unwrap();
            if (auto text = json["clickpack"].asString()) clickpack = utf8_path_text(text.unwrap());
            if (json.contains("clickpack_p2")) {
                if (auto text = json["clickpack_p2"].asString())
                    clickpack_p2 = utf8_path_text(text.unwrap());
            }
            else {
                clickpack_p2 = clickpack;
            }
            if (auto flag = json["clickbot"].asBool()) clickbot = flag.unwrap();
            if (auto number = json["frame_advance_hold_duration"].asInt())
                advance_hold_ms = (unsigned)number.unwrap();
            if (auto number = json["frame_advance_delay"].asInt())
                advance_delay_ms = (unsigned)number.unwrap();
            if (auto number = json["frame_advance_speed"].asDouble())
                advance_speed = std::clamp((float)number.unwrap(), 1.f, 200.f);
            if (auto flag = json["pause_before_kill"].asBool()) pause_before_kill = flag.unwrap();
            if (auto flag = json["trajectory_autoclicker"].asBool())
                traj_autoclicker = flag.unwrap();
            if (auto number = json["trajectory_click_lookahead"].asInt())
                traj_click_lookahead = std::max((int)number.unwrap(), 1);
            if (auto flag = json["trajectory_moving_objects"].asBool())
                traj_moving_objects = flag.unwrap();
            if (auto flag = json["near_movers"].asBool()) near_movers = flag.unwrap();
            if (auto flag = json["fixed_cadence"].asBool()) fixed_cadence = flag.unwrap();
            if (auto flag = json["backstep"].asBool()) backstep = flag.unwrap();
            if (auto flag = json["velocity_unrounding"].asBool())
                velocity_unrounding = flag.unwrap();
            if (auto percent = json["backstep_percent"].asInt())
                backstep_percent = std::clamp((int)percent.unwrap(), 1, 200);
            if (auto range = json["best_frame_range"].asInt())
                best_frame_range = std::clamp((int)range.unwrap(), 1, 2000);
            if (auto flag = json["best_frame_click"].asBool()) best_frame_click = flag.unwrap();
            if (auto flag = json["best_frame_auto"].asBool()) best_frame_auto = flag.unwrap();
            else if (auto amt = json["backstep_saves"].asInt())
                backstep_percent = std::clamp((int)std::lround(amt.unwrap() * 100.0 / 240.0), 1, 200);
            if (auto number = json["pause_trajectory_ms"].asInt()) {
                pause_traj_ms = (unsigned)std::clamp((int)number.unwrap(), 1, 4000);
            }
            else if (auto number = json["pause_trajectory_seconds"].asDouble()) {
                pause_traj_ms = (unsigned)(std::clamp((int)std::lround(number.unwrap() * 1000.0), 1, 4000));
            }
            if (auto number = json["percent_accuracy"].asInt())
                percent_accuracy = std::clamp((int)number.unwrap(), 0, 6);
            if (auto number = json["clickbot_volume"].asDouble())
                clickbot_volume = (float)number.unwrap();
            if (json.contains("clickbot_volume_p2")) {
                if (auto number = json["clickbot_volume_p2"].asDouble())
                    clickbot_volume_p2 = (float)number.unwrap();
            }
            else {
                clickbot_volume_p2 = clickbot_volume;
            }
            if (auto flag = json["p1_softs"].asBool()) p1_softs = flag.unwrap();
            if (auto flag = json["p2_softs"].asBool()) p2_softs = flag.unwrap();
            if (auto flag = json["p1_hards"].asBool()) p1_hards = flag.unwrap();
            if (auto flag = json["p2_hards"].asBool()) p2_hards = flag.unwrap();
            if (auto flag = json["p1_micros"].asBool()) p1_micros = flag.unwrap();
            if (auto flag = json["p2_micros"].asBool()) p2_micros = flag.unwrap();
            if (auto number = json["p1_softs_time"].asDouble()) p1_soft_ms = (float)number.unwrap();
            if (auto number = json["p2_softs_time"].asDouble()) p2_soft_ms = (float)number.unwrap();
            if (auto number = json["p1_hards_time"].asDouble())
                p1_hard_seconds = (float)number.unwrap();
            if (auto number = json["p2_hards_time"].asDouble())
                p2_hard_seconds = (float)number.unwrap();
            if (auto number = json["p1_micros_time"].asInt()) p1_micro_ms = (int)number.unwrap();
            if (auto number = json["p2_micros_time"].asInt()) p2_micro_ms = (int)number.unwrap();
            if (json["backgroundColor"].isArray() && json["backgroundColor"].size() >= 3) {
                for (int i = 0; i < 3; ++i) {
                    if (auto channel = json["backgroundColor"][i].asDouble())
                        background_color[i] = (float)channel.unwrap();
                }
            }
            if (json["blocksColor"].isArray() && json["blocksColor"].size() >= 3) {
                for (int i = 0; i < 3; ++i) {
                    if (auto channel = json["blocksColor"][i].asDouble())
                        blocks_color[i] = (float)channel.unwrap();
                }
            }
            frame_label = read_placement(json["frame_label"], frame_label);
            cps_label = read_placement(json["cps_label"], cps_label);
            percent_label = read_placement(json["percent_label"], percent_label);
            time_label = read_placement(json["time_label"], time_label);
            recording_label = read_placement(json["recording_label"], recording_label);
            pos_label = read_placement(json["position_label"], pos_label);
            fps_label = read_placement(json["fps_label"], fps_label);
            debug_label = read_placement(json["debug_label"], debug_label);
            speed_label = read_placement(json["speed_label"], speed_label);
            ssb_label = read_placement(json["ssb_label"], ssb_label);
            velocity_label = read_placement(json["velocity_label"], velocity_label);
            alive_label = read_placement(json["alive_label"], alive_label);
            max_cps_label = read_placement(json["max_cps_label"], max_cps_label);
            gravity_label = read_placement(json["gravity_label"], gravity_label);
            utility_label = read_placement(json["utility_label"], utility_label);
            death_label = read_placement(json["death_label"], death_label);
            safe_mode_label = read_placement(json["safe_mode_label"], safe_mode_label);
            if (auto number = json["press_interval"].asInt())
                auto_click.press_interval = (int)number.unwrap();
            if (auto number = json["release_interval"].asInt())
                auto_click.release_interval = (int)number.unwrap();
            if (auto number = json["autoclick_swifts"].asInt())
                auto_click.swifts = std::clamp((int)number.unwrap(), 0, 8);
        }
    }

    fps = Logic::read_float("macro-fps", fps);
    real_time_mode = Logic::read_bool("real-time", real_time_mode);
    lock_delta = Logic::read_bool("lock-delta", lock_delta);
    lock_display_fps = Logic::read_bool("lock-display-fps", lock_display_fps);
    save_debug = Logic::read_bool("save-debug", save_debug);
    ignore_inputs = Logic::read_bool("ignore-inputs", ignore_inputs);
    no_overwrite = Logic::read_bool("no-overwrite", no_overwrite);
    playback_tps_changes = Logic::read_bool("playback-tps-changes", playback_tps_changes);
    trace_logging = Logic::read_bool("trace-logging", trace_logging);
    no_editor_keybinds = Logic::read_bool("no-editor-keybinds", no_editor_keybinds);
    record_p1 = Logic::read_bool("record-p1", record_p1);
    record_p2 = Logic::read_bool("record-p2", record_p2);
    play_p1 = Logic::read_bool("play-p1", play_p1);
    play_p2 = Logic::read_bool("play-p2", play_p2);
    click_both = Logic::read_bool("click-both", click_both);
    maintain_gravity = Logic::read_bool("maintain-gravity", maintain_gravity);
    same_gravity = Logic::read_bool("same-gravity", same_gravity);
    swap_players = Logic::read_bool("swap-players", swap_players);
    inverse_p1 = Logic::read_bool("inverse-p1", inverse_p1);
    inverse_p2 = Logic::read_bool("inverse-p2", inverse_p2);
    noclip_p1 = Logic::read_bool("noclip-p1", noclip_p1);
    noclip_p2 = Logic::read_bool("noclip-p2", noclip_p2);
    show_hitboxes = Logic::read_bool("show-hitboxes", show_hitboxes);
    fill_hitboxes = Logic::read_bool("fill-hitboxes", fill_hitboxes);
    hitbox_trail = Logic::read_bool("hitbox-trail", hitbox_trail);
    hitbox_click = Logic::read_bool("hitbox-click", hitbox_click);
    if (had_trail) hitbox_trail = trail_file;
    layout_mode = Logic::read_bool("layout-mode", layout_mode);
    disable_shakes = Logic::read_bool("disable-shakes", disable_shakes);
    show_frame = Logic::read_bool("show-frame", show_frame);
    show_cps = Logic::read_bool("show-cps", show_cps);
    show_percent = Logic::read_bool("show-percent", show_percent);
    show_time = Logic::read_bool("show-time", show_time);
    show_recording = Logic::read_bool("show-recording", show_recording);
    max_cps = Logic::read_float("max-cps", max_cps);
    autoclicker = Logic::read_bool("autoclicker", autoclicker);
    autoclick_p1 = Logic::read_bool("autoclick-p1", autoclick_p1);
    autoclick_p2 = Logic::read_bool("autoclick-p2", autoclick_p2);
    auto_click.press_interval = Logic::read_int("autoclick-press", auto_click.press_interval);
    auto_click.release_interval = Logic::read_int("autoclick-release", auto_click.release_interval);
    auto_click.swifts = std::clamp(Logic::read_int("autoclick-swifts", auto_click.swifts), 0, 8);
    autoclick_auto_disable = Logic::read_bool("autoclick-auto-disable", autoclick_auto_disable);
    auto_click.disable_after = std::max(Logic::read_int("autoclick-disable-after", auto_click.disable_after), 1);
    auto_click.disable_in = auto_click.disable_after;
    auto pack = read_folder_setting("clickpack");
    if (!pack.empty()) clickpack = pack;
    if (clickpack.empty()) {
        auto example = save_root() / "clickpacks" / "example";
        std::error_code error;
        if (fs::is_directory(example, error))
            clickpack = geode::utils::string::pathToString(example);
    }
    if (clickpack_p2.empty()) clickpack_p2 = clickpack;
    if (fps < 1.f) fps = 240.f;
    macro_fps = fps;
    if (max_cps <= 0.f) max_cps = (float)Logic::MAX_PER_SECOND;
    speedhack = 1.f;
    format = save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
}

void Logic::push_runtime_settings() {
    saving_all_settings = true;
    Logic::write_float("macro-fps", fps);
    Logic::write_float("speedhack", speedhack);
    Logic::write_bool("real-time", real_time_mode);
    Logic::write_bool("lock-delta", lock_delta);
    Logic::write_bool("save-debug", save_debug);
    Logic::write_bool("ignore-inputs", ignore_inputs);
    Logic::write_bool("no-overwrite", no_overwrite);
    Logic::write_bool("playback-tps-changes", playback_tps_changes);
    Logic::write_bool("trace-logging", trace_logging);
    Logic::write_bool("no-editor-keybinds", no_editor_keybinds);
    Logic::write_bool("record-p1", record_p1);
    Logic::write_bool("record-p2", record_p2);
    Logic::write_bool("play-p1", play_p1);
    Logic::write_bool("play-p2", play_p2);
    Logic::write_bool("click-both", click_both);
    Logic::write_bool("maintain-gravity", maintain_gravity);
    Logic::write_bool("same-gravity", same_gravity);
    Logic::write_bool("swap-players", swap_players);
    Logic::write_bool("inverse-p1", inverse_p1);
    Logic::write_bool("inverse-p2", inverse_p2);
    Logic::write_bool("noclip-p1", noclip_p1);
    Logic::write_bool("noclip-p2", noclip_p2);
    Logic::write_bool("show-hitboxes", show_hitboxes);
    Logic::write_bool("fill-hitboxes", fill_hitboxes);
    Logic::write_bool("hitbox-trail", hitbox_trail);
    Logic::write_bool("hitbox-click", hitbox_click);
    Logic::write_bool("layout-mode", layout_mode);
    Logic::write_bool("disable-shakes", disable_shakes);
    Logic::write_bool("show-frame", show_frame);
    Logic::write_bool("show-cps", show_cps);
    Logic::write_bool("show-percent", show_percent);
    Logic::write_bool("show-time", show_time);
    Logic::write_bool("show-recording", show_recording);
    Logic::write_float("max-cps", max_cps);
    Logic::write_bool("autoclicker", autoclicker);
    Logic::write_bool("autoclick-p1", autoclick_p1);
    Logic::write_bool("autoclick-p2", autoclick_p2);
    Logic::write_int("autoclick-press", auto_click.press_interval);
    Logic::write_int("autoclick-release", auto_click.release_interval);
    Logic::write_int("autoclick-swifts", auto_click.swifts);
    Logic::write_bool("autoclick-auto-disable", autoclick_auto_disable);
    Logic::write_int("autoclick-disable-after", auto_click.disable_after);
    Mod::get()->setSettingValue<fs::path>("clickpack", ReplayIO::utf8_path(clickpack));
    saving_all_settings = false;
    save_settings_file();
}

void Logic::save_settings_file() {
    matjson::Value json;
    json["macro_fps"] = fps;
    json["speedhack"] = speedhack;
    json["speedhack_memory"] = saved_speedhack;
    json["real_time_mode"] = real_time_mode;
    json["true_real_time"] = true_real_time;
    json["lowest_playback_fps"] = lowest_playback_fps;
    json["prefetch_helper"] = prefetch_helper;
    json["batched_effects"] = batched_effects;
    json["lock_delta"] = lock_delta;
    json["fix_tps_rotation"] = fix_tps_rotation;
    json["save_debug"] = save_debug;
    json["lock_display_fps"] = lock_display_fps;
    json["playback_fps"] = playback_fps;
    json["ignore_inputs"] = ignore_inputs;
    json["no_overwrite"] = no_overwrite;
    json["playback_tps_changes"] = playback_tps_changes;
    json["trace_logging"] = trace_logging;
    json["no_editor_keybinds"] = no_editor_keybinds;
    json["opt_effects"] = optimize_effects;
    json["record_p1"] = record_p1;
    json["record_p2"] = record_p2;
    json["play_p1"] = play_p1;
    json["play_p2"] = play_p2;
    json["click_both"] = click_both;
    json["follow_mode"] = follow_mode;
    json["follow_delay"] = follow_delay;
    json["follow_delay_ms"] = follow_delay_ms;
    json["maintain_gravity"] = maintain_gravity;
    json["same_gravity"] = same_gravity;
    json["hide_p1"] = hide_p1;
    json["hide_p2"] = hide_p2;
    json["hide_hitbox_p1"] = hide_hitbox_p1;
    json["hide_hitbox_p2"] = hide_hitbox_p2;
    json["hitboxes_follow_hide_player"] = hitboxes_follow_hide_player;
    json["hide_trajectory_p1"] = hide_traj_p1;
    json["hide_trajectory_p2"] = hide_traj_p2;
    json["trajectory_follow_hide_player"] = traj_follow_hide_player;
    json["disable_death_effect"] = disable_death_effect;
    json["safe_mode"] = safe_mode;
    json["swap_players"] = swap_players;
    json["inverse_p1"] = inverse_p1;
    json["inverse_p2"] = inverse_p2;
    json["noclip_p1"] = noclip_p1;
    json["noclip_memory_p1"] = noclip_memory_p1;
    json["noclip_memory_p2"] = noclip_memory_p2;
    json["noclip_p2"] = noclip_p2;
    json["show_hitboxes"] = show_hitboxes;
    json["layout_mode"] = layout_mode;
    json["layout_default_art"] = layout_default_art;
    json["disable_shakes"] = disable_shakes;
    json["no_mirror_portals"] = no_mirror_portals;
    json["show_frame"] = show_frame;
    json["show_cps"] = show_cps;
    json["show_percent"] = show_percent;
    json["show_time"] = show_time;
    json["show_recording"] = show_recording;
    json["show_position"] = show_pos;
    json["show_fps"] = show_fps;
    json["show_debug"] = show_debug;
    json["show_speed"] = show_speed;
    json["show_ssb"] = show_ssb;
    json["label_decimals"] = label_decimals;
    json["show_velocity"] = show_velocity;
    json["show_alive"] = show_alive;
    json["show_max_cps"] = show_max_cps;
    json["show_gravity"] = show_gravity;
    json["show_utilities"] = show_utilities;
    json["show_safe_mode"] = show_safe_mode;
    json["labels_auto"] = labels_auto;
    json["label_corner"] = label_corner;
    json["label_scale"] = label_scale;
    json["label_font"] = label_font;
    json["label_order"] = matjson::Value::array();
    for (const auto& key : label_order)
        json["label_order"].push(key);
    json["show_death_next"] = show_death_next;
    json["click_dash_orbs"] = click_dash_orbs;
    json["click_black_orbs"] = click_black_orbs;
    json["orb_click_count"] = orb_click_count;
    json["pad_black_orb"] = pad_black_orb;
    json["pad_blue_orb"] = pad_blue_orb;
    json["easy_black_ufo"] = easy_black_ufo;
    json["auto_swift"] = auto_swift;
    json["extra_clicks"] = extra_clicks;
    json["straight_fly_p1"] = straight_fly_p1;
    json["straight_fly_p2"] = straight_fly_p2;
    json["straight_ufo_p1"] = straight_ufo_p1;
    json["straight_ufo_p2"] = straight_ufo_p2;
    json["hide_endscreen"] = hide_endscreen;
    json["hide_new_best"] = hide_new_best;
    json["no_effects"] = no_effects;
    json["straight_fly_threshold_p1"] = straight_fly_threshold_p1;
    json["straight_fly_threshold_p2"] = straight_fly_threshold_p2;
    json["straight_ufo_target_p1"] = straight_ufo_target_p1;
    json["straight_ufo_target_p2"] = straight_ufo_target_p2;
    json["straight_ufo_threshold_p1"] = straight_ufo_threshold_p1;
    json["straight_ufo_threshold_p2"] = straight_ufo_threshold_p2;
    json["extra_click_count"] = extra_click_count;
    json["autoclicker"] = autoclicker;
    json["sync_audio"] = sync_audio;
    json["sync_audio_threshold"] = sync_audio_threshold;
    json["audio_speedhack"] = audio_speedhack;
    json["practice_music_hack"] = practice_music_hack;
    json["autoclick_p1"] = autoclick_p1;
    json["autoclick_p2"] = autoclick_p2;
    json["autoclick_auto_disable"] = autoclick_auto_disable;
    json["sequence_enabled"] = sequence_enabled;
    json["sequence_random"] = sequence_random;
    json["max_cps"] = max_cps;
    json["hitbox_thickness"] = hitbox_thickness;
    json["hitbox_thickness_thin"] = true;
    json["trail_length"] = trail_length;
    json["sort_automatically"] = sort_automatically;
    json["keep_checkpoint_hold"] = keep_checkpoint_hold;
    json["immediate_restart"] = immediate_restart;
    json["layout_regular_bg"] = layout_use_regular_bg;
    json["render_width"] = render_width;
    json["render_presets"] = matjson::Value::array();
    for (const auto& preset : render_presets) {
        matjson::Value entry;
        entry["name"] = preset.name;
        entry["width"] = preset.width;
        entry["height"] = preset.height;
        entry["fps"] = preset.fps;
        entry["bitrate"] = preset.bitrate;
        json["render_presets"].push(entry);
    }
    json["render_height"] = render_height;
    json["render_fps"] = render_fps;
    json["render_bitrate"] = render_bitrate;
    json["render_fade_in"] = render_fade_in;
    json["render_fade_out"] = render_fade_out;
    json["render_after_end"] = render_after_end;
    json["render_music"] = render_music;
    json["render_sfx"] = render_sfx;
    json["render_record_pause"] = render_record_pause;
    json["render_show_level"] = render_show_lvl;
    json["render_screen_shape"] = render_screen_shape;
    json["seed_override"] = seed_override;
    json["seed_override_value"] = std::to_string(seed_override_value);
    json["render_ssb"] = render_ssb_fix;
    json["render_real_time"] = render_real_time;
    json["render_speedhack"] = render_speedhack;
    json["render_color_fix"] = render_color_fix;
    json["render_hide_labels"] = render_hide_labels;
    json["render_hide_pause"] = render_hide_pause;
    json["render_auto_exit"] = render_auto_exit;
    json["render_codec"] = render_codec;
    json["render_preset"] = render_preset;
    json["render_extension"] = render_extension;
    json["render_args"] = render_args;
    json["render_name"] = render_name;
    json["fill_hitboxes"] = fill_hitboxes;
    json["hitbox_outline_only"] = hitbox_outline_only;
    json["hitbox_trail"] = hitbox_trail;
    json["hitbox_trail_visual"] = hitbox_trail_visual;
    json["hitbox_player"] = hitbox_player;
    json["hitbox_click"] = hitbox_click;
    json["hitbox_rotated"] = hitbox_rotated;
    json["hitbox_inner"] = hitbox_inner;
    json["hitbox_circle"] = hitbox_circle;
    json["hitbox_solid"] = hitbox_solid;
    json["hitbox_passable"] = hitbox_passable;
    json["macrobuff_tint"] = macrobuff_tint;
    json["hitbox_slope"] = hitbox_slope;
    json["hitbox_hazard"] = hitbox_hazard;
    json["hitbox_interact"] = hitbox_interact;
    json["hide_activated_objects"] = hide_activated_objects;
    json["hitbox_fill_opacity"] = hitbox_fill_opacity;
    json["hitbox_trail_opacity"] = hitbox_trail_opacity;
    json["show_trajectory"] = show_traj;
    json["trajectory_hold"] = traj_hold;
    json["trajectory_release"] = traj_release;
    json["trajectory_swift"] = traj_swift;
    json["trajectory_seconds"] = traj_seconds;
    json["trajectory_width"] = traj_width;
    json["docked_menu"] = docked_menu;
    json["floating_x"] = floating_x;
    json["floating_y"] = floating_y;
    json["frame_bar_x"] = frame_bar_x;
    json["frame_bar_y"] = frame_bar_y;
    json["floating_hide_in_level"] = floating_hide_in_lvl;
    json["floating_size"] = floating_size;
    json["menu_scale"] = menu_scale;
    json["menu_scale_phone"] = true;
    json["menu_inner_scale"] = menu_inner_scale;
    json["menu_scale_undocked"] = menu_scale_undocked;
    json["menu_anim_speed"] = menu_anim_speed;
    json["theme_name"] = theme_name;
    json["ui_style"] = ui_style;
    json["tiles"] = matjson::Value::array();
    for (const auto& place : tile_places) {
        matjson::Value entry;
        entry["key"] = place.key;
        entry["pinned"] = place.pinned;
        entry["hidden"] = place.hidden;
        entry["x"] = place.x;
        entry["y"] = place.y;
        json["tiles"].push(entry);
    }
    json["open_pages"] = matjson::Value::array();
    for (const auto& name : open_pages)
        json["open_pages"].push(name);
    json["tab_order"] = matjson::Value::array();
    for (const auto& name : tab_order)
        json["tab_order"].push(name);
    auto write_color = [&](const char* key, const float* color, int count) {
        json[key] = matjson::Value::array();
        for (int i = 0; i < count; ++i)
            json[key].push(color[i]);
    };
    write_color("hitbox_outer_color", hitbox_outer_color, 4);
    write_color("hitbox_rotated_color", hitbox_rotated_color, 4);
    write_color("hitbox_inner_color", hitbox_inner_color, 4);
    write_color("hitbox_circle_color", hitbox_circle_color, 4);
    write_color("hitbox_solid_color", hitbox_solid_color, 4);
    write_color("hitbox_passable_color", hitbox_passable_color, 4);
    write_color("hitbox_slope_color", hitbox_slope_color, 4);
    write_color("hitbox_hazard_color", hitbox_hazard_color, 4);
    write_color("hitbox_trail_color", hitbox_trail_color, 4);
    write_color("hitbox_interact_color", hitbox_interact_color, 4);
    write_color("hitbox_click_color", hitbox_click_color, 4);
    write_color("hitbox_release_color", hitbox_release_color, 4);
    write_color("trajectory_color", traj_color, 4);
    write_color("trajectory_swift_color", traj_swift_color, 4);
    write_color("trajectory_release_color", traj_release_color, 4);
    write_color("editor_p1", editor_p1, 4);
    write_color("editor_p2", editor_p2, 4);
    write_color("editor_sel_p1", editor_sel_p1, 4);
    write_color("editor_sel_p2", editor_sel_p2, 4);
    json["rename_format"] = rename_format;
    json["clickpack"] = clickpack;
    json["clickpack_p2"] = clickpack_p2;
    json["clickbot"] = clickbot;
    json["frame_advance_hold_duration"] = advance_hold_ms;
    json["frame_advance_delay"] = advance_delay_ms;
    json["frame_advance_speed"] = advance_speed;
    json["pause_before_kill"] = pause_before_kill;
    json["trajectory_autoclicker"] = traj_autoclicker;
    json["trajectory_click_lookahead"] = traj_click_lookahead;
    json["trajectory_moving_objects"] = traj_moving_objects;
    json["near_movers"] = near_movers;
    json["fixed_cadence"] = fixed_cadence;
    json["backstep"] = backstep;
    json["velocity_unrounding"] = velocity_unrounding;
    json["backstep_percent"] = backstep_percent;
    json["best_frame_range"] = best_frame_range;
    json["best_frame_click"] = best_frame_click;
    json["best_frame_auto"] = best_frame_auto;
    json["pause_trajectory_ms"] = pause_traj_ms;
    json["percent_accuracy"] = percent_accuracy;
    json["clickbot_volume"] = clickbot_volume;
    json["clickbot_volume_p2"] = clickbot_volume_p2;
    json["p1_softs"] = p1_softs;
    json["p2_softs"] = p2_softs;
    json["p1_hards"] = p1_hards;
    json["p2_hards"] = p2_hards;
    json["p1_micros"] = p1_micros;
    json["p2_micros"] = p2_micros;
    json["p1_softs_time"] = p1_soft_ms;
    json["p2_softs_time"] = p2_soft_ms;
    json["p1_hards_time"] = p1_hard_seconds;
    json["p2_hards_time"] = p2_hard_seconds;
    json["p1_micros_time"] = p1_micro_ms;
    json["p2_micros_time"] = p2_micro_ms;
    json["press_interval"] = auto_click.press_interval;
    json["release_interval"] = auto_click.release_interval;
    json["autoclick_swifts"] = auto_click.swifts;
    json["backgroundColor"] = matjson::Value::array();
    json["blocksColor"] = matjson::Value::array();
    for (int i = 0; i < 3; ++i) {
        json["backgroundColor"].push(background_color[i]);
        json["blocksColor"].push(blocks_color[i]);
    }
    json["frame_label"] = placement_json(frame_label);
    json["cps_label"] = placement_json(cps_label);
    json["percent_label"] = placement_json(percent_label);
    json["time_label"] = placement_json(time_label);
    json["recording_label"] = placement_json(recording_label);
    json["position_label"] = placement_json(pos_label);
    json["fps_label"] = placement_json(fps_label);
    json["debug_label"] = placement_json(debug_label);
    json["speed_label"] = placement_json(speed_label);
    json["ssb_label"] = placement_json(ssb_label);
    json["velocity_label"] = placement_json(velocity_label);
    json["alive_label"] = placement_json(alive_label);
    json["max_cps_label"] = placement_json(max_cps_label);
    json["gravity_label"] = placement_json(gravity_label);
    json["utility_label"] = placement_json(utility_label);
    json["death_label"] = placement_json(death_label);
    json["safe_mode_label"] = placement_json(safe_mode_label);

    // geode keeps keybinds in the same file & i used to wipe them :/
    auto* mod = Mod::get();
    for (const auto& key : mod->getSettingKeys()) {
        auto setting = mod->getSetting(key);
        if (!setting) continue;
        auto* binds = typeinfo_cast<KeybindSettingV3*>(setting.get());
        if (!binds) continue;
        matjson::Value list = matjson::Value::array();
        for (const auto& bind : binds->getValue()) {
            list.push(matjson::makeObject({
                { "key", (int)bind.key },
                { "modifiers", (int)bind.modifiers },
            }));
        }
        json[key] = std::move(list);
    }
    if (auto* manager = ModSettingsManager::from(mod)) manager->getSaveData() = json;

    {
        std::ofstream file(save_root() / "settings.json");
        if (file) file << json.dump(4);
    }
    if (!saving_all_settings) (void)Mod::get()->saveData();
}
