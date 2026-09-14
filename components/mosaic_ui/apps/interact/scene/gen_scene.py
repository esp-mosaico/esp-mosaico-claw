#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / "common"))
from font_paths import DEJAVU_SANS
from scene_common import ASCII_PRINTABLE, container, image, label, layer, button, scene_out_path, write_scene
from rotate_scene import rotate_scene


def main():
    objs = [container(-1, 0, 0, 480, 480, name="root")]
    objs.append(image(0, "../assets/interact_board.png", 0, 0, 480, 480))

    # LED cards retain the hardware colors while following the dashboard layout.
    cards = [(12, "#FF4040"), (127, "#40FF70"), (242, "#4080FF"), (357, "#C060FF")]
    for i, (x, color) in enumerate(cards):
        glow = container(0, x, 64, 111, 63, bg=color, radius=8, border=color, border_w=2, opacity=55)
        glow.update(name=f"led_{i}", bind=f"led_{i}", bind_target="visible", hidden=True)
        objs.append(glow)
        objs.append(button(0, x, 64, 111, 63, "", radius=8, opacity=0, callback=f"led_{i}"))

    # LED 5 and LED 4 are the left and right key backlights.
    for led, x in [(5, 25), (4, 256)]:
        glow = container(0, x, 144, 34, 34, bg="#FFF5A0", radius=17, border="#FFF5A0", border_w=2, opacity=90)
        glow.update(name=f"led_{led}", bind=f"led_{led}", bind_target="visible", hidden=True)
        objs.append(glow)
        pressed = container(0, x, 144, 34, 34, bg="#4CFF85", radius=17, border="#D9FFE6", border_w=2, opacity=180)
        pressed.update(name=f"led_{led}_pressed", bind=f"led_{led}_pressed", bind_target="visible", hidden=True)
        objs.append(pressed)

    # Preserve the strong press feedback over the two large touch areas.
    for name, x, led in [("key_l", 12, 5), ("key_r", 242, 4)]:
        group = len(objs)
        active = layer(0, x, 131, 226, 233, name=name)
        active.update(bind=name, bind_target="visible", hidden=True)
        objs.append(active)
        objs.append(container(group, 0, 0, 226, 233, bg="#4CFF85", opacity=45, radius=8))
        objs.append(container(group, 0, 0, 226, 233, bg="#00000000", border="#4CFF85", border_w=3, radius=8))
        objs.append(container(group, 13, 13, 34, 34, bg="#4CFF85", border="#D9FFE6", border_w=2, radius=17))
        objs.append(container(group, 47, 190, 132, 28, bg="#4CFF85", radius=7))
        objs.append(label(group, 47, 192, 132, 24, "PRESSED", size=18, color="#071A0D", align="center"))
        objs.append(button(0, x, 131, 226, 233, "", radius=8, opacity=0, callback=f"led_{led}"))

    objs.append(label(0, 222, 413, 36, 22, "--", size=17, align="center", color="#101010"))
    group = len(objs)
    active = layer(0, 127, 368, 226, 98, name="pir")
    active.update(bind="pir", bind_target="visible", hidden=True)
    objs.append(active)
    objs.append(container(group, 25, 30, 176, 56, bg="#FF8A20", opacity=105, radius=28, border="#FFC080", border_w=2))
    objs.append(label(group, 25, 45, 176, 27, "MOTION", size=20, color="#201000", align="center"))

    objs.append(button(0, 12, 368, 111, 98, "", radius=8, opacity=0, callback="ir_send"))
    objs.append(label(0, 40, 410, 56, 28, "OFF", size=17, align="center", bind="ir_status", color="#0A0A0A"))

    # Let the LDR indicator track the measured percentage as well as the text.
    ldr_glow = container(0, 385, 397, 56, 56, bg="#303030", radius=28, opacity=255, bind="ldr_glow")
    ldr_glow["bind_target"] = "color"
    objs.append(ldr_glow)
    objs.append(label(0, 374, 411, 58, 26, "--%", size=17, align="center", bind="light", color="#0A0A0A"))
    objs.append(label(0, 23, 48, 255, 13, "", size=10, bind="status", color="#91919B"))

    # Keep input selection compact beside the hardware title.
    objs.append({
        "type": "dropdown", "parent": 0,
        "x": 292, "y": 11, "w": 116, "h": 40,
        "name": "input_mode_dropdown",
        "callback": "input_mode_select",
        "options": ["BUTTON", "TOUCH"],
        "selected": 1, "item_height": 38,
        "font_size": 15, "fg_color": "#E7E7EB",
        "bg_color": "#222224", "panel_color": "#29292C",
        "border_color": "#55555A", "border_width": 1,
        "radius": 10,
    })

    unavailable = layer(0, 30, 188, 420, 116, name="module_unavailable", bind="module_unavailable", bind_target="visible", hidden=True)
    objs.append(unavailable)
    objs.append(container(len(objs) - 1, 0, 0, 420, 116, bg="#161616", radius=18, border="#68686C", border_w=2, opacity=245))
    objs.append(container(len(objs) - 2, 18, 18, 8, 80, bg="#FF4C01", radius=4))
    objs.append(label(len(objs) - 3, 42, 22, 354, 34, "MODULE NOT CONNECTED", size=20, color="#F0F0F4", align="center"))
    objs.append(label(len(objs) - 4, 42, 62, 354, 28, "CONTROLS UNAVAILABLE", size=14, color="#9C9CA4", align="center"))
    for obj in objs:
        if obj["type"] in ("label", "button"):
            obj["font_charset"] = ASCII_PRINTABLE
    write_scene(scene_out_path(HERE, "interact_480.json"), "interact", rotate_scene(objs, HERE, DEJAVU_SANS, rotate=False), font=DEJAVU_SANS)


if __name__ == "__main__":
    main()
