#include "Hack/orbs.hpp"
#include "Hack/trajectory.hpp"

#include "Logic/logic.hpp"
#include "Hooks/step.hpp"

#include <Geode/binding/EnhancedGameObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/RingObject.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

bool Orbs::click_orb_assist(bool player2) {
    auto& logic = Logic::get();
    if (!logic.click_dash_orbs && !logic.black_orb_assist()) return false;
    if (logic.plays_player(player2)) return false;
    if (logic.is_playing() && !logic.is_recording()) return false;
    if (logic.is_recording()) return logic.records_player(player2);
    return true;
}

static const int GREEN_DASH_ORB_ID = 1704, BLACK_ORB_ID = 1330;

static std::unordered_set<int> used_orbs_p1, used_orbs_p2;
static std::unordered_set<int> claimed_orbs_p1, claimed_orbs_p2;
static bool tool_armed = false;
static bool held_before_push_p1 = false, held_before_push_p2 = false, fresh_press_p1 = false, fresh_press_p2 = false;

static bool is_live_pointer(void const* pointer) {
    auto bits = reinterpret_cast<uintptr_t>(pointer) & 0x00ffffffffffffffull; // drop the tag android puts in the top byte
    return bits >= 0x10000u && bits < 0x0000800000000000ull;                  // probably data
}

static bool is_player2_icon(PlayerObject* player) {
    auto const& trajectory = Trajectory::get();
    if (trajectory.isFake(player)) return trajectory.is_second_fake(player);
    auto* layer = GJBaseGameLayer::get();
    return layer && player == layer->m_player2;
}

enum class OrbType {
    None,
    Dash,
    Black
};

static bool holding_jump(PlayerObject* player) {
    if (!player) return false;
    int jump = (int)PlayerButton::Jump;
    auto it = player->m_holdingButtons.find(jump);
    return it != player->m_holdingButtons.end() && it->second;
}

static OrbType orb_type_of(GameObject* object) {
    if (!is_live_pointer(object) || object->m_isDisabled || object->m_isGroupDisabled) return OrbType::None;
    auto& logic = Logic::get();
    if (logic.click_dash_orbs && (object->m_objectID == GREEN_DASH_ORB_ID || object->m_objectType == GameObjectType::DashRing || object->m_objectType == GameObjectType::GravityDashRing))
        return OrbType::Dash;
    if (logic.black_orb_assist() && (object->m_objectID == BLACK_ORB_ID || object->m_objectType == GameObjectType::DropRing))
        return OrbType::Black;
    return OrbType::None;
}

static bool moving_up(PlayerObject* player) {
    if (Logic::get().click_black_orbs || !player) return false;
    return player->m_isUpsideDown ? player->m_yVelocity < 0.0 : player->m_yVelocity > 0.0;
}

static bool orb_already_used(GameObject* object, bool player2, std::unordered_set<int> const& used) {
    if (used.contains(object->m_uniqueID)) return true;
    auto* ring = static_cast<EnhancedGameObject*>(object);
    if (static_cast<RingObject*>(object)->m_isSpawnOnly) return true;
    return player2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
}

static void for_each_object_near(GJBaseGameLayer* play, CCPoint pos, int span, auto&& visit) {
    float factor_x = play->m_sectionXFactor > 1.f ? play->m_sectionXFactor : 100.f;
    float factor_y = play->m_sectionYFactor > 1.f ? play->m_sectionYFactor : 100.f;
    int center_column = (int)std::floor(pos.x / factor_x);
    int center_row = (int)std::floor(pos.y / factor_y);
    auto const& columns = play->m_sections;
    for (int column_index = center_column - span; column_index <= center_column + span; ++column_index) {
        if (column_index < 0 || column_index >= (int)columns.size()) continue;
        auto* column = columns[(size_t)column_index];
        if (!column) continue;
        for (int row_index = center_row - span; row_index <= center_row + span; ++row_index) {
            if (row_index < 0 || row_index >= (int)column->size()) continue;
            auto* row = (*column)[(size_t)row_index];
            if (!row) continue;
            for (size_t i = 0; i < row->size(); ++i) {
                auto* obj = (*row)[i];
                if (is_live_pointer(obj)) visit(obj);
            }
        }
    }
}

