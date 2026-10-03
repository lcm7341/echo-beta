#pragma once

#include <atomic>
#include <Geode/Geode.hpp>
#include "Logic/replay_algorithms.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace chrono = std::chrono;

using namespace cocos2d;

class PlayLayer;
class PlayerObject;
struct PlayerButtonCommand;
class GameObject;
class EnhancedGameObject;
class GJBaseGameLayer;
class LevelEditorLayer;
class TeleportPortalObject;

inline constexpr bool step_patched = true;
inline constexpr float VANILLA_TPS = 240.f;

enum class RunState {
	Idle,
	Recording,
	Playing,
	RecordingAndPlaying,
};

enum class ReplayFormat {
	Simple,
	Debug,
	Meta,
	MetaDebug,
};

struct RenderStatus {
	bool active = false;
	bool known = false;
	bool finishing = false;
	float progress = 0.f;
	float speed = 0.f;
	float eta = -1.f;
	std::string text;
};

struct Frame {
	int number = 0;
	uint8_t button = 1;
	bool holding = false;
	bool player2 = false;
};

struct FramePose {
	float x = 0.f, y = 0.f;
	float rotation = 0.f;
	double yVelocity = 0.0, x_velocity = 0.0;
};

struct RateChange;

struct TilePlace {
	std::string key;
	bool pinned = false;
	bool hidden = false;
	float x = 0.f, y = 0.f;
};

struct RenderPreset {
	std::string name;
	int width = 1920, height = 1080;
	int fps = 60;
	int bitrate = 20000000;
};

struct Replay {
	std::string name;
	std::vector<Frame> actions;
	float fps = 240.f;
	std::vector<struct RateChange> rates;
	uint64_t lvl_seed = 0;
	bool lvl_seed_known = false;
	bool unrounded = false;
	bool silicate_variance = false;
};

struct AutoClick {
	int press_interval = 50, release_interval = 50;
	int swifts = 0; // extra clicks on the SAME frame as each press
	int since_press = 0, since_release = 0;
	bool pressed = false;
	int disable_in = 1;
};

struct LabelPlacement {
	float x = 0.f, y = 0.f;
	float scale = 0.4f;
	int opacity = 40;
};

