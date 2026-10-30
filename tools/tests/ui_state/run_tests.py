#!/usr/bin/env python3
"""Exercise actual Battery/Album/Weather presentation with controlled GSP sinks."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
UI = ROOT / "components/mosaic_ui"
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"
with tempfile.TemporaryDirectory(prefix="mosaico-ui-state-") as directory:
    for name, app in (("battery", "settings"), ("album", "album"), ("weather", "weather")):
        binary = Path(directory) / name
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
                   # Keep unrelated descriptors out of the exercised callback graph.
                   "-fsanitize=address,undefined", "--param=asan-globals=0", "-fno-omit-frame-pointer",
                   "-include", str(HOST / "host_compat.h")]
        if name == "weather":
            command += ["-DESP_PLATFORM", "-I" + str(ROOT / "components/weather_service/include")]
        command += ["-I" + str(path) for path in
                    (HERE / "include", HERE.parent / "ui_probe/include", UI / "include", UI / "common",
                     UI / "common/qrcodegen", UI / "runtime",
                     ROOT / f"build/esp-idf/mosaic_ui/mosaic_gen/{app}",
                     ROOT / "build/esp-idf/mosaic_ui/mosaic_gen/hub",
                     ROOT / "managed_components/espressif__esp-gsp/include", HOST)]
        command += [str(HERE / f"test_{name}.c"), "-Wl,--gc-sections", "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    scene = json.loads((UI / "apps/album/scene/album_480.json").read_text())
    objects = scene["objects"]
    assert not any(str(obj.get("text", "")).strip().lower() == "info" for obj in objects)
    delete = next(obj for obj in objects if obj.get("name") == "album_select_delete_enabled")
    assert delete.get("hidden") is True
    print("PASS current Album scene has no Info action and defaults Delete to hidden")
