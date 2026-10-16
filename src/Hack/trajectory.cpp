#include "Logic/perf.hpp"
#include "Hack/trajectory.hpp"
#include "Hack/predicted_world.hpp"
#include "Hack/orbs.hpp"
#include "Logic/logic.hpp"
#include "Physics/checkpoint.hpp"
#include "Physics/collisions.hpp"
#include "Physics/object.hpp"
#include "Physics/prediction_art.hpp"
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GJGameState.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/UILayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

using namespace geode::prelude;

namespace chrono = std::chrono;

#define GRAVITY_TRIGGER 2066
#define ROTATE_GAMEPLAY_TRIGGER 2900
#define TELEPORT_TRIGGER 3022
#define WORLD_REACH 360.f
#define EXTEND_BUDGET_SECONDS 0.006
#define PLAYER_TOLERANCE 0.3f
#define VELOCITY_TOLERANCE 0.01f

static int prediction_steps(float seconds, float rate, float warp) {
	double count = std::ceil(seconds * rate / std::clamp(warp, 0.01f, 1.f));
	if (!std::isfinite(count)) return 1;
	return (int)std::clamp(count, 1., double(std::numeric_limits<int>::max() - 1));
}

template <class Emit>
static void draw_path(const std::vector<CCPoint>& points, Emit emit) {
	if (points.size() < 2) return;
	CCPoint start = points.front();
	CCPoint end = start;
	for (size_t i = 1; i < points.size(); ++i) {
		CCPoint next = points[i];
		double ax = double(end.x) - start.x, ay = double(end.y) - start.y;
		double bx = double(next.x) - end.x, by = double(next.y) - end.y;
		if (ax * by == ay * bx && ax * bx + ay * by >= 0.) end = next;
		else {
			emit(start, end);
			start = end;
			end = next;
		}
	}
	emit(start, end);
}

static bool jump_held(PlayerObject* body) {
	return body && Logic::button_held(body, JUMP);
}

void Trajectory::traj_log(const std::string& line) {
	auto& logic = Logic::get();
	if (!logic.trace_logging) return;
	auto* play = PlayLayer::get();
	if (!log_file.is_open() || log_play != play) {
		log_file.close();
		log_file.open(logic.save_root() / "logs" / "trajectory.txt", std::ios::trunc);
		log_play = play;
	}
	log_file << line << std::endl;
}

std::string Trajectory::ring_report(PlayLayer* play, PlayerObject* player) {
	std::string out;
	if (!player) return out;
	CCPoint at = player->getPosition();
	auto describe = [&](GameObject* object, const char* where) {
		if (!object) return;
		int type = (int)object->m_objectType;
		bool ring = type >= (int)GameObjectType::YellowJumpRing && type <= (int)GameObjectType::TeleportOrb;
		if (!ring && type != (int)GameObjectType::CustomRing && type != (int)GameObjectType::DashRing && type != (int)GameObjectType::GravityDashRing && type != (int)GameObjectType::GreenRing && type != (int)GameObjectType::PinkJumpRing && type != (int)GameObjectType::RedJumpRing && type != (int)GameObjectType::DropRing && type != (int)GameObjectType::GravityRing && type != (int)GameObjectType::SpiderOrb) return;
		if (std::fabs((float)object->m_positionX - at.x) > 60.f || std::fabs((float)object->m_positionY - at.y) > 60.f) return;
		out += fmt::format(" [{} uid={} id={} type={} pos=({:.2f},{:.2f}) extended={} group_off={} off={} act1={} act2={}]", where, object->m_uniqueID, object->m_objectID, type, object->m_positionX, object->m_positionY,
			object->m_hasExtendedCollision, object->m_isGroupDisabled, object->m_isDisabled, static_cast<EnhancedGameObject*>(object)->m_activatedByPlayer1, static_cast<EnhancedGameObject*>(object)->m_activatedByPlayer2);
	};
	int amount = std::min(play->m_calcNonEffectObjectsSize, (int)play->m_calcNonEffectObjects.size());
	for (int i = 0; i < amount; ++i)
		describe(play->m_calcNonEffectObjects[(size_t)i], "calc");
	float factor_x = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : 0.01f, factor_y = play->m_sectionYFactor > 0.f ? play->m_sectionYFactor : 0.01f;
	int cx = (int)(at.x * factor_x), cy = (int)(at.y * factor_y);
	auto& grid = play->m_nonEffectObjects;
	auto& sizes = play->m_nonEffectObjectsSizes;
	for (int x = cx - 1; x <= cx + 1; ++x) {
		if (x < 0 || (size_t)x >= grid.size() || !grid[(size_t)x] || (size_t)x >= sizes.size() || !sizes[(size_t)x]) continue;
		for (int y = cy - 1; y <= cy + 1; ++y) {
			if (y < 0 || (size_t)y >= grid[(size_t)x]->size() || (size_t)y >= sizes[(size_t)x]->size()) continue;
			auto* bucket = (*grid[(size_t)x])[(size_t)y];
			if (!bucket) continue;
			int used = std::min((*sizes[(size_t)x])[(size_t)y], (int)bucket->size());
			for (int i = 0; i < used; ++i)
				describe((*bucket)[(size_t)i], "grid");
		}
	}
	return out;
}

void Trajectory::note_live_step(PlayLayer* play) {
	auto& logic = Logic::get();
	if (!watch.valid || !logic.trace_logging || !play) return;
	int now = logic.step_frame;
	int index = now - watch.start_step - 1;
	if (index < 0 || (size_t)index >= watch.samples.size()) return;
	auto* body = watch.player1 ? play->m_player1 : play->m_player2;
	if (!body) return;
	const auto& sample = watch.samples[(size_t)index];
	CCPoint at = body->getPosition();
	float dx = at.x - sample.x, dy = at.y - sample.y, dv = (float)body->m_yVelocity - sample.velocity;
	bool milestone = index < 4 || (index & (index + 1)) == 0;
	if (milestone)
		traj_log(fmt::format("DRIFT frame={} index={} dx={:.4f} dy={:.4f} dvel={:.4f} held={} upside_down={} ground={}", now, index, dx, dy, dv,
			Logic::button_held(body, JUMP), body->m_isUpsideDown, body->m_isOnGround));
	if (watch.reported) return;
	if (std::fabs(dx) <= PLAYER_TOLERANCE && std::fabs(dy) <= PLAYER_TOLERANCE && std::fabs(dv) <= VELOCITY_TOLERANCE) return;
	watch.reported = true;
	auto& world = PredictedWorld::get();
	traj_log(fmt::format("DIVERGE frame={} index={} predicted=({:.4f},{:.4f} vel {:.4f}) live=({:.4f},{:.4f} vel {:.4f}) held={} buffered={} upside_down={} ground={} dart={} ship={} dead={} real_dt={:.6f} tps={:.2f} warp={:.3f} world_horizon={} world_expected_step={} tracked={} near:{}",
		now, index, sample.x, sample.y, sample.velocity, at.x, at.y, body->m_yVelocity, Logic::button_held(body, JUMP), body->m_jumpBuffered, body->m_isUpsideDown,
		body->m_isOnGround, body->m_isDart, body->m_isShip, body->m_isDead, real_step_delta, logic.sample_rate(), play->m_gameState.m_timeWarp, world.horizon(),
		world.expected_step(), world.tracked_count(), world.near_report(at, 200.f)));
}

static bool sample_matches(const Trajectory::Sample& sample, PlayerObject* body) {
	if (!body) return false;
	CCPoint at = body->getPosition();
	return std::fabs(at.x - sample.x) <= PLAYER_TOLERANCE && std::fabs(at.y - sample.y) <= PLAYER_TOLERANCE &&
		std::fabs((float)body->m_yVelocity - sample.velocity) <= VELOCITY_TOLERANCE;
}

void Trajectory::park_fake(PlayerObject* player) {
	if (!player) return;
	player->setVisible(false);
	player->m_isHidden = true;
	player->stopAllActions();
	player->unscheduleUpdate();
	// if i leave the fake here the real player hits it next step xd
	player->setPosition({ -100000.f, -100000.f });
	player->m_position = CCPoint{ -100000.f, -100000.f };
	if (auto* play = PlayLayer::get()) play->removeObjectFromSection(player);
}

void Trajectory::park_fakes(bool keep_kept) {
	for (int i = 0; i < FAKE_PAIRS * 2; ++i) {
		if (keep_kept && kept_valid && i / 2 == KEPT_PAIR) continue;
		park_fake(fakes[i]);
	}
}

PlayerObject* Trajectory::spawn_fake(PlayLayer* play, const char* id) {
	auto* player = PlayerObject::create(1, 1, play, play, true);
	player->retain();
	player->setID(id);
	player->setVisible(false);
	player->m_isHidden = true;
	player->setOpacity(0);
	player->stopAllActions();
	player->unscheduleUpdate();
	player->setPosition({ -100000.f, -100000.f });
	if (player->m_iconSprite) player->m_iconSprite->setVisible(false);
	if (player->m_iconSpriteSecondary) player->m_iconSpriteSecondary->setVisible(false);
	if (player->m_iconSpriteWhitener) player->m_iconSpriteWhitener->setVisible(false);
	player->removeFromParentAndCleanup(false);
	park_fake(player);
	return player;
}

void Trajectory::attach(PlayLayer* play) {
	detach();
	if (!play || !play->m_objectLayer) return;
	draw_node = CCDrawNode::create();
	draw_node->retain();
	draw_node->m_bUseArea = false;
	draw_node->setID("trajectory"_spr);
	draw_node->setBlendFunc({ GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA });
	Logic::get().attach_overlay(draw_node, play);
	for (int i = 0; i < FAKE_PAIRS * 2; ++i)
		fakes[i] = spawn_fake(play, fmt::format("trajectory-player-{}"_spr, i + 1).c_str());
}