struct PlayerSnapshot {
	bool present = false;
	float x = 0.f, y = 0.f;
	float rotation = 0.f, visual_rotation = 0.f;
	double yVelocity = 0.0, platformer_x_velocity = 0.0, y_velocity_related = 0.0;
	double fall_speed = 0.0;
	double gravity = 0.0;
	double speed_multiplier = 0.0;
	double y_start = 0.0;
	double maybe_reverse_speed = 0.0, maybe_reverse_acceleration = 0.0, acceleration_or_speed = 0.0;
	double snap_distance = 0.0;
	double last_flip_time = 0.0, game_mode_changed_time = 0.0, lastLandTime = 0.0, totalTime = 0.0;
	float player_speed = 0.f, player_speed_ac = 0.f;
	float vehicle_size = 1.f;
	float gravity_mod = 1.f;
	float x_velocity_related = 0.f, x_velocity_related2 = 0.f, y_velocity_related3 = 0.f;
	float rotation_speed = 0.f;
	decltype(PlayerObject::m_rotateSpeed) rotate_speed{};
	decltype(PlayerObject::m_isRotating) rotating{};
	decltype(PlayerObject::m_isBallRotating) ball_rotating{};
	decltype(PlayerObject::m_isBallRotating2) ball_rotating2{};
	decltype(PlayerObject::m_slopeRotation) slope_rotation{};
	float slope_velocity = 0.f;
	CCPoint ship_rotation;
	CCPoint last_portal_pos;
	bool ship = false;
	bool bird = false;
	bool ball = false;
	bool dart = false;
	bool robot = false;
	bool spider = false;
	bool swing = false;
	bool upside_down = false;
	bool on_ground = false, on_ground2 = false, on_ground3 = false, on_ground4 = false;
	bool going_left = false;
	bool sideways = false;
	bool hidden = false;
	bool dashing = false;
	bool dead = false;
	bool jump_buffered = false;
	bool on_slope = false, was_on_slope = false;
	bool was_teleported = false;
	bool fix_gravity_bug = false;
	bool reverse_sync = false;
	bool locked = false;
	bool controls_disabled = false;
	bool holding_right = false, holding_left = false;
	int reverse_related = 0;
	decltype(PlayerObject::m_leftPressedFirst) left_pressed_first{};
	decltype(PlayerObject::m_platformerMovingLeft) movingLeft{};
	decltype(PlayerObject::m_platformerMovingRight) movingRight{};
	decltype(PlayerObject::m_platformerVelocityRelated) platformer_velocity_related{};
	decltype(PlayerObject::m_maybeIsBoosted) boosted{};
	decltype(PlayerObject::m_scaleXRelatedTime) scale_x_related_time{};
	decltype(PlayerObject::m_decreaseBoostSlide) decrease_boost_slide{};
	decltype(PlayerObject::m_scaleXRelated) scale_x_related{};
	decltype(PlayerObject::m_scaleXRelated2) scale_x_related2{};
	decltype(PlayerObject::m_scaleXRelated3) scale_x_related3{};
	decltype(PlayerObject::m_scaleXRelated4) scale_x_related4{};
	decltype(PlayerObject::m_scaleXRelated5) scale_x_related5{};
	decltype(PlayerObject::m_maybeHasStopped) has_stopped{};
	decltype(PlayerObject::m_maybeGoingCorrectSlopeDirection) correct_slope_direction{};
	decltype(PlayerObject::m_isSliding) sliding{};
	decltype(PlayerObject::m_maybeSlopeForce) slope_force{};
	decltype(PlayerObject::m_isOnIce) on_ice{};
	decltype(PlayerObject::m_physDeltaRelated) phys_delta_related{};
	decltype(PlayerObject::m_maybeSlidingTime) sliding_time{};
	decltype(PlayerObject::m_maybeSlidingStartTime) sliding_start_time{};
	decltype(PlayerObject::m_changedDirectionsTime) changed_directions_time{};
	decltype(PlayerObject::m_slopeEndTime) slope_end_time{};
	decltype(PlayerObject::m_isMoving) moving{};
	decltype(PlayerObject::m_isSlidingRight) sliding_right{};
	decltype(PlayerObject::m_maybeChangedDirectionAngle) changed_direction_angle{};
	decltype(PlayerObject::m_groundYVelocity) ground_y_velocity{};
	decltype(PlayerObject::m_yVelocityBeforeSlope) y_velocity_before_slope{};
	decltype(PlayerObject::m_slopeStartTime) slope_start_time{};
	decltype(PlayerObject::m_lastGroundedPos) last_grounded_pos{};
	decltype(PlayerObject::m_isAccelerating) accelerating{};
	decltype(PlayerObject::m_groundObjectMaterial) ground_material{};
	decltype(PlayerObject::m_somethingPlayerSpeedTime) player_speed_time{};
	decltype(PlayerObject::m_affectedByForces) affected_by_forces{};
	decltype(PlayerObject::m_stateForce) state_force{};
	decltype(PlayerObject::m_stateForceVector) state_force_vector{};
	decltype(PlayerObject::m_stateBoostX) state_boost_x{};
	decltype(PlayerObject::m_stateBoostY) state_boost_y{};
	decltype(PlayerObject::m_stateFlipGravity) state_flip_gravity{};
	decltype(PlayerObject::m_stateScale) state_scale{};
	decltype(PlayerObject::m_stateDartSlide) state_dart_slide{};
	decltype(PlayerObject::m_stateNoAutoJump) state_no_auto_jump{};
	decltype(PlayerObject::m_stateRingJump) state_ring_jump{};
	decltype(PlayerObject::m_stateRingJump2) state_ring_jump2{};
	decltype(PlayerObject::m_wasJumpBuffered) was_jump_buffered{};
	decltype(PlayerObject::m_stateJumpBuffered) state_jump_buffered{};
	// from the checkpoint NEVER the death frame
	decltype(PlayerObject::m_ringJumpRelated) ring_jump_related{};
	decltype(PlayerObject::m_touchedRing) touched_ring{};
	decltype(PlayerObject::m_touchedCustomRing) touched_custom_ring{};
	decltype(PlayerObject::m_touchedPad) touched_pad{};
	decltype(PlayerObject::m_wasRobotJump) was_robot_jump{};
	decltype(PlayerObject::m_blackOrbRelated) black_orb_related{};
	decltype(PlayerObject::m_dashX) dash_x{};
	decltype(PlayerObject::m_dashY) dash_y{};
	decltype(PlayerObject::m_dashAngle) dash_angle{};
	decltype(PlayerObject::m_dashStartTime) dash_start_time{};
	decltype(PlayerObject::m_dashRing) dash_ring{};
	decltype(PlayerObject::m_spriteWidthScale) sprite_width_scale{};
	decltype(PlayerObject::m_spriteHeightScale) sprite_height_scale{};
	float scaleX = 1.f, scaleY = 1.f;
	CCPoint internal_pos;
	CCPoint lastPosition;
};

struct LayerSnapshot {
	double levelTime = 0.0, totalTime = 0.0, unk_time = 0.0, attemptTime = 0.0;
	unsigned progress = 0;
	float timeWarp = 1.f, queued_time_warp = 1.f, time_warp_related = 0.f, timeMod = 0.f;
	float camera_angle = 0.f;
	float levelFlipping = 0.f;
	CCPoint camera_pos;
	bool dual = false;
	bool streak_blend = false;
	bool gravity_related = false, time_mod_related = false;
};

struct ObjectBase {
	std::vector<GameObject*> objects;
	std::vector<std::uint8_t> bytes;
};

struct ObjectStates {
	int frame = -1;
	std::shared_ptr<ObjectBase const> base; // every checkpoint shares ts
	std::vector<std::uint8_t> diff; // only what changed from base
};

struct Checkpoint {
	CheckpointObject* object = nullptr; // gd owns it
	int frame = 0;
	size_t replay_pos = 0;
	int removed_frames = 0;
	bool held_p1 = false, held_p2 = false, held_left_p1 = false, held_left_p2 = false, held_right_p1 = false;
	bool held_right_p2 = false;
	PlayerSnapshot player1, player2;
	LayerSnapshot layer;
	uint64_t random_state = 0, shake_state = 0, tp_state = 0;
	std::vector<float> variance;
	std::vector<std::pair<int, uint64_t>> rand_states;
	std::shared_ptr<ObjectStates> objects;
};

