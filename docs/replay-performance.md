# Recording and playback optimization

These changes apply to Windows and Android; ARM64 and Windows retain their
unlocked physics batching, and ARM32 retains its existing fallback. Physics
steps, float precision, click ordering, replay formats and enabled tools are
preserved. No fast-math, input dropping, trajectory shortening or visual-quality
reduction is used.

- Recording starts with 4,096 input slots (32 KiB) instead of 1,048,576 (8 MiB).
  The vector still grows normally for arbitrarily long recordings. Practice
  trims reuse allocations; explicit Clear releases inputs, debug samples,
  editor IDs and the held-input index. Explicit player deletion shrinks storage.
- Button preparation routes and filters the original queue instead of making a
  separate copy. Empty-input physics steps avoid queue snapshots entirely, but
  retain gravity correction, pad swifts and tool commits. Nonempty steps use
  eight inline command slots, with a heap fallback for larger queues. Commands
  on the same frame still execute individually and in order.
- Pad processing transfers queued-command and pending-pad ownership instead of
  copying vectors. Steps without pending pads skip the save/restore work.
- Held-input queries use independent cursors for each player/button lane. Small
  forward steps advance those cursors; initial queries, rewinds and large jumps
  use binary searches. Rebuilding an edited timeline resets the cursors.
- TPS selection and the next rate-change boundary use binary search on the
  normalized rate list. The final valid change on a frame still wins.
- Practice trimming compacts inputs and matching debug/ID arrays in place and
  handles both recorded players in one pass. Already ordered input lists avoid
  allocating and sorting a full permutation. Checkpoints move their owned
  vectors into storage. Death diagnostics search backwards for the same last
  matching input, and ordered replay seeks use binary search.
- The per-step variance lock first compares its 2,000 floats with `memcmp`,
  retaining float equality when bit patterns differ. This accelerates the
  common identical-array case while preserving signed-zero behavior.
- Physics cost estimation counts every step but samples timestamps sparsely in
  long batches (first eight steps, then every sixteenth). It does not change the
  simulated timestep or skip physics. Timestamps outside measured updates are
  avoided, and update callbacks no longer use temporary `std::function`s.
- Explicit input offsets invalidate lookup caches even when order is preserved;
  sequence sorting refreshes metadata even when no permutation is needed.
  Offset operations no longer launch duplicate CPS scans.

## Local measurements

An optimized native microbenchmark on the development machine produced these
single-run results. These measure the components below, not in-game FPS or
Android throughput. Temporary-buffer byte counts describe the old and new
algorithms, not measured process RSS.

| Component | Before | After | Workload |
| --- | ---: | ---: | --- |
| Held-history lookup | 45.53 ms | 5.46 ms | 2 million forward queries, 1 million events |
| TPS lookup | 49.26 ms | 0.39 ms | 20,000 queries, 10,000 rate changes |
| Checkpoint trim | 7.37 ms | 0.55 ms | 1 million inputs with aligned debug/ID arrays; retain first half |
| Trim temporary buffers | 25 MB | 0 | Same workload |
| Initial input reservation | 8 MiB | 32 KiB | New recording |

## Validation

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Werror -Isrc \
    tests/replay_algorithms.cpp -o /tmp/echo-replay-tests
/tmp/echo-replay-tests --benchmark
c++ -std=c++23 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -Isrc tests/replay_algorithms.cpp \
    -o /tmp/echo-replay-sanitized
