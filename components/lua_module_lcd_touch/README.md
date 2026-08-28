# Lua LCD Touch

`lcd_touch` exposes touch state as Lua tables with press, release, movement,
and hold-time information. Use it for tap buttons, gestures, and interactive
Lua applications.

## Load the module

```lua
local lcd_touch = require("lcd_touch")
```

The module provides two explicit APIs. Choose the API that matches the touch
source; do not use `poll_device()` for the main display while that display is
being managed by the display service.

## `lcd_touch.poll_main()`

Returns the latest touch state for the main display. This is the preferred API
for Lua applications that draw on the board LCD through `display`.

```lua
local touch = lcd_touch.poll_main()
if touch.just_pressed then
    print(string.format("tap at %d, %d", touch.x, touch.y))
end
```

Arguments: none.

Returns one touch-state table. See [Touch-state table](#touch-state-table).

Raises a Lua error if the main display touch state is unavailable or invalid.

## Compatibility APIs

`lcd_touch.poll([handle])` is a compatibility alias for `poll_main()`. Its
optional argument is accepted but ignored, so it is safe for older scripts
that pass the main display touch handle.

`lcd_touch.sync([handle])` is a compatibility no-op. It accepts and ignores
an optional argument, returns `true`, and requires no cleanup.

## `lcd_touch.poll_device(handle)`

Reads a touch device identified by a board-manager handle. Use this API only
for a touch device that is not serving as the main display input.

```lua
local board_manager = require("board_manager")
local lcd_touch = require("lcd_touch")

local handle, err = board_manager.get_lcd_touch_handle("lcd_touch")
if not handle then
    error("touch handle unavailable: " .. tostring(err))
end

local touch = lcd_touch.poll_device(handle)
```

Arguments:

- `handle`: required lightuserdata touch handle, normally returned by
  `board_manager.get_lcd_touch_handle(name)`.

Returns one touch-state table. See [Touch-state table](#touch-state-table).

Raises a Lua error when the handle is missing or invalid, or when reading the
device fails.

## Touch-state table

Both APIs return a table with these fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `pressed` | boolean | `true` while the screen is currently pressed. |
| `just_pressed` | boolean | `true` only on the transition from released to pressed. |
| `just_released` | boolean | `true` only on the transition from pressed to released. |
| `x`, `y` | integer | Touch coordinates. On `just_released`, they identify the last touch position; when idle, both are `0`. |
| `dx`, `dy` | integer | Position change since the previous pressed sample; `0` when not moving. |
| `moved` | boolean | `true` when `dx` or `dy` is non-zero. |
| `held_ms` | number | Duration of the current press in milliseconds; `0` when idle. |

Poll once per application update cycle. Detect taps with `just_pressed` rather
than `pressed` when one action must occur only once per touch. The first poll
establishes the current state and does not report an edge for a touch already
in progress.

## Cleanup

Neither API allocates a Lua resource that needs explicit cleanup. Do not close,
deinitialize, or free the handle returned by `board_manager`; it remains owned
by the board manager. Release the display session through `display.deinit()`
when the application using the main display finishes.
