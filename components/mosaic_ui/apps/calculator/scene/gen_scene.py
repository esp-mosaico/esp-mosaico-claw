#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""480 x 480 scene for Calculator, Split Bill, and calculation history."""

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


# Per-size glyph sets: only the characters that labels actually render.
# Large sizes stay app-local; do not expand them to printable ASCII.
CHARSET_16 = packed_charset(
    "() SPLIT HISTORY TIP PEOPLE",
    "PER PERSON ENTER BILL FIRST SPLIT UNAVAILABLE",
    "PAY MAX $9999.99",
    "ENTER BILL IN CALC · OPEN CALC",
    "BILL · EDIT BILL TOO LARGE",
    "0123456789 RESULTS RESULT CLEAR SURE?",
)
CHARSET_18 = packed_charset(
    "ENTER A CALCULATION MAX",
    "Complete a calculation to save it.",
    "0123456789.$=()%+×÷−eE -/x*",
)
CHARSET_22 = packed_charset(
    "SPLIT BILL HISTORY AC DEL %± 0123456789 HISTORY IS CLEAR",
)
CHARSET_28 = packed_charset("0123456789.÷×−+=")
CHARSET_38 = packed_charset("‹0123456789.$ TOO HIGH")
CHARSET_48 = packed_charset(
    "0123456789.+-eE ",
    "DIVIDE BY 0",
    "RESULT OUT OF RANGE",
    "COMPLETE THE EXPRESSION",
    "CHECK THE EXPRESSION",
    "INPUT LIMIT REACHED",
)
CHARSET_BY_SIZE = {
    16: CHARSET_16,
    18: CHARSET_18,
    22: CHARSET_22,
    28: CHARSET_28,
    38: CHARSET_38,
    48: CHARSET_48,
}
FONT_POLICIES = {
    16: explicit_charset(CHARSET_16),
    18: explicit_charset(CHARSET_18),
    22: explicit_charset(CHARSET_22),
    28: explicit_charset(CHARSET_28),
    38: explicit_charset(CHARSET_38),
    48: explicit_charset(CHARSET_48),
}

BG = "#000000"
KEY = "#1F1F21"
KEY_SOFT = "#E5E5E8"
PAPER = "#FCFCFF"
INK = "#101012"
TEXT = "#FCFCFF"
MUTED = "#91919B"
ACCENT = "#FF4C01"
ERROR = "#EF4444"


def visible(obj, bind, *, hidden=False):
    obj["bind"] = bind
    obj["bind_target"] = "visible"
    if hidden:
        obj["hidden"] = True
    return obj


def dynamic_label(parent, x, y, w, h, text, bind, *, size=18,
                  color=TEXT, align="left", name=None):
    return label(
        parent, x, y, w, h, text, size=size, color=color, align=align,
        bind=bind, name=name or bind, font_charset=CHARSET_BY_SIZE[size],
    )


def calc_button(parent, x, y, w, h, text, callback, *, tone="number",
                name=None, size=28):
    if tone == "utility":
        bg, fg, font_size = KEY_SOFT, INK, 22
    elif tone == "operator":
        bg, fg, font_size = "#24150F", ACCENT, 28
    elif tone == "equals":
        bg, fg, font_size = ACCENT, PAPER, 28
    else:
        bg, fg, font_size = KEY, TEXT, size
    obj = button(
        parent, x, y, w, h, text, bg=bg, fg=fg, radius=18,
        size=font_size, callback=callback, name=name or callback,
        border="#343438", border_w=1, align="center",
    )
    obj["font_charset"] = CHARSET_BY_SIZE[font_size]
    return obj


def home_indicator(objs, parent):
    objs.append(container(parent, 188, 468, 104, 4, bg="#A8A8AC", radius=2))


