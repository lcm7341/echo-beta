# v2.1.0

First update since the beta launch on October 6, 2026.

## New

- Backstep: step back frame by frame while recording, with a keybind and a setting for how many saved states to keep.
- Prevent Death: while recording, frame advance stops on the frame before a death. No longer needs Show Trajectory.
- Orb assists while recording: Click Dash Orbs, Click Black Orbs, Auto Black Orb on Pad and Auto Blue Orb on Pad, each with a keybind.
- Follow Player: in two-player levels, one player copies the other's clicks after a set delay.
- Safe Mode: attempts that use any hack don't save level progress.
- Velocity Unrounding built in. Each macro remembers whether it was recorded with it.
- Trajectory option to move the level's moving objects along with the prediction.
- Layout mode: Default Art option shows blocks, spikes and saws as the default ones, sized to their hitboxes.
- Hitboxes: Outline Only option.
- Labels: Auto Position stacks labels from a corner, with drag-to-reorder, font, size and opacity options.
- New label listing every aid that is turned on.
- Render: Use Speedhack option renders the video at your speedhack speed.

## Improved

- Dash effects are faster at high TPS and are handled automatically, so the Limit/GPU Dash Effects toggles are gone.
- Sync Audio keeps music playing smoothly at normal pitch when the game slows down.
- Real Time mode keeps up much better and no longer freezes on one frame while catching up.
- Trajectory refreshes faster and now follows moving spikes and dual portals.
- Trajectory stops at the end of the level, so the end animation plays like in vanilla.
- Silicate macros now load and save their seed and use Silicate's object randomness.
- High Precision Editor is now a required dependency.

## Fixed

- Fixed second-player keys (like the up arrow) in one-player levels being recorded for player 2 and doing nothing on playback.
- Fixed showing hitboxes changing collisions.
- Fixed hitboxes appearing on disabled objects and objects that can't be collided with.
- Fixed the player circle hitbox not showing on levels that use it.
- Fixed No Mirror Portals changing gameplay. It now only changes what you see.
- Fixed practice respawns leaving the player or moving objects slightly out of place, which broke recordings made with checkpoints.
- Fixed practice checkpoints getting out of sync with the game's own checkpoints.
- Fixed checkpoints using up object IDs and randomness, which could make recordings break later in a level.
- Fixed Layout mode hiding some hazards and painting blocks the wrong colors.
- Fixed orbs in the trajectory staying used when the player never actually hit them.
- Fixed crashes in some levels with very far-away objects or a pending checkpoint.
- Fixed the level not finishing when the trajectory predicted the end first.
- Fixed render audio jumping around.

## Performance

- Much faster playback on heavy high-TPS levels: moving objects, collisions, orb spam and ring effects all cost less.
- Less lag with the trajectory on at high TPS and over long prediction lengths.
- Less lag with hitboxes on.
- Noclip no longer slows the game down while touching hazards.
- Removed a stutter during the opening pause before the level starts.
