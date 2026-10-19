# Echo Macro Scripting

Macro scripts are Lua files that click for you, read the level or edit your
macro.

## Basics

- On **Scripting**, set **Type** to **Macros** and press **New**.
- **Run** runs it, **Stop** stops it. **Save** only saves.
- **Autorun** starts it when the game opens.
- `print` and errors show up in the console.

The file runs once. If it has any of these functions it keeps running:

| Function | When |
| --- | --- |
| `step(frame)` | After every physics step |
| `before_step(frame)` | Before every physics step |
| `attempt(number)` | When an attempt starts |
| `death(frame, player)` | When you die |
| `complete(frame)` | When you beat the level |

```lua
function step(frame)
    if touching("yellow_orb") then click() end
end
```

## Variables

| Name | Meaning |
| --- | --- |
| `player().x`, `player().y` | Position |
| `player().y_velocity` | Vertical speed |
| `player().speed` | Speed portal |
| `player().mode` | `"cube"`, `"ship"`, `"ball"`, `"ufo"`, `"wave"`, `"robot"`, `"spider"`, `"swing"` |
| `player().mini`, `upside_down`, `on_ground`, `dead`, `holding` | True or false |
| `game.frame`, `game.tps`, `game.percent`, `game.attempt` | Numbers |
| `game.in_level`, `game.recording`, `game.playing`, `game.platformer`, `game.practice` | True or false |
| `game.level`, `game.level_id` | The level |
| `script.name` | This script's name |

`player(2)` gives player 2.

## Functions

| Function | Does |
| --- | --- |
| `press()`, `release()`, `click()` | Clicks. Can take `"left"`/`"right"` and a player number. |
| `touching(type)` | What you're touching, like `"yellow_orb"`, or nil |
| `objects(radius)` | Things near you, closest first |
| `survives(action, steps)` | Checks if you live doing `"release"`, `"hold"` or `"click"`. Only in `before_step`. |
| `label.set(name, text)` | Shows text on screen |
| `print_to(name)` | Sends `print` to a label |
| `echo.get(name)`, `echo.set(name, value)` | Echo's settings |
| `echo.notify(text)` | Shows a popup |
| `stop()` | Stops the script |

## Editing the macro

Inputs look like `{ frame = 120, button = "jump", down = true, player = 1 }`.

`macro.count()`, `macro.get(i)`, `macro.set(i, input)`,
`macro.insert(input)`, `macro.remove(i)`, `macro.filter(keep)`,
`macro.clear()`. Only works while not recording or playing.

## Full game access

Turn on **Full Game Access** on the Scripting page to get `gd`:

| Function | Gives |
| --- | --- |
| `gd.play_layer()` | The PlayLayer |
| `gd.player(n)` | Player 1 or 2 (PlayerObject) |
| `gd.level()` | The level (GJGameLevel) |

```lua
function step(frame)
    local p = gd.player()
    if p.m_yVelocity < -10 then p.m_yVelocity = -10 end
end
```

Read and change fields with `object.m_name`, call functions with
`object:name()`. Points and colors are tables like `{ x = 1, y = 2 }`.

- PlayLayer: `m_player1`, `m_player2`, `m_level`, `m_attempts`, `m_clicks`,
  `m_jumps`, `m_currentStep`, `m_timePlayed`, `m_attemptTime`, `m_levelLength`,
  `m_started`, `m_playerDied`, `m_isPaused`, `m_isPracticeMode`, `m_isTestMode`,
  `m_isPlatformer`, `m_hasCompletedLevel`, `m_lastAttemptPercent`, `m_points`,
  `m_orbs`. Functions: `getCurrentPercent()`, `resetLevel()`,
  `resetLevelFromStart()`, `pauseGame(bool)`, `resume()`,
  `togglePracticeMode(on)`, `removeCheckpoint(bool)`, `removeAllCheckpoints()`,
  `posForTime(seconds)`, `shakeCamera(time, strength, interval)`,
  `toggleGroup(id, on)`, `getItemValue(id, type)`.
- PlayerObject: `m_position`, `m_yVelocity`, `m_platformerXVelocity`,
  `m_gravity`, `m_fallSpeed`, `m_playerSpeed`, `m_vehicleSize`,
  `m_isShip`, `m_isBird`, `m_isBall`, `m_isDart`, `m_isRobot`, `m_isSpider`,
  `m_isSwing`, `m_isUpsideDown`, `m_isOnGround`, `m_isDashing`, `m_isDead`,
  `m_holdingLeft`, `m_holdingRight`, `m_lastJumpTime`, `m_playerColor1` and
  more. Functions: `setPosition(point)`, `setRotation(degrees)`,
  `setColor(color)`, `setYVelocity(speed, 0)`, `flipGravity(on, true)`,
  `togglePlayerScale(on, true)`, `toggleFlyMode(on, true)` (also `Roll`,
  `Bird`, `Dart`, `Robot`, `Spider`, `Swing`), `stopDashing()`.
- GJGameLevel: `m_levelID`, `m_levelName`, `m_creatorName`, `m_songID`,
  `m_stars`, `m_difficulty`, `m_attempts`, `m_normalPercent`,
  `m_practicePercent`, `m_bestTime`. Functions: `getSongName()`.

Objects only last until the function they were made in ends, so get them
again every time. Changing a field or calling a function turns on safe mode
until you leave the level.
