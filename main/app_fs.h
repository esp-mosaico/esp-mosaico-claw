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
 * @brief  Active base path of writable storage.
 *
 *         Always the Mosaico NAND LittleFS mount point. SD cards are separate
 *         filesystems and never replace the application DATA root. Valid only
 *         after app_fs_init() has run.
 *
 * @return Mount-point string owned by this module (never NULL).
 */
const char *app_fs_storage_base_path(void);

/**
 * @brief  Base path of the read-only system filesystem.
 *
 *         Holds firmware-baked content (skills, built-in Lua scripts/docs and
 *         the recovery seed files). Valid only after app_fs_init() has run.
 *
 * @return Mount-point string owned by this module (never NULL).
 */
const char *app_fs_system_base_path(void);

/**
 * @brief  Query the active writable filesystem capacity.
 *
 *         The ctx argument is unused and allows direct registration as a
 *         claw_path_space_provider_t callback.
 */
esp_err_t app_fs_get_storage_space(void *ctx,
                                   uint64_t *total_bytes,
                                   uint64_t *free_bytes);

/**
 * @brief Remove all writable NAND data and restore firmware recovery seeds.
 *
 *         Call during boot, after app_fs_init() and before any service starts
 *         using the DATA filesystem. The NAND mount point itself is retained.
 */
esp_err_t app_fs_factory_reset(void);

/**
 * @brief Wait for boot-time recovery of missing DATA files to finish.
 *
 * @return Recovery result. A partial recovery is reported without unmounting
 *         the writable filesystem.
 */
esp_err_t app_fs_wait_recovery(void);

/**
 * @brief  Initialize all filesystems.
 *
 *         Must be called after esp_board_manager_init() since it relies on the
 *         board manager to mount the mandatory NAND device. The system
 *         partition is mounted first, followed by NAND DATA and RAMFS. DATA
 *         recovery starts in the background and must be joined with
 *         app_fs_wait_recovery() before services consume recovery seeds.
 *
 * @return
 *       - ESP_OK  On success
 *       - Others  Underlying initialization error
 */
esp_err_t app_fs_init(void);


#ifdef __cplusplus
}
#endif