void Trajectory::sync_seat(PlayLayer* play) {
	if (!draw_node || !play) return;
	Logic::get().attach_overlay(draw_node, play);
}

void Trajectory::invalidate() {
	drawing_valid = false;
	watch.valid = false;
	cancel_job();
	kept_valid = false;
	PredictedWorld::get().invalidate();
}

void Trajectory::detach() {
	invalidate();
	auto drop = [](auto*& object) {
		if (!object) return;
		object->stopAllActions();
		object->unscheduleAllSelectors();
		object->removeFromParentAndCleanup(true);
		object->release();
		object = nullptr;
	};
	for (int i = 0; i < FAKE_PAIRS * 2; ++i) {
		drop(fakes[i]);
		activated[i].clear();
	}
	drop(draw_node);
	simulating = false;
	real_step_delta = 0.f;
	Logic::get().predicting = false;
	Logic::get().sync_predict_hooks();
}

Trajectory::Line* Trajectory::line_of(PlayerObject* player) {
	if (!job.active || !player) return nullptr;
	if (job.use_kept && (kept.fake == player || kept.partner == player)) return &kept;
	for (auto& line : job.lines)
		if (line.fake == player || line.partner == player) return &line;
	return nullptr;
}

bool Trajectory::mark_dead(PlayerObject* player) {
	int index = fake_index(player);
	if (index < 0) return false;
	fake_dead[index] = true;
	return true;
}

bool Trajectory::mark_predict_death(PlayerObject* player) {
	if (mark_dead(player)) return true;
	return mark_dead(colliding_fake);
}

bool Trajectory::fakeActivated(PlayerObject* player, EnhancedGameObject* object) const {
	if (!object || object->m_isMultiActivate) return false;
	int index = fake_index(player);
	if (index < 0) return false;
	const auto& list = activated[index];
	return std::find(list.begin(), list.end(), object) != list.end();
}

bool Trajectory::realActivated(PlayerObject* player, EnhancedGameObject* object) const {
	if (!object) return false;
	auto* layer = GJBaseGameLayer::get();
	PlayerObject* real = player;
	int index = fake_index(player);
	if (layer && index >= 0) real = (index & 1) ? layer->m_player2 : layer->m_player1;
	return phys::hasBeenActivatedByPlayer(real, object);
}

void Trajectory::rememberActivation(EnhancedGameObject* object, PlayerObject* player) {
	int index = fake_index(player);
	if (index >= 0) activated[index].push_back(object);
}

ccColor4F Trajectory::path_color(bool icon2, ClickType click) const {
	const auto& logic = Logic::get();
	const float* rgba = logic.traj_color;
	if (click == ClickType::Swift) rgba = logic.traj_swift_color;
	else if (click == ClickType::Release) rgba = logic.traj_release_color;
	if (icon2) return { 1.f - rgba[0], 1.f - rgba[1], 1.f - rgba[2], rgba[3] };
	return { rgba[0], rgba[1], rgba[2], rgba[3] };
}

float Trajectory::line_width(PlayLayer* play) const {
	return Logic::get().traj_width / std::max(play->m_gameState.m_cameraZoom, 0.05f);
}

Trajectory::CacheKey Trajectory::make_cache_key(PlayLayer* play) const {
	const auto& logic = Logic::get();
	CacheKey key;
	key.frame = Logic::get().get_frame(play);
	auto fill_player_state = [](PlayerObject* player, float* out) {
		if (!player) return;
		out[0] = player->m_position.x;
		out[1] = player->m_position.y;
		out[2] = player->getRotation();
		out[3] = (float)player->m_yVelocity;
		out[4] = (float)player->m_platformerXVelocity;
		out[5] = (float)((player->m_holdingLeft ? 1 : 0) | (player->m_holdingRight ? 2 : 0) | (player->m_leftPressedFirst ? 4 : 0));
	};
	fill_player_state(play->m_player1, key.player1_state);
	fill_player_state(play->m_player2, key.player2_state);
	key.dual = play->m_gameState.m_isDualMode;
	if (logic.maintain_gravity) {
		key.held_p1 = Logic::get().physical_hold(play, false);
		key.held_p2 = Logic::get().physical_hold(play, true);
	}
	else {
		key.held_p1 = jump_held(play->m_player1);
		key.held_p2 = jump_held(play->m_player2);
	}
	key.dead = play->m_playerDied;
	key.playing = logic.is_playing();
	key.recording = logic.is_recording();
	key.hold = logic.traj_hold;
	key.swift = logic.traj_swift;
	key.release = logic.traj_release;
	key.dash_orbs = Orbs::click_orb_assist(false) || Orbs::click_orb_assist(true);
	key.black_orbs = key.dash_orbs && logic.black_orb_assist();
	key.pad_black = logic.pad_black_orb;
	key.pad_blue = logic.pad_blue_orb;
	key.pause_kill = logic.pause_before_kill && logic.is_recording() && logic.show_traj;
	key.pause_length = logic.pause_at_traj_length;
	key.pause_ms = logic.pause_traj_ms;
	key.hide_p1 = Logic::get().traj_player_hidden(false);
	key.hide_p2 = Logic::get().traj_player_hidden(true);
	key.orbs = Orbs::orb_spots(play, play->m_player1);
	if (play->m_player2) {
		auto spots = Orbs::orb_spots(play, play->m_player2);
		key.orbs.insert(key.orbs.end(), spots.begin(), spots.end());
	}
	key.seconds = logic.traj_seconds;
	key.width = logic.traj_width;
	key.zoom = play->m_gameState.m_cameraZoom;
	std::copy(std::begin(logic.traj_color), std::end(logic.traj_color), key.color);
	std::copy(std::begin(logic.traj_swift_color), std::end(logic.traj_swift_color), key.swift_color);
	std::copy(std::begin(logic.traj_release_color), std::end(logic.traj_release_color), key.release_color);
	return key;
}

// w silly cat skid
void Trajectory::copy_real_into_fake(PlayerObject* fake, PlayerObject* real) {
	auto logs = std::array{ fake->m_collisionLogTop, fake->m_collisionLogBottom, fake->m_collisionLogLeft, fake->m_collisionLogRight };
	auto* wave = fake->m_waveTrail;
	auto* regular = fake->m_regularTrail;
	auto* ghost = fake->m_ghostTrail;
	auto* rings = fake->m_touchingRings;
	// mod 0 crashes on windows
	auto own_streak_type = fake->m_shipStreakType;
	fake->copyAttributes(real);
	if (rings && fake->m_touchingRings != rings) fake->m_touchingRings = rings;
	fake->m_lastPosition = real->m_lastPosition;
	SavedPlayerCheckpoint checkpoint = SavedPlayerCheckpoint::create(real);
	checkpoint.apply(fake);
	fake->m_collisionLogTop = logs[0];
	fake->m_collisionLogBottom = logs[1];
	fake->m_collisionLogLeft = logs[2];
	fake->m_collisionLogRight = logs[3];
	fake->m_waveTrail = wave;
	fake->m_regularTrail = regular;
	fake->m_ghostTrail = ghost;
	fake->m_shipStreakType = own_streak_type;
	fake->m_pendingCheckpoint = nullptr;
	fake->m_maybeReducedEffects = real->m_maybeReducedEffects;
	fake->m_isHidden = true;
	fake->m_playEffects = false;
	fake->setPosition(real->m_position);
	fake->setRotation(real->getRotation());
	fake->m_isObjectRectDirty = true;
	fake->updateOrientedBox();
}

void Trajectory::dualPortal(GJBaseGameLayer* layer, PlayerObject* player, EffectGameObject* portal, bool on) {
	auto& state = layer->m_gameState;
	auto* line = line_of(player);
	if (!line || state.m_isDualMode == on) return;
	PlayerObject* first = line->player1 ? line->fake : line->partner;
	PlayerObject* second = line->player1 ? line->partner : line->fake;
	bool twoPlayer = layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode;
	auto with_fakes = [&](auto&& action) {
		PlayerObject* real1 = layer->m_player1;
		PlayerObject* real2 = layer->m_player2;
		layer->m_player1 = first;
		layer->m_player2 = second;
		action();
		layer->m_player1 = real1;
		layer->m_player2 = real2;
	};
	if (on) {
		if (player != first || line->running_pair) return;
		state.m_isDualMode = true;
		first->m_unkA99 = true;
		if (line->partner_real) copy_real_into_fake(second, line->partner_real);
		second->m_unkA99 = true;
		second->resetObject();
		second->copyAttributes(first);
		bool unlinked = state.m_unlinkDualGravity;
		second->flipGravity(unlinked ? first->m_isUpsideDown : !first->m_isUpsideDown, true);
		second->setYVelocity(unlinked ? first->m_yVelocity : -first->m_yVelocity, 0);
		second->m_isOnGround = false;
		second->m_isOnGround2 = false;
		if (twoPlayer) {
			if (!second->m_inputsLocked) second->m_holdingButtons[JUMP] = false;
			if (!second->m_controlsDisabled) {
				second->m_jumpBuffered = false;
				second->m_stateRingJump = false;
				second->m_touchedPad = true;
				if (second->m_isDashing) second->stopDashing();
			}
		}
		second->setVisible(false);
		second->m_isHidden = true;
		second->m_playEffects = false;
		second->m_isObjectRectDirty = true;
		state.m_lastActivatedPortal2 = portal;
		with_fakes([&] {
			layer->playerWillSwitchMode(player, portal);
		});
		line->dual_started = line->can_start_dual;
		return;
	}
	state.m_isDualMode = false;
	first->m_unkA99 = false;
	second->m_unkA99 = false;
	if (player == second) first->copyAttributes(second);
	second->releaseAllButtons();
	state.m_lastActivatedPortal2 = nullptr;
	int mode = 6;
	if (first->m_isShip || first->m_isBird || first->m_isDart || first->m_isSwing) mode = 5;
	else if (first->m_isBall) mode = 16;
	else if (first->m_isSpider) mode = 33;
	with_fakes([&] {
		layer->updateDualGround(player, mode, false, 0.f);
	});
	line->dual_ended = true;
}

