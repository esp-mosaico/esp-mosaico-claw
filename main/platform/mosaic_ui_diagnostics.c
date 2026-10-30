/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_ui_diagnostics.h"

#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE && CONFIG_APP_CLAW_ENABLE_CLI
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_console.h"
#include "mosaic_loader.h"
#include "mosaic_ui_probe.h"

static int compare_gap(const void *left, const void *right)
{
    const uint32_t a = *(const uint32_t *)left;
    const uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static int ui_perf(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "start") == 0) {
        char *end = NULL;
        errno = 0;
        const unsigned long duration = strtoul(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || duration > 4000 || duration < 500) {
            printf("Usage: ui_perf start <500..4000 ms> | ui_perf read\n");
            return 1;
        }
        const esp_err_t err = mosaic_loader_profile_active((uint32_t)duration);
        printf("{\"started\":%s,\"error\":%d}\n", err == ESP_OK ? "true" : "false", err);
        return err == ESP_OK ? 0 : 1;
    }
    if (argc != 2 || strcmp(argv[1], "read") != 0) {
        printf("Usage: ui_perf start <500..4000 ms> | ui_perf read\n");
        return 1;
    }
    mosaic_ui_probe_snapshot_t snapshot;
    mosaic_ui_probe_read(&snapshot);
    static const char *const states[] = { "idle", "running", "complete", "cancelled" };
    cJSON *root = cJSON_CreateObject();
    cJSON *samples = cJSON_CreateArray();
    if (root == NULL || samples == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(samples);
        return 1;
    }
    cJSON_AddStringToObject(root, "state", states[snapshot.state]);
    cJSON_AddStringToObject(root, "measurement", "render_task_service_gap");
    cJSON_AddNumberToObject(root, "probe_period_ms", MOSAIC_UI_PROBE_PERIOD_MS);
    cJSON_AddNumberToObject(root, "generation", snapshot.generation);
    cJSON_AddNumberToObject(root, "duration_ms", snapshot.duration_ms);
    cJSON_AddNumberToObject(root, "sample_count", snapshot.sample_count);
    cJSON_AddBoolToObject(root, "truncated", snapshot.truncated);
    cJSON_AddNumberToObject(root, "frames_including_snapshots", snapshot.frames_after - snapshot.frames_before);
    cJSON_AddNumberToObject(root, "snapshot_attempts", snapshot.snapshot_attempts);
    cJSON_AddNumberToObject(root, "snapshot_started", snapshot.snapshot_started);
    cJSON_AddNumberToObject(root, "snapshot_reused", snapshot.snapshot_reused);
    for (uint32_t i = 0; i < snapshot.sample_count; ++i) {
        cJSON_AddItemToArray(samples, cJSON_CreateNumber(snapshot.gaps_us[i]));
    }
    cJSON_AddItemToObject(root, "gaps_us", samples);
    qsort(snapshot.gaps_us, snapshot.sample_count, sizeof(snapshot.gaps_us[0]), compare_gap);
    if (snapshot.sample_count != 0) {
        const uint32_t count = snapshot.sample_count;
        cJSON_AddNumberToObject(root, "p50_us", snapshot.gaps_us[(count * 50U + 99U) / 100U - 1U]);
        cJSON_AddNumberToObject(root, "p95_us", snapshot.gaps_us[(count * 95U + 99U) / 100U - 1U]);
        cJSON_AddNumberToObject(root, "max_us", snapshot.gaps_us[count - 1U]);
    }
    char *json = cJSON_PrintUnformatted(root);
    const bool printed = json != NULL;
    if (json != NULL) printf("%s\n", json);
    cJSON_free(json);
    cJSON_Delete(root);
    return printed ? 0 : 1;
}

esp_err_t mosaic_ui_diagnostics_register(void)
{
    const esp_console_cmd_t command = {
        .command = "ui_perf",
        .help = "Measure active App render-task service gaps; not physical LCD frame latency",
        .func = ui_perf,
    };
    return esp_console_cmd_register(&command);
}
#else
esp_err_t mosaic_ui_diagnostics_register(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
