/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_ui_probe.h"
#include "esp_gsp_debug.h"

#include <assert.h>
#include <stdio.h>

static int64_t now_us;
static esp_gsp_timer_cb_t callback;
static void *cookie;
static unsigned deletes;
static bool fail_create;
static bool finish_during_create;
static uint32_t frames;
static esp_gsp_drag_snapshot_stats_t counters;
static const esp_gsp_handle_t ui = (esp_gsp_handle_t)(uintptr_t)1;

int64_t esp_timer_get_time(void) { return now_us; }
uint32_t esp_gsp_frame_count(esp_gsp_handle_t g) { (void)g; return frames; }
void esp_gsp_drag_snapshot_stats(esp_gsp_handle_t g, esp_gsp_drag_snapshot_stats_t *out)
{
    (void)g;
    *out = counters;
}
void *esp_gsp_timer_create(esp_gsp_handle_t g, uint32_t period,
                          esp_gsp_timer_cb_t cb, void *ctx)
{
    assert(g == ui && period == 16);
    callback = cb;
    cookie = ctx;
    if (fail_create) return NULL;
    if (finish_during_create) {
        now_us = 1000;
        callback(ui, cookie);
        now_us = 600000;
        callback(ui, cookie);
    }
    return (void *)(uintptr_t)1;
}
esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t g, void *timer)
{
    assert(g == ui && timer == (void *)(uintptr_t)1);
    ++deletes;
    return ESP_GSP_OK;
}

static void tick(int64_t time, uint32_t frame)
{
    now_us = time;
    frames = frame;
    callback(ui, cookie);
}

int main(void)
{
    mosaic_ui_probe_snapshot_t snapshot;
    assert(mosaic_ui_probe_start(NULL, 500) == ESP_ERR_INVALID_ARG);
    assert(mosaic_ui_probe_start(ui, 499) == ESP_ERR_INVALID_ARG);
    assert(mosaic_ui_probe_start(ui, 4001) == ESP_ERR_INVALID_ARG);
    assert(mosaic_ui_probe_start(ui, 500) == ESP_OK);
    assert(mosaic_ui_probe_start(ui, 500) == ESP_ERR_INVALID_STATE);
    counters.started = 2;
    tick(16000, 10);
    tick(32000, 11);
    tick(96000, 12);
    counters.started = 5;
    tick(516000, 20);
    mosaic_ui_probe_read(&snapshot);
    assert(snapshot.state == MOSAIC_UI_PROBE_COMPLETE && deletes == 1);
    assert(snapshot.sample_count == 3 && snapshot.gaps_us[0] == 16000);
    assert(snapshot.gaps_us[1] == 64000 && snapshot.gaps_us[2] == 420000);
    assert(snapshot.frames_after - snapshot.frames_before == 10);
    assert(snapshot.snapshot_started == 3);
    puts("PASS measured gaps, completion and snapshot deltas");

    assert(mosaic_ui_probe_start(ui, 500) == ESP_OK);
    esp_gsp_timer_cb_t stale_callback = callback;
    void *stale_cookie = cookie;
    mosaic_ui_probe_cancel(ui);
    assert(deletes == 2);
    assert(mosaic_ui_probe_start(ui, 500) == ESP_OK);
    stale_callback(ui, stale_cookie);
    mosaic_ui_probe_read(&snapshot);
    assert(snapshot.state == MOSAIC_UI_PROBE_RUNNING && snapshot.started_us == 0);
    assert(snapshot.sample_count == 0);
    tick(1000000, 30);
    mosaic_ui_probe_cancel(ui);
    mosaic_ui_probe_read(&snapshot);
    assert(snapshot.state == MOSAIC_UI_PROBE_CANCELLED && deletes == 3);
    puts("PASS stale callbacks cannot modify a later capture");

    fail_create = true;
    assert(mosaic_ui_probe_start(ui, 500) == ESP_ERR_NO_MEM);
    mosaic_ui_probe_read(&snapshot);
    assert(snapshot.state == MOSAIC_UI_PROBE_CANCELLED);
    fail_create = false;
    finish_during_create = true;
    assert(mosaic_ui_probe_start(ui, 500) == ESP_OK);
    mosaic_ui_probe_read(&snapshot);
    assert(snapshot.state == MOSAIC_UI_PROBE_COMPLETE && deletes == 4);
    puts("PASS creation failure and early completion release timer ownership");
    return 0;
}
