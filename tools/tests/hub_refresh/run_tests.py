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
BUILD = Path(os.environ.get("MOSAICO_TEST_BUILD_DIR", ROOT / "build"))
GENERATED = BUILD / "esp-idf/mosaic_ui/mosaic_gen/hub"
if not (GENERATED / "mosaic_hub_objects.h").is_file():
    raise SystemExit("Build the Mosaic UI once to generate the Hub API headers.")

with tempfile.TemporaryDirectory(prefix="mosaico-hub-test-") as directory:
    binary = Path(directory) / "test_hub"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-unused-function", "-ffunction-sections", "-fdata-sections",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-include", str(HOST / "host_compat.h")]
    command += ["-I" + str(path) for path in
                (HERE / "include", HERE.parent / "ui_probe/include", UI / "include", UI / "common",
                 UI / "hub", GENERATED, GSP, HOST,
                 ROOT / "managed_components/mosaico_module_mgr/include")]
    command += [str(HERE / "test_hub_refresh.c"), str(UI / "common/mosaic_settings_model.c"),
                str(UI / "hub/mosaic_module_notice.c"),
                "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

    binary = Path(directory) / "test_module_notice"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I" + str(path) for path in
                (UI / "hub", ROOT / "managed_components/mosaico_module_mgr/include", HOST)]
    command += [str(HERE / "test_module_notice.c"), str(UI / "hub/mosaic_module_notice.c"),
                "-o", str(binary)]
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
