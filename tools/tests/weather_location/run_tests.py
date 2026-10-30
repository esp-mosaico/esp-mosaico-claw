#!/usr/bin/env python3
"""Exercise production weather state, NVS precedence and HTTP replies under ASan."""
import os
from pathlib import Path
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
HOST=ROOT/'third-party/esp-claw/components/claw_modules/claw_endpoint/test_host/include'
CJSON=ROOT/'managed_components/espressif__cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='weather-location-') as directory:
    temporary=Path(directory)
    for name in ['esp_crt_bundle.h','esp_heap_caps.h','esp_http_client.h','esp_log.h',
                 'esp_random.h','esp_timer.h','freertos/task.h','gzip_miniz.h','nvs.h','wifi_manager.h']:
        path=temporary/name
        path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text('#include "platform.h"\n')
    command=[os.environ.get('CC','cc'),'-std=gnu11','-g','-Wall','-Wextra','-Werror',
             '-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter',
             '-fsanitize=address,undefined','-fno-omit-frame-pointer',
             '-DCONFIG_WEATHER_CITY_SEARCH_URL="https://test.invalid/search?name="',
             '-include',str(HOST/'host_compat.h')]
    command+=['-I'+str(p) for p in [temporary,HERE,HERE.parent/'endpoint_config/include',
              HERE.parent/'trial_provider/include',HOST,ROOT/'components/weather_service/include',CJSON]]
    command += [str(HERE/'test_weather.c'),str(CJSON/'cJSON.c'),'-lm','-o',str(temporary/'weather')]
    subprocess.run(command,check=True)
    subprocess.run([str(temporary/'weather')],check=True)
