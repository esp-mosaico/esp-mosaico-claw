#!/usr/bin/env python3
"""Exercise production Setup scan recovery against asynchronous cached providers."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
UI = ROOT / "components/mosaic_ui"
BUILD = Path(os.environ.get("MOSAICO_TEST_BUILD_DIR", ROOT / "build"))
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"
with tempfile.TemporaryDirectory(prefix="mosaico-setup-test-") as directory:
    binary = Path(directory) / "test_scan"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
               # Do not keep the unrelated App descriptor/callback graph alive
               # through GCC's ASan global registration. Stack/heap ASan and
               # UBSan still instrument the exercised production helpers.
               "-fsanitize=address,undefined", "--param=asan-globals=0",
               "-fno-omit-frame-pointer", "-include", str(HOST / "host_compat.h")]
    command += ["-I" + str(path) for path in
                (UI / "include", UI / "common", UI / "common/qrcodegen", UI / "runtime",
                 BUILD / "esp-idf/mosaic_ui/mosaic_gen/setup_center",
                 BUILD / "esp-idf/mosaic_ui/mosaic_gen/hub",
                 ROOT / "managed_components/espressif__esp-gsp/include", HOST)]
    command += [str(HERE / "test_scan.c"), "-Wl,--gc-sections", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
