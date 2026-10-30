/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_SERVICE_CITY_LEN       48U
#define WEATHER_SERVICE_REGION_LEN     48U
#define WEATHER_SERVICE_TIMEZONE_LEN   40U
#define WEATHER_SERVICE_PUBLIC_IP_LEN  48U
#define WEATHER_SERVICE_SYMBOL_LEN     40U
#define WEATHER_SERVICE_CONDITION_LEN  24U
#define WEATHER_SERVICE_SUBSCRIBER_MAX 4U
#define WEATHER_SERVICE_FORECAST_DAYS  7U
#define WEATHER_SERVICE_DATE_LEN       6U
#define WEATHER_SERVICE_CITY_RESULTS   6U
#define WEATHER_SERVICE_QUERY_LEN      96U

typedef enum {
    WEATHER_LOCATION_IP = 0,
    WEATHER_LOCATION_MANUAL,
} weather_location_source_t;

typedef struct {
    double latitude, longitude;
    char city[WEATHER_SERVICE_CITY_LEN];
    char region[WEATHER_SERVICE_REGION_LEN];
    char country[8];
    char timezone[WEATHER_SERVICE_TIMEZONE_LEN];
} weather_city_t;

typedef enum {
    WEATHER_SEARCH_IDLE = 0, WEATHER_SEARCH_PENDING,
    WEATHER_SEARCH_COMPLETE, WEATHER_SEARCH_FAILED,
} weather_search_state_t;

typedef struct {
    uint32_t revision;
    weather_search_state_t state;
    esp_err_t last_error;
    size_t count;
    weather_city_t cities[WEATHER_SERVICE_CITY_RESULTS];
} weather_city_search_t;

typedef struct {
    bool valid;
    int16_t temperature_min_c;
    int16_t temperature_max_c;
    uint8_t temperature_sample_count;
    char date[WEATHER_SERVICE_DATE_LEN];
    char symbol_code[WEATHER_SERVICE_SYMBOL_LEN];
    char condition[WEATHER_SERVICE_CONDITION_LEN];
} weather_service_day_t;

typedef struct {
    bool online;
    bool location_valid;
    bool location_stale;
    weather_location_source_t location_source;
    int64_t location_updated_epoch;
    bool weather_valid;
    bool stale;
    double latitude;
    double longitude;
    int16_t temperature_c;
    char city[WEATHER_SERVICE_CITY_LEN];
    char region[WEATHER_SERVICE_REGION_LEN];
    char timezone[WEATHER_SERVICE_TIMEZONE_LEN];
    char public_ip[WEATHER_SERVICE_PUBLIC_IP_LEN];
    char symbol_code[WEATHER_SERVICE_SYMBOL_LEN];
    char condition[WEATHER_SERVICE_CONDITION_LEN];
    weather_service_day_t forecast[WEATHER_SERVICE_FORECAST_DAYS];
    int64_t last_updated_epoch;
    uint32_t sequence;
    esp_err_t last_error;
} weather_service_snapshot_t;

typedef struct {
    /** MET Norway requires an identifiable User-Agent with real contact info. */
    const char *user_agent;
    /** Successful refresh cadence. Zero selects one hour. */
    uint32_t refresh_interval_ms;
    /** Cached data older than this is marked stale. Zero selects six hours. */
    uint32_t stale_after_ms;
} weather_service_config_t;

/** Notifications run outside the service mutex and may overlap across tasks.
 * Consumers staging them must use snapshot.sequence to discard older events. */
typedef void (*weather_service_event_cb_t)(
    const weather_service_snapshot_t *snapshot, void *user_ctx);

/** Initialize the singleton service and load its normalized NVS cache. */
esp_err_t weather_service_init(const weather_service_config_t *config);

/** Factory reset: erase cached weather and selected city. Call before init or after stop. */
esp_err_t weather_service_erase_cache(void);

/** Start Wi-Fi observation and the asynchronous refresh worker. */
esp_err_t weather_service_start(void);

/** Stop the worker. Primarily intended for orderly tests/shutdown. */
esp_err_t weather_service_stop(void);

/** Return the latest immutable snapshot. */
esp_err_t weather_service_get_snapshot(
    weather_service_snapshot_t *ret_snapshot);
/** Explicit searches are serialized on the weather worker; no new task. */
esp_err_t weather_service_search_city(const char *query);
esp_err_t weather_service_get_city_search(weather_city_search_t *ret_search);
/** Persist a selected WGS84 city. IP refresh never replaces a manual choice. */
esp_err_t weather_service_set_city(const weather_city_t *city);
esp_err_t weather_service_use_network_location(void);
esp_err_t weather_service_request_refresh(void);

/** Registration immediately publishes the current snapshot. */
esp_err_t weather_service_subscribe(
    weather_service_event_cb_t callback, void *user_ctx);
esp_err_t weather_service_unsubscribe(
    weather_service_event_cb_t callback, void *user_ctx);

#ifdef __cplusplus
}
#endif
