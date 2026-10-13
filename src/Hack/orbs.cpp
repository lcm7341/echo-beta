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
    if (!logic.click_dash_orbs && !logic.black_orb_assist())
        return false;
    // playback uses the saved clicks only
    if (logic.plays_player(player2))
        return false;
    if (logic.is_playing() && !logic.is_recording())
        return false;
    if (logic.is_recording())
        return logic.records_player(player2);
    return true;
}

namespace {
    constexpr int GREEN_DASH_ORB = 1704, BLACK_ORB = 1330;

    std::unordered_set<int> used_p1, used_p2;
    // orbs the assist clicked for me, kept until i let go. see blockHoldRing
    std::unordered_set<int> claimed_p1, claimed_p2;
    bool jump_press_p1 = false, jump_press_p2 = false, hold_after_p1 = false, hold_after_p2 = false;
    bool tool_press_p1 = false, tool_press_p2 = false, tool_hit_p1 = false, tool_hit_p2 = false;
    bool pending_hit_p1 = false, pending_hit_p2 = false;
    bool tool_armed = false;
    bool pushing = false;
    bool held_before_p1 = false, held_before_p2 = false, swift_p1 = false, swift_p2 = false;

    bool live(void const* pointer) {
        auto bits = reinterpret_cast<uintptr_t>(pointer) & 0x00ffffffffffffffull; // drop the tag android puts in the top byte
        return bits >= 0x10000u && bits < 0x0000800000000000ull;                  // probably data
    }

    bool icon2_of(PlayerObject* player) {
        auto const& trajectory = Trajectory::get();
        if (player == trajectory.fake2)
            return true;
        if (player == trajectory.fake1)
            return false;
        auto* layer = GJBaseGameLayer::get();
        return layer && player == layer->m_player2;
    }

    enum class Kind {
        None,
        Dash,
        Black
    };

    bool holding_jump(PlayerObject* player) {
        if (!player)
            return false;
        int jump = static_cast<int>(PlayerButton::Jump);
        auto it = player->m_holdingButtons.find(jump);
        return it != player->m_holdingButtons.end() && it->second;
    }

    Kind classify(GameObject* object) {
        if (!live(object) || object->m_isDisabled || object->m_isGroupDisabled)
            return Kind::None;
        auto& logic = Logic::get();
        if (logic.click_dash_orbs && (object->m_objectID == GREEN_DASH_ORB || object->m_objectType == GameObjectType::DashRing || object->m_objectType == GameObjectType::GravityDashRing))
            return Kind::Dash;
        if (logic.black_orb_assist() && (object->m_objectID == BLACK_ORB || object->m_objectType == GameObjectType::DropRing))
            return Kind::Black;
        return Kind::None;
    }

    // easy black orb ufo: a black orb is left alone while the icon is still going up (down, upside down), so a ufo click
    // near one doesnt slam it back. click black orbs on its own takes them every time
    bool moving_up(PlayerObject* player) {
        if (Logic::get().click_black_orbs || !player)
            return false;
        return player->m_isUpsideDown ? player->m_yVelocity < 0.0 : player->m_yVelocity > 0.0;
    }

    bool taken(GameObject* object, bool player2, std::unordered_set<int> const& used) {
        if (used.contains(object->m_uniqueID))
            return true;
        auto* ring = static_cast<EnhancedGameObject*>(object);
        if (static_cast<RingObject*>(object)->m_isSpawnOnly)
            return true;
        return player2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
    }

    void each_near(GJBaseGameLayer* play, CCPoint pos, int span, auto&& visit) {
        float factor_x = play->m_sectionXFactor > 1.f ? play->m_sectionXFactor : 100.f;
        float factor_y = play->m_sectionYFactor > 1.f ? play->m_sectionYFactor : 100.f;
        int centerX = static_cast<int>(std::floor(pos.x / factor_x));
        int centerY = static_cast<int>(std::floor(pos.y / factor_y));
        auto const& columns = play->m_sections;
        for (int column_index = centerX - span; column_index <= centerX + span; ++column_index) {
            if (column_index < 0 || column_index >= static_cast<int>(columns.size()))
                continue;
            auto* column = columns[static_cast<size_t>(column_index)];
            if (!column)
                continue;
            for (int row_index = centerY - span; row_index <= centerY + span; ++row_index) {
                if (row_index < 0 || row_index >= static_cast<int>(column->size()))
                    continue;
                auto* row = (*column)[static_cast<size_t>(row_index)];
                if (!row)
                    continue;
                for (size_t i = 0; i < row->size(); ++i) {
                    auto* obj = (*row)[i];
                    if (live(obj))
                        visit(obj);
                }
            }
        }
    }