struct CpsNote {
	int first = 0, last = 0;
	std::string text;
};

struct RateChange {
	int frame = 0;
	float fps = 240.f;
};

struct HitboxTrailPoint {
	CCRect rect;
	CCRect inner;
	std::array<CCPoint, 4> corners{};
	CCPoint center{};
	float radius = 0.f;
	bool rotated = false;
	int frame = 0;
};

struct BackstepInfo;

struct Session {
	RunState state = RunState::Idle;
	ReplayFormat format = ReplayFormat::Meta;
	std::vector<Frame> inputs;
	std::vector<FramePose> poses;
	std::vector<uint64_t> input_ids;
	std::array<int, 6> live_frame{ -1, -1, -1, -1, -1, -1 };
	std::array<char, 6> live_held{}, live_seen{};
	int newest_input = -1;
	int hold_count = 0, release_count = 0;
	bool inputs_in_order = true;
	uint64_t inputs_revision = 0;
	uint64_t next_input_id = 0;
	std::deque<int> clicks_p1, clicks_p2;
	uint64_t clicks_revision = 0;
	std::vector<Checkpoint> checkpoints;

	struct BackstepSave {
		CheckpointObject* object = nullptr;
		Checkpoint echo;
		std::shared_ptr<BackstepInfo> extra;
		std::vector<std::uint8_t> object_undo;
		bool broken = false; // failed the check
	};

	std::deque<BackstepSave> backsteps;
	bool backstepping = false;
	int backstep_target = -1; // -1 = none
	RunState backstep_state = RunState::Idle;
	bool backstep_play_p1 = false, backstep_play_p2 = false;
	bool holding_back = false;
	std::vector<Frame> backstep_inputs;
	std::vector<FramePose> backstep_poses;
	std::vector<uint64_t> backstep_ids;
	size_t backstep_replay_pos = 0;
	RunState backstep_saved_state = RunState::Idle;
	bool backstep_swapped = false;
	chrono::steady_clock::time_point back_hold_start{}, last_back_step{};
	CheckpointObject* restored_checkpoint = nullptr;
	std::vector<Replay> sequence;
	std::vector<size_t> sequence_order;
	size_t replay_pos = 0, replay_index = 0;
	int step_frame = 0;
	int removed_frames = 0;
	int progress_base = 0;
	bool progress_base_set = false;
	bool in_step = false;
	uint32_t partner_presses = 0;
	float fps = 240.f, macro_fps = 240.f;
	// silicate saves the LAST tps it used, so a macro with tps changes can start on the wrong one. ts lets me fix it by hand
	bool playback_tps_changes = false;
	bool trace_logging = false;
	std::vector<RateChange> rates;
	int accuracy = 1;
	bool present = true;
	bool resetting = false;
	double carry = 0.0;
	int steps_left = 0;
	bool skip_physics = false;
	bool defer_player_art = false, player_art_deferred = false;
	uint32_t steps_this_tick = 0;
	int frame_steps = 0;
	int respawn_frames = 0, ignore_record_frames = 0;
	bool restoring_holds = false;
	bool refresh_only = false, refresh_queued = false;
	bool in_scheduler = false;
	int scheduler_depth = 0;
	bool save_shown_step = false;
	bool buttons_prepared = false;
	int replay_clicks_left = 0;
	float speedhack = 1.f;
	float speedhack_memory = 0.f;
	bool real_time_mode = true;
	bool lock_delta = false;
	bool fix_tps_rotation = false;
	bool lock_display_fps = false;
	float playback_fps = 0.f;
	bool playback_fps_held = false;
	float held_interval = 1.f / 60.f;
	bool save_debug = false, show_clock_debug = false;
	std::vector<std::string> tab_order{ "Play", "Macro", "Editor", "Macro Tools", "Visuals", "Sequence", "Render", "Theme", "Clickbot" };
	bool ignore_inputs = true;
	bool no_overwrite = false;
	bool record_p1 = true, record_p2 = true, play_p1 = true, play_p2 = true;
	bool click_both = false;
	int follow_mode = 0;
	float follow_delay = 10.f;
	bool follow_delay_ms = false;

	struct FollowPress {
		int frame;
		double time;
		bool down;
		int button;
		bool sent = false;
	};

