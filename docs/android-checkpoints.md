# Practice recording checkpoint restoration

Echo previously kept an independent stack of snapshots and always popped its
last entry when Geometry Dash removed a checkpoint. The game's checkpoint limit
evicts the oldest checkpoint, whereas undo removes the newest. That could leave
Echo's state out of sync with the native checkpoint array. Snapshot creation was
also tied to creating a checkpoint, rather than the game accepting it into the
array.

Echo now captures accepted checkpoints in `storeCheckpoint`, associates every
snapshot with its native checkpoint object, reconciles removals with the actual
array, and selects the exact checkpoint passed to `loadFromCheckpoint`. An
unrecognized native checkpoint is never overwritten with a different snapshot.
A normal retry clears the previous restore selection before choosing its target.

Player restoration now uses native mode, gravity, and mini-size methods before
restoring numerical state. These methods synchronize player art and orientation;
the exact saved velocity and rotation are then reapplied because the native
transitions can change them. Snapshots also preserve buffered jump flags, gravity
and scale trigger state, dash parameters, sprite scale, and internal position.

Touchscreen left/right holds no longer depend on a held keyboard key when
restoring a recording. Actual releases still clear the physical hold, and full
restart hold suppression remains in force.

`tests/checkpoint_restore.py` executes the production capture/apply and checkpoint
identity functions against a native-state stub. In desktop and mobile profiles it
checks all 128 mode/gravity transitions, snapshot round trips, idempotent restore,
oldest eviction, newest undo, explicitly selected checkpoints, unknown checkpoint
fallback, clearing, and touch-direction holds. It does not simulate Android game
physics. The existing button pipeline, frame advance, and editor selection tests
also cover related input behavior.

On-device acceptance remains necessary: record through mode/gravity/mini/dash
portals, place and undo checkpoints, hit the native checkpoint cap, die/reset with
a touch held and released, and compare continuation and replay in both dual and
platformer levels. No connected Android device was used for this change.

## Dual-exit release and rendering follow-up

Single-player dual presses are stored canonically on P1. A touch-up delivered
on P2 after leaving dual now closes an unmatched P1 hold in both native button
handling and recording. Independent P2 holds and actual two-player levels keep
their own release lane. `tests/mobile_controls.py` compiles the production
recording function, native release guard and speed setter against a stub.

The Android64 FFmpeg API binary (`eclipse.ffmpeg-api` 2.0.0) passes
`m_colorspaceFilters` directly as options to `avfilter_graph_create_filter` for
an already-selected colorspace filter (Impl::init, 0x1d9e0f0–0x1d9e11c in the
inspected binary). Echo previously supplied a graph-style `colorspace=` prefix,
causing filter initialization failure. The options now omit that prefix. A
failed codec/filter attempt uses a fresh recorder and retries without optional
color correction before trying another codec. Requested preferences remain
unchanged, and failed output files are removed. `tests/recorder_fallback.py`
compiles the production attempt loop to exercise these paths.

These tests cover routing and initialization logic. Device playback across the
reported portal and actual Android video export still require on-device checks.
