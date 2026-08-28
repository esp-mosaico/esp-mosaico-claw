/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_service_target.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "devices/dev_lcd_touch/dev_lcd_touch.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "display_service_present.h"
#include "esp_log.h"

static const char *TAG = "display_target";

#define DISPLAY_SERVICE_TARGET_BUFFER_COUNT 2

esp_err_t display_service_target_load_display(
    dev_display_lcd_config_t **out_lcd_cfg,
    dev_display_lcd_handles_t **out_lcd_handles)
{
#if !CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    return ESP_ERR_NOT_SUPPORTED;
#else
    void *lcd_handle = NULL;
    void *lcd_config = NULL;
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle), TAG,
        "get display_lcd handle failed");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(
        ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_config), TAG,
        "get display_lcd config failed");
    dev_display_lcd_handles_t *lcd_handles = lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = lcd_config;
    ESP_RETURN_ON_FALSE(lcd_handles && lcd_cfg && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG,
                        "display_lcd handle/config invalid");
    if (out_lcd_cfg) *out_lcd_cfg = lcd_cfg;
    if (out_lcd_handles) *out_lcd_handles = lcd_handles;
    return ESP_OK;
#endif
}

esp_lcd_touch_handle_t display_service_target_load_touch(void)
{
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    void *touch_handle = NULL;
    if (esp_board_manager_get_device_handle(
            ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle) != ESP_OK) {
        ESP_LOGI(TAG, "touch disabled: handle not found");
        return NULL;
    }
    dev_lcd_touch_handles_t *touch_handles = touch_handle;
    return touch_handles ? touch_handles->touch_handle : NULL;
#else
    ESP_LOGI(TAG, "touch disabled: board touch support off");
    return NULL;
#endif
}

esp_err_t display_service_target_build(
    const dev_display_lcd_config_t *lcd_cfg,
    const dev_display_lcd_handles_t *lcd_handles,
    esp_display_present_target_config_t *out_target)
{
    ESP_RETURN_ON_FALSE(lcd_cfg && lcd_handles && out_target,
                        ESP_ERR_INVALID_ARG, TAG,
                        "present target arguments missing");
    const char *sub_type = lcd_cfg->sub_type ? lcd_cfg->sub_type : "";
    const bool is_rgb = strcmp(sub_type, "rgb") == 0 ||
                        strcmp(sub_type, "rgb_3wire_spi") == 0;
    ESP_RETURN_ON_FALSE(is_rgb || lcd_handles->io_handle != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "panel IO missing");

    uint8_t num_fbs = 0;
    esp_display_present_mode_t mode = ESP_DISPLAY_PRESENT_MODE_AUTO;
    if (is_rgb) {
        if (strcmp(sub_type, "rgb_3wire_spi") == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
            num_fbs =
                (uint8_t)lcd_cfg->sub_cfg.rgb_3wire_spi.rgb_panel_config.num_fbs;
#endif
        } else {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
            num_fbs = (uint8_t)lcd_cfg->sub_cfg.rgb.panel_config.num_fbs;
#endif
        }
        if (num_fbs >= 3) {
            mode = ESP_DISPLAY_PRESENT_MODE_TRIPLE_PARTIAL;
        } else if (num_fbs == 2) {
            /* Two FB RGB panels use partition+repair into panel framebuffers
             * (CONTRACT_PARTITION), not full-frame fb_switch (CONTRACT_FULL). */
            mode = ESP_DISPLAY_PRESENT_MODE_DOUBLE_PARTIAL;
        } else {
            ESP_LOGE(TAG, "RGB presenter requires at least two framebuffers");
            return ESP_ERR_INVALID_STATE;
        }
    }

    bool te_enabled = false;
    esp_display_present_te_sync_config_t te_sync =
        ESP_DISPLAY_PRESENT_TE_SYNC_DISABLED();
#if CONFIG_DISPLAY_SERVICE_PRESENT_TE_ENABLE
    if (!is_rgb) {
        te_enabled = true;
        mode = ESP_DISPLAY_PRESENT_MODE_TE_SYNC;
        te_sync = (esp_display_present_te_sync_config_t) {
            .gpio_num = CONFIG_DISPLAY_SERVICE_PRESENT_TE_GPIO,
            .bus_freq_hz = CONFIG_DISPLAY_SERVICE_PRESENT_TE_BUS_FREQ_HZ,
            .data_lines = CONFIG_DISPLAY_SERVICE_PRESENT_TE_DATA_LINES,
        };
    }
#endif

    *out_target = (esp_display_present_target_config_t) {
        .hw = {
            .panel = lcd_handles->panel_handle,
            .io = lcd_handles->io_handle,
            .panel_type = is_rgb ? ESP_DISPLAY_PRESENT_PANEL_RGB
                                 : ESP_DISPLAY_PRESENT_PANEL_IO,
            .input_pixel_format = lcd_cfg->bits_per_pixel == 24
                ? ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB888
                : ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565,
            .rotation = ESP_DISPLAY_PRESENT_ROTATE_0,
            .swap_bytes = !is_rgb &&
                lcd_cfg->data_endian == LCD_RGB_DATA_ENDIAN_BIG,
            .te_enabled = te_enabled,
            .te_sync = te_sync,
        },
        .fb = {
            .mode = mode,
        },
    };

    const char *staging;
    out_target->drawbuf.lines = DISPLAY_SERVICE_PRESENT_DRAWBUF_LINES;
    out_target->drawbuf.in_psram = true;
    uint8_t staging_buffers = 1;
    if (is_rgb) {
        staging = "fb-partition";
    } else if (te_enabled) {
        staging = "gram-te";
    } else {
        staging = "gram";
        staging_buffers = DISPLAY_SERVICE_TARGET_BUFFER_COUNT;
    }
    ESP_LOGI(TAG,
             "panel=%s mode=%d te=%d gpio=%d bus=%" PRIu32
             " data_lines=%u staging=%s lines=%u buffers=%u",
             sub_type, (int)mode, (int)te_enabled, te_sync.gpio_num,
             te_sync.bus_freq_hz, (unsigned)te_sync.data_lines, staging,
             (unsigned)DISPLAY_SERVICE_PRESENT_DRAWBUF_LINES,
             (unsigned)staging_buffers);
    return ESP_OK;
}