/tmp/echo-replay-sanitized
python tests/button_pipeline.py
```

The randomized test compares the production algorithms against reference
results in 3,000 cases: timeline compaction, absent/incomplete metadata, both
players, same-frame events, forward seeks, rewinds, appends, large queues,
rate boundaries and variance bit patterns including signed zeros and NaNs.
The pipeline test compiles the actual production button methods against a
deterministic stub and checks mixed recording/playback, individual same-frame
commands, queue retention, gravity corrections, pad swifts and dual routing
under address/undefined-behavior sanitizers. The stub does not simulate the
game's physics or interactions with other mods.

Windows, Android ARM64 and Android ARM32 packages must all build. The existing
ARM64 patch emulator test also verifies the original game instructions and 63
count/delta cases. Real gameplay and end-to-end performance measurements are
still needed on Windows and the phone, especially at high TPS with debug
samples, platformer/dual inputs, practice checkpoints, gravity tools, trails,
trajectory and mid-replay rate changes enabled.

## Native game patches and frame advance

The native optimizations are based on disassembly of the installed GD 2.2081
Windows executable and the ARM64 `libcocos2dcpp.so` extracted from the phone.
They run with either Lock TPS/FPS setting and preserve the original physics
operations. Each optimization checks its original instruction bytes at startup;
changed instructions or another mod occupying the site cause it to be skipped.
ARM32 receives the scheduler fix but no unverified native patches.

| Platform | Address (module relative) | Change |
| --- | --- | --- |
| ARM64 | `0xace648`, `CCPoint::operator=` | Replace a stack frame, saved register and nested `setPoint` call with two float loads, two stores and return. Both loads precede stores to preserve aliases and all float bit patterns. Windows already uses a leaf implementation. |
| ARM64 | `0x623e30`, `PlayerObject::resetTouchedRings` | If the unordered set is already empty, skip redundant bucket `memset`; still reset its count/head. Nonempty sets retain every node deletion and bucket clear. Windows already checks empty sets. |
| Windows | `0x231daa`, `GJBaseGameLayer::processQueuedButtons` | Redirect the `memmove` call through checked padding at `0x231df3`; skip only zero-byte moves when the last command is removed. Nonempty moves tail-call the same original thunk. ARM64 already checks zero length. |

A manual frame-advance press now consumes one physics tick independent of TPS,
display delta, speedhack, time warp and Lock FPS. It resets carried time and
explicitly supplies a step count of one. Previously its step count was computed
from the display delta, so 10,000 TPS at 60 FPS could advance many physics ticks.
Holding the key retains continuous repeat using a separate explicit tick budget
and fractional elapsed-time credit after the configured hold delay. It still
observes the existing work cap and stops when the player dies.

`python tests/frame_advance.py` compiles the actual scheduler and held-repeat
methods against a stub under ASan/UBSan: 600 combinations of TPS, display delta,
speed, warp and Lock FPS, idle-after-step, repeat timing, rate changes and death.
`python tests/game_patches.py <arm64-library> <windows-executable>` checks the
production patch signatures against both real binaries and executes original
and replacement instruction streams with Unicorn. It compares 909 point copies
(including aliases, NaNs and signed zeros), 12 empty/nonempty ring clears and
five overlapping queue moves. External delete/memmove/memset calls are modeled
for their effects; these tests do not measure game FPS or validate other mods.
No overall speedup claim is made without measurements in the running game.

## Orb spam: reuse the effect mesh

Disassembly shows that `PlayerObject::ringJump` creates `CCCircleWave` effects.
The wave's `draw` calls `ccDrawCircle` or `ccDrawFilledCircle`; each primitive
allocates/frees a vertex buffer and computes cosine and sine for every vertex
on every draw. Every-frame orb activation therefore adds work that ordinary
button spam does not. This is a verified source of extra work, not a profile
proving it is the only cause of the reported slowdown.

`Rendering/orb_circles.cpp` scopes a mesh cache to `CCCircleWave::draw`, retaining
the original wave drawing method and the original creation/animation lifecycle.
Only the native standard zero-angle meshes (10, 15, 20, 30 and 50 segments)
use the fast path. Directions are computed once per mesh with the platform's
original trig implementation; Windows calls the game's own CRT imports.
Coordinates use the original operation order, including ARM64 FMA and separate
Windows rounding. A fixed 52-point buffer replaces the per-draw heap buffer.
The seam vertex is computed exactly as before, rather than copied from vertex 0.

Every effect is retained. Shader, color uniforms, transform, GL primitive mode,
vertex count, blending, line width and draw order stay the same. No physics,
inputs, orb/pad activation, effects, durations or circle segment counts are
removed. Unusual arguments, other primitive callers and an uninitialized shader
fall through to the original functions. Full instruction fingerprints guard the
Windows and ARM64 native renderer layouts; ARM32 keeps its original renderer.
The cache is bounded to approximately 2 KiB of mesh data per rendering thread,
plus a 416-byte vertex buffer per active draw.

A local optimized CPU microbenchmark generating 100,000 50-segment meshes took
59.39 ms with the native algorithm and 7.17 ms with the cache. Trig calls fell
from 10,200,000 to 102 (first mesh only); warmed draws perform none. These are
mesh-generation measurements, not game FPS, GPU throughput or phone results.
Other orb activation, allocation, animation and drawing costs still remain.

Validation:

```sh
c++ -std=c++23 -O2 -Wall -Wextra -Werror -ffp-contract=off -Isrc \
    tests/circle_geometry.cpp -o /tmp/echo-circle-test
