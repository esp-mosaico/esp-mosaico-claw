/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_display_present_types.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LV_ADAPTER_ROTATE_0   = 0,
    ESP_LV_ADAPTER_ROTATE_90  = 90,
    ESP_LV_ADAPTER_ROTATE_180 = 180,
    ESP_LV_ADAPTER_ROTATE_270 = 270,
} esp_lv_adapter_rotation_t;

typedef struct {
    esp_lv_adapter_rotation_t rotation;
    uint16_t hor_res;
    uint16_t ver_res;
    uint16_t buffer_height;
    bool use_psram;
} esp_lv_adapter_display_profile_t;

typedef struct {
    esp_lv_adapter_display_profile_t profile;
} esp_lv_adapter_display_config_t;

#define ESP_LV_ADAPTER_DISPLAY_PROFILE(_hor_res, _ver_res, _rotation, _buffer_height, _use_psram) \
    ((esp_lv_adapter_display_config_t){                                                          \
        .profile = {                                                                             \
            .rotation = (_rotation),                                                             \
            .hor_res = (_hor_res),                                                               \
            .ver_res = (_ver_res),                                                               \
            .buffer_height = (_buffer_height),                                                   \
            .use_psram = (_use_psram),                                                           \
        },                                                                                         \
    })

#define ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(_hor_res, _ver_res, _rotation) \
    ESP_LV_ADAPTER_DISPLAY_PROFILE(_hor_res, _ver_res, _rotation, 50, false)

#define ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(_hor_res, _ver_res, _rotation) \
    ESP_LV_ADAPTER_DISPLAY_PROFILE(_hor_res, _ver_res, _rotation, 50, false)

#define ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(_hor_res, _ver_res, _rotation) \
    ESP_LV_ADAPTER_DISPLAY_PROFILE(_hor_res, _ver_res, _rotation, _ver_res, true)

#define ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(_hor_res, _ver_res, _rotation) \
    ESP_LV_ADAPTER_DISPLAY_PROFILE(_hor_res, _ver_res, _rotation, 10, false)

struct esp_display_presenter;

typedef struct {
    struct esp_display_presenter *presenter;
    uint32_t producer_generation;
    esp_display_present_render_alignment_t render_alignment;
    bool (*validate_generation)(void *user_ctx, uint32_t generation);
    void *validation_user_ctx;
} esp_lv_adapter_presenter_config_t;

lv_display_t *esp_lv_adapter_register_display_with_presenter(
    const esp_lv_adapter_display_config_t *config,
    const esp_lv_adapter_presenter_config_t *presenter_config);

esp_err_t esp_lv_adapter_unregister_display(lv_display_t *disp);

#ifdef __cplusplus
}
#endif
