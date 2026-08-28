/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_service_present.h"

#include <string.h>

#include "esp_check.h"

esp_err_t display_service_present_buffer_lines(
    esp_display_presenter_t *presenter,
    uint32_t *out_lines)
{
    esp_display_presenter_caps_t caps = {0};

    ESP_RETURN_ON_FALSE(presenter && out_lines, ESP_ERR_INVALID_ARG,
                        "display_present", "buffer lines args missing");
    ESP_RETURN_ON_ERROR(
        esp_display_presenter_get_caps(presenter, &caps),
        "display_present", "get presenter caps");

    if (caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_PARTITION) {
        ESP_RETURN_ON_FALSE(
            caps.stride_bytes > 0 && caps.drawbuf_bytes > 0,
            ESP_ERR_INVALID_STATE, "display_present", "partition drawbuf missing");
        *out_lines = (uint32_t)(caps.drawbuf_bytes / caps.stride_bytes);
    } else if (caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL) {
        *out_lines = caps.height;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_FALSE(*out_lines > 0, ESP_ERR_INVALID_STATE,
                        "display_present", "invalid buffer lines");
    return ESP_OK;
}

size_t display_service_present_pixel_bytes(
    esp_display_present_pixel_format_t format)
{
    return format == ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565 ? 2U : 3U;
}

esp_err_t display_service_present_acquire_region(
    esp_display_presenter_t *presenter,
    const esp_display_presenter_caps_t *caps,
    const esp_display_present_area_t *area,
    esp_display_presenter_buffer_t *out_buffer,
    esp_display_presenter_region_t *out_region)
{
    ESP_RETURN_ON_FALSE(
        presenter && caps && area && out_buffer && out_region,
        ESP_ERR_INVALID_ARG, "display_present", "acquire region args missing");

    esp_err_t ret = esp_display_presenter_acquire_buffer(presenter, out_buffer);
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t color_bytes = display_service_present_pixel_bytes(caps->pixel_format);
    const size_t width = (size_t)(area->x2 - area->x1 + 1);
    const size_t requested_rows = (size_t)(area->y2 - area->y1 + 1);
    const size_t stride = caps->contract == ESP_DISPLAY_PRESENT_CONTRACT_PARTITION
                          ? width * color_bytes
                          : out_buffer->surface.stride_bytes;
    size_t rows = 0;

    ret = out_buffer->resolve_rows != NULL
          ? out_buffer->resolve_rows(
              out_buffer->resolve_rows_ctx, out_buffer->lease_id,
              stride, requested_rows, &rows)
          : ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK || rows == 0 || rows > UINT16_MAX || width > UINT16_MAX) {
        /*
         * Leave frame cancellation to the caller: every call site cancels the
         * frame on a non-OK return, so cancelling here as well would double
         * cancel. Just clear the output so the caller cannot misuse it.
         */
        memset(out_buffer, 0, sizeof(*out_buffer));
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *pixels = out_buffer->surface.pixels;
    if (caps->contract != ESP_DISPLAY_PRESENT_CONTRACT_PARTITION) {
        pixels += (size_t)area->y1 * stride + (size_t)area->x1 * color_bytes;
    }

    *out_region = (esp_display_presenter_region_t) {
        .surface = {
            .pixels = pixels,
            .stride_bytes = stride,
            .width = (uint16_t)width,
            .height = (uint16_t)rows,
            .pixel_format = out_buffer->surface.pixel_format,
        },
        .origin_x = (uint16_t)area->x1,
        .origin_y = (uint16_t)area->y1,
    };
    return ESP_OK;
}

esp_err_t display_service_present_submit_region(
    esp_display_presenter_t *presenter,
    const esp_display_presenter_buffer_t *buffer,
    const esp_display_presenter_region_t *region)
{
    const esp_display_present_area_t area = {
        .x1 = region->origin_x,
        .y1 = region->origin_y,
        .x2 = (int32_t)region->origin_x + region->surface.width - 1,
        .y2 = (int32_t)region->origin_y + region->surface.height - 1,
    };
    return esp_display_presenter_submit_buffer(
               presenter, buffer, &area, region->surface.stride_bytes);
}
