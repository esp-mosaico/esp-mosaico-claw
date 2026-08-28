/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "mosaico_boot_handoff.h"

static const char *TAG = "MOSAICO_DISPLAY";

/*
 * Sleep-out (0x11) wait is 60 ms, matching the CO5300 driver's default.
 * Display On is issued later by board manager after the first-frame hook
 * has painted GRAM. Do not send 0x29 here.
 */
static const co5300_lcd_init_cmd_t s_vendor_init[] = {
    {0x11, NULL, 0, 60},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
#if CONFIG_DISPLAY_SERVICE_PRESENT_TE_ENABLE
    {0x35, (uint8_t[]){0x00}, 1, 0},
#endif
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
};

static const co5300_vendor_config_t s_co5300_vendor_config = {
    .init_cmds = s_vendor_init,
    .init_cmds_size = sizeof(s_vendor_init) / sizeof(s_vendor_init[0]),
    .flags = {
        .use_qspi_interface = 1,
    },
};

/* The bootloader has already sent Sleep Out and waited for the panel.  Keep
 * the remaining idempotent commands so the CO5300 driver also learns the
 * effective MADCTL/COLMOD values without paying the 60 ms delay again. */
static const co5300_lcd_init_cmd_t s_handoff_init[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
#if CONFIG_DISPLAY_SERVICE_PRESENT_TE_ENABLE
    {0x35, (uint8_t[]){0x00}, 1, 0},
#endif
    {0x53, (uint8_t[]){0x20}, 1, 0},
    /* Keep the bootloader's 0x51 brightness value during panel adoption.
     * The saved user brightness is applied after the Hub first frame. */
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x36, (uint8_t[]){0xA0}, 1, 0},
};

static const co5300_vendor_config_t s_co5300_handoff_vendor_config = {
    .init_cmds = s_handoff_init,
    .init_cmds_size = sizeof(s_handoff_init) / sizeof(s_handoff_init[0]),
    .flags = {
        .use_qspi_interface = 1,
    },
};

static esp_lcd_panel_io_handle_t s_first_frame_io;
static esp_err_t (*s_orig_reset)(esp_lcd_panel_t *panel);
static esp_err_t (*s_orig_disp_on_off)(esp_lcd_panel_t *panel, bool on_off);
static bool s_first_frame_painted;
static bool s_bootloader_panel_ready;
static bool s_skip_next_reset;

esp_err_t mosaico_lcd_prepare_first_frame(
    esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io);

static esp_err_t panel_reset_with_bootloader_handoff(esp_lcd_panel_t *panel)
{
    if (s_skip_next_reset) {
        s_skip_next_reset = false;
        ESP_LOGI(TAG, "retain bootloader panel state; skip reset");
        return ESP_OK;
    }
    return s_orig_reset != NULL ? s_orig_reset(panel) : ESP_ERR_INVALID_STATE;
}

static esp_err_t panel_disp_on_off_with_first_frame(
    esp_lcd_panel_t *panel, bool on_off)
{
    if (on_off && !s_first_frame_painted) {
        ESP_LOGI(TAG, "prepare first frame");
        esp_err_t err = mosaico_lcd_prepare_first_frame(panel, s_first_frame_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "first frame prepare failed: %s",
                     esp_err_to_name(err));
        }
        s_first_frame_painted = true;
    }
    if (on_off && s_bootloader_panel_ready) {
        /* The bootloader already issued Display On.  Avoid even a redundant
         * command during adoption, then restore normal behavior. */
        s_bootloader_panel_ready = false;
        ESP_LOGI(TAG, "bootloader splash retained during App handoff");
        return ESP_OK;
    }
    if (s_orig_disp_on_off == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_orig_disp_on_off(panel, on_off);
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    s_bootloader_panel_ready = mosaico_boot_handoff_consume();
    panel_dev_cfg.vendor_config = s_bootloader_panel_ready
        ? (void *)&s_co5300_handoff_vendor_config
        : (void *)&s_co5300_vendor_config;

    esp_err_t ret = esp_lcd_new_panel_co5300(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New CO5300 panel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(*ret_panel, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set CO5300 panel gap failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_first_frame_io = io;
    s_skip_next_reset = s_bootloader_panel_ready;
    s_first_frame_painted = s_bootloader_panel_ready;
    s_orig_reset = (*ret_panel)->reset;
    s_orig_disp_on_off = (*ret_panel)->disp_on_off;
    (*ret_panel)->reset = panel_reset_with_bootloader_handoff;
    (*ret_panel)->disp_on_off = panel_disp_on_off_with_first_frame;
    return ESP_OK;
}

esp_err_t lcd_panel_set_brightness_entry_t(
    esp_lcd_panel_handle_t panel, uint8_t percent)
{
    return esp_lcd_panel_co5300_set_brightness(panel, percent);
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_cst9217(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CST9217 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