/tmp/echo-circle-test --benchmark
python tests/circle_renderer.py
python tests/native_circle_geometry.py <arm64-lib> <windows-cocos-dll>
```

The geometry test compares 20,000 cases bit-for-bit, including signed zeros,
negative radii, both primitive types and both rounding profiles. The GL recorder
test compiles the production hooks under ASan/UBSan and checks uniforms, modes,
vertex counts, alpha behavior and fallback paths. Native emulation verifies all
four full renderer fingerprints and 240 actual vertex instruction streams
against the production cache, with consistent modeled CRT functions. Real
visual/gameplay verification and an end-to-end profile remain necessary.

## Orb spam: compact circle animations

The mesh cache did not produce a noticeable improvement in the reported high-TPS
orb-spam case. It reduces mesh-generation work, not the cost of activating an
orb or creating and managing its effects. An exploratory sample of the running
Windows process mostly showed Wine timekeeping/frame pacing; it did not provide
an active orb-spam profile, so it is not evidence for the workload's dominant
bottleneck.

Native `CCCircleWave::init` constructs 5–7 action objects for every effect:

- Fade-out, linear: two tweens, a spawn, a callback and a sequence (5).
- Fade-out, eased: the same graph with two ease wrappers (7).
- Fade-in: a radius tween, two opacity tweens, two nested sequences, a removal
  callback and a spawn (7). The ease flag is ignored on this branch natively.

`Rendering/wave_animation.cpp` replaces that graph with one `CCActionInterval`.
This removes 4–6 action allocations and their autorelease/retain/release work
per effect, as well as nested action dispatch. Effect nodes themselves are still
created, rendered and removed individually. Shader meshes, orb collision,
activation, recording and playback input order are unchanged. Shine sprites and
other non-circle effects remain native and can still contribute substantial
cost at high TPS. No end-to-end speedup is claimed without an active-game test.

The compact action retains Cocos's interval stepping, including its ignored
first delta, pause/resume behavior and restart. It sends the original virtual
radius/opacity tween callbacks in order, including the extra opacity completion
callback when a step crosses the fade-in midpoint. Removal uses the original
wave callback, with the same strong target ownership (also when the action is
copied to another target). The rare reverse-action API reconstructs the native
graph and delegates to its reverse implementation.

Tween values use `to - delta * (1 - t)`, with ARM64 fused arithmetic and separate
Windows rounding. Easing uses the game's own `powf` implementation; a common
radius/opacity easing result is computed once. The actual native initializer
uses opacity 255, although the SDK's Windows inline `baseSetup` uses 1; the fast
initializer follows the binary's values. Nonfinite parameters, overflowing
radius deltas and unusually short durations use native initialization.

Fingerprints check the native initializer body and the native tween/ease
implementations on Windows and ARM64. ARM32 retains native animations. Startup
logs `Echo compact circle animations enabled` when verification succeeds; an
instruction mismatch leaves the original graph active. The initializer
fingerprint excludes its prologue, occupied by Echo's own Geode hook.

Validation:

```sh
python tests/wave_animation.py
c++ -std=c++23 -O1 -g -Wall -Wextra -Werror -ffp-contract=off \
    -fsanitize=address,undefined -Isrc tests/wave_timeline.cpp \
    -o /tmp/echo-wave-timeline
