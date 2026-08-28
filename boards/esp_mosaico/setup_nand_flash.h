/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Query capacity of a mounted Mosaico NAND LittleFS device.
 *
 * @param device_handle Handle returned by the board manager for nand_flash.
 * @param total_bytes Total filesystem capacity.
 * @param free_bytes Currently available filesystem capacity.
 */
esp_err_t mosaico_nand_flash_get_space(void *device_handle,
                                       uint64_t *total_bytes,
                                       uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif
