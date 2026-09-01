# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS
        "src/mosaic_capability_registry.c"
        "src/mosaic_capability_decls.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        freertos
)
