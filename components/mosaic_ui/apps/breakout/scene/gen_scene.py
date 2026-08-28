#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Scene generator for the Breakout demo App.

Static structure only: HUD, a brick wall (each brick toggled by a visibility
bind), a movable paddle and ball (bounded x/y geometry moved at runtime with
esp_gsp_component_set_position), and a start/end overlay. All motion is driven
by breakout_app.c; the scene never rebuilds.

Geometry constants here MUST match BRICK_* / PADDLE_* / BALL_* in
breakout_app.c so collision math lines up with what is drawn.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H,
    CONTENT_W,
    container,
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
    18: shared_charset(),
    20: shared_charset(),
    34: shared_charset(),
}
CHARSET = shared_charset()["charset"]

BG = "#0E1420"
INK = "#FCFCFF"
MUTED = "#8A93A6"
PADDLE = "#F2F5F7"
BALL = "#FFD166"
ROW_COLORS = ["#EF6461", "#E4B363", "#7FB685", "#4F9DDE"]

# Brick wall geometry (keep in sync with breakout_app.c).
ROWS = 4
COLS = 5
BRICK_W = 84
BRICK_H = 24
BRICK_GAP = 6
BRICK_LEFT = 18
BRICK_TOP = 104

PADDLE_W = 96
PADDLE_H = 14
PADDLE_Y = 408
BALL_D = 16
FIELD_TOP = 64


def main() -> None:
    objs, content = shared_prefix([], FONT_POLICIES, BOLD)

    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg=BG))

    # Keep the first-level App title in the shared Shell, exactly like Works.
    # Game-specific status gets its own row below that 64 px header.
    objs.append(label(
        content, 18, 70, 216, 26, "SCORE 0", size=20, color=INK,
        align="left", bind="game_score", name="game_score",
        font_charset=CHARSET))
    objs.append(label(
        content, 246, 70, 216, 26, "LIVES 3", size=20, color=INK,
        align="right", bind="game_lives", name="game_lives",
        font_charset=CHARSET))

    for r in range(ROWS):
        for c in range(COLS):
            x = BRICK_LEFT + c * (BRICK_W + BRICK_GAP)
            y = BRICK_TOP + r * (BRICK_H + BRICK_GAP)
            brick = container(content, x, y, BRICK_W, BRICK_H,
                              bg=ROW_COLORS[r], radius=6,
                              bind=f"brick_{r}_{c}_visible")
            brick["bind_target"] = "visible"
            objs.append(brick)

    paddle_max = CONTENT_W - BRICK_LEFT - PADDLE_W
    objs.append({
        "type": "container", "parent": content, "name": "paddle",
        "x": {"default": (CONTENT_W - PADDLE_W) // 2,
              "min": BRICK_LEFT, "max": paddle_max},
        # Y is bounded (not scalar) so the component reserves a Y property.
        # esp_gsp_component_set_position writes X and Y together; without a Y
        # property that batched update is rejected and the paddle never moves.
        # The App always commands PADDLE_Y, so the paddle stays on its row.
        "y": {"default": PADDLE_Y, "min": FIELD_TOP,
              "max": CONTENT_H - PADDLE_H},
        "w": PADDLE_W, "h": PADDLE_H,
        "bg_color": PADDLE, "radius": 7,
    })

    objs.append({
        "type": "container", "parent": content, "name": "ball",
        "x": {"default": (CONTENT_W - BALL_D) // 2,
              "min": 4, "max": CONTENT_W - BALL_D - 4},
        "y": {"default": PADDLE_Y - BALL_D,
              "min": FIELD_TOP, "max": CONTENT_H - BALL_D},
        "w": BALL_D, "h": BALL_D, "bg_color": BALL, "radius": BALL_D // 2,
    })

    overlay = len(objs)
    ov = layer(content, 0, FIELD_TOP, CONTENT_W, CONTENT_H - FIELD_TOP,
               name="game_overlay", bind="game_overlay_visible")
    ov["bind_target"] = "visible"
    objs.append(ov)
    panel = container(overlay, 50, 150, 380, 150, bg="#141C2B", radius=20,
                      opacity=235, border="#2A3550", border_w=1)
    objs.append(panel)
    objs.append(label(
        overlay, 50, 182, 380, 44, "TAP TO START", size=34, color=INK,
        align="center", bind="game_message", name="game_message",
        font_charset=CHARSET))
    objs.append(label(
        overlay, 50, 240, 380, 24, "Drag to move the paddle", size=18,
        color=MUTED, align="center", bind="game_hint", name="game_hint",
        font_charset=CHARSET))

    write_scene(scene_out_path(HERE, "breakout_480.json"),
                "breakout", objs, font=FONT)


if __name__ == "__main__":
    main()
