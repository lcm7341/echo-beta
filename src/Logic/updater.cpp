#include "Logic/logic.hpp"
#include "Hack/trajectory.hpp"
#include "Physics/moving_objects.hpp"
#include "Physics/collision_bounds.hpp"
#include "Physics/prediction_art.hpp"
#include "Rendering/dash_end.hpp"
#include "Rendering/dash_bursts.hpp"
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

using namespace geode::prelude;

// Every physics tick asks which kind of layer it is in several times. The running level and
// editor are known, so those answer by address and only anything else pays for the type check.
PlayLayer* Logic::asPlay(GJBaseGameLayer* layer) {
	if (!layer) return nullptr;
	if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer) return play;
	if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer) return nullptr;
	return typeinfo_cast<PlayLayer*>(layer);
}

LevelEditorLayer* Logic::asEditor(GJBaseGameLayer* layer) {
	if (!layer) return nullptr;
	if (auto* editor = LevelEditorLayer::get(); editor && static_cast<GJBaseGameLayer*>(editor) == layer) return editor;
	if (auto* play = PlayLayer::get(); play && static_cast<GJBaseGameLayer*>(play) == layer) return nullptr;
	return typeinfo_cast<LevelEditorLayer*>(layer);
}

bool Logic::advancePaused(GJBaseGameLayer* play) {
	return play && Logic::get().frameAdvance && !Logic::get().renderCapturing();
}

bool Logic::lockDeltaActive() const {
	if (frameAdvance && !Logic::get().renderCapturing()) return true;
	return lockDelta;
}

// Ten display frames' worth of ticks, as in Silicate's visual updates. A flat cap of ten ticks
// would play a high TPS macro in slow motion.
int Logic::unlockedStepCap() const {
	auto* director = CCDirector::sharedDirector();
	double interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
	if (!std::isfinite(interval) || interval <= 0.0) interval = 1.0 / 60.0;
	// Real Time: at least a second of game time per frame, so a high frame rate cannot hold
	// catch-up below real speed. affordableSteps bounds the wall time those steps may take.
	if (realTimeMode && !(lockFps && playbackFps >= 1.f)) interval = std::max(interval, 0.1);
	double perFrame = std::ceil(sampleRate() * std::max(interval, 1.0 / 1000.0));
	return static_cast<int>(std::clamp(perFrame, 1.0, 100000.0)) * 10;
}

namespace {
using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start, Clock::time_point end) {
	return std::chrono::duration<double>(end - start).count();
}

// A step's cost is the gap from its start to the next step's start, so the work the game does
// once per update is measured apart from the work it does every step.
struct StepClock {
	Clock::time_point first;
	Clock::time_point last;
	int steps = 0;
	int sampledStep = 0;
	bool active = false;
};
struct FrameSample {
	double spans = 0.0;
	double gaps = 0.0;
	double updates = 0.0;
	double updateTime = 0.0;
	double steps = 0.0;
};
struct StepCost {
	double perStep = 0.0;
	double perUpdate = 0.0;
	double lastPhysics = 0.0;
	double otherWork = 0.0;
};
StepClock& stepClock() {
	static StepClock clock;
	return clock;
}
FrameSample& frameSample() {
	static FrameSample sample;
	return sample;
}
StepCost& stepCost() {
	static StepCost cost;
	return cost;
}

template <class Update>
void timeUpdate(Update&& update) {
	stepClock() = {};
	stepClock().active = true;
	auto started = Clock::now();
	update();
	stepClock().active = false;
	auto const& clock = stepClock();
	auto& sample = frameSample();
	sample.updates += 1.0;
	sample.updateTime += secondsSince(started, Clock::now());
	sample.steps += clock.steps;
	if (clock.sampledStep < 2) return;
	sample.spans += secondsSince(clock.first, clock.last);
	sample.gaps += clock.sampledStep - 1;
}