def build_main(objs, parent):
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H, bg=BG))
    display = len(objs)
    objs.append(container(
        parent, 16, 12, 448, 128, bg=PAPER, radius=24,
        border="#D8D8DC", border_w=1,
    ))

    for text, callback, x, w in (
        ("( )", "calc_paren", 184, 56),
        ("SPLIT", "calc_split", 248, 78),
        ("HISTORY", "calc_history", 334, 112),
    ):
        obj = button(
            display, x - 16, 6, w, 42, text, bg="#E7E7EA", fg=INK,
            radius=18, size=16, callback=callback, name=callback,
            align="center",
        )
        obj["font_charset"] = CHARSET_BY_SIZE[16]
        objs.append(obj)

    objs.append(dynamic_label(
        display, 18, 48, 414, 30, "ENTER A CALCULATION",
        "calc_expression", size=18, color="#55555D", align="right",
    ))
    objs.append(dynamic_label(
        display, 18, 77, 414, 46, "0", "calc_result",
        size=48, color=INK, align="right",
    ))

    keys = (
        ("AC", "calc_clear", "utility"), ("DEL", "calc_backspace", "utility"),
        ("%", "calc_percent", "utility"), ("÷", "calc_divide", "operator"),
        ("7", "calc_7", "number"), ("8", "calc_8", "number"),
        ("9", "calc_9", "number"), ("×", "calc_multiply", "operator"),
        ("4", "calc_4", "number"), ("5", "calc_5", "number"),
        ("6", "calc_6", "number"), ("−", "calc_subtract", "operator"),
        ("1", "calc_1", "number"), ("2", "calc_2", "number"),
        ("3", "calc_3", "number"), ("+", "calc_add", "operator"),
        ("±", "calc_sign", "utility"), ("0", "calc_0", "number"),
        (".", "calc_decimal", "number"), ("=", "calc_equal", "equals"),
    )
    for index, (text, callback, tone) in enumerate(keys):
        row, col = divmod(index, 4)
        objs.append(calc_button(
            parent, 16 + col * 113, 147 + row * 61, 108, 55,
            text, callback, tone=tone,
        ))
    home_indicator(objs, parent)


def back_button(objs, parent, name):
    obj = button(
        parent, 18, 10, 60, 48, "‹", bg=KEY, fg=ACCENT,
        radius=20, size=38, callback="calc_back", name=name,
        align="center",
    )
    obj["font_charset"] = CHARSET_BY_SIZE[38]
    objs.append(obj)


def build_split(objs, parent):
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H, bg=BG))
    back_button(objs, parent, "calc_back_split")
    objs.append(label(
        parent, 90, 17, 300, 34, "SPLIT BILL", size=22,
        color=TEXT, align="center", font_charset=CHARSET_22,
    ))

    hero = len(objs)
    objs.append(container(
        parent, 18, 70, 444, 160, bg=PAPER, radius=24,
        border="#D8D8DC", border_w=1,
    ))
    objs.append(dynamic_label(
        hero, 20, 16, 240, 24, "PER PERSON", "split_each_label",
        size=16, color=INK,
    ))
    objs.append(dynamic_label(
        hero, 20, 43, 240, 55, "$0.00", "split_each",
        size=38, color=INK,
    ))
    objs.append(dynamic_label(
        hero, 20, 104, 240, 30, "", "split_remainder",
        size=16, color="#44444B",
    ))
    objs.append(container(hero, 274, 0, 170, 160, bg="#EFEFF2"))
    for index, title in enumerate(("BILL", "TIP", "TOTAL")):
        y = 10 + index * 49
        objs.append(label(hero, 288, y, 62, 24, title, size=14, color="#66666E"))
        objs.append(dynamic_label(
            hero, 344, y, 86, 24, "$0.00",
            f"split_{title.lower()}", size=18, color=INK, align="right",
        ))
        if index < 2:
            objs.append(container(hero, 288, y + 35, 142, 1, bg="#D0D0D4"))

    objs.append(label(parent, 22, 244, 80, 22, "TIP", size=16, color=MUTED,
                      font_charset=CHARSET_16))
    for index, (text, callback) in enumerate((
        ("0%", "split_tip_0"), ("10%", "split_tip_10"),
        ("15%", "split_tip_15"), ("20%", "split_tip_20"),
    )):
        x = 18 + index * 113
        selected = container(
            parent, x, 270, 106, 54, bg=ACCENT, radius=16,
            bind=f"{callback}_selected", hidden=index != 2,
        )
        selected["bind_target"] = "visible"
        objs.append(selected)
        obj = button(
            parent, x, 270, 106, 54, text, bg="#242426", fg=TEXT,
            radius=16, size=22, callback=callback, name=callback,
            border="#3A3A3E", border_w=1, opacity=210,
        )
        obj["font_charset"] = CHARSET_BY_SIZE[22]
        objs.append(obj)

    minus = calc_button(
        parent, 18, 333, 98, 72, "−", "split_people_minus",
        tone="operator", size=32,
    )
    plus = calc_button(
        parent, 364, 333, 98, 72, "+", "split_people_plus",
        tone="operator", size=32,
    )
    objs.extend((minus, plus))
    objs.append(container(parent, 124, 333, 232, 72, bg="#151517", radius=20))
    objs.append(dynamic_label(
        parent, 136, 339, 96, 52, "4", "split_people",
        size=38, color=TEXT, align="right",
    ))
    objs.append(label(
        parent, 242, 351, 102, 30, "PEOPLE", size=16, color=MUTED,
        align="left", font_charset=CHARSET_16,
    ))

    source = button(
        parent, 18, 413, 444, 44, "ENTER BILL IN CALC",
        bg="#1D1D20", fg=MUTED, radius=16, size=16,
        callback="split_bill_edit", name="split_bill_edit",
        border="#333337", border_w=1, align="center",
    )
    source["bind"] = "split_source"
    source["font_charset"] = CHARSET_BY_SIZE[16]
    objs.append(source)
    home_indicator(objs, parent)


