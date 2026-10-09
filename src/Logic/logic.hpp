#pragma once

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

class PlayLayer;
class PlayerObject;
struct PlayerButtonCommand;
class GameObject;
class EnhancedGameObject;
class GJBaseGameLayer;
class LevelEditorLayer;
class TeleportPortalObject;

// ARM64 enables batching only after verifying and installing its instruction patch.
// Other unpatched builds retain the vanilla 240 TPS minimum and per-step updates.
#ifdef GEODE_IS_WINDOWS
inline constexpr bool kStepPatch = true;
#else
inline bool kStepPatch = false;
#endif
inline constexpr float kVanillaTps = 240.f;

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

// One click. Pose and editor ids live beside this, and only when something actually asked for them.
struct Frame {
	int number = 0;
	uint8_t button = 1;
	bool holding = false;
	bool player2 = false;
};
struct FramePose {
	float x = 0.f;
	float y = 0.f;
	float rotation = 0.f;
	double yVelocity = 0.0;
	double xVelocity = 0.0;
};

struct Replay {
	std::string name;
	std::vector<Frame> actions;
};

struct AutoClick {
	int pressInterval = 50;
	int releaseInterval = 50;
	int sincePress = 0;
	int sinceRelease = 0;
	bool pressed = false;
	int disableIn = 1;
};

struct LabelPlacement {
	float x = 0.f;
	float y = 0.f;
	float scale = 0.4f;
	int opacity = 40;
};

struct PlayerSnapshot {
	bool present = false;
	float x = 0.f;
	float y = 0.f;
	float rotation = 0.f;
	float visualRotation = 0.f;
	double yVelocity = 0.0;
	double platformerXVelocity = 0.0;
	double yVelocityRelated = 0.0;
	double fallSpeed = 0.0;
	double gravity = 0.0;
	double speedMultiplier = 0.0;
	double yStart = 0.0;
	double maybeReverseSpeed = 0.0;
	double maybeReverseAcceleration = 0.0;
	double accelerationOrSpeed = 0.0;
	double snapDistance = 0.0;
	double lastFlipTime = 0.0;
	double gameModeChangedTime = 0.0;
	double lastLandTime = 0.0;
	double totalTime = 0.0;
	float playerSpeed = 0.f;
	float playerSpeedAC = 0.f;
	float vehicleSize = 1.f;
	float gravityMod = 1.f;
	float xVelocityRelated = 0.f;
	float xVelocityRelated2 = 0.f;
	float yVelocityRelated3 = 0.f;
	float rotationSpeed = 0.f;
	decltype(PlayerObject::m_rotateSpeed) rotateSpeed{};
	decltype(PlayerObject::m_isRotating) rotating{};
	decltype(PlayerObject::m_isBallRotating) ballRotating{};
	decltype(PlayerObject::m_isBallRotating2) ballRotating2{};
	decltype(PlayerObject::m_slopeRotation) slopeRotation{};
	float slopeVelocity = 0.f;
	cocos2d::CCPoint shipRotation;
	cocos2d::CCPoint lastPortalPos;
	bool ship = false;
	bool bird = false;
	bool ball = false;
	bool dart = false;
	bool robot = false;
	bool spider = false;
	bool swing = false;
	bool upsideDown = false;
	bool onGround = false;
	bool onGround2 = false;
	bool onGround3 = false;
	bool onGround4 = false;
	bool goingLeft = false;
	bool sideways = false;
	bool hidden = false;
	bool dashing = false;
	bool dead = false;
	bool jumpBuffered = false;
	bool onSlope = false;
	bool wasOnSlope = false;
	bool wasTeleported = false;
	bool fixGravityBug = false;
	bool reverseSync = false;
	bool locked = false;
	bool controlsDisabled = false;
	bool holdingRight = false;
	bool holdingLeft = false;
	int reverseRelated = 0;
	// Platformer movement. Without these a restore keeps the speed but loses the turn, slide and boost state.
	decltype(PlayerObject::m_leftPressedFirst) leftPressedFirst{};
	decltype(PlayerObject::m_platformerMovingLeft) movingLeft{};
	decltype(PlayerObject::m_platformerMovingRight) movingRight{};
	decltype(PlayerObject::m_platformerVelocityRelated) platformerVelocityRelated{};
	decltype(PlayerObject::m_maybeIsBoosted) boosted{};
	decltype(PlayerObject::m_scaleXRelatedTime) scaleXRelatedTime{};
	decltype(PlayerObject::m_decreaseBoostSlide) decreaseBoostSlide{};
	decltype(PlayerObject::m_scaleXRelated) scaleXRelated{};
	decltype(PlayerObject::m_scaleXRelated2) scaleXRelated2{};
	decltype(PlayerObject::m_scaleXRelated3) scaleXRelated3{};
	decltype(PlayerObject::m_scaleXRelated4) scaleXRelated4{};
	decltype(PlayerObject::m_scaleXRelated5) scaleXRelated5{};
	decltype(PlayerObject::m_maybeHasStopped) hasStopped{};
	decltype(PlayerObject::m_maybeGoingCorrectSlopeDirection) correctSlopeDirection{};
	decltype(PlayerObject::m_isSliding) sliding{};
	decltype(PlayerObject::m_maybeSlopeForce) slopeForce{};
	decltype(PlayerObject::m_isOnIce) onIce{};
	decltype(PlayerObject::m_physDeltaRelated) physDeltaRelated{};
	decltype(PlayerObject::m_maybeSlidingTime) slidingTime{};
	decltype(PlayerObject::m_maybeSlidingStartTime) slidingStartTime{};
	decltype(PlayerObject::m_changedDirectionsTime) changedDirectionsTime{};
	decltype(PlayerObject::m_slopeEndTime) slopeEndTime{};
	decltype(PlayerObject::m_isMoving) moving{};
	decltype(PlayerObject::m_isSlidingRight) slidingRight{};
	decltype(PlayerObject::m_maybeChangedDirectionAngle) changedDirectionAngle{};
	decltype(PlayerObject::m_groundYVelocity) groundYVelocity{};
	decltype(PlayerObject::m_yVelocityBeforeSlope) yVelocityBeforeSlope{};
	decltype(PlayerObject::m_slopeStartTime) slopeStartTime{};
	decltype(PlayerObject::m_lastGroundedPos) lastGroundedPos{};
	decltype(PlayerObject::m_isAccelerating) accelerating{};
	decltype(PlayerObject::m_groundObjectMaterial) groundMaterial{};
	decltype(PlayerObject::m_somethingPlayerSpeedTime) playerSpeedTime{};
	decltype(PlayerObject::m_affectedByForces) affectedByForces{};
	decltype(PlayerObject::m_stateForce) stateForce{};
	decltype(PlayerObject::m_stateForceVector) stateForceVector{};
	decltype(PlayerObject::m_stateBoostX) stateBoostX{};
	decltype(PlayerObject::m_stateBoostY) stateBoostY{};
	// Portal, dash and buffered-input state needed when resuming a recording.
	decltype(PlayerObject::m_stateFlipGravity) stateFlipGravity{};
	decltype(PlayerObject::m_stateScale) stateScale{};
	decltype(PlayerObject::m_stateDartSlide) stateDartSlide{};
	decltype(PlayerObject::m_stateNoAutoJump) stateNoAutoJump{};
	decltype(PlayerObject::m_stateRingJump) stateRingJump{};
	decltype(PlayerObject::m_stateRingJump2) stateRingJump2{};
	decltype(PlayerObject::m_wasJumpBuffered) wasJumpBuffered{};
	decltype(PlayerObject::m_stateJumpBuffered) stateJumpBuffered{};
	// Restore orb/jump transitions from the checkpoint, never from the death frame.
	decltype(PlayerObject::m_ringJumpRelated) ringJumpRelated{};
	decltype(PlayerObject::m_touchedRing) touchedRing{};
	decltype(PlayerObject::m_touchedCustomRing) touchedCustomRing{};
	decltype(PlayerObject::m_touchedPad) touchedPad{};
	decltype(PlayerObject::m_wasRobotJump) wasRobotJump{};
	decltype(PlayerObject::m_blackOrbRelated) blackOrbRelated{};
	decltype(PlayerObject::m_dashX) dashX{};
	decltype(PlayerObject::m_dashY) dashY{};
	decltype(PlayerObject::m_dashAngle) dashAngle{};
	decltype(PlayerObject::m_dashStartTime) dashStartTime{};
	decltype(PlayerObject::m_dashRing) dashRing{};
	decltype(PlayerObject::m_spriteWidthScale) spriteWidthScale{};
	decltype(PlayerObject::m_spriteHeightScale) spriteHeightScale{};
	float scaleX = 1.f;
	float scaleY = 1.f;
	cocos2d::CCPoint internalPosition;
	cocos2d::CCPoint lastPosition;
};

