// recording
//
// record_input is called from gds button handler. the input is stored for step_frame + 1 bc gd counts a step BEFORE it reads the button
// pressing a button thats already held is not a new input. the latest hold per player & button is kept so that check is instant
// in a 1 player dual 1 button moves both icons, thats stored as p1 even when the 2nd icon is the one being recorded
// recording over part of a macro trims everything after that point for the recorded player & leaves the other players inputs
// capture_player / apply_player copy a players whole state. setting the flags alone leaves the sprite in the wrong mode, so it goes thru gds setters
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
#include "GUI/gui.hpp"
#include <Geode/utils/async.hpp>
#include <Geode/binding/PlayerButtonCommand.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>
#include <Geode/binding/GJUINode.hpp>
#include <Geode/binding/UILayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <random>

using namespace geode::prelude;

FramePose Logic::capture_pose(PlayerObject* player) {
    FramePose pose;
    if (!player)
        return pose;
    pose.x = player->getPositionX();
    pose.y = player->getPositionY();
    pose.rotation = player->getRotation();
    pose.yVelocity = player->m_yVelocity;
    pose.x_velocity = static_cast<double>(player->m_playerSpeed);
    return pose;
}

bool Logic::flip_controls() {
    auto* manager = GameManager::get();
    return manager && manager->getGameVariable("0010");
}

void Logic::set_replay_pos(PlayLayer* play) {
    auto& logic = Logic::get();
    int frame = play ? Logic::get().get_frame(play) : 0;
    auto later = logic.inputs_in_order
                     ? std::lower_bound(logic.inputs.begin(), logic.inputs.end(), frame,
                           [](Frame const& input, int key) {
                               return input.number < key;
                           })
                     : std::find_if(logic.inputs.begin(), logic.inputs.end(), [frame](Frame const& input) {
                           return input.number >= frame;
                       });
    logic.replay_pos = static_cast<size_t>(std::distance(logic.inputs.begin(), later));
    logic.hold_sync = true;
}

void Logic::trim_recorded_inputs(Session& value, int frame) {
    bool record1 = value.records_player(false), record2 = value.records_player(true);
    if (!record1 && !record2)
        return;
    replay::compact(value.inputs, value.poses, value.input_ids, [&](Frame const& input) {
        bool matches = input.player2 ? record2 : record1;
        bool later = frame <= 0 ? input.number >= frame : input.number > frame;
        return !(matches && later);
    });
    value.touch_inputs();
    Logic::get().refresh_live_hold();
}

void Logic::trim_fresh_attempt(PlayLayer* play, Session& value) {
    (void)play;
    Logic::get().trim_recorded_inputs(value, 0);
    if (!value.is_playing())
        Logic::get().trim_rates(0);
    value.removed_frames = 0;
    value.replay_pos = 0;
}

void Logic::trim_from_checkpoint(PlayLayer* play, Session& value, Checkpoint const& checkpoint) {
    value.step_frame = checkpoint.frame;
    Logic::get().apply_layer(play, checkpoint.layer);
    Logic::get().apply_player(play->m_player1, checkpoint.player1);
    Logic::get().apply_player(play->m_player2, checkpoint.player2);
    if ((value).is_recording()) {
        int frame = Logic::get().get_frame(play);
        Logic::get().trim_recorded_inputs(value, frame);
        if (!value.is_playing())
            Logic::get().trim_rates(frame);
        Logic::get().sort_inputs();
        value.replay_pos = value.inputs.size();
    }
}

void Logic::clear_physical_holds() {
    auto& logic = Logic::get();
    logic.key_jump_p1 = logic.key_jump_p2 = false;
    logic.steered_keys.clear();
    logic.key_left_p1 = logic.key_right_p1 = false;
    logic.key_left_p2 = logic.key_right_p2 = false;
    logic.death_jump_p1 = logic.death_jump_p2 = false;
    logic.death_left_p1 = logic.death_right_p1 = false;
    logic.death_left_p2 = logic.death_right_p2 = false;
    logic.physical_jump_p1 = logic.physical_jump_p2 = false;
    logic.physical_left_p1 = logic.physical_right_p1 = false;
    logic.physical_left_p2 = logic.physical_right_p2 = false;
}

namespace {
    struct HeldLane {
        std::vector<int> frames;
        std::vector<char> held;
        mutable replay::PrefixCursor cursor;
    };

