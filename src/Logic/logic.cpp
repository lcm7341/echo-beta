#include "Logic/logic.hpp"
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

namespace {
    bool bulk_settings = false;
}

namespace {
    thread_local Logic* g_loading = nullptr;
}

void Logic::set_loading(Logic* session) {
    g_loading = session;
}

Logic& Logic::live() {
    static Logic value;
    return value;
}

Logic& Logic::get() {
    if (g_loading)
        return *g_loading;
    return Logic::live();
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

std::string Logic::read_folder_setting(char const* key) {
    auto value = Mod::get()->getSettingValue<fs::path>(key);
    return value.string();
}

matjson::Value Logic::placement_json(LabelPlacement const& placement) {
    matjson::Value json;
    json["x"] = placement.x;
    json["y"] = placement.y;
    json["scale"] = placement.scale;
    json["opacity"] = placement.opacity;
    return json;
}

LabelPlacement Logic::read_placement(matjson::Value const& json, LabelPlacement fallback) {
    if (!json.isObject())
        return fallback;
    if (auto number = json["x"].asDouble())
        fallback.x = static_cast<float>(number.unwrap());
    if (auto number = json["y"].asDouble())
        fallback.y = static_cast<float>(number.unwrap());
    if (auto number = json["scale"].asDouble())
        fallback.scale = static_cast<float>(number.unwrap());
    if (auto number = json["opacity"].asInt())
        fallback.opacity = static_cast<int>(number.unwrap());
    return fallback;
}

void Logic::boot() {
    auto root = Logic::get().save_root();
    std::error_code error;
    for (auto const* folder : { "replays", "converted", "clickpacks", "renders", "logs" }) {
        fs::create_directories(root / folder, error);
    }
    auto packs = root / "clickpacks";
    bool packs_empty = true;
    if (fs::is_directory(packs)) {
        for (auto const& entry : fs::directory_iterator(packs, error)) {
            (void)entry;
            packs_empty = false;
            break;
        }
    }
    if (packs_empty) {
        for (auto const* folder : {
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
    auto migrate_key = [](char const* id, enumKeyCodes from, KeyboardModifier from_mods, enumKeyCodes to, KeyboardModifier to_mods) {
        auto cur = Mod::get()->getSettingValue<std::vector<Keybind>>(id);
        if (cur.size() != 1 || cur[0].key != from || cur[0].modifiers != from_mods)
            return;
        Mod::get()->setSettingValue<std::vector<Keybind>>(id, { Keybind(to, to_mods) });
    };
    auto const left_alt = KeyboardModifier::Alt;
    migrate_key("open-menu", KEY_Alt, KeyboardModifier::None, KEY_LeftMenu, left_alt);
    migrate_key("open-menu", KEY_LeftMenu, KeyboardModifier::None, KEY_LeftMenu, left_alt);
    migrate_key("toggle-advance", KEY_F6, KeyboardModifier::None, KEY_V, KeyboardModifier::None);
    migrate_key("step-frame", KEY_F7, KeyboardModifier::None, KEY_C, KeyboardModifier::None);
#endif
    if (std::fabs(Logic::read_float("macro-fps", 240.f) - 60.f) < 0.01f)
        Logic::write_float("macro-fps", 240.f);
    Logic::get().pull_settings();
    SettingChangedEventV3().listen([](std::string_view modID, std::string_view, std::shared_ptr<SettingV3>) {
                               if (bulk_settings || modID != Mod::get()->getID())
                                   return ListenerResult::Propagate;
                               Logic::get().save_settings_file();
                               return ListenerResult::Propagate;
                           })
        .leak();
}

void Logic::pull_settings() {
    auto& logic = Logic::get();
    bool had_trail = false, trail_file = false;
    auto path = Logic::get().save_root() / "settings.json";
    if (fs::exists(path)) {
        std::ifstream file(path);
        auto parsed = matjson::Value::parse(file);
        if (parsed) {
            auto const& json = parsed.unwrap();
            if (auto number = json["macro_fps"].asDouble())
                logic.fps = static_cast<float>(number.unwrap());
            if (auto number = json["speedhack"].asDouble())
                logic.speedhack = static_cast<float>(number.unwrap());
            if (auto number = json["speedhack_memory"].asDouble())
                logic.speedhack_memory = static_cast<float>(number.unwrap());
            else if (std::abs(logic.speedhack - 1.f) > 0.05f)
                logic.speedhack_memory = logic.speedhack;
            if (auto flag = json["real_time_mode"].asBool())
                logic.real_time_mode = flag.unwrap();
            if (auto flag = json["lock_delta"].asBool())
                logic.lock_delta = flag.unwrap();
            if (auto flag = json["fix_tps_rotation"].asBool())
                logic.fix_tps_rotation = flag.unwrap();
            if (auto flag = json["save_debug"].asBool())
                logic.save_debug = flag.unwrap();
            if (auto flag = json["lock_display_fps"].asBool())
                logic.lock_display_fps = flag.unwrap();
            if (auto number = json["playback_fps"].asDouble())
                logic.playback_fps = std::clamp(static_cast<float>(number.unwrap()), 0.f, 100000.f);
            else if (auto number = json["playback_fps"].asInt())
                logic.playback_fps = std::clamp(static_cast<float>(number.unwrap()), 0.f, 100000.f);
            if (auto flag = json["ignore_inputs"].asBool())
                logic.ignore_inputs = flag.unwrap();
            if (auto flag = json["no_overwrite"].asBool())
                logic.no_overwrite = flag.unwrap();
            if (auto flag = json["playback_tps_changes"].asBool())
                logic.playback_tps_changes = flag.unwrap();
            if (auto flag = json["trace_logging"].asBool())
                logic.trace_logging = flag.unwrap();
            if (auto flag = json["opt_getters"].asBool())
                echo::opt::getters = flag.unwrap();
            if (auto flag = json["opt_effects"].asBool())
                echo::opt::effects = flag.unwrap();
            if (auto flag = json["record_p1"].asBool())
                logic.record_p1 = flag.unwrap();
            if (auto flag = json["record_p2"].asBool())
                logic.record_p2 = flag.unwrap();
            if (auto flag = json["play_p1"].asBool())
                logic.play_p1 = flag.unwrap();
            if (auto flag = json["play_p2"].asBool())
                logic.play_p2 = flag.unwrap();
            if (auto flag = json["click_both"].asBool())
                logic.click_both = flag.unwrap();
            if (auto number = json["follow_mode"].asInt())
                logic.follow_mode = std::clamp(static_cast<int>(number.unwrap()), 0, 2);
            if (auto number = json["follow_delay"].asDouble())
                logic.follow_delay = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto flag = json["follow_delay_ms"].asBool())
                logic.follow_delay_ms = flag.unwrap();
            if (auto flag = json["maintain_gravity"].asBool())
                logic.maintain_gravity = flag.unwrap();
            if (auto flag = json["same_gravity"].asBool())
                logic.same_gravity = flag.unwrap();
            if (auto flag = json["hide_p1"].asBool())
                logic.hide_p1 = flag.unwrap();
            if (auto flag = json["hide_p2"].asBool())
                logic.hide_p2 = flag.unwrap();
            if (auto flag = json["hide_hitbox_p1"].asBool())
                logic.hide_hitbox_p1 = flag.unwrap();
            if (auto flag = json["hide_hitbox_p2"].asBool())
                logic.hide_hitbox_p2 = flag.unwrap();
            if (auto flag = json["hitboxes_follow_hide_player"].asBool())
                logic.hitboxes_follow_hide_player = flag.unwrap();
            if (auto flag = json["hide_trajectory_p1"].asBool())
                logic.hide_traj_p1 = flag.unwrap();
            if (auto flag = json["hide_trajectory_p2"].asBool())
                logic.hide_traj_p2 = flag.unwrap();
            if (auto flag = json["trajectory_follow_hide_player"].asBool())
                logic.traj_follow_hide_player = flag.unwrap();
            if (auto flag = json["disable_death_effect"].asBool())
                logic.disable_death_effect = flag.unwrap();
            if (auto flag = json["safe_mode"].asBool())
                logic.safeMode = flag.unwrap();
            if (auto flag = json["swap_players"].asBool())
                logic.swap_players = flag.unwrap();
            if (auto flag = json["inverse_p1"].asBool())
                logic.inverse_p1 = flag.unwrap();
            if (auto flag = json["inverse_p2"].asBool())
                logic.inverse_p2 = flag.unwrap();
            if (auto flag = json["noclip_p1"].asBool())
                logic.noclip_p1 = flag.unwrap();
            if (auto flag = json["noclip_p2"].asBool())
                logic.noclip_p2 = flag.unwrap();
            if (auto flag = json["show_hitboxes"].asBool())
                logic.show_hitboxes = flag.unwrap();
            if (auto flag = json["layout_mode"].asBool())
                logic.layout_mode = flag.unwrap();
            if (auto flag = json["layout_default_art"].asBool())
                logic.layout_default_art = flag.unwrap();
            if (auto flag = json["disable_shakes"].asBool())
                logic.disable_shakes = flag.unwrap();
            if (auto flag = json["no_mirror_portals"].asBool())
                logic.no_mirror_portals = flag.unwrap();
            if (auto flag = json["show_frame"].asBool())
                logic.show_frame = flag.unwrap();
            if (auto flag = json["show_cps"].asBool())
                logic.show_cps = flag.unwrap();
            if (auto flag = json["show_percent"].asBool())
                logic.show_percent = flag.unwrap();
            if (auto flag = json["show_time"].asBool())
                logic.show_time = flag.unwrap();
            if (auto flag = json["show_recording"].asBool())
                logic.show_recording = flag.unwrap();
            if (auto flag = json["show_position"].asBool())
                logic.show_pos = flag.unwrap();
            if (auto flag = json["show_velocity"].asBool())
                logic.show_velocity = flag.unwrap();
            if (auto flag = json["show_alive"].asBool())
                logic.show_alive = flag.unwrap();
            if (auto flag = json["show_max_cps"].asBool())
                logic.show_max_cps = flag.unwrap();
            if (auto flag = json["show_gravity"].asBool())
                logic.show_gravity = flag.unwrap();
            if (auto flag = json["show_utilities"].asBool())
                logic.show_utilities = flag.unwrap();
            if (auto flag = json["show_safe_mode"].asBool())
                logic.show_safe_mode = flag.unwrap();
            if (auto flag = json["labels_auto"].asBool())
                logic.labels_auto = flag.unwrap();
            if (auto number = json["label_corner"].asInt())
                logic.label_corner = std::clamp(static_cast<int>(number.unwrap()), 0, 3);
            if (auto number = json["label_scale"].asDouble())
                logic.labelScale = std::clamp(static_cast<float>(number.unwrap()), .05f, 10.f);
            if (auto text = json["label_font"].asString())
                logic.labelFont = text.unwrap();
            if (json["label_order"].isArray()) {
                logic.label_order.clear();
                for (auto const& key : json["label_order"]) {
                    if (auto text = key.asString())
                        logic.label_order.push_back(text.unwrap());
                }
            }
            if (auto flag = json["show_death_next"].asBool())
                logic.show_death_next = flag.unwrap();
            if (auto flag = json["click_dash_orbs"].asBool())
                logic.click_dash_orbs = flag.unwrap();
            if (auto flag = json["click_black_orbs"].asBool())
                logic.click_black_orbs = flag.unwrap();
            if (auto number = json["orb_click_count"].asInt())
                logic.orb_click_count = std::max(1, static_cast<int>(number.unwrap()));
            if (auto flag = json["pad_black_orb"].asBool())
                logic.pad_black_orb = flag.unwrap();
            if (auto flag = json["pad_blue_orb"].asBool())
                logic.pad_blue_orb = flag.unwrap();
            if (auto flag = json["easy_black_ufo"].asBool())
                logic.easy_black_ufo = flag.unwrap();
            if (auto flag = json["auto_swift"].asBool())
                logic.auto_swift = flag.unwrap();
            if (auto flag = json["extra_clicks"].asBool())
                logic.extra_clicks = flag.unwrap();
            if (auto flag = json["straight_fly_p1"].asBool())
                logic.straight_fly_p1 = flag.unwrap();
            if (auto flag = json["straight_fly_p2"].asBool())
                logic.straight_fly_p2 = flag.unwrap();
            if (auto flag = json["straight_ufo_p1"].asBool())
                logic.straight_ufo_p1 = flag.unwrap();
            if (auto flag = json["straight_ufo_p2"].asBool())
                logic.straight_ufo_p2 = flag.unwrap();
            if (auto flag = json["hide_endscreen"].asBool())
                logic.hide_endscreen = flag.unwrap();
            if (auto flag = json["hide_new_best"].asBool())
                logic.hide_new_best = flag.unwrap();
            if (auto flag = json["no_effects"].asBool())
                logic.no_effects = flag.unwrap();
            if (auto number = json["straight_fly_threshold_p1"].asDouble())
                logic.straight_fly_threshold_p1 = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto number = json["straight_fly_threshold_p2"].asDouble())
                logic.straight_fly_threshold_p2 = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto number = json["straight_ufo_target_p1"].asDouble())
                logic.straight_ufo_target_p1 = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto number = json["straight_ufo_target_p2"].asDouble())
                logic.straight_ufo_target_p2 = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto number = json["straight_ufo_threshold_p1"].asDouble())
                logic.straight_ufo_threshold_p1 = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto number = json["straight_ufo_threshold_p2"].asDouble())
                logic.straight_ufo_threshold_p2 = std::max(static_cast<float>(number.unwrap()), 0.f);
            if (auto number = json["extra_click_count"].asInt())
                logic.extra_click_count = std::clamp(static_cast<int>(number.unwrap()), 1, 16);
            if (auto flag = json["autoclicker"].asBool())
                logic.autoclicker = flag.unwrap();
            if (auto flag = json["sync_audio"].asBool())
                logic.sync_audio = flag.unwrap();
            if (auto number = json["sync_audio_threshold"].asInt())
                logic.sync_audio_threshold = std::clamp(static_cast<int>(number.unwrap()), 20, 2000);
            if (auto flag = json["audio_speedhack"].asBool())
                logic.audio_speedhack = flag.unwrap();
            if (auto flag = json["practice_music_hack"].asBool())
                logic.practice_music_hack = flag.unwrap();
            if (auto flag = json["autoclick_p1"].asBool())
                logic.autoclick_p1 = flag.unwrap();
            if (auto flag = json["autoclick_p2"].asBool())
                logic.autoclick_p2 = flag.unwrap();
            if (auto flag = json["autoclick_auto_disable"].asBool())
                logic.autoclick_auto_disable = flag.unwrap();
            if (auto flag = json["sequence_enabled"].asBool())
                logic.sequence_enabled = flag.unwrap();
            if (auto flag = json["sequence_random"].asBool())
                logic.sequence_random = flag.unwrap();
            if (auto number = json["max_cps"].asDouble())
                logic.max_cps = static_cast<float>(number.unwrap());
            if (auto number = json["hitbox_thickness"].asDouble())
                logic.hitbox_thickness = static_cast<float>(number.unwrap());
            // default should be 0.3 now
            if (!json.contains("hitbox_thickness_thin") && logic.hitbox_thickness == 1.f)
                logic.hitbox_thickness = 0.3f;
            if (auto number = json["trail_length"].asInt())
                logic.trail_length = std::max(0, static_cast<int>(number.unwrap()));
            if (auto flag = json["sort_automatically"].asBool())
                logic.sort_automatically = flag.unwrap();
            if (auto flag = json["keep_checkpoint_hold"].asBool())
                logic.keep_checkpoint_hold = flag.unwrap();
            if (auto flag = json["immediate_restart"].asBool())
                logic.immediate_restart = flag.unwrap();
            if (auto flag = json["layout_regular_bg"].asBool())
                logic.layout_use_regular_bg = flag.unwrap();
            if (auto number = json["render_width"].asInt())
                logic.render_width = std::max(2, static_cast<int>(number.unwrap()));
            if (json["render_presets"].isArray()) {
                logic.render_presets.clear();
                for (auto const& entry : json["render_presets"]) {
                    RenderPreset preset;
                    preset.name = entry["name"].asString().unwrapOr("Preset");
                    preset.width = std::max(2, static_cast<int>(entry["width"].asInt().unwrapOr(1920)));
                    preset.height = std::max(2, static_cast<int>(entry["height"].asInt().unwrapOr(1080)));
                    preset.fps = std::max(1, static_cast<int>(entry["fps"].asInt().unwrapOr(60)));
                    preset.bitrate = std::max(0, static_cast<int>(entry["bitrate"].asInt().unwrapOr(20000000)));
                    logic.render_presets.push_back(std::move(preset));
                }
            }
            if (auto number = json["render_height"].asInt())
                logic.render_height = std::max(2, static_cast<int>(number.unwrap()));
            if (auto number = json["render_fps"].asInt())
                logic.render_fps = std::max(1, static_cast<int>(number.unwrap()));
            if (auto number = json["render_bitrate"].asInt()) {
                int bits = static_cast<int>(number.unwrap());
                logic.render_bitrate = bits <= 0 ? 0 : std::max(bits, 1000);
            }
            if (auto number = json["render_fade_in"].asDouble())
                logic.render_fade_in = static_cast<float>(number.unwrap());
            if (auto number = json["render_fade_out"].asDouble())
                logic.render_fade_out = static_cast<float>(number.unwrap());
            if (auto number = json["render_after_end"].asDouble())
                logic.render_after_end = static_cast<float>(number.unwrap());
            if (auto number = json["render_music"].asDouble())
                logic.render_music = static_cast<float>(number.unwrap());
            if (auto number = json["render_sfx"].asDouble())
                logic.render_sfx = static_cast<float>(number.unwrap());
            if (auto flag = json["render_record_pause"].asBool())
                logic.render_record_pause = flag.unwrap();
            if (auto flag = json["render_show_level"].asBool())
                logic.render_show_lvl = flag.unwrap();
            if (auto flag = json["render_screen_shape"].asBool())
                logic.render_screen_shape = flag.unwrap();
            if (auto flag = json["seed_override"].asBool())
                logic.seed_override = flag.unwrap();
            if (json.contains("seed_override_value")) {
                if (auto text = json["seed_override_value"].asString())
                    logic.seed_override_value = std::strtoull(text.unwrap().c_str(), nullptr, 10);
            }
            if (auto flag = json["render_ssb"].asBool())
                logic.render_ssb_fix = flag.unwrap();
            if (auto flag = json["render_speedhack"].asBool())
                logic.render_speedhack = flag.unwrap();
            if (auto flag = json["render_color_fix"].asBool())
                logic.render_color_fix = flag.unwrap();
            if (auto flag = json["render_hide_labels"].asBool())
                logic.render_hide_labels = flag.unwrap();
            if (auto flag = json["render_hide_pause"].asBool())
                logic.render_hide_pause = flag.unwrap();
            if (auto flag = json["render_auto_exit"].asBool())
                logic.render_auto_exit = flag.unwrap();
            if (auto text = json["render_codec"].asString())
                logic.render_codec = text.unwrap();
            if (auto text = json["render_extension"].asString())
                logic.render_extension = text.unwrap();
            if (auto text = json["render_preset"].asString()) {
                auto preset = text.unwrap();
                if (preset == "ultrafast" || preset == "veryfast" || preset == "medium")
                    logic.render_preset = preset;
            }
            if (auto text = json["render_args"].asString())
                logic.render_args = text.unwrap();
            if (auto text = json["render_name"].asString())
                logic.render_name = text.unwrap();
            if (auto flag = json["fill_hitboxes"].asBool())
                logic.fill_hitboxes = flag.unwrap();
            if (auto flag = json["hitbox_outline_only"].asBool())
                logic.hitbox_outline_only = flag.unwrap();
            if (json.contains("hitbox_trail")) {
                if (auto flag = json["hitbox_trail"].asBool()) {
                    had_trail = true;
                    trail_file = flag.unwrap();
                    logic.hitbox_trail = trail_file;
                }
            }
            if (auto flag = json["hitbox_trail_visual"].asBool())
                logic.hitbox_trail_visual = flag.unwrap();
            // no toggle for ts so a saved true would be stuck
            logic.hitbox_dedup = false;
            if (auto flag = json["hitbox_player"].asBool())
                logic.hitbox_player = flag.unwrap();
            if (auto flag = json["hitbox_click"].asBool())
                logic.hitbox_click = flag.unwrap();
            if (auto flag = json["hitbox_rotated"].asBool())
                logic.hitbox_rotated = flag.unwrap();
            if (auto flag = json["hitbox_inner"].asBool())
                logic.hitbox_inner = flag.unwrap();
            if (auto flag = json["hitbox_circle"].asBool())
                logic.hitbox_circle = flag.unwrap();
            if (auto flag = json["hitbox_solid"].asBool())
                logic.hitbox_solid = flag.unwrap();
            if (auto flag = json["hitbox_slope"].asBool())
                logic.hitbox_slope = flag.unwrap();
            if (auto flag = json["hitbox_hazard"].asBool())
                logic.hitbox_hazard = flag.unwrap();
            if (auto flag = json["hitbox_interact"].asBool())
                logic.hitbox_interact = flag.unwrap();
            if (auto flag = json["hide_activated_objects"].asBool())
                logic.hide_activated_objects = flag.unwrap();
            if (auto number = json["hitbox_fill_opacity"].asDouble())
                logic.hitbox_fill_opacity = static_cast<float>(number.unwrap());
            if (auto number = json["hitbox_trail_opacity"].asDouble())
                logic.hitbox_trail_opacity = std::clamp(static_cast<float>(number.unwrap()), 0.f, 1.f);
            if (auto flag = json["show_trajectory"].asBool())
                logic.show_traj = flag.unwrap();
            if (auto flag = json["trajectory_hold"].asBool())
                logic.traj_hold = flag.unwrap();
            if (auto flag = json["trajectory_release"].asBool())
                logic.traj_release = flag.unwrap();
            if (auto flag = json["trajectory_swift"].asBool())
                logic.traj_swift = flag.unwrap();
            bool had_seconds = false;
            if (auto number = json["trajectory_seconds"].asDouble()) {
                logic.traj_seconds = std::clamp(static_cast<float>(number.unwrap()), 0.05f, 4.f);
                had_seconds = true;
            }
            else if (auto frames = json["trajectory_length"].asInt()) {
                logic.traj_seconds = std::clamp(static_cast<float>(frames.unwrap()) / 240.f, 0.05f, 4.f);
            }
            if (auto number = json["trajectory_width"].asDouble())
                logic.traj_width = static_cast<float>(number.unwrap());
            if (!had_seconds && logic.traj_width >= 1.4f)
                logic.traj_width = 0.5f;
#ifndef GEODE_IS_MOBILE
            if (auto flag = json["docked_menu"].asBool())
                logic.docked_menu = flag.unwrap();
#endif
            if (auto number = json["floating_x"].asDouble())
                logic.floating_x = static_cast<float>(number.unwrap());
            if (auto number = json["floating_y"].asDouble())
                logic.floating_y = static_cast<float>(number.unwrap());
            if (auto number = json["frame_bar_x"].asDouble())
                logic.frame_bar_x = static_cast<float>(number.unwrap());
            if (auto number = json["frame_bar_y"].asDouble())
                logic.frame_bar_y = static_cast<float>(number.unwrap());
            if (auto flag = json["floating_hide_in_level"].asBool())
                logic.floating_hide_in_lvl = flag.unwrap();
            if (auto number = json["floating_size"].asDouble()) {
                float size = static_cast<float>(number.unwrap());
                if (std::isfinite(size))
                    logic.floating_size = std::clamp(size, 24.f, 72.f);
            }
            if (auto number = json["menu_scale"].asDouble())
                logic.menu_scale = std::clamp(static_cast<float>(number.unwrap()), 0.35f, 3.f);
#ifdef GEODE_IS_MOBILE
            // phones that saved the old 1x default get the new one once
            if (!json.contains("menu_scale_phone"))
                logic.menu_scale = std::min(logic.menu_scale, .96f);
#endif
            if (auto number = json["menu_inner_scale"].asDouble())
                logic.menu_inner_scale = std::clamp(static_cast<float>(number.unwrap()), 1.f, 1.5f);
            if (auto number = json["menu_scale_undocked"].asDouble())
                logic.menu_scale_undocked = std::clamp(static_cast<float>(number.unwrap()), 0.35f, 3.f);
            if (auto number = json["menu_anim_speed"].asDouble())
                logic.menu_anim_speed = std::clamp(static_cast<float>(number.unwrap()), 0.25f, 4.f);
            if (auto text = json["theme_name"].asString())
                logic.theme_name = text.unwrap();
            if (auto number = json["ui_style"].asInt())
                logic.ui_style = static_cast<int>(number.unwrap());
            if (json["tiles"].isArray()) {
                logic.tile_places.clear();
                for (auto const& entry : json["tiles"]) {
                    TilePlace place;
                    place.key = entry["key"].asString().unwrapOr("");
                    place.pinned = entry["pinned"].asBool().unwrapOr(false);
                    place.hidden = entry["hidden"].asBool().unwrapOr(false);
                    place.x = static_cast<float>(entry["x"].asDouble().unwrapOr(0.0));
                    place.y = static_cast<float>(entry["y"].asDouble().unwrapOr(0.0));
                    if (!place.key.empty())
                        logic.tile_places.push_back(std::move(place));
                }
            }
            if (json["open_pages"].isArray()) {
                logic.open_pages.clear();
                for (auto const& entry : json["open_pages"]) {
                    if (auto name = entry.asString())
                        logic.open_pages.push_back(name.unwrap());
                }
            }
            if (json["tab_order"].isArray() && json["tab_order"].size() > 0) {
                std::vector<std::string> order;
                for (auto const& entry : json["tab_order"]) {
                    if (auto name = entry.asString())
                        order.push_back(name.unwrap());
                }
                if (!order.empty())
                    logic.tab_order = std::move(order);
            }
            auto read_color = [&](char const* key, float* color, int count) {
                if (!json[key].isArray() || static_cast<int>(json[key].size()) < count)
                    return;
                for (int i = 0; i < count; ++i) {
                    if (auto channel = json[key][i].asDouble())
                        color[i] = static_cast<float>(channel.unwrap());
                }
            };
            read_color("hitbox_outer_color", logic.hitbox_outer_color, 4);
            read_color("hitbox_rotated_color", logic.hitbox_rotated_color, 4);
            read_color("hitbox_inner_color", logic.hitbox_inner_color, 4);
            read_color("hitbox_circle_color", logic.hitbox_circle_color, 4);
            read_color("hitbox_solid_color", logic.hitbox_solid_color, 4);
            read_color("hitbox_slope_color", logic.hitbox_slope_color, 4);
            read_color("hitbox_hazard_color", logic.hitbox_hazard_color, 4);
            read_color("hitbox_trail_color", logic.hitbox_trail_color, 4);
            read_color("hitbox_interact_color", logic.hitbox_interact_color, 4);
            read_color("hitbox_click_color", logic.hitbox_click_color, 4);
            read_color("hitbox_release_color", logic.hitbox_release_color, 4);
            if (json.contains("trajectory_color"))
                read_color("trajectory_color", logic.traj_color, 4);
            if (json.contains("trajectory_swift_color"))
                read_color("trajectory_swift_color", logic.traj_swift_color, 4);
            if (json.contains("trajectory_release_color"))
                read_color("trajectory_release_color", logic.traj_release_color, 4);
            read_color("editor_p1", logic.editor_p1, 4);
            read_color("editor_p2", logic.editor_p2, 4);
            read_color("editor_sel_p1", logic.editor_sel_p1, 4);
            read_color("editor_sel_p2", logic.editor_sel_p2, 4);
            if (auto text = json["rename_format"].asString())
                logic.rename_format = text.unwrap();
            if (auto text = json["clickpack"].asString())
                logic.clickpack = text.unwrap();
            if (json.contains("clickpack_p2")) {
                if (auto text = json["clickpack_p2"].asString())
                    logic.clickpack_p2 = text.unwrap();
            }
            else {
                logic.clickpack_p2 = logic.clickpack;
            }
            if (auto flag = json["clickbot"].asBool())
                logic.clickbot = flag.unwrap();
            if (auto number = json["frame_advance_hold_duration"].asInt())
                logic.advance_hold_ms = static_cast<unsigned>(number.unwrap());
            if (auto number = json["frame_advance_delay"].asInt())
                logic.advance_delay_ms = static_cast<unsigned>(number.unwrap());
            if (auto number = json["frame_advance_speed"].asDouble())
                logic.advance_speed = std::clamp(static_cast<float>(number.unwrap()), 1.f, 200.f);
            if (auto flag = json["pause_before_kill"].asBool())
                logic.pause_before_kill = flag.unwrap();
            if (auto flag = json["trajectory_autoclicker"].asBool())
                logic.traj_autoclicker = flag.unwrap();
            if (auto flag = json["trajectory_moving_objects"].asBool())
                logic.traj_moving_objects = flag.unwrap();
            if (auto flag = json["backstep"].asBool())
                logic.backstep = flag.unwrap();
            if (auto flag = json["velocity_unrounding"].asBool())
                logic.velocity_unrounding = flag.unwrap();
            if (auto percent = json["backstep_percent"].asInt())
                logic.backstep_percent = std::clamp(static_cast<int>(percent.unwrap()), 1, 200);
            if (auto range = json["best_frame_range"].asInt())
                logic.best_frame_range = std::clamp(static_cast<int>(range.unwrap()), 1, 2000);
            if (auto flag = json["best_frame_click"].asBool())
                logic.best_frame_click = flag.unwrap();
            if (auto flag = json["best_frame_auto"].asBool())
                logic.best_frame_auto = flag.unwrap();
            if (auto flag = json["best_frame_release"].asBool())
                logic.best_frame_release = flag.unwrap();
            else if (auto amt = json["backstep_saves"].asInt())
                logic.backstep_percent = std::clamp(static_cast<int>(std::lround(amt.unwrap() * 100.0 / 240.0)), 1, 200);
            if (auto flag = json["pause_at_trajectory_length"].asBool())
                logic.pause_at_traj_length = flag.unwrap();
            logic.pause_at_traj_length = false;
            if (auto number = json["pause_trajectory_ms"].asInt()) {
                logic.pause_traj_ms = static_cast<unsigned>(std::clamp(static_cast<int>(number.unwrap()), 1, 4000));
            }
            else if (auto number = json["pause_trajectory_seconds"].asDouble()) {
                logic.pause_traj_ms = static_cast<unsigned>(std::clamp(static_cast<int>(std::lround(number.unwrap() * 1000.0)), 1, 4000));
            }
            if (auto number = json["percent_accuracy"].asInt())
                logic.percent_accuracy = std::clamp(static_cast<int>(number.unwrap()), 0, 6);
            if (auto number = json["clickbot_volume"].asDouble())
                logic.clickbot_volume = static_cast<float>(number.unwrap());
            if (json.contains("clickbot_volume_p2")) {
                if (auto number = json["clickbot_volume_p2"].asDouble())
                    logic.clickbot_volume_p2 = static_cast<float>(number.unwrap());
            }
            else {
                logic.clickbot_volume_p2 = logic.clickbot_volume;
            }
            if (auto flag = json["p1_softs"].asBool())
                logic.p1_softs = flag.unwrap();
            if (auto flag = json["p2_softs"].asBool())
                logic.p2_softs = flag.unwrap();
            if (auto flag = json["p1_hards"].asBool())
                logic.p1_hards = flag.unwrap();
            if (auto flag = json["p2_hards"].asBool())
                logic.p2_hards = flag.unwrap();
            if (auto flag = json["p1_micros"].asBool())
                logic.p1_micros = flag.unwrap();
            if (auto flag = json["p2_micros"].asBool())
                logic.p2_micros = flag.unwrap();
            if (auto number = json["p1_softs_time"].asDouble())
                logic.p1_soft_ms = static_cast<float>(number.unwrap());
            if (auto number = json["p2_softs_time"].asDouble())
                logic.p2_soft_ms = static_cast<float>(number.unwrap());
            if (auto number = json["p1_hards_time"].asDouble())
                logic.p1_hard_seconds = static_cast<float>(number.unwrap());
            if (auto number = json["p2_hards_time"].asDouble())
                logic.p2_hard_seconds = static_cast<float>(number.unwrap());
            if (auto number = json["p1_micros_time"].asInt())
                logic.p1_micro_ms = static_cast<int>(number.unwrap());
            if (auto number = json["p2_micros_time"].asInt())
                logic.p2_micro_ms = static_cast<int>(number.unwrap());
            if (json["backgroundColor"].isArray() && json["backgroundColor"].size() >= 3) {
                for (int i = 0; i < 3; ++i) {
                    if (auto channel = json["backgroundColor"][i].asDouble())
                        logic.backgroundColor[i] = static_cast<float>(channel.unwrap());
                }
            }
            if (json["blocksColor"].isArray() && json["blocksColor"].size() >= 3) {
                for (int i = 0; i < 3; ++i) {
                    if (auto channel = json["blocksColor"][i].asDouble())
                        logic.blocks_color[i] = static_cast<float>(channel.unwrap());
                }
            }
            logic.frame_label = Logic::get().read_placement(json["frame_label"], logic.frame_label);
            logic.cps_label = Logic::get().read_placement(json["cps_label"], logic.cps_label);
            logic.percent_label = Logic::get().read_placement(json["percent_label"], logic.percent_label);
            logic.time_label = Logic::get().read_placement(json["time_label"], logic.time_label);
            logic.recording_label = Logic::get().read_placement(json["recording_label"], logic.recording_label);
            logic.pos_label = Logic::get().read_placement(json["position_label"], logic.pos_label);
            logic.velocity_label = Logic::get().read_placement(json["velocity_label"], logic.velocity_label);
            logic.alive_label = Logic::get().read_placement(json["alive_label"], logic.alive_label);
            logic.max_cps_label = Logic::get().read_placement(json["max_cps_label"], logic.max_cps_label);
            logic.gravity_label = Logic::get().read_placement(json["gravity_label"], logic.gravity_label);
            logic.utility_label = Logic::get().read_placement(json["utility_label"], logic.utility_label);
            logic.death_label = Logic::get().read_placement(json["death_label"], logic.death_label);
            logic.safe_mode_label = Logic::get().read_placement(json["safe_mode_label"], logic.safe_mode_label);
            if (auto number = json["press_interval"].asInt())
                logic.auto_click.press_interval = static_cast<int>(number.unwrap());
            if (auto number = json["release_interval"].asInt())
                logic.auto_click.release_interval = static_cast<int>(number.unwrap());
            if (auto number = json["autoclick_swifts"].asInt())
                logic.auto_click.swifts = std::clamp(static_cast<int>(number.unwrap()), 0, 8);
        }
    }

    logic.fps = Logic::read_float("macro-fps", logic.fps);
    logic.speedhack = Logic::read_float("speedhack", logic.speedhack);
    logic.real_time_mode = Logic::read_bool("real-time", logic.real_time_mode);
    logic.lock_delta = Logic::read_bool("lock-delta", logic.lock_delta);
    logic.lock_display_fps = Logic::read_bool("lock-display-fps", logic.lock_display_fps);
    logic.save_debug = Logic::read_bool("save-debug", logic.save_debug);
    logic.ignore_inputs = Logic::read_bool("ignore-inputs", logic.ignore_inputs);
    logic.no_overwrite = Logic::read_bool("no-overwrite", logic.no_overwrite);
    logic.playback_tps_changes = Logic::read_bool("playback-tps-changes", logic.playback_tps_changes);
    logic.trace_logging = Logic::read_bool("trace-logging", logic.trace_logging);
    logic.record_p1 = Logic::read_bool("record-p1", logic.record_p1);
    logic.record_p2 = Logic::read_bool("record-p2", logic.record_p2);
    logic.play_p1 = Logic::read_bool("play-p1", logic.play_p1);
    logic.play_p2 = Logic::read_bool("play-p2", logic.play_p2);
    logic.click_both = Logic::read_bool("click-both", logic.click_both);
    logic.maintain_gravity = Logic::read_bool("maintain-gravity", logic.maintain_gravity);
    logic.same_gravity = Logic::read_bool("same-gravity", logic.same_gravity);
    logic.swap_players = Logic::read_bool("swap-players", logic.swap_players);
    logic.inverse_p1 = Logic::read_bool("inverse-p1", logic.inverse_p1);
    logic.inverse_p2 = Logic::read_bool("inverse-p2", logic.inverse_p2);
    logic.noclip_p1 = Logic::read_bool("noclip-p1", logic.noclip_p1);
    logic.noclip_p2 = Logic::read_bool("noclip-p2", logic.noclip_p2);
    logic.show_hitboxes = Logic::read_bool("show-hitboxes", logic.show_hitboxes);
    logic.fill_hitboxes = Logic::read_bool("fill-hitboxes", logic.fill_hitboxes);
    logic.hitbox_trail = Logic::read_bool("hitbox-trail", logic.hitbox_trail);
    logic.hitbox_click = Logic::read_bool("hitbox-click", logic.hitbox_click);
    if (had_trail)
        logic.hitbox_trail = trail_file;
    logic.layout_mode = Logic::read_bool("layout-mode", logic.layout_mode);
    logic.disable_shakes = Logic::read_bool("disable-shakes", logic.disable_shakes);
    logic.show_frame = Logic::read_bool("show-frame", logic.show_frame);
    logic.show_cps = Logic::read_bool("show-cps", logic.show_cps);
    logic.show_percent = Logic::read_bool("show-percent", logic.show_percent);
    logic.show_time = Logic::read_bool("show-time", logic.show_time);
    logic.show_recording = Logic::read_bool("show-recording", logic.show_recording);
    logic.max_cps = Logic::read_float("max-cps", logic.max_cps);
    logic.autoclicker = Logic::read_bool("autoclicker", logic.autoclicker);
    logic.autoclick_p1 = Logic::read_bool("autoclick-p1", logic.autoclick_p1);
    logic.autoclick_p2 = Logic::read_bool("autoclick-p2", logic.autoclick_p2);
    logic.auto_click.press_interval = Logic::read_int("autoclick-press", logic.auto_click.press_interval);
    logic.auto_click.release_interval = Logic::read_int("autoclick-release", logic.auto_click.release_interval);
    logic.auto_click.swifts = std::clamp(Logic::read_int("autoclick-swifts", logic.auto_click.swifts), 0, 8);
    logic.autoclick_auto_disable = Logic::read_bool("autoclick-auto-disable", logic.autoclick_auto_disable);
    auto pack = Logic::get().read_folder_setting("clickpack");
    if (!pack.empty())
        logic.clickpack = pack;
    if (logic.clickpack.empty()) {
        auto example = Logic::get().save_root() / "clickpacks" / "example";
        std::error_code error;
        if (fs::is_directory(example, error))
            logic.clickpack = example.string();
    }
    if (logic.clickpack_p2.empty())
        logic.clickpack_p2 = logic.clickpack;
    if (logic.fps < 1.f)
        logic.fps = 240.f;
    logic.macro_fps = logic.fps;
    if (logic.speedhack <= 0.f)
        logic.speedhack = 1.f;
    if (logic.max_cps <= 0.f)
        logic.max_cps = static_cast<float>(Logic::MAX_PER_SECOND);
    logic.speedhack = 1.f;
    logic.format = logic.save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
}

