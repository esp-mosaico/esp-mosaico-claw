#!/usr/bin/env python3
"""Exercise Camera workspace admission, save failures and retirement barriers."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = (ROOT / 'components/mosaic_ui/core/mosaic_camera.c').read_text()


def function(name, source=SOURCE):
    match = re.search(r'^(?:static )?(?:esp_err_t|void|bool) ' + name + r'\([^;]*?\)\n\{', source, re.M)
    if match is None:
        raise RuntimeError('Missing production function ' + name)
    depth = 1
    end = source.index('{', match.start()) + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


with tempfile.TemporaryDirectory(prefix='mosaico-camera-session-') as directory:
    temporary = Path(directory)
    camera_header = (ROOT / 'components/mosaic_ui/core/mosaic_camera.h').read_text()
    status_type = camera_header[camera_header.index('typedef struct {'):
                                camera_header.index('} mosaic_camera_capture_status_t;') +
                                len('} mosaic_camera_capture_status_t;')]
    (temporary / 'camera_status_type.h').write_text(status_type)
    types = SOURCE[SOURCE.index('typedef enum {\n    MOSAIC_FRAME_FREE'):
                   SOURCE.index('static bool camera_is_stopping(')]
    (temporary / 'camera_types.h').write_text(types)
    (temporary / 'camera_under_test.h').write_text('\n'.join(function(name) for name in (
        'camera_is_stopping', 'camera_clear_preview_ready',
        'camera_ensure_preview_buffers',
        'camera_free_working_buffers', 'camera_retire_working_buffers',
        'frame_released', 'frame_finish_write', 'camera_submit_preview',
        'camera_stop_registered_stream', 'mosaic_camera_get_capture_status',
        'camera_save_jpeg', 'camera_save_frame', 'mosaic_camera_stop')))
    binary = temporary / 'test_session'
    command = [os.environ.get('CC', 'cc'), '-std=c11', '-D_GNU_SOURCE', '-g',
               '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I' + str(temporary),
               str(HERE / 'test_session.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary), str(temporary)], check=True)

    app_source = (ROOT / 'components/mosaic_ui/apps/camera/camera_app.c').read_text()
    status_functions = '\n'.join(function(name, app_source) for name in (
        'camera_status_text', 'camera_notice', 'camera_render_recognition',
        'camera_capture_status_tick'))
    binds = sorted(set(re.findall(r'GSP_BIND_\w+', status_functions)))
    (temporary / 'camera_status_type.h').write_text(status_type)
    (temporary / 'camera_status_functions.h').write_text(
        'enum {' + ','.join(binds) + '};\n' + status_functions)
    binary = temporary / 'test_status'
    command = [os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1', '-Wall',
               '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I' + str(temporary),
               str(HERE / 'test_status.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
    (temporary / 'camera_start_under_test.h').write_text('\n'.join(
        function(name, app_source) for name in ('camera_start_deferred', 'camera_retry_deferred')))
    binary = temporary / 'test_start'
    command[-3:] = [str(HERE / 'test_start.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
