/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gsp/gsp_renderer.h"
#include "gsp/gsp_transition_compose.h"
#include "gsp/gsp_types.h"

typedef gsp_err_t (*gsp_visual_capture_cb_t)(
    void *ctx, int32_t offset, gsp_surface_t *surface);

struct gsp_repeater;

/**
 * Portable description of a frozen-frame visual transition.  Controllers
 * choose the source; the display implementation owns capture, composition,
 * presentation and teardown.
 */
typedef enum {
    GSP_VISUAL_SOURCE_SCENE = 0,
    GSP_VISUAL_SOURCE_VIEWPORT_GROUP = 1,
    GSP_VISUAL_SOURCE_CAPTURE = 2,
} gsp_visual_source_kind_t;

typedef struct {
    gsp_visual_source_kind_t source;
    gsp_transition_kind_t kind;
    union {
        struct {
            uint16_t target_scene;
            /** Slide-fade only; zero selects the 50% default. */
            uint8_t fade_black_point_percent;
            /** Finger-driven transitions prioritize immediate first motion. */
            bool interactive;
        } scene;
        struct {
            uint16_t group_id;
            gsp_rect_t viewport;
            int32_t from_offset;
            int32_t target_offset;
            bool vertical;
            bool overlay;
        } viewport;
        struct {
            gsp_rect_t viewport;
            int32_t from_offset;
            int32_t target_offset;
            gsp_visual_capture_cb_t capture;
            void *capture_ctx;
            /* Retain endpoints after end. Reuse also requires a matching
             * owner/generation/window and explicit controller permission. */
            bool preserve_endpoints;
            bool reuse_endpoints;
            bool replay_overlay;
            uint32_t cache_generation;
            uint32_t paint_after;
            gsp_rect_t live_region;
            const struct gsp_repeater *repeater;
        } capture;
    };
} gsp_visual_transition_request_t;