    struct InputIndex {
        std::vector<Frame> const* owner = nullptr;
        Frame const* data = nullptr;
        size_t count = 0;
        uint64_t revision = 0;
        Frame last;
        bool sorted = true;
        int holds = 0;
        int releases = 0;
        // 0-5 = p1 & p2 jump, left, right. 6-8 = the same 3 buttons with BOTH players together
        // (outside 2 player levels gd gives every input to the same icon)
        std::array<HeldLane, 9> lanes;
    };

    int lane_of(bool player2, int button) {
        int const jump = static_cast<int>(PlayerButton::Jump);
        int const left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
        int slot = button == 0 || button == jump ? 0 : button == left ? 1
                                                   : button == right  ? 2
                                                                      : -1;
        if (slot < 0)
            return -1;
        return (player2 ? 3 : 0) + slot;
    }

    bool same_input(Frame const& a, Frame const& b) {
        return a.number == b.number && a.holding == b.holding && a.player2 == b.player2 && a.button == b.button;
    }

    bool append_input(InputIndex& index, Frame const& input) {
        int lane = lane_of(input.player2, input.button);
        if (lane >= 0) {
            auto& at = index.lanes[static_cast<size_t>(lane)];
            auto& both = index.lanes[static_cast<size_t>(6 + lane % 3)];
            if (!at.frames.empty() && input.number < at.frames.back())
                return false;
            if (!both.frames.empty() && input.number < both.frames.back())
                return false;
            at.frames.push_back(input.number);
            at.held.push_back(input.holding ? 1 : 0);
            both.frames.push_back(input.number);
            both.held.push_back(input.holding ? 1 : 0);
        }
        if (index.count > 0 && input.number < index.last.number)
            index.sorted = false;
        (input.holding ? index.holds : index.releases) += 1;
        index.last = input;
        ++index.count;
        return true;
    }

    void rebuild_index(InputIndex& index, std::vector<Frame> const& inputs) {
        auto reset = [&] {
            for (auto& lane : index.lanes) {
                lane.frames.clear();
                lane.held.clear();
                lane.cursor = {};
            }
            index.count = 0;
            index.sorted = true;
            index.holds = 0;
            index.releases = 0;
            index.last = Frame{};
        };
        reset();
        bool in_order = std::ranges::all_of(inputs, [&](Frame const& input) {
            return append_input(index, input);
        });
        if (in_order)
            return;

        reset();
        std::array<std::vector<size_t>, 9> members;
        for (size_t i = 0; i < inputs.size(); ++i) {
            auto const& input = inputs[i];
            int lane = lane_of(input.player2, input.button);
            if (lane >= 0) {
                members[static_cast<size_t>(lane)].push_back(i);
                members[static_cast<size_t>(6 + lane % 3)].push_back(i);
            }
            if (i > 0 && input.number < inputs[i - 1].number)
                index.sorted = false;
            (input.holding ? index.holds : index.releases) += 1;
        }
        for (size_t lane = 0; lane < members.size(); ++lane) {
            auto& order = members[lane];
            std::ranges::stable_sort(order, {}, [&](size_t i) {
                return inputs[i].number;
            });
            auto& at = index.lanes[lane];
            at.frames.reserve(order.size());
            at.held.reserve(order.size());
            size_t newest = 0;
            for (size_t k = 0; k < order.size(); ++k) {
                newest = k == 0 ? order[k] : std::max(newest, order[k]);
                at.frames.push_back(inputs[order[k]].number);
                at.held.push_back(inputs[newest].holding ? 1 : 0);
            }
        }
        index.count = inputs.size();
        if (!inputs.empty())
            index.last = inputs.back();
    }

    InputIndex& held_index() {
        thread_local InputIndex idx;
        return idx;
    }

    InputIndex& synced_index(Session const& value) {
        auto& idx = held_index();
        auto const& inputs = value.inputs;
        bool kept = idx.owner == &inputs && idx.revision == value.inputs_revision && idx.count <= inputs.size();
        if (kept && idx.count > 0)
            kept = same_input(inputs[idx.count - 1], idx.last);
        if (kept && idx.count == inputs.size())
            kept = idx.data == inputs.data();
        if (!kept)
            rebuild_index(idx, inputs);
        for (size_t i = idx.count; i < inputs.size(); ++i) {
            if (append_input(idx, inputs[i]))
                continue;
            rebuild_index(idx, inputs);
            break;
        }
        idx.owner = &inputs;
        idx.data = inputs.data();
        idx.revision = value.inputs_revision;
        return idx;
    }
}

