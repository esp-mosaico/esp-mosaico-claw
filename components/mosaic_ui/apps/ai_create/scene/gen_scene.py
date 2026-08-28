#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Generate the ESP-Claw AI Create application scene."""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS  # noqa: E402
from scene_common import (  # noqa: E402
    ASCII_PRINTABLE,
    CONTENT_H,
    CONTENT_W,
    button,
    container,
    label,
    layer,
    scene_out_path,
    shared_prefix,
    shared_charset,
    write_scene,
)

FONT = DEJAVU_SANS
BOLD = FONT
APP_FONT_SIZES = {
    # Only dynamic text sizes need the full glyph seed. Static labels still
    # generate their exact glyphs automatically, without bloating each GFB.
    "ai_create": [16, 17, 18, 22],
    "calc": [18, 72],
    "battery": [22],
    "stopwatch": [20, 84],
    "timer": [20, 84],
    "music": [20, 28],
    "world_clock": [24],
    "settings": [],
}
GLYPH_SEED = (
    "0123456789:.-+*/=%()<> "
    "CALCBATTERYSTOPWATCHTIMERMUSICWORLDCLOCKSETTINGS"
    "ClearStartPauseResetLapRemainingChargingHealthy"
    "ShanghaiTokyoLondonNewYorkVolumeDisplaySoundNetwork"
    "BluetoothNotificationsPrivacySystemAboutAirplaneMode"
    "MerryChristmasRyuichiSakamotoNightDriveMosaico"
    "CloudAtlasESPLabERRORDIVIDEBYZEROCountingdownComplete"
    "ChargedFull"
)
AI_FONT = DEJAVU_SANS
SESSION_PAGE_SIZE = 3
SESSION_MAX_PAGES = 11
AI_GLYPH_SEED = ASCII_PRINTABLE + "…·→‹›"


def font_policies(name, charset=GLYPH_SEED):
    return {
        size: shared_charset(charset)
        for size in APP_FONT_SIZES[name]
    }


def back_key(objs, parent, x, y, callback, *, name=None):
    """First-level App marker; physical Back owns navigation."""
    del callback
    objs.append(container(parent, x + 4, y + 17, 22, 22,
                          bg="#FF4C01", radius=4, name=name))


def app_root(name, title):
    objs, content = shared_prefix(
        [], font_policies(name), BOLD,
    )
    page = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      name=f"{name}_root"))
    # The 52 px hit target remains finger-sized, while the visual is the
    # unboxed back glyph used throughout the interaction specification.
    back_key(objs, page, 12, 9, "app_back", name=f"back_{name}")
    objs.append(label(page, 72, 18, 384, 38, title, size=30,
                      color="#D6D6DE"))
    return objs, page


def action_button(objs, parent, x, y, w, h, text, action, *,
                  bg="#181819", fg="#FCFCFF", size=24, radius=24):
    objs.append(button(
        parent, x, y, w, h, text, bg=bg, fg=fg, size=size, radius=radius,
        callback=action, name=action,
    ))


def voice_icon(objs, parent, x, y, *, color="#91919B"):
    """Five-bar voice glyph matching the reference input affordance."""
    heights = (10, 20, 28, 20, 10)
    for index, height in enumerate(heights):
        objs.append(container(
            parent, x + index * 5, y + (28 - height) // 2,
            2, height, bg=color, radius=1,
        ))


def build_calc():
    objs, content = shared_prefix(
        [], font_policies("calc"), BOLD,
    )
    page = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      name="calc_root"))
    # Calculator is intentionally immersive: the back affordance lives in
    # the readout band instead of consuming a separate title row.
    back_key(objs, page, 12, 12, "app_back", name="back_calc")
    objs.append(label(
        page, 82, 14, 366, 24, "", size=18, color="#91919B",
        align="right", bind="calc_expression", name="calc_expression",
    ))
    objs.append(label(
        page, 82, 36, 366, 60, "0", size=72, align="right",
        bind="calc_display", name="calc_display",
    ))
    keys = (
        ("7", "calc_7"), ("8", "calc_8"), ("9", "calc_9"),
        ("/", "calc_div"),
        ("4", "calc_4"), ("5", "calc_5"), ("6", "calc_6"),
        ("*", "calc_mul"),
        ("1", "calc_1"), ("2", "calc_2"), ("3", "calc_3"),
        ("-", "calc_sub"),
        ("C", "calc_clear"), ("0", "calc_0"), ("=", "calc_equal"),
        ("+", "calc_add"),
    )
    for index, (text, action) in enumerate(keys):
        row, col = divmod(index, 4)
        operator = col == 3 or text == "="
        action_button(
            objs, page, 12 + col * 116, 108 + row * 86, 108, 78,
            text, action,
            bg="#FF4C01" if operator else "#242424",
            fg="#FCFCFF", size=32, radius=22,
        )
    return objs