struct LayerSnapshot {
	double levelTime = 0.0;
	double totalTime = 0.0;
	double unkTime = 0.0;
	double attemptTime = 0.0;
	unsigned progress = 0;
	float timeWarp = 1.f;
	float queuedTimeWarp = 1.f;
	float timeWarpRelated = 0.f;
	float timeMod = 0.f;
	float cameraAngle = 0.f;
	float levelFlipping = 0.f;
	cocos2d::CCPoint cameraPosition;
	bool dual = false;
	bool streakBlend = false;
	bool gravityRelated = false;
	bool timeModRelated = false;
};

// Each object's move state at the first step from a practice checkpoint (Logic/object_states.cpp).
struct ObjectStates {
	int frame = -1;
	std::vector<GameObject*> objects;
	std::vector<std::uint8_t> bytes;
};

struct Checkpoint {
	CheckpointObject* object = nullptr; // Identity only; the game owns the checkpoint.
	int frame = 0;
	size_t replayPos = 0;
	int removedFrames = 0;
	bool heldP1 = false;
	bool heldP2 = false;
	bool heldLeftP1 = false;
	bool heldLeftP2 = false;
	bool heldRightP1 = false;
	bool heldRightP2 = false;
	PlayerSnapshot player1;
	PlayerSnapshot player2;
	LayerSnapshot layer;
	uint64_t randomState = 0;
	uint64_t shakeRandom = 0;
	uint64_t teleportRandom = 0;
	std::vector<float> variance;
	std::vector<std::pair<int, uint64_t>> randStates;
	std::shared_ptr<ObjectStates> objects;
};


struct CpsNote {
	int first = 0;
	int last = 0;
	std::string text;
};

struct RateChange {
	int frame = 0;
	float fps = 240.f;
};

struct HitboxTrailPoint {
	cocos2d::CCRect rect;
	cocos2d::CCRect inner;
	std::array<cocos2d::CCPoint, 4> corners{};
	cocos2d::CCPoint center{};
	float radius = 0.f;
	bool rotated = false;
	int frame = 0;
};

struct BackstepExtra; // src/Logic/backstep.cpp