	std::deque<FollowPress> follow_log;
	bool follow_injecting = false;
	bool maintain_gravity = false, same_gravity = false, same_gravity_seen = false;
	bool same_gravity_was_p1 = false, same_gravity_was_p2 = false, maintaining_gravity = false;
	// the other icon in a 1 player dual. NOT a 2nd input
	bool mirror_click = false, suppress_click_hitbox = false, suppress_rings = false;
	bool restart_hold_captured = false;
	bool hide_p1 = false, hide_p2 = false, hide_hitbox_p1 = false, hide_hitbox_p2 = false;
	bool hitboxes_follow_hide_player = true, hide_traj_p1 = false, hide_traj_p2 = false;
	bool traj_follow_hide_player = true;
	bool disable_death_effect = false;
	bool safeMode = false;
	bool swap_players = false;
	bool inverse_p1 = false, inverse_p2 = false, noclip_p1 = false, noclip_p2 = false;
	bool show_hitboxes = false, fill_hitboxes = false;
	bool hitbox_outline_only = false, hitbox_trail = false, hitbox_trail_visual = false, hitbox_dedup = false;
	int visual_frame = 0;
	float hitbox_thickness = 0.3f;
	bool sort_automatically = true;
	int trail_length = 50;
	bool hitbox_player = true, hitbox_rotated = true, hitbox_inner = true, hitbox_circle = true;
	bool hitbox_solid = true, hitbox_slope = true, hitbox_hazard = true, hitbox_interact = true;
	bool hide_activated_objects = false;
	bool hitbox_click = true;
	float hitbox_fill_opacity = 0.22f, hitbox_trail_opacity = 0.35f;
	float hitbox_outer_color[4] = { 1.f, 1.f, 1.f, 1.f };
	float hitbox_rotated_color[4] = { 1.f, 0.25f, 0.25f, 1.f };
	float hitbox_inner_color[4] = { 0.35f, 1.f, 0.45f, 1.f };
	float hitbox_circle_color[4] = { 0.45f, 0.75f, 1.f, 1.f };
	float hitbox_solid_color[4] = { 0.15f, 0.35f, 1.f, 1.f };
	float hitbox_slope_color[4] = { 0.45f, 0.55f, 1.f, 1.f };
	float hitbox_hazard_color[4] = { 1.f, 0.15f, 0.15f, 1.f };
	float hitbox_trail_color[4] = { 1.f, 1.f, 1.f, 0.35f };
	float hitbox_interact_color[4] = { 0.2f, 1.f, 0.35f, 1.f };
	float hitbox_click_color[4] = { 1.f, 0.85f, 0.15f, 1.f };
	float hitbox_release_color[4] = { 0.35f, 0.7f, 1.f, 1.f };
	bool predicting = false;
	bool showed_new_best = false;
	bool show_traj = false, traj_hold = true, traj_release = true, traj_swift = false;
	float traj_seconds = 1.f, traj_width = 0.5f, traj_color[4] = { 0.f, 1.f, 0.f, 1.f };
	float traj_swift_color[4] = { 1.f, 0.75f, 0.15f, 1.f };
	float traj_release_color[4] = { 0.25f, 0.65f, 1.f, 1.f };
	float editor_p1[4] = { 0.20f, 0.55f, 0.95f, 1.f }, editor_p2[4] = { 0.95f, 0.45f, 0.25f, 1.f };
	float editor_sel_p1[4] = { 0.55f, 0.82f, 1.f, 1.f }, editor_sel_p2[4] = { 1.f, 0.72f, 0.45f, 1.f };
	bool docked_menu = true;
	std::vector<std::string> open_pages{ "Replay", "Assists", "Visuals" };
	std::vector<TilePlace> tile_places;
	int ui_style = 0;
	float floating_x = -1.f, floating_y = -1.f;
	float frame_bar_x = -1.f, frame_bar_y = -1.f;
	bool floating_hide_in_lvl = true;
	float floating_size = 38.f;
	float menu_scale = 1.f, menu_scale_undocked = 1.f, menu_anim_speed = 1.f;
	std::string theme_name = "Default";
	bool layout_mode = false, layout_use_regular_bg = false, layout_default_art = true, layout_styled = false;
	bool disable_shakes = false;
	bool no_mirror_portals = false;
	bool show_frame = false, show_cps = false, show_percent = false, show_time = false, show_recording = true;
	bool show_pos = false, show_velocity = false, show_alive = false, show_max_cps = false;
	bool show_gravity = false, show_utilities = false, show_safe_mode = true, show_death_next = false;
	bool click_dash_orbs = false, click_black_orbs = false;
	int orb_click_count = 1;
	bool pad_black_orb = false, pad_blue_orb = false;
	bool death_next_p1 = false, noclip_dead_p1 = false, noclip_dead_p2 = false, death_next_p2 = false;
	float traj_alive_p1 = 0.f, traj_alive_p2 = 0.f;
	bool traj_dies_p1 = false, traj_dies_p2 = false;
	float traj_hold_alive_p1 = 0.f, traj_hold_alive_p2 = 0.f, traj_release_alive_p1 = 0.f;
	float traj_release_alive_p2 = 0.f;
	bool pause_before_kill = false;
	bool traj_autoclicker = false;
	bool traj_flip_p1 = false, traj_flip_p2 = false; // trajectory autoclicker "holding" for me (maintain gravity)
	bool find_best_queued = false;
	int best_frame_range = 120;
	bool best_frame_click = true;
	int best_frame_click_at = -1;
	bool best_frame_click_p1 = true, best_frame_auto = false, best_frame_running = false;
	bool best_frame_step_first = false;
	bool traj_moving_objects = true;
	bool backstep = false;
	int backstep_percent = 100;
	bool pause_at_traj_length = false;
	unsigned pause_traj_ms = 500;
	bool pause_kill_armed = true, pause_length_armed = true;
	int percent_accuracy = 1;
	int max_cps = 16;
	float current_cps = 0.f;
	bool over_max_cps = false, cps_breaking = false;
	bool currently_pressing = false;
	bool autoclicker = false;
	bool sync_audio = false;
	int sync_audio_threshold = 400; // ms the song may drift before it gets moved
	bool audio_speedhack = true;
	bool practice_music_hack = false;
	bool autoclick_p1 = true, autoclick_p2 = true, autoclick_auto_disable = false;
	AutoClick auto_click;
	bool clickbot = false;
	float clickbot_volume = 1.f, clickbot_volume_p2 = 1.f;
	std::string clickpack, clickpack_p2;
	bool p1_softs = true;
	float p1_soft_ms = 150.f;
	bool p1_hards = true;
	float p1_hard_seconds = 2.f;
	bool p1_micros = true;
	int p1_micro_ms = 25;
	bool p2_softs = true;
	float p2_soft_ms = 150.f;
	bool p2_hards = true;
	float p2_hard_seconds = 2.f;
	bool p2_micros = true;
	int p2_micro_ms = 25;
	double last_click_p1 = 0.0, last_click_p2 = 0.0;
	bool frame_advance = false;
	bool editor_testing = false, editor_paused = false;
	bool advance_queued = false;
	int advance_steps = 1;
	bool holding_advance = false;
	unsigned advance_hold_ms = 300, advance_delay_ms = 50;
	float advance_speed = 100.f;
	chrono::steady_clock::time_point advance_hold_start{}, last_advance_step{};
	bool injecting = false;
	bool applying_playback = false;
	bool hold_sync = false, keep_checkpoint_hold = true;
	bool immediate_restart = false, arm_immediate_restart = false;
	bool suppress_auto_hold = false;
	bool routing_input = false;
	std::map<int, int> steered_keys;
	bool key_jump_p1 = false, key_jump_p2 = false, key_left_p1 = false, key_right_p1 = false;
	bool key_left_p2 = false, key_right_p2 = false, death_jump_p1 = false, death_jump_p2 = false;
	bool death_left_p1 = false, death_right_p1 = false, death_left_p2 = false, death_right_p2 = false;
	bool physical_jump_p1 = false, physical_jump_p2 = false, physical_left_p1 = false;
	bool physical_right_p1 = false, physical_left_p2 = false, physical_right_p2 = false;
	bool ffmpeg_missing = false;
	bool resume_pending = false;
	bool sequence_enabled = false, sequence_random = false;
	std::string macro_name = "Replay";
	bool macro_name_typed = false;
	fs::path macro_path, autosaved_path;
	std::string rename_format = "_#";
	std::string message = "Ready";
	int attempts = 1;
	double recording_seconds = 0.0;
	uint64_t lvl_seed = 0;
	bool lvl_seed_known = false;
	bool velocity_unrounding = false;
	bool macro_unrounded = false, macro_silicate_variance = false;
	bool macro_ldm = false, macro_ldm_known = false; // low detail mode when it was recorded (old macros dont know)
	// everything off after a restart, only the menu & the step trace stay. for comparing against another bot