void settleCost() {
	auto sample = std::exchange(frameSample(), FrameSample{});
	auto& cost = stepCost();
	cost.lastPhysics = sample.updateTime;
	if (sample.gaps < 4.0) return;
	double perStep = sample.spans / sample.gaps;
	if (!(perStep > 0.0) || !std::isfinite(perStep)) return;
	double perUpdate = std::max(0.0, (sample.updateTime - sample.steps * perStep) / std::max(sample.updates, 1.0));
	bool fresh = !(cost.perStep > 0.0);
	cost.perStep = fresh ? perStep : cost.perStep + (perStep - cost.perStep) * 0.3;
	cost.perUpdate = fresh ? perUpdate : cost.perUpdate + (perUpdate - cost.perUpdate) * 0.3;
}

// The native setter placed trail, fire and particle nodes from the position on every step, and
// the last step's placement is the one that is drawn. Doing it once, with the final position,
// leaves the same nodes where the batch's last step would have.
void placeDeferredPlayerArt(GJBaseGameLayer* play) {
	auto& value = Logic::get();
	if (!std::exchange(value.playerArtDeferred, false)) return;
	for (auto* player : {play->m_player1, play->m_player2}) {
		if (player) player->setPosition(player->getPosition());
	}
}
}

void Logic::noteStep() {
	auto& clock = stepClock();
	if (!clock.active) return;
	++clock.steps;
	// Measure short batches exactly; in long batches sample every sixteenth step.
	// Count every step so fixed update overhead is still estimated separately.
	if (clock.steps > 8 && (clock.steps & 15) != 0) return;
	auto now = Clock::now();
	if (clock.steps == 1) clock.first = now;
	clock.last = now;
	clock.sampledStep = clock.steps;
}

// Everything a frame does besides physics: drawing, the menu, sounds, and the rest of the update.
void Logic::noteFrameWork(double seconds) {
	auto& cost = stepCost();
	double other = std::max(0.0, seconds - std::exchange(cost.lastPhysics, 0.0));
	if (!std::isfinite(other)) return;
	cost.otherWork = cost.otherWork > 0.0 ? cost.otherWork + (other - cost.otherWork) * 0.2 : other;
}

