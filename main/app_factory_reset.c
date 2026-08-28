/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_factory_reset.h"

#include "esp_check.h"
#include "nvs.h"

#define FACTORY_RESET_NVS_NAMESPACE "factory_reset"
#define FACTORY_RESET_PENDING_KEY   "pending"

static const char *TAG = "factory_reset";

esp_err_t app_factory_reset_request(void)
{
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(
        nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
        "open reset state");

    esp_err_t err = nvs_set_u8(nvs, FACTORY_RESET_PENDING_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t app_factory_reset_is_pending(bool *ret_pending)
{
    ESP_RETURN_ON_FALSE(ret_pending != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "pending output missing");
    *ret_pending = false;

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(
        FACTORY_RESET_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open reset state");

    uint8_t pending = 0;
    err = nvs_get_u8(nvs, FACTORY_RESET_PENDING_KEY, &pending);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read reset state");
    *ret_pending = pending != 0;
    return ESP_OK;
}

esp_err_t app_factory_reset_complete(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(
        FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open reset state");

    err = nvs_erase_key(nvs, FACTORY_RESET_PENDING_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}
