/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

/* Bounded runtime chrome draw-list over caller-owned storage, without
 * allocation. Owned batches follow their authored viewport's paint order;
 * unowned commands remain above the scene and template instances. Typical
 * commands are selection rectangles, cursors, scroll indicators and waveforms.
 *
 * Commands use profile-native colors and screen coordinates; every
 * mutation marks the affected area dirty and bumps the runtime revision
 * (so it must not run while a prepared frame is rendering).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_types.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GSP_OVERLAY_FILL_RECT = 1,
    GSP_OVERLAY_FILL_ROUND_RECT = 2,
    GSP_OVERLAY_GLYPH_A8 = 3,
} gsp_overlay_opcode_t;

typedef struct {
    gsp_rect_t bbox;
    uint32_t color;         /*!< Profile-native RGB565/RGB888 color */
    uint16_t radius;
    uint16_t stroke_width;  /*!< 0 = filled */
    uint8_t stroke_mode;    /*!< gsp_gsb stroke mode enum */
    uint8_t opacity;
    uint8_t opcode;         /*!< gsp_overlay_opcode_t */
    uint8_t reserved;
    const uint8_t *a8;
    size_t a8_size;
    size_t a8_stride;
} gsp_overlay_command_t;

#define GSP_OVERLAY_FLAG_ANCHORED 0x1U
/* Anchors use opcode-specific spare fields, preserving caller-owned storage. */
static inline uint32_t gsp_overlay_paint_after(const gsp_overlay_command_t *command)
{
    if ((command->reserved & GSP_OVERLAY_FLAG_ANCHORED) == 0) {
        return UINT32_MAX;
    }
    return command->opcode == GSP_OVERLAY_GLYPH_A8 ?
           (uint32_t)command->radius | ((uint32_t)command->stroke_width << 16) :
           (uint32_t)command->a8_size;
}

typedef struct gsp_overlay {
    gsp_runtime_t *runtime;
    gsp_overlay_command_t *commands;
    size_t capacity;
    size_t count;
} gsp_overlay_t;

static inline size_t gsp_overlay_owned_count(const gsp_overlay_t *overlay)
{
    size_t count = 0;
    while (overlay != NULL && count < overlay->count &&
            (overlay->commands[count].reserved & GSP_OVERLAY_FLAG_ANCHORED) != 0) {
        ++count;
    }
    return count;
}

/**
 * Attaches a draw-list over caller-owned storage to the runtime. Only
 * one overlay can be attached. Unowned commands render in push order above
 * scene commands and template instances; owned batches use paint boundaries.
 */
gsp_err_t gsp_overlay_init(gsp_overlay_t *overlay, gsp_runtime_t *runtime,
                           gsp_overlay_command_t *storage,
                           size_t capacity);

/** Detaches the overlay and marks its area dirty. */
void gsp_overlay_deinit(gsp_overlay_t *overlay);

/** Removes every command (previous bboxes go dirty). */
gsp_err_t gsp_overlay_clear(gsp_overlay_t *overlay);

/** Attach a newly contributed batch to its canonical DRAW_INSTANCES instruction. */
gsp_err_t gsp_overlay_set_paint_after(gsp_overlay_t *overlay, size_t first,
                                      uint32_t command_boundary);

gsp_err_t gsp_overlay_push_fill_rect(gsp_overlay_t *overlay,
                                     gsp_rect_t bbox, uint32_t color,
                                     uint8_t opacity);

gsp_err_t gsp_overlay_push_round_rect(gsp_overlay_t *overlay,
                                      gsp_rect_t bbox, uint32_t color,
                                      uint16_t radius,
                                      uint16_t stroke_width,
                                      uint8_t stroke_mode,
                                      uint8_t opacity);

gsp_err_t gsp_overlay_push_glyph_a8(gsp_overlay_t *overlay,
                                    gsp_rect_t bbox, uint32_t color,
                                    const uint8_t *a8, size_t a8_size,
                                    size_t a8_stride);

#ifdef __cplusplus
}
#endif