void Trajectory::press_fake(PlayerObject* fake, ClickType click, bool icon2) {
	auto& logic = Logic::get();
	bool stash_orbs = Orbs::click_orb_assist(icon2);
	if (stash_orbs) logic.suppress_rings = true;
	switch (click) {
	case ClickType::Replay:
		break;
	case ClickType::Hold:
		fake->pushButton(PlayerButton::Jump);
		break;
	case ClickType::Swift:
		fake->releaseButton(PlayerButton::Jump);
		fake->m_jumpBuffered = false;
		fake->pushButton(PlayerButton::Jump);
		fake->releaseButton(PlayerButton::Jump);
		break;
	case ClickType::Release:
		fake->releaseButton(PlayerButton::Jump);
		fake->m_jumpBuffered = false;
		break;
	}
	if (stash_orbs) logic.suppress_rings = false;
}

ClickType Trajectory::next_click(PlayLayer* play, bool player1) const {
	const auto& logic = Logic::get();
	PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
	if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart)
		return Logic::get().physical_hold(play, !player1) ? ClickType::Hold : ClickType::Release;
	for (auto it = play->m_queuedButtons.rbegin(); it != play->m_queuedButtons.rend(); ++it) {
		int button = (int)it->m_button;
		if ((button == 0 || button == JUMP) && it->m_isPlayer2 != player1)
			return it->m_isPush ? ClickType::Hold : ClickType::Release;
	}
	return jump_held(body) ? ClickType::Hold : ClickType::Release;
}

ClickType Trajectory::actual_click(PlayLayer* play, bool player1) const {
	const auto& logic = Logic::get();
	if (logic.plays_player(!player1)) return ClickType::Replay;
	PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
	if (logic.maintain_gravity && logic.records_player(!player1) && body && body->m_isDart)
		return Logic::get().physical_hold(play, !player1) ? ClickType::Hold : ClickType::Release;
	return jump_held(body) ? ClickType::Hold : ClickType::Release;
}

Trajectory::Line Trajectory::make_line(PlayLayer* play, bool player1, ClickType click, bool with_partner, bool draw, int measure, int pair) {
	Line line;
	line.click = click;
	line.player1 = player1;
	line.with_partner = with_partner;
	line.draw = draw;
	line.measure = measure;
	line.pair = pair;
	line.fake = fakes[pair * 2 + (player1 ? 0 : 1)];
	line.partner = fakes[pair * 2 + (player1 ? 1 : 0)];
	line.real = player1 ? play->m_player1 : play->m_player2;
	line.partner_real = player1 ? play->m_player2 : play->m_player1;
	return line;
}

void Trajectory::add_click_lines(PlayLayer* play, std::vector<Line>& lines, bool player1, bool with_partner, bool draw, bool rendering) {
	const auto& logic = Logic::get();
	ClickType actual = actual_click(play, player1);
	int pair = 0;
	for (const auto& line : lines)
		if (line.player1 == player1) ++pair;
	auto add = [&](ClickType click, bool show) {
		int measure = 0;
		if (click == actual) measure |= MEASURE_LIVE;
		if (logic.pause_at_traj_length && click == ClickType::Hold) measure |= MEASURE_HOLD;
		if (logic.pause_at_traj_length && click == ClickType::Release) measure |= MEASURE_RELEASE;
		if (!show && measure == 0) return;
		if (pair >= KEPT_PAIR) return;
		lines.push_back(make_line(play, player1, click, with_partner, show, measure, pair++));
	};
	if (play->m_playerDied || rendering) {
		add(actual, draw);
		return;
	}
	bool show_hold = draw && logic.traj_hold, show_swift = draw && logic.traj_swift, show_release = draw && logic.traj_release;
	bool need_hold = show_hold || logic.pause_at_traj_length, need_release = show_release || logic.pause_at_traj_length;
	if (need_hold) add(ClickType::Hold, show_hold);
	if (show_swift) add(ClickType::Swift, true);
	if (need_release) add(ClickType::Release, show_release);
	bool covered = (actual == ClickType::Hold && need_hold) || (actual == ClickType::Swift && show_swift) || (actual == ClickType::Release && need_release);
	if (!covered) add(actual, draw);
}

bool Trajectory::kept_matches(PlayLayer* play, bool linked, std::string& reason) {
	if (!kept_valid) {
		reason = "no kept line";
		return false;
	}
	if (kept_play != play) {
		reason = "other level";
		return false;
	}
	if (kept.click != actual_click(play, true)) {
		reason = fmt::format("click changed {}->{}", (int)kept.click, (int)actual_click(play, true));
		return false;
	}
	if (kept.with_partner != linked) {
		reason = "dual changed";
		return false;
	}
	int now = Logic::get().step_frame;
	if (now < kept.start_step) {
		reason = "stepped back";
		return false;
	}
	int index = now - kept.start_step - 1;
	if (index < 0) return true;
	if ((size_t)index >= kept.samples.size()) {
		reason = fmt::format("kept line too short ({} of {})", kept.samples.size(), index + 1);
		return false;
	}
	auto describe = [&](const Sample& sample, PlayerObject* body, const char* who) {
		CCPoint at = body->getPosition();
		reason = fmt::format("{} off at index {}: dx={:.4f} dy={:.4f} dvel={:.4f} dashing {}->{} ground {}->{} upside_down {}->{} ring={} buffered={}", who, index, at.x - sample.x, at.y - sample.y,
			(float)body->m_yVelocity - sample.velocity, sample.dashing, body->m_isDashing, sample.ground, body->m_isOnGround, sample.upside_down, body->m_isUpsideDown,
			body->m_dashRing ? body->m_dashRing->m_uniqueID : 0, body->m_jumpBuffered);
	};
	if (!sample_matches(kept.samples[(size_t)index], play->m_player1)) {
		describe(kept.samples[(size_t)index], play->m_player1, "p1");
		return false;
	}
	if (kept.with_partner) {
		if ((size_t)index >= kept.partner_samples.size() || !sample_matches(kept.partner_samples[(size_t)index], play->m_player2)) {
			if ((size_t)index < kept.partner_samples.size()) describe(kept.partner_samples[(size_t)index], play->m_player2, "p2");
			else reason = "kept partner line too short";
			return false;
		}
	}
	return true;
}

bool Trajectory::begin_job(PlayLayer* play, std::vector<Line> lines, int limit, bool draw, bool sliced) {
	cancel_job();
	if (!play->m_objectLayer || lines.empty()) return false;
	auto& logic = Logic::get();
	auto& world = PredictedWorld::get();
	job.~Job();
	new (&job) Job();
	job.play = play;
	job.frame = logic.step_frame;
	job.now = logic.step_frame;
	job.draw = draw;
	job.sliced = sliced;
	job.lines = std::move(lines);
	job.started = chrono::steady_clock::now();
	for (auto* fake : fakes)
		if (!fake->getParent()) play->m_objectLayer->addChild(fake);
	phys::prepare_prediction_art();
	job.warp = play->m_gameState.m_timeWarp;
	job.step_seconds = logic.physics_step();
	// a macro can change tps part way so each step gets its own length (NOT the last real frames)
	job.varying_tps = logic.is_playing() && !logic.rates.empty();
	if (job.varying_tps) job.step_seconds = 1.f / std::max(logic.rate_at(logic.get_frame(play)), 1.f);
	job.step_delta = job.step_seconds * 60.f * std::min(job.warp, 1.f);
	if (!job.varying_tps && real_step_delta > 0.f && std::fabs(real_step_delta - job.step_delta) < job.step_delta * 0.01f) job.step_delta = real_step_delta;
	job.steps = prediction_steps(logic.traj_seconds, (float)logic.sample_rate(), job.warp);
	if (limit > 0) job.steps = std::min(job.steps, limit);
	perf_count_traj_steps(job.steps);
	job.steps_per_frame = std::max(1, (int)std::lround(logic.sample_rate() / 60.0));
	job.use_world = logic.traj_moving_objects && job.steps > 1 && !job.varying_tps;
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool linked = play->m_gameState.m_isDualMode && play->m_player2 && !twoPlayer;
	job.active = true;
	enter_prediction();
	Orbs::reset_prediction_pads();
	if (job.use_world) {
		std::string reason;
		bool reuse = kept_matches(play, linked, reason);
		if (reuse) {
			world.trim(job.now);
			reuse = world.matches_live(play, job.now);
			if (!reuse) reason = world.mismatch;
		}
		if (!reuse) {
			world.rebuild(play, job.step_delta, job.now);
			if (logic.trace_logging)
				traj_log(fmt::format("REBUILD frame={} reason=\"{}\" steps={} dt={:.6f} real_dt={:.6f} warp={:.3f} tps={:.2f} draw={} sliced={} live=({:.3f},{:.3f} vel {:.4f} dashing={} ground={}){}", job.now, reason, job.steps, job.step_delta, real_step_delta, job.warp, logic.sample_rate(), draw, sliced,
					play->m_player1->getPositionX(), play->m_player1->getPositionY(), play->m_player1->m_yVelocity, play->m_player1->m_isDashing, play->m_player1->m_isOnGround, world.follow_report(play)));
			kept = make_line(play, true, actual_click(play, true), linked, false, 0, KEPT_PAIR);
			kept.start_step = job.now;
			kept_play = play;
			kept_valid = true;
			start_line(kept);
		}
		else {
			if (logic.trace_logging && draw)
				traj_log(fmt::format("REUSE frame={} horizon={} want={} tracked={}", job.now, world.horizon(), job.now + job.steps, world.tracked_count()));
			size_t consumed = (size_t)(job.now - kept.start_step);
			if (consumed > 0) {
				auto cut = [consumed](auto& list) {
					list.erase(list.begin(), list.begin() + (std::ptrdiff_t)std::min(consumed, list.size()));
				};
				cut(kept.path);
				cut(kept.partner_path);
				cut(kept.samples);
				cut(kept.partner_samples);
				kept.start_step = job.now;
			}
		}
		kept.draw = false;
		kept.measure = 0;
		job.use_kept = true;
		bool covered = world.horizon() >= job.now + job.steps;
		for (size_t i = 0; covered && i < job.lines.size();) {
			auto& line = job.lines[i];
			bool same_role = line.player1 && line.click == kept.click && line.with_partner == kept.with_partner;
			if (!same_role) {
				++i;
				continue;
			}
			kept.draw = kept.draw || line.draw;
			kept.measure |= line.measure;
			job.lines.erase(job.lines.begin() + (std::ptrdiff_t)i);
		}
	}
	for (auto& line : job.lines)
		start_line(line);
	if (job.use_world) {
		world.begin_session(play);
		job.phase = world.horizon() < job.now + job.steps && !world.full() ? Phase::Extend : Phase::Lines;
		if (job.phase == Phase::Extend) world.resume_horizon(play);
		job.extend_started = chrono::steady_clock::now();
		job.horizon_at_start = world.horizon();
	}
	return true;
}

