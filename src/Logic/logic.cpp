#include "Logic/logic.hpp"
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




using namespace geode::prelude;

namespace {
bool bulkSettings = false;
}


namespace {
thread_local Logic* g_loading = nullptr;
}

void Logic::setLoading(Logic* session) {
	g_loading = session;
}
Logic& Logic::live() {
	static Logic value;
	return value;
}
Logic& Logic::get() {
	if (g_loading) return *g_loading;
	return Logic::live();
}
const char* Logic::stateName(RunState state) {
	switch (state) {
		case RunState::Recording: return "Recording";
		case RunState::Playing: return "Playing";
		case RunState::RecordingAndPlaying: return "Record+Play";
		case RunState::Idle: return "Idle";
	}
	return "Idle";
}
std::filesystem::path Logic::saveRoot() {
	return Mod::get()->getSaveDir();
}






std::string Logic::readFolderSetting(char const* key) {
	auto value = Mod::get()->getSettingValue<std::filesystem::path>(key);
	return value.string();
}
matjson::Value Logic::placementJson(LabelPlacement const& placement) {
	matjson::Value json;
	json["x"] = placement.x;
	json["y"] = placement.y;
	json["scale"] = placement.scale;
	json["opacity"] = placement.opacity;
	return json;
}
LabelPlacement Logic::readPlacement(matjson::Value const& json, LabelPlacement fallback) {
	if (!json.isObject()) return fallback;
	if (auto number = json["x"].asDouble()) fallback.x = static_cast<float>(number.unwrap());
	if (auto number = json["y"].asDouble()) fallback.y = static_cast<float>(number.unwrap());
	if (auto number = json["scale"].asDouble()) fallback.scale = static_cast<float>(number.unwrap());
	if (auto number = json["opacity"].asInt()) fallback.opacity = static_cast<int>(number.unwrap());
	return fallback;
}
void Logic::boot() {
	auto root = Logic::get().saveRoot();
	std::error_code error;
	for (auto const* folder : {"replays", "converted", "clickpacks", "renders"}) {
		std::filesystem::create_directories(root / folder, error);
	}
	auto packs = root / "clickpacks";
	bool packsEmpty = true;
	if (std::filesystem::is_directory(packs)) {
		for (auto const& entry : std::filesystem::directory_iterator(packs, error)) {
			(void)entry;
			packsEmpty = false;
			break;
		}
	}
	if (packsEmpty) {
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
			std::filesystem::create_directories(packs / folder, error);
		}
	}
	auto migrateKey = [](char const* id, cocos2d::enumKeyCodes from, KeyboardModifier fromMods, cocos2d::enumKeyCodes to, KeyboardModifier toMods) {
		auto current = Mod::get()->getSettingValue<std::vector<Keybind>>(id);
		if (current.size() != 1 || current[0].key != from || current[0].modifiers != fromMods) return;
		Mod::get()->setSettingValue<std::vector<Keybind>>(id, {Keybind(to, toMods)});
	};
	auto const leftAlt = KeyboardModifier::Alt;
	migrateKey("open-menu", cocos2d::KEY_Alt, KeyboardModifier::None, cocos2d::KEY_LeftMenu, leftAlt);
	migrateKey("open-menu", cocos2d::KEY_LeftMenu, KeyboardModifier::None, cocos2d::KEY_LeftMenu, leftAlt);
	migrateKey("toggle-advance", cocos2d::KEY_F6, KeyboardModifier::None, cocos2d::KEY_V, KeyboardModifier::None);
	migrateKey("step-frame", cocos2d::KEY_F7, KeyboardModifier::None, cocos2d::KEY_C, KeyboardModifier::None);
	if (std::fabs(Logic::readFloat("macro-fps", 240.f) - 60.f) < 0.01f) Logic::writeFloat("macro-fps", 240.f);
	Logic::get().pullSettings();
	SettingChangedEventV3().listen([](std::string_view modID, std::string_view, std::shared_ptr<SettingV3>) {
		if (bulkSettings || modID != Mod::get()->getID()) return ListenerResult::Propagate;
		Logic::get().saveSettingsFile();
		return ListenerResult::Propagate;
	});
	log::info("Echo Geode port ready. Replays: {}", root.string());
}
void Logic::pullSettings() {
	auto& value = Logic::get();
	bool hadTrail = false;
	bool trailFile = false;
	auto path = Logic::get().saveRoot() / "settings.json";
	if (std::filesystem::exists(path)) {
		std::ifstream file(path);
		auto parsed = matjson::Value::parse(file);
		if (parsed) {
			auto const& json = parsed.unwrap();
			if (auto number = json["macro_fps"].asDouble()) value.fps = static_cast<float>(number.unwrap());
			if (auto number = json["speedhack"].asDouble()) value.speedhack = static_cast<float>(number.unwrap());
			if (auto number = json["speedhack_memory"].asDouble()) value.speedhackMemory = static_cast<float>(number.unwrap());
			else if (std::abs(value.speedhack - 1.f) > 0.05f) value.speedhackMemory = value.speedhack;
			if (auto flag = json["real_time_mode"].asBool()) value.realTimeMode = flag.unwrap();
			if (auto flag = json["lock_delta"].asBool()) value.lockDelta = flag.unwrap();
			if (auto flag = json["lock_fps"].asBool()) value.lockFps = flag.unwrap();
			if (auto flag = json["fix_tps_rotation"].asBool()) value.fixTpsRotation = flag.unwrap();
			if (auto flag = json["save_debug"].asBool()) value.saveDebug = flag.unwrap();
			if (auto flag = json["lock_display_fps"].asBool()) value.lockDisplayFps = flag.unwrap();
			if (auto number = json["playback_fps"].asDouble()) value.playbackFps = std::clamp(static_cast<float>(number.unwrap()), 0.f, 100000.f);
			else if (auto number = json["playback_fps"].asInt()) value.playbackFps = std::clamp(static_cast<float>(number.unwrap()), 0.f, 100000.f);
			if (auto flag = json["ignore_inputs"].asBool()) value.ignoreInputs = flag.unwrap();
			if (auto flag = json["no_overwrite"].asBool()) value.noOverwrite = flag.unwrap();
			if (auto flag = json["record_p1"].asBool()) value.recordP1 = flag.unwrap();
			if (auto flag = json["record_p2"].asBool()) value.recordP2 = flag.unwrap();
			if (auto flag = json["play_p1"].asBool()) value.playP1 = flag.unwrap();
			if (auto flag = json["play_p2"].asBool()) value.playP2 = flag.unwrap();
			if (auto flag = json["click_both"].asBool()) value.clickBoth = flag.unwrap();
			if (auto number = json["follow_mode"].asInt()) value.followMode = std::clamp(static_cast<int>(number.unwrap()), 0, 2);
			if (auto number = json["follow_delay"].asDouble()) value.followDelay = std::max(static_cast<float>(number.unwrap()), 0.f);
			if (auto flag = json["follow_delay_ms"].asBool()) value.followDelayMs = flag.unwrap();
			if (auto flag = json["maintain_gravity"].asBool()) value.maintainGravity = flag.unwrap();
			if (auto flag = json["same_gravity"].asBool()) value.sameGravity = flag.unwrap();
			if (auto flag = json["hide_p1"].asBool()) value.hideP1 = flag.unwrap();
			if (auto flag = json["hide_p2"].asBool()) value.hideP2 = flag.unwrap();
			if (auto flag = json["hide_hitbox_p1"].asBool()) value.hideHitboxP1 = flag.unwrap();
			if (auto flag = json["hide_hitbox_p2"].asBool()) value.hideHitboxP2 = flag.unwrap();
			if (auto flag = json["hitboxes_follow_hide_player"].asBool()) value.hitboxesFollowHidePlayer = flag.unwrap();
			if (auto flag = json["hide_trajectory_p1"].asBool()) value.hideTrajectoryP1 = flag.unwrap();
			if (auto flag = json["hide_trajectory_p2"].asBool()) value.hideTrajectoryP2 = flag.unwrap();
			if (auto flag = json["trajectory_follow_hide_player"].asBool()) value.trajectoryFollowHidePlayer = flag.unwrap();
			if (auto flag = json["disable_death_effect"].asBool()) value.disableDeathEffect = flag.unwrap();
			if (auto flag = json["safe_mode"].asBool()) value.safeMode = flag.unwrap();
			if (auto flag = json["swap_players"].asBool()) value.swapPlayers = flag.unwrap();
			if (auto flag = json["inverse_p1"].asBool()) value.inverseP1 = flag.unwrap();
			if (auto flag = json["inverse_p2"].asBool()) value.inverseP2 = flag.unwrap();
			if (auto flag = json["noclip_p1"].asBool()) value.noclipP1 = flag.unwrap();
			if (auto flag = json["noclip_p2"].asBool()) value.noclipP2 = flag.unwrap();
			if (auto flag = json["show_hitboxes"].asBool()) value.showHitboxes = flag.unwrap();
			if (auto flag = json["layout_mode"].asBool()) value.layoutMode = flag.unwrap();
			if (auto flag = json["layout_default_art"].asBool()) value.layoutDefaultArt = flag.unwrap();
			if (auto flag = json["disable_shakes"].asBool()) value.disableShakes = flag.unwrap();
			if (auto flag = json["no_mirror_portals"].asBool()) value.noMirrorPortals = flag.unwrap();
			if (auto flag = json["show_frame"].asBool()) value.showFrame = flag.unwrap();
			if (auto flag = json["show_cps"].asBool()) value.showCps = flag.unwrap();
			if (auto flag = json["show_percent"].asBool()) value.showPercent = flag.unwrap();
			if (auto flag = json["show_time"].asBool()) value.showTime = flag.unwrap();
			if (auto flag = json["show_recording"].asBool()) value.showRecording = flag.unwrap();
			if (auto flag = json["show_position"].asBool()) value.showPosition = flag.unwrap();
			if (auto flag = json["show_velocity"].asBool()) value.showVelocity = flag.unwrap();
			if (auto flag = json["show_alive"].asBool()) value.showAlive = flag.unwrap();
			if (auto flag = json["show_max_cps"].asBool()) value.showMaxCps = flag.unwrap();
			if (auto flag = json["show_gravity"].asBool()) value.showGravity = flag.unwrap();
			if (auto flag = json["show_utilities"].asBool()) value.showUtilities = flag.unwrap();
			if (auto flag = json["show_safe_mode"].asBool()) value.showSafeMode = flag.unwrap();
			if (auto flag = json["labels_auto"].asBool()) value.labelsAuto = flag.unwrap();
			if (auto number = json["label_corner"].asInt()) value.labelCorner = std::clamp(static_cast<int>(number.unwrap()), 0, 3);
			if (auto number = json["label_scale"].asDouble()) value.labelScale = std::clamp(static_cast<float>(number.unwrap()), .05f, 10.f);
			if (auto text = json["label_font"].asString()) value.labelFont = text.unwrap();
			if (json["label_order"].isArray()) {
				value.labelOrder.clear();
				for (auto const& key : json["label_order"]) {
					if (auto text = key.asString()) value.labelOrder.push_back(text.unwrap());
				}
			}
			if (auto flag = json["show_death_next"].asBool()) value.showDeathNext = flag.unwrap();
			if (auto flag = json["click_dash_orbs"].asBool()) value.clickDashOrbs = flag.unwrap();
			if (auto flag = json["click_black_orbs"].asBool()) value.clickBlackOrbs = flag.unwrap();
			if (auto number = json["orb_click_count"].asInt()) value.orbClickCount = std::max(1, static_cast<int>(number.unwrap()));
			if (auto flag = json["pad_black_orb"].asBool()) value.padBlackOrb = flag.unwrap();
			if (auto flag = json["pad_blue_orb"].asBool()) value.padBlueOrb = flag.unwrap();
			if (auto flag = json["autoclicker"].asBool()) value.autoclicker = flag.unwrap();
			if (auto flag = json["sync_audio"].asBool()) value.syncAudio = flag.unwrap();
			if (auto flag = json["autoclick_p1"].asBool()) value.autoclickP1 = flag.unwrap();
			if (auto flag = json["autoclick_p2"].asBool()) value.autoclickP2 = flag.unwrap();
			if (auto flag = json["autoclick_auto_disable"].asBool()) value.autoclickAutoDisable = flag.unwrap();
			if (auto flag = json["sequence_enabled"].asBool()) value.sequenceEnabled = flag.unwrap();
			if (auto flag = json["sequence_random"].asBool()) value.sequenceRandom = flag.unwrap();
			if (auto number = json["max_cps"].asDouble()) value.maxCps = static_cast<float>(number.unwrap());
			if (auto number = json["hitbox_thickness"].asDouble()) value.hitboxThickness = static_cast<float>(number.unwrap());
			if (auto number = json["hitbox_scale"].asDouble()) value.hitboxScale = static_cast<float>(number.unwrap());
			if (auto number = json["trail_length"].asInt()) value.trailLength = std::max(0, static_cast<int>(number.unwrap()));
			if (auto flag = json["sort_automatically"].asBool()) value.sortAutomatically = flag.unwrap();
			if (auto flag = json["keep_checkpoint_hold"].asBool()) value.keepCheckpointHold = flag.unwrap();
			if (auto flag = json["immediate_restart"].asBool()) value.immediateRestart = flag.unwrap();
			if (auto flag = json["layout_regular_bg"].asBool()) value.layoutUseRegularBg = flag.unwrap();
			if (auto number = json["render_width"].asInt()) value.renderWidth = std::max(2, static_cast<int>(number.unwrap()));
			if (auto number = json["render_height"].asInt()) value.renderHeight = std::max(2, static_cast<int>(number.unwrap()));
			if (auto number = json["render_fps"].asInt()) value.renderFps = std::max(1, static_cast<int>(number.unwrap()));
			if (auto number = json["render_bitrate"].asInt()) {
				int bits = static_cast<int>(number.unwrap());
				value.renderBitrate = bits <= 0 ? 0 : std::max(bits, 1000);
			}
			if (auto number = json["render_fade_in"].asDouble()) value.renderFadeIn = static_cast<float>(number.unwrap());
			if (auto number = json["render_fade_out"].asDouble()) value.renderFadeOut = static_cast<float>(number.unwrap());
			if (auto number = json["render_after_end"].asDouble()) value.renderAfterEnd = static_cast<float>(number.unwrap());
			if (auto number = json["render_music"].asDouble()) value.renderMusic = static_cast<float>(number.unwrap());
			if (auto number = json["render_sfx"].asDouble()) value.renderSfx = static_cast<float>(number.unwrap());
			if (auto flag = json["render_record_pause"].asBool()) value.renderRecordPause = flag.unwrap();
			if (auto flag = json["seed_override"].asBool()) value.seedOverride = flag.unwrap();
			if (json.contains("seed_override_value")) {
				if (auto text = json["seed_override_value"].asString()) value.seedOverrideValue = std::strtoull(text.unwrap().c_str(), nullptr, 10);
			}
			if (auto flag = json["render_ssb"].asBool()) value.renderSsbFix = flag.unwrap();
			if (auto flag = json["render_speedhack"].asBool()) value.renderSpeedhack = flag.unwrap();
			if (auto flag = json["render_color_fix"].asBool()) value.renderColorFix = flag.unwrap();
			if (auto flag = json["render_hide_labels"].asBool()) value.renderHideLabels = flag.unwrap();
			if (auto flag = json["render_auto_exit"].asBool()) value.renderAutoExit = flag.unwrap();
			if (auto text = json["render_codec"].asString()) value.renderCodec = text.unwrap();
			if (auto text = json["render_extension"].asString()) value.renderExtension = text.unwrap();
			if (auto text = json["render_preset"].asString()) {
				auto preset = text.unwrap();
				if (preset == "ultrafast" || preset == "veryfast" || preset == "medium") value.renderPreset = preset;
			}
			if (auto text = json["render_args"].asString()) value.renderArgs = text.unwrap();
			if (auto text = json["render_name"].asString()) value.renderName = text.unwrap();
			if (auto flag = json["fill_hitboxes"].asBool()) value.fillHitboxes = flag.unwrap();
			if (auto flag = json["hitbox_outline_only"].asBool()) value.hitboxOutlineOnly = flag.unwrap();
			if (json.contains("hitbox_trail")) {
				if (auto flag = json["hitbox_trail"].asBool()) {
					hadTrail = true;
					trailFile = flag.unwrap();
					value.hitboxTrail = trailFile;
				}
			}
			if (auto flag = json["hitbox_trail_visual"].asBool()) value.hitboxTrailVisual = flag.unwrap();
			// Deduplication has no toggle in the menu, so a saved true would stick with no way to turn it off.
			value.hitboxDedup = false;
			if (auto flag = json["hitbox_player"].asBool()) value.hitboxPlayer = flag.unwrap();
			if (auto flag = json["hitbox_click"].asBool()) value.hitboxClick = flag.unwrap();
			if (auto flag = json["hitbox_rotated"].asBool()) value.hitboxRotated = flag.unwrap();
			if (auto flag = json["hitbox_inner"].asBool()) value.hitboxInner = flag.unwrap();
			if (auto flag = json["hitbox_circle"].asBool()) value.hitboxCircle = flag.unwrap();
			if (auto flag = json["hitbox_solid"].asBool()) value.hitboxSolid = flag.unwrap();
			if (auto flag = json["hitbox_slope"].asBool()) value.hitboxSlope = flag.unwrap();
			if (auto flag = json["hitbox_hazard"].asBool()) value.hitboxHazard = flag.unwrap();
			if (auto flag = json["hitbox_interact"].asBool()) value.hitboxInteract = flag.unwrap();
			if (auto flag = json["hide_activated_objects"].asBool()) value.hideActivatedObjects = flag.unwrap();
			if (auto number = json["hitbox_fill_opacity"].asDouble()) value.hitboxFillOpacity = static_cast<float>(number.unwrap());
			if (auto number = json["hitbox_trail_opacity"].asDouble()) value.hitboxTrailOpacity = std::clamp(static_cast<float>(number.unwrap()), 0.f, 1.f);
			if (auto flag = json["show_trajectory"].asBool()) value.showTrajectory = flag.unwrap();
			if (auto flag = json["trajectory_hold"].asBool()) value.trajectoryHold = flag.unwrap();
			if (auto flag = json["trajectory_release"].asBool()) value.trajectoryRelease = flag.unwrap();
			if (auto flag = json["trajectory_swift"].asBool()) value.trajectorySwift = flag.unwrap();
			bool hadSeconds = false;
			if (auto number = json["trajectory_seconds"].asDouble()) {
				value.trajectorySeconds = std::clamp(static_cast<float>(number.unwrap()), 0.05f, 4.f);
				hadSeconds = true;
			} else if (auto frames = json["trajectory_length"].asInt()) {
				value.trajectorySeconds = std::clamp(static_cast<float>(frames.unwrap()) / 240.f, 0.05f, 4.f);
			}
			if (auto number = json["trajectory_width"].asDouble()) value.trajectoryWidth = static_cast<float>(number.unwrap());
			if (!hadSeconds && value.trajectoryWidth >= 1.4f) value.trajectoryWidth = 0.5f;
			if (auto flag = json["docked_menu"].asBool()) value.dockedMenu = flag.unwrap();
			if (auto number = json["menu_scale"].asDouble()) value.menuScale = std::clamp(static_cast<float>(number.unwrap()), 0.35f, 3.f);
			if (auto number = json["menu_anim_speed"].asDouble()) value.menuAnimSpeed = std::clamp(static_cast<float>(number.unwrap()), 0.25f, 4.f);
			if (auto text = json["theme_name"].asString()) value.themeName = text.unwrap();
			if (json["tab_order"].isArray() && json["tab_order"].size() > 0) {
				std::vector<std::string> order;
				for (auto const& entry : json["tab_order"]) {
					if (auto name = entry.asString()) order.push_back(name.unwrap());
				}
				if (!order.empty()) value.tabOrder = std::move(order);
			}
			auto readColor = [&](char const* key, float* color, int count) {
				if (!json[key].isArray() || static_cast<int>(json[key].size()) < count) return;
				for (int i = 0; i < count; ++i) {
					if (auto channel = json[key][i].asDouble()) color[i] = static_cast<float>(channel.unwrap());
				}
			};
			readColor("hitbox_outer_color", value.hitboxOuterColor, 4);
			readColor("hitbox_rotated_color", value.hitboxRotatedColor, 4);
			readColor("hitbox_inner_color", value.hitboxInnerColor, 4);
			readColor("hitbox_circle_color", value.hitboxCircleColor, 4);
			readColor("hitbox_solid_color", value.hitboxSolidColor, 4);
			readColor("hitbox_slope_color", value.hitboxSlopeColor, 4);
			readColor("hitbox_hazard_color", value.hitboxHazardColor, 4);
			readColor("hitbox_trail_color", value.hitboxTrailColor, 4);
			readColor("hitbox_interact_color", value.hitboxInteractColor, 4);
			readColor("hitbox_click_color", value.hitboxClickColor, 4);
			readColor("hitbox_release_color", value.hitboxReleaseColor, 4);
			if (json.contains("trajectory_color")) readColor("trajectory_color", value.trajectoryColor, 4);
			if (json.contains("trajectory_swift_color")) readColor("trajectory_swift_color", value.trajectorySwiftColor, 4);
			if (json.contains("trajectory_release_color")) readColor("trajectory_release_color", value.trajectoryReleaseColor, 4);
			readColor("editor_p1", value.editorP1, 4);
			readColor("editor_p2", value.editorP2, 4);
			readColor("editor_sel_p1", value.editorSelP1, 4);
			readColor("editor_sel_p2", value.editorSelP2, 4);
			if (auto text = json["rename_format"].asString()) value.renameFormat = text.unwrap();
			if (auto text = json["clickpack"].asString()) value.clickpack = text.unwrap();
			if (json.contains("clickpack_p2")) {
				if (auto text = json["clickpack_p2"].asString()) value.clickpackP2 = text.unwrap();
			} else {
				value.clickpackP2 = value.clickpack;
			}
			if (auto flag = json["clickbot"].asBool()) value.clickbot = flag.unwrap();
			if (auto number = json["frame_advance_hold_duration"].asInt()) value.advanceHoldMs = static_cast<unsigned>(number.unwrap());
			if (auto number = json["frame_advance_delay"].asInt()) value.advanceDelayMs = static_cast<unsigned>(number.unwrap());
			if (auto number = json["frame_advance_speed"].asDouble()) value.advanceSpeed = std::clamp(static_cast<float>(number.unwrap()), 1.f, 200.f);
			if (auto flag = json["pause_before_kill"].asBool()) value.pauseBeforeKill = flag.unwrap();
			if (auto flag = json["trajectory_moving_objects"].asBool()) value.trajectoryMovingObjects = flag.unwrap();
			if (auto flag = json["backstep"].asBool()) value.backstep = flag.unwrap();
			if (auto flag = json["velocity_unrounding"].asBool()) value.velocityUnrounding = flag.unwrap();
			if (auto count = json["backstep_saves"].asInt()) value.backstepSaves = std::clamp(static_cast<int>(count.unwrap()), 1, 100000);
			if (auto flag = json["pause_at_trajectory_length"].asBool()) value.pauseAtTrajectoryLength = flag.unwrap();
			// Temporarily disabled — a leftover true flag was forcing a full traj predict every frame.
			value.pauseAtTrajectoryLength = false;
			if (auto number = json["pause_trajectory_ms"].asInt()) {
				value.pauseTrajectoryMs = static_cast<unsigned>(std::clamp(static_cast<int>(number.unwrap()), 1, 4000));
			} else if (auto number = json["pause_trajectory_seconds"].asDouble()) {
				value.pauseTrajectoryMs = static_cast<unsigned>(std::clamp(static_cast<int>(std::lround(number.unwrap() * 1000.0)), 1, 4000));
			}
			if (auto number = json["percent_accuracy"].asInt()) value.percentAccuracy = static_cast<int>(number.unwrap());
			if (auto number = json["clickbot_volume"].asDouble()) value.clickbotVolume = static_cast<float>(number.unwrap());
			if (json.contains("clickbot_volume_p2")) {
				if (auto number = json["clickbot_volume_p2"].asDouble()) value.clickbotVolumeP2 = static_cast<float>(number.unwrap());
			} else {
				value.clickbotVolumeP2 = value.clickbotVolume;
			}
			if (auto flag = json["p1_softs"].asBool()) value.p1Softs = flag.unwrap();
			if (auto flag = json["p2_softs"].asBool()) value.p2Softs = flag.unwrap();
			if (auto flag = json["p1_hards"].asBool()) value.p1Hards = flag.unwrap();
			if (auto flag = json["p2_hards"].asBool()) value.p2Hards = flag.unwrap();
			if (auto flag = json["p1_micros"].asBool()) value.p1Micros = flag.unwrap();
			if (auto flag = json["p2_micros"].asBool()) value.p2Micros = flag.unwrap();
			if (auto number = json["p1_softs_time"].asDouble()) value.p1SoftMs = static_cast<float>(number.unwrap());
			if (auto number = json["p2_softs_time"].asDouble()) value.p2SoftMs = static_cast<float>(number.unwrap());
			if (auto number = json["p1_hards_time"].asDouble()) value.p1HardSeconds = static_cast<float>(number.unwrap());
			if (auto number = json["p2_hards_time"].asDouble()) value.p2HardSeconds = static_cast<float>(number.unwrap());
			if (auto number = json["p1_micros_time"].asInt()) value.p1MicroMs = static_cast<int>(number.unwrap());
			if (auto number = json["p2_micros_time"].asInt()) value.p2MicroMs = static_cast<int>(number.unwrap());
			if (json["backgroundColor"].isArray() && json["backgroundColor"].size() >= 3) {
				for (int i = 0; i < 3; ++i) {
					if (auto channel = json["backgroundColor"][i].asDouble()) value.backgroundColor[i] = static_cast<float>(channel.unwrap());
				}
			}
			if (json["blocksColor"].isArray() && json["blocksColor"].size() >= 3) {
				for (int i = 0; i < 3; ++i) {
					if (auto channel = json["blocksColor"][i].asDouble()) value.blocksColor[i] = static_cast<float>(channel.unwrap());
				}
			}
			value.frameLabel = Logic::get().readPlacement(json["frame_label"], value.frameLabel);
			value.cpsLabel = Logic::get().readPlacement(json["cps_label"], value.cpsLabel);
			value.percentLabel = Logic::get().readPlacement(json["percent_label"], value.percentLabel);
			value.timeLabel = Logic::get().readPlacement(json["time_label"], value.timeLabel);
			value.recordingLabel = Logic::get().readPlacement(json["recording_label"], value.recordingLabel);
			value.positionLabel = Logic::get().readPlacement(json["position_label"], value.positionLabel);
			value.velocityLabel = Logic::get().readPlacement(json["velocity_label"], value.velocityLabel);
			value.aliveLabel = Logic::get().readPlacement(json["alive_label"], value.aliveLabel);
			value.maxCpsLabel = Logic::get().readPlacement(json["max_cps_label"], value.maxCpsLabel);
			value.gravityLabel = Logic::get().readPlacement(json["gravity_label"], value.gravityLabel);
			value.utilityLabel = Logic::get().readPlacement(json["utility_label"], value.utilityLabel);
			value.deathLabel = Logic::get().readPlacement(json["death_label"], value.deathLabel);
			value.safeModeLabel = Logic::get().readPlacement(json["safe_mode_label"], value.safeModeLabel);
			if (auto number = json["press_interval"].asInt()) value.autoClick.pressInterval = static_cast<int>(number.unwrap());
			if (auto number = json["release_interval"].asInt()) value.autoClick.releaseInterval = static_cast<int>(number.unwrap());
		}
	}

	value.fps = Logic::readFloat("macro-fps", value.fps);
	value.speedhack = Logic::readFloat("speedhack", value.speedhack);
	value.realTimeMode = Logic::readBool("real-time", value.realTimeMode);
	value.lockDelta = Logic::readBool("lock-delta", value.lockDelta);
	value.lockDisplayFps = Logic::readBool("lock-display-fps", value.lockDisplayFps);
	value.saveDebug = Logic::readBool("save-debug", value.saveDebug);
	value.ignoreInputs = Logic::readBool("ignore-inputs", value.ignoreInputs);
	value.noOverwrite = Logic::readBool("no-overwrite", value.noOverwrite);
	value.recordP1 = Logic::readBool("record-p1", value.recordP1);
	value.recordP2 = Logic::readBool("record-p2", value.recordP2);
	value.playP1 = Logic::readBool("play-p1", value.playP1);
	value.playP2 = Logic::readBool("play-p2", value.playP2);
	value.clickBoth = Logic::readBool("click-both", value.clickBoth);
	value.maintainGravity = Logic::readBool("maintain-gravity", value.maintainGravity);
	value.sameGravity = Logic::readBool("same-gravity", value.sameGravity);
	value.swapPlayers = Logic::readBool("swap-players", value.swapPlayers);
	value.inverseP1 = Logic::readBool("inverse-p1", value.inverseP1);
	value.inverseP2 = Logic::readBool("inverse-p2", value.inverseP2);
	value.noclipP1 = Logic::readBool("noclip-p1", value.noclipP1);
	value.noclipP2 = Logic::readBool("noclip-p2", value.noclipP2);
	value.showHitboxes = Logic::readBool("show-hitboxes", value.showHitboxes);
	value.fillHitboxes = Logic::readBool("fill-hitboxes", value.fillHitboxes);
	value.hitboxTrail = Logic::readBool("hitbox-trail", value.hitboxTrail);
	value.hitboxClick = Logic::readBool("hitbox-click", value.hitboxClick);
	if (hadTrail) value.hitboxTrail = trailFile;
	value.layoutMode = Logic::readBool("layout-mode", value.layoutMode);
	value.disableShakes = Logic::readBool("disable-shakes", value.disableShakes);
	value.showFrame = Logic::readBool("show-frame", value.showFrame);
	value.showCps = Logic::readBool("show-cps", value.showCps);
	value.showPercent = Logic::readBool("show-percent", value.showPercent);
	value.showTime = Logic::readBool("show-time", value.showTime);
	value.showRecording = Logic::readBool("show-recording", value.showRecording);
	value.maxCps = Logic::readFloat("max-cps", value.maxCps);
	value.autoclicker = Logic::readBool("autoclicker", value.autoclicker);
	value.autoclickP1 = Logic::readBool("autoclick-p1", value.autoclickP1);
	value.autoclickP2 = Logic::readBool("autoclick-p2", value.autoclickP2);
	value.autoClick.pressInterval = Logic::readInt("autoclick-press", value.autoClick.pressInterval);
	value.autoClick.releaseInterval = Logic::readInt("autoclick-release", value.autoClick.releaseInterval);
	value.autoclickAutoDisable = Logic::readBool("autoclick-auto-disable", value.autoclickAutoDisable);
	auto pack = Logic::get().readFolderSetting("clickpack");
	if (!pack.empty()) value.clickpack = pack;
	if (value.clickpack.empty()) {
		auto example = Logic::get().saveRoot() / "clickpacks" / "example";
		std::error_code error;
		if (std::filesystem::is_directory(example, error)) value.clickpack = example.string();
	}
	if (value.clickpackP2.empty()) value.clickpackP2 = value.clickpack;
	if (value.fps < 1.f) value.fps = 240.f;
	value.macroFps = value.fps;
	if (value.speedhack <= 0.f) value.speedhack = 1.f;
	if (value.maxCps <= 0.f) value.maxCps = static_cast<float>(Logic::kMaxPerSecond);
	value.speedhack = 1.f;
	value.format = value.saveDebug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
}
void Logic::pushRuntimeSettings() {
	bulkSettings = true;
	auto const& value = Logic::get();
	Logic::writeFloat("macro-fps", value.fps);
	Logic::writeFloat("speedhack", value.speedhack);
	Logic::writeBool("real-time", value.realTimeMode);
	Logic::writeBool("lock-delta", value.lockDelta);
	Logic::writeBool("save-debug", value.saveDebug);
	Logic::writeBool("ignore-inputs", value.ignoreInputs);
	Logic::writeBool("no-overwrite", value.noOverwrite);
	Logic::writeBool("record-p1", value.recordP1);
	Logic::writeBool("record-p2", value.recordP2);
	Logic::writeBool("play-p1", value.playP1);
	Logic::writeBool("play-p2", value.playP2);
	Logic::writeBool("click-both", value.clickBoth);
	Logic::writeBool("maintain-gravity", value.maintainGravity);
	Logic::writeBool("same-gravity", value.sameGravity);
	Logic::writeBool("swap-players", value.swapPlayers);
	Logic::writeBool("inverse-p1", value.inverseP1);
	Logic::writeBool("inverse-p2", value.inverseP2);
	Logic::writeBool("noclip-p1", value.noclipP1);
	Logic::writeBool("noclip-p2", value.noclipP2);
	Logic::writeBool("show-hitboxes", value.showHitboxes);
	Logic::writeBool("fill-hitboxes", value.fillHitboxes);
	Logic::writeBool("hitbox-trail", value.hitboxTrail);
	Logic::writeBool("hitbox-click", value.hitboxClick);
	Logic::writeBool("layout-mode", value.layoutMode);
	Logic::writeBool("disable-shakes", value.disableShakes);
	Logic::writeBool("show-frame", value.showFrame);
	Logic::writeBool("show-cps", value.showCps);
	Logic::writeBool("show-percent", value.showPercent);
	Logic::writeBool("show-time", value.showTime);
	Logic::writeBool("show-recording", value.showRecording);
	Logic::writeFloat("max-cps", value.maxCps);
	Logic::writeBool("autoclicker", value.autoclicker);
	Logic::writeBool("autoclick-p1", value.autoclickP1);
	Logic::writeBool("autoclick-p2", value.autoclickP2);
	Logic::writeInt("autoclick-press", value.autoClick.pressInterval);
	Logic::writeInt("autoclick-release", value.autoClick.releaseInterval);
	Logic::writeBool("autoclick-auto-disable", value.autoclickAutoDisable);
	Mod::get()->setSettingValue<std::filesystem::path>("clickpack", std::filesystem::path(value.clickpack));
	bulkSettings = false;
	Logic::get().saveSettingsFile();
}
void Logic::saveSettingsFile() {
	auto const& value = Logic::get();
	matjson::Value json;
	json["macro_fps"] = value.fps;
	json["speedhack"] = value.speedhack;
	json["speedhack_memory"] = value.speedhackMemory;
	json["real_time_mode"] = value.realTimeMode;
	json["lock_delta"] = value.lockDelta;
	json["lock_fps"] = value.lockFps;
	json["fix_tps_rotation"] = value.fixTpsRotation;
	json["save_debug"] = value.saveDebug;
	json["lock_display_fps"] = value.lockDisplayFps;
	json["playback_fps"] = value.playbackFps;
	json["ignore_inputs"] = value.ignoreInputs;
	json["no_overwrite"] = value.noOverwrite;
	json["record_p1"] = value.recordP1;
	json["record_p2"] = value.recordP2;
	json["play_p1"] = value.playP1;
	json["play_p2"] = value.playP2;
	json["click_both"] = value.clickBoth;
	json["follow_mode"] = value.followMode;
	json["follow_delay"] = value.followDelay;
	json["follow_delay_ms"] = value.followDelayMs;
	json["maintain_gravity"] = value.maintainGravity;
	json["same_gravity"] = value.sameGravity;
	json["hide_p1"] = value.hideP1;
	json["hide_p2"] = value.hideP2;
	json["hide_hitbox_p1"] = value.hideHitboxP1;
	json["hide_hitbox_p2"] = value.hideHitboxP2;
	json["hitboxes_follow_hide_player"] = value.hitboxesFollowHidePlayer;
	json["hide_trajectory_p1"] = value.hideTrajectoryP1;
	json["hide_trajectory_p2"] = value.hideTrajectoryP2;
	json["trajectory_follow_hide_player"] = value.trajectoryFollowHidePlayer;
	json["disable_death_effect"] = value.disableDeathEffect;
	json["safe_mode"] = value.safeMode;
	json["swap_players"] = value.swapPlayers;
	json["inverse_p1"] = value.inverseP1;
	json["inverse_p2"] = value.inverseP2;
	json["noclip_p1"] = value.noclipP1;
	json["noclip_p2"] = value.noclipP2;
	json["show_hitboxes"] = value.showHitboxes;
	json["layout_mode"] = value.layoutMode;
	json["layout_default_art"] = value.layoutDefaultArt;
	json["disable_shakes"] = value.disableShakes;
	json["no_mirror_portals"] = value.noMirrorPortals;
	json["show_frame"] = value.showFrame;
	json["show_cps"] = value.showCps;
	json["show_percent"] = value.showPercent;
	json["show_time"] = value.showTime;
	json["show_recording"] = value.showRecording;
	json["show_position"] = value.showPosition;
	json["show_velocity"] = value.showVelocity;
	json["show_alive"] = value.showAlive;
	json["show_max_cps"] = value.showMaxCps;
	json["show_gravity"] = value.showGravity;
	json["show_utilities"] = value.showUtilities;
	json["show_safe_mode"] = value.showSafeMode;
	json["labels_auto"] = value.labelsAuto;
	json["label_corner"] = value.labelCorner;
	json["label_scale"] = value.labelScale;
	json["label_font"] = value.labelFont;
	json["label_order"] = matjson::Value::array();
	for (auto const& key : value.labelOrder) json["label_order"].push(key);
	json["show_death_next"] = value.showDeathNext;
	json["click_dash_orbs"] = value.clickDashOrbs;
	json["click_black_orbs"] = value.clickBlackOrbs;
	json["orb_click_count"] = value.orbClickCount;
	json["pad_black_orb"] = value.padBlackOrb;
	json["pad_blue_orb"] = value.padBlueOrb;
	json["autoclicker"] = value.autoclicker;
	json["sync_audio"] = value.syncAudio;
	json["autoclick_p1"] = value.autoclickP1;
	json["autoclick_p2"] = value.autoclickP2;
	json["autoclick_auto_disable"] = value.autoclickAutoDisable;
	json["sequence_enabled"] = value.sequenceEnabled;
	json["sequence_random"] = value.sequenceRandom;
	json["max_cps"] = value.maxCps;
	json["hitbox_thickness"] = value.hitboxThickness;
	json["hitbox_scale"] = value.hitboxScale;
	json["trail_length"] = value.trailLength;
	json["sort_automatically"] = value.sortAutomatically;
	json["keep_checkpoint_hold"] = value.keepCheckpointHold;
	json["immediate_restart"] = value.immediateRestart;
	json["layout_regular_bg"] = value.layoutUseRegularBg;
	json["render_width"] = value.renderWidth;
	json["render_height"] = value.renderHeight;
	json["render_fps"] = value.renderFps;
	json["render_bitrate"] = value.renderBitrate;
	json["render_fade_in"] = value.renderFadeIn;
	json["render_fade_out"] = value.renderFadeOut;
	json["render_after_end"] = value.renderAfterEnd;
	json["render_music"] = value.renderMusic;
	json["render_sfx"] = value.renderSfx;
	json["render_record_pause"] = value.renderRecordPause;
	json["seed_override"] = value.seedOverride;
	json["seed_override_value"] = std::to_string(value.seedOverrideValue);
	json["render_ssb"] = value.renderSsbFix;
	json["render_speedhack"] = value.renderSpeedhack;
	json["render_color_fix"] = value.renderColorFix;
	json["render_hide_labels"] = value.renderHideLabels;
	json["render_auto_exit"] = value.renderAutoExit;
	json["render_codec"] = value.renderCodec;
	json["render_preset"] = value.renderPreset;
	json["render_extension"] = value.renderExtension;
	json["render_args"] = value.renderArgs;
	json["render_name"] = value.renderName;
	json["fill_hitboxes"] = value.fillHitboxes;
	json["hitbox_outline_only"] = value.hitboxOutlineOnly;
	json["hitbox_trail"] = value.hitboxTrail;
	json["hitbox_trail_visual"] = value.hitboxTrailVisual;
	json["hitbox_dedup"] = value.hitboxDedup;
	json["hitbox_player"] = value.hitboxPlayer;
	json["hitbox_click"] = value.hitboxClick;
	json["hitbox_rotated"] = value.hitboxRotated;
	json["hitbox_inner"] = value.hitboxInner;
	json["hitbox_circle"] = value.hitboxCircle;
	json["hitbox_solid"] = value.hitboxSolid;
	json["hitbox_slope"] = value.hitboxSlope;
	json["hitbox_hazard"] = value.hitboxHazard;
	json["hitbox_interact"] = value.hitboxInteract;
	json["hide_activated_objects"] = value.hideActivatedObjects;
	json["hitbox_fill_opacity"] = value.hitboxFillOpacity;
	json["hitbox_trail_opacity"] = value.hitboxTrailOpacity;
	json["show_trajectory"] = value.showTrajectory;
	json["trajectory_hold"] = value.trajectoryHold;
	json["trajectory_release"] = value.trajectoryRelease;
	json["trajectory_swift"] = value.trajectorySwift;
	json["trajectory_seconds"] = value.trajectorySeconds;
	json["trajectory_width"] = value.trajectoryWidth;
	json["docked_menu"] = value.dockedMenu;
	json["menu_scale"] = value.menuScale;
	json["menu_anim_speed"] = value.menuAnimSpeed;
	json["theme_name"] = value.themeName;
	json["tab_order"] = matjson::Value::array();
	for (auto const& name : value.tabOrder) json["tab_order"].push(name);
	auto writeColor = [&](char const* key, float const* color, int count) {
		json[key] = matjson::Value::array();
		for (int i = 0; i < count; ++i) json[key].push(color[i]);
	};
	writeColor("hitbox_outer_color", value.hitboxOuterColor, 4);
	writeColor("hitbox_rotated_color", value.hitboxRotatedColor, 4);
	writeColor("hitbox_inner_color", value.hitboxInnerColor, 4);
	writeColor("hitbox_circle_color", value.hitboxCircleColor, 4);
	writeColor("hitbox_solid_color", value.hitboxSolidColor, 4);
	writeColor("hitbox_slope_color", value.hitboxSlopeColor, 4);
	writeColor("hitbox_hazard_color", value.hitboxHazardColor, 4);
	writeColor("hitbox_trail_color", value.hitboxTrailColor, 4);
	writeColor("hitbox_interact_color", value.hitboxInteractColor, 4);
	writeColor("hitbox_click_color", value.hitboxClickColor, 4);
	writeColor("hitbox_release_color", value.hitboxReleaseColor, 4);
	writeColor("trajectory_color", value.trajectoryColor, 4);
	writeColor("trajectory_swift_color", value.trajectorySwiftColor, 4);
	writeColor("trajectory_release_color", value.trajectoryReleaseColor, 4);
	writeColor("editor_p1", value.editorP1, 4);
	writeColor("editor_p2", value.editorP2, 4);
	writeColor("editor_sel_p1", value.editorSelP1, 4);
	writeColor("editor_sel_p2", value.editorSelP2, 4);
	json["rename_format"] = value.renameFormat;
	json["clickpack"] = value.clickpack;
	json["clickpack_p2"] = value.clickpackP2;
	json["clickbot"] = value.clickbot;
	json["frame_advance_hold_duration"] = value.advanceHoldMs;
	json["frame_advance_delay"] = value.advanceDelayMs;
	json["frame_advance_speed"] = value.advanceSpeed;
	json["pause_before_kill"] = value.pauseBeforeKill;
	json["trajectory_moving_objects"] = value.trajectoryMovingObjects;
	json["backstep"] = value.backstep;
	json["velocity_unrounding"] = value.velocityUnrounding;
	json["backstep_saves"] = value.backstepSaves;
	json["pause_at_trajectory_length"] = value.pauseAtTrajectoryLength;
	json["pause_trajectory_ms"] = value.pauseTrajectoryMs;
	json["percent_accuracy"] = value.percentAccuracy;
	json["clickbot_volume"] = value.clickbotVolume;
	json["clickbot_volume_p2"] = value.clickbotVolumeP2;
	json["p1_softs"] = value.p1Softs;
	json["p2_softs"] = value.p2Softs;
	json["p1_hards"] = value.p1Hards;
	json["p2_hards"] = value.p2Hards;
	json["p1_micros"] = value.p1Micros;
	json["p2_micros"] = value.p2Micros;
	json["p1_softs_time"] = value.p1SoftMs;
	json["p2_softs_time"] = value.p2SoftMs;
	json["p1_hards_time"] = value.p1HardSeconds;
	json["p2_hards_time"] = value.p2HardSeconds;
	json["p1_micros_time"] = value.p1MicroMs;
	json["p2_micros_time"] = value.p2MicroMs;
	json["press_interval"] = value.autoClick.pressInterval;
	json["release_interval"] = value.autoClick.releaseInterval;
	json["backgroundColor"] = matjson::Value::array();
	json["blocksColor"] = matjson::Value::array();
	for (int i = 0; i < 3; ++i) {
		json["backgroundColor"].push(value.backgroundColor[i]);
		json["blocksColor"].push(value.blocksColor[i]);
	}
	json["frame_label"] = Logic::get().placementJson(value.frameLabel);
	json["cps_label"] = Logic::get().placementJson(value.cpsLabel);
	json["percent_label"] = Logic::get().placementJson(value.percentLabel);
	json["time_label"] = Logic::get().placementJson(value.timeLabel);
	json["recording_label"] = Logic::get().placementJson(value.recordingLabel);
	json["position_label"] = Logic::get().placementJson(value.positionLabel);
	json["velocity_label"] = Logic::get().placementJson(value.velocityLabel);
	json["alive_label"] = Logic::get().placementJson(value.aliveLabel);
	json["max_cps_label"] = Logic::get().placementJson(value.maxCpsLabel);
	json["gravity_label"] = Logic::get().placementJson(value.gravityLabel);
	json["utility_label"] = Logic::get().placementJson(value.utilityLabel);
	json["death_label"] = Logic::get().placementJson(value.deathLabel);
	json["safe_mode_label"] = Logic::get().placementJson(value.safeModeLabel);

	// Geode stores keybinds in this same file, and only writes them on its own save.
	// Echo used to replace the file first and leave that save looking at a stale copy,
	// so a rebind never reached disk.
	auto* mod = Mod::get();
	for (auto const& key : mod->getSettingKeys()) {
		auto setting = mod->getSetting(key);
		if (!setting) continue;
		auto* binds = typeinfo_cast<KeybindSettingV3*>(setting.get());
		if (!binds) continue;
		matjson::Value list = matjson::Value::array();
		for (auto const& bind : binds->getValue()) {
			list.push(matjson::makeObject({
				{"key", static_cast<int>(bind.key)},
				{"modifiers", static_cast<int>(bind.modifiers)},
			}));
		}
		json[key] = std::move(list);
	}
	if (auto* manager = ModSettingsManager::from(mod)) manager->getSaveData() = json;

	{
		std::ofstream file(Logic::get().saveRoot() / "settings.json");
		if (file) file << json.dump(4);
	}
	if (!bulkSettings) (void)Mod::get()->saveData();
}


#ifndef NOMINMAX
#define NOMINMAX
#endif
