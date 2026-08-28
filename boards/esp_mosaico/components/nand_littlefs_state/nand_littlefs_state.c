/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nand_littlefs_state.h"
#include "esp_check.h"
#include "nvs.h"

#define NAND_LITTLEFS_NVS_NAMESPACE          "mosaico_nand"
#define NAND_LITTLEFS_NVS_FORMAT_VERSION_KEY "lfs_fmt_v1"
#define NAND_LITTLEFS_FORMAT_VERSION          1

static const char *TAG = "NAND_LFS_STATE";

esp_err_t nand_littlefs_state_is_migration_complete(bool *migration_complete)
{
    ESP_RETURN_ON_FALSE(migration_complete != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid migration state output");

    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(NAND_LITTLEFS_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *migration_complete = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to open migration state: %s", esp_err_to_name(ret));

    uint8_t format_version = 0;
    ret = nvs_get_u8(nvs, NAND_LITTLEFS_NVS_FORMAT_VERSION_KEY, &format_version);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        *migration_complete = false;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to read migration state: %s", esp_err_to_name(ret));

    *migration_complete = format_version >= NAND_LITTLEFS_FORMAT_VERSION;
    return ESP_OK;
}

esp_err_t nand_littlefs_state_mark_migration_complete(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(NAND_LITTLEFS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to open migration state for update: %s", esp_err_to_name(ret));

    ret = nvs_set_u8(nvs, NAND_LITTLEFS_NVS_FORMAT_VERSION_KEY, NAND_LITTLEFS_FORMAT_VERSION);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}