void Trajectory::start_line(Line& line) {
	auto& logic = Logic::get();
	auto* play = job.play;
	bool dual = play->m_gameState.m_isDualMode;
	line.with_partner = line.with_partner && dual && line.partner_real;
	line.partner_done = !line.with_partner;
	line.start_step = job.now;
	line.first_step = job.now;
	line.start_frame = logic.get_frame(play);
	copy_real_into_fake(line.fake, line.real);
	if (line.with_partner) copy_real_into_fake(line.partner, line.partner_real);
	fake_dead[fake_index(line.fake)] = false;
	fake_dead[fake_index(line.partner)] = false;
	activated[fake_index(line.fake)].clear();
	activated[fake_index(line.partner)].clear();
	line.path.clear();
	line.partner_path.clear();
	line.samples.clear();
	line.partner_samples.clear();
	line.path.push_back(line.fake->getPosition());
	if (line.with_partner) line.partner_path.push_back(line.partner->getPosition());
	bool player1 = line.player1;
	line.macro_plays = logic.plays_player(!player1);
	line.uses_autoclicker = !line.macro_plays && logic.autoclicker && logic.records_player(!player1) && (player1 ? logic.autoclick_p1 : logic.autoclick_p2);
	bool physical_key_held = Logic::get().physical_hold(play, !player1);
	line.started_held = jump_held(line.real) || (logic.maintain_gravity && physical_key_held && line.real->m_isDart);
	line.autoclick = logic.auto_click;
	auto each_body = [&](auto&& action) {
		action(line.fake);
		if (line.with_partner) action(line.partner);
	};
	each_body([&](PlayerObject* body) {
		body->m_controlsDisabled = false;
		body->m_isLocked = false;
	});
	if (line.uses_autoclicker) {
		bool stash_orbs = Orbs::click_orb_assist(!player1) || (line.with_partner && Orbs::click_orb_assist(player1));
		if (stash_orbs) logic.suppress_rings = true;
		each_body([&](PlayerObject* body) {
			if (line.autoclick.pressed) body->pushButton(PlayerButton::Jump);
			else body->releaseButton(PlayerButton::Jump);
		});
		if (stash_orbs) logic.suppress_rings = false;
	}
	else {
		each_body([&](PlayerObject* body) {
			bool already_held = jump_held(body);
			if (!already_held && logic.maintain_gravity && physical_key_held && body->m_isDart && line.click == ClickType::Hold) already_held = true;
			if (line.click == ClickType::Hold && already_held) return;
			if (line.click != ClickType::Release && line.click != ClickType::Replay) body->m_jumpBuffered = false;
			press_fake(body, line.click, is_second_fake(body));
		});
	}
	line.input_cursor = logic.replay_pos;
	const int HOLD_BUTTONS[] = { JUMP, LEFT, RIGHT };
	line.last_input_frame[0] = line.last_input_frame[1] = -1;
	std::memset(line.macro_held, 0, sizeof(line.macro_held));
	if (logic.is_playing()) {
		for (int side = 0; side < 2; ++side) {
			if (!logic.plays_player(side == 1)) continue;
			for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b)
				line.macro_held[side][b] = Logic::get().held_at(side == 1, line.start_frame, HOLD_BUTTONS[b]);
		}
		for (size_t i = logic.inputs.size(); i-- > 0 && (line.last_input_frame[0] < 0 || line.last_input_frame[1] < 0);) {
			int& last = line.last_input_frame[logic.inputs[i].player2 ? 1 : 0];
			if (last < 0) last = logic.inputs[i].number;
		}
	}
	const auto& state = play->m_gameState;
	auto cursor = state.m_spawnChannelRelated0.find(state.m_currentChannel);
	line.spawn_cursor[0] = line.spawn_cursor[1] = cursor == state.m_spawnChannelRelated0.end() ? 0 : cursor->second;
	line.orb_clicks[0].clear();
	line.orb_clicks[1].clear();
	line.pads = {};
	line.done = false;
	line.left_dual = line.partner_left_dual = false;
	line.death_step = line.partner_death_step = -1;
	line.has_death = line.partner_has_death = false;
	line.can_start_dual = player1 && !line.with_partner && line.partner_real && !Logic::get().traj_player_hidden(true);
	line.running_pair = line.with_partner;
	line.dual_started = line.dual_ended = false;
}

void Trajectory::enter_prediction() {
	auto& logic = Logic::get();
	auto* play = job.play;
	job.live_state = std::make_unique<SavedGameState>(play->m_gameState);
	job.live_random = GameToolbox::getfast_srand();
	job.live_teleport = logic.teleport_random;
	job.live_shake = logic.shake_random;
	job.live_player1 = SavedPlayerCheckpoint::create(play->m_player1);
	job.has_player2 = play->m_player2 != nullptr;
	if (job.has_player2) job.live_player2 = SavedPlayerCheckpoint::create(play->m_player2);
	job.live_died = play->m_playerDied;
	job.live_queued = play->m_queuedButtons;
	job.live_recorded = play->m_queuedRecordedButtons;
	job.live_recorded_size = play->m_queuedRecordedButtonsSize;
	if (auto* layer = play->m_objectLayer) {
		job.layer_position = layer->getPosition();
		job.layer_scale_x = layer->getScaleX();
		job.layer_scale_y = layer->getScaleY();
		job.layer_rotation = layer->getRotation();
	}
	for (int i = 0; i < 2; ++i) {
		auto* body = i == 0 ? play->m_player1 : play->m_player2;
		job.hold_buffered[i] = body && body->m_jumpBuffered;
		if (body) job.hold_buttons[i] = body->m_holdingButtons;
		else job.hold_buttons[i].clear();
	}
	if (auto* ui = play->m_uiLayer) {
		job.ui_jumping[0] = ui->m_p1Jumping;
		job.ui_jumping[1] = ui->m_p2Jumping;
		job.ui_touch[0] = ui->m_p1TouchId;
		job.ui_touch[1] = ui->m_p2TouchId;
	}
	auto* manager = GameManager::get();
	job.performance_mode = manager->m_performanceMode;
	manager->m_performanceMode = true;
	simulating = true;
	logic.predicting = true;
	Logic::get().sync_predict_hooks();
	play->m_playerDied = false;
	if (!job.paused) return;
	job.paused = false;
	auto& world = PredictedWorld::get();
	if (job.phase == Phase::Extend) {
		world.resume_horizon(play);
		return;
	}
	job.prediction_state->restore(play->m_gameState);
	GameToolbox::fast_srand(job.prediction_random);
	logic.teleport_random = job.prediction_teleport;
	logic.shake_random = job.prediction_shake;
	play->m_queuedButtons = job.prediction_queued;
	play->m_queuedRecordedButtons = job.prediction_recorded;
	play->m_queuedRecordedButtonsSize = job.prediction_recorded_size;
	if (job.use_world) world.reapply_records(play);
}

