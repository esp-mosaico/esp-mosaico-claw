#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Album grid app matching the dark gallery mockup."""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H,
    CONTENT_W,
    button,
    container,
    explicit_charset,
    image,
    label,
    layer,
    scene_out_path,
    shared_prefix,
    shared_charset,
    write_scene,
)

FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD
ASSETS = "../../../common/assets"
PLACEHOLDER = f"{ASSETS}/camera_canvas.png"

IMAGES = [
    (PLACEHOLDER, 456, 340),
]
FONT_POLICIES = {
    12: shared_charset(),
    14: shared_charset(),
    16: shared_charset(),
    18: shared_charset(),
    20: shared_charset(),
    24: shared_charset(),
    32: shared_charset(),
    40: explicit_charset("<>"),
}

TILE = 150
GAP = 4
COLUMNS = 3
GRID_X = 8
GRID_Y = 70
GRID_W = 464
# Reserve the bottom system gesture band and its shared home indicator.
GRID_H = CONTENT_H - GRID_Y - 20


def dynamic_image(parent, x, y, w, h, *, name, bind, image_path=PLACEHOLDER, fit="cover", scalable=False, callback=None):
    obj = {
        "type": "image",
        "parent": parent,
        "x": x,
        "y": y,
        "w": w,
        "h": h,
        "image": image_path,
        "codec": "raw",
        "fit": fit,
        "dynamic_image": True,
        "name": name,
    }
    if bind:
        obj["bind"] = bind
    if callback:
        obj["callback"] = callback
    if scalable:
        obj.update({
            "scalable": True,
            "scale": 1.0,
            "min_scale": 1.0,
            "max_scale": 4.0,
            "position_x": 0.5,
            "position_y": 0.5,
        })
    return obj


def dynamic_color_container(parent, x, y, w, h, *, name, bg, radius=0, opacity=None):
    obj = container(parent, x, y, w, h, bg=bg, radius=radius, name=name, opacity=opacity)
    obj["dynamic_color"] = True
    return obj


def visible_button(parent, x, y, w, h, text, *, name, bind, bg, fg, radius, size, callback=None, hidden=False):
    obj = button(parent, x, y, w, h, text, bg=bg, fg=fg, radius=radius, size=size, name=name, callback=callback)
    obj["bind"] = bind
    obj["bind_target"] = "visible"
    if hidden:
        obj["hidden"] = True
    return obj


def toolbar_normal(objs, root):
    bar = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, 64, name="album_toolbar_normal", bind="album_toolbar_normal_visible"))
    objs.append(container(bar, 16, 21, 22, 22, bg="#FF4C01", radius=4,
                          name="album_app_marker"))
    objs.append(label(bar, 52, 15, 130, 34, "Album", size=24,
                      color="#D6D6DE", name="album_title"))
    objs.append(button(bar, 376, 12, 88, 40, "Select", bg="#181819", fg="#FCFCFF", radius=20, size=14, callback="album_select"))


def toolbar_select(objs, root):
    bar = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, 64, hidden=True, name="album_toolbar_select", bind="album_toolbar_select_visible"))
    objs.append(button(bar, 16, 12, 72, 40, "Cancel", bg="#181819", fg="#D6D6DE", radius=20, size=14, callback="album_cancel_select"))
    objs.append(label(bar, 152, 17, 176, 30, "0 selected", size=18, color="#FCFCFF", align="center", name="album_select_count", bind="album_select_count"))
    objs.append(visible_button(bar, 392, 12, 72, 40, "Delete", name="album_select_delete_enabled",
                               bind="album_select_delete_enabled_visible", bg="#FF4C01", fg="#FCFCFF",
                               radius=20, size=14, callback="album_delete", hidden=True))


def grid(objs, root):
    objs.append({
        "type": "grid",
        "parent": root,
        "name": "album",
        "x": GRID_X,
        "y": GRID_Y,
        "w": GRID_W,
        "h": GRID_H,
        "column_count": COLUMNS,
        "column_gap": GAP,
        "row_gap": GAP,
        "item_count": 1,
        "cell_template": "album_cell",
    })
    empty = len(objs)
    objs.append(layer(root, 80, 228, 320, 32, hidden=True, name="album_empty", bind="album_empty_visible"))
    objs.append(label(empty, 0, 0, 320, 32, "No JPEG Photos", size=20, color="#595959", align="center"))


