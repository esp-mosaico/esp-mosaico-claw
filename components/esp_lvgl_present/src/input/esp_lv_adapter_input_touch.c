/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter_input.h"
#include "adapter_internal.h"

static const char *TAG = "esp_lvgl:touch";

#define DEFAULT_SCALE_FACTOR 1.0f

typedef struct {
    esp_lcd_touch_handle_t handle;
    struct {
        float x;
        float y;
    } scale;
    esp_lv_adapter_touch_callbacks_t callbacks;
    lv_point_t last_point;
    lv_indev_state_t last_state;
} esp_lv_adapter_touch_ctx_t;

static void lvgl_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lv_adapter_touch_ctx_t *touch_ctx = lv_indev_get_driver_data(indev);
    if (!touch_ctx || !touch_ctx->handle) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t touch_data[1] = {0};
    uint8_t count = 0;
    esp_err_t ret;

    if (touch_ctx->callbacks.custom_touch_read) {
        ret = touch_ctx->callbacks.custom_touch_read(touch_ctx->handle, touch_data, &count,
                                                     1, touch_ctx->callbacks.user_ctx);
    } else {
        esp_lcd_touch_read_data(touch_ctx->handle);
        ret = esp_lcd_touch_get_data(touch_ctx->handle, touch_data, &count, 1);
    }

    if (ret == ESP_OK && count > 0) {
        touch_ctx->last_point.x = (lv_coord_t)(touch_ctx->scale.x * touch_data[0].x);
        touch_ctx->last_point.y = (lv_coord_t)(touch_ctx->scale.y * touch_data[0].y);
        touch_ctx->last_state = LV_INDEV_STATE_PRESSED;
    } else {
        touch_ctx->last_state = LV_INDEV_STATE_RELEASED;
    }

    data->point = touch_ctx->last_point;
    data->state = touch_ctx->last_state;
}

lv_indev_t *esp_lv_adapter_register_touch(const esp_lv_adapter_touch_config_t *config)
{
    if (!config || !config->disp || !config->handle) {
        ESP_LOGE(TAG, "invalid touch configuration");
        return NULL;
    }

    esp_lv_adapter_touch_ctx_t *touch_ctx = calloc(1, sizeof(*touch_ctx));
    if (!touch_ctx) {
        ESP_LOGE(TAG, "failed to allocate touch context");
        return NULL;
    }

    touch_ctx->handle = config->handle;
    touch_ctx->scale.x = config->scale.x ? config->scale.x : DEFAULT_SCALE_FACTOR;
    touch_ctx->scale.y = config->scale.y ? config->scale.y : DEFAULT_SCALE_FACTOR;
    touch_ctx->callbacks = config->callbacks;
    touch_ctx->last_state = LV_INDEV_STATE_RELEASED;

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        ESP_LOGE(TAG, "failed to acquire LVGL lock");
        free(touch_ctx);
        return NULL;
    }

    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        ESP_LOGE(TAG, "failed to create LVGL input device");
        esp_lv_adapter_unlock();
        free(touch_ctx);
        return NULL;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvgl_touch_read);
    lv_indev_set_disp(indev, config->disp);
    lv_indev_set_driver_data(indev, touch_ctx);
    esp_lv_adapter_unlock();

    esp_err_t ret = esp_lv_adapter_register_input_device(indev, ESP_LV_ADAPTER_INPUT_TYPE_TOUCH, touch_ctx);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "failed to track touch input device: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Touch input device registered successfully");
    return indev;
}

esp_err_t esp_lv_adapter_unregister_touch(lv_indev_t *touch)
{
    ESP_RETURN_ON_FALSE(touch, ESP_ERR_INVALID_ARG, TAG, "invalid touch handle");

    esp_lv_adapter_touch_ctx_t *touch_ctx = lv_indev_get_driver_data(touch);
    ESP_RETURN_ON_FALSE(touch_ctx, ESP_ERR_INVALID_STATE, TAG, "touch context missing");

    esp_lv_adapter_unregister_input_device(touch);

    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(-1), TAG, "failed to acquire LVGL lock");
    lv_indev_delete(touch);
    esp_lv_adapter_unlock();

    free(touch_ctx);
    ESP_LOGI(TAG, "touch input unregistered");
    return ESP_OK;
}
