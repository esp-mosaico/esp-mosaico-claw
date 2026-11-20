/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "bsp/nand_flash.h"
#include "esp_blockdev.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "gen_board_device_custom.h"
#include "setup_nand_flash.h"

static const char *TAG = "MOSAICO_NAND";

#define NAND_FLASH_CLOCK_SPEED_HZ  (40 * 1000 * 1000)
#define NAND_FLASH_MAX_TRANSFER_SZ (4 * 1024)
#define NAND_FLASH_QUEUE_SIZE      10
#define NAND_FLASH_GC_FACTOR       0
#define NAND_FLASH_MOUNT_PATH      "/nand"

typedef struct {
    esp_blockdev_handle_t blockdev;
    bool mounted;
} nand_flash_handle_t;

static int nand_flash_init(void *config, int cfg_size, void **device_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && device_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid NAND flash config");
    ESP_RETURN_ON_FALSE(cfg_size == sizeof(dev_custom_nand_flash_config_t), ESP_ERR_INVALID_SIZE, TAG, "invalid NAND flash config size: %d", cfg_size);

    *device_handle = NULL;
    const dev_custom_nand_flash_config_t *cfg = (const dev_custom_nand_flash_config_t *)config;
    nand_flash_handle_t *handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG, "failed to allocate NAND flash handle");

    const bsp_nand_flash_config_t nand_config = {
        .clock_speed_hz = NAND_FLASH_CLOCK_SPEED_HZ,
        .queue_size = NAND_FLASH_QUEUE_SIZE,
        .max_transfer_sz = NAND_FLASH_MAX_TRANSFER_SZ,
        .gc_factor = NAND_FLASH_GC_FACTOR,
        .io_mode = SPI_NAND_IO_MODE_QIO,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .poll_short_commands = true,
    };
    ESP_LOGI(TAG, "Initializing NAND through BSP, data=%d MHz", NAND_FLASH_CLOCK_SPEED_HZ / 1000000);
    esp_err_t ret = bsp_nand_flash_init_blockdev(&nand_config, &handle->blockdev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize NAND BSP block device: %s", esp_err_to_name(ret));
        free(handle);
        return ret;
    }

    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = NAND_FLASH_MOUNT_PATH,
        .blockdev = handle->blockdev,
        // Recover from an unreadable or incompatible filesystem by recreating it.
        // This intentionally discards the existing NAND contents on mount failure.
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    ret = esp_vfs_littlefs_register(&mount_config);
    if (ret != ESP_OK) {
        // esp_vfs_littlefs_register() consumes and releases the block device on failure.
        handle->blockdev = NULL;
        ESP_LOGE(TAG, "failed to mount NAND LittleFS at %s: %s", NAND_FLASH_MOUNT_PATH, esp_err_to_name(ret));
        free(handle);
        return ret;
    }
    handle->mounted = true;

    ESP_LOGI(TAG, "NAND LittleFS mounted at %s, cfg_chip_size=%" PRIu32, NAND_FLASH_MOUNT_PATH, (uint32_t)cfg->chip_size);
    *device_handle = handle;
    return ESP_OK;
}

static int nand_flash_deinit(void *device_handle)
{
    nand_flash_handle_t *handle = (nand_flash_handle_t *)device_handle;
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid NAND flash handle");

    if (handle->mounted) {
        esp_err_t ret = esp_vfs_littlefs_unregister_blockdev(handle->blockdev);
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to unmount NAND LittleFS: %s", esp_err_to_name(ret));
        handle->mounted = false;
        handle->blockdev = NULL;
    }

    free(handle);
    ESP_LOGI(TAG, "NAND flash deinitialized");
    return ESP_OK;
}

esp_err_t mosaico_nand_flash_get_space(void *device_handle, uint64_t *total_bytes, uint64_t *free_bytes)
{
    nand_flash_handle_t *handle = (nand_flash_handle_t *)device_handle;
    ESP_RETURN_ON_FALSE(handle && handle->mounted && handle->blockdev, ESP_ERR_INVALID_STATE, TAG, "NAND LittleFS is not mounted");
    ESP_RETURN_ON_FALSE(total_bytes && free_bytes, ESP_ERR_INVALID_ARG, TAG, "space output is NULL");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_littlefs_blockdev_info(handle->blockdev, &total, &used), TAG, "failed to query NAND LittleFS info");
    *total_bytes = total;
    *free_bytes = total >= used ? total - used : 0;
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(nand_flash, nand_flash_init, nand_flash_deinit);
