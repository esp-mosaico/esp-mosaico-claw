#!/usr/bin/env python3
"""Generate Welcome scenes from the canonical Python scene definitions."""

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import NOTO_SANS  # noqa: E402

WELCOME_CONTENT_WIDTH = 480
WELCOME_FONT = NOTO_SANS
WELCOME_FONT_SIZES = (18, 22, 24, 30, 36, 40, 44)
WELCOME_IMAGES = (("bodyDim.png", 188, 198),
                  ("bodyLit.png", 299, 316),
                  ("guideTop.png", 269, 180),
                  ("guideBottom.png", 269, 180),
                  ("hand.png", 69, 70),
                  ("arrow_down.png", 35, 80),
                  ("arrow_up.png", 35, 80),
                  ("arrow_right.png", 80, 35),
                  ("arrow_left_small.png", 56, 25))
WELCOME_GLYPH_SEED = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
    "›·"
)


def root(callback="welcome_next"):
    obj = {
        "type": "container", "parent": -1, "x": 0, "y": 0,
        "w": 480, "h": 480, "bg_color": "#060606", "radius": 0,
    }
    if callback:
        obj["callback"] = callback
    return obj


def label(x, y, w, h, text, size, color):
    return {
        "type": "label", "parent": 0, "x": x, "y": y, "w": w, "h": h,
        "text": text, "font_size": size, "fg_color": color,
    }


def image(x, y, w, h, filename, **extra):
    return {
        "type": "image", "parent": 0, "x": x, "y": y, "w": w, "h": h,
        "image": f"assets/{filename}", **extra,
    }


def marker(x, y, w, h, radius=2):
    return {
        "type": "container", "parent": 0, "x": x, "y": y, "w": w,
        "h": h, "bg_color": "#91919B", "radius": radius,
    }


def skip_button():
    return {
        "type": "button", "parent": 0, "x": 278, "y": 418,
        "w": 190, "h": 50, "text": "Skip Guide  ›", "font_size": 24,
        "bg_color": "#060606", "fg_color": "#91919B", "radius": 0,
        "callback": "welcome_exit",
    }


def gesture_header(direction, title):
    return [
        label(30, 64, 420, 34, direction, 24, "#FF4C01"),
        label(30, 97, 420, 56, title, 40, "#FCFCFF"),
    ]


def scene_intro():
    title1 = label(24, 52, 432, 48, "Welcome To", 44, "#FCFCFF")
    title2 = label(24, 96, 432, 48, "Mosaico", 44, "#FCFCFF")
    return [
        root(), title1, title2,
        label(24, 152, 432, 34, "Discover buttons and gestures", 22,
              "#91919B"),
        image(146, 212, 188, 198, "bodyDim.png"), skip_button(),
    ]


def scene_keys():
    orange = "#FF4C01"
    white = "#FCFCFF"
    return [
        root(), image(-35, 65, 299, 316, "bodyLit.png"),
        {"type": "container", "parent": 0, "x": 295, "y": 65,
         "w": 12, "h": 12, "bg_color": orange, "radius": 6},
        label(331, 62, 145, 28, "Single-click", 18, orange),
        label(295, 88, 181, 36, "Sleep/Go back", 24, white),
        {"type": "container", "parent": 0, "x": 295, "y": 125,
         "w": 32, "h": 12, "bg_color": orange, "radius": 6},
        label(331, 122, 145, 28, "Long-press", 18, orange),
        label(295, 148, 181, 36, "Chat with Claw", 24, white),
        label(12, 403, 170, 25, "Boot Key", 18, orange),
        label(12, 426, 170, 34, "Boot Mode", 24, white),
        label(188, 404, 120, 25, "Power Key", 18, orange),
        label(188, 427, 175, 34, "Power Off/On", 24, white),
    ]


def scene_g1():
    return [root(), *gesture_header("Swipe down", "Open Control Center"),
            image(106, 185, 269, 180, "guideTop.png"),
            marker(214, 212, 52, 4),
            image({"default": 223, "min": 222, "max": 224},
                  {"default": 216, "min": 216, "max": 272}, 35, 80,
                  "arrow_down.png", name="gesture_arrow"),
            image({"default": 264, "min": 263, "max": 265},
                  {"default": 287, "min": 287, "max": 343}, 69, 70,
                  "hand.png", name="gesture_hand"), skip_button()]


def scene_g2():
    return [root(), *gesture_header("Swipe up", "Back to Home"),
            image(106, 179, 269, 180, "guideBottom.png"),
            marker(214, 325, 52, 4),
            image({"default": 223, "min": 222, "max": 224},
                  {"default": 245, "min": 189, "max": 245}, 35, 80,
                  "arrow_up.png", name="gesture_arrow"),
            image({"default": 264, "min": 263, "max": 265},
                  {"default": 330, "min": 274, "max": 330}, 69, 70,
                  "hand.png", name="gesture_hand"), skip_button()]


