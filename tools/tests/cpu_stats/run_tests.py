#!/usr/bin/env python3
"""Check the production CPU counter normalization independently of ESP-IDF."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
with tempfile.TemporaryDirectory(prefix="mosaico-cpu-test-") as directory:
    binary = Path(directory) / "test_cpu"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I" + str(ROOT / "components/cap_system/src"),
                    str(HERE / "test_cpu.c"), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