static int trajectory_section_span(GJBaseGameLayer* play, PlayerObject* player) {
    float factor = play->m_sectionXFactor > 1.f ? play->m_sectionXFactor : 100.f;
    float speed = std::abs(player->m_playerSpeed * (float)player->m_speedMultiplier * 5.77f);
    int span = (int)std::ceil(speed * Logic::get().traj_seconds / factor) + 2;
    return std::clamp(span, 2, 24);
}

static bool overlaps_now(PlayerObject* player, GameObject* object) {
    if (!is_live_pointer(player) || !is_live_pointer(object)) return false;
    player->updateOrientedBox();
    object->updateOrientedBox();
    auto* player_box = player->getOrientedBox();
    auto* object_box = object->m_orientedBox;
    if (is_live_pointer(player_box) && is_live_pointer(object_box)) return player_box->overlaps(object_box);
    return player->getObjectRect().intersectsRect(object->getObjectRect());
}

// 1 steps worth, it was a whole 60fps frame at any tps so orbs got clicked early
// yw scarlet W bug reporting
static CCPoint movement_per_step(PlayerObject* player) {
    float scale = 60.f / (float)std::max(Logic::get().sample_rate(), 1.0);
    float speed = player->m_playerSpeed * (float)player->m_speedMultiplier * 5.77f * scale;
    float dx = player->m_isGoingLeft ? -speed : speed;
    return { dx, (float)player->m_yVelocity * scale };
}

static bool overlaps_soon(PlayerObject* player, GameObject* object) {
    if (!is_live_pointer(player) || !is_live_pointer(object)) return false;
    if (overlaps_now(player, object)) return true;
    if (is_live_pointer(player->m_touchingRings) && player->m_touchingRings->containsObject(object))
        return true;
    CCRect next = player->getObjectRect();
    next.origin.x += movement_per_step(player).x;
    return next.intersectsRect(object->getObjectRect());
}

static bool orb_fits_gamemode(PlayerObject* player, OrbType orb_type) {
    if (!player || orb_type == OrbType::None) return false;
    if (orb_type == OrbType::Dash) return true;
    return player->m_isBird;
}

static GameObject* find_clickable_orb(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int> const& used, bool allow_dash, bool allow_black) {
    if (!player || player->m_isDead || player->m_controlsDisabled) return nullptr;
    if (!allow_dash && !allow_black) return nullptr;
    GameObject* found = nullptr;
    auto consider = [&](GameObject* object) {
        if (found || !object) return;
        OrbType orb_type = orb_type_of(object);
        if (orb_type == OrbType::Dash && !allow_dash) return;
        if (orb_type == OrbType::Black && !allow_black) return;
        if (orb_type == OrbType::None || !orb_fits_gamemode(player, orb_type) || orb_already_used(object, player2, used))
            return;
        if (overlaps_soon(player, object)) found = object;
    };
    if (is_live_pointer(player->m_touchingRings)) {
        unsigned int count = player->m_touchingRings->count();
        for (unsigned int i = 0; i < count; ++i) {
            auto* obj = static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i));
            if (is_live_pointer(obj)) consider(obj);
        }
    }
    if (!found) for_each_object_near(play, player->getPosition(), 1, consider);
    return found;
}

static void swift_click(PlayerObject* player, bool keep_hold) {
    auto key = PlayerButton::Jump;
    int jump = (int)key;
    Orbs::arm_tool(true);
    player->m_stateRingJump2 = true;
    player->pushButton(key);
    player->releaseButton(key);
    Orbs::arm_tool(false);
    if (!keep_hold) return;
    player->pushButton(key);
    player->m_holdingButtons[jump] = true;
    player->m_jumpBuffered = true;
    player->m_wasJumpBuffered = true;
    player->m_stateJumpBuffered = 1;
}

static int queued_assist_clicks = 0;

static void queue_jump_input(PlayLayer* play, bool player2, bool down) {
    ++queued_assist_clicks;
    if (tracing())
        trace_event(fmt::format("ASSIST_QUEUE frame={} p2={} down={}\n", Logic::get().step_frame + 1, player2, down));
    play->queueButton((int)PlayerButton::Jump, down, player2, Logic::get().click_stamp());
}

