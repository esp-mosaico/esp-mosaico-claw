/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nand_littlefs_state_is_migration_complete(bool *migration_complete);
esp_err_t nand_littlefs_state_mark_migration_complete(void);

#ifdef __cplusplus
}
#endif