// When the steps a frame asks for cost more than the frame has, the game slows down either way.
// Taking only what fits keeps frames coming at the same game speed, instead of one frame a second.
// Lock FPS holds the frame rate itself, so physics only gets what the rest of the frame leaves.
int Logic::affordableSteps(bool lockVisual) const {
	auto const& cost = stepCost();
	if (!(cost.perStep > 0.0)) return std::numeric_limits<int>::max();
	auto* director = CCDirector::sharedDirector();
	double interval = director ? director->getAnimationInterval() : 1.0 / 60.0;
	if (!std::isfinite(interval) || interval <= 0.0) interval = 1.0 / 60.0;
	// Real Time puts game time before frames: while physics is behind, a displayed frame may hold
	// up to a tenth of a second of steps, so the fixed per-frame work is a small share of the time.
	if (!lockVisual && realTimeMode) {
		constexpr double kRealTimeFrame = 0.1;
		// When drawing and actions alone take longer than that (hundreds of thousands of effect
		// nodes), physics still gets at least as long as they do, up to a second: half of every
		// frame goes to game time instead of a sixth.
		double fixed = cost.perUpdate + cost.otherWork;
		double room = std::max({kRealTimeFrame - fixed, kRealTimeFrame * 0.5, std::min(fixed, 1.0)});
		double steps = std::floor(room / cost.perStep);
		if (!std::isfinite(steps) || steps >= static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
		return std::max(static_cast<int>(steps), 8);
	}
	double budget = lockVisual ? std::max(interval * 0.85 - cost.otherWork, interval * 0.25) : std::clamp(interval * 1.5, 0.004, 0.05);
	// Each frame pays a fixed cost besides its steps: the update's visibility and camera work and
	// the rest of the frame (drawing, the menu, sounds). When the game cannot keep up, giving steps
	// only as much again as that cost spent half of every frame on it. Without Lock FPS, physics
	// gets at least four fifths of the frame, within the same 50 ms frame cap.
	double fixed = cost.perUpdate + cost.otherWork;
	double floor = lockVisual ? cost.perUpdate : std::max(cost.perUpdate, std::min(fixed * 4.0, 0.05 - fixed));
	// When the fixed work alone passes the frame cap (hundreds of thousands of effect nodes),
	// physics still gets as long as it, up to a quarter second, instead of almost nothing.
	if (!lockVisual) floor = std::max(floor, std::min(fixed, 0.25));
	double room = std::max(budget - cost.perUpdate, floor);
	double steps = std::floor(room / cost.perStep);
	if (!std::isfinite(steps) || steps >= static_cast<double>(std::numeric_limits<int>::max())) return std::numeric_limits<int>::max();
	return std::max(static_cast<int>(steps), 8);
}

// Silicate's lock-delta-off path (BotUpdater::calculateSteps): one display frame,
// as many exact physics steps as fit, and the leftover time kept for the next frame.
int Logic::countUnlockedSteps(GJBaseGameLayer* play, double dt) {
	auto& value = Logic::get();
	if (!play) return 0;
	Logic::get().applyRate(value.stepFrame);
	float warp = std::min(play->m_gameState.m_timeWarp, 1.f);
	if (!std::isfinite(warp) || warp < 0.f) warp = 1.f;
	float wanted = value.physicsStep() * warp;
	if (!std::isfinite(wanted) || !(wanted > 0.f)) return 0;
	if (!std::isfinite(dt) || dt < 0.f) dt = 0.f;
	if (value.respawnFrames > 0) {
		value.carry = 0.f;
		play->m_extraDelta = 0.0;
	}
	if (!std::isfinite(value.carry) || value.carry < 0.f) value.carry = 0.f;
	double pending = value.carry + dt;
	if (!std::isfinite(pending) || pending < 0.f) pending = 0.f;
	double requested = std::floor(pending / wanted);
	int steps = static_cast<int>(std::min(requested, double(std::numeric_limits<int>::max())));
	if (steps < 0) steps = 0;
	bool capped = false;
	bool lockVisual = value.lockFps && value.playbackFps >= 1.f;
	if (!Logic::get().renderCapturing()) {
		int affordable = Logic::get().affordableSteps(lockVisual);
		int cap = std::max(std::min(Logic::get().unlockedStepCap(), affordable) - value.frameSteps, 0);
		capped = steps > cap;
		steps = std::min(steps, cap);
	}
	// Real Time retains EVERY unfinished tick. Only the existing non-real-time
	// slowdown mode discards excess wall time when its budget is exhausted.
	value.carry = capped && !value.realTimeMode ? 0.f : pending - static_cast<double>(steps) * wanted;
	if (!std::isfinite(value.carry) || value.carry < 0.f) value.carry = 0.f;
	if (value.respawnFrames > 0) {
		--value.respawnFrames;
		steps = std::min(steps, 1);
	}
	return steps;
}

void Logic::runFrame(GJBaseGameLayer* play, float dt, std::function<void(float)> const& tick) {
	if (Logic::get().renderPaused()) return;
	auto& value = Logic::get();
	if (!play) {
		tick(dt);
		return;
	}
	echo::movement::prepare();
#ifdef GEODE_IS_WINDOWS
	echo::dash::prepare();
	echo::bursts::prepare();
	echo::collision_bounds::prepare();
#endif
	value.frameSteps = 0;
	bool paused = Logic::get().editorPaused;
	if (auto* live = Logic::asPlay(play)) paused = paused || live->m_isPaused;
	if (auto* live = Logic::asPlay(play); live && !live->m_started && !Logic::get().renderCapturing()) {
		value.carry = 0.f;
		value.stepsLeft = 0;
		tick(dt);
		return;
	}
	if (paused) {
		value.carry = 0.f;
		tick(dt);
		return;
	}
	// The death timer and auto-retry live in the scheduled update. Skipping it freezes the attempt.
	// syncTimeScale puts speedhack on the scheduler while dead so this stays at the chosen rate.
	if (auto* live = Logic::asPlay(play); live && live->m_playerDied) {
		value.carry = 0.f;
		value.stepsLeft = 0;
		value.stepsThisTick = 0;
		// Frame advance holds the death where it is: the update would run the respawn timer and
		// reset the level. An advance press has nothing to step while dead. Turning frame advance
		// off, a backstep or a manual reset carries on from here.
		if (Logic::get().advancePaused(play)) {
			value.advanceQueued = false;
			Trajectory::get().refresh(PlayLayer::get());
			return;
		}
		Logic::get().syncTimeScale();
		tick(dt);
		Trajectory::get().refresh(PlayLayer::get());
		return;
	}
	if (Logic::get().deferPhysics()) {
		value.carry = 0.f;
		tick(dt);
		Trajectory::get().refresh(PlayLayer::get());
		return;
	}

	bool forceStep = false;
	if (Logic::get().advancePaused(play)) {
		value.carry = 0.f;
		value.respawnFrames = 0;
		play->m_extraDelta = 0.0;
		if (!value.advanceQueued) {
			Trajectory::get().refresh(PlayLayer::get());
			return;
		}
		forceStep = true;
	}

	float speed = Logic::get().renderActive() ? 1.f : std::max(value.speedhack, 0.01f);
	if (!std::isfinite(speed) || speed < 0.01f) speed = 1.f;
	float frameDt = dt;
	if (!std::isfinite(frameDt) || frameDt < 0.f) frameDt = 0.f;
	bool rendering = Logic::get().renderCapturing();
	// Lock FPS moves the game the same amount every frame, and the frame rate is held by giving
	// physics only the time the frame has left. A heavy part slows the game instead of the frames.
	bool lockVisual = !rendering && value.lockFps && value.playbackFps >= 1.f;
	if (lockVisual) {
		// Speed lives on the animation interval while Lock FPS is on. Each visual frame
		// still advances one locked quantum so the step cap does not eat the keybind speed.
		frameDt = 1.f / value.playbackFps;
	} else {
		if (!rendering && !value.realTimeMode && frameDt > 0.25f) frameDt = 0.25f;
		frameDt *= speed;
	}

	auto died = [&] {
		auto* live = Logic::asPlay(play);
		return live && live->m_playerDied;
	};
	auto tickLength = [&] {
		return value.physicsStep() * std::min(play->m_gameState.m_timeWarp, 1.f);
	};
	// Pause Before Kill: use the last trajectory draw's death-next / alive length — no extra
	// predict. Cap this frame's steps so a batch cannot skip the frame before death.
	// Align with the "Death next frame" label: only stop when deathNext is set (0 surviving
	// steps). Do not subtract an extra step — that paused 1–2 frames early.
	auto pauseKillSafeSteps = [&]() -> int {
		if (!value.pauseBeforeKill || !value.isRecording() || rendering) return std::numeric_limits<int>::max();
		auto watch = [&](bool player2) {
			if (player2 && !(play->m_gameState.m_isDualMode && play->m_player2)) return false;
			if (value.isRecording()) return value.recordsPlayer(player2);
			return true;
		};
		if ((watch(false) && value.deathNextP1) || (watch(true) && value.deathNextP2)) return 0;
		bool haveTraj = value.trajDiesP1 || value.trajDiesP2 || value.trajAliveP1 > 0.f || value.trajAliveP2 > 0.f;
		if (!haveTraj) return std::numeric_limits<int>::max();
		float alive = std::numeric_limits<float>::infinity();
		if (watch(false)) alive = std::min(alive, value.trajAliveP1);
		if (watch(true)) alive = std::min(alive, value.trajAliveP2);
		float stepSec = tickLength();
		if (!(stepSec > 0.f) || !std::isfinite(alive)) return std::numeric_limits<int>::max();
		int safe = static_cast<int>(std::floor(static_cast<double>(alive / stepSec) + 1e-9));
		return std::max(0, safe);
	};
	// Pause Before Kill steps one at a time. Without a drawn trajectory, a one-step lookahead of the
	// live input before each step says whether that step kills.
	bool pauseKill = value.pauseBeforeKill && value.isRecording() && !rendering;
	auto lookahead = [&] {
		if (pauseKill && !value.showTrajectory) Trajectory::get().lookahead(PlayLayer::get());
	};
	if (!forceStep && pauseKill) {
		auto* live = PlayLayer::get();
		lookahead();
		Logic::get().checkAdvancePause(live);
		if (Logic::get().advancePaused(play) && !value.advanceQueued) {
			Trajectory::get().refresh(live);
			return;
		}
	}
	// Lock TPS off keeps the old speed: one layer update holds every step that fits in the frame.
	// The game splits that update into steps, and each step still takes its own click. The scheduler
	// then moves by exactly those steps, so a different FPS or speedhack does not change the clock.
	// Lock TPS on stays one update per step.
	// Pause Before Kill cannot batch — a multi-step update can skip the frame before death.
	// Without the step patch only 240 TPS batches; other rates take the one-update-per-step loop.
	bool canBatch = kStepPatch || value.sampleRate() == kVanillaTps;
	if (!forceStep && !Logic::get().lockDeltaActive() && !pauseKill && canBatch) {
		int steps = Logic::get().countUnlockedSteps(play, frameDt);
		steps = std::min(steps, pauseKillSafeSteps());
		if (steps <= 0) {
			Logic::get().checkAdvancePause(PlayLayer::get());
			// Keep carry so slow speedhack can accumulate toward the next step.
			value.present = true;
			Trajectory::get().refresh(PlayLayer::get());
			return;
		}
		while (steps > 0) {
			if (died()) break;
			int batch = std::min(steps, value.stepsUntilRateChange());
			float left = static_cast<float>(steps - batch) * tickLength();
			float stepDt = value.physicsStep();
			value.stepsLeft = batch;
			value.stepsThisTick = static_cast<uint32_t>(batch);
			value.frameSteps += batch;
			value.deferPlayerArt = batch > 1 && phys::fastPredictionPosition();
			timeUpdate([&] { play->update(stepDt * static_cast<float>(batch)); });
			value.deferPlayerArt = false;
			placeDeferredPlayerArt(play);
			value.stepsLeft = 0;
			value.skipPhysics = true;
			tick(stepDt * static_cast<float>(batch));
			value.skipPhysics = false;
			if (died() || batch == steps) break;
			double carry = value.carry;
			value.carry = 0.f;
			steps = Logic::get().countUnlockedSteps(play, left + carry);
			steps = std::min(steps, pauseKillSafeSteps());
		}
		settleCost();
		value.frameSteps = 0;
		value.stepsLeft = 0;
		value.stepsThisTick = 0;
		value.present = true;
		Trajectory::get().refresh(PlayLayer::get());
		return;
	}

	if (!forceStep) {
		int steps = Logic::get().countUnlockedSteps(play, frameDt);
		steps = std::min(steps, pauseKillSafeSteps());
		if (steps <= 0) {
			Logic::get().checkAdvancePause(PlayLayer::get());
			// Keep carry so slow speedhack can accumulate toward the next step.
			value.present = true;
			Trajectory::get().refresh(PlayLayer::get());
			return;
		}
		while (steps > 0) {
			if (died()) break;
			int batch = std::min(steps, value.stepsUntilRateChange());
			float left = static_cast<float>(steps - batch) * tickLength();
			bool finished = false;
			for (int i = 0; i < batch; ++i) {
				if (died()) {
					finished = true;
					break;
				}
				// Re-check the last trajectory step budget — do not re-simulate mid-frame.
				if (pauseKill && i > 0) lookahead();
				if (pauseKill && pauseKillSafeSteps() <= 0) {
					Logic::get().checkAdvancePause(PlayLayer::get());
					finished = true;
					break;
				}
				float stepDt = value.physicsStep();
				value.stepsLeft = 1;
				value.stepsThisTick = 1;
				++value.frameSteps;
				timeUpdate([&] { play->update(stepDt); });
				value.stepsLeft = 0;
				value.skipPhysics = true;
				tick(stepDt);
				value.skipPhysics = false;
				if (died()) {
					finished = true;
					break;
				}
				// Account for the step we just took against the last traj alive length.
				if (pauseKill) {
					float stepSec = tickLength();
					if (value.trajAliveP1 > 0.f) value.trajAliveP1 = std::max(0.f, value.trajAliveP1 - stepSec);
					if (value.trajAliveP2 > 0.f) value.trajAliveP2 = std::max(0.f, value.trajAliveP2 - stepSec);
					// Same meaning as trajectory's deathNext: zero surviving steps left.
					if (value.trajAliveP1 <= 1e-6f && value.trajDiesP1) value.deathNextP1 = true;
					if (value.trajAliveP2 <= 1e-6f && value.trajDiesP2) value.deathNextP2 = true;
					if (pauseKillSafeSteps() <= 0) {
						Logic::get().checkAdvancePause(PlayLayer::get());
						finished = true;
						break;
					}
				}
			}
			if (finished || batch == steps) break;
			double carry = value.carry;
			value.carry = 0.f;
			steps = Logic::get().countUnlockedSteps(play, left + carry);
			steps = std::min(steps, pauseKillSafeSteps());
		}
		settleCost();
		value.frameSteps = 0;
		value.stepsLeft = 0;
		value.stepsThisTick = 0;
		value.present = true;
		Trajectory::get().refresh(PlayLayer::get());
		return;
	}

	// A press advances exactly one physics tick regardless of display delta,
	// speedhack, TPS, time warp, or Lock FPS. Only held repeat supplies more.
	int limit = std::max(value.advanceSteps, 1);
	value.advanceQueued = false;
	value.advanceSteps = 1;
	value.carry = 0.f;
	play->m_extraDelta = 0.0;
	value.stepsLeft = 0;
	for (int taken = 0; taken < limit; ++taken) {
		if (died()) break;
		// Holding the advance key steps many times; Pause Before Kill still stops before a death.
		if (pauseKill && value.holdingAdvance && value.backstepTarget < 0) {
			Trajectory::get().lookahead(PlayLayer::get());
			Logic::get().checkAdvancePause(PlayLayer::get());
			if (!value.holdingAdvance) break;
		}
		Logic::get().applyRate(value.stepFrame);
		float tickDt = value.physicsStep();
		if (!std::isfinite(tickDt) || !(tickDt > 0.f) || tickDt > 1.f) break;
		value.stepsLeft = 1;
		value.stepsThisTick = 1;
		tick(tickDt);
		value.stepsLeft = 0;
		value.stepsThisTick = 0;
	}
	// These steps replayed a backstep's recorded inputs up to the frame stepped back to.
	if (value.backstepTarget >= 0) Logic::get().finishBackstep(Logic::asPlay(play));
	value.present = true;

	Trajectory::get().refresh(PlayLayer::get());
}

void Logic::cancelSteps() {
	auto& value = Logic::get();
	value.stepsLeft = 0;
	value.carry = 0.f;
}

void Logic::flushVisualRefresh() {
	auto& value = Logic::get();
	if (!value.refreshQueued) return;
	auto* director = CCDirector::sharedDirector();
	if (!director || !director->getScheduler()) return;
	value.refreshQueued = false;
	value.refreshOnly = true;
	director->getScheduler()->update(0.f);
	value.refreshOnly = false;
}