struct StepPads {
    bool pending = false;
    bool black_clicked_by_assist = false;
    bool dash_orb_hit = false;
    int black_orb_count = 0;
    bool unknown_pad = false;
    std::vector<GameObject*> blue_orbs;
    std::vector<int> pad_ids;
};

static StepPads step_pads_p1, step_pads_p2;
static std::unordered_set<int> spent_pad_ids_p1, spent_pad_ids_p2, spent_blue_orb_ids_p1, spent_blue_orb_ids_p2;

using Orbs::PredictedPads;

static PredictedPads predicted_pads_p1, predicted_pads_p2;
static std::unordered_set<int> predicted_spent_pad_ids_p1, predicted_spent_pad_ids_p2, predicted_spent_blue_ids_p1, predicted_spent_blue_ids_p2;
static std::unordered_set<int> predicted_spent_black_ids_p1, predicted_spent_black_ids_p2;

static bool jump_or_gravity_pad(int type) {
    return type == (int)GameObjectType::YellowJumpPad || type == (int)GameObjectType::PinkJumpPad || type == (int)GameObjectType::RedJumpPad || type == (int)GameObjectType::GravityPad || type == (int)GameObjectType::SpiderPad;
}

static bool is_pad_black_orb(GameObject* object) {
    auto& logic = Logic::get();
    return logic.pad_black_orb && is_live_pointer(object) && !object->m_isDisabled && !object->m_isGroupDisabled && (object->m_objectID == BLACK_ORB_ID || object->m_objectType == GameObjectType::DropRing) && !static_cast<RingObject*>(object)->m_isSpawnOnly;
}

static bool is_pad_blue_orb(GameObject* object) {
    auto& logic = Logic::get();
    return logic.pad_blue_orb && is_live_pointer(object) && !object->m_isDisabled && !object->m_isGroupDisabled && object->m_objectID != 1022 && (object->m_objectID == 84 || object->m_objectType == GameObjectType::GravityRing) && !static_cast<RingObject*>(object)->m_isSpawnOnly;
}

static bool orb_triggered(GameObject* object, bool player2) {
    auto* ring = static_cast<EnhancedGameObject*>(object);
    return player2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
}

static void remember_blue_orb(StepPads& step, GameObject* object, bool player2, std::unordered_set<int> const& spent) {
    if (!is_pad_blue_orb(object) || orb_triggered(object, player2) || spent.contains(object->m_uniqueID))
        return;
    if (std::ranges::find(step.blue_orbs, object) == step.blue_orbs.end()) step.blue_orbs.push_back(object);
}

static void collect_pad_orbs(PlayLayer* play, PlayerObject* player, bool player2, StepPads& step) {
    std::unordered_set<GameObject*> seen;
    int black_orbs_touched = 0;
    auto take = [&](GameObject* object) {
        if (!object || !seen.insert(object).second) return;
        if (is_pad_black_orb(object)) ++black_orbs_touched;
        remember_blue_orb(step, object, player2, player2 ? spent_blue_orb_ids_p2 : spent_blue_orb_ids_p1);
    };
    if (player->m_touchingRings) {
        unsigned int count = player->m_touchingRings->count();
        for (unsigned int i = 0; i < count; ++i)
            take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
    }
    for_each_object_near(play, player->getPosition(), 1, [&](GameObject* object) {
        if (!is_pad_black_orb(object) && !is_pad_blue_orb(object)) return;
        if (overlaps_now(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect()))
            take(object);
    });
    if (black_orbs_touched > step.black_orb_count) step.black_orb_count = black_orbs_touched;
}

static void touch_only_this_blue_orb(PlayerObject* player, GameObject* orb) {
    auto* rings = player->m_touchingRings;
    if (!rings) return;
    std::vector<GameObject*> other_blue_orbs;
    unsigned int count = rings->count();
    for (unsigned int i = 0; i < count; ++i) {
        auto* obj = static_cast<GameObject*>(rings->objectAtIndex(i));
        if (obj != orb && is_pad_blue_orb(obj)) other_blue_orbs.push_back(obj);
    }
    for (auto* obj : other_blue_orbs)
        rings->removeObject(obj, false);
    if (!rings->containsObject(orb)) rings->addObject(orb);
}

