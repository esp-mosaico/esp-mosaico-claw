/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* net.weather provider.
 *
 * Bridges the weather service's snapshot and subscription onto the
 * capability surface so that Apps never link against the HTTP client or the
 * MET Norway payload shape.
 */

#include "mosaic_net_weather.h"

#include <string.h>

#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"
#include "weather_service.h"

/* The two forecast horizons must agree or the copy below would silently
 * truncate. */
_Static_assert(WEATHER_SERVICE_FORECAST_DAYS ==
    MOSAIC_CAP_WEATHER_FORECAST_DAYS,
    "net.weather contract must cover the whole service forecast");

static bool s_registered;

static void weather_to_payload(const weather_service_snapshot_t *in,
                               mosaic_cap_weather_t *out)
{
    memset(out, 0, sizeof(*out));
    out->online = in->online;
    out->location_valid = in->location_valid;
    out->weather_valid = in->weather_valid;
    out->stale = in->stale;
    out->temperature_c = in->temperature_c;
    strlcpy(out->city, in->city, sizeof(out->city));
    strlcpy(out->symbol_code, in->symbol_code, sizeof(out->symbol_code));
    strlcpy(out->condition, in->condition, sizeof(out->condition));
    out->last_updated_epoch = in->last_updated_epoch;
    out->sequence = in->sequence;
    for (size_t day = 0; day < MOSAIC_CAP_WEATHER_FORECAST_DAYS; ++day) {
        const weather_service_day_t *src = &in->forecast[day];
        mosaic_cap_weather_day_t *dst = &out->forecast[day];
        dst->valid = src->valid;
        dst->low_valid = src->temperature_sample_count >= 2U &&
            src->temperature_min_c != src->temperature_max_c;
        dst->temperature_min_c = src->temperature_min_c;
        dst->temperature_max_c = src->temperature_max_c;
        strlcpy(dst->date, src->date, sizeof(dst->date));
        strlcpy(dst->symbol_code, src->symbol_code, sizeof(dst->symbol_code));
        strlcpy(dst->condition, src->condition, sizeof(dst->condition));
    }
}

/* Runs on the weather service's worker task. */
static void on_weather_snapshot(const weather_service_snapshot_t *snapshot,
                                void *user_ctx)
{
    (void)user_ctx;
    if (snapshot == NULL) {
        return;
    }
    mosaic_cap_weather_t payload;
    weather_to_payload(snapshot, &payload);
    mosaic_capability_publish("net.weather", &payload, sizeof(payload));
}

static esp_err_t weather_read(void *user_ctx, void *out, size_t size)
{
    (void)user_ctx;
    if (out == NULL || size != sizeof(mosaic_cap_weather_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    weather_service_snapshot_t snapshot;
    const esp_err_t err = weather_service_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    weather_to_payload(&snapshot, out);
    return ESP_OK;
}

static esp_err_t weather_invoke(void *user_ctx, uint16_t command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    (void)user_ctx;
    (void)args;
    (void)args_size;
    (void)out_result;
    (void)result_size;
    if (command != MOSAIC_CAP_WEATHER_CMD_REFRESH) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* The service refreshes on its own schedule; a request only restarts the
     * poll loop and the result arrives through publish. */
    const esp_err_t err = weather_service_start();
    return err == ESP_OK ? ESP_ERR_NOT_FINISHED : err;
}

static const mosaic_capability_ops_t s_weather_ops = {
    .read = weather_read,
    .invoke = weather_invoke,
};

esp_err_t mosaic_net_weather_init(void)
{
    if (s_registered) {
        return ESP_OK;
    }
    esp_err_t err = mosaic_capability_register(&(mosaic_capability_provider_t) {
        .name = "net.weather",
        .ops = &s_weather_ops,
    });
    if (err != ESP_OK) {
        return err;
    }
    err = weather_service_subscribe(on_weather_snapshot, NULL);
    if (err != ESP_OK) {
        (void)mosaic_capability_unregister("net.weather", NULL);
        return err;
    }
    s_registered = true;
    return ESP_OK;
}