def build_history(objs, parent):
    objs.append(container(parent, 0, 0, CONTENT_W, CONTENT_H, bg=BG))
    back_button(objs, parent, "calc_back_history")
    objs.append(label(
        parent, 90, 17, 300, 34, "HISTORY", size=22,
        color=TEXT, align="center", font_charset=CHARSET_22,
    ))

    tape = len(objs)
    objs.append(container(
        parent, 22, 74, 436, 374, bg=PAPER, radius=22,
        border="#D8D8DC", border_w=1,
    ))
    objs.append(dynamic_label(
        tape, 20, 15, 210, 30, "0 RESULTS", "history_count",
        size=16, color=INK,
    ))
    clear = button(
        tape, 318, 6, 100, 46, "CLEAR", bg="#ECECEF", fg="#55555D",
        radius=18, size=16, callback="history_clear", name="history_clear",
        align="center",
    )
    clear["bind"] = "history_clear_text"
    clear["font_charset"] = CHARSET_BY_SIZE[16]
    objs.append(clear)
    objs.append(container(tape, 18, 58, 400, 1, bg="#D0D0D4"))

    empty = len(objs)
    empty_layer = layer(
        tape, 0, 59, 436, 315, name="history_empty",
        bind="history_empty_visible",
    )
    empty_layer["bind_target"] = "visible"
    objs.append(empty_layer)
    objs.append(label(
        empty, 38, 84, 360, 38, "HISTORY IS CLEAR",
        size=22, color=INK, align="center", font_charset=CHARSET_22,
    ))
    objs.append(label(
        empty, 48, 132, 340, 28, "Complete a calculation",
        size=18, color="#66666E", align="center", font_charset=CHARSET_18,
    ))
    objs.append(label(
        empty, 48, 162, 340, 28, "to save it.",
        size=18, color="#66666E", align="center", font_charset=CHARSET_18,
    ))

    for index in range(5):
        row = len(objs)
        row_layer = layer(
            tape, 0, 59 + index * 62, 436, 62, hidden=True,
            name=f"history_row_{index}",
            bind=f"history_row_{index}_visible",
        )
        row_layer["bind_target"] = "visible"
        objs.append(row_layer)
        row_button = button(
            row, 6, 0, 424, 61, "", bg=PAPER, fg=INK,
            radius=0, size=18, callback=f"history_load_{index}",
            name=f"history_load_{index}", align="left",
        )
        row_button["bind"] = f"history_row_{index}_text"
        row_button["font_charset"] = CHARSET_BY_SIZE[18]
        objs.append(row_button)
        objs.append(container(row, 14, 61, 408, 1, bg="#D8D8DC"))
    home_indicator(objs, parent)


def main() -> None:
    objs, content = shared_prefix([], FONT_POLICIES, BOLD)
    stack = len(objs)
    objs.append({
        "type": "stackview", "parent": content,
        "x": 0, "y": 0, "w": CONTENT_W, "h": CONTENT_H,
        "name": "calculator_stack", "page_count": 3, "initial_page": 0,
        "capacity": 3, "axis": "horizontal",
    })

    main_page = len(objs)
    objs.append(layer(stack, 0, 0, CONTENT_W, CONTENT_H,
                      name="calculator_stack_page0"))
    build_main(objs, main_page)

    split_page = len(objs)
    objs.append(layer(stack, CONTENT_W, 0, CONTENT_W, CONTENT_H,
                      name="calculator_stack_page1"))
    build_split(objs, split_page)

    history_page = len(objs)
    objs.append(layer(stack, CONTENT_W * 2, 0, CONTENT_W, CONTENT_H,
                      name="calculator_stack_page2"))
    build_history(objs, history_page)

    write_scene(
        scene_out_path(HERE, "calculator_480.json"),
        "calculator", objs, font=FONT,
    )


if __name__ == "__main__":
    main()