static void queue_pad_swift_clicks(PlayLayer* play, bool player2, int times, bool already_held) {
    int swift_count = already_held ? 0 : std::max(times, 1);
    for (int i = 0; i < swift_count; ++i) {
        queue_jump_input(play, player2, true);
        queue_jump_input(play, player2, false);
    }
    if (already_held) queue_jump_input(play, player2, false);
}

std::vector<Orbs::Pose> Orbs::capture(GJBaseGameLayer* play, PlayerObject* player) {
    std::vector<Pose> poses;
    if (!play || !player) return poses;
    auto& logic = Logic::get();
    if (!logic.click_dash_orbs && !logic.black_orb_assist() && !logic.pad_blue_orb && !logic.pad_black_orb)
        return poses;
    for_each_object_near(play, player->getPosition(), trajectory_section_span(play, player), [&](GameObject* object) {
        bool listed = orb_type_of(object) != OrbType::None || is_pad_blue_orb(object) || is_pad_black_orb(object);
        if (!listed) return;
        if (std::ranges::any_of(poses, [&](Pose const& pose) {
                return pose.object == object;
            }))
            return;
        poses.push_back({ object, object->getPosition(), object->getRotation(), object->getRotationX(), object->getRotationY() });
    });
    return poses;
}

void Orbs::restore(std::vector<Pose> const& poses) {
    for (auto const& pose : poses) {
        if (!pose.object) continue;
        bool changed = false;
        if (pose.object->getPosition() != pose.position) {
            pose.object->setPosition(pose.position);
            changed = true;
        }
        if (pose.object->getRotation() != pose.rotation) {
            pose.object->setRotation(pose.rotation);
            changed = true;
        }
        if (pose.object->getRotationX() != pose.rotation_x) {
            pose.object->setRotationX(pose.rotation_x);
            changed = true;
        }
        if (pose.object->getRotationY() != pose.rotation_y) {
            pose.object->setRotationY(pose.rotation_y);
            changed = true;
        }
        if (changed || pose.object->m_isOrientedBoxDirty) pose.object->updateOrientedBox();
    }
}

std::vector<Orbs::Spot> Orbs::orb_spots(GJBaseGameLayer* play, PlayerObject* player) {
    std::vector<Spot> spots;
    for (auto const& pose : capture(play, player))
        spots.push_back({ pose.object->m_uniqueID, pose.position.x, pose.position.y, pose.rotation });
    return spots;
}

void Orbs::clear() {
    used_orbs_p1.clear();
    used_orbs_p2.clear();
    claimed_orbs_p1.clear();
    claimed_orbs_p2.clear();
    tool_armed = false;
    held_before_push_p1 = false;
    held_before_push_p2 = false;
    fresh_press_p1 = false;
    fresh_press_p2 = false;
    spent_pad_ids_p1.clear();
    spent_pad_ids_p2.clear();
    spent_blue_orb_ids_p1.clear();
    spent_blue_orb_ids_p2.clear();
    step_pads_p1 = {};
    step_pads_p2 = {};
    Orbs::reset_prediction_pads();
}

void Orbs::click_pads(PlayLayer* play, PlayerObject* player, int object_type, GameObject* pad) {
    if (!play || !player || Logic::get().predicting || !jump_or_gravity_pad(object_type)) return;
    auto& logic = Logic::get();
    if (!logic.pad_black_orb && !logic.pad_blue_orb) return;
    bool player2 = player == play->m_player2;
    if (logic.plays_player(player2)) return;
    if (pad && !jump_or_gravity_pad((int)pad->m_objectType)) pad = nullptr;
    auto& step = player2 ? step_pads_p2 : step_pads_p1;
    auto& spent_pads = player2 ? spent_pad_ids_p2 : spent_pad_ids_p1;
    step.pending = true;
    collect_pad_orbs(play, player, player2, step);
    if (!pad) {
        step.unknown_pad = true;
        return;
    }
    if (spent_pads.contains(pad->m_uniqueID)) return;
    if (std::ranges::find(step.pad_ids, pad->m_uniqueID) == step.pad_ids.end())
        step.pad_ids.push_back(pad->m_uniqueID);
}

