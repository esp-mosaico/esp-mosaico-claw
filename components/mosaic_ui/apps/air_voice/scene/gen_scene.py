#!/usr/bin/env python3
"""Air Voice (whisper capture) test screen for the 480x480 round display.

Every row is placed inside the circular safe area. Dynamic text is bound to
the native App (air_voice_app.c); recording/playback state switches between
pre-authored layers instead of recoloring widgets.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))
from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H, CONTENT_W, button, container, label, layer, scene_out_path,
    shared_charset, shared_prefix, write_scene,
)

ASCII = shared_charset()["charset"]
SIZES = (14, 15, 16, 18, 20, 22)

BG = "#0B0C0F"
CARD = "#17191E"
TRACK = "#2A2D35"
INK = "#F2F3F5"
MUTED = "#8E929C"
ACCENT = "#FF6B35"
REC_RED = "#E5383B"
GOOD = "#4FD66D"
BUTTON = "#2A2D35"


def dyn_label(parent, x, y, w, h, text, bind, *, size=16, color=INK, align="center",
              bold=False, callback=None):
    return label(parent, x, y, w, h, text, size=size, color=color, align=align,
                 bind=bind, name=bind, font=DEJAVU_SANS_BOLD if bold else DEJAVU_SANS,
                 font_charset=ASCII, callback=callback)


def meter_row(objs, card, y, title, bind, color):
    objs.append(label(card, 16, y - 2, 44, 20, title, size=14, color=MUTED,
                      font=DEJAVU_SANS))
    objs.append({
        "type": "progress", "parent": card,
        "x": 60, "y": y + 2, "w": 248, "h": 12,
        "value": 0, "min": 0, "max": 100,
        "bg_color": TRACK, "fg_color": color, "radius": 6,
        "name": f"{bind}_bar", "bind": f"{bind}_bar",
    })
    objs.append(dyn_label(card, 314, y - 3, 76, 22, "-- dB", f"{bind}_text",
                          size=15, align="right"))


def round_button(objs, parent, x, y, w, h, text, callback, *, bg=BUTTON, fg=INK,
                 size=16, text_bind=None):
    objs.append(button(parent, x, y, w, h, "" if text_bind else text, bg=bg, fg=fg,
                       radius=h // 2, size=size, callback=callback, name=callback))
    if text_bind:
        # Bound caption on top of the button; it forwards the same callback so
        # a tap on the text still reaches the App.
        objs.append(dyn_label(parent, x, y + (h - 24) // 2, w, 24, text, text_bind,
                              size=size, color=fg, bold=True, callback=callback))


def main():
    policies = {size: shared_charset() for size in SIZES}
    objs, content = shared_prefix([], policies, DEJAVU_SANS_BOLD)
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg=BG))

    # Header: title, mode, state and last recording.
    objs.append(label(content, 90, 22, 300, 30, "AIR VOICE", size=22, color=ACCENT,
                      align="center", font=DEJAVU_SANS_BOLD))
    objs.append(dyn_label(content, 60, 54, 360, 22, "WHISPER 3-5CM | GAIN 30+0 dB",
                          "av_mode", size=15, color=MUTED))
    objs.append(dyn_label(content, 40, 80, 400, 30, "READY", "av_status", size=22,
                          bold=True))
    objs.append(dyn_label(content, 40, 112, 400, 20, "NO RECORDINGS", "av_last",
                          size=14, color=MUTED))

    # Meter card.
    card = len(objs)
    objs.append(container(content, 40, 140, 400, 128, bg=CARD, radius=20))
    meter_row(objs, card, 12, "IN", "av_in", ACCENT)
    meter_row(objs, card, 40, "OUT", "av_out", GOOD)
    objs.append(dyn_label(card, 16, 66, 368, 22, "NOISE -- dB   SNR -- dB", "av_noise",
                          size=15, align="left"))
    objs.append(dyn_label(card, 16, 94, 340, 22, "WHISPER --  AIR --  AGC --",
                          "av_detail", size=15, align="left", color=MUTED))
    vad = len(objs)
    objs.append(layer(card, 366, 96, 18, 18, hidden=True, name="av_vad",
                      bind="av_vad"))
    objs.append(container(vad, 0, 0, 18, 18, bg=GOOD, radius=9))

    # Operator hint.
    objs.append(dyn_label(content, 50, 274, 380, 24, "WHISPER INTO THE MIC", "av_hint",
                          size=16, color=ACCENT, bold=True))

    # Primary row: PLAY | REC/STOP | TRACK.
    round_button(objs, content, 92, 314, 96, 56, "PLAY", "av_play", text_bind="av_play_text")
    rec_idle = len(objs)
    objs.append(layer(content, 200, 302, 80, 80, name="av_rec_idle", bind="av_rec_idle"))
    objs.append(button(rec_idle, 0, 0, 80, 80, "REC", bg=REC_RED, fg=INK, radius=40,
                       size=18, callback="av_record", name="av_record"))
    rec_active = len(objs)
    objs.append(layer(content, 200, 302, 80, 80, hidden=True, name="av_rec_active",
                      bind="av_rec_active"))
    objs.append(button(rec_active, 0, 0, 80, 80, "STOP", bg=INK, fg=REC_RED, radius=40,
                       size=18, callback="av_record", name="av_record_stop"))
    round_button(objs, content, 292, 314, 96, 56, "PROC", "av_track",
                 text_bind="av_track_text")

    # Secondary row: VOL- | MODE | DEL | VOL+.
    round_button(objs, content, 90, 390, 52, 40, "-", "av_vol_down", size=20)
    round_button(objs, content, 150, 390, 100, 40, "MODE", "av_mode_next", size=15)
    round_button(objs, content, 258, 390, 72, 40, "DEL", "av_delete", size=15)
    round_button(objs, content, 338, 390, 52, 40, "+", "av_vol_up", size=20)

    write_scene(scene_out_path(HERE, "air_voice_480.json"), "air_voice", objs,
                font=DEJAVU_SANS)


if __name__ == "__main__":
    main()