void Logic::push_runtime_settings() {
    bulk_settings = true;
    auto const& logic = Logic::get();
    Logic::write_float("macro-fps", logic.fps);
    Logic::write_float("speedhack", logic.speedhack);
    Logic::write_bool("real-time", logic.real_time_mode);
    Logic::write_bool("lock-delta", logic.lock_delta);
    Logic::write_bool("save-debug", logic.save_debug);
    Logic::write_bool("ignore-inputs", logic.ignore_inputs);
    Logic::write_bool("no-overwrite", logic.no_overwrite);
    Logic::write_bool("playback-tps-changes", logic.playback_tps_changes);
    Logic::write_bool("trace-logging", logic.trace_logging);
    Logic::write_bool("record-p1", logic.record_p1);
    Logic::write_bool("record-p2", logic.record_p2);
    Logic::write_bool("play-p1", logic.play_p1);
    Logic::write_bool("play-p2", logic.play_p2);
    Logic::write_bool("click-both", logic.click_both);
    Logic::write_bool("maintain-gravity", logic.maintain_gravity);
    Logic::write_bool("same-gravity", logic.same_gravity);
    Logic::write_bool("swap-players", logic.swap_players);
    Logic::write_bool("inverse-p1", logic.inverse_p1);
    Logic::write_bool("inverse-p2", logic.inverse_p2);
    Logic::write_bool("noclip-p1", logic.noclip_p1);
    Logic::write_bool("noclip-p2", logic.noclip_p2);
    Logic::write_bool("show-hitboxes", logic.show_hitboxes);
    Logic::write_bool("fill-hitboxes", logic.fill_hitboxes);
    Logic::write_bool("hitbox-trail", logic.hitbox_trail);
    Logic::write_bool("hitbox-click", logic.hitbox_click);
    Logic::write_bool("layout-mode", logic.layout_mode);
    Logic::write_bool("disable-shakes", logic.disable_shakes);
    Logic::write_bool("show-frame", logic.show_frame);
    Logic::write_bool("show-cps", logic.show_cps);
    Logic::write_bool("show-percent", logic.show_percent);
    Logic::write_bool("show-time", logic.show_time);
    Logic::write_bool("show-recording", logic.show_recording);
    Logic::write_float("max-cps", logic.max_cps);
    Logic::write_bool("autoclicker", logic.autoclicker);
    Logic::write_bool("autoclick-p1", logic.autoclick_p1);
    Logic::write_bool("autoclick-p2", logic.autoclick_p2);
    Logic::write_int("autoclick-press", logic.auto_click.press_interval);
    Logic::write_int("autoclick-release", logic.auto_click.release_interval);
    Logic::write_int("autoclick-swifts", logic.auto_click.swifts);
    Logic::write_bool("autoclick-auto-disable", logic.autoclick_auto_disable);
    Mod::get()->setSettingValue<fs::path>("clickpack", fs::path(logic.clickpack));
    bulk_settings = false;
    Logic::get().save_settings_file();
}

