#include "Logic/replay_algorithms.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

namespace gd { template <class T> using vector = std::vector<T>; }
enum class PlayerButton { Jump = 1, Left = 2, Right = 3 };
struct PlayerButtonCommand {
    PlayerButton m_button = PlayerButton::Jump;
    bool m_isPush = false, m_isPlayer2 = false;
    int m_step = 0;
    double m_timestamp = 0;
    bool operator==(PlayerButtonCommand const&) const = default;
};
struct Frame { int number; bool holding, player2; int button = 1; };
struct Body { bool m_controlsDisabled = false, m_isDead = false; std::array<bool, 4> held{}; };
struct Settings { bool m_twoPlayerMode = false; };
struct GJBaseGameLayer {
    gd::vector<PlayerButtonCommand> m_queuedButtons;
    struct { bool m_isDualMode = false; } m_gameState;
    Settings settings;
    Settings* m_levelSettings = &settings;
    Body p1, p2;
    Body* m_player1 = &p1;
    Body* m_player2 = &p2;
    bool m_isPlatformer = false, m_isPaused = false;
    void processQueuedButtons(float, bool);
};
struct PlayLayer : GJBaseGameLayer {};
struct Event {
    int kind, button;
    bool down, player2, playback, injected;
    bool operator==(Event const&) const = default;
};
std::vector<Event> events;
std::vector<PlayerButtonCommand> pads;
bool autoDual = false;
int commits = 0;

struct Logic {
    bool editorPaused = false, buttonsPrepared = false, resetting = false, refreshOnly = false;
    bool restoringHolds = false, ignoreInputs = true, predicting = false;
    bool suppressClickHitbox = false, injecting = false, applyingPlayback = false;
    bool maintainGravity = false, sameGravity = false;
    bool recording = false, playing = false;
    bool playP1 = true, playP2 = true, recordP1 = true, recordP2 = true;
    int ignoreRecordFrames = 0, replayClicksLeft = 0, stepFrame = 0;
    uint32_t partnerPresses = 0;
    size_t replayPos = 0;
    std::vector<Frame> inputs;
    uint64_t inputsRevision = 0; // Bumped by touchInputs in the real Session.
    static Logic& get() { static Logic value; return value; }
    static PlayLayer* asPlay(GJBaseGameLayer* play) { return static_cast<PlayLayer*>(play); }
    bool physicsLocked() const { return playing || recording; }
    bool isRecording() const { return recording; }
    bool isPlaying() const { return playing; }
    bool playsPlayer(bool p2) const { return playing && (p2 ? playP2 : playP1); }
    bool recordsPlayer(bool p2) const { return recording && !playsPlayer(p2) && (p2 ? recordP2 : recordP1); }
    bool steerPlayer(bool& p1) {
        bool r1 = recordsPlayer(false), r2 = recordsPlayer(true);
        if (r1 == r2) return false;
        p1 = !r2;
        return true;
    }
    void rememberMove(PlayerButtonCommand const& cmd) {
        if (cmd.m_button != PlayerButton::Jump) events.push_back({1, int(cmd.m_button), cmd.m_isPush, cmd.m_isPlayer2, false, false});
    }
    Body* iconBody(GJBaseGameLayer* play, bool p2) { return p2 ? play->m_player2 : play->m_player1; }
    static bool buttonHeld(Body* body, int button) { return body->held[button]; }
    bool heldAt(Logic const& value, bool p2, int frame, int button) {
        bool held = false;
        for (auto const& input : value.inputs)
            if (input.player2 == p2 && input.number <= frame && input.button == button) held = input.holding;
        return held;
    }
    void recordInput(GJBaseGameLayer*, bool down, bool p1, int button) { events.push_back({2, button, down, !p1, false, false}); }
    void applyMaintainGravity(GJBaseGameLayer*) { events.push_back({3, 0, false, false, false, false}); }
    void syncSameGravity(GJBaseGameLayer*) { events.push_back({4, 0, false, false, false, false}); }
    void prepareButtons(GJBaseGameLayer*);
    void pumpButtons(GJBaseGameLayer*, GJBaseGameLayer*, float, bool);
};
void GJBaseGameLayer::processQueuedButtons(float, bool clear) {
    auto& value = Logic::get();
    for (auto const& cmd : m_queuedButtons) {
        events.push_back({0, int(cmd.m_button), cmd.m_isPush, cmd.m_isPlayer2, value.applyingPlayback, value.injecting});
        (cmd.m_isPlayer2 ? p2 : p1).held[int(cmd.m_button)] = cmd.m_isPush;
        if (autoDual && m_gameState.m_isDualMode && !settings.m_twoPlayerMode && !cmd.m_isPlayer2) {
            ++value.partnerPresses;
            p2.held[int(cmd.m_button)] = cmd.m_isPush;
        }
    }
    if (clear) m_queuedButtons.clear();
}
namespace Orbs {
bool hasPendingPads() { return !pads.empty(); }
void flushPads(PlayLayer* play) { play->m_queuedButtons.insert(play->m_queuedButtons.end(), pads.begin(), pads.end()); pads.clear(); }
void noteJumpPresses(PlayLayer*) {}
void commitToolHits() { ++commits; }
}
// PRODUCTION_PIPELINE

