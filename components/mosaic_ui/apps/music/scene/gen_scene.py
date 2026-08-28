#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS  # noqa: E402
from scene_common import (  # noqa: E402
    asset_scene,
    CONTENT_H,
    CONTENT_W,
    button,
    container,
    image,
    label,
    layer,
    scene_out_path,
    shared_charset,
    shared_prefix,
    write_scene,
)

FONT = DEJAVU_SANS
BOLD = FONT
FONT_POLICIES = {
    13: shared_charset(),
    15: shared_charset(),
    20: shared_charset(),
    22: shared_charset(),
    24: shared_charset(),
    18: shared_charset(),
    32: shared_charset(),
}
RUNTIME_CHARSET = shared_charset()["charset"]
ASSETS = "../../../common/assets/control_center"
MUSIC_ASSETS = "../../../common/assets/music"
IMAGES = [
    (f"{MUSIC_ASSETS}/vinyl_base.png", 260, 260),
    (f"{MUSIC_ASSETS}/vinyl_label.png", 159, 159),
    (f"{MUSIC_ASSETS}/tonearm.png", 114, 271),
    (f"{MUSIC_ASSETS}/music_loop.png", 48, 48),
    (f"{MUSIC_ASSETS}/music_shuffle.png", 48, 48),
    (f"{MUSIC_ASSETS}/music_list.png", 48, 48),
    (f"{ASSETS}/prev.png", 48, 48),
    (f"{ASSETS}/play.png", 48, 48),
    (f"{ASSETS}/pause.png", 48, 48),
    (f"{ASSETS}/next.png", 48, 48),
]

# Modern dark theme.
BG_TOP = "#0E1119"
BG_BOT = "#191F2C"
CARD = "#1E2531"
CARD2 = "#2A3242"
INK = "#F3F5F9"
MUTED = "#8A93A8"
ACCENT = "#6E8BFF"
ACCENT2 = "#9C6BFF"
TRACK = "#2A3242"
DIVIDER = "#232A38"


def grad(parent, x, y, w, h, c0, c1, *, radius=0, direction="vertical",
         name=None, bind=None, callback=None, border=None, border_w=0):
    """Container with a linear background gradient (raw GSP keys)."""
    obj = {
        "type": "container", "parent": parent,
        "x": x, "y": y, "w": w, "h": h,
        "bg_color": c0, "bg_gradient": c1, "gradient_dir": direction,
        "radius": radius,
    }
    if name:
        obj["name"] = name
    if bind:
        obj["bind"] = bind
    if callback:
        obj["callback"] = callback
    if border:
        obj["border_color"] = border
        obj["border_width"] = border_w
    return obj


def header(objs, parent, title, *, back=True):
    objs.append(container(parent, 0, 0, CONTENT_W, 56, bg=BG_TOP))
    objs.append(container(parent, 0, 55, CONTENT_W, 1, bg=DIVIDER))
    if back:
        back_btn = container(
            parent, 18, 16, 24, 24, bg="#FF4C01", radius=4)
        objs.append(back_btn)
    objs.append(label(
        parent, 52, 15, 300, 28, title, size=24, color=INK,
        align="left"))


def library_row(objs, parent, y, index, title, artist):
    row = len(objs)
    row_obj = container(
        parent, 0, y, 480, 100, bg="#000000", radius=0,
        name=f"music_library_row_{index}",
        bind=f"music_library_row_{index}_visible",
        callback=f"music_track_{index}")
    row_obj["bind_target"] = "visible"
    objs.append(row_obj)
    playing = len(objs)
    objs.append(layer(
        row, 18, 32, 24, 32, hidden=index != 0,
        name=f"music_library_playing_{index}",
        bind=f"music_library_playing_{index}_visible"))
    objs.append(container(playing, 0, 12, 3, 20, bg="#FF4C01"))
    objs.append(container(playing, 9, 4, 3, 28, bg="#FF4C01"))
    objs.append(container(playing, 18, 8, 3, 24, bg="#FF4C01"))
    objs.append(label(
        row, 58, 20, 398, 40, title, size=32, color="#FCFCFF",
        bind=f"music_library_title_{index}",
        name=f"music_library_title_{index}",
        font_charset=RUNTIME_CHARSET,
        callback=f"music_track_{index}"))
    objs.append(label(
        row, 58, 62, 398, 20, artist, size=15, color="#91919B",
        bind=f"music_library_artist_{index}",
        name=f"music_library_artist_{index}",
        font_charset=RUNTIME_CHARSET,
        callback=f"music_track_{index}"))
    objs.append(container(row, 0, 99, 480, 1, bg="#3B3C3D"))


