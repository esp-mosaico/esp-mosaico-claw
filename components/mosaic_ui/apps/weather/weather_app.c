/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "weather_binds.h"
#include "weather_forecast_icon_data.h"
#include "weather_objects.h"
#include "weather_templates.h"

enum { WEATHER_FORECAST_DAYS = 7 };

typedef struct {
    char date[6];
    char low[12];
    char high[12];
    const uint8_t *icon;
    size_t icon_size;
} weather_forecast_row_t;

static weather_forecast_row_t s_forecast_rows[WEATHER_FORECAST_DAYS];
static esp_gsp_list_t s_forecast_list = ESP_GSP_LIST_NONE;

static void weather_forecast_set_icon(weather_forecast_row_t *row,
                                      const char *symbol)
{
#define WEATHER_ICON(_name) do { \
    row->icon = weather_forecast_icon_##_name; \
    row->icon_size = sizeof(weather_forecast_icon_##_name); \
} while (0)
    if (symbol && (strstr(symbol, "thunder") || strstr(symbol, "rain"))) {
        WEATHER_ICON(thunder);
    } else if (symbol && (strstr(symbol, "snow") || strstr(symbol, "sleet"))) {
        WEATHER_ICON(snow);
    } else if (symbol && strstr(symbol, "wind")) {
        WEATHER_ICON(windy);
    } else if (symbol && (strstr(symbol, "clear") || strstr(symbol, "fair"))) {
        WEATHER_ICON(sunny);
    } else {
        WEATHER_ICON(cloudy);
    }
#undef WEATHER_ICON
}
static void weather_render_dates(esp_gsp_handle_t ui)
{
    (void)ui;
    const time_t now = time(NULL);
    for (size_t i = 0; i < WEATHER_FORECAST_DAYS; ++i) {
        weather_forecast_row_t *row = &s_forecast_rows[i];
        memcpy(row->date, "--/--", sizeof(row->date));
        memcpy(row->low, "--", 3);
        memcpy(row->high, "--", 3);
        weather_forecast_set_icon(row, NULL);
        const time_t value = now + (time_t)i * 86400;
        struct tm local;
        if (localtime_r(&value, &local)) {
            const unsigned month = (unsigned)(local.tm_mon + 1);
            const unsigned day = (unsigned)local.tm_mday;
            row->date[0] = (char)('0' + month / 10U);
            row->date[1] = (char)('0' + month % 10U);
            row->date[3] = (char)('0' + day / 10U);
            row->date[4] = (char)('0' + day % 10U);
        }
    }
}

static gsp_err_t weather_forecast_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    if (item_index >= WEATHER_FORECAST_DAYS) {
        return GSP_ERR_INVALID_ARG;
    }
    const weather_forecast_row_t *item = &s_forecast_rows[item_index];
    gsp_err_t err = gsp_weather_weather_forecast_row_row_set_date_text(
        ui, row, item->date);
    if (err == GSP_OK) {
        err = gsp_weather_weather_forecast_row_row_set_icon_resource(
            ui, row, item->icon, item->icon_size);
    }
    if (err == GSP_OK) {
        err = gsp_weather_weather_forecast_row_row_set_low_text(
            ui, row, item->low);
    }
    if (err == GSP_OK) {
        err = gsp_weather_weather_forecast_row_row_set_high_text(
            ui, row, item->high);
    }
    return err;
}

static void weather_forecast_attach(esp_gsp_handle_t ui)
{
    if (s_forecast_list == ESP_GSP_LIST_NONE) {
        s_forecast_list = gsp_weather_weather_forecast_list_bind(
            ui, weather_forecast_bind_item, NULL);
    }
    if (s_forecast_list != ESP_GSP_LIST_NONE) {
        (void)gsp_weather_weather_forecast_list_set_total(
            ui, s_forecast_list, WEATHER_FORECAST_DAYS);
        (void)gsp_weather_weather_forecast_list_refresh(ui, s_forecast_list);
    }
}

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "weather_service.h"

static weather_service_snapshot_t s_pending;
static bool s_pending_valid;
static bool s_subscribed;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void weather_stage(const weather_service_snapshot_t *snapshot,
                          void *user_ctx)
{
    (void)user_ctx;
    if (!snapshot) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_pending = *snapshot;
    s_pending_valid = true;
    portEXIT_CRITICAL(&s_lock);
}

