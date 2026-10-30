#!/usr/bin/env python3
"""Verify product ownership around public SDK lifecycle failures."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = (ROOT / 'components/mosaic_ui/core/mosaic_esp_platform.c').read_text()


def function(name):
    match = re.search(r'^(?:static )?(?:esp_err_t|void) ' + name + r'\([^;]*?\)\n\{', SOURCE, re.M)
    if match is None:
        raise RuntimeError('Missing production function ' + name)
    depth = 1
    end = SOURCE.index('{', match.start()) + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end] + '\n'


with tempfile.TemporaryDirectory(prefix='mosaico-platform-lifecycle-') as directory:
    temporary = Path(directory)
    types = SOURCE[SOURCE.index('typedef struct {\n    struct mosaic_esp_platform_t*'):
                   SOURCE.index('static esp_err_t platform_close_app(')]
    (temporary / 'platform_types.h').write_text(types)
    names = ('app_ui_destroyed', 'resume_hub', 'platform_recover',
             'trim_camera_replacement', 'start_ui', 'platform_open_app',
             'platform_close_app', 'platform_replace_app', 'platform_dispatch_event',
             'mosaic_esp_platform_delete', 'mosaic_esp_platform_quiesce',
             'mosaic_esp_platform_activate', 'mosaic_esp_platform_pause_screen',
             'mosaic_esp_platform_resume_screen')
    (temporary / 'platform_under_test.h').write_text(
        '\n'.join(function(name) for name in names))
    for results in (False, True):
        binary = temporary / ('results' if results else 'legacy')
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1', '-Wall',
                   '-Wextra', '-Werror', '-fsanitize=address,undefined',
                   '-fno-omit-frame-pointer', '-I' + str(temporary),
                   '-DTEST_SESSION_RESULTS=' + str(int(results)),
                   str(HERE / 'test_lifecycle.c'), '-o', str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
