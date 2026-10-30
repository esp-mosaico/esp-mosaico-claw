/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Value-drag behavior (sliders, arc dials): maps a captured pointer
 * position to 0..100 along the control's track. Pure geometry — the
 * caller owns the state transaction. The configuration is decoded
 * from the VALUE action's packed arg (gspc lowers orientation and the
 * dial range there) and the hit bbox.
 */
typedef struct {
    bool vertical;              /*!< linear: track runs bottom-up */
    bool polar;                 /*!< arc dial: angle -> value */
    uint16_t start_deg;         /*!< dial start, cw from 12 o'clock */
    uint16_t sweep_deg;         /*!< dial sweep, degrees */
    uint16_t bind;              /*!< I32 bind receiving the value */
    uint16_t property;          /*!< direct property, UINT16_MAX if 1.0 */
    gsp_rect_t track;           /*!< control bbox (hit bbox) */
} gsp_value_drag_t;

/** Decodes the compiler-packed action arg (bit 0 vertical, bit 1
 *  polar, bits 2..10 start, bits 11..19 sweep). */
void gsp_value_drag_init(gsp_value_drag_t *drag, uint32_t action_arg,
                         uint16_t target_bind, uint16_t target_property,
                         gsp_rect_t track);

/**
 * Maps (x, y) to 0..100. Linear tracks clamp to the span; dials snap
 * positions outside the sweep to the nearer end. False when the
 * geometry is degenerate (empty span / zero sweep) — publish nothing.
 */
bool gsp_value_drag_value(const gsp_value_drag_t *drag, int32_t x,
                          int32_t y, int32_t *out_value);

#ifdef __cplusplus
}
#endif
