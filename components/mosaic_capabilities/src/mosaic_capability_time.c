/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capability_adapters.h"

#include <time.h>

#include "mosaic_capability.h"

static esp_err_t read_time(
    void *user_ctx, void *out_payload, size_t payload_size)
{
    (void)user_ctx;
    if (out_payload == NULL || payload_size != sizeof(mosaic_cap_time_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    const time_t now = time(NULL);
    if (now == (time_t)-1) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm local = {0};
    struct tm utc = {0};
    if (localtime_r(&now, &local) == NULL ||
            gmtime_r(&now, &utc) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    local.tm_isdst = -1;
    utc.tm_isdst = -1;
    const time_t local_epoch = mktime(&local);
    const time_t utc_as_local = mktime(&utc);
    if (local_epoch == (time_t)-1 || utc_as_local == (time_t)-1) {
        return ESP_ERR_INVALID_STATE;
    }
    mosaic_cap_time_t *out = out_payload;
    out->unix_seconds = (int64_t)now;
    out->utc_offset_minutes = (int32_t)((local_epoch - utc_as_local) / 60);
    return ESP_OK;
}

static const mosaic_capability_ops_t s_time_ops = {
    .read = read_time,
};

esp_err_t mosaic_capability_time_register(void)
{
    return mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "system.time",
        .ops = &s_time_ops,
    });
}
