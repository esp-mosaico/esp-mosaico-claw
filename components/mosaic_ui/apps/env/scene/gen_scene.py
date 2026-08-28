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
    explicit_charset,
    shared_charset,
    build_env_page,
    layer,
    shared_prefix,
    scene_out_path,
    write_scene,
)

FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD
ASSETS = "../../../common/assets"

IMAGES = [
    (f"{ASSETS}/icons/env_flame.png", 24, 24),
    (f"{ASSETS}/icons/env_scale.png", 210, 14),
]
FONT_POLICIES = {
    16: shared_charset(),
    18: shared_charset(),
    75: explicit_charset("-+.0123456789"),
}


def main():
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD)
    page = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H, name="env_root"))
    build_env_page(objs, page, ASSETS)
    write_scene(scene_out_path(HERE, "env_480.json"), "env", objs, font=FONT)
    asset_scene(scene_out_path(HERE, "env_assets_480.json"), "env_assets", IMAGES,
                FONT_POLICIES, BOLD, FONT)


if __name__ == "__main__":
    main()