void Orbs::note_dash(PlayLayer* play, PlayerObject* player, GameObject* ring) {
    auto& logic = Logic::get();
    if (!play || !player || logic.predicting || !logic.click_dash_orbs || tool_armed) return;
    bool player2 = player == play->m_player2;
    if (!Orbs::click_orb_assist(player2) || (ring && orb_type_of(ring) != OrbType::Dash)) return;
    auto& step = player2 ? step_pads_p2 : step_pads_p1;
    step.pending = true;
    step.dash_orb_hit = true;
}

// the trajectorys side of note_dash. w/ the assist on no dash is ever still going when a steps clicks are done: a fresh click
// is swifted on the spot, & a dash a held click started gets let go + pressed again the step after. so a fake icon thats
// dashing & holding right here can only be that 2nd kind, & it gets the same 2 inputs the real icon is about to get.
// a stack of dash orbs comes apart 1 orb a step ts way (the press goes straight into the next orb), on the real icon too
void Orbs::click_fake_dash(PlayerObject* player, bool player2) {
    if (!player || !Logic::get().click_dash_orbs || !Orbs::click_orb_assist(player2)) return;
    if (!player->m_isDashing || !holding_jump(player)) return;
    if (player->m_dashRing && orb_type_of(player->m_dashRing) != OrbType::Dash) return;
    player->releaseButton(PlayerButton::Jump);
    player->pushButton(PlayerButton::Jump);
}

bool Orbs::has_pending_pads() {
    return step_pads_p1.pending || step_pads_p2.pending;
}

void Orbs::flush_pads(PlayLayer* play) {
    if (!play || Logic::get().predicting) return;
    if (!has_pending_pads()) return;
    auto saved_p1 = std::move(step_pads_p1);
    auto saved_p2 = std::move(step_pads_p2);
    step_pads_p1 = {};
    step_pads_p2 = {};
    auto flush = [&](PlayerObject* player, bool player2, StepPads const& step) {
        if (!step.pending || !player) return;
        auto& spent_pads = player2 ? spent_pad_ids_p2 : spent_pad_ids_p1;
        auto& spent_blue = player2 ? spent_blue_orb_ids_p2 : spent_blue_orb_ids_p1;
        if (step.dash_orb_hit && player->m_isDashing && holding_jump(player)) {
            queue_jump_input(play, player2, false);
            queue_jump_input(play, player2, true);
        }
        if (step.black_orb_count > 0) queue_pad_swift_clicks(play, player2, step.black_orb_count, step.black_clicked_by_assist);
        if (step.blue_orbs.empty() || (step.pad_ids.empty() && !step.unknown_pad)) return;
        bool already_flipped = std::ranges::any_of(step.blue_orbs, [&](GameObject* orb) {
            return orb_triggered(orb, player2);
        });
        if (already_flipped) {
            if (!step.pad_ids.empty()) spent_pads.insert(step.pad_ids.front());
            return;
        }
        auto waiting = std::ranges::find_if(step.blue_orbs, [&](GameObject* orb) {
            return orb && !orb_triggered(orb, player2) && !spent_blue.contains(orb->m_uniqueID);
        });
        if (waiting == step.blue_orbs.end()) return;
        touch_only_this_blue_orb(player, *waiting);
        spent_blue.insert((*waiting)->m_uniqueID);
        if (!step.pad_ids.empty()) spent_pads.insert(step.pad_ids.front());
        if (holding_jump(player)) queue_jump_input(play, player2, false);
        queue_jump_input(play, player2, true);
        queue_jump_input(play, player2, false);
    };
    flush(play->m_player1, false, saved_p1);
    if (play->m_gameState.m_isDualMode) flush(play->m_player2, true, saved_p2);
}

static std::vector<GameObject*> stacked_orbs(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int> const& used, GameObject* orb) {
    OrbType orb_type = orb_type_of(orb);
    std::vector<GameObject*> stack;
    auto take = [&](GameObject* object) {
        if (!is_live_pointer(object) || orb_type_of(object) != orb_type || orb_already_used(object, player2, used)) return;
        if (std::ranges::find(stack, object) != stack.end()) return;
        stack.push_back(object);
    };
    if (is_live_pointer(player->m_touchingRings)) {
        unsigned int count = player->m_touchingRings->count();
        for (unsigned int i = 0; i < count; ++i)
            take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
    }
    for_each_object_near(play, player->getPosition(), 1, [&](GameObject* object) {
        if (!is_live_pointer(object) || orb_type_of(object) != orb_type) return;
        if (overlaps_soon(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect()))
            take(object);
    });
    if (stack.empty()) stack.push_back(orb);
    return stack;
}

