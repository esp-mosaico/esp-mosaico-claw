#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Full retained scene for the HTML Sky Shooter game contract."""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

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
    write_scene,
)

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def packed_charset(*parts: str) -> str:
    return "".join(sorted(set("".join(parts))))


CHARSET_14 = packed_charset(
    "SCORE BEST LV STRAIGHT MAX COMBO",
    "♪ ON OFF",
    "0123456789",
)
CHARSET_16 = packed_charset(
    "COMBO 0123456789",
    "TURNER · CHANGES COURSE",
    "STRIKER · SPEEDS UP",
    "GUNNER · DODGE FIRE DUAL TRIPLE",
    "Drag to dodge · Tilt to aim",
    "TAP TO RETRY",
    "LV  · SCORE ",
)
CHARSET_18 = packed_charset(
    "!+0123456789",
    "NEW BEST TAP TO RESUME",
)
CHARSET_24 = packed_charset("‹Ⅱ▶×LEVEL 0123456789")
CHARSET_31 = packed_charset("TAP TO START PAUSED NEW BEST GAME OVER")
CHARSET_42 = packed_charset("0123456789")
CHARSET_BY_SIZE = {
    14: CHARSET_14,
    16: CHARSET_16,
    18: CHARSET_18,
    24: CHARSET_24,
    31: CHARSET_31,
    42: CHARSET_42,
}
FONT_POLICIES = {
    14: explicit_charset(CHARSET_14),
    16: explicit_charset(CHARSET_16),
    18: explicit_charset(CHARSET_18),
    24: explicit_charset(CHARSET_24),
    31: explicit_charset(CHARSET_31),
    42: explicit_charset(CHARSET_42),
}

BG = "#000000"
INK = "#FCFCFF"
MUTED = "#91919B"
DIM = "#3B3C3D"
KEY = "#181819"
ACCENT = "#FF4C01"
WARN = "#FFD166"
RED = "#EF4444"
RAIL_RED = "#9D241E"

PLAY_TOP = 92
PLAYER_W = 48
PLAYER_H = 60
PLAYER_X0 = 216
PLAYER_Y0 = 372
PLAYER_MIN_X = 12
PLAYER_MAX_X = 468
PLAYER_MIN_Y = 100
PLAYER_MAX_Y = 468
BULLET_W = 4
BULLET_H = 12
BULLET_MAX = 8
ENEMY_W = 48
ENEMY_H = 44
ENEMY_MAX = 7
ENEMY_KINDS = 4
ENEMY_BULLET_W = 8
ENEMY_BULLET_H = 14
ENEMY_BULLET_MAX = 12
BOOM_W = 48
BOOM_H = 48
BOOM_MAX = 4
BOOM_FRAMES = 3
SCORE_POP_MAX = 6
STAR_MAX = 25
ASSETS = "../assets"
ENEMY_SRC = tuple(f"{ASSETS}/enemy_{letter}.png" for letter in "abcd")


def visibility(obj, bind, *, hidden=False):
    obj["bind"] = bind
    obj["bind_target"] = "visible"
    if hidden:
        obj["hidden"] = True
    return obj


def bounded(parent, *, obj_type="container", name, x, y, w, h,
            xmin, xmax, ymin, ymax, **values):
    obj = {
        "type": obj_type,
        "parent": parent,
        "name": name,
        "x": {"default": x, "min": xmin, "max": xmax},
        "y": {"default": y, "min": ymin, "max": ymax},
        "w": w,
        "h": h,
        **values,
    }
    return obj


def bounded_image(parent, src, *, name, x, y, w, h, bind, hidden=False):
    obj = bounded(
        parent, obj_type="image", name=name, x=x, y=y, w=w, h=h,
        xmin=-32, xmax=CONTENT_W + 32, ymin=PLAY_TOP - 48,
        ymax=CONTENT_H + 32, image=src,
    )
    return visibility(obj, bind, hidden=hidden)


