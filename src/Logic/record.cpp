// recording
#include "Logic/logic.hpp"
#include "Hooks/rotation.hpp"
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
    // TODO
    return {};
}

bool Logic::flip_controls() {
    auto* manager = GameManager::get();
    return manager && manager->getGameVariable("0010");
}

void Logic::set_replay_pos(PlayLayer* play) {
    // TODO
}

void Logic::trim_recorded_inputs(Session& value, int frame) {
    // TODO
}

void Logic::trim_fresh_attempt(PlayLayer* play, Session& value) {
    // TODO
}

void Logic::trim_from_checkpoint(PlayLayer* play, Session& value, Checkpoint const& checkpoint) {
    // TODO
}

void Logic::clear_physical_holds() {
    // todo
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
    // TODO
    return {};
}

std::pair<int, int> Logic::count_holds(Session const& value) {
    return { value.hold_count, value.release_count };
}

bool Logic::inputs_sorted(Session const& value) {
    return value.inputs_in_order;
}

void note_live(Session& value, Frame const& frame) {
    // TODO
}

void grow_sides(Session& value, size_t index, bool appended) {
    // TODO
}

size_t Logic::add_input(Session& value, Frame const& frame) {
    // TODO
    return {};
}

void Logic::refresh_live_hold() {
    // TODO
}

void Logic::record_input(GJBaseGameLayer* play, bool down, bool player1, int button, bool force) {
    // TODO
}

void Logic::inject_button(PlayLayer* play, bool down, int button, bool player1) {
    // later
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