// an assists click must ALWAYS be recorded. record_input drops a press on a button it thinks is already held,
// & after a swift that guess can be wrong. so these skip that check
bool Orbs::take_queued_assist() {
    if (queued_assist_clicks <= 0) return false;
    --queued_assist_clicks;
    return true;
}

void Orbs::arm_tool(bool on) {
    tool_armed = on;
}

void Orbs::begin_step() {
    used_orbs_p1.clear();
    used_orbs_p2.clear();
    if (!claimed_orbs_p1.empty() || !claimed_orbs_p2.empty()) {
        auto* layer = GJBaseGameLayer::get();
        if (!layer || !holding_jump(layer->m_player1)) claimed_orbs_p1.clear();
        if (!layer || !holding_jump(layer->m_player2)) claimed_orbs_p2.clear();
    }
    tool_armed = false;
    fresh_press_p1 = false;
    fresh_press_p2 = false;
}

bool Orbs::blockHoldRing(PlayerObject* player, GameObject* object) {
    if (!player || !object) return false;
    bool player2 = is_player2_icon(player);
    if (!Orbs::click_orb_assist(player2)) return false;
    OrbType orb_type = orb_type_of(object);
    if (orb_type == OrbType::None || !orb_fits_gamemode(player, orb_type)) return false;
    if (tool_armed) return false;
    return (player2 ? claimed_orbs_p2 : claimed_orbs_p1).contains(object->m_uniqueID);
}

bool Orbs::assist_clicks_orb(PlayerObject* player, GameObject* orb) {
    if (!player || !orb || !Orbs::click_orb_assist(is_player2_icon(player))) return false;
    return orb_fits_gamemode(player, orb_type_of(orb));
}

void Orbs::begin_push(PlayerObject* player) {
    if (!player) return;
    bool player2 = is_player2_icon(player);
    (player2 ? held_before_push_p2 : held_before_push_p1) = holding_jump(player);
}

void Orbs::end_push(PlayerObject* player) {
    if (!player) return;
    bool player2 = is_player2_icon(player);
    if (!(player2 ? held_before_push_p2 : held_before_push_p1)) (player2 ? fresh_press_p2 : fresh_press_p1) = true;
}

int Orbs::claim_click(GJBaseGameLayer* layer, PlayerObject* player, bool player2) {
    if (!layer || !player || Logic::get().predicting || Logic::get().editor_paused) return 0;
    if (auto* play_layer = Logic::as_play(layer); play_layer && play_layer->m_isPaused) return 0;
    if (!Orbs::click_orb_assist(player2)) return 0;

    auto& logic = Logic::get();

    if (holding_jump(player) || !player->m_touchingRings) return 0;
    if (player2 ? fresh_press_p2 : fresh_press_p1) return 0;

    auto& used = player2 ? used_orbs_p2 : used_orbs_p1;

    GameObject* orb = find_clickable_orb(layer, player, player2, used, logic.click_dash_orbs, logic.black_orb_assist());

    if (!orb) return 0;
    if (orb_type_of(orb) == OrbType::Black && moving_up(player)) return 0;

    auto stack = stacked_orbs(layer, player, player2, used, orb);
    for (auto* obj : stack) {
        if (obj && !player->m_touchingRings->containsObject(obj))
            player->m_touchingRings->addObject(obj);
    }

    player->m_stateRingJump2 = true;
    player->m_stateJumpBuffered = 1;

    if (orb_type_of(orb) == OrbType::Black) (player2 ? step_pads_p2 : step_pads_p1).black_clicked_by_assist = true;
    
    for (auto* obj : stack) {
        if (obj) {
            used.insert(obj->m_uniqueID);
            (player2 ? claimed_orbs_p2 : claimed_orbs_p1).insert(obj->m_uniqueID);
        }
    }
    if (tracing())
        trace_event(fmt::format("ASSIST_CLICK frame={} p2={} orb_id={} orb_uid={} stack={} (that many extra release+press on ts press)\n",
            Logic::get().step_frame + 1, player2, orb->m_objectID, orb->m_uniqueID, stack.size()));
    return std::max(1, (int)stack.size());
}