struct Session {
	RunState state = RunState::Idle;
	ReplayFormat format = ReplayFormat::Meta;
	std::vector<Frame> inputs;
	// Debug samples, one per input, only while Save Debug is on or a file had them.
	std::vector<FramePose> poses;
	// Editor row ids. Empty until the editor names a row, so recording does not keep one per click.
	std::vector<uint64_t> inputIds;
	// Latest hold per player and button, so recording can reject a duplicate without a second copy of the macro.
	std::array<int, 6> liveFrame{-1, -1, -1, -1, -1, -1};
	std::array<char, 6> liveHeld{};
	std::array<char, 6> liveSeen{};
	int newestInput = -1;
	int holdCount = 0;
	int releaseCount = 0;
	bool inputsInOrder = true;
	// Anything that edits, reorders, or replaces inputs other than appending at the end bumps this,
	// so the lookups built over the list know to start over.
	uint64_t inputsRevision = 0;
	uint64_t nextInputId = 0;
	std::deque<int> clicksP1;
	std::deque<int> clicksP2;
	// Bumped when clicks are removed. Appending does not need it.
	uint64_t clicksRevision = 0;
	std::vector<Checkpoint> checkpoints;
	// One saved state every backstepInterval() steps, oldest first. Each holds a retained native
	// checkpoint (the whole level) and Echo's own part of it.
	struct BackstepSave {
		CheckpointObject* object = nullptr;
		Checkpoint echo;
		// What Silicate's backstep puts back after the native load (src/Logic/backstep.cpp).
		std::shared_ptr<BackstepExtra> extra;
		// What the grouped objects held here, for those that changed before the next save
		// (src/Logic/object_states.cpp). Empty on the newest save.
		std::vector<std::uint8_t> objectUndo;
		bool broken = false; // Its game state failed the check (Logic/state_check.hpp).
	};
	std::deque<BackstepSave> backsteps;
	bool backstepping = false; // Inside the resetLevel that loads a backstep save.
	int backstepTarget = -1; // Frame a backstep is re-simulating to, or -1.
	RunState backstepState = RunState::Idle;
	bool backstepPlayP1 = false;
	bool backstepPlayP2 = false;
	bool holdingBack = false;
	// Outside recording and playback nothing keeps the clicks, so backstep keeps its own copy of
	// each step's queued buttons to replay. Saves belong to the run state they were made in.
	std::vector<Frame> backstepInputs;
	std::vector<FramePose> backstepPoses;
	std::vector<uint64_t> backstepIds;
	size_t backstepReplayPos = 0;
	RunState backstepSavedState = RunState::Idle;
	bool backstepSwapped = false;
	std::chrono::steady_clock::time_point backHoldStart{};
	std::chrono::steady_clock::time_point lastBackStep{};
	CheckpointObject* restoredCheckpoint = nullptr;
	std::vector<Replay> sequence;
	std::vector<size_t> sequenceOrder;
	size_t replayPos = 0;
	size_t replayIndex = 0;
	int stepFrame = 0;
	int removedFrames = 0;
	int progressBase = 0;
	bool progressBaseSet = false;
	bool inStep = false;
	uint32_t partnerPresses = 0;
	float fps = 240.f;
	float macroFps = 240.f;
	std::vector<RateChange> rates;
	int accuracy = 1;
	bool present = true;
	bool resetting = false;
	double carry = 0.0;
	int stepsLeft = 0;
	bool skipPhysics = false;
	// Set while one layer update runs several steps: the players' setter skips visual-only
	// placement, and runFrame places it once after the batch at the final position.
	bool deferPlayerArt = false;
	bool playerArtDeferred = false;
	uint32_t stepsThisTick = 0;
	// Steps runFrame has already run this display frame. Its later recounts (after a rate change
	// or a 2048-step chunk) only get what is left of the frame's budget, not a fresh one.
	int frameSteps = 0;
	int respawnFrames = 0;
	int ignoreRecordFrames = 0;
	bool restoringHolds = false;
	bool refreshOnly = false;
	bool refreshQueued = false;
	bool inScheduler = false;
	// Frame advance: save the next step's starting state (the one on screen) for backstep.
	bool saveShownStep = false;
	bool buttonsPrepared = false;
	int replayClicksLeft = 0;
	float speedhack = 1.f;
	// Last non-1x speed for the keybind toggle. 0 means unset (do not invent a default).
	float speedhackMemory = 0.f;
	bool realTimeMode = true;
	bool lockDelta = false;
	bool lockFps = false;
	bool fixTpsRotation = false;
	// Above 240 TPS, share CPU state for identical dash effects; all effects are still drawn.
	// Draw dash end effects on the GPU instead of as sprites. Every effect is still shown.
	bool lockDisplayFps = false;
	float playbackFps = 0.f;
	bool playbackFpsHeld = false;
	float heldInterval = 1.f / 60.f;
	bool saveDebug = false;
	bool showClockDebug = false;
	std::vector<std::string> tabOrder{"Play", "Macro", "Editor", "Macro Tools", "Visuals", "Sequence", "Render", "Theme", "Clickbot"};
	bool ignoreInputs = true;
	bool noOverwrite = false;
	bool recordP1 = true;
	bool recordP2 = true;
	bool playP1 = true;
	bool playP2 = true;
	bool clickBoth = false;
	// Follow Player: 0 off, 1 player 2 copies player 1, 2 player 1 copies player 2, followDelay later.
	int followMode = 0;
	float followDelay = 10.f;
	bool followDelayMs = false;
	struct FollowPress {
		int frame;
		double time; // the level time of the step that takes the press
		bool down;
		int button;
		bool sent = false;
	};
	// The leader's presses, in order, and whether the follower has had each copy yet.
	std::deque<FollowPress> followLog;
	bool followInjecting = false;
	bool maintainGravity = false;
	bool sameGravity = false;
	bool sameGravitySeen = false;
	bool sameGravityWasP1 = false;
	bool sameGravityWasP2 = false;
	bool maintainingGravity = false;
	// The other icon of a 1-player dual gets the same click. That copy is not a second recorded input.
	bool mirrorClick = false;
	bool suppressClickHitbox = false;
	bool suppressRings = false;
	bool restartHoldCaptured = false;
	bool hideP1 = false;
	bool hideP2 = false;
	bool hideHitboxP1 = false;
	bool hideHitboxP2 = false;
	bool hitboxesFollowHidePlayer = true;
	bool hideTrajectoryP1 = false;
	bool hideTrajectoryP2 = false;
	bool trajectoryFollowHidePlayer = true;
	bool disableDeathEffect = false;
	// Any hack used during an attempt keeps it from making level progress (safe_mode.cpp).
	bool safeMode = false;
	bool swapPlayers = false;
	bool inverseP1 = false;
	bool inverseP2 = false;
	bool noclipP1 = false;
	bool noclipP2 = false;
	bool showHitboxes = false;
	bool fillHitboxes = false;
	// Overlapping hitboxes draw as one outline: lines inside other hitboxes are hidden.
	bool hitboxOutlineOnly = false;
	bool hitboxTrail = false;
	bool hitboxTrailVisual = false;
	bool hitboxDedup = false;
	int visualFrame = 0;
	float hitboxThickness = 1.f;
	float hitboxScale = 1.f;
	bool sortAutomatically = true;
	int trailLength = 50;
	bool hitboxPlayer = true;
	bool hitboxRotated = true;
	bool hitboxInner = true;
	bool hitboxCircle = true;
	bool hitboxSolid = true;
	bool hitboxSlope = true;
	bool hitboxHazard = true;
	bool hitboxInteract = true;
	bool hideActivatedObjects = false;
	bool hitboxClick = true;
	float hitboxFillOpacity = 0.22f;
	float hitboxTrailOpacity = 0.35f;
	float hitboxOuterColor[4] = {1.f, 1.f, 1.f, 1.f};
	float hitboxRotatedColor[4] = {1.f, 0.25f, 0.25f, 1.f};
	float hitboxInnerColor[4] = {0.35f, 1.f, 0.45f, 1.f};
	float hitboxCircleColor[4] = {0.45f, 0.75f, 1.f, 1.f};
	float hitboxSolidColor[4] = {0.15f, 0.35f, 1.f, 1.f};
	float hitboxSlopeColor[4] = {0.45f, 0.55f, 1.f, 1.f};
	float hitboxHazardColor[4] = {1.f, 0.15f, 0.15f, 1.f};
	float hitboxTrailColor[4] = {1.f, 1.f, 1.f, 0.35f};
	float hitboxInteractColor[4] = {0.2f, 1.f, 0.35f, 1.f};
	float hitboxClickColor[4] = {1.f, 0.85f, 0.15f, 1.f};
	float hitboxReleaseColor[4] = {0.35f, 0.7f, 1.f, 1.f};
	bool predicting = false;
	bool showedNewBest = false;
	bool showTrajectory = false;
	bool trajectoryHold = true;
	bool trajectoryRelease = true;
	bool trajectorySwift = false;
	float trajectorySeconds = 1.f;
	float trajectoryWidth = 0.5f;
	float trajectoryColor[4] = {0.f, 1.f, 0.f, 1.f};
	float trajectorySwiftColor[4] = {1.f, 0.75f, 0.15f, 1.f};
	float trajectoryReleaseColor[4] = {0.25f, 0.65f, 1.f, 1.f};
	float editorP1[4] = {0.20f, 0.55f, 0.95f, 1.f};
	float editorP2[4] = {0.95f, 0.45f, 0.25f, 1.f};
	float editorSelP1[4] = {0.55f, 0.82f, 1.f, 1.f};
	float editorSelP2[4] = {1.f, 0.72f, 0.45f, 1.f};
	bool dockedMenu = true;
	float menuScale = 1.f;
	float menuAnimSpeed = 1.f;
	std::string themeName = "Default";
	bool layoutMode = false;
	bool layoutUseRegularBg = false;
	// Layout mode draws every block, spike and saw as the default one, sized to its hitbox.
	bool layoutDefaultArt = true;
	bool layoutStyled = false;
	bool disableShakes = false;
	bool noMirrorPortals = false;
	bool showFrame = false;
	bool showCps = false;
	bool showPercent = false;
	bool showTime = false;
	bool showRecording = true;
	bool showPosition = false;
	bool showVelocity = false;
	bool showAlive = false;
	bool showMaxCps = false;
	bool showGravity = false;
	// Lists the aids that are switched on (labels.cpp), so none is left on by accident.
	bool showUtilities = false;
	bool showSafeMode = true; // While Safe Mode blocks progress: the hacks doing it.
	bool showDeathNext = false;
	bool clickDashOrbs = false;
	bool clickBlackOrbs = false;
	int orbClickCount = 1;
	bool padBlackOrb = false;
	bool padBlueOrb = false;
	bool deathNextP1 = false;
	bool noclipDeadP1 = false;
	bool noclipDeadP2 = false;
	bool deathNextP2 = false;
	// Actual-click survival length from the last trajectory predict (seconds).
	float trajAliveP1 = 0.f;
	float trajAliveP2 = 0.f;
	bool trajDiesP1 = false;
	bool trajDiesP2 = false;
	// Hold / release path lengths for Pause At Trajectory Length (seconds).
	float trajHoldAliveP1 = 0.f;
	float trajHoldAliveP2 = 0.f;
	float trajReleaseAliveP1 = 0.f;
	float trajReleaseAliveP2 = 0.f;
	bool pauseBeforeKill = false;
	// The trajectory moves the level's moving objects with it (Hack/level_forecast.cpp).
	bool trajectoryMovingObjects = true;
	// Backstep: saved game states to step backwards to while recording (src/Logic/backstep.cpp).
	bool backstep = false;
	int backstepSaves = 240;
	bool pauseAtTrajectoryLength = false;
	unsigned pauseTrajectoryMs = 500;
	// Edge latches so autopauses fire once until the condition clears again.
	bool pauseKillArmed = true;
	bool pauseLengthArmed = true;
	int percentAccuracy = 1;
	int maxCps = 16;
	float currentCps = 0.f;
	bool overMaxCps = false;
	bool cpsBreaking = false;
	bool currentlyPressing = false;
	bool autoclicker = false;
	bool syncAudio = false;
	bool autoclickP1 = true;
	bool autoclickP2 = true;
	bool autoclickAutoDisable = false;
	AutoClick autoClick;
	bool clickbot = false;
	float clickbotVolume = 1.f;
	float clickbotVolumeP2 = 1.f;
	std::string clickpack;
	std::string clickpackP2;
	bool p1Softs = true;
	float p1SoftMs = 150.f;
	bool p1Hards = true;
	float p1HardSeconds = 2.f;
	bool p1Micros = true;
	int p1MicroMs = 25;
	bool p2Softs = true;
	float p2SoftMs = 150.f;
	bool p2Hards = true;
	float p2HardSeconds = 2.f;
	bool p2Micros = true;
	int p2MicroMs = 25;
	double lastClickP1 = 0.0;
	double lastClickP2 = 0.0;
	bool frameAdvance = false;
	bool editorTesting = false;
	bool editorPaused = false;
	bool advanceQueued = false;
	int advanceSteps = 1;
	bool holdingAdvance = false;
	unsigned advanceHoldMs = 300;
	unsigned advanceDelayMs = 50;
	float advanceSpeed = 100.f;
	std::chrono::steady_clock::time_point advanceHoldStart{};
	std::chrono::steady_clock::time_point lastAdvanceStep{};
	bool injecting = false;
	bool applyingPlayback = false;
	bool holdSync = false;
	bool keepCheckpointHold = true;
	bool immediateRestart = false;
	bool armImmediateRestart = false;
	bool suppressAutoHold = false;
	bool routingInput = false;
	std::map<int, int> steeredKeys;
	bool keyJumpP1 = false;
	bool keyJumpP2 = false;
	bool keyLeftP1 = false;
	bool keyRightP1 = false;
	bool keyLeftP2 = false;
	bool keyRightP2 = false;
	bool deathJumpP1 = false;
	bool deathJumpP2 = false;
	bool deathLeftP1 = false;
	bool deathRightP1 = false;
	bool deathLeftP2 = false;
	bool deathRightP2 = false;
	bool physicalJumpP1 = false;
	bool physicalJumpP2 = false;
	bool physicalLeftP1 = false;
	bool physicalRightP1 = false;
	bool physicalLeftP2 = false;
	bool physicalRightP2 = false;
	bool ffmpegMissing = false;
	bool resumePending = false;
	bool sequenceEnabled = false;
	bool sequenceRandom = false;
	std::string macroName = "Replay";
	std::filesystem::path macroPath;
	std::filesystem::path autosavedPath;
	std::string renameFormat = "_#";
	std::string message = "Ready";
	int attempts = 1;
	double recordingSeconds = 0.0;
	uint64_t levelSeed = 0;
	bool levelSeedKnown = false;
	// Velocity Unrounding (src/Physics/velocity_unrounding.cpp). The setting picks it for new
	// recordings; a macro keeps the value it was recorded with in its header.
	bool velocityUnrounding = false;
	bool macroUnrounded = false;
	// Came from Silicate: moving objects take Silicate's variance indexes (src/Physics/silicate_variance.cpp).
	bool macroSilicateVariance = false;
	bool unroundedPhysics() const { return isPlaying() || isRecording() ? macroUnrounded : velocityUnrounding; }
	void syncUnrounding();
	// The single menu toggle: the loaded macro's value when there is one.
	bool shownUnrounded() const { return isPlaying() || isRecording() || !inputs.empty() ? macroUnrounded : velocityUnrounding; }
	void setUnrounded(bool on) {
		velocityUnrounding = on;
		macroUnrounded = on;
	}
	bool levelSeedFromFile = false;
	bool keepLevelSeed = false;
	bool seedOverride = false;
	uint64_t seedOverrideValue = 0;
	uint64_t shakeRandom = 0;
	uint64_t teleportRandom = 0;
	float endX = 0.f;
	bool displayFpsLocked = false;
	float backgroundColor[3] = {0.28235295f, 0.49019608f, 1.f};
	float blocksColor[3] = {0.28235295f, 0.49019608f, 1.f};
	int renderWidth = 1920;
	int renderHeight = 1080;
	int renderFps = 60;
	int renderBitrate = 20000000;
	float renderFadeIn = 0.5f;
	float renderFadeOut = 0.5f;
	float renderAfterEnd = 2.f;
	float renderMusic = 1.f;
	float renderSfx = 1.f;
	bool renderQueued = false;
	bool renderRecordPause = false;
	bool renderSsbFix = true;
	// Renders advance the level at the speedhack instead of real time.
	bool renderSpeedhack = false;
	bool renderHideLabels = false;
	bool renderColorFix = true;
	bool mergeBusy = false;
	bool renderAutoExit = false;
	std::string renderCodec = "libx264";
	std::string renderPreset = "veryfast";
	std::string renderExtension = "mp4";
	std::string renderArgs;
	std::string renderName;
	LabelPlacement frameLabel{3.f, 268.f, 0.4f, 35};
	LabelPlacement cpsLabel{3.f, 280.f, 0.4f, 35};
	LabelPlacement percentLabel{3.f, 310.f, 0.55f, 35};
	LabelPlacement timeLabel{3.f, 294.f, 0.4f, 35};
	LabelPlacement recordingLabel{0.f, 15.f, 0.75f, 70};
	LabelPlacement positionLabel{3.f, 252.f, 0.4f, 35};
	LabelPlacement velocityLabel{3.f, 238.f, 0.4f, 35};
	LabelPlacement aliveLabel{3.f, 224.f, 0.4f, 35};
	LabelPlacement maxCpsLabel{3.f, 210.f, 0.4f, 35};
	LabelPlacement gravityLabel{3.f, 196.f, 0.4f, 35};
	LabelPlacement utilityLabel{3.f, 150.f, 0.35f, 80};
	LabelPlacement deathLabel{3.f, 170.f, 0.5f, 100};
	LabelPlacement safeModeLabel{3.f, 130.f, 0.35f, 80};
	// Auto Position stacks the shown labels from a corner, in labelOrder (keys, top to bottom).
	bool labelsAuto = true;
	int labelCorner = 0; // kLabelCorners
	float labelScale = 1.f; // Multiplies every label's own scale.
	std::string labelFont = "bigFont.fnt";
	std::vector<std::string> labelOrder;
	std::deque<HitboxTrailPoint> trailP1;
	std::deque<HitboxTrailPoint> trailP2;
	std::vector<GameObject*> hiddenDecorations;
	std::vector<CpsNote> cpsNotesP1;
	std::vector<CpsNote> cpsNotesP2;
	std::string highestCps = "P1: 0";

