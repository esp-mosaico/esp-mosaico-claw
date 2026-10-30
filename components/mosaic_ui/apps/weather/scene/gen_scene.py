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
    CONTENT_H, CONTENT_W, button, container, explicit_charset, image, label, layer,
    scene_out_path, shared_charset, shared_prefix, write_scene,
)

FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD
FONT_POLICIES = {
    16: shared_charset(),
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
    stack = len(objs)
    objs.append({"type": "stackview", "parent": content, "x": 0, "y": 0,
                 "w": CONTENT_W, "h": CONTENT_H, "name": "weather_stack",
                 "page_count": 3, "initial_page": 0, "capacity": 3,
                 "axis": "horizontal", "transition_ms": 200})
    content = len(objs)
    objs.append(layer(stack, 0, 0, CONTENT_W, CONTENT_H, name="weather_main_page"))
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg="#000000"))

    # The shared App shell owns y=0..63. Prototype current-weather region.
    objs.append(label(content, 33, 86, 178, 80, "--", size=88,
                      color="#F9FAFB", bind="weather_temperature",
                      name="weather_temperature", font_charset="-0123456789°"))
    objs.append(label(content, 33, 171, 190, 34, "Unavailable", size=24,
                      color="#F9FAFB", bind="weather_condition",
                      name="weather_condition", font_charset=CHARSET))
    city = label(content, 33, 196, 190, 48, "Location unavailable", size=18,
                 color="#91919B", bind="weather_city", name="weather_city",
                 font_charset=CHARSET)
    city["callback"] = "weather_open_location"
    city["overflow"] = "ellipsis"
    objs.append(city)

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
    objs.append(label(content, 308, 70, 148, 22, "OFFLINE", size=16,
                      color="#91919B", align="right", bind="weather_status",
                      name="weather_status", font_charset=CHARSET))

    location = len(objs)
    objs.append(layer(stack, CONTENT_W, 0, CONTENT_W, CONTENT_H, name="weather_location_page"))
    objs.append(container(location, 0, 0, CONTENT_W, CONTENT_H, bg="#000000"))
    back = button(location, 12, 8, 56, 48, "‹", size=24, bg="#000000", fg="#FCFCFF")
    back["callback"] = "weather_location_back"
    objs.append(back)
    objs.append(label(location, 96, 8, 288, 48, "Location", size=24, align="center"))
    for y, text, action in ((80, "Use network location", "weather_use_network"),
                             (144, "Search city", "weather_city_search")):
        control = button(location, 12, y, 456, 52, text, size=24,
                         bg="#181819", fg="#FCFCFF", radius=18)
        control["callback"] = action
        objs.append(control)
    objs.append(label(location, 16, 208, 448, 32, "Search for your city", size=18,
                      color="#91919B", bind="weather_city_search_status",
                      name="weather_city_search_status", font_charset=CHARSET))
    objs.append({"type": "list", "parent": location, "x": 12, "y": 248,
                 "w": 456, "h": 168, "name": "weather_city_results",
                 "item_height": 56, "item_count": 0, "row_template": "weather_city_row",
                 "bg_color": "#181819", "fg_color": "#FCFCFF", "scroll_snapshot": True})
    row = len(objs)
    objs.append({"type": "container", "parent": -1, "x": 0, "y": 0,
                 "w": 456, "h": 56, "template": "weather_city_row",
                 "callback": "weather_select_city", "bg_color": "#181819"})
    name = label(row, 16, 8, 424, 40, "City", size=20,
                 name="title", font_charset=CHARSET)
    name["overflow"] = "ellipsis"
    objs.append(name)
    objs.append(label(location, 16, 424, 448, 28, "City data: GeoNames / Open-Meteo",
                      size=16, color="#91919B", align="center", font_charset=CHARSET))

    search = len(objs)
    objs.append(layer(stack, CONTENT_W * 2, 0, CONTENT_W, CONTENT_H, name="weather_search_page"))
    objs.append(container(search, 0, 0, CONTENT_W, CONTENT_H, bg="#000000"))
    back = button(search, 12, 8, 56, 48, "‹", size=24, bg="#000000", fg="#FCFCFF")
    back["callback"] = "weather_location_back"
    objs.append(back)
    objs.append(label(search, 96, 8, 288, 48, "Search city", size=24, align="center"))
    objs.append(container(search, 12, 80, 352, 64, bg="#181819", radius=16))
    objs.append(label(search, 24, 92, 328, 40, "", size=24,
                      bind="weather_city_query", name="weather_city_query", font_charset=CHARSET))
    find = button(search, 376, 80, 92, 64, "Find", size=24,
                  bg="#FF4C01", fg="#FCFCFF", radius=16)
    find["callback"] = "weather_city_find"
    objs.append(find)
    objs.append(label(search, 16, 160, 448, 48, "City or city, country (e.g. Suzhou, CN)",
                      size=18, color="#91919B", bind="weather_city_query_hint",
                      name="weather_city_query_hint", font_charset=CHARSET))
    objs.append({"type": "keyboard", "parent": search, "x": 0, "y": 231,
                 "w": 480, "h": 233, "name": "weather_city_keyboard",
                 "bg_color": "#000000", "fg_color": "#ECECF0", "font_size": 24,
                 "key_color": "#55555A", "function_color": "#3B3C3D",
                 "function_text_color": "#ECECF0", "delete_color": "#3B3C3D",
                 "delete_text_color": "#ECECF0", "space_color": "#55555A",
                 "space_text_color": "#ECECF0", "ok_color": "#FF4C01",
                 "ok_text_color": "#FCFCFF", "key_radius": 8, "shift_label": "SHIFT",
                 "delete_label": "DELETE", "ok_label": "FIND", "symbols_label": "ABC",
                 "letters_label": "ABC", "space_label": "", "function_font_size": 20})

    write_scene(scene_out_path(HERE, "weather_480.json"),
                "weather", objs, font=FONT)


if __name__ == "__main__":
    main()