static void gather_prediction_orbs(GJBaseGameLayer* play, PlayerObject* player, bool player2, PredictedPads& step) {
    auto& spent_blue = player2 ? predicted_spent_blue_ids_p2 : predicted_spent_blue_ids_p1;
    auto& spent_black = player2 ? predicted_spent_black_ids_p2 : predicted_spent_black_ids_p1;
    auto& real_spent_blue = player2 ? spent_blue_orb_ids_p2 : spent_blue_orb_ids_p1;
    auto take = [&](GameObject* object) {
        if (!is_live_pointer(object)) return;
        if (player->m_ringRelatedSet.count(object->m_uniqueID) || orb_triggered(object, player2))
            return;
        if (is_pad_black_orb(object) && !spent_black.contains(object->m_uniqueID)) {
            if (std::ranges::find(step.black_orbs, object) == step.black_orbs.end())
                step.black_orbs.push_back(object);
        }
        if (is_pad_blue_orb(object) && !spent_blue.contains(object->m_uniqueID) && !real_spent_blue.contains(object->m_uniqueID)) {
            if (std::ranges::find(step.blue_orbs, object) == step.blue_orbs.end())
                step.blue_orbs.push_back(object);
        }
    };
    if (is_live_pointer(player->m_touchingRings)) {
        unsigned int count = player->m_touchingRings->count();
        for (unsigned int i = 0; i < count; ++i)
            take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
    }
    for_each_object_near(play, player->getPosition(), 1, [&](GameObject* object) {
        if (!is_pad_black_orb(object) && !is_pad_blue_orb(object)) return;
        if (overlaps_now(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect()))
            take(object);
    });
}

void Orbs::swap_prediction_pads(PadPrediction& state) {
    std::swap(predicted_pads_p1, state.p1);
    std::swap(predicted_pads_p2, state.p2);
    std::swap(predicted_spent_pad_ids_p1, state.spent_pads_p1);
    std::swap(predicted_spent_pad_ids_p2, state.spent_pads_p2);
    std::swap(predicted_spent_blue_ids_p1, state.spent_blue_p1);
    std::swap(predicted_spent_blue_ids_p2, state.spent_blue_p2);
    std::swap(predicted_spent_black_ids_p1, state.spent_black_p1);
    std::swap(predicted_spent_black_ids_p2, state.spent_black_p2);
}

void Orbs::reset_prediction_pads() {
    predicted_pads_p1 = {};
    predicted_pads_p2 = {};
    predicted_spent_pad_ids_p1.clear();
    predicted_spent_pad_ids_p2.clear();
    predicted_spent_blue_ids_p1.clear();
    predicted_spent_blue_ids_p2.clear();
    predicted_spent_black_ids_p1.clear();
    predicted_spent_black_ids_p2.clear();
}

void Orbs::notePredictionPad(GJBaseGameLayer* play, PlayerObject* player, GameObject* pad) {
    if (!Logic::get().predicting || !play || !player) return;
    auto& logic = Logic::get();
    if (!logic.pad_black_orb && !logic.pad_blue_orb) return;
    bool player2 = is_player2_icon(player);
    if (logic.plays_player(player2)) return;
    if (pad && !jump_or_gravity_pad((int)pad->m_objectType)) return;
    auto& step = player2 ? predicted_pads_p2 : predicted_pads_p1;
    auto& spent_pads = player2 ? predicted_spent_pad_ids_p2 : predicted_spent_pad_ids_p1;
    if (pad && spent_pads.contains(pad->m_uniqueID)) return;
    step.pending = true;
    if (!pad) step.unknown_pad = true;
    else if (std::ranges::find(step.pad_ids, pad->m_uniqueID) == step.pad_ids.end())
        step.pad_ids.push_back(pad->m_uniqueID);
    gather_prediction_orbs(play, player, player2, step);
}