	uint64_t inputId(size_t index) const {
		return index < inputIds.size() ? inputIds[index] : 0;
	}
	void setInputId(size_t index, uint64_t id) {
		if (index >= inputs.size()) return;
		if (inputIds.size() <= index) inputIds.resize(index + 1);
		inputIds[index] = id;
	}
	FramePose poseAt(size_t index) const {
		return index < poses.size() ? poses[index] : FramePose{};
	}
	void setPoseAt(size_t index, FramePose pose) {
		if (poses.size() < inputs.size()) poses.resize(inputs.size());
		if (index < poses.size()) poses[index] = pose;
	}
	void clearSides() {
		poses.clear();
		inputIds.clear();
		liveFrame.fill(-1);
		liveHeld.fill(0);
		liveSeen.fill(0);
		newestInput = -1;
		holdCount = 0;
		releaseCount = 0;
		inputsInOrder = true;
	}
	void touchInputs() {
		++inputsRevision;
	}
	bool isRecording() const {
		return state == RunState::Recording || state == RunState::RecordingAndPlaying;
	}
	bool isPlaying() const {
		return state == RunState::Playing || state == RunState::RecordingAndPlaying;
	}
	bool isIdle() const {
		return state == RunState::Idle;
	}
	bool playsPlayer(bool player2) const {
		if (!isPlaying()) return false;
		return player2 ? playP2 : playP1;
	}
	bool recordsPlayer(bool player2) const {
		if (!isRecording() || playsPlayer(player2)) return false;
		return player2 ? recordP2 : recordP1;
	}
	bool physicsLocked() const {
		return isRecording() || isPlaying() || frameAdvance;
	}
	int stepsUntilRateChange() const {
		auto after = replay::rateAfter(rates, stepFrame);
		if (after != rates.end()) return after->frame - stepFrame;
		return 2048;
	}
	double sampleRate() const {
		float rate = std::isfinite(fps) ? fps : 240.f;
		return static_cast<double>(std::max(rate, kStepPatch ? 1.f : kVanillaTps));
	}
	float physicsStep() const {
		return static_cast<float>(1.0 / sampleRate());
	}
	float rateAt(int frame) const {
		float rate = std::isfinite(macroFps) ? macroFps : 240.f;
		auto after = replay::rateAfter(rates, frame);
		while (after != rates.begin()) {
			auto const& change = *--after;
			if (std::isfinite(change.fps) && change.fps >= 1.f) {
				rate = change.fps;
				break;
			}
		}
		return std::max(rate, 1.f);
	}
	double secondsAt(int frame) const {
		if (frame <= 0) return 0.0;
		double time = 0.0;
		int at = 0;
		float rate = macroFps;
		auto add = [&](int upto) {
			if (upto <= at) return;
			time += static_cast<double>(upto - at) / static_cast<double>(std::max(rate, 1.f));
			at = upto;
		};
		for (auto const& change : rates) {
			if (change.frame >= frame) break;
			if (change.frame > at) add(change.frame);
			rate = change.fps;
		}
		add(frame);
		return time;
	}
};

