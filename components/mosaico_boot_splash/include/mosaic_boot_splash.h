/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Paint the boot logo into panel GRAM before the first Display On.
 * The board LCD factory calls this from a wrapped disp_on_off().
 */
esp_err_t mosaico_lcd_prepare_first_frame(
    esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io);

#ifdef __cplusplus
}
#endif