def build_battery():
    objs, page = app_root("battery", "BATTERY")
    rows = (
        ("CURRENT LEVEL", "battery_level", "82", "%"),
        ("STATUS", "battery_state", "Charging", ""),
        ("ESTIMATED USE", "battery_remaining", "6 h 20 m", ""),
        ("FULL CHARGE", None, "2.5 h", ""),
        ("HEALTH", "battery_health", "Healthy", ""),
    )
    for index, (title, bind, value, unit) in enumerate(rows):
        y = 78 + index * 76
        objs.append(container(page, 12, y, 456, 68, bg="#181819",
                              radius=20))
        objs.append(label(page, 36, y + 22, 238, 26, title, size=18,
                          color="#D6D6DE"))
        value_color = "#FF4C01" if title == "STATUS" else "#FCFCFF"
        value_x = 294 if unit else 272
        objs.append(label(
            page, value_x, y + 19, 154, 30, value, size=22,
            color=value_color, align="right", bind=bind, name=bind,
        ))
        if unit:
            objs.append(label(page, 450, y + 22, 18, 24, unit, size=18,
                              color="#91919B", align="right"))
    # Keep the numeric progress binding alive without introducing a second
    # competing visualization.
    objs.append({
        "type": "progress", "parent": page,
        "x": 36, "y": 136, "w": 412, "h": 4,
        "value": 82, "min": 0, "max": 100,
        "bind": "battery_progress", "name": "battery_progress",
        "bg_color": "#3B3C3D", "fg_color": "#FF4C01", "radius": 2,
    })
    return objs


def build_stopwatch():
    objs, page = app_root("stopwatch", "STOPWATCH")
    objs.append(label(
        page, 12, 108, 456, 98, "00:00.0", size=84, align="center",
        bind="sw_time", name="sw_time",
    ))
    objs.append(label(page, 12, 218, 456, 28, "READY · 1/10 SECOND",
                      size=18, color="#91919B", align="center"))
    objs.append(label(page, 24, 254, 160, 24, "LAPS", size=16,
                      color="#91919B"))
    for index in range(3):
        y = 280 + index * 27
        objs.append(label(page, 24, y, 70, 30, f"{index + 1:02d}",
                          size=18, color="#595959"))
        objs.append(label(
            page, 112, y, 330, 30, "--:--.-", size=20, align="right",
            color="#D6D6DE",
            bind=f"sw_lap{index + 1}", name=f"sw_lap{index + 1}",
        ))
    action_button(objs, page, 12, 376, 144, 88, "RESET", "sw_reset",
                  size=20, radius=28)
    action_button(objs, page, 168, 376, 144, 88, "LAP", "sw_lap",
                  size=20, radius=28)
    action_button(objs, page, 324, 376, 144, 88, "", "sw_toggle",
                  bg="#FF4C01", size=20, radius=28)
    objs.append(label(
        page, 324, 405, 144, 30, "START", size=20, align="center",
        bind="sw_toggle_label", name="sw_toggle_label",
    ))
    return objs