class Logic : public Session {
public:
	static constexpr int kMaxPerSecond = 16;
	static constexpr int kMaxPerFrame = 3;
	static constexpr float kMaxBurstRate = 48.f;
	static constexpr int kMinBurstClicks = 5;

	static Logic& get();
	static Logic& live();
	static void setLoading(Logic* session);
	const char* stateName(RunState state);
	void boot();
	void pullSettings();
	void pushRuntimeSettings();
	void saveSettingsFile();
	int getFrame(PlayLayer* play);
	void toggleRecording();
	void togglePlaying();
	void toggleFrameAdvance();
	void requestStep();
	void pollAdvanceHold();
	int backstepInterval() const;
	void saveBackstep(PlayLayer* play);
	void noteShownCamera(PlayLayer* play, bool stepped);
	int followSteps() const;
	bool followActive(GJBaseGameLayer* play) const;
	void noteFollowPress(GJBaseGameLayer* play, bool down, int button);
	bool followDue(FollowPress const& press, int frame, double time) const;
	void rewindFollow(GJBaseGameLayer* play);
	void runFollow(GJBaseGameLayer* play);
	void clearBacksteps();
	void trimBacksteps(PlayLayer* play);
	bool stepBack(PlayLayer* play, int steps);
	void finishBackstep(PlayLayer* play);
	void pollBackHold();
	Checkpoint captureCheckpoint(PlayLayer* play, CheckpointObject* object, int frame);
	void disarmLengthPauseIfHolding();
	// Trajectory-driven: turn frame advance on (or stop a held advance) before a kill / long path.
	void checkAdvancePause(PlayLayer* play);
	void onKeybind(std::string_view key, bool down);
	void tickAutoclicker(PlayLayer* play);
	void afterFrame(PlayLayer* play, float dt);
	std::vector<std::string_view> activeCheats() const;
	std::vector<std::string_view> const& attemptCheats() const;
	bool attemptCheated() const;
	void updateSafeMode(PlayLayer* play);
	void holdSafeModeTestMode(PlayLayer* play);
	void endSafeModeAttempt(PlayLayer* play);
	void resetSafeMode();
	bool playerHidden(PlayerObject* player) const;
	bool hitboxPlayerHidden(bool player2) const;
	bool trajectoryPlayerHidden(bool player2) const;
	void concealPlayer(PlayerObject* player, bool hide);
	void pumpButtons(GJBaseGameLayer* play, GJBaseGameLayer* layer, float dt, bool clearInputQueue);
	void prepareButtons(GJBaseGameLayer* play);
	bool cbfEnabled() const;
	bool vanillaClickPhysics() const;
	bool deferPhysics() const;
	double clickStamp() const;
	void rememberMove(PlayerButtonCommand const& cmd);
	void restoreHolds(PlayLayer* play);
	void onPlayInit(PlayLayer* play);
	void onReset(PlayLayer* play);
	void plantLevelSeed(PlayLayer* play);
	void applyAttemptVariance(PlayLayer* play, uint64_t seed);
	void keepVariance(PlayLayer* play);
	void rollLiveSeed(PlayLayer* play);
	void restoreCheckpointRandom(PlayLayer* play);
	void noteRandTrigger(int uniqueId, uint64_t* state);
	void reseedRandTriggers(uint64_t seed);
	void clearRandTriggers();
	uint64_t gameRandomState();
	void setGameRandomState(uint64_t state);
	void captureRestartHolds(PlayLayer* play);
	void clearStuckDeath(PlayLayer* play);
	void onCheckpointCreated(PlayLayer* play, CheckpointObject* object);
	// Saves each object's move state at the first step from a new checkpoint, and puts it back at
	// the first step after respawning there.
	void stepObjectStates(PlayLayer* play);
	void noteBackstepObjects(PlayLayer* play);
	void popBackstep();
	// Releases a save, or logs and leaks one whose game state is broken (GD's destructor would crash
	// on it; Logic/state_check.hpp).
	void releaseBackstep(BackstepSave& save, char const* why);
	void clearBackstepObjects();
	void loadBackstepObjects(int frame);
	CheckpointObject* objectCapture = nullptr;
	CheckpointObject* objectRestore = nullptr;
	void syncCheckpoints(PlayLayer* play);
	Checkpoint const* savedCheckpoint(PlayLayer* play);
	void onComplete(PlayLayer* play);
	void onQuit(PlayLayer* play);
	bool shouldNoclip(PlayLayer* play, PlayerObject* player);
	bool steerPlayer(bool& isPlayer1);
	PlayerObject* iconBody(GJBaseGameLayer* play, bool icon2);
	bool pressIcon(GJBaseGameLayer* play, bool icon2, bool down, int button);
	bool physicalHold(GJBaseGameLayer* play, bool icon2);
	bool steerKey(GJBaseGameLayer* play, cocos2d::enumKeyCodes key, bool down, double timestamp);
	void setRoutedHold(bool icon2, int button, bool down);
	void reinforceRoutedHold(PlayerObject* body);
	void applyMaintainGravity(GJBaseGameLayer* play);
	void syncSameGravity(GJBaseGameLayer* play);
	void offsetInputs(int delta, int first = -1, int last = -1);
	void randomOffsetInputs(int magnitude, int first = -1, int last = -1);
	void removePlayerInputs(bool player2);
	void flipInputHolds();
	void flipInputPlayers();
	void sortInputs();
	void identifyInputs(bool force = false);
	uint64_t mintInputId();
	void clearInputs();
	bool dispatchSettingKey(std::string_view key, bool down);
	std::filesystem::path saveRoot();
	void refreshPopup();
	void notify(std::string const& text, bool success);
	bool saveReplay();
	bool autosaveReplay();
	bool readReplay(std::filesystem::path const& path);
	bool exportKind(std::string_view kind);
	bool appendSequenceFile(std::filesystem::path const& path, int amount);
	void pickReplay();
	void importReplays();
	int convertLegacyEchoes();
	unsigned countPressesInLastSecond(bool player2);
	void noteClick(PlayLayer* play, bool down, bool player1, int button = static_cast<int>(PlayerButton::Jump));
	void updateCps(PlayLayer* play);
	bool burstTooFast(Session const& value, int clicks, int first, int last) const;
	bool playerCpsBroken(std::deque<int> const& clicks, int now, float maxPerSecond) const;
	void setTps(float rate);
	void placeRate(int frame, float rate);
	void applyRate(int frame);
	void normalizeRates();
	void trimRates(int frame);
	void syncTimeScale();
	void syncPlaybackFps();
	int unlockedStepCap() const;
	void onFocusChange();
	void resumeAttempt(PlayLayer* play);
	// Pause releases the hold. Playback puts that hold back without queueing a click.
	void retainPlaybackHold(PlayLayer* play);
	void playClick(PlayLayer* play, bool down, bool player1);
	void flushClicks();
	void noteStep();
	void releaseReplayIndex();
	int affordableSteps(bool lockVisual) const;
	void noteFrameWork(double seconds);
	void drawOverlay(PlayLayer* play);
	void syncDeathLabel(PlayLayer* play);
	void applyLayout(PlayLayer* play);
	void drawLayoutArt(PlayLayer* play);
	static cocos2d::CCRect objectHitbox(GameObject* object);
	void clearLayoutHidden();
	bool layoutActive();
	void paintLayoutColors(PlayLayer* play);
	// Layout Mode: every color channel the objects read this frame, so no block takes a level color
	// (often the background's).
	void paintLayoutChannels(GJEffectManager* effects);
	void styleLayoutObject(GameObject* object);
	void styleLayoutGlow(GameObject* object);
	void drawHitboxes(GJBaseGameLayer* play);
	void seatOverlay(cocos2d::CCNode* node, GJBaseGameLayer* play);
	void syncOverlaySeats(GJBaseGameLayer* play);
	void sampleHitboxTrail(GJBaseGameLayer* play);
	void noteClickHitbox(GJBaseGameLayer* play, bool player1, bool down);
	void capturePendingClick(PlayerObject* player);
	void flushClickHitboxes(GJBaseGameLayer* play);
	void clearClickHitbox();
	void prepareLevelView(PlayLayer* play);
	void beginEditorPlaytest();
	void endEditorPlaytest(GJBaseGameLayer* editor);
	bool renderActive();
	bool renderCapturing();
	bool renderPaused();
	bool pauseMenuOpen();
	bool renderShouldSkip();
	bool renderHoldsView();
	bool renderBlocksEngine();
	bool renderBlocksMixer();
	float renderStep();
	// The level speed of a render: the speedhack with Use Speedhack on, else 1.
	float renderSpeed();
	float scrollSpeedFactor();
	float renderGameStep();
	// Game time the current render frame ran (renderGameStep when it was taken).
	float renderFrameGameStep = 0.f;
	bool beginRenderFrame();
	bool captureRenderFrame();
	bool renderBatchMore(std::chrono::steady_clock::time_point started);
	void holdRenderEffects();
	void showRenderOverlay();
	RenderStatus renderStatus();
	float renderWallDelta();
	void syncMusicPitch();
	void syncAudioClock();
	void resetOpeningMusicHold();
	bool ffmpegReady();
	std::string renderFileStem() const;
	void startRender();
	void stopRender();
	void mergeRenders(std::filesystem::path const& first, std::filesystem::path const& second);
	void resetTrajectory();
	bool isTrajectoryPlayer(cocos2d::CCObject* object);
	// Installs the hooks that only matter during a prediction while Show Trajectory is on.
	void syncPredictHooks();
	bool advancePaused(GJBaseGameLayer* play);
	bool lockDeltaActive() const;
	int countUnlockedSteps(GJBaseGameLayer* play, double dt);
	void runFrame(GJBaseGameLayer* play, float dt, std::function<void(float)> const& tick);
	void cancelSteps();
	void flushVisualRefresh();
	void recomputeCps(Session& value);