	bool unrounded_physics() const {
		return is_playing() || is_recording() ? macro_unrounded : velocity_unrounding;
	}

	bool shown_unrounded() const {
		return is_playing() || is_recording() || !inputs.empty() ? macro_unrounded : velocity_unrounding;
	}

	void set_unrounded(bool on) {
		velocity_unrounding = on;
		macro_unrounded = on;
	}

	bool lvl_seed_from_file = false, keep_lvl_seed = false, seed_override = false;
	uint64_t seed_override_value = 0;
	uint64_t shake_state = 0, tp_state = 0;
	float end_x = 0.f;
	bool display_fps_locked = false;
	float backgroundColor[3] = { 0.28235295f, 0.49019608f, 1.f };
	float blocks_color[3] = { 0.28235295f, 0.49019608f, 1.f };
	std::vector<RenderPreset> render_presets;
	int render_width = 1920, render_height = 1080, render_fps = 60, render_bitrate = 20000000;
	float render_fade_in = 0.5f, render_fade_out = 0.5f, render_after_end = 2.f, render_music = 1.f;
	float render_sfx = 1.f;
	bool render_queued = false, render_record_pause = false, render_show_lvl = true;
	bool render_screen_shape = true, render_ssb_fix = true, render_speedhack = false;
	float render_speed_locked = 0.f;
	bool render_hide_labels = false, render_hide_pause = false, render_color_fix = true;
	bool merge_busy = false;
	std::atomic<float> merge_seconds = 0.f; // how long the joined video will be
	fs::path merge_folder;
	std::string merge_label;
	bool render_auto_exit = false;
	std::string render_codec = "libx264", render_preset = "veryfast", render_extension = "mp4", render_args;
	std::string render_name;
	LabelPlacement frame_label{ 3.f, 268.f, 0.4f, 35 }, cps_label{ 3.f, 280.f, 0.4f, 35 };
	LabelPlacement percent_label{ 3.f, 310.f, 0.55f, 35 }, time_label{ 3.f, 294.f, 0.4f, 35 };
	LabelPlacement recording_label{ 0.f, 15.f, 0.75f, 70 }, pos_label{ 3.f, 252.f, 0.4f, 35 };
	LabelPlacement velocity_label{ 3.f, 238.f, 0.4f, 35 }, alive_label{ 3.f, 224.f, 0.4f, 35 };
	LabelPlacement max_cps_label{ 3.f, 210.f, 0.4f, 35 }, gravity_label{ 3.f, 196.f, 0.4f, 35 };
	LabelPlacement utility_label{ 3.f, 150.f, 0.35f, 80 }, death_label{ 3.f, 170.f, 0.5f, 100 };
	LabelPlacement safe_mode_label{ 3.f, 130.f, 0.35f, 80 };
	bool labels_auto = true;
	int label_corner = 0;
	float labelScale = 1.f;
	std::string labelFont = "bigFont.fnt";
	std::vector<std::string> label_order;
	std::deque<HitboxTrailPoint> trail_p1, trail_p2;
	std::vector<GameObject*> hidden_decorations;
	std::vector<CpsNote> cps_notes_p1, cps_notes_p2;
	std::string highest_cps = "P1: 0";