void Trajectory::leave_prediction(bool finished) {
	auto& logic = Logic::get();
	auto* play = job.play;
	auto& world = PredictedWorld::get();
	if (!finished) {
		job.paused = true;
		if (job.phase == Phase::Lines) {
			job.prediction_state = std::make_unique<SavedGameState>(play->m_gameState);
			job.prediction_random = GameToolbox::getfast_srand();
			job.prediction_teleport = logic.teleport_random;
			job.prediction_shake = logic.shake_random;
			job.prediction_queued = play->m_queuedButtons;
			job.prediction_recorded = play->m_queuedRecordedButtons;
			job.prediction_recorded_size = play->m_queuedRecordedButtonsSize;
		}
		if (job.use_world) world.pause_session(play);
	}
	else if (job.use_world) world.end_session(play);
	job.live_state->restore(play->m_gameState);
	GameToolbox::fast_srand(job.live_random);
	logic.teleport_random = job.live_teleport;
	logic.shake_random = job.live_shake;
	if (logic.trace_logging && play->m_player2)
		traj_log(fmt::format("LEAVE frame={} finished={} p2=({:.2f},{:.2f}) p2_rings_now={} p2_rings_saved={} p2_buffered_saved={} p2_held={} p1_rings_now={} p1_rings_saved={} near_p2:{}", job.now, finished,
			play->m_player2->getPositionX(), play->m_player2->getPositionY(), play->m_player2->m_touchingRings ? play->m_player2->m_touchingRings->count() : 0, job.live_player2.m_touchingRings.size(), job.hold_buffered[1],
			jump_held(play->m_player2), play->m_player1->m_touchingRings ? play->m_player1->m_touchingRings->count() : 0, job.live_player1.m_touchingRings.size(), ring_report(play, play->m_player2)));
	auto restore_body = [&](PlayerObject* body, SavedPlayerCheckpoint& checkpoint) {
		if (!body) return;
		auto logs = std::array{ body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight };
		auto* wave = body->m_waveTrail;
		auto* regular = body->m_regularTrail;
		auto* ghost = body->m_ghostTrail;
		checkpoint.apply(body);
		body->m_collisionLogTop = logs[0];
		body->m_collisionLogBottom = logs[1];
		body->m_collisionLogLeft = logs[2];
		body->m_collisionLogRight = logs[3];
		body->m_waveTrail = wave;
		body->m_regularTrail = regular;
		body->m_ghostTrail = ghost;
		if (!body->m_touchingRings) return;
		body->m_touchingRings->removeAllObjects();
		for (auto* ring : checkpoint.m_touchingRings)
			if (ring) body->m_touchingRings->addObject(ring);
	};
	restore_body(play->m_player1, job.live_player1);
	if (job.has_player2) restore_body(play->m_player2, job.live_player2);
	play->m_playerDied = job.live_died;
	play->m_queuedButtons = job.live_queued;
	play->m_queuedRecordedButtons = job.live_recorded;
	play->m_queuedRecordedButtonsSize = job.live_recorded_size;
	if (auto* layer = play->m_objectLayer) {
		layer->setPosition(job.layer_position);
		layer->setScaleX(job.layer_scale_x);
		layer->setScaleY(job.layer_scale_y);
		layer->setRotation(job.layer_rotation);
	}
	for (int i = 0; i < 2; ++i) {
		auto* body = i == 0 ? play->m_player1 : play->m_player2;
		if (!body) continue;
		body->m_jumpBuffered = job.hold_buffered[i];
		body->m_holdingButtons = job.hold_buttons[i];
	}
	if (auto* ui = play->m_uiLayer) {
		ui->m_p1Jumping = job.ui_jumping[0];
		ui->m_p2Jumping = job.ui_jumping[1];
		ui->m_p1TouchId = job.ui_touch[0];
		ui->m_p2TouchId = job.ui_touch[1];
	}
	GameManager::get()->m_performanceMode = job.performance_mode;
	simulating = false;
	logic.predicting = false;
	Logic::get().sync_predict_hooks();
}

void Trajectory::switch_to_lines() {
	auto& logic = Logic::get();
	auto* play = job.play;
	if (job.use_world) PredictedWorld::get().pause_horizon(play);
	job.live_state->restore(play->m_gameState);
	GameToolbox::fast_srand(job.live_random);
	logic.teleport_random = job.live_teleport;
	logic.shake_random = job.live_shake;
	play->m_playerDied = false;
	job.phase = Phase::Lines;
	job.step = 0;
}

bool Trajectory::want_slicing(PlayLayer* play) const {
	const auto& logic = Logic::get();
	return play && draw_node && logic.lock_delta && logic.show_traj && !Logic::get().render_capturing() && !logic.frame_advance &&
		!logic.traj_autoclicker && !(logic.pause_before_kill && logic.is_recording()) && !play->m_playerDied;
}

void Trajectory::run_slice() {
	auto& logic = Logic::get();
	auto* director = CCDirector::sharedDirector();
	unsigned drawn = director->getTotalFrames();
	if (drawn != 0 && drawn == job.last_drawn_frame) return;
	if (job.play != PlayLayer::get() || job.frame != logic.step_frame) {
		cancel_job();
		return;
	}
	job.last_drawn_frame = drawn;
	double interval = director->getAnimationInterval();
	double used = chrono::duration<double>(chrono::steady_clock::now() - logic.frame_start).count();
	double budget = interval - used - logic.draw_time;
	if (job.paused) enter_prediction();
	run_steps(budget);
	if (job.cancel || job.finished) finish_job();
	else leave_prediction(false);
}

void Trajectory::run_steps(double budget_seconds) {
	job.stepping = true;
	job.slice_start = chrono::steady_clock::now();
	job.slice_counter = 0;
	while (!job.cancel) {
		if ((++job.slice_counter & 15) == 0 && chrono::duration<double>(chrono::steady_clock::now() - job.slice_start).count() >= budget_seconds) break;
		if (step_once()) {
			job.finished = true;
			break;
		}
	}
	job.stepping = false;
}

void Trajectory::advance_time() {
	auto& state = job.play->m_gameState;
	float dt = job.step_seconds;
	state.m_totalTime += dt;
	state.m_unkDouble3 += dt / state.m_timeWarp;
	state.m_currentProgress++;
	state.m_unkUint5 += (int)roundf(state.m_timeWarp * 1000.0f);
}

bool Trajectory::step_once() {
	auto& logic = Logic::get();
	auto* play = job.play;
	auto& world = PredictedWorld::get();
	auto& state = play->m_gameState;
	if (job.phase == Phase::Extend) {
		int step = world.horizon();
		int extended = world.horizon() - job.horizon_at_start;
		bool budget_over = extended >= job.steps_per_frame * 2 && !job.sliced && !logic.frame_advance &&
			chrono::duration<double>(chrono::steady_clock::now() - job.extend_started).count() >= EXTEND_BUDGET_SECONDS;
		if (step >= job.now + job.steps || world.full() || budget_over) {
			switch_to_lines();
			return false;
		}
		advance_time();
		bool last_step = (step + 1) % job.steps_per_frame == 0;
		world.step(play, kept.fake, kept.with_partner && !kept.partner_done ? kept.partner : nullptr, last_step);
		float time_mod = *reinterpret_cast<float*>(&state.m_timeModRelated);
		if (time_mod != 0.0f) {
			state.m_timeModRelated = 0;
			state.m_timeModRelated2 = 0;
			if (!kept.done) kept.fake->updateTimeMod(time_mod, true);
			if (!kept.partner_done) kept.partner->updateTimeMod(time_mod, true);
		}
		step_line(kept, step);
		world.after_players(play);
		return false;
	}
	if (job.varying_tps) {
		job.step_seconds = 1.f / std::max(logic.rate_at(logic.get_frame(play) + job.step), 1.f);
		job.step_delta = job.step_seconds * 60.f * std::min(job.warp, 1.f);
	}
	int step = job.now + job.step;
	advance_time();
	if (job.use_world) world.apply_records(play, step + 1, lines_window());
	// w skidicate
	float time_mod = *reinterpret_cast<float*>(&state.m_timeModRelated);
	if (time_mod != 0.0f) {
		state.m_timeModRelated = 0;
		state.m_timeModRelated2 = 0;
		for (auto& line : job.lines) {
			if (!line.done) line.fake->updateTimeMod(time_mod, true);
			if (!line.partner_done) line.partner->updateTimeMod(time_mod, true);
		}
	}
	bool all_done = true;
	for (auto& line : job.lines) {
		step_line(line, step);
		if (!line.done || !line.partner_done) all_done = false;
	}
	++job.step;
	return all_done || job.step >= job.steps;
}

CCRect Trajectory::lines_window() const {
	float left = 0.f, right = 0.f, bottom = 0.f, top = 0.f;
	bool first = true;
	auto include = [&](PlayerObject* body) {
		CCPoint at = body->getPosition();
		if (first) {
			left = right = at.x;
			bottom = top = at.y;
			first = false;
			return;
		}
		left = std::min(left, at.x);
		right = std::max(right, at.x);
		bottom = std::min(bottom, at.y);
		top = std::max(top, at.y);
	};
	for (const auto& line : job.lines) {
		if (!line.done) include(line.fake);
		if (!line.partner_done) include(line.partner);
	}
	if (first) return { -WORLD_REACH, -WORLD_REACH, 0.f, 0.f };
	return { left - WORLD_REACH, bottom - WORLD_REACH, right - left + 2.f * WORLD_REACH, top - bottom + 2.f * WORLD_REACH };
}

