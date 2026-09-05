---
{
  "name": "album_app",
  "description": "Launch an LVGL photo album on the board LCD to browse saved pictures from storage. Requires a display peripheral.",
  "author": "ESP-Claw contributor",
  "metadata": {
    "category": ["game"],
    "tags": ["album", "gallery", "photo", "picture", "lvgl", "touch"],
    "peripherals": ["display"],
    "cap_groups": ["cap_lua"]
  }
}
---

# Album App

Use this skill when the user asks to open a photo album, browse pictures, view
saved photos, or launch an LVGL gallery on the board LCD.

The Lua script uses LVGL to draw a thumbnail grid. Tap a thumbnail to open it
full-screen, use the left and right arrow buttons to switch photos, and tap the
picture/background area to return to the grid. It scans a DATA-root directory
for JPEG photos and LVGL `.bin` image assets, then displays JPEG photos on LVGL
canvas widgets.

## Requirements

- A display device declared as `display_lcd` in board hardware info.
- LCD touch declared as `lcd_touch` for button interaction.
- JPEG photos or LVGL `.bin` images under the DATA root, defaulting to `photos/`.

## Tool Call Inputs

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/album_app.lua",
  "args": {}
}
```

Common optional args:

| Arg | Default | Meaning |
|-----|---------|---------|
| `dir` | `photos` | Single directory name under the DATA root |
| `run_time_ms` | `180000` | App runtime in milliseconds |

If startup or runtime fails, report the `[album_app] ...` error line directly
to the user and do not retry with changed arguments unless the user asks.

## Files

- `scripts/album_app.lua`
