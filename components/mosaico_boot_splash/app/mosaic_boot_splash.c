/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_boot_splash.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "mosaic_boot_logo.h"

#define MOSAIC_BOOT_LCD_WIDTH 480U
#define MOSAIC_BOOT_LCD_HEIGHT 480U
#define MOSAIC_BOOT_STRIP_LINES 40U
#define MOSAIC_BOOT_BYTES_PER_PIXEL 2U
#define MOSAIC_BOOT_TRANSFER_TIMEOUT_MS 1000U

static const char *TAG = "mosaic_boot";

static bool IRAM_ATTR splash_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel_io;
    (void)event_data;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &wake);
    return wake == pdTRUE;
}

static esp_err_t splash_draw_wait(
    esp_lcd_panel_handle_t panel, SemaphoreHandle_t done,
    int x, int y, int width, int height, const void *pixels)
{
    while (xSemaphoreTake(done, 0) == pdTRUE) {
    }
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_draw_bitmap(panel, x, y, x + width, y + height, pixels),
        TAG, "draw boot splash");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(done, pdMS_TO_TICKS(MOSAIC_BOOT_TRANSFER_TIMEOUT_MS)) ==
            pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "boot splash transfer timeout");
    return ESP_OK;
}

static void write_be565(uint8_t *dst, uint16_t color)
{
    dst[0] = (uint8_t)(color >> 8);
    dst[1] = (uint8_t)color;
}

esp_err_t __attribute__((used)) mosaico_lcd_prepare_first_frame(
    esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io)
{
    ESP_RETURN_ON_FALSE(panel != NULL && io != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "boot LCD handles missing");

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(done != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate boot transfer semaphore");
    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = splash_transfer_done,
    };
    esp_err_t err = esp_lcd_panel_io_register_event_callbacks(
        io, &callbacks, done);
    if (err != ESP_OK) {
        vSemaphoreDelete(done);
        return err;
    }

    const size_t strip_bytes = MOSAIC_BOOT_LCD_WIDTH *
        MOSAIC_BOOT_STRIP_LINES * MOSAIC_BOOT_BYTES_PER_PIXEL;
    uint8_t *strip = heap_caps_malloc(
        strip_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (strip == NULL) {
        const esp_lcd_panel_io_callbacks_t empty = {0};
        (void)esp_lcd_panel_io_register_event_callbacks(io, &empty, NULL);
        vSemaphoreDelete(done);
        return ESP_ERR_NO_MEM;
    }
    memset(strip, 0, strip_bytes);

    for (unsigned y = 0; y < MOSAIC_BOOT_LCD_HEIGHT && err == ESP_OK;
         y += MOSAIC_BOOT_STRIP_LINES) {
        const unsigned lines =
            y + MOSAIC_BOOT_STRIP_LINES <= MOSAIC_BOOT_LCD_HEIGHT
                ? MOSAIC_BOOT_STRIP_LINES
                : MOSAIC_BOOT_LCD_HEIGHT - y;
        err = splash_draw_wait(panel, done, 0, (int)y, MOSAIC_BOOT_LCD_WIDTH,
                               lines, strip);
    }

    const int logo_x = (MOSAIC_BOOT_LCD_WIDTH - MOSAIC_BOOT_LOGO_WIDTH) / 2;
    const int logo_y = (MOSAIC_BOOT_LCD_HEIGHT - MOSAIC_BOOT_LOGO_HEIGHT) / 2;
    size_t run_index = 0;
    uint16_t run_left = s_mosaic_boot_logo_runs[0].count;
    uint16_t run_color = s_mosaic_boot_logo_runs[0].color;
    for (unsigned row = 0; row < MOSAIC_BOOT_LOGO_HEIGHT && err == ESP_OK;) {
        const unsigned lines =
            row + MOSAIC_BOOT_STRIP_LINES <= MOSAIC_BOOT_LOGO_HEIGHT
                ? MOSAIC_BOOT_STRIP_LINES
                : MOSAIC_BOOT_LOGO_HEIGHT - row;
        const size_t pixels = MOSAIC_BOOT_LOGO_WIDTH * lines;
        for (size_t pixel = 0; pixel < pixels; ++pixel) {
            write_be565(&strip[pixel * MOSAIC_BOOT_BYTES_PER_PIXEL], run_color);
            if (--run_left == 0U && ++run_index < MOSAIC_BOOT_LOGO_RUN_COUNT) {
                run_left = s_mosaic_boot_logo_runs[run_index].count;
                run_color = s_mosaic_boot_logo_runs[run_index].color;
            }
        }
        err = splash_draw_wait(panel, done, logo_x, logo_y + (int)row,
                               MOSAIC_BOOT_LOGO_WIDTH, lines, strip);
        row += lines;
    }

    const esp_lcd_panel_io_callbacks_t empty = {0};
    (void)esp_lcd_panel_io_register_event_callbacks(io, &empty, NULL);
    heap_caps_free(strip);
    vSemaphoreDelete(done);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "early boot logo visible");
    }
    return err;
}