void Orbs::apply_prediction_pads(GJBaseGameLayer* play, PlayerObject* player, bool player2) {
    if (!Logic::get().predicting || !play || !player) return;
    auto& step = player2 ? predicted_pads_p2 : predicted_pads_p1;
    if (!step.pending) return;
    PredictedPads saved = step;
    step = {};
    auto& spent_pads = player2 ? predicted_spent_pad_ids_p2 : predicted_spent_pad_ids_p1;
    auto& spent_blue = player2 ? predicted_spent_blue_ids_p2 : predicted_spent_blue_ids_p1;
    auto& spent_black = player2 ? predicted_spent_black_ids_p2 : predicted_spent_black_ids_p1;
    if (!saved.black_orbs.empty() && !saved.black_clicked_by_assist) {
        if (auto* rings = player->m_touchingRings) {
            for (auto* obj : saved.black_orbs) {
                if (obj && !rings->containsObject(obj)) rings->addObject(obj);
            }
        }
        for (auto* obj : saved.black_orbs) {
            if (!obj || player->m_ringRelatedSet.count(obj->m_uniqueID)) continue;
            player->m_stateRingJump2 = true;
            player->m_stateJumpBuffered = 1;
            swift_click(player, false);
            spent_black.insert(obj->m_uniqueID);
        }
    }
    else if (saved.black_clicked_by_assist) {
        player->releaseButton(PlayerButton::Jump);
    }
    if (saved.blue_orbs.empty() || (saved.pad_ids.empty() && !saved.unknown_pad)) return;
    bool already_flipped = std::ranges::any_of(saved.blue_orbs, [&](GameObject* orb) {
        return orb && (orb_triggered(orb, player2) || spent_blue.contains(orb->m_uniqueID) || player->m_ringRelatedSet.count(orb->m_uniqueID));
    });
    if (already_flipped) {
        if (!saved.pad_ids.empty()) spent_pads.insert(saved.pad_ids.front());
        return;
    }
    auto waiting = std::ranges::find_if(saved.blue_orbs, [&](GameObject* orb) {
        return orb && !orb_triggered(orb, player2) && !spent_blue.contains(orb->m_uniqueID) && !player->m_ringRelatedSet.count(orb->m_uniqueID);
    });
    if (waiting == saved.blue_orbs.end()) return;
    touch_only_this_blue_orb(player, *waiting);
    spent_blue.insert((*waiting)->m_uniqueID);
    if (!saved.pad_ids.empty()) spent_pads.insert(saved.pad_ids.front());
    player->m_stateRingJump2 = true;
    player->m_stateJumpBuffered = 1;
    if (holding_jump(player)) player->releaseButton(PlayerButton::Jump);
    swift_click(player, false);
}

void Orbs::click_fake(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int>& used, bool fresh_click) {
    if (!play || !player) return;
    auto& logic = Logic::get();
    if (!Orbs::click_orb_assist(player2)) {
        if (!fresh_click && !holding_jump(player)) used.clear();
        return;
    }
    bool held = holding_jump(player);
    if (!fresh_click) {
        if (!held) used.clear();
        return;
    }
    auto key = PlayerButton::Jump;
    GameObject* orb = find_clickable_orb(play, player, player2, used, logic.click_dash_orbs, logic.black_orb_assist());
    if (!orb) return;
    if (orb_type_of(orb) == OrbType::Black && moving_up(player)) return;
    auto stack = stacked_orbs(play, player, player2, used, orb);
    if (auto* rings = player->m_touchingRings) {
        for (auto* obj : stack) {
            if (obj && !rings->containsObject(obj)) rings->addObject(obj);
        }
    }
    if (orb_type_of(orb) == OrbType::Black) (player2 ? predicted_pads_p2 : predicted_pads_p1).black_clicked_by_assist = true;
    int swift_count = std::max(1, (int)stack.size());
    if (held) player->releaseButton(key);
    for (int i = 0; i < swift_count; ++i) {
        player->m_stateRingJump2 = true;
        player->m_stateJumpBuffered = 1;
        Orbs::arm_tool(true);
        player->pushButton(key);
        player->releaseButton(key);
        Orbs::arm_tool(false);
    }
    if (held) {
        player->pushButton(key);
        int jump = (int)key;
        player->m_holdingButtons[jump] = true;
        player->m_jumpBuffered = true;
        player->m_wasJumpBuffered = true;
        player->m_stateJumpBuffered = 1;
    }
    for (auto* obj : stack) {
        if (obj) used.insert(obj->m_uniqueID);
    }
}