def cell_template(objs):
    cell = len(objs)
    objs.append(container(-1, 0, 0, TILE, TILE, bg="#181819", radius=6, name="album_cell_body", callback="album_row"))
    objs[-1]["template"] = "album_cell"
    objs.append(dynamic_image(cell, 0, 0, TILE, TILE, name="photo", bind=None))
    marker_opacity = {"default": 0, "min": 0, "max": 255}
    objs.append(dynamic_color_container(cell, 124, 8, 24, 24, name="select_circle", bg="#181819", radius=12, opacity=marker_opacity))


def fullscreen(objs, root):
    full = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H, hidden=True, name="album_fullscreen", bind="album_fullscreen_visible", block_scene_swipe=True))
    objs.append(container(full, 0, 0, CONTENT_W, CONTENT_H, bg="#000000", name="album_fullscreen_bg"))
    objs.append(dynamic_image(full, 0, 0, CONTENT_W, CONTENT_H, name="album_full_image", bind="album_full_image",
                              fit="contain", scalable=True, callback="album_fullscreen_tap"))
    objs.append(button(full, 0, 190, 64, 100, "<", bg="#000000", fg="#FCFCFF", radius=0, size=40, opacity=128, callback="album_prev"))
    objs.append(button(full, 416, 190, 64, 100, ">", bg="#000000", fg="#FCFCFF", radius=0, size=40, opacity=128, callback="album_next"))


def modal(objs, root):
    shade = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H, hidden=True, name="album_modal", bind="album_modal_visible", block_scene_swipe=True))
    objs.append(container(shade, 0, 0, CONTENT_W, CONTENT_H, bg="#000000", opacity=188, name="album_modal_shade"))
    card = len(objs)
    objs.append(container(shade, 44, 104, 392, 272, bg="#181819", radius=8, name="album_modal_card"))
    objs.append(label(card, 24, 22, 344, 30, "Details", size=24, color="#FCFCFF", name="album_modal_title", bind="album_modal_title"))
    objs.append(label(card, 24, 66, 344, 24, "file:", size=16, color="#91919B", name="album_detail_name", bind="album_detail_name"))
    objs.append(label(card, 24, 96, 344, 24, "size:", size=16, color="#91919B", name="album_detail_size", bind="album_detail_size"))
    objs.append(button(card, 192, 220, 80, 36, "Cancel", bg="#2A2A2C", fg="#FCFCFF", radius=18, size=14, callback="album_cancel_modal"))
    objs.append(button(card, 284, 220, 84, 36, "Delete", bg="#FF4C01", fg="#FCFCFF", radius=18, size=14, callback="album_confirm_delete"))


def album_asset_scene(path):
    # Keep the assets scene as a resource superset of album_480.json; scalable fullscreen fallback requires a raw copy.
    asset_objects = [container(-1, 0, 0, CONTENT_W, CONTENT_H)]
    asset_objects.append(image(0, PLACEHOLDER, 0, 0, 456, 340))
    asset_objects.append({
        "type": "image",
        "parent": 0,
        "x": 0,
        "y": 120,
        "w": 456,
        "h": 340,
        "image": PLACEHOLDER,
        "codec": "raw",
    })
    write_scene(path, "album_assets", asset_objects, font=FONT)


def main():
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD)
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H, name="album_root"))
    objs.append(container(root, 0, 0, CONTENT_W, CONTENT_H, bg="#000000", name="album_bg"))
    toolbar_normal(objs, root)
    toolbar_select(objs, root)
    grid(objs, root)
    cell_template(objs)
    fullscreen(objs, root)
    modal(objs, root)
    write_scene(scene_out_path(HERE, "album_480.json"), "album", objs, font=FONT)
    album_asset_scene(scene_out_path(HERE, "album_assets_480.json"))


if __name__ == "__main__":
    main()
