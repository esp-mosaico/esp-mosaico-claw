#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""GSP scene for the Classic Bluetooth A2DP media App."""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import NOTO_SANS  # noqa: E402
from scene_common import (  # noqa: E402
    ASCII_PRINTABLE,
    CONTENT_H,
    CONTENT_W,
    asset_scene,
    auto_charset,
    button,
    container,
    explicit_charset,
    image,
    label,
    layer,
    scene_out_path,
    shared_charset,
    shared_prefix,
    top_notice,
    write_scene,
)

FONT = NOTO_SANS
BOLD = FONT
BT_TEXT_CHARS = ASCII_PRINTABLE + "·"
ASSETS = "../../../common/assets/control_center"
BT_ASSETS = "../../../common/assets/bluetooth"
COVER_PLACEHOLDER = "bluetooth_cover_placeholder.png"

IMAGES: list[tuple[str, int, int]] = [
    (f"{BT_ASSETS}/vinyl_overlay.png", 260, 260),
    (f"{BT_ASSETS}/tonearm.png", 114, 271),
    (f"{BT_ASSETS}/bt_platter.png", 159, 159),
    (f"{BT_ASSETS}/bt_halftone.png", 225, 358),
    (f"{ASSETS}/prev.png", 48, 48),
    (f"{ASSETS}/play.png", 48, 48),
    (f"{ASSETS}/pause.png", 48, 48),
    (f"{ASSETS}/next.png", 48, 48),
    (f"{ASSETS}/sound.png", 48, 48),
]
FONT_POLICIES = {
    14: shared_charset(),
    18: shared_charset(),
    24: auto_charset(),
    28: shared_charset(),
    32: shared_charset(),
    36: shared_charset(),
}


def call_event(name: str) -> list[dict]:
    return [{"event": "click", "action": "call", "target_name": name}]


def generate_cover_placeholder() -> None:
    """Build the checked-in scene's neutral artwork without another asset tool."""
    size = 180
    canvas = Image.new("RGB", (size, size), "#242426")
    draw = ImageDraw.Draw(canvas)
    for y in range(size):
        ratio = y / (size - 1)
        color = (
            255,
            round(76 - 28 * ratio),
            round(1 + 15 * ratio),
        )
        draw.line((0, y, size, y), fill=color)
    # Quiet dotted texture echoes the orange cards used by the other pages.
    for row in range(5):
        for column in range(5):
            x = 108 + column * 13
            y = 20 + row * 13
            draw.ellipse((x, y, x + 6, y + 6), fill="#FF7540")
    # Geometric music note; no host-font dependency in generated assets.
    draw.rounded_rectangle((68, 48, 79, 121), radius=5, fill="#FFFFFF")
    draw.rounded_rectangle((76, 48, 125, 59), radius=5, fill="#FFFFFF")
    draw.rounded_rectangle((116, 52, 127, 108), radius=5, fill="#FFFFFF")
    draw.ellipse((48, 108, 80, 136), fill="#FFFFFF")
    draw.ellipse((96, 94, 128, 122), fill="#FFFFFF")
    canvas.save(HERE / COVER_PLACEHOLDER)


def icon_button(objs, parent, x, y, size, asset, action, *, active=False):
    button_index = len(objs)
    objs.append(button(
        parent, x, y, size, size, "", bg="#FF4C01" if active else "#181819",
        radius=size // 2, events=call_event(action),
        border="#303033" if not active else None,
        border_w=1 if not active else 0,
    ))
    icon_size = 48
    offset = (size - icon_size) // 2
    objs.append(image(button_index, asset, offset, offset, icon_size, icon_size))
    return button_index


