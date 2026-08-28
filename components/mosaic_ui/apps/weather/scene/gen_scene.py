#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Weather page matching the Mosaico 480 x 480 prototype."""

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H, CONTENT_W, container, explicit_charset, image, label, layer,
    scene_out_path, shared_charset, shared_prefix, write_scene,
)

FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD
FONT_POLICIES = {
    18: shared_charset(),
    20: shared_charset(),
    24: shared_charset(),
    88: explicit_charset("-0123456789°"),
}
CHARSET = shared_charset()["charset"]
ASSETS = "../assets"
WEATHER_ART = ("overcast", "sunny", "cloudy", "snow", "windy", "thunder")


def visibility(obj, bind):
    obj["bind"] = bind
    obj["bind_target"] = "visible"
    return obj


def main():
    images = [(f"{ASSETS}/{name}.png", 257, 257) for name in WEATHER_ART]
    objs, content = shared_prefix(images, FONT_POLICIES, BOLD)
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg="#000000"))

    # The shared App shell owns y=0..63. Prototype current-weather region.
    objs.append(label(content, 33, 86, 178, 80, "--", size=88,
                      color="#F9FAFB", bind="weather_temperature",
                      name="weather_temperature", font_charset="-0123456789°"))
    objs.append(label(content, 33, 171, 190, 34, "Unavailable", size=24,
                      color="#F9FAFB", bind="weather_condition",
                      name="weather_condition", font_charset=CHARSET))
    objs.append(label(content, 33, 204, 190, 26, "Location unavailable", size=18,
                      color="#91919B", bind="weather_city",
                      name="weather_city", font_charset=CHARSET))

    # Original WX_ART illustrations exported from the web prototype.
    for index, name in enumerate(WEATHER_ART):
        art = len(objs)
        objs.append(visibility(layer(content, 219, 64, 261, 176,
                                     name=f"weather_art_{name}",
                                     hidden=index != 0),
                               f"weather_art_{name}_visible"))
        objs.append(image(art, f"{ASSETS}/{name}.png",
                          4, -36, 257, 257, name=f"weather_{name}_image"))

    # Match the prototype's 24 px typography and 40 px row rhythm.  The card
    # remains one page; seven days scroll vertically inside its clipped list.
    panel = len(objs)
    objs.append(container(content, 13, 248, 455, 216,
                          bg="#181819", radius=24))
    objs.append({
        "type": "list", "parent": panel,
        "x": 16, "y": 16, "w": 423, "h": 184,
        "name": "weather_forecast_list",
        "item_height": 40, "item_count": 7,
        "row_template": "weather_forecast_row",
        "scroll_snapshot": True, "snap_to_item": True,
        "bg_color": "#181819", "fg_color": "#FCFCFF",
    })
    row = len(objs)
    objs.append({
        "type": "container", "parent": -1,
        "x": 0, "y": 0, "w": 423, "h": 40,
        "template": "weather_forecast_row",
        "callback": "weather_forecast_row",
        "bg_color": "#181819", "radius": 0,
    })
    objs.append(label(row, 0, 0, 112, 24, "--/--", size=24,
                      color="#D6D6DE", name="date",
                      font_charset="-/0123456789"))
    forecast_icon = image(row, f"{ASSETS}/forecast_cloudy.png",
                          104, 0, 24, 24, name="icon")
    forecast_icon["dynamic_image"] = True
    objs.append(forecast_icon)
    objs.append(label(row, 275, 0, 58, 24, "--", size=24,
                      color="#91919B", align="right", name="low",
                      font_charset="-0123456789°"))
    objs.append(label(row, 349, 0, 58, 24, "--", size=24,
                      color="#FCFCFF", align="right", name="high",
                      font_charset="-0123456789°"))
    objs.append(label(content, 366, 70, 90, 22, "OFFLINE", size=16,
                      color="#91919B", align="right", bind="weather_status",
                      name="weather_status", font_charset=CHARSET))

    write_scene(scene_out_path(HERE, "weather_480.json"),
                "weather", objs, font=FONT)


if __name__ == "__main__":
    main()