PlayerButtonCommand cmd(bool down, bool p2 = false, int button = 1) {
    PlayerButtonCommand value;
    value.m_isPush = down; value.m_isPlayer2 = p2; value.m_button = PlayerButton(button);
    return value;
}
void reset() { Logic::get() = {}; events.clear(); pads.clear(); autoDual = false; commits = 0; }
void run(PlayLayer& play, bool clear = true) { Logic::get().pumpButtons(&play, &play, 1.f / 240, clear); }
int main() {
    // Same-frame press/release/press must reach the game as three distinct commands.
    for (int count : {3, 7, 8, 9, 40}) {
        reset(); PlayLayer play; auto& value = Logic::get(); value.playing = true;
        for (int i = 0; i < count; ++i) value.inputs.push_back({1, i % 2 == 0, false});
        run(play);
        assert(events.size() == static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) assert((events[i] == Event{0, 1, i % 2 == 0, false, true, false}));
        assert(value.replayPos == value.inputs.size() && play.m_queuedButtons.empty());
    }
    // Live P1 recording coexists with P2 playback; physical movement is noted before steering.
    reset(); PlayLayer play; auto& value = Logic::get();
    value.playing = value.recording = true; value.playP1 = false; play.settings.m_twoPlayerMode = true;
    play.m_queuedButtons = {cmd(true, true, 2)};
    value.inputs = {{1, true, true}};
    run(play);
    assert((events == std::vector<Event>{{1, 2, true, true, false, false}, {0, 2, true, false, false, false}, {0, 1, true, true, true, false}}));
    // Queues ignored during playback do not reach the game.
    reset(); PlayLayer ignored; value.playing = true; ignored.m_queuedButtons = {cmd(true)}; run(ignored);
    assert(events.empty());
    // A recording queue retained by the caller keeps its exact order and metadata.
    reset(); PlayLayer retained; value.recording = true;
    retained.m_queuedButtons = {cmd(true), cmd(false)};
    auto original = retained.m_queuedButtons; run(retained, false);
    assert(retained.m_queuedButtons == original && events.size() == 2);
    // The empty-input fast path still runs gravity correction, pad recording and tool processing.
    reset(); PlayLayer gravity; value.recording = true; value.maintainGravity = value.sameGravity = true;
    pads = {cmd(true), cmd(false)}; run(gravity);
    assert((events == std::vector<Event>{{3, 0, false, false, false, false}, {2, 1, true, false, false, false},
        {2, 1, false, false, false, false}, {0, 1, true, false, false, true},
        {0, 1, false, false, false, true}, {4, 0, false, false, false, false}}));
    assert(!value.injecting && !value.applyingPlayback && !value.suppressClickHitbox && !value.buttonsPrepared);
    // Maintain Gravity removes live jumps but preserves movement and replay clicks.
    reset(); PlayLayer filtered; value.recording = true; value.maintainGravity = true;
    filtered.m_queuedButtons = {cmd(true), cmd(true, false, 2)}; run(filtered);
    assert((events == std::vector<Event>{{1, 2, true, false, false, false}, {0, 2, true, false, false, false}, {3, 0, false, false, false, false}}));
    // A 1-player dual receives the fallback copy only when vanilla did not route it.
    for (bool native : {false, true}) {
        reset(); PlayLayer dual; value.playing = true; dual.m_gameState.m_isDualMode = true;
        autoDual = native; value.inputs = {{1, true, false}}; run(dual);
        assert(events.size() == (native ? 1u : 2u));
        assert(dual.p1.held[1] && dual.p2.held[1]);
    }
    // A pending orb flush must preserve the caller's queue, and record only injected commands.
    reset(); PlayLayer orb; value.recording = true; orb.m_queuedButtons = {cmd(true, false, 2)};
    original = orb.m_queuedButtons; pads = {cmd(true), cmd(false)}; runOrbClicks(&orb, &orb, 0);
    assert(orb.m_queuedButtons == original && events.size() == 4);
    assert(commits == 1 && !value.injecting && !value.suppressClickHitbox);
    std::cout << "Production button pipeline: ordering, routing, stack/overflow snapshots, gravity, pads and dual regression cases passed\n";
}