def build_timer():
    objs, page = app_root("timer", "TIMER")
    objs.append(label(
        page, 12, 108, 456, 104, "05:00", size=84, align="center",
        bind="timer_time", name="timer_time",
    ))
    objs.append(label(page, 12, 238, 456, 28,
                      "HOLD TO ADJUST · VIBRATES WHEN DONE", size=18,
                      color="#91919B", align="center"))
    objs.append({
        "type": "progress", "parent": page,
        "x": 36, "y": 294, "w": 408, "h": 8,
        "value": 100, "min": 0, "max": 100,
        "bind": "timer_progress", "name": "timer_progress",
        "bg_color": "#242424", "fg_color": "#FF4C01", "radius": 4,
    })
    objs.append(label(
        page, 36, 320, 408, 30, "Ready", size=20,
        color="#91919B", align="center",
        bind="timer_state", name="timer_state",
    ))
    action_button(objs, page, 12, 376, 144, 88, "+1 MIN", "timer_add",
                  size=20, radius=28)
    action_button(objs, page, 168, 376, 144, 88, "RESET", "timer_reset",
                  size=20, radius=28)
    action_button(objs, page, 324, 376, 144, 88, "", "timer_toggle",
                  bg="#FF4C01", size=20, radius=28)
    objs.append(label(
        page, 324, 405, 144, 30, "START", size=20, align="center",
        bind="timer_toggle_label", name="timer_toggle_label",
    ))
    return objs


def build_music():
    objs, page = app_root("music", "MUSIC")
    objs.append(label(page, 26, 108, 428, 24, "NOW PLAYING · 01",
                      size=18, color="#FF4C01"))
    objs.append(label(
        page, 26, 145, 428, 76, "Merry Christmas", size=28,
        bind="music_title", name="music_title",
    ))
    objs.append(label(
        page, 26, 224, 428, 30, "Ryuichi Sakamoto", size=20,
        color="#91919B",
        bind="music_artist", name="music_artist",
    ))
    objs.append({
        "type": "progress", "parent": page,
        "x": 26, "y": 286, "w": 428, "h": 8,
        "value": 32, "min": 0, "max": 100,
        "bind": "music_progress", "name": "music_progress",
        "bg_color": "#242424", "fg_color": "#FF4C01", "radius": 4,
    })
    objs.append(label(page, 26, 308, 120, 24, "02:14", size=18,
                      color="#91919B"))
    objs.append(label(page, 334, 308, 120, 24, "04:37", size=18,
                      color="#91919B", align="right"))
    action_button(objs, page, 12, 368, 144, 96, "PREV", "music_prev",
                  size=20, radius=30)
    action_button(objs, page, 168, 368, 144, 96, "", "music_toggle",
                  bg="#FF4C01", size=20, radius=30)
    objs.append(label(
        page, 168, 401, 144, 30, "PLAY", size=20, align="center",
        bind="music_toggle_label", name="music_toggle_label",
    ))
    action_button(objs, page, 324, 368, 144, 96, "NEXT", "music_next",
                  size=20, radius=30)
    return objs


def build_world_clock():
    objs, page = app_root("world_clock", "WORLD CLOCK")
    cities = (
        ("SHANGHAI", "UTC+8", "wclk_shanghai"),
        ("TOKYO", "UTC+9", "wclk_tokyo"),
        ("LONDON", "UTC+0", "wclk_london"),
        ("NEW YORK", "UTC-4", "wclk_new_york"),
    )
    for index, (city, zone, bind) in enumerate(cities):
        y = 78 + index * 76
        objs.append(container(page, 12, y, 456, 68, bg="#181819",
                              radius=20))
        objs.append(label(page, 36, y + 13, 210, 28, city, size=20))
        objs.append(label(page, 36, y + 41, 110, 20, zone, size=16,
                          color="#91919B"))
        objs.append(label(
            page, 268, y + 18, 180, 34, "--:--", size=24, align="right",
            bind=bind, name=bind,
        ))
    return objs


def build_settings():
    objs, page = app_root("settings", "SETTINGS")
    rows = (
        ("DISPLAY & SOUND", "settings_display", True),
        ("NETWORK", "settings_network", True),
        ("BLUETOOTH", "settings_bluetooth", True),
        ("NOTIFICATIONS", "settings_notifications", True),
        ("AIRPLANE MODE", "settings_airplane", False),
    )
    for index, (title, name, checked) in enumerate(rows):
        y = 78 + index * 76
        objs.append(container(page, 12, y, 456, 68, bg="#181819",
                              radius=20))
        objs.append(label(page, 36, y + 21, 288, 28, title, size=18))
        objs.append({
            "type": "toggle", "parent": page,
            "x": 374, "y": y + 12, "w": 74, "h": 44,
            "checked": checked, "bg_color": "#3B3C3D",
            "fg_color": "#F9FAFB", "knob_color": "#FF4C01",
            "radius": 22, "name": name,
            "callback": "settings_toggle_" + name.removeprefix("settings_"),
        })
    objs.append(label(page, 24, 462, 432, 16,
                      "MOSAICO SYSTEM · 1.0", size=15,
                      color="#595959", align="center"))
    return objs


