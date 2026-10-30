/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gsp/gsp_platform.h"

typedef struct {
    void (*cancel_single)(void *ctx);
    bool (*pinch_begin)(void *ctx, int32_t center_x, int32_t center_y);
    void (*pinch_update)(void *ctx, int32_t center_x, int32_t center_y,
                         uint32_t scale_q16);
    void (*pinch_end)(void *ctx, bool cancelled);
} gsp_multitouch_ops_t;

typedef struct {
    const gsp_multitouch_ops_t *ops;
    void *ctx;
    uint32_t initial_distance;
    bool tracking;
    bool blocked_until_clear;
    bool pass_through_until_clear;
} gsp_multitouch_t;

void gsp_multitouch_init(gsp_multitouch_t *touch,
                         const gsp_multitouch_ops_t *ops, void *ctx);
void gsp_multitouch_feed(gsp_multitouch_t *touch,
                         const gsp_platform_touch_frame_t *frame);
void gsp_multitouch_cancel(gsp_multitouch_t *touch);