	uint64_t input_id(size_t index) const {
		return index < input_ids.size() ? input_ids[index] : 0;
	}

	void set_input_id(size_t index, uint64_t id) {
		if (index >= inputs.size())
			return;
		if (input_ids.size() <= index)
			input_ids.resize(index + 1);
		input_ids[index] = id;
	}

	FramePose pose_at(size_t index) const {
		return index < poses.size() ? poses[index] : FramePose{};
	}

	void set_pose_at(size_t index, FramePose pose) {
		if (poses.size() < inputs.size())
			poses.resize(inputs.size());
		if (index < poses.size())
			poses[index] = pose;
	}

	void clear_sides() {
		poses.clear();
		input_ids.clear();
		live_frame.fill(-1);
		live_held.fill(0);
		live_seen.fill(0);
		newest_input = -1;
		hold_count = 0;
		release_count = 0;
		inputs_in_order = true;
	}

	void touch_inputs() {
		++inputs_revision;
	}

	bool is_recording() const {
		return state == RunState::Recording || state == RunState::RecordingAndPlaying;
	}

	bool is_playing() const {
		return state == RunState::Playing || state == RunState::RecordingAndPlaying;
	}

	bool is_idle() const {
		return state == RunState::Idle;
	}

	bool plays_player(bool player2) const {
		if (!is_playing())
			return false;
		return player2 ? play_p2 : play_p1;
	}

	bool records_player(bool player2) const {
		if (!is_recording() || plays_player(player2))
			return false;
		return player2 ? record_p2 : record_p1;
	}

	bool physics_locked() const {
		return is_recording() || is_playing() || frame_advance;
	}

	int steps_until_rate_change() const {
		auto after = replay::rate_after(rates, step_frame);
		if (after != rates.end())
			return after->frame - step_frame;
		return 2048;
	}

	double sample_rate() const {
		float rate = std::isfinite(fps) ? fps : 240.f;
		return static_cast<double>(std::max(rate, step_patched ? 1.f : VANILLA_TPS));
	}

	float physics_step() const {
		return static_cast<float>(1.0 / sample_rate());
	}

	float rate_at(int frame) const {
		float rate = std::isfinite(macro_fps) ? macro_fps : 240.f;
		auto after = replay::rate_after(rates, frame);
		while (after != rates.begin()) {
			auto const& change = *--after;
			if (std::isfinite(change.fps) && change.fps >= 1.f) {
				rate = change.fps;
				break;
			}
		}
		return std::max(rate, 1.f);
	}

	double seconds_at(int frame) const {
		if (frame <= 0)
			return 0.0;
		double time = 0.0;
		int at = 0;
		float rate = macro_fps;
		auto add = [&](int upto) {
			if (upto <= at)
				return;
			time += static_cast<double>(upto - at) / static_cast<double>(std::max(rate, 1.f));
			at = upto;
		};
		for (auto const& change : rates) {
			if (change.frame >= frame)
				break;
			if (change.frame > at)
				add(change.frame);
			rate = change.fps;
		}
		add(frame);
		return time;
	}
};

class Logic : public Session {
public:
	static constexpr int MAX_PER_SECOND = 16, MAX_PER_FRAME = 3;
	static constexpr float MAX_BURST_RATE = 48.f;
	static constexpr int MIN_BURST_CLICKS = 5;

