#!/usr/bin/env python3
"""Check the production BMI270 complementary filter with controlled samples."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"
with tempfile.TemporaryDirectory(prefix="mosaico-imu-test-") as directory:
    binary = Path(directory) / "test_filter"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I" + str(path) for path in
                (HERE / "include", HERE.parent / "include", ROOT / "main/platform",
                 ROOT / "components/mosaic_ui/include", HOST,
                 ROOT / "third-party/esp-claw/tools/lua_lvgl_web_sim/sim/compat")]
    command += [str(HERE / "test_filter.c"), str(ROOT / "main/platform/mosaic_imu_platform.c"),
                "-lm", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