def build_ai_create():
    """AI Create as one retained page with local welcome/chat states."""
    objs, content = shared_prefix(
        [], font_policies("ai_create", AI_GLYPH_SEED), AI_FONT,
    )
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      name="ai_create_root"))

    welcome = len(objs)
    objs.append(layer(
        root, 0, 0, CONTENT_W, CONTENT_H,
        name="ai_welcome",
    ))
    objs[-1]["bind"] = "ai_welcome_visible"

    # This is the single retained welcome-page chrome. Composer states below
    # are sibling visibility groups, so the hero and quick actions never
    # leave, re-enter or get duplicated.
    hero = len(objs)
    objs.append(container(welcome, 24, 24, 432, 142,
                          bg="#E64501", radius=18))
    objs.append(label(hero, 20, 58, 214, 28, "Hello,", size=20))
    objs.append(label(hero, 20, 88, 240, 28,
                      "How can I help?", size=20))
    dot_spans = ((5, 8), (4, 8), (3, 8), (2, 8),
                 (1, 8), (0, 8), (0, 8), (1, 8))
    for row, (start, stop) in enumerate(dot_spans):
        for col in range(start, stop + 1):
            objs.append(container(
                hero, 256 + col * 12, 16 + row * 13,
                6, 6, bg="#FF672A", radius=3,
            ))

    # The picker immediately covers this entry row, so its default selection
    # colors can remain static and do not consume scarce runtime bind slots.
    objs.append(button(
        welcome, 24, 180, 210, 40, "New Session",
        bg="#E64501", fg="#FCFCFF",
        radius=20, size=15, callback="ai_new",
    ))
    objs.append(button(
        welcome, 246, 180, 210, 40, "Continue",
        bg="#18181A", fg="#FCFCFF",
        radius=20, size=15, callback="ai_resume_work",
    ))

    feature_groups = (
        (
            ("Quick Ask", "ai_mode_quick", "quick"),
            ("AI Create", "ai_mode_create", "create"),
            ("Install Skill", "ai_mode_skill", "skill"),
        ),
        (
            ("Remember", "ai_mode_memory", "memory"),
            ("Plan", "ai_mode_plan", "plan"),
            ("Schedule", "ai_mode_schedule", "schedule"),
        ),
    )

    def add_feature_flow(variant, y):
        feature_gate = len(objs)
        gate_name = f"ai_feature_{variant}"
        objs.append(layer(
            welcome, 24, y, 432, 54,
            hidden=True, name=gate_name,
        ))
        objs[-1]["bind"] = f"{gate_name}_visible"
        feature_flow = len(objs)
        flow_name = f"ai_feature_{variant}_pages"
        objs.append({
            "type": "page_flow", "parent": feature_gate,
            "x": 0, "y": 0,
            "w": 432, "h": 54,
            "name": flow_name,
            "page_count": 2, "selected": 0, "bar_height": 0,
            "axis": "horizontal", "cyclic": False,
        })
        for index, group in enumerate(feature_groups):
            feature_page = len(objs)
            objs.append(layer(
                feature_flow, 432 * index, 0, 432, 54,
                name=f"{flow_name}_tab{index}",
            ))
            card_start_x = 0 if index == 0 else 38
            for card_index, (title, action, mode_name) in enumerate(group):
                x = card_start_x + card_index * 134
                objs.append(button(
                    feature_page, x, 2, 126, 48, title,
                    bg="#18181A", fg="#D6D6DE", radius=24, size=16,
                    callback=action,
                ))
                # A static marker belongs to the same PageFlow transform as
                # its button, so it follows the selected item during swipes.
                # Binding the dot directly avoids the former full-page state
                # layer retained for each mode.
                indicator_name = f"ai_selected_{variant}_{mode_name}"
                objs.append(container(
                    feature_page, x + 12, 25, 6, 6,
                    bg="#FF4C01", radius=3,
                    name=indicator_name,
                    bind=f"{indicator_name}_visible", hidden=True,
                ))
                objs[-1]["bind_target"] = "visible"
            # The compact arrow uses the same pill language as the feature
            # cards, completes the input field's 432 px alignment, and is an
            # honest affordance: tap and swipe both change pages.
            hint_text = "›" if index == 0 else "‹"
            hint_x = 402 if index == 0 else 0
            objs.append(button(
                feature_page, hint_x, 2, 30, 48, hint_text,
                bg="#18181A", fg="#91919B", radius=15, size=22,
                align="center",
                name=f"{flow_name}_hint_{index}",
                events=[{
                    "event": "click", "action": "set_page",
                    "target_name": flow_name, "arg": 1 - index,
                    "animated": True,
                }],
            ))

    # Two fixed flows preserve the original responsive composition without
    # nesting a dynamic transform inside PageFlow (unsupported by new GSP).
    add_feature_flow("default", 340)
    add_feature_flow("mode", 284)

    def state_layer(parent, name, x=0, y=0,
                    width=CONTENT_W, height=CONTENT_H):
        state = len(objs)
        objs.append(layer(
            parent, x, y, width, height,
            hidden=True, name=name,
        ))
        objs[-1]["bind"] = f"{name}_visible"
        return state

    def default_composer(parent, name, *, chat=False):
        state = state_layer(parent, name, y=400, height=60)
        composer = len(objs)
        objs.append(container(
            state, 24, 0, 432, 60, bg="#1A1A1C", radius=30,
            border="#303034", border_w=1,
        ))
        composer_text = "Hold to talk, release to send…"
        objs.append(button(
            composer, 6, 6, 350, 48, "",
            bg="#1A1A1C", fg="#6E6E76", radius=24, size=16,
            name=f"{name}_prompt",
        ))
        objs.append(label(
            composer, 6, 16, 350, 28, composer_text,
            size=16, color="#6E6E76", bind=f"{name}_prompt",
            name=f"{name}_prompt_text", font_charset=AI_GLYPH_SEED,
        ))
        objs.append(button(
            composer, 368, 6, 56, 48, "", bg="#1A1A1C",
            fg="#91919B", radius=24, size=20,
        ))
        voice_icon(objs, composer, 384, 16)

    def mode_composer(parent):
        state = state_layer(parent, "ai_welcome_mode", y=344, height=116)
        panel = len(objs)
        objs.append(container(
            state, 24, 0, 432, 116, bg="#1A1A1C", radius=24,
            border="#3B3B40", border_w=1,
        ))
        objs.append(container(panel, 18, 20, 10, 10,
                              bg="#FF4C01", radius=3))
        objs.append(label(
            panel, 38, 12, 300, 28, "AI Create", size=18,
            bind="ai_mode_title", name="ai_mode_title",
        ))
        objs.append(button(
            panel, 376, 8, 40, 40, "X", bg="#242426",
            fg="#91919B", radius=20, size=16,
            callback="ai_clear_mode",
        ))
        objs.append(button(
            panel, 14, 58, 342, 46, "",
            bg="#111113", fg="#91919B", radius=23, size=16,
        ))
        objs.append(label(
            panel, 30, 67, 306, 28,
            "Hold to talk: describe your device…", size=16,
            color="#6E6E76",
            bind="ai_mode_prompt", name="ai_mode_prompt",
        ))
        objs.append(button(
            panel, 368, 58, 48, 46, "", bg="#242426",
            fg="#FCFCFF", radius=23, size=22,
        ))
        voice_icon(objs, panel, 384, 67)

    default_composer(welcome, "ai_welcome_default")
    mode_composer(welcome)

    # Continue Session is a retained overlay driven entirely by the native
    # Controller model.
    session_picker = state_layer(root, "ai_session_picker")
    objs.append(container(
        session_picker, 0, 0, CONTENT_W, CONTENT_H, bg="#000000",
    ))
    objs.append(label(session_picker, 88, 20, 250, 30,
                      "Select Session", size=22))
    objs.append(label(session_picker, 88, 51, 250, 22,
                      "0 sessions", size=15,
                      color="#91919B", bind="ai_session_count"))
    objs.append(button(
        session_picker, 24, 14, 52, 52, "<", bg="#000000",
        fg="#FCFCFF", radius=26, size=30, callback="ai_session_back",
        border="#3B3C3D", border_w=1, name="ai_session_back",
    ))
    session_page = len(objs)
    objs.append(layer(
        session_picker,
        {"default": 0, "min": -CONTENT_W, "max": CONTENT_W,
         "property": "x"},
        {"default": 82, "min": 82, "max": 83, "property": "y"},
        CONTENT_W, 300,
        name="ai_session_page",
    ))
    for row_index in range(SESSION_PAGE_SIZE):
        y = 8 + row_index * 92
        row_state = len(objs)
        objs.append(layer(
            session_page, 24, y, 432, 78,
            hidden=True, name=f"ai_session_{row_index}",
        ))
        objs[-1]["bind"] = f"ai_session_{row_index}_visible"
        row = len(objs)
        objs.append(container(
            row_state, 0, 0, 432, 78,
            bg="#181819", radius=22,
        ))
        resume = len(objs)
        objs.append(button(
            row, 0, 0, 356, 78, "",
            bg="#181819", fg="#FCFCFF", radius=22, size=16,
            events=[{
                "event": "click", "action": "call",
                "target_name": "ai_resume_session", "arg": row_index,
            }],
        ))
        objs.append(container(resume, 21, 18, 10, 10,
                              bg="#FF4C01", radius=5))
        objs.append(label(
            resume, 64, 11, 276, 28, "", size=18,
            bind=f"ai_session_{row_index}_title"))
        objs.append(label(
            resume, 64, 42, 276, 22, "Continue session", size=14,
            color="#91919B"))
        objs.append(button(
            row, 364, 15, 52, 48, "Delete", bg="#3A1711",
            fg="#FF8A70", radius=18, size=14,
            events=[{
                "event": "click", "action": "call",
                "target_name": "ai_delete_session", "arg": row_index,
            }],
        ))
    # The presenter centers and reveals only the dots backed by real pages.
    # 32 maximum sessions at three rows per page requires at most 11 dots.
    for dot_index in range(SESSION_MAX_PAGES):
        dot_state = len(objs)
        objs.append(layer(
            session_picker,
            {"default": 0, "min": 0, "max": CONTENT_W - 10,
             "property": "x"},
            {"default": 364, "min": 364, "max": 365,
             "property": "y"},
            10, 10, hidden=True,
            name=f"ai_session_dot_state_{dot_index}",
            bind=f"ai_session_dot_{dot_index}_visible",
        ))
        objs.append(container(
            dot_state, 0, 0, 10, 10, bg="#3B3C3D", radius=5,
            name=f"ai_session_dot_{dot_index}",
            bind=f"ai_session_dot_{dot_index}_color",
        ))
    objs.append(button(
        session_picker, 120,
        400, 240, 52, "New Blank Session",
        bg="#242426", fg="#D6D6DE", radius=26, size=16,
        callback="ai_new", name="ai_session_new",
    ))
    delete_modal = len(objs)
    objs.append(layer(
        session_picker, 0, 0, CONTENT_W, CONTENT_H, hidden=True,
        name="ai_session_delete_modal",
        bind="ai_session_delete_modal_visible", block_scene_swipe=True,
    ))
    objs.append(container(
        delete_modal, 0, 0, CONTENT_W, CONTENT_H,
        bg="#000000", opacity=196,
    ))
    delete_card = len(objs)
    objs.append(container(
        delete_modal, 36, 126, 408, 220,
        bg="#181819", radius=22, border="#3B3C3D", border_w=1,
    ))
    objs.append(label(
        delete_card, 24, 22, 360, 32, "Delete Session?", size=22,
        color="#FCFCFF", align="center",
    ))
    objs.append(label(
        delete_card, 24, 66, 360, 28, "", size=18,
        color="#FF8A70", align="center",
        bind="ai_session_delete_alias", name="ai_session_delete_alias",
    ))
    objs.append(label(
        delete_card, 24, 100, 360, 24, "This cannot be undone.", size=15,
        color="#91919B", align="center",
    ))
    delete_controls = len(objs)
    objs.append(layer(
        delete_card, 0, 0, 408, 220, hidden=True,
        name="ai_session_delete_controls",
        bind="ai_session_delete_controls_visible",
    ))
    objs.append(button(
        delete_controls, 76, 150, 116, 46, "Cancel",
        bg="#2A2A2C", fg="#FCFCFF", radius=23, size=16,
        callback="ai_cancel_delete_session",
    ))
    objs.append(button(
        delete_controls, 216, 150, 116, 46, "Delete",
        bg="#FF4C01", fg="#FCFCFF", radius=23, size=16,
        callback="ai_confirm_delete_session",
    ))
    delete_progress = len(objs)
    objs.append(layer(
        delete_card, 0, 0, 408, 220, hidden=True,
        name="ai_session_delete_progress",
        bind="ai_session_delete_progress_visible",
    ))
    objs.append(label(
        delete_progress, 76, 156, 256, 34, "Deleting…", size=17,
        color="#FF8A70", align="center",
    ))

    # Conversation is another Controller-owned state of the retained page.
    chat = len(objs)
    objs.append(layer(
        root, 0, 0, CONTENT_W, CONTENT_H,
        hidden=True, name="ai_chat",
    ))
    objs[-1]["bind"] = "ai_chat_visible"
    # This opaque hit surface covers and blocks the retained welcome chrome
    # while the local conversation state is active. It is not navigation:
    # both states remain siblings in the same scene/page instance.
    objs.append(button(
        chat, 0, 0, CONTENT_W, CONTENT_H, "",
        bg="#000000", radius=0, callback="ai_chat_background",
    ))
    objs.append(button(
        chat, 14, 14, 52, 52, "<", bg="#000000",
        radius=26, size=30, callback="ai_leave_chat",
        border="#3B3C3D", border_w=1,
    ))
    objs.append(label(chat, 76, 22, 220, 26,
                      "AI CREATE", size=16, color="#91919B"))
    # A bounded message window is populated only from the native Controller.
    rounds = len(objs)
    objs.append(layer(
        chat, 0, 0, CONTENT_W, CONTENT_H,
        hidden=True, name="ai_chat_rounds",
    ))
    objs[-1]["bind"] = "ai_chat_rounds_visible"
    objs.append({
        "type": "message_list", "parent": rounds,
        "x": 0, "y": 70, "w": CONTENT_W, "h": 320,
        "name": "ai_chat_messages", "font_size": 16,
        "font_charset": AI_GLYPH_SEED,
        "background_color": "#000000",
        "incoming_color": "#181819", "outgoing_color": "#FF4C01",
        "message_text_color": "#B8B8C0",
        "outgoing_text_color": "#FCFCFF",
        "bubble_radius": 20,
        "bubble_padding_x": 16, "bubble_padding_y": 10,
        "message_gap": 8, "side_margin": 24,
        "max_bubble_width": 392, "max_message_height": 360,
    })
    # Instance rows render above ordinary scene commands. Keep a shallow
    # clipped viewport for recording, but the Presenter updates only whichever
    # viewport is active instead of refreshing both copies on every message.
    voice_rounds = len(objs)
    objs.append(layer(
        chat, 0, 0, CONTENT_W, CONTENT_H,
        hidden=True, name="ai_chat_voice_rounds",
    ))
    objs[-1]["bind"] = "ai_chat_voice_rounds_visible"
    objs.append({
        "type": "message_list", "parent": voice_rounds,
        "x": 0, "y": 70, "w": CONTENT_W, "h": 100,
        "name": "ai_chat_voice_messages", "font_size": 16,
        "font_charset": AI_GLYPH_SEED,
        "row_template": "__ai_chat_messages_message_row",
        "background_color": "#000000",
        "incoming_color": "#181819", "outgoing_color": "#FF4C01",
        "message_text_color": "#B8B8C0",
        "outgoing_text_color": "#FCFCFF",
        "bubble_radius": 20,
        "bubble_padding_x": 16, "bubble_padding_y": 10,
        "message_gap": 8, "side_margin": 24,
        "max_bubble_width": 392, "max_message_height": 100,
    })

    default_composer(root, "ai_chat_default", chat=True)

    notice = len(objs)
    objs.append(layer(
        root, 24, 286, 432, 88, hidden=True, name="ai_notice",
    ))
    objs[-1]["bind"] = "ai_notice_visible"
    objs.append(container(notice, 0, 0, 432, 88, bg="#3A1711",
                          radius=20, border="#8A3826", border_w=1))
    objs.append(label(
        notice, 18, 14, 396, 60, "", size=16, color="#FFD8CF",
        bind="ai_notice_text", name="ai_notice_text",
    ))

    # Retained voice Composer: this layer grows upward from the default
    # Composer's exact origin. It stays in this root scene for recording,
    # cancellation and sending; no look-alike page is pushed or swapped in.
    voice_panel = len(objs)
    objs.append(layer(
        root, 0,
        {"default": 480, "min": 180, "max": 480,
         "property": "voice_panel_y"},
        CONTENT_W, 300, hidden=True, name="ai_voice_panel",
    ))
    objs[-1]["bind"] = "ai_voice_panel_visible"
    card = len(objs)
    objs.append(container(
        voice_panel, 24, 0, 432, 280, bg="#1A1110", radius=26,
        border="#3D2925", border_w=1,
    ))
    objs.append(container(card, 190, 16, 52, 4,
                          bg="#555158", radius=2))
    objs.append(label(
        card, 34, 42, 364, 66, "Listening…", size=22,
        align="center", bind="ai_voice_text", name="ai_voice_text",
    ))
    objs.append(label(
        card, 42, 226, 294, 30, "Release: send · Swipe up: cancel",
        size=15, align="center", bind="ai_voice_hint",
        name="ai_voice_hint",
    ))
    objs.append(label(
        card, 330, 228, 72, 28, "0.0 s", size=16,
        color="#91919B", align="right",
        bind="ai_voice_time", name="ai_voice_time",
    ))

    # Dynamic transforms cannot be nested inside the sliding panel transform,
    # so the waveform is a root sibling at the panel's open position.  Its
    # visibility follows the panel while each leaf owns only height and Y.
    waveform = len(objs)
    objs.append(layer(
        root, 24, 0, 432, 480, hidden=True,
        name="ai_voice_waveform", bind="ai_voice_waveform_visible",
    ))
    # A compact, centre-weighted waveform reads as one living surface instead
    # of a row of static equaliser bars.  Height and Y are both dynamic so the
    # pills grow around their shared baseline; the Presenter supplies slow,
    # staggered targets and GSP interpolates between them.
    wave_heights = (10, 14, 20, 28, 36, 44, 52, 58, 52, 44, 36, 28, 20, 14, 10)
    wave_colors = (
        "#6F2818", "#84301A", "#9A3519", "#B83A14", "#D6400D",
        "#EA4608", "#F74A04", "#FF632B", "#F74A04", "#EA4608",
        "#D6400D", "#B83A14", "#9A3519", "#84301A", "#6F2818",
    )
    # Dynamic Y is expressed in scene coordinates by GSP even though the
    # static X remains relative to this root sibling.
    wave_center_y = 354
    for index, (height, color) in enumerate(zip(wave_heights, wave_colors)):
        objs.append(container(
            waveform, 104 + index * 14,
            {
                "default": wave_center_y - height // 2,
                "min": wave_center_y - 32,
                "max": wave_center_y - 3,
                "property": "y",
            },
            6,
            {
                "default": height,
                "min": 6,
                "max": 64,
                "property": "height",
            },
            bg=color, radius=3, name=f"ai_voice_bar_{index:02d}",
        ))

    return objs


BUILDERS = {
    "ai_create": build_ai_create,
    "calc": build_calc,
    "battery": build_battery,
    "stopwatch": build_stopwatch,
    "timer": build_timer,
    "music": build_music,
    "world_clock": build_world_clock,
    "settings": build_settings,
}


def main():
    write_scene(
        scene_out_path(HERE, "ai_create_480.json"),
        "ai_create",
        build_ai_create(),
        font=AI_FONT,
        # App-level Back is a control-plane action invoked by the shared
        # Shell. Register it without attaching it to a visible hit target so
        # the chat page's ai_leave_chat remains local to AI Create.
        actions=[{
            "event": "none",
            "action": "call",
            "target_name": "ai_exit_app",
            "src_name": "ai_create_root",
        }],
    )


if __name__ == "__main__":
    main()