void Logic::release_replay_index() {
    held_index() = {};
}

bool Logic::held_at(Session const& value, bool player2, int frame, int button) {
    int const jump = static_cast<int>(PlayerButton::Jump);
    int want = button == 0 ? jump : button;
    int lane = lane_of(player2, want);
    if (lane < 0) {
        bool held = false;
        for (auto const& input : value.inputs) {
            int stored = input.button == 0 ? jump : input.button;
            if (input.player2 != player2 || stored != want || input.number > frame)
                continue;
            held = input.holding;
        }
        return held;
    }
    size_t slot = static_cast<size_t>(lane);
    if (value.live_seen[slot] && frame >= value.live_frame[slot])
        return value.live_held[slot] != 0;
    // playing a macro outside a 2 player level: a p2 input moves the same icon as a p1 input, so count them as 1
    if (value.is_playing() && !value.is_recording()) {
        auto* play = PlayLayer::get();
        if (play && !(play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode)) {
            auto const& both = synced_index(value).lanes[6 + slot % 3];
            size_t after = both.cursor.seek(both.frames, frame);
            return after > 0 && both.held[after - 1] != 0;
        }
    }
    if (!value.is_playing()) {
        for (size_t i = value.inputs.size(); i-- > 0;) {
            auto const& input = value.inputs[i];
            int stored = input.button == 0 ? jump : input.button;
            if (input.player2 != player2 || stored != want || input.number > frame)
                continue;
            return input.holding;
        }
        return false;
    }
    auto const& at = synced_index(value).lanes[slot];
    size_t after = at.cursor.seek(at.frames, frame);
    return after > 0 && at.held[after - 1] != 0;
}

std::pair<int, int> Logic::count_holds(Session const& value) {
    return { value.hold_count, value.release_count };
}

bool Logic::inputs_sorted(Session const& value) {
    return value.inputs_in_order;
}

void note_live(Session& value, Frame const& frame) {
    value.newest_input = std::max(value.newest_input, frame.number);
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    int button = frame.button == 0 ? jump : frame.button;
    int slot = button == jump ? 0 : button == left ? 1
                                : button == right  ? 2
                                                   : -1;
    if (slot < 0)
        return;
    slot += frame.player2 ? 3 : 0;
    size_t lane = static_cast<size_t>(slot);
    if (value.live_seen[lane] && frame.number < value.live_frame[lane])
        return;
    value.live_frame[lane] = frame.number;
    value.live_held[lane] = frame.holding ? 1 : 0;
    value.live_seen[lane] = 1;
}

void grow_sides(Session& value, size_t index, bool appended) {
    auto grow = [&](auto& side, auto empty) {
        if (side.empty())
            return;
        if (side.size() + 1 != value.inputs.size()) {
            side.clear();
            return;
        }
        if (appended)
            side.push_back(empty);
        else
            side.insert(side.begin() + static_cast<std::ptrdiff_t>(index), empty);
    };
    grow(value.poses, FramePose{});
    grow(value.input_ids, uint64_t{ 0 });
    if (value.save_debug && value.poses.size() != value.inputs.size())
        value.poses.resize(value.inputs.size());
}

size_t Logic::add_input(Session& value, Frame const& frame) {
    (frame.holding ? value.hold_count : value.release_count) += 1;
    if (!value.sort_automatically || !(value).is_playing()) {
        if (!value.inputs.empty() && frame.number < value.inputs.back().number)
            value.inputs_in_order = false;
        value.inputs.push_back(frame);
        grow_sides(value, value.inputs.size() - 1, true);
        note_live(value, frame);
        return value.inputs.size() - 1;
    }
    auto later = std::upper_bound(value.inputs.begin(), value.inputs.end(), frame, [](Frame const& key, Frame const& element) {
        if (key.number != element.number)
            return key.number < element.number;
        if (key.player2 != element.player2)
            return key.player2 < element.player2;
        return key.button < element.button;
    });
    size_t idx = static_cast<size_t>(std::distance(value.inputs.begin(), later));
    value.inputs.insert(later, frame);
    grow_sides(value, idx, false);
    if (idx + 1 != value.inputs.size())
        value.touch_inputs();
    if (idx <= value.replay_pos)
        ++value.replay_pos;
    note_live(value, frame);
    return idx;
}