def build_library(objs, parent):
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H, bg="#000000"))
    objs.append(label(parent, 16, 6, 64, 52, "‹", size=32,
                      color="#91919B", callback="music_now_playing",
                      font_charset="‹"))
    objs.append(label(parent, 0, 16, 480, 32, "Local Music", size=24,
                      color="#91919B", align="center"))
    library_row(objs, parent, 64, 0, "Midnight Drive", "Mosaico")
    library_row(objs, parent, 164, 1, "Blue Monday", "ESP Lab")
    library_row(objs, parent, 264, 2, "Quiet Circuit", "Claw Ensemble")
    hidden = len(objs)
    objs.append(layer(parent, 0, 0, 1, 1, hidden=True))
    objs.append(label(
        hidden, 0, 0, 1, 1, "3 songs",
        size=13, color="#000000",
        bind="music_library_summary", name="music_library_summary",
        font_charset=RUNTIME_CHARSET))


def cover(objs, parent, index, color, accent, caption):
    cover_layer = len(objs)
    objs.append(layer(
        parent, 140, 64, 200, 200, hidden=index != 0,
        name=f"music_cover_{index}",
        bind=f"music_cover_{index}_visible"))
    # Album tile: solid card + a vinyl-disc motif + caption.
    objs.append(container(cover_layer, 0, 0, 200, 200, bg=color,
                          radius=26, border="#FFFFFF", border_w=1))
    objs.append(container(cover_layer, 58, 34, 84, 84, bg="#0E1119",
                          radius=42, opacity=210))
    objs.append(container(cover_layer, 72, 48, 56, 56, bg=accent, radius=28))
    objs.append(container(cover_layer, 90, 66, 20, 20, bg=color, radius=10))
    objs.append(label(
        cover_layer, 12, 150, 176, 22, caption, size=13,
        color="#F3F5F9", align="center"))


def transport(objs, parent, x, y, d, text, callback, name, *, size=18):
    btn = button(
        parent, x, y, d, d, text, bg=CARD2, fg=INK, radius=d // 2,
        size=size, callback=callback, name=name)
    # Only the triangle glyphs are needed, so seed an explicit tiny charset.
    btn["font_charset"] = "".join(sorted(set(text)))
    objs.append(btn)


