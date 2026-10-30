#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Executable Works launcher: recent and installed skills."""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import NOTO_SANS  # noqa: E402
from scene_common import (  # noqa: E402
    ASCII_PRINTABLE,
    CONTENT_H,
    CONTENT_W,
    app_root_shell,
    auto_charset,
    button,
    container,
    explicit_charset,
    label,
    layer,
    scene_out_path,
    shared_charset,
    shared_prefix,
    write_scene,
)

FONT = NOTO_SANS
BOLD = FONT
TABS = ("Recent", "Installed")
ROW_COUNT = 4
STATE_CHARSET = ASCII_PRINTABLE
META_CHARSET = ASCII_PRINTABLE + "·"

PLACEHOLDER = "../../../common/assets/black_placeholder.png"
IMAGES = [(PLACEHOLDER, 48, 48)]
FONT_POLICIES = {
    14: auto_charset(),
    15: explicit_charset(META_CHARSET),
    17: explicit_charset(STATE_CHARSET),
    18: shared_charset(),
    22: auto_charset(),
    30: auto_charset(),
    46: explicit_charset("‹"),
}


def call_event(name: str, arg: int = 0) -> list[dict]:
    return [{
        "event": "click",
        "action": "call",
        "target_name": name,
        "arg": arg,
    }]


def tab_button(objs, parent, x, width, text, index):
    events = call_event("works_select_tab", index)
    objs.append(button(
        parent, x, 65, width, 50, text,
        radius=25, size=22, name=f"works_tab_{index}",
        bg="#000000", fg="#D6D6DE", border="#3B3C3D", border_w=1,
        events=events,
    ))

    selected = len(objs)
    objs.append(layer(
        parent, x, 65, width, 50, hidden=True,
        bind=f"works_tab_{index}_selected",
        bind_target="visible",
        name=f"works_tab_{index}_selected",
    ))
    objs.append(button(
        selected, 0, 0, width, 50, text,
        radius=25, size=22, bg="#FCFCFF", fg="#000000",
        events=events,
    ))


def status_layer(objs, parent, row_index, name, text, color):
    state = len(objs)
    objs.append(layer(
        parent, 0, 0, CONTENT_W, 64, hidden=True,
        bind=f"works_row_{row_index}_{name}_visible",
        bind_target="visible",
        name=f"works_row_{row_index}_{name}",
    ))
    objs.append(container(state, 320, 30, 8, 8, bg=color, radius=4))
    state_label = label(
        state, 320, 20, 140, 28, text, size=17, color=color,
        align="right",
        bind=(f"works_row_{row_index}_active_text"
              if name == "active" else None),
        font_charset=STATE_CHARSET,
    )
    state_label["overflow"] = "ellipsis"
    objs.append(state_label)


def works_row(objs, parent, row_index):
    row = len(objs)
    objs.append(layer(
        parent, 0, 124 + row_index * 72, CONTENT_W, 64,
        hidden=True, bind=f"works_row_{row_index}_visible",
        bind_target="visible",
        name=f"works_row_{row_index}",
    ))
    events = call_event("works_toggle", row_index)
    objs.append(button(
        row, 0, 0, CONTENT_W, 64, "", bg="#000000", fg="#000000",
        events=events,
    ))

    # Keep the default tile underneath the asynchronous image layer.
    objs.append(container(row, 16, 8, 48, 48, bg="#242424", radius=10))
    objs.append(label(row, 16, 23, 48, 20, "APP", size=14, color="#91919B", align="center"))
    icon = len(objs)
    objs.append(layer(row, 16, 8, 48, 48, hidden=True,
                      name=f"works_row_{row_index}_icon_layer",
                      bind=f"works_row_{row_index}_icon_visible", bind_target="visible"))
    objs.append({
        "type": "image", "parent": icon, "x": 0, "y": 0, "w": 48, "h": 48,
        "image": PLACEHOLDER, "codec": "raw", "fit": "contain", "dynamic_image": True,
        "name": f"works_row_{row_index}_icon", "bind": f"works_row_{row_index}_icon",
        "events": events,
    })

    title = label(
        row, 76, 8, 232, 28, "", size=22, color="#FCFCFF",
        bind=f"works_row_{row_index}_name",
        font_charset=ASCII_PRINTABLE,
    )
    title["overflow"] = "ellipsis"
    objs.append(title)
    meta = label(
        row, 76, 36, 232, 20, "", size=15, color="#595959",
        bind=f"works_row_{row_index}_meta",
        font_charset=META_CHARSET,
    )
    meta["overflow"] = "ellipsis"
    objs.append(meta)

    status_layer(objs, row, row_index, "active", "Running", "#35E68A")
    status_layer(objs, row, row_index, "stopped", "Stopped", "#595959")
    status_layer(objs, row, row_index, "failed", "Error", "#FF4C01")
    objs.append(container(row, 16, 62, 448, 1, bg="#242424"))


def build_pager(objs, parent):
    pager = len(objs)
    objs.append(layer(
        parent, 0, 408, CONTENT_W, 52, hidden=True,
        bind="works_pager_visible", bind_target="visible",
        name="works_pager",
    ))
    objs.append(button(
        pager, 124, 2, 70, 48, "‹", bg="#181819", fg="#D6D6DE",
        radius=22, size=31, events=call_event("works_page_prev"),
    ))
    objs.append(label(
        pager, 198, 14, 84, 24, "1 / 1", size=14, color="#91919B",
        align="center", bind="works_page_text",
        font_charset=ASCII_PRINTABLE,
    ))
    objs.append(button(
        pager, 286, 2, 70, 48, "›", bg="#181819", fg="#D6D6DE",
        radius=22, size=31, events=call_event("works_page_next"),
    ))


def build_works_ui(objs, content):
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      name="works_root"))
    objs.append(container(root, 0, 0, CONTENT_W, CONTENT_H,
                          bg="#000000", name="works_bg"))
    app_root_shell(objs, root, "Works", name="shell_works",
                   header_style="settings")
    objs.append(label(
        root, 340, 26, 120, 28, "0 ITEMS", size=18, color="#595959",
        align="right", name="works_count", bind="works_count",
        font_charset=ASCII_PRINTABLE,
    ))

    tab_button(objs, root, 16, 98, TABS[0], 0)
    tab_button(objs, root, 122, 121, TABS[1], 1)
    for row_index in range(ROW_COUNT):
        works_row(objs, root, row_index)
    build_pager(objs, root)


def main():
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD)
    build_works_ui(objs, content)
    write_scene(scene_out_path(HERE, "works_480.json"), "works", objs,
                font=FONT)


if __name__ == "__main__":
    main()
