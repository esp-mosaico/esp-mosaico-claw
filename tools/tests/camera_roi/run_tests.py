#!/usr/bin/env python3
"""Exercise the real ROI implementation with resolved sensor tables and I/O faults."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SENSOR = ROOT / 'managed_components/espressif__esp_cam_sensor/sensors/sc101iot'
HEADERS = {
    'esp_err.h': '''#pragma once
 typedef int esp_err_t;
 enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=0x103 };
''',
    'mosaico_module_camera.h': '''#pragma once
 #include <stdbool.h>
 #include <stdint.h>
 #include "esp_err.h"
 typedef void *mosaico_camera_handle_t;
 typedef struct { uint8_t buffer_count, outstanding_count; bool streaming, power_down; } mosaico_camera_pipeline_stats_t;
 esp_err_t mosaico_camera_get_pipeline_stats(mosaico_camera_handle_t, mosaico_camera_pipeline_stats_t *);
 esp_err_t mosaico_camera_set_power_down(mosaico_camera_handle_t, bool);
''',
    'esp_cam_sensor_types.h': '''#pragma once
 #include <stdint.h>
 enum { ESP_CAM_SENSOR_PIXFORMAT_YUV422_UYVY=1, ESP_CAM_SENSOR_DVP=2, ESP_CAM_SENSOR_IOC_G_CHIP_ID=3 };
 typedef struct { uint16_t pid; } esp_cam_sensor_id_t;
 typedef struct { const char *name; int format, port, xclk; uint16_t width,height; const void *regs; int regs_size; uint8_t fps; const void *isp_info; void *reserved; } esp_cam_sensor_format_t;
''',
    'esp_log.h': '''#pragma once
 #include <stdio.h>
 #define ESP_LOGI(tag,format,...) do { (void)(tag); if (0) fprintf(stderr,format,##__VA_ARGS__); } while(0)
 #define ESP_LOGW ESP_LOGI
''',
    'esp_video_device.h': '#pragma once\n#define ESP_VIDEO_DVP_DEVICE_NAME "/dev/video2"\n',
    'esp_video_ioctl.h': '''#pragma once
 #include <stdint.h>
 enum { VIDIOC_G_EXT_CTRLS=100, VIDIOC_G_SENSOR_FMT, VIDIOC_S_SENSOR_FMT, V4L2_CTRL_CLASS_ESP_CAM_IOCTL };
 struct v4l2_ext_control { unsigned id,size; uint8_t *p_u8; };
 struct v4l2_ext_controls { unsigned ctrl_class,count; struct v4l2_ext_control *controls; };
''',
}
with tempfile.TemporaryDirectory(prefix='mosaico-camera-roi-') as directory:
    temporary = Path(directory)
    for name, content in HEADERS.items():
        (temporary / name).write_text(content)
    fixtures = '\n'.join('#include "' + str(SENSOR / 'private_include' / name) + '"' for name in [
        'sc101iot_dvp_8bit_20Minput_1280x720_yuv422_uyvy_15fps.h',
        'sc101iot_dvp_8bit_20Minput_1280x720_yuv422_uyvy_25fps.h'])
    (temporary / 'sensor_fixtures.h').write_text(fixtures)
    binary = temporary / 'camera_roi'
    source = ROOT / 'components/mosaico_camera_service/mosaico_camera_roi.c'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-g', '-O1',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I'+str(temporary), '-I'+str(SENSOR/'include'),
                    '-DROI_SOURCE_FILE="'+str(source)+'"',
                    str(Path(__file__).with_name('test_roi.c')), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, env=os.environ | {
        'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1',
        'UBSAN_OPTIONS':'halt_on_error=1'})

# Exercise the real capability gate before the unrelated archive-patching rules.
with tempfile.TemporaryDirectory(prefix='mosaico-roi-sdk-gate-') as directory:
    temporary = Path(directory)
    rules = (ROOT/'components/mosaic_ui/common/mosaic_gsp_product_fixes.cmake').read_text()
    prefix = rules.split('get_target_property(mosaic_gsp_archive', 1)[0]
    (temporary/'gate.cmake').write_text(prefix)
    (temporary/'dummy.c').write_text('void dummy(void) {}\n')
    sdk = temporary/'sdk/include'
    sdk.mkdir(parents=True)
    for case, enabled, trim, result, expected in [
        ('legacy-default', False, False, False, True),
        ('legacy-roi', True, False, False, False),
        ('trim-only-roi', True, True, False, False),
        ('matched-roi', True, True, True, True),
    ]:
        header = ''
        if trim:
            header += 'esp_err_t esp_gsp_esp_lcd_trim_session(\n'
            header += 'esp_err_t esp_gsp_esp_lcd_trim_paused(\n'
        if result:
            header += '#define ESP_GSP_ESP_LCD_SESSION_RESULT_INIT() {}\n'
        (sdk/'esp_gsp_esp_lcd.h').write_text(header)
        (temporary/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(roi_gate C)
add_library(fake_ui STATIC dummy.c)
set(COMPONENT_LIB fake_ui)
set(CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI {'ON' if enabled else 'OFF'})
set_property(GLOBAL PROPERTY ESP_GSP_COMPONENT_LIB fake_gsp)
set_property(GLOBAL PROPERTY ESP_GSP_COMPONENT_DIR "{temporary/'sdk'}")
include("{temporary/'gate.cmake'}")
''')
        completed = subprocess.run(['cmake','-S',str(temporary),'-B',str(temporary/case)],
                                   capture_output=True, text=True)
        assert (completed.returncode == 0) == expected, completed.stdout+completed.stderr
        if not expected:
            assert 'Equivalent Camera ROI requires' in completed.stdout+completed.stderr
    print('PASS SDK gate: legacy defaults build, incompatible ROI rejected, matched capabilities admitted')