def add_stars(objs, parent):
    for index in range(STAR_MAX):
        size = 2 if index % 5 == 0 else 1
        objs.append(bounded(
            parent, name=f"star_{index}",
            x=12 + index * 83 % 456,
            y=PLAY_TOP + index * 137 % (CONTENT_H - PLAY_TOP),
            w=size, h=size, xmin=4, xmax=CONTENT_W - 4,
            ymin=PLAY_TOP - 32, ymax=CONTENT_H + 4,
            bg_color=INK, opacity=225 if size == 2 else 165,
        ))


def hud_button(parent, x, text, callback, name):
    obj = button(
        parent, x, 8, 48, 48, text, bg=KEY, fg=ACCENT, radius=10,
        size=24, callback=callback, name=name,
        border=DIM, border_w=1, align="center",
    )
    obj["font_charset"] = CHARSET_BY_SIZE[24]
    return obj


def add_rail(objs, parent, side):
    rail_x = 17 if side == "left" else 453
    for module, (top, height) in enumerate(((98, 52), (234, 72), (396, 58))):
        rail = len(objs)
        prefix = f"rail_{side}_{module}"
        item = bounded(
            parent, obj_type="layer", name=prefix,
            x=rail_x, y=top, w=10, h=height,
            xmin=rail_x - 1, xmax=rail_x + 1,
            ymin=top - 8, ymax=top + 8,
        )
        objs.append(item)
        vertical_x = 0 if side == "left" else 8
        node_x = 0 if side == "left" else 5
        vertical = container(
            rail, vertical_x, 0, 2, height, bg=DIM,
            name=f"{prefix}_vertical",
        )
        vertical["bind"] = f"{prefix}_vertical_color"
        vertical["bind_target"] = "color"
        objs.append(vertical)
        top_line = container(
            rail, 0, 0, 9, 1, bg=DIM, name=f"{prefix}_top",
        )
        top_line["bind"] = f"{prefix}_top_color"
        top_line["bind_target"] = "color"
        objs.append(top_line)
        bottom_line = container(
            rail, 0, height - 1, 9, 1, bg=DIM,
            name=f"{prefix}_bottom",
        )
        bottom_line["bind"] = f"{prefix}_bottom_color"
        bottom_line["bind_target"] = "color"
        objs.append(bottom_line)
        node = container(
            rail, node_x, height // 2 - 2, 5, 5,
            bg=ACCENT, radius=1, border=BG, border_w=1,
            name=f"{prefix}_node",
        )
        node["bind"] = f"{prefix}_node_color"
        node["bind_target"] = "color"
        objs.append(node)
        if module == 1:
            for segment in range(7):
                dash = container(
                    rail, vertical_x, 2 + segment * 10, 2, 5,
                    bg=RAIL_RED,
                    name=f"rail_{side}_mid_dash_{segment}",
                    hidden=True,
                )
                dash["bind"] = (
                    f"rail_{side}_mid_dash_{segment}_visible"
                )
                dash["bind_target"] = "visible"
                objs.append(dash)


def add_hud_divider(objs, parent):
    objs.append(container(parent, 12, 63, 132, 1, bg=DIM))
    for step in range(22):
        objs.append(container(
            parent, 143 + step * 29 // 21, 63 + step, 2, 1, bg=DIM,
        ))
    objs.append(container(parent, 172, 84, 136, 2, bg=KEY))
    for step in range(22):
        objs.append(container(
            parent, 308 + step * 29 // 21, 84 - step, 2, 1, bg=DIM,
        ))
    objs.append(container(parent, 337, 63, 131, 1, bg=DIM))


def aim_guide(objs, parent):
    guide = len(objs)
    obj = bounded(
        parent, obj_type="layer", name="aim_guide",
        x=PLAYER_X0 + PLAYER_W // 2 - 1, y=PLAYER_Y0 - 82,
        w=3, h=84, xmin=-24, xmax=CONTENT_W,
        ymin=PLAYER_MIN_Y - 82, ymax=CONTENT_H,
    )
    visibility(obj, "aim_guide_visible", hidden=True)
    objs.append(obj)
    for segment in range(6):
        objs.append(container(
            guide, 0, segment * 14, 3, 7,
            bg=ACCENT, radius=1, opacity=150,
        ))