	std::string readFolderSetting(char const* key);
	matjson::Value placementJson(LabelPlacement const& placement);
	LabelPlacement readPlacement(matjson::Value const& json, LabelPlacement fallback);
	FramePose capturePose(PlayerObject* player);
	bool flipControls();
	void setReplayPos(PlayLayer* play);
	void trimRecordedInputs(Session& value, int frame);
	void trimFreshAttempt(PlayLayer* play, Session& value);
	PlayerSnapshot capturePlayer(PlayerObject* player);
	void applyPlayer(PlayerObject* player, PlayerSnapshot const& shot);
	LayerSnapshot captureLayer(PlayLayer* play);
	void applyLayer(PlayLayer* play, LayerSnapshot const& shot);
	void trimFromCheckpoint(PlayLayer* play, Session& value, Checkpoint const& checkpoint);
	void clearPhysicalHolds();
	bool heldAt(Session const& value, bool player2, int frame, int button = static_cast<int>(PlayerButton::Jump));
	std::pair<int, int> countHolds(Session const& value);
	bool inputsSorted(Session const& value);
	// Pressing a direction clears the other one in m_holdingButtons even while it is still held.
	// m_holdingLeft / m_holdingRight keep both, and only skip updates while controls are disabled.
	static bool buttonHeld(PlayerObject* body, int button) {
		if (!body) return false;
		auto found = body->m_holdingButtons.find(button);
		bool mapped = found != body->m_holdingButtons.end() && found->second;
		if (button == static_cast<int>(PlayerButton::Left)) return mapped || body->m_holdingLeft;
		if (button == static_cast<int>(PlayerButton::Right)) return mapped || body->m_holdingRight;
		return mapped;
	}
	static PlayLayer* asPlay(GJBaseGameLayer* layer);
	static LevelEditorLayer* asEditor(GJBaseGameLayer* layer);
	size_t placeRecorded(Session& value, Frame const& frame);
	void refreshLiveHold();
	void recordInput(GJBaseGameLayer* play, bool down, bool player1, int button = static_cast<int>(PlayerButton::Jump), bool force = false);
	void injectButton(PlayLayer* play, bool down, int button, bool player1);
	void noteRule(std::vector<CpsNote>& notes, int first, int last, std::string text);
	void evalPlayerCps(std::vector<int> const& presses, bool player2, Session& value);

	static float readFloat(char const* key, float fallback) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key)) return fallback;
		return mod->getSettingValue<float>(key);
	}
	static bool readBool(char const* key, bool fallback) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key)) return fallback;
		return mod->getSettingValue<bool>(key);
	}
	static int readInt(char const* key, int fallback) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key)) return fallback;
		return static_cast<int>(mod->getSettingValue<int64_t>(key));
	}
	static void writeBool(char const* key, bool value) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key)) return;
		mod->setSettingValue<bool>(key, value);
	}
	static void writeFloat(char const* key, float value) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key)) return;
		mod->setSettingValue<float>(key, value);
	}
	static void writeInt(char const* key, int value) {
		auto* mod = geode::Mod::get();
		if (!mod->hasSetting(key)) return;
		mod->setSettingValue<int64_t>(key, static_cast<int64_t>(value));
	}
};
