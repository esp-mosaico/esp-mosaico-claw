/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_gsp.h"

#define MOSAIC_UI_PROBE_CAPACITY 256U
#define MOSAIC_UI_PROBE_PERIOD_MS 16U

typedef enum {
    MOSAIC_UI_PROBE_IDLE = 0,
    MOSAIC_UI_PROBE_RUNNING,
    MOSAIC_UI_PROBE_COMPLETE,
    MOSAIC_UI_PROBE_CANCELLED,
} mosaic_ui_probe_state_t;

typedef struct {
    mosaic_ui_probe_state_t state;
    uint32_t generation;
    uint32_t duration_ms;
    uint32_t sample_count;
    bool truncated;
    int64_t started_us;
    int64_t finished_us;
    uint32_t frames_before;
    uint32_t frames_after;
    uint32_t snapshot_attempts;
    uint32_t snapshot_started;
    uint32_t snapshot_reused;
    uint32_t gaps_us[MOSAIC_UI_PROBE_CAPACITY];
} mosaic_ui_probe_snapshot_t;

/** Called with the runtime lifecycle serialized. The timer creates no damage. */
esp_err_t mosaic_ui_probe_start(esp_gsp_handle_t ui, uint32_t duration_ms);
/** Cancel before the profiled UI is stopped or suspended. */
void mosaic_ui_probe_cancel(esp_gsp_handle_t ui);
/** Copy bounded diagnostics; no GSP handle is read from the calling task. */
void mosaic_ui_probe_read(mosaic_ui_probe_snapshot_t *out);