void Trajectory::step_line(Line& line, int step) {
	auto& logic = Logic::get();
	auto* play = job.play;
	if (line.done && line.partner_done) return;
	int i = step - line.first_step;
	bool player1 = line.player1;
	auto each_body = [&](auto&& action) {
		if (!line.done) action(line.fake);
		if (!line.partner_done) action(line.partner);
	};
	Orbs::swap_prediction_pads(line.pads);
	std::array<bool, 2> orb_pressed{};
	if (line.macro_plays || (line.with_partner && logic.is_playing())) {
		int at = line.start_frame + i + 1;
		bool inputs_shared = line.with_partner && logic.records_player(false) == logic.records_player(true) && (!logic.is_playing() || logic.play_p1 == logic.play_p2);
		const int HOLD_BUTTONS[] = { JUMP, LEFT, RIGHT };
		PlayerObject* first = player1 ? line.fake : line.partner;
		PlayerObject* second = player1 ? line.partner : line.fake;
		auto send_input = [&](PlayerObject* target, bool player2, PlayerButton button, bool holding) {
			auto press_or_release = [&](PlayerObject* body, bool icon2) {
				if (!body) return;
				if (holding) {
					body->pushButton(button);
					if (button == PlayerButton::Jump) orb_pressed[icon2 ? 1 : 0] = true;
				}
				else body->releaseButton(button);
			};
			press_or_release(target, player2);
			if (inputs_shared) press_or_release(target == first ? second : first, !player2);
		};
		std::array<std::array<bool, 3>, 2> sent_this_step{};
		bool two_player_lvl = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
		while (line.input_cursor < logic.inputs.size() && logic.inputs[line.input_cursor].number <= at) {
			const auto& input = logic.inputs[line.input_cursor++];
			int raw_button = input.button == 0 ? 1 : input.button;
			// outside 2 player levels gd gives p2 inputs to p1 (silicate macros can be all p2)
			bool to_p2 = input.player2 && two_player_lvl;
			send_input(to_p2 ? second : first, to_p2, (PlayerButton)raw_button, input.holding);
			int side = to_p2 ? 1 : 0;
			for (int b = 0; b < 3; ++b) {
				if (HOLD_BUTTONS[b] != raw_button) continue;
				line.macro_held[side][b] = input.holding;
				sent_this_step[(size_t)side][(size_t)b] = true;
			}
		}
		for (int side = 0; side < (two_player_lvl ? 2 : 1); ++side) {
			if (!logic.plays_player(side == 1)) continue;
			if (!logic.ignore_inputs && at > line.last_input_frame[side]) continue;
			PlayerObject* body = side == 1 ? second : first;
			if (body != line.fake && !(line.with_partner && body == line.partner)) continue;
			for (int b = 0; b < (play->m_isPlatformer ? 3 : 1); ++b) {
				if (sent_this_step[(size_t)side][(size_t)b]) continue;
				if (b > 0 && body->m_controlsDisabled) continue;
				if (Logic::button_held(body, HOLD_BUTTONS[b]) == line.macro_held[side][b]) continue;
				send_input(body, side == 1, (PlayerButton)HOLD_BUTTONS[b], line.macro_held[side][b]);
			}
		}
	}

	bool pressed_now = i == 0 && (line.click == ClickType::Swift || (line.click == ClickType::Hold && !line.started_held));
	if (line.uses_autoclicker) {
		auto& autoclick = line.autoclick;
		if (autoclick.pressed) ++autoclick.since_press;
		else ++autoclick.since_release;
		bool flip = autoclick.pressed ? autoclick.since_press >= autoclick.press_interval : autoclick.since_release >= autoclick.release_interval;
		if (flip) {
			autoclick.pressed = !autoclick.pressed;
			autoclick.since_press = 0;
			autoclick.since_release = 0;
			pressed_now = autoclick.pressed;
			bool stash_orbs = Orbs::click_orb_assist(!player1) || (line.with_partner && Orbs::click_orb_assist(player1));
			if (stash_orbs) logic.suppress_rings = true;
			each_body([&](PlayerObject* body) {
				if (autoclick.pressed) body->pushButton(PlayerButton::Jump);
				else body->releaseButton(PlayerButton::Jump);
			});
			if (stash_orbs) logic.suppress_rings = false;
			if (autoclick.pressed && autoclick.press_interval <= 0) {
				autoclick.pressed = false;
				pressed_now = false;
				each_body([&](PlayerObject* body) {
					body->releaseButton(PlayerButton::Jump);
				});
			}
		}
	}

	auto keep_gravity_hold = [&](PlayerObject* body) {
		bool icon2 = is_second_fake(body);
		if (!body->m_isDart || logic.plays_player(icon2) || !logic.maintain_gravity || !logic.records_player(icon2)) return;
		bool physically_held = line.uses_autoclicker ? line.autoclick.pressed : line.click == ClickType::Hold;
		bool stash_orbs = Orbs::click_orb_assist(icon2);
		if (stash_orbs) logic.suppress_rings = true;
		for (int round = 0; round < 8 && !body->m_controlsDisabled; ++round) {
			if (body->m_isUpsideDown != (physically_held == body->m_jumpBuffered)) break;
			bool before_flip = body->m_isUpsideDown, before_buffer = body->m_jumpBuffered;
			if (physically_held != body->m_isUpsideDown) body->pushButton(PlayerButton::Jump);
			else body->releaseButton(PlayerButton::Jump);
			if (body->m_isUpsideDown == before_flip && body->m_jumpBuffered == before_buffer) break;
		}
		if (stash_orbs) logic.suppress_rings = false;
	};
	each_body(keep_gravity_hold);
	each_body([&](PlayerObject* body) {
		bool icon2 = is_second_fake(body);
		bool fresh_press = orb_pressed[icon2 ? 1 : 0] || (!logic.plays_player(icon2) && pressed_now);
		Orbs::click_fake(play, body, icon2, line.orb_clicks[icon2 ? 1 : 0], fresh_press);
		Orbs::click_fake_dash(body, icon2);
		Orbs::apply_prediction_pads(play, body, icon2);
	});
	each_body(keep_gravity_hold);

	each_body([&](PlayerObject* body) {
		body->resetTouchedRings(false);
	});
	if (!line.done) line.done = step_fake(line, line.fake, false, step);
	if (job.survival_path && &line != &kept && !line.done) job.survival_path->push_back(line.fake->getPosition());
	apply_dual_portal_changes(line);
	if (!line.partner_done) line.partner_done = step_fake(line, line.partner, true, step);
	apply_dual_portal_changes(line);
	Orbs::swap_prediction_pads(line.pads);
}

bool Trajectory::step_fake(Line& line, PlayerObject* body, bool is_partner, int step) {
	auto* play = job.play;
	CCPoint from = body->getPosition();
	body->m_totalTime += job.step_seconds;
	for (auto* log : { body->m_collisionLogTop, body->m_collisionLogBottom, body->m_collisionLogLeft, body->m_collisionLogRight })
		if (log) log->removeAllObjects();
	body->m_playEffects = false;
	// a fire sprite w a type that has no frames must never reach update()
	if (int streak_type = (int)(body->m_shipStreakType); body->m_shipStreak && (streak_type < 2 || streak_type > 6))
		body->m_shipStreakType = (ShipStreak)2;
	body->update(job.step_delta);
	PlayerObject*& player_slot = is_second_fake(body) ? play->m_player2 : play->m_player1;
	PlayerObject* saved_slot = player_slot;
	if (player_slot) player_slot = body;
	colliding_fake = body;
	play->checkCollisions(body, job.step_delta, false);
	colliding_fake = nullptr;
	if (player_slot) player_slot = saved_slot;
	if (play->m_playerDied) {
		play->m_playerDied = false;
		mark_dead(body);
	}
	body->m_unk838 += job.step_delta;
	if (body->m_unk838 >= 0.01f) {
		body->m_unk838 -= 0.01f;
		++body->m_followRelated;
	}
	if (auto& history = body->m_playerFollowFloats; !history.empty()) {
		int slot = body->m_followRelated % (int)history.size();
		if (slot < 0) slot += (int)history.size();
		history[(size_t)slot] = body->getPosition().y;
	}
	int index = fake_index(body);
	bool dead = fake_dead[index] || body->m_isDead;
	if (!dead) {
		bool drives_world = job.use_world && job.phase == Phase::Extend && &line == &kept && !is_partner;
		if (drives_world) PredictedWorld::get().fire_spawn_triggers(play, body, line.with_partner && !line.partner_done ? line.partner : nullptr);
		else walk_spawn_triggers(line, body, is_partner);
	}
	body->m_unkUnused3 = body->getRotation();
	body->updateRotation(job.step_delta);
	body->m_shipRotation = body->getPosition();
	dead = dead || fake_dead[index] || body->m_isDead;
	auto& path = is_partner ? line.partner_path : line.path;
	auto& samples = is_partner ? line.partner_samples : line.samples;
	if (dead) {
		fake_dead[index] = true;
		CCPoint saved = body->getPosition();
		body->setPosition(from);
		CCRect outer = body->getObjectRect(), inner = body->getObjectRect(0.3f, 0.3f);
		body->setPosition(saved);
		if (is_partner) {
			line.partner_has_death = true;
			line.partner_death_outer = outer;
			line.partner_death_inner = inner;
			line.partner_death_step = step + 1;
		}
		else {
			line.has_death = true;
			line.death_outer = outer;
			line.death_inner = inner;
			line.death_step = step + 1;
		}
		return true;
	}
	CCPoint at = body->getPosition();
	path.push_back(at);
	samples.push_back({ at.x, at.y, (float)body->m_yVelocity, body->m_isDashing, body->m_isOnGround, body->m_isUpsideDown });
	return false;
}

