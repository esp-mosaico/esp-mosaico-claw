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
    # Coordinates match the supplied 480 px PCB, including silkscreen indices.
    lamps = [(40, 40, 20, "#FF4040"), (176, 40, 20, "#40FF70"),
             (303, 40, 20, "#4080FF"), (438, 40, 20, "#C060FF"),
             (371, 176, 30, "#FFF5A0"), (109, 176, 30, "#FFF5A0")]
    for i, (x, y, size, color) in enumerate(lamps):
        glow = container(0, x - size // 2, y - size // 2, size, size, bg=color, radius=size // 2)
        glow.update(name=f"led_{i}", bind=f"led_{i}", bind_target="visible", hidden=True)
        objs.append(glow)
        if i in (4, 5):
            pressed = container(0, x - size // 2, y - size // 2, size, size, bg="#4CFF85", radius=size // 2)
            pressed.update(name=f"led_{i}_pressed", bind=f"led_{i}_pressed", bind_target="visible", hidden=True)
            objs.append(pressed)
        objs.append(button(0, x - 22, y - 22, 44, 44, "", opacity=0, callback=f"led_{i}"))
    # Group the strong outline, key highlight and badge under the live input bind.
    for name, x in [("key_l", 17), ("key_r", 280)]:
        group = len(objs)
        active = layer(0, x, 139, 182, 176, name=name)
        active.update(bind=name, bind_target="visible", hidden=True)
        objs.append(active)
        objs.append(container(group, 0, 0, 182, 176, bg="#4CFF85", opacity=65, radius=6))
        objs.append(container(group, 0, 0, 182, 176, bg="#00000000", border="#4CFF85", border_w=5, radius=6))
        objs.append(container(group, 64, 62, 54, 54, bg="#4CFF85", border="#FFFFFF", border_w=3, radius=27))
        objs.append(container(group, 25, 139, 132, 29, bg="#4CFF85", radius=7))
        objs.append(label(group, 25, 141, 132, 25, "PRESSED", size=20, color="#071A0D", align="center"))
    group = len(objs)
    active = layer(0, 182, 365, 116, 112, name="pir")
    active.update(bind="pir", bind_target="visible", hidden=True)
    objs.append(active)
    objs.append(container(group, 3, 0, 110, 110, bg="#FF8A20", opacity=100, radius=55))
    objs.append(container(group, 3, 0, 110, 110, bg="#00000000", border="#FF8A20", border_w=6, radius=55))
    objs.append(container(group, 0, 39, 116, 32, bg="#FF8A20", radius=8))
    objs.append(label(group, 0, 42, 116, 26, "MOTION", size=22, color="#201000", align="center"))
    objs.append(button(0, 22, 385, 68, 78, "", opacity=0, callback="ir_send"))
    objs.append(label(0, 17, 465, 170, 15, "", size=12, bind="status", color="#FFE060"))
    objs.append(label(0, 313, 447, 152, 18, "--%", size=16, align="right", bind="light", color="#FFE060"))
    objs.append(label(0, 18, 367, 120, 17, "", size=12, bind="ir_status", color="#FFE060"))
    objs.append(button(0, 185, 332, 104, 26, "AUTO", size=12, radius=8, callback="slot", name="slot", bg="#161616"))
    objs[-1]["bind"] = "slot"
    for obj in objs:
        if obj["type"] in ("label", "button"):
            obj["font_charset"] = ASCII_PRINTABLE
    write_scene(scene_out_path(HERE, "interact_480.json"), "interact", rotate_scene(objs, HERE, DEJAVU_SANS), font=DEJAVU_SANS)


if __name__ == "__main__":
    main()
