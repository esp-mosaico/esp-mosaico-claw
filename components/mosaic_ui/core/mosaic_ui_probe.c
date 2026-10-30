/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_ui_probe.h"

#include <string.h>

#include "esp_gsp_debug.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static mosaic_ui_probe_snapshot_t s_probe;
static esp_gsp_handle_t s_ui;
static void *s_timer;
static int64_t s_last_us;
static esp_gsp_drag_snapshot_stats_t s_snapshot_before;

static void probe_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    const uint32_t generation = (uint32_t)(uintptr_t)user_ctx;
    const int64_t now_us = esp_timer_get_time();
    void *finished_timer = NULL;
    /* Component debug counters are read only here, on the render task. */
    esp_gsp_drag_snapshot_stats_t snapshots = {0};
    esp_gsp_drag_snapshot_stats(ui, &snapshots);
    const uint32_t frames = esp_gsp_frame_count(ui);

    portENTER_CRITICAL(&s_probe_lock);
    if (s_probe.generation != generation ||
            s_probe.state != MOSAIC_UI_PROBE_RUNNING || s_ui != ui) {
        portEXIT_CRITICAL(&s_probe_lock);
        return;
    }
    if (s_last_us == 0) {
        s_probe.started_us = now_us;
        s_probe.frames_before = frames;
        s_snapshot_before = snapshots;
    } else if (s_probe.sample_count < MOSAIC_UI_PROBE_CAPACITY) {
        const uint64_t gap = (uint64_t)(now_us - s_last_us);
        s_probe.gaps_us[s_probe.sample_count++] =
            gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
    } else {
        s_probe.truncated = true;
    }
    s_last_us = now_us;
    s_probe.frames_after = frames;
    s_probe.snapshot_attempts = snapshots.attempts - s_snapshot_before.attempts;
    s_probe.snapshot_started = snapshots.started - s_snapshot_before.started;
    s_probe.snapshot_reused = snapshots.reused - s_snapshot_before.reused;
    if (now_us - s_probe.started_us >= (int64_t)s_probe.duration_ms * 1000) {
        s_probe.state = MOSAIC_UI_PROBE_COMPLETE;
        s_probe.finished_us = now_us;
        finished_timer = s_timer;
        s_timer = NULL;
    }
    portEXIT_CRITICAL(&s_probe_lock);
    if (finished_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, finished_timer);
    }
}

esp_err_t mosaic_ui_probe_start(esp_gsp_handle_t ui, uint32_t duration_ms)
{
    if (ui == NULL || duration_ms < 500 || duration_ms > 4000) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_probe_lock);
    if (s_probe.state == MOSAIC_UI_PROBE_RUNNING) {
        portEXIT_CRITICAL(&s_probe_lock);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = s_probe.generation + 1U;
    if (generation == 0) generation = 1;
    memset(&s_probe, 0, sizeof(s_probe));
    s_probe.generation = generation;
    s_probe.duration_ms = duration_ms;
    s_probe.state = MOSAIC_UI_PROBE_RUNNING;
    s_last_us = 0;
    s_ui = ui;
    portEXIT_CRITICAL(&s_probe_lock);

    /* The opaque cookie is a value, not a pointer to reusable capture data.
     * A callback selected before cancellation cannot write a later capture. */
    void *timer = esp_gsp_timer_create(ui, MOSAIC_UI_PROBE_PERIOD_MS,
        probe_tick, (void *)(uintptr_t)generation);
    portENTER_CRITICAL(&s_probe_lock);
    const bool keep_timer = timer != NULL && s_probe.generation == generation &&
        s_probe.state == MOSAIC_UI_PROBE_RUNNING;
    if (keep_timer) {
        s_timer = timer;
    } else if (timer == NULL && s_probe.generation == generation) {
        s_probe.state = MOSAIC_UI_PROBE_CANCELLED;
    }
    portEXIT_CRITICAL(&s_probe_lock);
    if (timer != NULL && !keep_timer) {
        (void)esp_gsp_timer_delete(ui, timer);
    }
    return timer != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void mosaic_ui_probe_cancel(esp_gsp_handle_t ui)
{
    void *timer = NULL;
    portENTER_CRITICAL(&s_probe_lock);
    if (s_ui == ui && s_probe.state == MOSAIC_UI_PROBE_RUNNING) {
        s_probe.state = MOSAIC_UI_PROBE_CANCELLED;
        s_probe.finished_us = esp_timer_get_time();
        timer = s_timer;
        s_timer = NULL;
    }
    portEXIT_CRITICAL(&s_probe_lock);
    if (timer != NULL) {
        (void)esp_gsp_timer_delete(ui, timer);
    }
}

void mosaic_ui_probe_read(mosaic_ui_probe_snapshot_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_probe_lock);
    *out = s_probe;
    portEXIT_CRITICAL(&s_probe_lock);
}