static bool weather_take(weather_service_snapshot_t *snapshot)
{
    bool ready;
    portENTER_CRITICAL(&s_lock);
    ready = s_pending_valid;
    if (ready) {
        *snapshot = s_pending;
        s_pending_valid = false;
    }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

static void weather_render(esp_gsp_handle_t ui,
                           const weather_service_snapshot_t *snapshot)
{
    char temperature[12] = "--";
    const char *condition = "Unavailable";
    const char *city = "Location unavailable";
    if (snapshot->weather_valid) {
        snprintf(temperature, sizeof(temperature), "%d°",
                 (int)snapshot->temperature_c);
        condition = snapshot->condition[0] ? snapshot->condition : "Weather";
    }
    if (snapshot->location_valid && snapshot->city[0]) {
        city = snapshot->city;
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_WEATHER_TEMPERATURE, temperature);
    (void)esp_gsp_set_text(ui, GSP_BIND_WEATHER_CONDITION, condition);
    (void)esp_gsp_set_text(ui, GSP_BIND_WEATHER_CITY, city);
    for (size_t i = 0; i < WEATHER_SERVICE_FORECAST_DAYS; ++i) {
        const weather_service_day_t *day = &snapshot->forecast[i];
        weather_forecast_row_t *row = &s_forecast_rows[i];
        if (day->date[0]) {
            strlcpy(row->date, day->date, sizeof(row->date));
        }
        if (day->valid) {
            snprintf(row->high, sizeof(row->high), "%d°",
                     (int)day->temperature_max_c);
            if (day->temperature_sample_count >= 2U &&
                    day->temperature_min_c != day->temperature_max_c) {
                snprintf(row->low, sizeof(row->low), "%d°",
                         (int)day->temperature_min_c);
            } else {
                memcpy(row->low, "--", 3);
            }
        }
        weather_forecast_set_icon(row, day->symbol_code);
    }
    if (s_forecast_list != ESP_GSP_LIST_NONE) {
        (void)gsp_weather_weather_forecast_list_refresh(ui, s_forecast_list);
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_WEATHER_STATUS,
                          snapshot->stale ? "CACHED" :
                          snapshot->online ? "LIVE" : "OFFLINE");

    const char *symbol = snapshot->symbol_code;
    const bool thunder = strstr(symbol, "thunder") != NULL ||
                         strstr(symbol, "rain") != NULL;
    const bool snow = strstr(symbol, "snow") != NULL ||
                      strstr(symbol, "sleet") != NULL;
    const bool windy = strstr(symbol, "wind") != NULL;
    const bool sunny = strstr(symbol, "clear") != NULL ||
                       strstr(symbol, "fair") != NULL;
    const bool cloudy = strstr(symbol, "partlycloudy") != NULL;
    const bool overcast = !thunder && !snow && !windy && !sunny && !cloudy;
    (void)esp_gsp_set_visible(ui, GSP_BIND_WEATHER_ART_THUNDER_VISIBLE, thunder);
    (void)esp_gsp_set_visible(ui, GSP_BIND_WEATHER_ART_SNOW_VISIBLE, snow);
    (void)esp_gsp_set_visible(ui, GSP_BIND_WEATHER_ART_WINDY_VISIBLE, windy);
    (void)esp_gsp_set_visible(ui, GSP_BIND_WEATHER_ART_SUNNY_VISIBLE, sunny);
    (void)esp_gsp_set_visible(ui, GSP_BIND_WEATHER_ART_CLOUDY_VISIBLE, cloudy);
    (void)esp_gsp_set_visible(ui, GSP_BIND_WEATHER_ART_OVERCAST_VISIBLE, overcast);
}
#endif

static void weather_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
#if defined(ESP_PLATFORM)
    weather_service_snapshot_t snapshot;
    if (weather_take(&snapshot)) {
        weather_render(ui, &snapshot);
    }
#else
    (void)ui;
#endif
}

static void weather_started(esp_gsp_handle_t ui)
{
    weather_render_dates(ui);
    weather_forecast_attach(ui);
#if defined(ESP_PLATFORM)
    weather_service_snapshot_t snapshot = {0};
    if (!s_subscribed && weather_service_subscribe(weather_stage, NULL) == ESP_OK) {
        s_subscribed = true;
    }
    if (weather_service_get_snapshot(&snapshot) == ESP_OK) {
        weather_render(ui, &snapshot);
    }
#endif
    (void)esp_gsp_timer_create(ui, 1000, weather_tick, NULL);
}

static void weather_stopping(esp_gsp_handle_t ui)
{
    (void)ui;
    s_forecast_list = ESP_GSP_LIST_NONE;
#if defined(ESP_PLATFORM)
    if (s_subscribed) {
        (void)weather_service_unsubscribe(weather_stage, NULL);
        s_subscribed = false;
    }
    portENTER_CRITICAL(&s_lock);
    s_pending_valid = false;
    portEXIT_CRITICAL(&s_lock);
#endif
}

const mosaic_app_descriptor_t mosaic_weather_app = {
    .id = 13,
    .launch_action = GSP_ACT_ID_APP_WEATHER,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "weather",
    .title = "Weather",
    .directory = &gsp_obj_directory_weather,
    .disable_swipe = true,
    .instance_slots = GSP_TEMPLATE_WEATHER_FORECAST_ROW_MAX_INSTANCES,
    .dynamic_image_slots = GSP_TEMPLATE_WEATHER_FORECAST_ROW_MAX_INSTANCES,
    .on_started = weather_started,
    .on_stopping = weather_stopping,
};