	static Logic& get();
	static Logic& live();
	static void set_loading(Logic* session);
	const char* state_name(RunState state);
	void boot();
	void pull_settings();
	void push_runtime_settings();
	void save_settings_file();
	int get_frame(PlayLayer* play);
	void toggle_recording();
	void toggle_playing();
	void toggle_frame_advance();
	void advance_one_frame();
	void poll_advance_hold();
	int follow_steps() const;
	bool follow_active(GJBaseGameLayer* play) const;
	void note_follow_press(GJBaseGameLayer* play, bool down, int button);
	bool follow_due(FollowPress const& press, int frame, double time) const;
	void rewind_follow(GJBaseGameLayer* play);
	void run_follow(GJBaseGameLayer* play);
	void request_best_frame();
	void stop_best_frame(char const* why = nullptr);
	void find_best_frame(PlayLayer* play, std::function<void(float)> const& tick);
	void click_best_frame(PlayLayer* play);
	Checkpoint capture_checkpoint(PlayLayer* play, CheckpointObject* object, int frame);
	void disarm_length_pause_if_holding();
	void check_advance_pause(PlayLayer* play);
	void on_keybind(std::string_view key, bool down);
	void tick_autoclicker(PlayLayer* play);
	void after_frame(PlayLayer* play, float dt);
	bool player_hidden(PlayerObject* player) const;
	bool hitbox_player_hidden(bool player2) const;
	bool traj_player_hidden(bool player2) const;
	void conceal_player(PlayerObject* player, bool hide);
	void play_macro(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt, bool clearInputQueue);
	void prepare_buttons(GJBaseGameLayer* play);
	bool cbf_enabled() const;
	bool vanilla_click_physics() const;
	bool defer_physics() const;
	double click_stamp() const;
	void remember_move(PlayerButtonCommand const& cmd);
	void restore_holds(PlayLayer* play);
	void on_play_init(PlayLayer* play);
	void onReset(PlayLayer* play);
	void plant_lvl_seed(PlayLayer* play);
	void apply_attempt_variance(PlayLayer* play, uint64_t seed);
	void keep_variance(PlayLayer* play);
	void roll_live_seed(PlayLayer* play);
	void restore_checkpoint_random(PlayLayer* play);
	void note_rand_trigger(int unique_id, uint64_t* state);
	void reseed_rand_triggers(uint64_t seed);
	void clear_rand_triggers();
	uint64_t game_random_state();
	void set_game_random_state(uint64_t state);
	void capture_restart_holds(PlayLayer* play);
	void match_live_holds(PlayLayer* play);
	void poll_hold_block();
	void clear_stuck_death(PlayLayer* play);
	void save_checkpoint(PlayLayer* play, CheckpointObject* object);
	CheckpointObject* object_capture = nullptr;
	CheckpointObject* object_restore = nullptr;
	void sync_checkpoints(PlayLayer* play);
	Checkpoint const* get_latest_checkpoint(PlayLayer* play);
	void on_complete(PlayLayer* play);
	void onQuit(PlayLayer* play);
	bool should_noclip(PlayLayer* play, PlayerObject* player);
	bool steer_player(bool& isPlayer1);
	PlayerObject* icon_body(GJBaseGameLayer* play, bool icon2);
	bool play_input(GJBaseGameLayer* play, bool icon2, bool down, int button);
	bool physical_hold(GJBaseGameLayer* play, bool icon2);
	bool steer_key(GJBaseGameLayer* play, enumKeyCodes key, bool down, double timestamp);
	void set_routed_hold(bool icon2, int button, bool down);
	void reinforce_routed_hold(PlayerObject* body);
	void apply_maintain_gravity(GJBaseGameLayer* play);
	void sync_same_gravity(GJBaseGameLayer* play);
	void offset_frames(int delta, int first = -1, int last = -1);
	void offset_inputs(int magnitude, int first = -1, int last = -1);
	void remove_player_inputs(bool player2);
	void flip_input_holds();
	void flip_input_players();
	void sort_inputs();
	void identify_inputs(bool force = false);
	uint64_t mint_input_id();
	void clear_inputs();
	bool dispatch_setting_key(std::string_view key, bool down);
	fs::path save_root();
	void notify(std::string const& text, bool success);
	bool write_file();
	bool autosave_replay();
	void poll_autosave();
	bool read_file(fs::path const& path);
	bool export_kind(std::string_view kind);
	bool append_sequence_file(fs::path const& path, int amount);
	bool merge_replay(fs::path const& path);
	void pick_replay();
	void import_replays();
	int convert_legacy_echoes();
	unsigned count_presses_in_last_second(bool player2);
	void note_click(PlayLayer* play, bool down, bool player1, int button = static_cast<int>(PlayerButton::Jump));
	void update_cps(PlayLayer* play);
	bool burst_too_fast(Session const& value, int clicks, int first, int last) const;
	bool player_cps_broken(std::deque<int> const& clicks, int now, float max_per_second) const;
	void set_tps(float rate);
	void place_rate(int frame, float rate);
	void apply_rate(int frame);
	void normalize_rates();
	void trim_rates(int frame);
	void sync_time_scale();
	void sync_playback_fps();
	int unlocked_step_cap() const;
	void on_focus_change();
	void resume_attempt(PlayLayer* play);
	void retain_playback_hold(PlayLayer* play);
	void play_click(PlayLayer* play, bool down, bool player1);
	void flush_clicks();
	void note_step();
	void release_replay_index();
	int max_steps_for_frame() const;
	void note_frame_work(double seconds);
	double frame_work = 0.0;
	void draw_overlay(PlayLayer* play);
	void sync_death_label(PlayLayer* play);
	void apply_layout(PlayLayer* play);
	void draw_layout_art(PlayLayer* play);
	static CCRect object_hitbox(GameObject* object);
	void clear_layout_hidden();
	bool layout_active();
	void paint_layout_colors(PlayLayer* play);
	void paint_layout_channels(GJEffectManager* effects);
	void style_layout_object(GameObject* object);
	void style_layout_glow(GameObject* object);
	void draw_hitboxes(GJBaseGameLayer* play);
	void seat_overlay(CCNode* node, GJBaseGameLayer* play);
	void sync_overlay_seats(GJBaseGameLayer* play);
	void add_trail_point(GJBaseGameLayer* play);
	void note_click_hitbox(GJBaseGameLayer* play, bool player1, bool down);
	void capture_pending_click(PlayerObject* player);
	void flush_click_hitboxes(GJBaseGameLayer* play);
	void clear_click_hitbox();
	void prepare_lvl_view(PlayLayer* play);
	void begin_editor_playtest();
	void end_editor_playtest(GJBaseGameLayer* editor);
	bool render_active();
	bool render_capturing();
	bool render_paused();
	bool pause_menu_open();
	bool render_should_skip();
	bool render_holds_view();
	bool render_uses_screen_shape();
	void sync_render_pause_button();
	int render_width_for(int width, int height);
	bool render_blocks_engine();
	bool render_blocks_mixer();
	float render_step();
	float render_speed();
	float scroll_speed_factor();
	float render_game_step();
	float render_frame_game_step = 0.f;
	bool begin_render_frame();
	bool capture_frame();
	bool render_batch_more(chrono::steady_clock::time_point started);
	void hold_render_effects();
	void show_render_overlay();
	RenderStatus render_status();
	float render_wall_delta();
	void sync_music_pitch();
	void sync_audio_clock();
	void reset_opening_music_hold();
	bool ffmpeg_ready();
	std::string render_file_stem() const;
	void start_render();
	void stop_render();
	void merge_renders(fs::path const& first, fs::path const& second);
	float merge_progress();
	void reset_traj();
	bool is_traj_player(CCObject* object);
	void sync_predict_hooks();
	bool advance_paused(GJBaseGameLayer* play);
	bool lock_delta_active() const;
	int count_unlocked_steps(GJBaseGameLayer* play, double dt);
	void run_frame(GJBaseGameLayer* play, float dt, std::function<void(float)> const& tick);
	void cancel_steps();
	void flush_visual_refresh();
	void recompute_cps(Session& value);