def build_player(objs, parent):
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H, bg="#000000"))
    objs.append(label(parent, 24, 17, 82, 32, "Local", size=24,
                      color="#FCFCFF"))
    objs.append(label(parent, 113, 17, 150, 32, "Bluetooth", size=24,
                      color="#91919B", callback="music_bluetooth"))
    objs.append(container(parent, 50, 47, 13, 13, bg="#FF4C01", radius=3))
    disc_static = len(objs)
    objs.append(layer(parent, 110, 48, 260, 260,
                      name="music_disc_static"))
    objs.append(image(disc_static, f"{MUSIC_ASSETS}/vinyl_base.png",
                      0, 0, 260, 260, callback="music_toggle"))
    objs.append(image(disc_static, f"{MUSIC_ASSETS}/vinyl_label.png",
                      51, 51, 159, 159, callback="music_toggle"))
    objs.append(image(parent, f"{MUSIC_ASSETS}/tonearm.png",
                      273, 57, 93, 221))
    objs.append(label(parent, 12, 300, 408, 48, "Music name", size=32,
                      color="#FCFCFF", bind="music_title",
                      name="music_title", font_charset=RUNTIME_CHARSET))
    objs.append(label(parent, 12, 332, 408, 35, "Singer", size=18,
                      color="#91919B", bind="music_artist",
                      name="music_artist", font_charset=RUNTIME_CHARSET))
    hidden = len(objs)
    objs.append(layer(parent, 0, 0, 1, 1, hidden=True,
                      name="music_hidden_metadata"))
    objs.append(label(hidden, 0, 0, 1, 1, "", size=13, color="#000000",
                      bind="music_album", name="music_album",
                      font_charset=RUNTIME_CHARSET))
    objs.append(label(hidden, 0, 0, 1, 1, "", size=13, color="#000000",
                      bind="music_track_count", name="music_track_count",
                      font_charset=RUNTIME_CHARSET))
    objs.append(label(hidden, 0, 0, 1, 1, "", size=13, color="#000000",
                      bind="music_toggle_text", name="music_toggle_text",
                      font_charset=RUNTIME_CHARSET))

    # Keep the volume control deliberately quiet: the same graphite track and
    # orange accent as the transport progress, with a wider generated hit box.
    objs.append({
        "type": "slider", "parent": parent,
        "x": 447, "y": 88, "w": 18, "h": 204,
        "vertical": True, "value": 65, "min": 0, "max": 100,
        "bind": "music_volume_level", "name": "music_volume_slider",
        "bg_color": "#2B2B2D", "fg_color": "#FF4C01",
        "knob": True, "knob_color": "#FF4C01", "track_size": 4,
    })
    objs.append(label(
        parent, 432, 64, 48, 20, "65%", size=14,
        color="#91919B", align="center", bind="music_volume_text",
        name="music_volume_text", font_charset=RUNTIME_CHARSET))

    objs.append({
        "type": "progress", "parent": parent,
        "x": 12, "y": 375, "w": 456, "h": 8,
        "value": 0, "min": 0, "max": 100,
        "bind": "music_progress", "name": "music_progress",
        "bg_color": "#181819", "fg_color": "#3B3C3D", "radius": 4,
    })
    objs.append({
        "type": "container", "parent": parent,
        "name": "music_progress_cursor",
        "x": {"default": 8, "min": 8, "max": 464},
        "y": {"default": 372, "min": 370, "max": 374},
        "w": 8, "h": 14, "bg_color": "#FF4C01",
    })
    objs.append(label(
        parent, 12, 398, 90, 24, "00:00", size=18, color="#91919B",
        bind="music_elapsed", name="music_elapsed",
        font_charset=RUNTIME_CHARSET))
    objs.append(label(
        parent, 378, 398, 90, 24, "03:20", size=18, color="#91919B",
        align="right", bind="music_remaining", name="music_remaining",
        font_charset=RUNTIME_CHARSET))
    # Keep the five transport slots on the same 48 px grid as the prototype.
    repeat_order = len(objs)
    objs.append(layer(parent, 12, 422, 48, 48,
                      name="music_repeat_order",
                      bind="music_repeat_order_visible"))
    objs.append(image(repeat_order, f"{MUSIC_ASSETS}/music_loop.png",
                      0, 0, 48, 48, callback="music_repeat"))
    repeat_shuffle = len(objs)
    objs.append(layer(parent, 12, 422, 48, 48, hidden=True,
                      name="music_repeat_shuffle",
                      bind="music_repeat_shuffle_visible"))
    objs.append(image(repeat_shuffle, f"{MUSIC_ASSETS}/music_shuffle.png",
                      0, 0, 48, 48, callback="music_repeat"))
    objs.append(image(parent, f"{ASSETS}/prev.png", 130, 420, 52, 52,
                      callback="music_previous", name="music_previous"))
    play = len(objs)
    objs.append(layer(parent, 214, 420, 52, 52,
                      name="music_play", bind="music_play_visible"))
    objs.append(image(play, f"{ASSETS}/play.png", 0, 0, 52, 52,
                      callback="music_toggle", name="music_toggle"))
    pause = len(objs)
    objs.append(layer(parent, 214, 420, 52, 52, hidden=True,
                      name="music_pause", bind="music_pause_visible"))
    objs.append(image(pause, f"{ASSETS}/pause.png", 0, 0, 52, 52,
                      callback="music_toggle"))
    objs.append(image(parent, f"{ASSETS}/next.png", 298, 420, 52, 52,
                      callback="music_next", name="music_next"))
    objs.append(image(parent, f"{MUSIC_ASSETS}/music_list.png",
                      420, 422, 48, 48,
                      callback="music_menu", name="music_menu"))


def main():
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD)
    stack = len(objs)
    objs.append({
        "type": "stackview", "parent": content,
        "x": 0, "y": 0, "w": CONTENT_W, "h": CONTENT_H,
        "name": "music_stack", "page_count": 2, "initial_page": 1,
        "capacity": 2, "axis": "horizontal",
    })
    library = len(objs)
    objs.append(layer(stack, 0, 0, CONTENT_W, CONTENT_H,
                      name="music_stack_page0"))
    build_library(objs, library)
    player = len(objs)
    objs.append(layer(stack, CONTENT_W, 0, CONTENT_W, CONTENT_H,
                      name="music_stack_page1"))
    build_player(objs, player)
    write_scene(scene_out_path(HERE, "music_480.json"),
                "music", objs, font=FONT)
    asset_scene(scene_out_path(HERE, "music_assets_480.json"),
                "music_assets", IMAGES, FONT_POLICIES, BOLD, FONT)


if __name__ == "__main__":
    main()
