/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_display_present.h"
#include "esp_err.h"

/** Partition drawbuf height configured in display_service_target_build(). */
#define DISPLAY_SERVICE_PRESENT_DRAWBUF_LINES 10
// #define DISPLAY_SERVICE_PRESENT_DRAWBUF_LINES 30

esp_err_t display_service_present_buffer_lines(
    esp_display_presenter_t *presenter,
    uint32_t *out_lines);

size_t display_service_present_pixel_bytes(
    esp_display_present_pixel_format_t format);

esp_err_t display_service_present_acquire_region(
    esp_display_presenter_t *presenter,
    const esp_display_presenter_caps_t *caps,
    const esp_display_present_area_t *area,
    esp_display_presenter_buffer_t *out_buffer,
    esp_display_presenter_region_t *out_region);

esp_err_t display_service_present_submit_region(
    esp_display_presenter_t *presenter,
    const esp_display_presenter_buffer_t *buffer,
    const esp_display_presenter_region_t *region);
