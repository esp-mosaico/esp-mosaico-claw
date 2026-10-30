#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H,
    CONTENT_W,
    asset_scene,
    auto_charset,
    build_camera_page,
    layer,
    scene_out_path,
    shared_prefix,
    shared_charset,
    write_scene,
)

FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD

ASSETS = "../../../common/assets"
CAMERA_ASSETS = "."

IMAGES = [
    (f"{CAMERA_ASSETS}/camera_canvas_fullscreen.png", 480, 480),
    (f"{CAMERA_ASSETS}/camera_album.png", 48, 48),
    (f"{CAMERA_ASSETS}/camera_shutter.png", 60, 60),
    (f"{CAMERA_ASSETS}/camera_flip.png", 48, 48),
    (f"{CAMERA_ASSETS}/camera_flash_auto.png", 48, 48),
    (f"{CAMERA_ASSETS}/camera_flash_off.png", 48, 48),
    (f"{CAMERA_ASSETS}/camera_flash_on.png", 48, 48),
    (f"{CAMERA_ASSETS}/camera_recognition.png", 48, 48),
    (f"{ASSETS}/black_placeholder.png", 48, 48),
]
FONT_POLICIES = {
    16: shared_charset(),
    18: shared_charset(),
    24: auto_charset(),
    36: auto_charset(),
}


def main():
    # Dynamic canvases must not share resource views with hidden seed images.
    canvas_images = {f"{CAMERA_ASSETS}/camera_canvas_fullscreen.png", f"{ASSETS}/black_placeholder.png"}
    seed_images = [image for image in IMAGES if image[0] not in canvas_images]
    objs, content = shared_prefix(seed_images, FONT_POLICIES, BOLD)
    page = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H, name="camera_root"))
    build_camera_page(objs, page, ASSETS, CAMERA_ASSETS)
    # Keep static pixels in Flash so capture/JPEG can use contiguous PSRAM.
    # The small thumbnail seed stays compressed to fit the existing partition.
    for obj in objs:
        if obj.get("type") == "image" and not obj.get("image", "").endswith("black_placeholder.png"):
            obj.update(codec="raw", cache_policy="mmap_direct")
    write_scene(scene_out_path(HERE, "camera_480.json"), "camera", objs, font=FONT)
    asset_scene(scene_out_path(HERE, "camera_assets_480.json"), "camera_assets", IMAGES,
                FONT_POLICIES, BOLD, FONT)


if __name__ == "__main__":
    main()
