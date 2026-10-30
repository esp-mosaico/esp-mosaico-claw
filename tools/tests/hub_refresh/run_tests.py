#!/usr/bin/env python3
"""Exercise actual Hub refresh callbacks with controlled motion and providers."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
UI = ROOT / "components/mosaic_ui"
GSP = ROOT / "managed_components/espressif__esp-gsp/include"
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"
GENERATED = ROOT / "build/esp-idf/mosaic_ui/mosaic_gen/hub"
if not (GENERATED / "mosaic_hub_objects.h").is_file():
    raise SystemExit("Build the Mosaic UI once to generate the Hub API headers.")

with tempfile.TemporaryDirectory(prefix="mosaico-hub-test-") as directory:
    binary = Path(directory) / "test_hub"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-include", str(HOST / "host_compat.h")]
    command += ["-I" + str(path) for path in
                (HERE / "include", UI / "include", UI / "common", UI / "hub", GENERATED, GSP, HOST)]
    command += [str(HERE / "test_hub_refresh.c"), str(UI / "common/mosaic_settings_model.c"),
                "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    binary = Path(directory) / "test_backdrop"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined",
               "-fno-omit-frame-pointer"]
    command += ["-I" + str(path) for path in (GENERATED, GSP, HOST)]
    command += [str(HERE / "test_backdrop.c"), "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