    int travel_span(GJBaseGameLayer* play, PlayerObject* player) {
        float factor = play->m_sectionXFactor > 1.f ? play->m_sectionXFactor : 100.f;
        float speed = std::abs(player->m_playerSpeed * static_cast<float>(player->m_speedMultiplier) * 5.77f);
        int span = static_cast<int>(std::ceil(speed * Logic::get().traj_seconds / factor)) + 2;
        return std::clamp(span, 2, 24);
    }

    bool overlaps_now(PlayerObject* player, GameObject* object) {
        if (!live(player) || !live(object))
            return false;
        player->updateOrientedBox();
        object->updateOrientedBox();
        auto* playerBox = player->getOrientedBox();
        auto* object_box = object->m_orientedBox;
        if (live(playerBox) && live(object_box))
            return playerBox->overlaps(object_box);
        return player->getObjectRect().intersectsRect(object->getObjectRect());
    }

    CCPoint step_delta(PlayerObject* player) {
        float speed = player->m_playerSpeed * static_cast<float>(player->m_speedMultiplier) * 5.77f;
        float dx = player->m_isGoingLeft ? -speed : speed;
        return { dx, static_cast<float>(player->m_yVelocity) };
    }

    bool overlaps_soon(PlayerObject* player, GameObject* object) {
        if (!live(player) || !live(object))
            return false;
        if (overlaps_now(player, object))
            return true;
        if (live(player->m_touchingRings) && player->m_touchingRings->containsObject(object))
            return true;
        CCRect next = player->getObjectRect();
        next.origin.x += step_delta(player).x;
        return next.intersectsRect(object->getObjectRect());
    }

    bool fits_mode(PlayerObject* player, Kind kind) {
        if (!player || kind == Kind::None)
            return false;
        if (kind == Kind::Dash)
            return true;
        return player->m_isBird;
    }

