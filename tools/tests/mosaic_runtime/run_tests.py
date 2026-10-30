#!/usr/bin/env python3
"""Check App switch timer behavior using the production runtime and catalog."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
UI = ROOT / "components/mosaic_ui"
GSP = ROOT / "managed_components/espressif__esp-gsp/include"
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"

if not (GSP / "esp_gsp.h").is_file():
    raise SystemExit("Missing managed esp-gsp headers; configure this ESP-IDF project first.")

with tempfile.TemporaryDirectory(prefix="mosaic-runtime-test-") as directory:
    binary = Path(directory) / "test_runtime"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I" + str(path) for path in
                (UI / "runtime", UI / "common", UI / "include", GSP, HOST)]
    command += [str(HERE / "test_runtime.c"), str(UI / "runtime/mosaic_runtime.c"),
                str(UI / "common/mosaic_app_catalog.c"), str(UI / "common/mosaic_system_flow.c"),
                "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