def build_ui(objs, content):
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      name="bluetooth_root"))
    objs.append(container(root, 0, 0, CONTENT_W, CONTENT_H,
                          bg="#000000", name="bluetooth_bg"))
    # Waiting/linking page from the unified prototype.
    waiting = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H,
                      name="bt_waiting", bind="bt_waiting_visible"))
    objs.append(label(waiting, 24, 17, 82, 32, "Local", size=24,
                      color="#91919B", callback="bt_local"))
    objs.append(label(waiting, 113, 17, 150, 32, "Bluetooth", size=24,
                      color="#FCFCFF"))
    objs.append(container(waiting, 161, 47, 13, 13,
                          bg="#FF4C01", radius=3))
    halftone = image(waiting, f"{BT_ASSETS}/bt_halftone.png",
                     255, 0, 225, 358)
    halftone["opacity"] = 72
    objs.append(halftone)
    objs.append(label(waiting, 24, 260, 432, 43,
                      "ESP-Claw-Audio", size=36,
                      color="#FCFCFF", font_charset=ASCII_PRINTABLE))
    objs.append(label(waiting, 24, 398, 432, 34,
                      "Open Bluetooth settings on your", size=24,
                      color="#91919B", font_charset=ASCII_PRINTABLE))
    objs.append(label(waiting, 24, 432, 432, 34,
                      "phone and connect to MOSAICO.", size=24,
                      color="#91919B", font_charset=ASCII_PRINTABLE))
    # Host/WASM uses this surface to emulate the phone completing a link.
    # Firmware receives the action too, but deliberately ignores it and keeps
    # using the real Bluetooth runtime state.
    objs.append(container(waiting, 0, 64, CONTENT_W, CONTENT_H - 64,
                          bg="#000000", opacity=0,
                          callback="bt_sim_connect"))

    player = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H, hidden=True,
                      name="bt_player", bind="bt_player_visible"))
    objs.append(label(player, 24, 17, 82, 32, "Local", size=24,
                      color="#91919B", callback="bt_local"))
    objs.append(label(player, 113, 17, 150, 32, "Bluetooth", size=24,
                      color="#FCFCFF"))
    objs.append(container(player, 161, 47, 13, 13,
                          bg="#FF4C01", radius=3))
    placeholder = len(objs)
    objs.append(layer(player, 161, 99, 159, 159,
                      name="bt_cover_placeholder",
                      bind="bt_cover_placeholder_visible"))
    objs.append(image(placeholder, f"{BT_ASSETS}/bt_platter.png",
                      0, 0, 159, 159,
                      events=call_event("bt_toggle_play")))
    cover = len(objs)
    objs.append(layer(player, 161, 99, 159, 159, hidden=True,
                      name="bt_cover_layer", bind="bt_cover_visible"))
    # The runtime image inherits its compiled placeholder pixel format.
    # Keep this placeholder RGB: using the transparent RGBA platter here
    # makes JPEG cover art decode target an alpha format, which GSP's JPEG
    # decoder does not support. The idle circular artwork remains in the
    # separate placeholder layer.
    objs.append(image(cover, COVER_PLACEHOLDER, 0, 0, 159, 159,
                      name="bt_cover", bind="bt_cover",
                      events=call_event("bt_toggle_play")))
    # Album art stays underneath the vinyl. Its decoded image is swapped
    # atomically, while this transparent-centre overlay remains unchanged.
    base_static = len(objs)
    objs.append(layer(player, 110, 48, 260, 260,
                      name="bt_disc_static"))
    objs.append(image(base_static, f"{BT_ASSETS}/vinyl_overlay.png",
                      0, 0, 260, 260,
                      events=call_event("bt_toggle_play")))
    objs.append(image(player, f"{BT_ASSETS}/tonearm.png",
                      273, 57, 93, 221))

    title = label(player, 12, 300, 408, 48, "Music name", size=32,
                  color="#FCFCFF", bind="bt_title",
                  font_charset=BT_TEXT_CHARS)
    title["overflow"] = "ellipsis"
    objs.append(title)
    artist = label(player, 12, 332, 408, 35, "Singer", size=18,
                   color="#B8B8C0", bind="bt_artist",
                   font_charset=BT_TEXT_CHARS)
    artist["overflow"] = "ellipsis"
    objs.append(artist)
    objs.append({
        "type": "progress", "parent": player,
        "x": 12, "y": 375, "w": 456, "h": 8,
        "value": 0, "min": 0, "max": 100,
        "bind": "bt_progress", "name": "bt_progress",
        "bg_color": "#181819", "fg_color": "#3B3C3D", "radius": 4,
    })
    objs.append({
        "type": "container", "parent": player,
        "name": "bt_progress_cursor",
        "x": {"default": 8, "min": 8, "max": 464},
        "y": {"default": 372, "min": 370, "max": 374},
        "w": 8, "h": 14, "bg_color": "#FF4C01",
    })
    objs.append(label(player, 12, 398, 100, 24, "00:00", size=18,
                      color="#77777F", bind="bt_elapsed",
                      font_charset=ASCII_PRINTABLE))
    objs.append(label(player, 368, 398, 100, 24, "00:00", size=18,
                      color="#77777F", align="right", bind="bt_duration",
                      font_charset=ASCII_PRINTABLE))
    objs.append(image(player, f"{ASSETS}/prev.png", 130, 420, 52, 52,
                      events=call_event("bt_previous")))
    play_button = len(objs)
    objs.append(button(player, 214, 420, 52, 52, "", bg="#000000",
                       radius=0, events=call_event("bt_toggle_play")))
    play = len(objs)
    objs.append(layer(play_button, 2, 2, 48, 48,
                      name="bt_play", bind="bt_play_visible"))
    objs.append(image(play, f"{ASSETS}/play.png", 0, 0, 48, 48))
    pause = len(objs)
    objs.append(layer(play_button, 2, 2, 48, 48, hidden=True,
                      name="bt_pause", bind="bt_pause_visible"))
    objs.append(image(pause, f"{ASSETS}/pause.png", 0, 0, 48, 48))
    objs.append(image(player, f"{ASSETS}/next.png", 298, 420, 52, 52,
                      events=call_event("bt_next")))
    objs.append({
        "type": "slider", "parent": player,
        "x": 447, "y": 88, "w": 18, "h": 204,
        "vertical": True, "value": 80, "min": 0, "max": 100,
        "bind": "bt_volume_level", "name": "bt_volume_slider",
        "bg_color": "#2B2B2D", "fg_color": "#FF4C01",
        "knob": True, "knob_color": "#FF4C01", "track_size": 4,
    })
    objs.append(label(
        player, 432, 64, 48, 20, "80%", size=14,
        color="#91919B", align="center", bind="bt_volume",
        font_charset=ASCII_PRINTABLE))
    # Preserve runtime-only status/device/volume binds without exposing the
    # old card layout in the unified player.
    hidden = len(objs)
    objs.append(layer(root, 0, 0, 1, 1, hidden=True, name="bt_hidden"))
    objs.append(label(hidden, 0, 0, 1, 1, "", size=14, color="#000000",
                      bind="bt_status", name="bt_status",
                      font_charset=BT_TEXT_CHARS))
    objs.append(label(hidden, 0, 0, 1, 1, "", size=14, color="#000000",
                      bind="bt_device_name", font_charset=ASCII_PRINTABLE))


def main():
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD)
    build_ui(objs, content)
    top_notice(
        objs, content, name="bt_top_notice", font=FONT,
        title="Disconnecting Bluetooth",
        message="Returning to Home...",
    )
    write_scene(scene_out_path(HERE, "bluetooth_480.json"), "bluetooth",
                objs, font=FONT)
    asset_scene(scene_out_path(HERE, "bluetooth_assets_480.json"),
                "bluetooth_assets", IMAGES, FONT_POLICIES, BOLD, FONT)


if __name__ == "__main__":
    main()
