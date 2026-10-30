#!/usr/bin/env python3
"""Validate the board charge-profile transaction against a faultable gauge model."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
BOARD = ROOT / "boards/esp_mosaico"
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"
with tempfile.TemporaryDirectory(prefix="mosaico-charge-test-") as directory:
    binary = Path(directory) / "test_charge"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += ["-I" + str(path) for path in (HERE / "include", BOARD, HOST)]
    command += [str(HERE / "test_charge.c"), str(BOARD / "battery_charge_config.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