void Logic::refresh_live_hold() {
    auto& logic = Logic::get();
    logic.live_frame.fill(-1);
    logic.live_held.fill(0);
    logic.live_seen.fill(0);
    logic.newest_input = -1;
    logic.hold_count = 0;
    logic.release_count = 0;
    logic.inputs_in_order = true;
    int prev = -1;
    for (auto const& input : logic.inputs) {
        note_live(logic, input);
        (input.holding ? logic.hold_count : logic.release_count) += 1;
        if (prev >= 0 && input.number < prev)
            logic.inputs_in_order = false;
        prev = input.number;
    }
}

void Logic::record_input(GJBaseGameLayer* play, bool down, bool player1, int button, bool force) {
    auto& logic = Logic::get();
    if (!(logic).is_recording() || !play)
        return;
    if (logic.resetting || logic.refresh_only || logic.ignore_record_frames > 0)
        return;
    int jump = static_cast<int>(PlayerButton::Jump);
    int left = static_cast<int>(PlayerButton::Left), right = static_cast<int>(PlayerButton::Right);
    if (button != 0 && button != jump && button != left && button != right)
        return;
    if (button == 0)
        button = jump;
    if (logic.mirror_click)
        return;
    bool player2 = !player1;
    bool twoPlayer = play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
    bool folded = !twoPlayer && player2;
    if (folded) {
        player1 = true;
        player2 = false;
    }
    if (!logic.records_player(player2) && !(folded && logic.records_player(true)))
        return;
    int now = Logic::get().step_frame + 1;
    if (!(logic).is_playing()) {
        auto last = std::find_if(logic.inputs.rbegin(), logic.inputs.rend(), [player2](Frame const& input) {
            return input.player2 == player2;
        });
        if (last != logic.inputs.rend() && last->number > now)
            return;
    }
    if (!force && down == Logic::get().held_at(logic, player2, now, button))
        return;

    Frame frame;
    frame.number = now;
    frame.holding = down;
    frame.player2 = player2;
    frame.button = static_cast<uint8_t>(button);
    size_t at = Logic::get().add_input(logic, frame);
    if (at != static_cast<size_t>(-1) && logic.save_debug) {
        PlayerObject* body = player1 ? play->m_player1 : play->m_player2;
        logic.set_pose_at(at, Logic::get().capture_pose(body));
    }
    if (!logic.click_both || logic.routing_input || logic.maintaining_gravity || !logic.records_player(!player2))
        return;
    logic.routing_input = true;
    Logic::get().record_input(play, down, !player1, button, force);
    logic.routing_input = false;
}

void Logic::inject_button(PlayLayer* play, bool down, int button, bool player1) {
    auto& logic = Logic::get();
    logic.injecting = true;
    play->handleButton(down, button == 0 ? static_cast<int>(PlayerButton::Jump) : button, player1);
    logic.injecting = false;
}

void Logic::note_rule(std::vector<CpsNote>& notes, int first, int last, std::string text) {
    // todo
}

void Logic::eval_player_cps(std::vector<int> const& presses, bool player2, Session& value) {
    // todo
}

namespace {
    struct CpsResult {
        std::vector<CpsNote> p1, p2;
        std::string highest;
    };

    CpsResult scan_cps(std::vector<int> const& p1, std::vector<int> const& p2, float macro_fps, std::vector<RateChange> rates, int max_cps) {
        Session scan;
        scan.macro_fps = macro_fps;
        scan.fps = macro_fps;
        scan.max_cps = std::max(max_cps, 1);
        scan.rates = std::move(rates);
        scan.highest_cps = "P1: 0";
        Logic::live().eval_player_cps(p1, false, scan);
        Logic::live().eval_player_cps(p2, true, scan);
        return { std::move(scan.cps_notes_p1), std::move(scan.cps_notes_p2), std::move(scan.highest_cps) };
    }

    std::pair<std::vector<int>, std::vector<int>> split_presses(std::vector<Frame> const& inputs) {
        std::pair<std::vector<int>, std::vector<int>> presses;
        for (auto const& input : inputs) {
            if (!input.holding)
                continue;
            (input.player2 ? presses.second : presses.first).push_back(input.number);
        }
        return presses;
    }

    void take_cps(Session& value, CpsResult result) {
        value.cps_notes_p1 = std::move(result.p1);
        value.cps_notes_p2 = std::move(result.p2);
        value.highest_cps = std::move(result.highest);
    }
}

// runs on a worker so big macros dont freeze the game
void Logic::recompute_cps(Session& value) {
    // TODO
}
