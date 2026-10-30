#!/usr/bin/env python3
"""Check App switch timer behavior using the production runtime and catalog."""
import os
from pathlib import Path
import subprocess
import tempfile
import re

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

    loader = (UI / 'core/mosaic_loader.c').read_text()
    types = loader[loader.index('typedef struct {\n    mosaic_loader_deferred_cb_t callback;'):
                   loader.index('} mosaic_loader_deferred_t;') + len('} mosaic_loader_deferred_t;')]
    capacity = re.search(r'#define MOSAIC_LOADER_MAX_DEFERRED (\d+U)', loader).group(1)
    def function(name):
        match = re.search(r'^(?:static )?(?:void|esp_err_t) ' + name + r'\([^;]*?\)\n\{', loader, re.M)
        assert match, name
        end = loader.index('{', match.start()) + 1
        depth = 1
        while depth:
            depth += (loader[end] == '{') - (loader[end] == '}')
            end += 1
        return loader[match.start():end]
    temporary = Path(directory)
    (temporary / 'loader_deferred_type.h').write_text(
        '#define MOSAIC_LOADER_MAX_DEFERRED ' + capacity + '\n' + types)
    (temporary / 'loader_deferred_functions.h').write_text(
        function('mosaic_loader_defer') + '\n' + function('process_deferred'))
    binary = temporary / 'test_deferred'
    command = [os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1', '-Wall',
               '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I' + str(temporary),
               str(HERE / 'test_deferred.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)

    begin = loader.index('    if (command->type == MOSAIC_LOADER_COMMAND_BACK) {')
    end = loader.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (loader[end] == '{') - (loader[end] == '}')
        end += 1
    (temporary / 'loader_back_under_test.h').write_text(
        'static void process_back_under_test(void) {\n' + loader[begin:end] + '\n}\n')
    binary = temporary / 'test_loader_back'
    command[-3:] = [str(HERE / 'test_loader_back.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
