# Theme Scripting Guide

```lua
theme = {
    name = "My Theme",
    logo = "logo.png",
    palette = { body = 0.75, accent = 0.9, light = false, rainbow = false },
    style_colors = { Text = "#FFFFFF" },
    metrics = { frame_rounding = 3, item_spacing = {8, 4} },
    sharp = false,
    layout = { nav = "top", pages = { "Replay", "Settings" } },
}
```

## Redrawing widgets

Make a function with a widget's name and Echo calls it instead of drawing it.
`w.default()` draws the normal look.

```lua
function button(w)
    w.default()
    draw.rect(w.x, w.y, w.x + w.w, w.y + w.h, colors.accent, w.rounding, 1)
end
```

Widgets: `window_background`, `window_frame`, `card`, `header`, `button`,
`action_button`, `checkbox`, `field`, `slider`, `selectable`, `nav_item`,
`nav_marker`, `separator`, `heading`, `progress`, `close_button`, `gear`,
`help`, `status_dot`, `swatch`, `scroll_mark`, `logo`, `title_bar`,
`nav_bar`, `status_bar`, `bar`, `tile_bar`, `collapse_bar`, `theme_tile`,
`floating_logo`.

Also: `update(dt)` runs every frame, `overlay(dt)` draws over the screen and
`background(area)` draws one background behind every window.

## Variables

| Name | Meaning |
| --- | --- |
| `w.x`, `w.y`, `w.w`, `w.h` | Widget position and size |
| `w.cx`, `w.cy` | Widget middle |
| `w.label` | Widget text |
| `w.hover`, `w.down`, `w.on` | 0 to 1 |
| `w.value` | 0 to 1 for sliders and progress bars |
| `w.rounding`, `w.tint`, `w.id` | Rounding, color, unique number |
| `w.pressed`, `w.clicked` | True on the frame it happens |
| `time`, `dt` | Seconds |
| `unit` | Font size |
| `screen.w`, `screen.h` | Screen size |
| `mouse.x`, `mouse.y`, `mouse.down`, `mouse.clicked`, `mouse.wheel` | Mouse |
| `sharp`, `docked` | True or false |
| `colors.text`, `colors.accent`, `colors.window`, `colors.card`, `colors.frame`, `colors.button`, `colors.border` | Theme colors |

## Functions

- Colors: `rgb(r, g, b, a)`, `hsv(h, s, v, a)`, `blend(a, b, t)`,
  `fade(color, amount)`. A color can also be `"#RRGGBB"`.
- Math: `lerp`, `clamp`, `smoothstep`, `noise`, `ease(key, target)`.
- Drawing: `draw.fill`, `draw.rect`, `draw.gradient`, `draw.line`,
  `draw.circle`, `draw.disc`, `draw.triangle`, `draw.polygon`, `draw.glow`,
  `draw.text`, `draw.image`, `draw.logo`, `draw.clip`, `draw.unclip`.
  Most take `x1, y1, x2, y2, color`.
