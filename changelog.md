# v2.2.1

- echogd.com! browse and download macros, clickpacks and themes, or upload ur own right from echo
- Online tab in the macro, clickpack and theme pickers so u can search echogd.com in game and play a macro without saving it
- Macrobuff tint: in the editor the screen turns red if something u place or move would kill u on ur last playtest
- Trajectory is way more accurate now
- Trajectory keeps updating while frame advancing
- Trajectory length is based on how much ram you have available
- Fixed Frame Cadence: moving objects update at a steady rate so the trajectory can actually predict them
- Skip Far Moving Objects: only checks moving objects that can reach u, way less lag
- Lowest FPS in real time settings
- Holding frame advance doesnt skip frames anymore unless real time is on
- Stacked orbs and big piles of effects lag a lot less (Batched Effects, Prefetch Helper)
- Default Art for layout mode
- Position decimals for labels
- 8K 60 and 8K 120 render presets
- typing a frame rate in the renderer actually uses that number now
- Real Time Rendering is pc only now, and it works with True Real Time on
- fixed keybinds
- fixed predicted dash orb spins going way too fast
- fixed the p2 trajectory line jumping after dual portals
- cleaned up a lot of the code
- probably more that i forgot

# v2.2.0

- New menu with its own look, pages are now Replay, Assists, Visuals, Editor, Library, Clickbot, Render and Settings
- Themes! pick a base color or make ur own, and Randomize gives u a random one that still looks like echo. the logo changes color with it
- Undocked mode is a mod menu now, every section is a tile u can pin or hide
- Settings has a list of every keybind
- Merge In: add another macros inputs to the one ur working on
- Video presets for common sizes and frame rates, and u can save ur own
- Docked and undocked menus remember their own size

# v2.1.0

- Backstepping! hopefully not too laggy
- Prevent death (used to be pause before kill) no longer needs trajectory
- Recording utilities modal removed and orb options are in tools
- Follow player: one player can copy the other players movements after a certain frame count or ms amount while recording
- Safe mode (its really auto safe mode)
- Velocity unrounding is now built in, but will disable itself if you have the chizz mod enabled
- Moving objects support now toggleable in trajectory options. Its a toggle cuz its laggy as hell in some levels
- Outline only hitboxes (try it, hard to explain but it shows the outline rather than bunching hitboxes together)
- Labels now autoposition, and u can use gd fonts now
- Utility label: shows mods u have enabled that affect recording/idling
- split tools into 2 windows, macro tools & visual
- Render with speedhack
- high precision editor is built in so you dont need the mod to macrobuff
- Trajectory and high tps is more optimized
- fixed accuracy bugs n recording bugs
- fixed ssb fix being weird/not working
- should be more performant all around
- probably more that i forgot

# v2.0.0

- MUCH better accuracy and performance
- Compressed macros
- Sync Audio
- Maintain Gravity
- No Mirror Portals
- Fix Rotation Bug (Visual)
- Hide Player
- Click Dash Orbs + Auto Click Blue/Black Orb on Pads
- Trajectory (+ pause before death)
- Conversion Types: Silicate, TCBot, GDR
- TPS Changes!
- Extra labels: position, velocity, alive/dead, max cps, gravity, death on next frame
- Render: Merge Renders, Immediate Restart, Hide Labels, Clickbot in video
- Much better hitboxes (including click hitboxes!)
- Full record/play capabilities inside the editor
- Level seed override