def scene_g4():
    return [
        root(None), *gesture_header("Swipe left/right", "Switch screens"),
        image(106, 179, 269, 180, "guideBottom.png"),
        {"type": "layer", "parent": 0,
         "x": {"default": 176, "min": 148, "max": 176},
         "y": {"default": 246, "min": 245, "max": 247},
         "w": 56, "h": 25, "hidden": False,
         "bind": "gesture_arrow_left_visible", "name": "gesture_arrow_left"},
        {"type": "image", "parent": 4, "x": 0, "y": 0, "w": 56,
         "h": 25, "image": "assets/arrow_left_small.png"},
        {"type": "layer", "parent": 0,
         "x": {"default": 232, "min": 232, "max": 272},
         "y": {"default": 240, "min": 239, "max": 241},
         "w": 80, "h": 35, "hidden": True,
         "bind": "gesture_arrow_right_visible", "name": "gesture_arrow_right"},
        {"type": "image", "parent": 6, "x": 0, "y": 0, "w": 80,
         "h": 35, "image": "assets/arrow_right.png"},
        image({"default": 206, "min": 178, "max": 246},
              {"default": 280, "min": 279, "max": 281}, 69, 70,
              "hand.png", name="gesture_hand"),
        {"type": "button", "parent": 0, "x": 64, "y": 356,
         "w": 354, "h": 88, "text": "Get Started!", "font_size": 30,
         "bg_color": "#FF4C01", "fg_color": "#FCFCFF", "radius": 44,
         "callback": "welcome_exit"},
    ]


SCENES = {
    "welcome_intro": scene_intro,
    "welcome_keys": scene_keys,
    "welcome_g1": scene_g1,
    "welcome_g2": scene_g2,
    "welcome_g4": scene_g4,
}


def generate(scene_name, output, width):
    if scene_name not in SCENES:
        raise SystemExit(f"unknown Welcome scene: {scene_name}")
    here = pathlib.Path(__file__).resolve().parent
    font = str((here / WELCOME_FONT).resolve())
    scene = {
        "screen": scene_name, "w": width, "h": 480,
        "screen_bg": "#060606", "font": font,
        "default_font_size": 18, "objects": SCENES[scene_name](),
    }
    offset = (width - WELCOME_CONTENT_WIDTH) // 2
    objects = scene["objects"]
    root_index = next(i for i, obj in enumerate(objects)
                      if obj.get("parent") == -1)
    insert_at = root_index + 1
    seed_count = 1 + len(WELCOME_FONT_SIZES) + len(WELCOME_IMAGES)
    for obj in objects:
        parent = obj.get("parent")
        if isinstance(parent, int) and parent >= insert_at:
            obj["parent"] = parent + seed_count
    seed_objects = [{
        "type": "layer", "parent": root_index, "x": 0, "y": 0,
        "w": 1, "h": 1, "hidden": True, "name": "shared_font_seed",
    }]
    seed_objects.extend({
        "type": "label", "parent": insert_at, "x": 0, "y": 0,
        "w": 1, "h": 1, "text": WELCOME_GLYPH_SEED,
        "font_size": size, "fg_color": "#000000",
    } for size in WELCOME_FONT_SIZES)
    seed_objects.extend({
        "type": "image", "parent": insert_at, "x": -512, "y": 0,
        "w": image_width, "h": image_height,
        "image": str((here / "assets" / image_name).resolve()),
    } for image_name, image_width, image_height in WELCOME_IMAGES)
    seed_objects[1]["name"] = "welcome_runtime"
    seed_objects[1]["bind"] = "welcome_runtime"
    objects[insert_at:insert_at] = seed_objects

    for obj in objects:
        if obj.get("parent") == -1:
            obj["w"] = width
        elif obj.get("parent") == root_index:
            authored_x = obj.get("x", 0)
            if isinstance(authored_x, dict):
                obj["x"] = {key: value + offset
                            for key, value in authored_x.items()}
            else:
                obj["x"] = authored_x + offset
        if obj.get("font"):
            obj["font"] = font
        if obj.get("image"):
            obj["image"] = str((here / obj["image"]).resolve())
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(scene, indent=2, ensure_ascii=False) + "\n",
                      encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("usage: gen_scene.py SCENE OUTPUT WIDTH")
    generate(sys.argv[1], pathlib.Path(sys.argv[2]).resolve(), int(sys.argv[3]))