void Logic::save_settings_file() {
    auto const& logic = Logic::get();
    matjson::Value json;
    json["macro_fps"] = logic.fps;
    json["speedhack"] = logic.speedhack;
    json["speedhack_memory"] = logic.speedhack_memory;
    json["real_time_mode"] = logic.real_time_mode;
    json["lock_delta"] = logic.lock_delta;
    json["fix_tps_rotation"] = logic.fix_tps_rotation;
    json["save_debug"] = logic.save_debug;
    json["lock_display_fps"] = logic.lock_display_fps;
    json["playback_fps"] = logic.playback_fps;
    json["ignore_inputs"] = logic.ignore_inputs;
    json["no_overwrite"] = logic.no_overwrite;
    json["playback_tps_changes"] = logic.playback_tps_changes;
    json["trace_logging"] = logic.trace_logging;
    json["opt_getters"] = echo::opt::getters;
    json["opt_effects"] = echo::opt::effects;
    json["record_p1"] = logic.record_p1;
    json["record_p2"] = logic.record_p2;
    json["play_p1"] = logic.play_p1;
    json["play_p2"] = logic.play_p2;
    json["click_both"] = logic.click_both;
    json["follow_mode"] = logic.follow_mode;
    json["follow_delay"] = logic.follow_delay;
    json["follow_delay_ms"] = logic.follow_delay_ms;
    json["maintain_gravity"] = logic.maintain_gravity;
    json["same_gravity"] = logic.same_gravity;
    json["hide_p1"] = logic.hide_p1;
    json["hide_p2"] = logic.hide_p2;
    json["hide_hitbox_p1"] = logic.hide_hitbox_p1;
    json["hide_hitbox_p2"] = logic.hide_hitbox_p2;
    json["hitboxes_follow_hide_player"] = logic.hitboxes_follow_hide_player;
    json["hide_trajectory_p1"] = logic.hide_traj_p1;
    json["hide_trajectory_p2"] = logic.hide_traj_p2;
    json["trajectory_follow_hide_player"] = logic.traj_follow_hide_player;
    json["disable_death_effect"] = logic.disable_death_effect;
    json["safe_mode"] = logic.safeMode;
    json["swap_players"] = logic.swap_players;
    json["inverse_p1"] = logic.inverse_p1;
    json["inverse_p2"] = logic.inverse_p2;
    json["noclip_p1"] = logic.noclip_p1;
    json["noclip_p2"] = logic.noclip_p2;
    json["show_hitboxes"] = logic.show_hitboxes;
    json["layout_mode"] = logic.layout_mode;
    json["layout_default_art"] = logic.layout_default_art;
    json["disable_shakes"] = logic.disable_shakes;
    json["no_mirror_portals"] = logic.no_mirror_portals;
    json["show_frame"] = logic.show_frame;
    json["show_cps"] = logic.show_cps;
    json["show_percent"] = logic.show_percent;
    json["show_time"] = logic.show_time;
    json["show_recording"] = logic.show_recording;
    json["show_position"] = logic.show_pos;
    json["show_velocity"] = logic.show_velocity;
    json["show_alive"] = logic.show_alive;
    json["show_max_cps"] = logic.show_max_cps;
    json["show_gravity"] = logic.show_gravity;
    json["show_utilities"] = logic.show_utilities;
    json["show_safe_mode"] = logic.show_safe_mode;
    json["labels_auto"] = logic.labels_auto;
    json["label_corner"] = logic.label_corner;
    json["label_scale"] = logic.labelScale;
    json["label_font"] = logic.labelFont;
    json["label_order"] = matjson::Value::array();
    for (auto const& key : logic.label_order)
        json["label_order"].push(key);
    json["show_death_next"] = logic.show_death_next;
    json["click_dash_orbs"] = logic.click_dash_orbs;
    json["click_black_orbs"] = logic.click_black_orbs;
    json["orb_click_count"] = logic.orb_click_count;
    json["pad_black_orb"] = logic.pad_black_orb;
    json["pad_blue_orb"] = logic.pad_blue_orb;
    json["easy_black_ufo"] = logic.easy_black_ufo;
    json["auto_swift"] = logic.auto_swift;
    json["extra_clicks"] = logic.extra_clicks;
    json["straight_fly_p1"] = logic.straight_fly_p1;
    json["straight_fly_p2"] = logic.straight_fly_p2;
    json["straight_ufo_p1"] = logic.straight_ufo_p1;
    json["straight_ufo_p2"] = logic.straight_ufo_p2;
    json["hide_endscreen"] = logic.hide_endscreen;
    json["hide_new_best"] = logic.hide_new_best;
    json["no_effects"] = logic.no_effects;
    json["straight_fly_threshold_p1"] = logic.straight_fly_threshold_p1;
    json["straight_fly_threshold_p2"] = logic.straight_fly_threshold_p2;
    json["straight_ufo_target_p1"] = logic.straight_ufo_target_p1;
    json["straight_ufo_target_p2"] = logic.straight_ufo_target_p2;
    json["straight_ufo_threshold_p1"] = logic.straight_ufo_threshold_p1;
    json["straight_ufo_threshold_p2"] = logic.straight_ufo_threshold_p2;
    json["extra_click_count"] = logic.extra_click_count;
    json["autoclicker"] = logic.autoclicker;
    json["sync_audio"] = logic.sync_audio;
    json["sync_audio_threshold"] = logic.sync_audio_threshold;
    json["audio_speedhack"] = logic.audio_speedhack;
    json["practice_music_hack"] = logic.practice_music_hack;
    json["autoclick_p1"] = logic.autoclick_p1;
    json["autoclick_p2"] = logic.autoclick_p2;
    json["autoclick_auto_disable"] = logic.autoclick_auto_disable;
    json["sequence_enabled"] = logic.sequence_enabled;
    json["sequence_random"] = logic.sequence_random;
    json["max_cps"] = logic.max_cps;
    json["hitbox_thickness"] = logic.hitbox_thickness;
    json["hitbox_thickness_thin"] = true;
    json["trail_length"] = logic.trail_length;
    json["sort_automatically"] = logic.sort_automatically;
    json["keep_checkpoint_hold"] = logic.keep_checkpoint_hold;
    json["immediate_restart"] = logic.immediate_restart;
    json["layout_regular_bg"] = logic.layout_use_regular_bg;
    json["render_width"] = logic.render_width;
    json["render_presets"] = matjson::Value::array();
    for (auto const& preset : logic.render_presets) {
        matjson::Value entry;
        entry["name"] = preset.name;
        entry["width"] = preset.width;
        entry["height"] = preset.height;
        entry["fps"] = preset.fps;
        entry["bitrate"] = preset.bitrate;
        json["render_presets"].push(entry);
    }
    json["render_height"] = logic.render_height;
    json["render_fps"] = logic.render_fps;
    json["render_bitrate"] = logic.render_bitrate;
    json["render_fade_in"] = logic.render_fade_in;
    json["render_fade_out"] = logic.render_fade_out;
    json["render_after_end"] = logic.render_after_end;
    json["render_music"] = logic.render_music;
    json["render_sfx"] = logic.render_sfx;
    json["render_record_pause"] = logic.render_record_pause;
    json["render_show_level"] = logic.render_show_lvl;
    json["render_screen_shape"] = logic.render_screen_shape;
    json["seed_override"] = logic.seed_override;
    json["seed_override_value"] = std::to_string(logic.seed_override_value);
    json["render_ssb"] = logic.render_ssb_fix;
    json["render_speedhack"] = logic.render_speedhack;
    json["render_color_fix"] = logic.render_color_fix;
    json["render_hide_labels"] = logic.render_hide_labels;
    json["render_hide_pause"] = logic.render_hide_pause;
    json["render_auto_exit"] = logic.render_auto_exit;
    json["render_codec"] = logic.render_codec;
    json["render_preset"] = logic.render_preset;
    json["render_extension"] = logic.render_extension;
    json["render_args"] = logic.render_args;
    json["render_name"] = logic.render_name;
    json["fill_hitboxes"] = logic.fill_hitboxes;
    json["hitbox_outline_only"] = logic.hitbox_outline_only;
    json["hitbox_trail"] = logic.hitbox_trail;
    json["hitbox_trail_visual"] = logic.hitbox_trail_visual;
    json["hitbox_dedup"] = logic.hitbox_dedup;
    json["hitbox_player"] = logic.hitbox_player;
    json["hitbox_click"] = logic.hitbox_click;
    json["hitbox_rotated"] = logic.hitbox_rotated;
    json["hitbox_inner"] = logic.hitbox_inner;
    json["hitbox_circle"] = logic.hitbox_circle;
    json["hitbox_solid"] = logic.hitbox_solid;
    json["hitbox_slope"] = logic.hitbox_slope;
    json["hitbox_hazard"] = logic.hitbox_hazard;
    json["hitbox_interact"] = logic.hitbox_interact;
    json["hide_activated_objects"] = logic.hide_activated_objects;
    json["hitbox_fill_opacity"] = logic.hitbox_fill_opacity;
    json["hitbox_trail_opacity"] = logic.hitbox_trail_opacity;
    json["show_trajectory"] = logic.show_traj;
    json["trajectory_hold"] = logic.traj_hold;
    json["trajectory_release"] = logic.traj_release;
    json["trajectory_swift"] = logic.traj_swift;
    json["trajectory_seconds"] = logic.traj_seconds;
    json["trajectory_width"] = logic.traj_width;
    json["docked_menu"] = logic.docked_menu;
    json["floating_x"] = logic.floating_x;
    json["floating_y"] = logic.floating_y;
    json["frame_bar_x"] = logic.frame_bar_x;
    json["frame_bar_y"] = logic.frame_bar_y;
    json["floating_hide_in_level"] = logic.floating_hide_in_lvl;
    json["floating_size"] = logic.floating_size;
    json["menu_scale"] = logic.menu_scale;
    json["menu_scale_phone"] = true;
    json["menu_inner_scale"] = logic.menu_inner_scale;
    json["menu_scale_undocked"] = logic.menu_scale_undocked;
    json["menu_anim_speed"] = logic.menu_anim_speed;
    json["theme_name"] = logic.theme_name;
    json["ui_style"] = logic.ui_style;
    json["tiles"] = matjson::Value::array();
    for (auto const& place : logic.tile_places) {
        matjson::Value entry;
        entry["key"] = place.key;
        entry["pinned"] = place.pinned;
        entry["hidden"] = place.hidden;
        entry["x"] = place.x;
        entry["y"] = place.y;
        json["tiles"].push(entry);
    }
    json["open_pages"] = matjson::Value::array();
    for (auto const& name : logic.open_pages)
        json["open_pages"].push(name);
    json["tab_order"] = matjson::Value::array();
    for (auto const& name : logic.tab_order)
        json["tab_order"].push(name);
    auto write_color = [&](char const* key, float const* color, int count) {
        json[key] = matjson::Value::array();
        for (int i = 0; i < count; ++i)
            json[key].push(color[i]);
    };
    write_color("hitbox_outer_color", logic.hitbox_outer_color, 4);
    write_color("hitbox_rotated_color", logic.hitbox_rotated_color, 4);
    write_color("hitbox_inner_color", logic.hitbox_inner_color, 4);
    write_color("hitbox_circle_color", logic.hitbox_circle_color, 4);
    write_color("hitbox_solid_color", logic.hitbox_solid_color, 4);
    write_color("hitbox_slope_color", logic.hitbox_slope_color, 4);
    write_color("hitbox_hazard_color", logic.hitbox_hazard_color, 4);
    write_color("hitbox_trail_color", logic.hitbox_trail_color, 4);
    write_color("hitbox_interact_color", logic.hitbox_interact_color, 4);
    write_color("hitbox_click_color", logic.hitbox_click_color, 4);
    write_color("hitbox_release_color", logic.hitbox_release_color, 4);
    write_color("trajectory_color", logic.traj_color, 4);
    write_color("trajectory_swift_color", logic.traj_swift_color, 4);
    write_color("trajectory_release_color", logic.traj_release_color, 4);
    write_color("editor_p1", logic.editor_p1, 4);
    write_color("editor_p2", logic.editor_p2, 4);
    write_color("editor_sel_p1", logic.editor_sel_p1, 4);
    write_color("editor_sel_p2", logic.editor_sel_p2, 4);
    json["rename_format"] = logic.rename_format;
    json["clickpack"] = logic.clickpack;
    json["clickpack_p2"] = logic.clickpack_p2;
    json["clickbot"] = logic.clickbot;
    json["frame_advance_hold_duration"] = logic.advance_hold_ms;
    json["frame_advance_delay"] = logic.advance_delay_ms;
    json["frame_advance_speed"] = logic.advance_speed;
    json["pause_before_kill"] = logic.pause_before_kill;
    json["trajectory_autoclicker"] = logic.traj_autoclicker;
    json["trajectory_moving_objects"] = logic.traj_moving_objects;
    json["backstep"] = logic.backstep;
    json["velocity_unrounding"] = logic.velocity_unrounding;
    json["backstep_percent"] = logic.backstep_percent;
    json["best_frame_range"] = logic.best_frame_range;
    json["best_frame_click"] = logic.best_frame_click;
    json["best_frame_auto"] = logic.best_frame_auto;
    json["best_frame_release"] = logic.best_frame_release;
    json["pause_at_trajectory_length"] = logic.pause_at_traj_length;
    json["pause_trajectory_ms"] = logic.pause_traj_ms;
    json["percent_accuracy"] = logic.percent_accuracy;
    json["clickbot_volume"] = logic.clickbot_volume;
    json["clickbot_volume_p2"] = logic.clickbot_volume_p2;
    json["p1_softs"] = logic.p1_softs;
    json["p2_softs"] = logic.p2_softs;
    json["p1_hards"] = logic.p1_hards;
    json["p2_hards"] = logic.p2_hards;
    json["p1_micros"] = logic.p1_micros;
    json["p2_micros"] = logic.p2_micros;
    json["p1_softs_time"] = logic.p1_soft_ms;
    json["p2_softs_time"] = logic.p2_soft_ms;
    json["p1_hards_time"] = logic.p1_hard_seconds;
    json["p2_hards_time"] = logic.p2_hard_seconds;
    json["p1_micros_time"] = logic.p1_micro_ms;
    json["p2_micros_time"] = logic.p2_micro_ms;
    json["press_interval"] = logic.auto_click.press_interval;
    json["release_interval"] = logic.auto_click.release_interval;
    json["autoclick_swifts"] = logic.auto_click.swifts;
    json["backgroundColor"] = matjson::Value::array();
    json["blocksColor"] = matjson::Value::array();
    for (int i = 0; i < 3; ++i) {
        json["backgroundColor"].push(logic.backgroundColor[i]);
        json["blocksColor"].push(logic.blocks_color[i]);
    }
    json["frame_label"] = Logic::get().placement_json(logic.frame_label);
    json["cps_label"] = Logic::get().placement_json(logic.cps_label);
    json["percent_label"] = Logic::get().placement_json(logic.percent_label);
    json["time_label"] = Logic::get().placement_json(logic.time_label);
    json["recording_label"] = Logic::get().placement_json(logic.recording_label);
    json["position_label"] = Logic::get().placement_json(logic.pos_label);
    json["velocity_label"] = Logic::get().placement_json(logic.velocity_label);
    json["alive_label"] = Logic::get().placement_json(logic.alive_label);
    json["max_cps_label"] = Logic::get().placement_json(logic.max_cps_label);
    json["gravity_label"] = Logic::get().placement_json(logic.gravity_label);
    json["utility_label"] = Logic::get().placement_json(logic.utility_label);
    json["death_label"] = Logic::get().placement_json(logic.death_label);
    json["safe_mode_label"] = Logic::get().placement_json(logic.safe_mode_label);

    // geode keeps keybinds in the same file & i used to wipe them :/
    auto* mod = Mod::get();
    for (auto const& key : mod->getSettingKeys()) {
        auto setting = mod->getSetting(key);
        if (!setting)
            continue;
        auto* binds = typeinfo_cast<KeybindSettingV3*>(setting.get());
        if (!binds)
            continue;
        matjson::Value list = matjson::Value::array();
        for (auto const& bind : binds->getValue()) {
            list.push(matjson::makeObject({
                { "key", static_cast<int>(bind.key) },
                { "modifiers", static_cast<int>(bind.modifiers) },
            }));
        }
        json[key] = std::move(list);
    }
    if (auto* manager = ModSettingsManager::from(mod))
        manager->getSaveData() = json;

    {
        std::ofstream file(Logic::get().save_root() / "settings.json");
        if (file)
            file << json.dump(4);
    }
    if (!bulk_settings)
        (void)Mod::get()->saveData();
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
