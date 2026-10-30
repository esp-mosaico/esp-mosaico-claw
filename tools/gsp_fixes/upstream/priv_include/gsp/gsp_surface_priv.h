/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_types.h"

typedef struct gsp_surface {
    void *pixels;
    size_t stride_bytes;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_format;
    uint16_t origin_x;
    uint16_t origin_y;
} gsp_surface_t;

static inline gsp_rect_t gsp_surface_bounds(const gsp_surface_t *surface)
{
    gsp_rect_t bounds;
    bounds.x1 = surface->origin_x;
    bounds.y1 = surface->origin_y;
    bounds.x2 = (int32_t)surface->origin_x + surface->width;
    bounds.y2 = (int32_t)surface->origin_y + surface->height;
    return bounds;
}