void Trajectory::walk_spawn_triggers(Line& line, PlayerObject* body, bool is_partner) {
	auto* play = job.play;
	const auto& state = play->m_gameState;
	int channel = state.m_currentChannel;
	auto* list = static_cast<CCArray*>(play->m_spawnObjects ? play->m_spawnObjects->objectForKey(channel) : nullptr);
	if (!list) return;
	auto going_back = state.m_spawnChannelRelated1.find(channel);
	bool backwards = going_back != state.m_spawnChannelRelated1.end() && going_back->second;
	int& cursor = line.spawn_cursor[is_partner ? 1 : 0];
	CCPoint position = body->getPosition();
	bool sideways = body->m_isSideways;
	while (cursor >= 0 && (unsigned)cursor < list->count()) {
		auto* object = static_cast<EffectGameObject*>(list->objectAtIndex((unsigned)cursor));
		float key = sideways ? object->m_speedStart.y : object->m_speedStart.x;
		float at = sideways ? position.y : position.x;
		if (backwards ? key < at : key > at) break;
		++cursor;
		int id = object->m_objectID;
		if (id != GRAVITY_TRIGGER && id != ROTATE_GAMEPLAY_TRIGGER && id != TELEPORT_TRIGGER) continue;
		if (object->m_isGroupDisabled || object->m_isTouchTriggered) continue;
		if (fakeActivated(body, object) || realActivated(body, object)) continue;
		phys::triggerObject(object, play, body);
	}
}

void Trajectory::apply_dual_portal_changes(Line& line) {
	if (std::exchange(line.dual_started, false)) {
		line.with_partner = true;
		line.running_pair = true;
		line.partner_done = false;
		line.partner->m_controlsDisabled = false;
		line.partner->m_isLocked = false;
		fake_dead[fake_index(line.partner)] = false;
		line.partner_path.clear();
		line.partner_samples.clear();
		line.partner_path.push_back(line.partner->getPosition());
	}
	if (std::exchange(line.dual_ended, false)) {
		line.running_pair = false;
		if (!line.player1) {
			line.left_dual = !line.done;
			line.done = true;
		}
		else if (line.with_partner) {
			line.partner_left_dual = !line.partner_done;
			line.partner_done = true;
		}
	}
}

void Trajectory::store_results() {
	auto& logic = Logic::get();
	int steps = job.steps;
	measured_alive_steps[0] = measured_alive_steps[1] = steps;
	measured_total_steps = steps;
	float steps_to_seconds = std::clamp(job.warp, 0.01f, 1.f) / (float)logic.sample_rate();
	auto store = [&](const Line& line) {
		if (line.measure == 0) return;
		bool dies = line.death_step >= 0 && line.death_step <= job.now + steps && !line.left_dual;
		bool partner_dies = line.with_partner && line.partner_death_step >= 0 && line.partner_death_step <= job.now + steps && !line.partner_left_dual;
		int alive_steps = dies ? line.death_step - job.now - 1 : steps;
		int partner_alive_steps = partner_dies ? line.partner_death_step - job.now - 1 : steps;
		float alive = (float)alive_steps * steps_to_seconds, partner_alive = (float)partner_alive_steps * steps_to_seconds;
		bool first_step = dies && line.death_step == job.now + 1, partner_first_step = partner_dies && line.partner_death_step == job.now + 1;
		if (line.measure & MEASURE_LIVE) {
			if (line.player1) {
				logic.traj_alive_p1 = alive;
				logic.traj_dies_p1 = dies;
				if (line.with_partner) {
					logic.traj_alive_p2 = partner_alive;
					logic.traj_dies_p2 = partner_dies;
				}
				if (!job.live_died && first_step) logic.death_next_p1 = true;
				if (!job.live_died && partner_first_step) logic.death_next_p2 = true;
			}
			else {
				logic.traj_alive_p2 = alive;
				logic.traj_dies_p2 = dies;
				if (!job.live_died && first_step) logic.death_next_p2 = true;
			}
		}
		if (line.measure & MEASURE_HOLD) {
			if (line.player1) {
				logic.traj_hold_alive_p1 = alive;
				if (line.with_partner) logic.traj_hold_alive_p2 = partner_alive;
			}
			else logic.traj_hold_alive_p2 = alive;
		}
		if (line.measure & MEASURE_RELEASE) {
			if (line.player1) {
				logic.traj_release_alive_p1 = alive;
				if (line.with_partner) logic.traj_release_alive_p2 = partner_alive;
			}
			else logic.traj_release_alive_p2 = alive;
		}
		if (line.measure & (MEASURE_HOLD | MEASURE_RELEASE | MEASURE_STEPS)) {
			measured_alive_steps[0] = dies ? alive_steps : steps;
			measured_alive_steps[1] = partner_dies ? partner_alive_steps : steps;
		}
	};
	if (job.use_kept) store(kept);
	for (const auto& line : job.lines)
		store(line);
}

void Trajectory::draw_lines() {
	auto* play = job.play;
	const auto& logic = Logic::get();
	float width = line_width(play);
	auto to_color = [](const float* c) {
		return ccColor4F{ c[0], c[1], c[2], c[3] };
	};
	draw_node->clear();
	int last_step = job.now + job.steps;
	auto draw_line = [&](const Line& line) {
		if (!line.draw) return;
		auto draw_body = [&](const std::vector<CCPoint>& path, bool icon2, int death_step, bool has_death, const CCRect& outer, const CCRect& inner) {
			auto color = path_color(icon2, line.click);
			draw_path(path, [&](CCPoint a, CCPoint b) {
				draw_node->drawSegment(a, b, width, color);
			});
			if (!has_death || death_step > last_step) return;
			draw_node->drawRect(outer, { 0.f, 0.f, 0.f, 0.f }, width, to_color(logic.hitbox_outer_color), BorderAlignment::Inside);
			draw_node->drawRect(inner, { 0.f, 0.f, 0.f, 0.f }, width, to_color(logic.hitbox_inner_color), BorderAlignment::Inside);
		};
		draw_body(line.path, !line.player1, line.death_step, line.has_death, line.death_outer, line.death_inner);
		if (line.with_partner) draw_body(line.partner_path, line.player1, line.partner_death_step, line.partner_has_death, line.partner_death_outer, line.partner_death_inner);
	};
	if (job.use_kept) draw_line(kept);
	for (const auto& line : job.lines)
		draw_line(line);
}

void Trajectory::finish_job() {
	auto* play = job.play;
	if (job.phase == Phase::Extend) switch_to_lines();
	store_results();
	if (job.draw && Logic::get().trace_logging) {
		const Line* actual = job.use_kept && (kept.measure & MEASURE_LIVE) ? &kept : nullptr;
		for (const auto& line : job.lines)
			if (!actual && (line.measure & MEASURE_LIVE) && line.player1) actual = &line;
		if (actual) {
			watch.valid = true;
			watch.reported = false;
			watch.player1 = true;
			watch.start_step = actual->start_step;
			watch.samples = actual->samples;
			traj_log(fmt::format("WATCH frame={} click={} kept={} samples={} death_step={} world={} horizon={}", job.now, (int)actual->click, actual == &kept, watch.samples.size(),
				actual->death_step, job.use_world, PredictedWorld::get().horizon()));
		}
	}
	if (job.draw && !job.cancel) draw_lines();
	leave_prediction(true);
	park_fakes(true);
	for (int i = 0; i < FAKE_PAIRS * 2; ++i)
		if (i / 2 != KEPT_PAIR || !kept_valid) activated[i].clear();
	bool draw = job.draw;
	drawing_valid = draw && !job.cancel;
	if (draw) last_cache_key = job.key;
	job.active = false;
	job.lines.clear();
	if (draw) {
		Logic::get().sync_death_label(play);
		Logic::get().check_advance_pause(play);
	}
}

void Trajectory::cancel_job() {
	if (!job.active) return;
	if (job.stepping) {
		job.cancel = true;
		return;
	}
	job.cancel = true;
	if (job.paused) {
		if (job.use_world) PredictedWorld::get().end_session(job.play);
	}
	else leave_prediction(true);
	park_fakes(true);
	for (int i = 0; i < FAKE_PAIRS * 2; ++i)
		if (i / 2 != KEPT_PAIR || !kept_valid) activated[i].clear();
	job.active = false;
	job.lines.clear();
	drawing_valid = false;
}