    GameObject* find_orb(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int> const& used, bool dash_ok, bool black_ok) {
        if (!player || player->m_isDead || player->m_controlsDisabled)
            return nullptr;
        if (!dash_ok && !black_ok)
            return nullptr;
        GameObject* it = nullptr;
        auto consider = [&](GameObject* object) {
            if (it || !object)
                return;
            Kind kind = classify(object);
            if (kind == Kind::Dash && !dash_ok)
                return;
            if (kind == Kind::Black && !black_ok)
                return;
            if (kind == Kind::None || !fits_mode(player, kind) || taken(object, player2, used))
                return;
            if (overlaps_soon(player, object))
                it = object;
        };
        if (live(player->m_touchingRings)) {
            unsigned int count = player->m_touchingRings->count();
            for (unsigned int i = 0; i < count; ++i) {
                auto* obj = static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i));
                if (live(obj))
                    consider(obj);
            }
        }
        if (!it)
            each_near(play, player->getPosition(), 1, consider);
        return it;
    }

    void swift_hold(PlayerObject* player, bool keep_hold) {
        auto key = PlayerButton::Jump;
        int jump = static_cast<int>(key);
        Orbs::arm_tool(true);
        player->m_stateRingJump2 = true;
        player->pushButton(key);
        player->releaseButton(key);
        Orbs::arm_tool(false);
        if (!keep_hold)
            return;
        player->pushButton(key);
        player->m_holdingButtons[jump] = true;
        player->m_jumpBuffered = true;
        player->m_wasJumpBuffered = true;
        player->m_stateJumpBuffered = 1;
    }

    // clicks an assist put in gds queue & that still have to reach the macro
    int queued_assist = 0;

    void queue_jump(PlayLayer* play, bool player2, bool down) {
        ++queued_assist;
        if (echo::step::tracing())
            echo::step::trace_event(fmt::format("ASSIST_QUEUE frame={} p2={} down={}\n", Logic::get().step_frame + 1, player2, down));
        play->queueButton(static_cast<int>(PlayerButton::Jump), down, player2, Logic::get().click_stamp());
    }

    struct PadStep {
        bool pending = false;
        bool live_black = false;
        bool dash = false; // a held click started a dash, click it off next step
        int black = 0;
        bool anon = false;
        std::vector<GameObject*> blue;
        std::vector<int> pads;
    };

    PadStep pad_p1, pad_p2;
    std::unordered_set<int> spent_pad_p1, spent_pad_p2, spent_blue_p1, spent_blue_p2;

    struct PredictionPad {
        bool pending = false;
        bool live_black = false;
        bool anon = false;
        std::vector<GameObject*> black;
        std::vector<GameObject*> blue;
        std::vector<int> pads;
    };

    PredictionPad pred_pad1, pred_pad2;
    std::unordered_set<int> pred_spent_pad1, pred_spent_pad2, pred_spent_blue1, pred_spent_blue2;
    std::unordered_set<int> pred_spent_black1, pred_spent_black2;

    bool jump_or_gravity_pad(int type) {
        return type == static_cast<int>(GameObjectType::YellowJumpPad) || type == static_cast<int>(GameObjectType::PinkJumpPad) || type == static_cast<int>(GameObjectType::RedJumpPad) || type == static_cast<int>(GameObjectType::GravityPad) || type == static_cast<int>(GameObjectType::SpiderPad);
    }

    bool black_orb(GameObject* object) {
        auto& logic = Logic::get();
        return logic.pad_black_orb && live(object) && !object->m_isDisabled && !object->m_isGroupDisabled && (object->m_objectID == BLACK_ORB || object->m_objectType == GameObjectType::DropRing) && !static_cast<RingObject*>(object)->m_isSpawnOnly;
    }

    bool blue_orb(GameObject* object) {
        auto& logic = Logic::get();
        return logic.pad_blue_orb && live(object) && !object->m_isDisabled && !object->m_isGroupDisabled && object->m_objectID != 1022 && (object->m_objectID == 84 || object->m_objectType == GameObjectType::GravityRing) && !static_cast<RingObject*>(object)->m_isSpawnOnly;
    }

    bool orb_triggered(GameObject* object, bool player2) {
        auto* ring = static_cast<EnhancedGameObject*>(object);
        return player2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
    }

    void remember_blue(PadStep& step, GameObject* object, bool player2, std::unordered_set<int> const& spent) {
        if (!blue_orb(object) || orb_triggered(object, player2) || spent.contains(object->m_uniqueID))
            return;
        if (std::ranges::find(step.blue, object) == step.blue.end())
            step.blue.push_back(object);
    }

    void collect_pad_orbs(PlayLayer* play, PlayerObject* player, bool player2, PadStep& step) {
        std::unordered_set<GameObject*> seen;
        int black = 0;
        auto take = [&](GameObject* object) {
            if (!object || !seen.insert(object).second)
                return;
            if (black_orb(object))
                ++black;
            remember_blue(step, object, player2, player2 ? spent_blue_p2 : spent_blue_p1);
        };
        if (player->m_touchingRings) {
            unsigned int count = player->m_touchingRings->count();
            for (unsigned int i = 0; i < count; ++i)
                take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
        }
        each_near(play, player->getPosition(), 1, [&](GameObject* object) {
            if (!black_orb(object) && !blue_orb(object))
                return;
            if (overlaps_now(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect()))
                take(object);
        });
        if (black > step.black)
            step.black = black;
    }

    void arm_blue(PlayerObject* player, GameObject* orb) {
        auto* rings = player->m_touchingRings;
        if (!rings)
            return;
        std::vector<GameObject*> extra;
        unsigned int count = rings->count();
        for (unsigned int i = 0; i < count; ++i) {
            auto* obj = static_cast<GameObject*>(rings->objectAtIndex(i));
            if (obj != orb && blue_orb(obj))
                extra.push_back(obj);
        }
        for (auto* obj : extra)
            rings->removeObject(obj, false);
        if (!rings->containsObject(orb))
            rings->addObject(orb);
    }

    void queue_pad_swift(PlayLayer* play, bool player2, int times, bool already_held) {
        int extra = already_held ? 0 : std::max(times, 1);
        for (int i = 0; i < extra; ++i) {
            queue_jump(play, player2, true);
            queue_jump(play, player2, false);
        }
        if (already_held)
            queue_jump(play, player2, false);
    }
}