	std::string read_folder_setting(char const* key);
	matjson::Value placement_json(LabelPlacement const& placement);
	LabelPlacement read_placement(matjson::Value const& json, LabelPlacement fallback);
	FramePose capture_pose(PlayerObject* player);
	bool flip_controls();
	void set_replay_pos(PlayLayer* play);
	void trim_recorded_inputs(Session& value, int frame);
	void trim_fresh_attempt(PlayLayer* play, Session& value);
	PlayerSnapshot capture_player(PlayerObject* player);
	void apply_player(PlayerObject* player, PlayerSnapshot const& shot);
	LayerSnapshot capture_layer(PlayLayer* play);
	void apply_layer(PlayLayer* play, LayerSnapshot const& shot);
	void trim_from_checkpoint(PlayLayer* play, Session& value, Checkpoint const& checkpoint);
	void clear_physical_holds();
	bool held_at(Session const& value, bool player2, int frame, int button = static_cast<int>(PlayerButton::Jump));
	std::pair<int, int> count_holds(Session const& value);
	bool inputs_sorted(Session const& value);

	// pressing a direction clears the other one even if its still held :/
	static bool button_held(PlayerObject* body, int button) {
		if (!body)
			return false;
		auto found = body->m_holdingButtons.find(button);
		bool mapped = found != body->m_holdingButtons.end() && found->second;
		if (button == static_cast<int>(PlayerButton::Left))
			return mapped || body->m_holdingLeft;
		if (button == static_cast<int>(PlayerButton::Right))
			return mapped || body->m_holdingRight;
		return mapped;
	}

	static PlayLayer* as_play(GJBaseGameLayer* layer);
	static LevelEditorLayer* as_editor(GJBaseGameLayer* layer);
	size_t add_input(Session& value, Frame const& frame);
	void refresh_live_hold();
	void record_input(GJBaseGameLayer* play, bool down, bool player1, int button = static_cast<int>(PlayerButton::Jump), bool force = false);
	void inject_button(PlayLayer* play, bool down, int button, bool player1);
	void note_rule(std::vector<CpsNote>& notes, int first, int last, std::string text);
	void eval_player_cps(std::vector<int> const& presses, bool player2, Session& value);

	static float read_float(char const* key, float fallback) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key))
			return fallback;
		return mod->getSettingValue<float>(key);
	}

	static bool read_bool(char const* key, bool fallback) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key))
			return fallback;
		return mod->getSettingValue<bool>(key);
	}

	static int read_int(char const* key, int fallback) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key))
			return fallback;
		return static_cast<int>(mod->getSettingValue<int64_t>(key));
	}

	static void write_bool(char const* key, bool value) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key))
			return;
		mod->setSettingValue<bool>(key, value);
	}

	static void write_float(char const* key, float value) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key))
			return;
		mod->setSettingValue<float>(key, value);
	}

	static void write_int(char const* key, int value) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key))
			return;
		mod->setSettingValue<int64_t>(key, static_cast<int64_t>(value));
	}
};
