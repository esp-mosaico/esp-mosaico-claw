#!/usr/bin/env python3
"""Check shutdown pulses and screen-power recovery with controlled failures."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PLATFORM = ROOT / "main/platform"
HOST = ROOT / "third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include"

with tempfile.TemporaryDirectory(prefix="mosaico-power-test-") as directory:
    for supported in (True, False):
        binary = Path(directory) / ("power" if supported else "unsupported")
        command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   f"-DCONFIG_ESP_BOARD_ESP_MOSAICO={int(supported)}"]
        command += ["-I" + str(path) for path in (HERE / "include", PLATFORM, HOST)]
        command += [str(HERE / "test_power.c"), str(PLATFORM / "mosaic_power_platform.c"),
                    "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

    temporary = Path(directory)
    ui_source = (ROOT / "components/mosaic_ui/mosaic_ui.c").read_text()
    functions = ui_source[ui_source.index("static void screen_apply_wake("):
                          ui_source.index("void mosaic_ui_note_screen_activity(")]
    (temporary / "screen_power_under_test.h").write_text(functions)
    binary = temporary / "screen-power"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-variable", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(temporary), "-I" + str(HOST),
               str(HERE / "test_screen_power.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

    loader_source = (ROOT / "components/mosaic_ui/core/mosaic_loader.c").read_text()
    function = loader_source[loader_source.index("esp_err_t mosaic_loader_pause_screen("):
                             loader_source.index("esp_err_t mosaic_loader_resume_screen(")]
    (temporary / "loader_pause_under_test.h").write_text(function)
    binary = temporary / "loader-pause"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-I" + str(temporary), "-I" + str(HOST),
               str(HERE / "test_loader_pause.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