/tmp/echo-wave-timeline
python tests/native_wave_timeline.py <arm64-lib> <windows-cocos-dll> <windows-exe>
```

The production initializer/action test runs under ASan/UBSan in both arithmetic
profiles and checks initial values, first tick, midpoint, completion, target
ownership, copies, restart, parameter fallback and failed initialization cleanup.
Native emulation checks six fingerprints and compares 160 actual native
Sequence/Spawn/Ease/Tween instruction traces against the production timeline,
bit-for-bit and callback-for-callback. It includes both fades, easing, large
update jumps and backward updates before completion. Allocation, action
start/stop bookkeeping, delegate calls and CRT `powf` are modeled; this is not
an in-game lifecycle or performance measurement. Gameplay verification after
restarting is still required.

### Windows gravity-event lookup

Windows-only follow-up: GD 2.2081 `gameEventTriggered` looks up its
event-state map entry twice before dispatching a newly updated event. The
first lookup already returns the entry (and inserts missing keys). No callback
or map mutation occurs between lookups. A two-byte jump at RVA `0x232043`
reuses that result, leaving insertion, same-tick suppression, trigger dispatch,
and player-specific recursion native. This also benefits gravity-orb events.
There is no cache or additional allocation. The surrounding caller instructions
and complete lookup helper fingerprint must match before installation; a
modified/detoured helper disables this optimization.

`tests/windows_event_lookup.py GeometryDash.exe` passed 752 comparisons with
the actual native caller and tree-lookup instructions. Tests cover empty and
nonempty trees, existing/missing keys, signed composite keys, and suppressed
or updated ticks. Allocation and tree insertion are modeled at library
boundaries; existing-key traversal executes actual native instructions. Heap
contents, event-state contents, continuation, and returned pointer match.
Updated events execute 46–117 fewer instructions in the tested trees. This is
an instruction-count result, not an end-to-end orb-spam speed measurement.
Windows was rebuilt and the installed package verified identical to
`dist/decks.echo.windows.geode`. Android artifacts were not rebuilt for this
Windows-only follow-up. Gravity flips still perform their required collision
reset, player transforms, callbacks and effect work; this patch does not make
gravity-orb spam equivalent in cost to ordinary clicks.

### Windows high-TPS input follow-up

A live main-thread CPU profile of the user's running replay captured 4,937
samples in an active section: game code 1,192; Cocos 1,010; Geode 991; Echo
218. Other samples included installed mods, runtime libraries, graphics and
Wine. This is a sample distribution for that section, not a before/after
throughput result. A separate earlier section showed significant editor
selection scanning and menu rendering alongside frame-loop timing work. The
workload and input list changed during observation, so the samples must not be
compared as a controlled performance benchmark.

The Windows-only changes target demonstrated redundant work:

- Single-command physics ticks retain the existing queue and call the native
  button processor directly through the same binding. General replay takes
  three command copies; this path takes zero. It preserves tool-commit order
  and post-callback gravity correction. Mixed commands, one-player dual replay,
  pending pads, retained queues and active recording gravity correction retain
  the general path. It does not bypass native button handling or drop inputs.
- Pause-menu queries retain current child membership and lifetime checks but
  reuse the type classification of unchanged children. Reordering, removal,
  replacement, and reuse of an address under a new Cocos lifetime ID cause
  reclassification. Cached entries do not retain scene objects.
- Stable editor selection and its range anchor validate their existing indices
  before searching by ID. For a four-million-input macro, tracking a stable
  selection/anchor requires two ID reads instead of 7,999,999. Moving/deleting
  an input still searches; IDs remain unique under the editor's minting scheme.
- Windows section repair still validates every used entry and repairs stale
  slots, counts and tombstones. It no longer writes an unchanged pointer or
  index back into every valid object. Corruption handling remains present;
  this is a reduction in memory writes, not a removal of the safety checks or
  a claim that the bucket scan became constant-time.

Validation (ASan/UBSan): `tests/windows_button_fast_path.py` compares 10,000
randomized input/tool/dual/callback cases with the production general path,
checks three-to-zero command copies, and processes 10,000 alternating inputs
at a 5000-TPS delta. `tests/child_type_cache.cpp` checks 20,000 graph/lifetime
changes and proves stable queries avoid a million classifications.
`tests/editor_selection.py` compares 10,000 selection/edit cases and the large
macro read count. `tests/windows_sections.py` compares 5,000 production repair
cases with the prior implementation, including object 1816. Button-pipeline
and frame-advance regressions passed. Windows rebuilt and its installed
package was verified byte-for-byte against `dist/decks.echo.windows.geode`.

The running game must restart before it uses the new DLL. An end-to-end
speedup has not yet been verified. Native collision work, moving objects,
effects and other mods remain material costs; no claim of physical optimality
or ordinary-click-equivalent gravity-orb cost is justified by these checks.

### Live Windows diagnosis, 2026-10-06

The running DLL and native step-count patch were checked in process memory.
The patch at GD RVA `0x237a5b` loads Echo's `stepsThisTick`; it is active.
The compact circle-animation optimization is also enabled in the loader log.
In a sparse portion of Metathesis, 4663 requested TPS produces approximately
4663 physics steps per wall second at 1x and 37304 at 8x. Those results verify
the scheduler and step patch in that portion; they are not orb-spam benchmarks.
At 1x, reducing CPU cost cannot make a replay exceed its requested game speed.

The heavy sections have different dominant costs:

- Metathesis: a dense-input sample attributed 3731 samples, approximately
  75% of the sampled main-thread CPU time, to Wave Trail Draw Fix 1.2.0.
  Its offset-path construction and geometry math dominate this sample.
  This is profiling evidence, not a controlled speedup measurement.
- Luua Tron: at 33115 configured TPS, the dense section contains about
  20 input events per physics step. A scene census found 133420 CCSprites
  among 138882 nodes, with 66624 children in the object layer. A subsequent
  5006-sample profile attributed 2349 samples to Cocos, 1063 to the graphics
  driver, 16 to Echo, and 6 to Wave Trail Draw Fix. Cocos hotspots include
  `CCNode::visit`, `CCActionManager::update`, `CCSequence::update`, and
  `CCActionInterval::step`. The wave-trail mod is not the dominant cost
  in this Luua Tron sample.

A controlled rendering-only diagnostic in Luua Tron's sustained input section
kept TPS, speedhack, physics, and actions unchanged. Temporarily hiding the
object layer isolated scene traversal and drawing:

| Rendering | Physics steps/wall second | Physics frames sampled | Input events |
| --- | ---: | --- | ---: |
| Original | 279.1 | 983543–983987 | 8900 |
| Object layer temporarily hidden | 641.7 | 984211–985233 | 20420 |
| Original restored | 270.4 | 985318–985748 | 8600 |

The roughly 2.3x increase demonstrates a material rendering bottleneck.
The remaining slowdown with the layer hidden includes action updates and
physics; it does not prove that drawing is the only bottleneck. Both test
windows process approximately 20 events per physics frame. A separate earlier
trail-bypass attempt crossed different input sections and is excluded from
performance conclusions. A later attempt crossed into a changed replay and
is also excluded.

All temporary code, visibility, and speed changes were restored. The game was
left paused at 1x, with Luua Tron's 33115 TPS preserved. No effects were removed
from the shipped mod and no new binary was built for this diagnosis. Improving
these workloads while retaining their visuals requires addressing the trail
renderer and the large population of animated sprites; another physics-count
patch or small button-queue optimization alone cannot remove those costs.
Raw comparison samples and profile counts are retained in
`performance/windows-live-diagnosis-2026-10-06.json`.

### Reset recovery and continued optimization, 2026-10-06

The prior session finished building and installing compact dash-end actions
and cached wave-trail geometry before the PC reset interrupted live testing.
Both installed packages matched the surviving distribution files byte-for-byte.
The dash animation lifetime/copy/fallback checks passed again with sanitizers.
The trail test again matched 2,000 geometry traces bit-for-bit; its optimized
component measured 39.88 ms to 11.59 ms in the ordinary build. Component
benchmarks do not establish an end-to-end replay improvement.

The recovered Luua Tron replay is responsive but remains severely constrained.
A read-only progress sample at 33,115 TPS and 1x measured 257.65 physics
steps/wall second, with 66,512–66,932 children in the effect/object layer.
This sample uses a different replay frame range from the earlier baseline
and must not be treated as a controlled before/after result. A five-second
instruction-address sample collected 4,532 samples: Cocos 1,965; NVIDIA
graphics driver 1,177; Echo 50; Wave Trail Draw Fix 11. The sampler briefly
stops the main thread at each observation, so it is diagnostic rather than
a throughput benchmark. Cocos export attribution identifies scene traversal,
action management and matrix multiplication as substantial costs.

The next Windows change vectorizes the exact native `kmMat4Multiply` routine
with SSE2. It retains each result's multiplication/summation order and loads
all inputs before storing, preserving overlapping input/output buffers.
Nonfinite inputs and unmasked floating-point exceptions execute a retained
copy of the native routine. A full instruction fingerprint disables the
optimization when the installed routine differs or another mod has patched
it. This changes matrix arithmetic cost while retaining effects and draw order.

`tests/native_matrix_multiply.py` executes the actual installed Windows machine
code on the Linux x86_64 host using its Windows calling convention. Its 50,000
cases cover ordinary/random float bits, aliasing including partial overlap,
rounding modes, denormals and floating-point status. All output bytes and status
flags matched. The isolated benchmark measured about 1.4–1.5x faster including
the exceptional-input check. The production Windows object uses packed SSE
multiply/add instructions without fused arithmetic. This is a small component
improvement; the effect population and graphics-driver work remain dominant.

Recovery evidence is retained in `performance/windows-resume-profile-2026-10-06.json`
and `performance/windows-resume-throughput-2026-10-06.json`. The profiling and
measurement scripts are now saved under `scripts/` so they survive another reset.

The Windows rebuild passed and `dist/decks.echo.windows.geode` matched the
installed package (SHA-256 `8132778e442b64cf683a019ffcda35f59a5ff188e4b4c7fa7c748bc890874b65`).
The preceding package is backed up at `dist/backups/decks.echo.pre-simd.windows.geode`.
After a normal restart, `Geode 2026-10-06 11.58.38.log` confirms both the compact
dash animation and SIMD matrix patch loaded. The game is responsive in Saved
Levels. A controlled full-replay benchmark of the matrix patch remains pending;
no end-to-end speedup is claimed.

## High-TPS trajectory work (2026-10-06)

Trajectory still executes every prediction physics step, collision check,
input branch, and the configured horizon. The initial subpixel polyline reducer
was removed following the explicit requirement for no accuracy loss. Rendering
now merges only exactly collinear forward segments; curves and corners retain
all samples. The reducer is linear, has no tolerance, and preserves reversals
and last-alive death markers. Storage is reused between branches.

Prediction-only hooks remain installed while Show Trajectory is enabled.
An attempted per-frame toggle was reverted: Geode/Tulip create/remove hook
handlers on those transitions. The state test now verifies 100 prediction
passes without handler recreation.

Fake seeding now applies reduced-effects/hidden presentation flags AFTER the
checkpoint copy, which previously overwrote them with live player flags.
Invisible wave trail point insertion is skipped, preserving the native
m_justPlacedStreak reset (verified against the installed Windows routine at
0x3a0680). The trajectory line and death hitboxes are still drawn normally.
No player integration, rotation update, input processing, or collision step is
skipped or resampled.

Orb restoration skips unchanged transform setters and clean oriented-box
rebuilds while restoring changed fields and rebuilding dirty bounds.

Validation: python3 tests/trajectory_path.py (exact sample coverage, straight
paths, curves, reversals and corners up to 20,000 steps), python3
 tests/trajectory_orbs.py (clean, changed, dirty and null restores), and python3
 tests/trajectory_state.py (100 hook lifecycles, seed physics preserved and
20,000 invisible trail calls without trail appends). geode build -p windows
passes and installs the package into the game. End-to-end game FPS has not
been measured for these changes.

### Further high-TPS fixes and endscreen isolation

At 31,010 TPS with a one-second horizon, prediction now takes all 31,010 steps
(62,020 at half time warp). The previous 20,000-step cap silently shortened the
horizon. Removing it restores the configured duration rather than trading
accuracy for speed.

The Windows fake-player position setter now retains the two collision dirty
flags and calls the same native GameObject::setPosition routine, skipping
PlayerObject's 3,243-byte visual tail. That tail places particle, fire, and trail
nodes, including world-coordinate conversions and trigonometry, on every step.
It does not store player physics state. The fast path requires the exact GD
2.2081 body fingerprint, and falls back to the full native setter if another
mod hooks PlayerObject::setPosition or the body differs. Its selected path is
logged on first prediction. Live players always retain the full native setter.
`tests/prediction_art.py` audits the installed body and runs the production
helper over 31,010 position/bounds/unchanged-physics checks. This is a component
check and static native-code audit, not a full-game differential replay.

Prediction also guards PlayLayer::checkForEnd before GD sets
m_hasCompletedLevel. Previously, guarding only the later animation let the
prediction latch completion first, preventing the real finish from opening the
endscreen. tests/trajectory_finish.py exercises 100 predicted finish checks
followed by the real finish, which completes exactly once.

### Real Time and moving objects at extreme TPS

Real Time now budgets catch-up batches using the measured tick cost. It retains
unfinished time in a double-precision accumulator and executes the original
physics steps on later frames. Rendering retains its full step count. When the
requested TPS exceeds hardware throughput, simulation can fall behind wall time,
but oversized catch-up batches no longer monopolize each displayed frame.

On supported Windows builds, moving groups retain every position update, old
position snapshot, dirty flag, section transition, and final native callback.
Objects staying in their existing collision-grid cell skip the section-management
call, whose native unchanged-cell path has no side effects. Exact native body
fingerprints and checks for other mods' hooks guard this optimization; otherwise
the native routine runs.

The native differential test compares 50,000 section decisions and 2,000 movement
groups, including rounding modes, locks, boundaries and null termination. Real Time tests cover up to 1,000,000 TPS, frame spikes, backlog conservation and
draining, warm-up, FPS lock, respawn and capture scheduling.

Real Time and slowdown mode use the same measured physics budget and FPS-lock
setting. Real Time retains pending time; it does not force the stricter FPS-lock
budget. Isolated timing benchmarks were removed; native comparisons check
correctness only. Performance must be evaluated on the affected in-game level.

An active Haze Traveler (`haze75k.echo`, 75,000 TPS) main-thread profile captured
4,627 samples over five seconds: 1,256 were in the movement loop (27.1%). The
compiled section check performed repeated finite/range checks and clamped-domain
selection per axis per object. The common case now uses ordered domain checks:
positions in [0, 1e7), factors in [0, 200], then the same double multiplication
and integer truncation. This domain guarantees representable integer products;
unusual positions/factors retain the full native-compatible path. Movement and
old-position snapshots still execute each tick. This profile identifies a cost;
it does not establish a before/after throughput gain.
