/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GSP_SCROLL_PHYS_SAMPLES ESP_GSP_BUILD_CAP_SCROLL_PHYS_SAMPLES

typedef struct {
    uint32_t velocity_window_us;
    uint32_t release_grace_us;
    int32_t stop_speed_px_s;
    int32_t min_fling_speed_px_s;
    int32_t max_release_speed_px_s;
    uint32_t decay_us;
    uint32_t max_tick_us;
    uint32_t snap_response_us;
} gsp_scroll_phys_policy_t;

/**
 * Vertical scroll physics: finger drags with edge clamp, released drags
 * coast from recent-weighted timestamped motion samples with smooth velocity
 * decay, and snap viewports ease onto a row boundary after coasting. The
 * caller republishes the integer pixel offset and owns the timestamped frame
 * tick.
 */
typedef struct {
    int32_t offset;
    int32_t max_offset;
    uint16_t row_height;        /*!< row pitch for snapping */
    bool snap;                  /*!< ease onto a row boundary at rest */
    bool coasting;              /*!< keep ticking while true */
    int32_t velocity;           /*!< signed content velocity in px/s */
    int64_t position_q16;       /*!< sub-pixel coast position */
    int64_t last_tick_us;
    int32_t sample_positions[GSP_SCROLL_PHYS_SAMPLES];
    int64_t sample_times_us[GSP_SCROLL_PHYS_SAMPLES];
    uint8_t sample_count;
    gsp_scroll_phys_policy_t policy;
} gsp_scroll_phys_t;

void gsp_scroll_phys_configure(gsp_scroll_phys_t *phys,
                               const gsp_scroll_phys_policy_t *policy);

/** Begin a newly claimed drag and cancel any previous coast. */
void gsp_scroll_phys_press(gsp_scroll_phys_t *phys, int64_t now_us);

/** Account for signed finger motion consumed by gesture arbitration.
 *  This changes release velocity sampling without moving the content. */
void gsp_scroll_phys_claim(gsp_scroll_phys_t *phys,
                           int32_t consumed_delta_y);

/** Finger drag step: content follows the finger (offset -= delta_y),
 *  clamped to [0, max_offset]; records a timestamped fling sample. */
void gsp_scroll_phys_drag(gsp_scroll_phys_t *phys, int32_t delta_y,
                          int64_t now_us);

/** Release: derives px/s coast velocity from recent timed motion. */
void gsp_scroll_phys_release(gsp_scroll_phys_t *phys, int64_t now_us);

/** Starts a programmatic fling at velocity_px_per_s (sign: content
 *  direction, matching gsp_scroll_phys_drag). */
void gsp_scroll_phys_fling(gsp_scroll_phys_t *phys,
                           int32_t velocity_px_per_s, int64_t now_us);

/** Timestamped coast step. True when the published pixel offset moved. */
bool gsp_scroll_phys_tick(gsp_scroll_phys_t *phys, int64_t now_us);

#ifdef __cplusplus
}
#endif
