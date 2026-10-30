#!/usr/bin/env python3
"""Check capture lifetime and bounded sampling using the production probe."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CORE = ROOT / "components/mosaic_ui/core"
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"
GSP = ROOT / "managed_components/espressif__esp-gsp/include"
with tempfile.TemporaryDirectory(prefix="mosaico-probe-test-") as directory:
    binary = Path(directory) / "test_probe"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I" + str(path) for path in (HERE / "include", CORE, HOST, GSP)]
    command += [str(HERE / "test_probe.c"), str(CORE / "mosaic_ui_probe.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