void Trajectory::refresh(PlayLayer* play) {
	PerfScope traj_time(PERF_TRAJECTORY);
	if (play && Logic::get().trace_logging && !(job.active && job.sliced)) note_live_step(play);
	if (job.active && job.sliced) {
		if (play == job.play && want_slicing(play)) {
			run_slice();
			return;
		}
		cancel_job();
	}
	if (!play || !play->m_started || !play->m_player1 || !draw_node || !fakes[0]) return;
	auto& logic = Logic::get();
	auto reset_results = [&] {
		logic.death_next_p1 = false;
		logic.death_next_p2 = false;
		logic.traj_alive_p1 = 0.f;
		logic.traj_alive_p2 = 0.f;
		logic.traj_dies_p1 = false;
		logic.traj_dies_p2 = false;
		logic.traj_hold_alive_p1 = 0.f;
		logic.traj_hold_alive_p2 = 0.f;
		logic.traj_release_alive_p1 = 0.f;
		logic.traj_release_alive_p2 = 0.f;
	};
	if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) {
		if (draw_node->isVisible()) draw_node->setVisible(false);
		park_fakes(true);
		if (drawing_valid || logic.death_next_p1 || logic.death_next_p2) {
			drawing_valid = false;
			logic.death_next_p1 = false;
			logic.death_next_p2 = false;
			Logic::get().sync_death_label(play);
		}
		return;
	}
	bool rendering = Logic::get().render_capturing();
	if (!logic.show_traj) {
		if (draw_node->isVisible()) draw_node->setVisible(false);
		bool pause_kill = logic.pause_before_kill && logic.is_recording();
		if (!rendering && ((logic.show_death_next && !(logic.is_playing() && !logic.is_recording())) || pause_kill)) {
			lookahead(play);
			Logic::get().check_advance_pause(play);
			Logic::get().sync_death_label(play);
			return;
		}
		if (drawing_valid || logic.death_next_p1 || logic.death_next_p2 || logic.traj_dies_p1 || logic.traj_dies_p2) {
			drawing_valid = false;
			reset_results();
			Logic::get().sync_death_label(play);
		}
		return;
	}
	Logic::get().attach_overlay(draw_node, play);
	park_fakes(true);
	draw_node->setVisible(true);
	CacheKey key = make_cache_key(play);
	if (drawing_valid && key == last_cache_key) {
		Logic::get().check_advance_pause(play);
		return;
	}
	reset_results();
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode && play->m_player2;
	bool hide1 = Logic::get().traj_player_hidden(false), hide2 = Logic::get().traj_player_hidden(true);
	bool linked = dual && !twoPlayer && !hide1 && !hide2;
	std::vector<Line> lines;
	if (!hide1) {
		if (!rendering && !play->m_playerDied && logic.plays_player(false))
			lines.push_back(make_line(play, true, ClickType::Replay, linked, true, MEASURE_LIVE, 0));
		if (!logic.plays_player(false) || rendering || play->m_playerDied) add_click_lines(play, lines, true, linked, true, rendering);
	}
	if (dual && !hide2 && !linked) {
		if (!rendering && !play->m_playerDied && logic.plays_player(true))
			lines.push_back(make_line(play, false, ClickType::Replay, false, true, MEASURE_LIVE, 0));
		if (!logic.plays_player(true) || rendering || play->m_playerDied) add_click_lines(play, lines, false, false, true, rendering);
	}
	if (lines.empty()) {
		draw_node->clear();
		drawing_valid = true;
		last_cache_key = key;
		Logic::get().sync_death_label(play);
		Logic::get().check_advance_pause(play);
		return;
	}
	if (!begin_job(play, std::move(lines), 0, true, want_slicing(play))) return;
	job.key = key;
	if (job.sliced) {
		run_slice();
		return;
	}
	run_steps(std::numeric_limits<double>::infinity());
	finish_job();
}

int Trajectory::flip_survival(PlayLayer* play, bool player1, int& full, std::vector<CCPoint>* path, int limit) {
	full = 0;
	if (!play || !play->m_started || play->m_playerDied || !draw_node || !fakes[0]) return 0;
	auto& logic = Logic::get();
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool linked = player1 && play->m_gameState.m_isDualMode && play->m_player2 && !twoPlayer;
	ClickType flip = next_click(play, player1) == ClickType::Hold ? ClickType::Release : ClickType::Hold;
	float saved_alive[4] = { logic.traj_hold_alive_p1, logic.traj_hold_alive_p2, logic.traj_release_alive_p1, logic.traj_release_alive_p2 };
	measured_alive_steps[0] = measured_alive_steps[1] = measured_total_steps = 0;
	std::vector<Line> lines;
	lines.push_back(make_line(play, player1, flip, linked, false, flip == ClickType::Hold ? MEASURE_HOLD : MEASURE_RELEASE, 0));
	if (begin_job(play, std::move(lines), limit, false, false)) {
		job.survival_path = path;
		run_steps(std::numeric_limits<double>::infinity());
		finish_job();
	}
	logic.traj_hold_alive_p1 = saved_alive[0];
	logic.traj_hold_alive_p2 = saved_alive[1];
	logic.traj_release_alive_p1 = saved_alive[2];
	logic.traj_release_alive_p2 = saved_alive[3];
	drawing_valid = false;
	full = measured_total_steps;
	return std::min(measured_alive_steps[0], measured_alive_steps[1]);
}

int Trajectory::click_survival(PlayLayer* play, bool player1, ClickType click, int& full, int limit) {
	full = 0;
	if (!play || !play->m_started || play->m_playerDied || !draw_node || !fakes[0]) return 0;
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool linked = player1 && play->m_gameState.m_isDualMode && play->m_player2 && !twoPlayer;
	measured_alive_steps[0] = measured_alive_steps[1] = measured_total_steps = 0;
	std::vector<Line> lines;
	lines.push_back(make_line(play, player1, click, linked, false, MEASURE_STEPS, 0));
	if (begin_job(play, std::move(lines), limit, false, false)) {
		run_steps(std::numeric_limits<double>::infinity());
		finish_job();
	}
	drawing_valid = false;
	full = measured_total_steps;
	return std::min(measured_alive_steps[0], measured_alive_steps[1]);
}

void Trajectory::autoclick(PlayLayer* play) {
	PerfScope click_time(PERF_AUTOCLICK);
	auto& logic = Logic::get();
	if (!logic.traj_autoclicker || !play || play->m_isPaused || play->m_playerDied) return;
	if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) return;
	if (logic.step_frame < clicked_frame) clicked_frame = -1;
	if (logic.step_frame == clicked_frame) return;
	std::memset(logic.assist_clicks, 0, sizeof(logic.assist_clicks));
	lookahead(play);
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode && play->m_player2;
	bool flip1 = logic.death_next_p1 || (dual && !twoPlayer && logic.death_next_p2);
	bool flip2 = dual && twoPlayer && logic.death_next_p2;
	int lookahead_steps = std::max(logic.traj_click_lookahead, 1);
	int current_lives[2] = { 0, 0 };
	auto is_watched = [&](bool player2) {
		return logic.is_recording() ? logic.records_player(player2) : !logic.plays_player(player2);
	};
	auto dies_within_lookahead = [&](bool player1) {
		int full = 0;
		int lives = click_survival(play, player1, next_click(play, player1), full, lookahead_steps * 2);
		if (full <= 0) return false;
		if (lives >= full || lives >= lookahead_steps) return false;
		current_lives[player1 ? 0 : 1] = lives;
		return true;
	};
	if (!flip1 && is_watched(false) && !logic.plays_player(false)) {
		bool dies_soon = dies_within_lookahead(true);
		if (lookahead_steps > 1) flip1 = dies_soon;
	}
	if (!flip2 && dual && twoPlayer && is_watched(true) && !logic.plays_player(true)) {
		bool dies_soon = dies_within_lookahead(false);
		if (lookahead_steps > 1) flip2 = dies_soon;
	}
	if (!flip1 && !flip2) return;
	auto flip_click = [&](bool player1) {
		bool held = next_click(play, player1) == ClickType::Hold;
		auto* body = player1 ? play->m_player1 : play->m_player2;
		int current = current_lives[player1 ? 0 : 1];
		int horizon = current > 0 ? lookahead_steps * 2 : 0;
		int flip_full = 0, swift_full = 0;
		int flip_lives = click_survival(play, player1, held ? ClickType::Release : ClickType::Hold, flip_full, horizon);
		int swift_lives = click_survival(play, player1, ClickType::Swift, swift_full, horizon);
		if (current > 0 && std::max(flip_lives, swift_lives) <= current) return;
		if (logic.maintain_gravity && body && body->m_isDart && logic.records_player(!player1)) {
			bool& flipped = player1 ? logic.traj_flip_p1 : logic.traj_flip_p2;
			if (!flipped) {
				Logic::get().physical_hold(play, !player1);
				(player1 ? logic.traj_flip_hold_p1 : logic.traj_flip_hold_p2) = player1 ? logic.jump_held_p1 : logic.jump_held_p2;
			}
			flipped ^= true;
			return;
		}
		auto queue_click = [&](bool down) {
			++logic.assist_clicks[player1 ? 0 : 1][down ? 1 : 0];
			play->queueButton(JUMP, down, !player1, Logic::get().click_stamp());
		};
		if (swift_lives > flip_lives) {
			if (held) queue_click(false);
			queue_click(true);
			queue_click(false);
			return;
		}
		queue_click(!held);
	};
	if (flip1) flip_click(true);
	if (flip2) flip_click(false);
	clicked_frame = logic.step_frame;
	lookahead(play);
}

void Trajectory::lookahead(PlayLayer* play) {
	PerfScope look_time(PERF_AUTOCLICK);
	auto& logic = Logic::get();
	logic.death_next_p1 = false;
	logic.death_next_p2 = false;
	if (!play || !play->m_started || play->m_playerDied || !play->m_player1 || !draw_node || !fakes[0]) return;
	if (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) return;
	auto is_watched = [&](bool player2) {
		return logic.is_recording() ? logic.records_player(player2) : !logic.plays_player(player2);
	};
	bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
	bool dual = play->m_gameState.m_isDualMode && play->m_player2;
	bool linked = dual && !twoPlayer;
	std::vector<Line> lines;
	if (is_watched(false) && !logic.plays_player(false))
		lines.push_back(make_line(play, true, next_click(play, true), linked, false, MEASURE_LIVE, 0));
	if (dual && !linked && is_watched(true) && !logic.plays_player(true))
		lines.push_back(make_line(play, false, next_click(play, false), false, false, MEASURE_LIVE, 0));
	if (begin_job(play, std::move(lines), 1, false, false)) {
		run_steps(std::numeric_limits<double>::infinity());
		finish_job();
	}
	logic.traj_alive_p1 = logic.traj_alive_p2 = 0.f;
	logic.traj_dies_p1 = logic.traj_dies_p2 = false;
	drawing_valid = false;
}

bool Logic::is_traj_player(CCObject* object) {
	return object && Trajectory::get().isFake(static_cast<PlayerObject*>(object));
}

void Logic::reset_traj() {
	Trajectory::get().invalidate();
}