std::vector<Orbs::Pose> Orbs::capture(GJBaseGameLayer* play, PlayerObject* player) {
    std::vector<Pose> poses;
    if (!play || !player)
        return poses;
    auto& logic = Logic::get();
    if (!logic.click_dash_orbs && !logic.black_orb_assist() && !logic.pad_blue_orb && !logic.pad_black_orb)
        return poses;
    each_near(play, player->getPosition(), travel_span(play, player), [&](GameObject* object) {
        bool listed = classify(object) != Kind::None || blue_orb(object) || black_orb(object);
        if (!listed)
            return;
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
        if (!pose.object)
            continue;
        bool changed = false;
        if (pose.object->getPosition() != pose.pos) {
            pose.object->setPosition(pose.pos);
            changed = true;
        }
        if (pose.object->getRotation() != pose.rotation) {
            pose.object->setRotation(pose.rotation);
            changed = true;
        }
        if (pose.object->getRotationX() != pose.rotationX) {
            pose.object->setRotationX(pose.rotationX);
            changed = true;
        }
        if (pose.object->getRotationY() != pose.rotationY) {
            pose.object->setRotationY(pose.rotationY);
            changed = true;
        }
        if (changed || pose.object->m_isOrientedBoxDirty)
            pose.object->updateOrientedBox();
    }
}

uint32_t Orbs::pose_hash(GJBaseGameLayer* play, PlayerObject* player) {
    uint32_t hash = 2166136261u;
    auto mix = [&](uint32_t value) {
        hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    }; // boost hash_combine
    for (auto const& pose : capture(play, player)) {
        mix(static_cast<uint32_t>(pose.object->m_uniqueID));
        mix(std::bit_cast<uint32_t>(pose.pos.x));
        mix(std::bit_cast<uint32_t>(pose.pos.y));
        mix(std::bit_cast<uint32_t>(pose.rotation));
    }
    return hash;
}

void Orbs::clear() {
    used_p1.clear();
    used_p2.clear();
    claimed_p1.clear();
    claimed_p2.clear();
    jump_press_p1 = false;
    jump_press_p2 = false;
    hold_after_p1 = false;
    hold_after_p2 = false;
    tool_press_p1 = false;
    tool_press_p2 = false;
    tool_hit_p1 = false;
    tool_hit_p2 = false;
    pending_hit_p1 = false;
    pending_hit_p2 = false;
    tool_armed = false;
    pushing = false;
    held_before_p1 = false;
    held_before_p2 = false;
    swift_p1 = false;
    swift_p2 = false;
    spent_pad_p1.clear();
    spent_pad_p2.clear();
    spent_blue_p1.clear();
    spent_blue_p2.clear();
    pad_p1 = {};
    pad_p2 = {};
    Orbs::reset_prediction_pads();
}

void Orbs::click_pads(PlayLayer* play, PlayerObject* player, int objectType, GameObject* pad) {
    if (!play || !player || Logic::get().predicting || !jump_or_gravity_pad(objectType))
        return;
    auto& logic = Logic::get();
    if (!logic.pad_black_orb && !logic.pad_blue_orb)
        return;
    bool player2 = player == play->m_player2;
    if (logic.plays_player(player2))
        return;
    if (pad && !jump_or_gravity_pad(static_cast<int>(pad->m_objectType)))
        pad = nullptr;
    auto& step = player2 ? pad_p2 : pad_p1;
    auto& spent_pads = player2 ? spent_pad_p2 : spent_pad_p1;
    step.pending = true;
    collect_pad_orbs(play, player, player2, step);
    if (!pad) {
        step.anon = true;
        return;
    }
    if (spent_pads.contains(pad->m_uniqueID))
        return;
    if (std::ranges::find(step.pads, pad->m_uniqueID) == step.pads.end())
        step.pads.push_back(pad->m_uniqueID);
}

// a dash a HELD click started (i was already holding when the icon got to the orb). the assist clicks every dash orb, so
// ts one is let go & pressed again at the start of the next step, before the icon moves: same as a click, the dash is 0 steps long.
// its done with real inputs that go in the macro, so playback (where gd starts the same dash off the same hold) ends it the same way
void Orbs::note_dash(PlayLayer* play, PlayerObject* player, GameObject* ring) {
    auto& logic = Logic::get();
    if (!play || !player || logic.predicting || !logic.click_dash_orbs || tool_armed)
        return;
    bool player2 = player == play->m_player2;
    if (!Orbs::click_orb_assist(player2) || (ring && classify(ring) != Kind::Dash))
        return;
    auto& step = player2 ? pad_p2 : pad_p1;
    step.pending = true;
    step.dash = true;
}

// the trajectorys side of note_dash. w/ the assist on no dash is ever still going when a steps clicks are done: a fresh click
// is swifted on the spot, & a dash a held click started gets let go + pressed again the step after. so a fake icon thats
// dashing & holding right here can only be that 2nd kind, & it gets the same 2 inputs the real icon is about to get.
// a stack of dash orbs comes apart 1 orb a step ts way (the press goes straight into the next orb), on the real icon too
void Orbs::click_fake_dash(PlayerObject* player, bool player2) {
    if (!player || !Logic::get().click_dash_orbs || !Orbs::click_orb_assist(player2))
        return;
    if (!player->m_isDashing || !holding_jump(player))
        return;
    if (player->m_dashRing && classify(player->m_dashRing) != Kind::Dash)
        return;
    player->releaseButton(PlayerButton::Jump);
    player->pushButton(PlayerButton::Jump);
}

bool Orbs::has_pending_pads() {
    return pad_p1.pending || pad_p2.pending;
}

void Orbs::flush_pads(PlayLayer* play) {
    if (!play || Logic::get().predicting)
        return;
    if (!has_pending_pads())
        return;
    auto saved1 = std::move(pad_p1);
    auto saved2 = std::move(pad_p2);
    pad_p1 = {};
    pad_p2 = {};
    auto flush = [&](PlayerObject* player, bool player2, PadStep const& step) {
        if (!step.pending || !player)
            return;
        auto& spent_pads = player2 ? spent_pad_p2 : spent_pad_p1;
        auto& spent_blue = player2 ? spent_blue_p2 : spent_blue_p1;
        if (step.dash && player->m_isDashing && holding_jump(player)) {
            queue_jump(play, player2, false);
            queue_jump(play, player2, true);
        }
        if (step.black > 0)
            queue_pad_swift(play, player2, step.black, step.live_black);
        if (step.blue.empty() || (step.pads.empty() && !step.anon))
            return;
        bool already_flipped = std::ranges::any_of(step.blue, [&](GameObject* orb) {
            return orb_triggered(orb, player2);
        });
        if (already_flipped) {
            if (!step.pads.empty())
                spent_pads.insert(step.pads.front());
            return;
        }
        auto waiting = std::ranges::find_if(step.blue, [&](GameObject* orb) {
            return orb && !orb_triggered(orb, player2) && !spent_blue.contains(orb->m_uniqueID);
        });
        if (waiting == step.blue.end())
            return;
        arm_blue(player, *waiting);
        spent_blue.insert((*waiting)->m_uniqueID);
        if (!step.pads.empty())
            spent_pads.insert(step.pads.front());
        if (holding_jump(player))
            queue_jump(play, player2, false);
        queue_jump(play, player2, true);
        queue_jump(play, player2, false);
    };
    flush(play->m_player1, false, saved1);
    if (play->m_gameState.m_isDualMode)
        flush(play->m_player2, true, saved2);
}

namespace {
    std::vector<GameObject*> orb_stack(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int> const& used, GameObject* orb) {
        Kind kind = classify(orb);
        std::vector<GameObject*> stack;
        auto take = [&](GameObject* object) {
            if (!live(object) || classify(object) != kind || taken(object, player2, used))
                return;
            if (std::ranges::find(stack, object) != stack.end())
                return;
            stack.push_back(object);
        };
        if (live(player->m_touchingRings)) {
            unsigned int count = player->m_touchingRings->count();
            for (unsigned int i = 0; i < count; ++i)
                take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
        }
        each_near(play, player->getPosition(), 1, [&](GameObject* object) {
            if (!live(object) || classify(object) != kind)
                return;
            if (overlaps_soon(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect()))
                take(object);
        });
        if (stack.empty())
            stack.push_back(orb);
        return stack;
    }
}

// an assists click must ALWAYS be recorded. record_input drops a press on a button it thinks is already held,
// & after a swift that guess can be wrong. so these skip that check
bool Orbs::take_queued_assist() {
    if (queued_assist <= 0)
        return false;
    --queued_assist;
    return true;
}

void Orbs::arm_tool(bool on) {
    tool_armed = on;
}

void Orbs::begin_step() {
    used_p1.clear();
    used_p2.clear();
    if (!claimed_p1.empty() || !claimed_p2.empty()) {
        auto* layer = GJBaseGameLayer::get();
        if (!layer || !holding_jump(layer->m_player1))
            claimed_p1.clear();
        if (!layer || !holding_jump(layer->m_player2))
            claimed_p2.clear();
    }
    jump_press_p1 = false;
    jump_press_p2 = false;
    tool_hit_p1 = false;
    tool_hit_p2 = false;
    pending_hit_p1 = false;
    pending_hit_p2 = false;
    tool_armed = false;
    swift_p1 = false;
    swift_p2 = false;
}

void Orbs::commit_tool_hits() {
    if (pending_hit_p1)
        tool_hit_p1 = true;
    if (pending_hit_p2)
        tool_hit_p2 = true;
    pending_hit_p1 = false;
    pending_hit_p2 = false;
}

bool Orbs::blockHoldRing(PlayerObject* player, GameObject* object) {
    if (!player || !object)
        return false;
    bool player2 = icon2_of(player);
    if (!Orbs::click_orb_assist(player2))
        return false;
    Kind kind = classify(object);
    if (kind == Kind::None || !fits_mode(player, kind))
        return false;
    if (tool_armed)
        return false;
    // ONLY the orbs the assist clicked itself are held back: the swift it put in the macro already used them, so playback
    // wont fire them again either. every other orb goes off the way gd does it. ts used to block every held click, so
    // holding into a dash orb did nothing while recording & then dashed when the macro played (nothing in the macro can stop that)
    return (player2 ? claimed_p2 : claimed_p1).contains(object->m_uniqueID);
}

void Orbs::begin_push(PlayerObject* player) {
    if (!player)
        return;
    bool player2 = icon2_of(player);
    pushing = true;
    (player2 ? held_before_p2 : held_before_p1) = holding_jump(player);
}

void Orbs::end_push(PlayerObject* player) {
    if (!player)
        return;
    bool player2 = icon2_of(player);
    if (!(player2 ? held_before_p2 : held_before_p1))
        (player2 ? swift_p2 : swift_p1) = true;
    pushing = false;
}

void Orbs::note_jump_presses(PlayLayer* play) {
    jump_press_p1 = false;
    jump_press_p2 = false;
    hold_after_p1 = play && holding_jump(play->m_player1);
    hold_after_p2 = play && play->m_player2 && holding_jump(play->m_player2);
    if (!play)
        return;
    int jump = static_cast<int>(PlayerButton::Jump);
    for (auto const& cmd : play->m_queuedButtons) {
        int button = static_cast<int>(cmd.m_button);
        if (button != 0 && button != jump)
            continue;
        if (cmd.m_isPush)
            (cmd.m_isPlayer2 ? jump_press_p2 : jump_press_p1) = true;
        (cmd.m_isPlayer2 ? hold_after_p2 : hold_after_p1) = cmd.m_isPush;
    }
}

int Orbs::claim_click(GJBaseGameLayer* layer, PlayerObject* player, bool player2) {
    if (!layer || !player || Logic::get().predicting || Logic::get().editor_paused) return 0;
    if (auto* live = Logic::as_play(layer); live && live->m_isPaused) return 0;
    if (!Orbs::click_orb_assist(player2)) return 0;

    auto& logic = Logic::get();

    if (holding_jump(player) || !player->m_touchingRings) return 0;
    if (player2 ? swift_p2 : swift_p1) return 0;

    auto& used = player2 ? used_p2 : used_p1;

    GameObject* orb = find_orb(layer, player, player2, used, logic.click_dash_orbs, logic.black_orb_assist());

    if (!orb) return 0;
    if (classify(orb) == Kind::Black && moving_up(player)) return 0;

    auto stack = orb_stack(layer, player, player2, used, orb);
    for (auto* obj : stack) {
        if (obj && !player->m_touchingRings->containsObject(obj))
            player->m_touchingRings->addObject(obj);
    }

    player->m_stateRingJump2 = true;
    player->m_stateJumpBuffered = 1;

    if (classify(orb) == Kind::Black) (player2 ? pad_p2 : pad_p1).live_black = true;
    
    for (auto* obj : stack) {
        if (obj) {
            used.insert(obj->m_uniqueID);
            (player2 ? claimed_p2 : claimed_p1).insert(obj->m_uniqueID);
        }
    }
    if (echo::step::tracing())
        echo::step::trace_event(fmt::format("ASSIST_CLICK frame={} p2={} orb_id={} orb_uid={} stack={} (that many extra release+press on ts press)\n",
            Logic::get().step_frame + 1, player2, orb->m_objectID, orb->m_uniqueID, stack.size()));
    return std::max(1, static_cast<int>(stack.size()));
}

namespace {
    void gather_prediction_orbs(GJBaseGameLayer* play, PlayerObject* player, bool player2, PredictionPad& step) {
        auto& spent_blue = player2 ? pred_spent_blue2 : pred_spent_blue1;
        auto& spent_black = player2 ? pred_spent_black2 : pred_spent_black1;
        auto& live_blue = player2 ? spent_blue_p2 : spent_blue_p1;
        auto take = [&](GameObject* object) {
            if (!live(object))
                return;
            if (player->m_ringRelatedSet.count(object->m_uniqueID) || orb_triggered(object, player2))
                return;
            if (black_orb(object) && !spent_black.contains(object->m_uniqueID)) {
                if (std::ranges::find(step.black, object) == step.black.end())
                    step.black.push_back(object);
            }
            if (blue_orb(object) && !spent_blue.contains(object->m_uniqueID) && !live_blue.contains(object->m_uniqueID)) {
                if (std::ranges::find(step.blue, object) == step.blue.end())
                    step.blue.push_back(object);
            }
        };
        if (live(player->m_touchingRings)) {
            unsigned int count = player->m_touchingRings->count();
            for (unsigned int i = 0; i < count; ++i)
                take(static_cast<GameObject*>(player->m_touchingRings->objectAtIndex(i)));
        }
        each_near(play, player->getPosition(), 1, [&](GameObject* object) {
            if (!black_orb(object) && !blue_orb(object))
                return;
            if (overlaps_now(player, object) || player->getObjectRect().intersectsRect(object->getObjectRect()))
                take(object);
        });
    }
}

void Orbs::reset_prediction_pads() {
    pred_pad1 = {};
    pred_pad2 = {};
    pred_spent_pad1.clear();
    pred_spent_pad2.clear();
    pred_spent_blue1.clear();
    pred_spent_blue2.clear();
    pred_spent_black1.clear();
    pred_spent_black2.clear();
}

void Orbs::notePredictionPad(GJBaseGameLayer* play, PlayerObject* player, GameObject* pad) {
    if (!Logic::get().predicting || !play || !player)
        return;
    auto& logic = Logic::get();
    if (!logic.pad_black_orb && !logic.pad_blue_orb)
        return;
    bool player2 = icon2_of(player);
    if (logic.plays_player(player2))
        return;
    if (pad && !jump_or_gravity_pad(static_cast<int>(pad->m_objectType)))
        return;
    auto& step = player2 ? pred_pad2 : pred_pad1;
    auto& spent_pads = player2 ? pred_spent_pad2 : pred_spent_pad1;
    if (pad && spent_pads.contains(pad->m_uniqueID))
        return;
    step.pending = true;
    if (!pad)
        step.anon = true;
    else if (std::ranges::find(step.pads, pad->m_uniqueID) == step.pads.end())
        step.pads.push_back(pad->m_uniqueID);
    gather_prediction_orbs(play, player, player2, step);
}

void Orbs::apply_prediction_pads(GJBaseGameLayer* play, PlayerObject* player, bool player2) {
    if (!Logic::get().predicting || !play || !player)
        return;
    auto& step = player2 ? pred_pad2 : pred_pad1;
    if (!step.pending)
        return;
    PredictionPad saved = step;
    step = {};
    auto& spent_pads = player2 ? pred_spent_pad2 : pred_spent_pad1;
    auto& spent_blue = player2 ? pred_spent_blue2 : pred_spent_blue1;
    auto& spent_black = player2 ? pred_spent_black2 : pred_spent_black1;
    if (!saved.black.empty() && !saved.live_black) {
        if (auto* rings = player->m_touchingRings) {
            for (auto* obj : saved.black) {
                if (obj && !rings->containsObject(obj))
                    rings->addObject(obj);
            }
        }
        for (auto* obj : saved.black) {
            if (!obj || player->m_ringRelatedSet.count(obj->m_uniqueID))
                continue;
            player->m_stateRingJump2 = true;
            player->m_stateJumpBuffered = 1;
            swift_hold(player, false);
            spent_black.insert(obj->m_uniqueID);
        }
    }
    else if (saved.live_black) {
        player->releaseButton(PlayerButton::Jump);
    }
    if (saved.blue.empty() || (saved.pads.empty() && !saved.anon))
        return;
    bool already = std::ranges::any_of(saved.blue, [&](GameObject* orb) {
        return orb && (orb_triggered(orb, player2) || spent_blue.contains(orb->m_uniqueID) || player->m_ringRelatedSet.count(orb->m_uniqueID));
    });
    if (already) {
        if (!saved.pads.empty())
            spent_pads.insert(saved.pads.front());
        return;
    }
    auto waiting = std::ranges::find_if(saved.blue, [&](GameObject* orb) {
        return orb && !orb_triggered(orb, player2) && !spent_blue.contains(orb->m_uniqueID) && !player->m_ringRelatedSet.count(orb->m_uniqueID);
    });
    if (waiting == saved.blue.end())
        return;
    arm_blue(player, *waiting);
    spent_blue.insert((*waiting)->m_uniqueID);
    if (!saved.pads.empty())
        spent_pads.insert(saved.pads.front());
    player->m_stateRingJump2 = true;
    player->m_stateJumpBuffered = 1;
    if (holding_jump(player))
        player->releaseButton(PlayerButton::Jump);
    swift_hold(player, false);
}

void Orbs::click_fake(GJBaseGameLayer* play, PlayerObject* player, bool player2, std::unordered_set<int>& used, bool fresh_click) {
    if (!play || !player)
        return;
    auto& logic = Logic::get();
    if (!Orbs::click_orb_assist(player2)) {
        if (!fresh_click && !holding_jump(player))
            used.clear();
        return;
    }
    bool held = holding_jump(player);
    if (!fresh_click) {
        if (!held)
            used.clear();
        return;
    }
    auto key = PlayerButton::Jump;
    GameObject* orb = find_orb(play, player, player2, used, logic.click_dash_orbs, logic.black_orb_assist());
    if (!orb)
        return;
    if (classify(orb) == Kind::Black && moving_up(player))
        return;
    auto stack = orb_stack(play, player, player2, used, orb);
    if (auto* rings = player->m_touchingRings) {
        for (auto* obj : stack) {
            if (obj && !rings->containsObject(obj))
                rings->addObject(obj);
        }
    }
    if (classify(orb) == Kind::Black)
        (player2 ? pred_pad2 : pred_pad1).live_black = true;
    int swifts = std::max(1, static_cast<int>(stack.size()));
    if (held)
        player->releaseButton(key);
    for (int i = 0; i < swifts; ++i) {
        player->m_stateRingJump2 = true;
        player->m_stateJumpBuffered = 1;
        Orbs::arm_tool(true);
        player->pushButton(key);
        player->releaseButton(key);
        Orbs::arm_tool(false);
    }
    if (held) {
        player->pushButton(key);
        int jump = static_cast<int>(key);
        player->m_holdingButtons[jump] = true;
        player->m_jumpBuffered = true;
        player->m_wasJumpBuffered = true;
        player->m_stateJumpBuffered = 1;
    }
    for (auto* obj : stack) {
        if (obj)
            used.insert(obj->m_uniqueID);
    }
}
