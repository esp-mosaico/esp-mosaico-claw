# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS
        "src/mosaic_capability_registry.c"
        "src/mosaic_capability_decls.c"
        "src/mosaic_capability_builtin.c"
        "src/mosaic_capability_battery.c"
        "src/mosaic_capability_status.c"
        "src/mosaic_capability_time.c"
        "src/mosaic_capability_imu.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        freertos
)