def main() -> None:
    images = [
        (f"{ASSETS}/player.png", PLAYER_W, PLAYER_H),
        *[(src, ENEMY_W, ENEMY_H) for src in ENEMY_SRC],
        (f"{ASSETS}/exhaust_0.png", 16, 24),
        (f"{ASSETS}/exhaust_1.png", 16, 24),
        *[(f"{ASSETS}/boom_{frame}.png", BOOM_W, BOOM_H)
          for frame in range(BOOM_FRAMES)],
    ]
    objs, content = shared_prefix(images, FONT_POLICIES, BOLD)
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg=BG))

    # Decorative telemetry rails sit behind every gameplay object.
    add_rail(objs, content, "left")
    add_rail(objs, content, "right")

    add_stars(objs, content)

    for index in range(BULLET_MAX):
        obj = bounded(
            content, name=f"bullet_{index}", x=238, y=350,
            w=BULLET_W, h=BULLET_H, xmin=-24, xmax=CONTENT_W + 24,
            ymin=PLAY_TOP - 24, ymax=CONTENT_H,
            bg_color=WARN, radius=2,
        )
        objs.append(visibility(obj, f"bullet_{index}_visible", hidden=True))

    for index in range(ENEMY_BULLET_MAX):
        shot = len(objs)
        obj = bounded(
            content, obj_type="layer", name=f"enemy_bullet_{index}",
            x=238, y=PLAY_TOP, w=ENEMY_BULLET_W, h=ENEMY_BULLET_H,
            xmin=-24, xmax=CONTENT_W + 24, ymin=PLAY_TOP - 24,
            ymax=CONTENT_H + 24,
        )
        objs.append(visibility(
            obj, f"enemy_bullet_{index}_visible", hidden=True))
        objs.append(container(shot, 1, 1, 6, 12, bg=RED, radius=3))
        objs.append(container(shot, 3, 4, 2, 6, bg=INK, radius=1))

    for index in range(ENEMY_MAX):
        for kind, src in enumerate(ENEMY_SRC):
            objs.append(bounded_image(
                content, src, name=f"enemy_{index}_{kind}",
                x=PLAYER_MIN_X + index * 48, y=PLAY_TOP,
                w=ENEMY_W, h=ENEMY_H,
                bind=f"enemy_{index}_{kind}_visible", hidden=True,
            ))
        warning = bounded(
            content, obj_type="label", name=f"enemy_warning_{index}",
            x=PLAYER_MIN_X + index * 48 + 13, y=PLAY_TOP + ENEMY_H,
            w=22, h=20, xmin=-24, xmax=CONTENT_W,
            ymin=PLAY_TOP - 24, ymax=CONTENT_H,
            text="!", font_size=18, fg_color=WARN,
            text_align="center", font_charset=CHARSET_18,
        )
        objs.append(visibility(
            warning, f"enemy_warning_{index}_visible", hidden=True))

    objs.append(bounded_image(
        content, f"{ASSETS}/exhaust_0.png", name="exhaust_0",
        x=232, y=428, w=16, h=24,
        bind="exhaust_0_visible",
    ))
    objs.append(bounded_image(
        content, f"{ASSETS}/exhaust_1.png", name="exhaust_1",
        x=232, y=428, w=16, h=24,
        bind="exhaust_1_visible", hidden=True,
    ))
    objs.append(bounded_image(
        content, f"{ASSETS}/player.png", name="player",
        x=PLAYER_X0, y=PLAYER_Y0, w=PLAYER_W, h=PLAYER_H,
        bind="player_visible",
    ))

    aim_guide(objs, content)

    for index in range(BOOM_MAX):
        for frame in range(BOOM_FRAMES):
            objs.append(bounded_image(
                content, f"{ASSETS}/boom_{frame}.png",
                name=f"boom_{index}_{frame}", x=216, y=240,
                w=BOOM_W, h=BOOM_H,
                bind=f"boom_{index}_{frame}_visible", hidden=True,
            ))

    for index in range(SCORE_POP_MAX):
        pop = len(objs)
        pop_layer = bounded(
            content, obj_type="layer", name=f"score_pop_{index}",
            x=180, y=220, w=120, h=24, xmin=-40,
            xmax=CONTENT_W - 40, ymin=PLAY_TOP, ymax=CONTENT_H,
        )
        objs.append(visibility(
            pop_layer, f"score_pop_{index}_visible", hidden=True))
        objs.append(label(
            pop, 0, 0, 120, 24, "+10", size=18, color=WARN,
            align="center", bind=f"score_pop_{index}_text",
            name=f"score_pop_{index}_text", font_charset=CHARSET_18,
        ))

    # Opaque HUD: same 64 px score marquee + 24 px instrument shelf as HTML.
    hud = len(objs)
    objs.append(container(content, 0, 0, CONTENT_W, 88, bg=BG))
    add_hud_divider(objs, hud)
    objs.append(hud_button(hud, 8, "‹", "game_exit", "game_exit"))
    pause = hud_button(hud, 424, "Ⅱ", "game_pause", "game_pause")
    pause["bind"] = "game_pause"
    objs.append(pause)
    objs.append(label(
        hud, 88, 1, 304, 14, "SCORE", size=14, color=ACCENT,
        align="center", font_charset=CHARSET_14,
    ))
    objs.append(label(
        hud, 88, 12, 304, 40, "0000", size=42, color=INK,
        align="center", bind="game_score", name="game_score",
        font_charset=CHARSET_42,
    ))
    for index in range(3):
        life = image(
            hud, f"{ASSETS}/player.png", 221 + index * 17, 50, 12, 15,
            name=f"life_{index}", bind=f"life_{index}_visible",
        )
        if index >= 0:
            life["bind_target"] = "visible"
        objs.append(life)
    objs.append(label(
        hud, 18, 66, 70, 18, "BEST", size=14, color=ACCENT,
        font_charset=CHARSET_14,
    ))
    objs.append(label(
        hud, 62, 66, 92, 18, "0000", size=14, color=INK,
        bind="game_best", name="game_best", font_charset=CHARSET_14,
    ))
    objs.append(label(
        hud, 421, 66, 20, 18, "LV", size=14, color=ACCENT,
        align="right", font_charset=CHARSET_14,
    ))
    objs.append(label(
        hud, 443, 66, 21, 18, "01", size=14, color=INK,
        align="right", bind="game_level", name="game_level",
        font_charset=CHARSET_14,
    ))
    audio = button(
        hud, 268, 55, 48, 33, "♪ ON", bg=BG, fg=ACCENT, radius=8,
        size=14, callback="game_audio", name="game_audio",
        align="center",
    )
    audio["bind"] = "game_audio"
    audio["font_charset"] = CHARSET_14
    objs.append(audio)
    objs.append(label(
        hud, 318, 61, 94, 24, "STRAIGHT", size=14, color=MUTED,
        align="right",
        font_charset=CHARSET_14,
    ))
    objs.append({
        "type": "progress", "parent": hud,
        "x": 172, "y": 84, "w": 136, "h": 2,
        "value": 0, "min": 0, "max": 100,
        "bind": "level_progress", "name": "level_progress",
        "bg_color": KEY, "fg_color": ACCENT, "radius": 0,
    })

    combo = len(objs)
    combo_layer = layer(
        content, 30, 105, 176, 42, hidden=True,
        name="combo_panel", bind="combo_panel_visible",
    )
    combo_layer["bind_target"] = "visible"
    objs.append(combo_layer)
    objs.append(label(
        combo, 0, 0, 116, 26, "COMBO 2", size=16, color=INK,
        bind="combo_count", name="combo_count", font_charset=CHARSET_16,
    ))
    objs.append(label(
        combo, 118, 0, 58, 26, "×1", size=24, color=WARN,
        bind="combo_multiplier", name="combo_multiplier",
        font_charset=CHARSET_24,
    ))
    objs.append({
        "type": "progress", "parent": combo,
        "x": 0, "y": 30, "w": 136, "h": 2,
        "value": 100, "min": 0, "max": 100,
        "bind": "combo_progress", "name": "combo_progress",
        "bg_color": KEY, "fg_color": WARN, "radius": 0,
    })

    notice = len(objs)
    notice_layer = layer(
        content, 90, 184, 300, 68, hidden=True,
        name="level_notice", bind="level_notice_visible",
    )
    notice_layer["bind_target"] = "visible"
    objs.append(notice_layer)
    objs.append(container(notice, 0, 0, 300, 68, bg=BG, opacity=230))
    objs.append(container(notice, 0, 0, 300, 1, bg=DIM))
    objs.append(container(notice, 0, 67, 300, 1, bg=DIM))
    objs.append(label(
        notice, 0, 4, 300, 32, "LEVEL 02", size=24, color=INK,
        align="center", bind="level_notice_title",
        name="level_notice_title", font_charset=CHARSET_24,
    ))
    objs.append(label(
        notice, 0, 36, 300, 25, "", size=16, color=WARN,
        align="center", bind="level_notice_unlock",
        name="level_notice_unlock", font_charset=CHARSET_16,
    ))

    best = label(
        content, 142, 112, 196, 36, "NEW BEST", size=18,
        color=WARN, align="center", bind="new_best_visible",
        name="new_best", font_charset=CHARSET_18,
    )
    best["bind_target"] = "visible"
    best["hidden"] = True
    objs.append(best)
    left_flash = container(
        content, 0, 246, 46, 2, bg=RED,
        bind="hit_flash_visible", hidden=True,
    )
    left_flash["bind_target"] = "visible"
    objs.append(left_flash)
    right_flash = container(
        content, 434, 246, 46, 2, bg=RED,
        bind="hit_flash_right_visible", hidden=True,
    )
    right_flash["bind_target"] = "visible"
    objs.append(right_flash)

    overlay = len(objs)
    overlay_layer = layer(
        content, 0, PLAY_TOP, CONTENT_W, CONTENT_H - PLAY_TOP,
        name="game_overlay", bind="game_overlay_visible",
    )
    overlay_layer["bind_target"] = "visible"
    objs.append(overlay_layer)
    panel = len(objs)
    objs.append(container(
        overlay, 44, 70, 392, 252, bg=BG, opacity=235,
        border=DIM, border_w=1,
    ))
    objs.append(label(
        panel, 20, 14, 352, 40, "TAP TO START", size=31,
        color=INK, align="center", bind="game_message",
        name="game_message", font_charset=CHARSET_31,
    ))
    objs.append(label(
        panel, 20, 58, 352, 28, "BEST 0000", size=18,
        color=ACCENT, align="center", bind="game_hint",
        name="game_hint", font_charset=CHARSET_18,
    ))
    objs.append(label(
        panel, 20, 91, 352, 28, "Drag to dodge · Tilt to aim",
        size=16, color=MUTED, align="center", bind="game_subhint",
        name="game_subhint", font_charset=CHARSET_16,
    ))
    results = len(objs)
    results_layer = layer(
        panel, 28, 64, 336, 164, hidden=True,
        name="game_results", bind="game_results_visible",
    )
    results_layer["bind_target"] = "visible"
    objs.append(results_layer)
    for index, title in enumerate(("SCORE", "BEST", "MAX COMBO")):
        y = index * 30
        objs.append(label(results, 0, y, 150, 22, title,
                          size=14, color=ACCENT, font_charset=CHARSET_14))
        objs.append(label(
            results, 160, y, 176, 22, "0", size=18, color=INK,
            align="right", bind=f"result_{title.lower().replace(' ', '_')}",
            name=f"result_{title.lower().replace(' ', '_')}",
            font_charset=CHARSET_18,
        ))
    objs.append(label(
        results, 0, 112, 336, 24, "TAP TO RETRY", size=16,
        color=ACCENT, align="center", font_charset=CHARSET_16,
    ))

    write_scene(
        scene_out_path(HERE, "air_battle_480.json"),
        "air_battle", objs, font=FONT,
    )


if __name__ == "__main__":
    main()
