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
#include "weather_actions.h"
#include "mosaic_runtime.h"
#include "mosaic_app_shell.h"
#if defined(ESP_PLATFORM)
#include "weather_service.h"
#endif
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
static esp_gsp_list_t s_city_list = ESP_GSP_LIST_NONE;
static bool s_location_page, s_keyboard_active;
static char s_city_labels[6][128];
#if defined(ESP_PLATFORM)
static uint32_t s_rendered_sequence;
static bool s_rendered_stale;
static weather_city_search_t s_city_search;
#endif

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
static bool s_pending_valid, s_pending_seen;
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
    /* Worker and Wi-Fi notifications run outside the service mutex and can
     * arrive out of order. Keep the most recently committed snapshot. */
    if (!s_pending_seen || (int32_t)(snapshot->sequence - s_pending.sequence) >= 0) {
        s_pending = *snapshot;
        s_pending_valid = s_pending_seen = true;
    }
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
        strlcpy(row->date, day->date[0] ? day->date : "--/--", sizeof(row->date));
        memcpy(row->low, "--", 3);
        memcpy(row->high, "--", 3);
        if (day->valid) {
            snprintf(row->high, sizeof(row->high), "%d°",
                     (int)day->temperature_max_c);
            if (day->temperature_sample_count >= 2U &&
                    day->temperature_min_c != day->temperature_max_c) {
                snprintf(row->low, sizeof(row->low), "%d°",
                         (int)day->temperature_min_c);
            }
        }
        weather_forecast_set_icon(row, day->symbol_code);
    }
    if (s_forecast_list != ESP_GSP_LIST_NONE) {
        (void)gsp_weather_weather_forecast_list_refresh(ui, s_forecast_list);
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_WEATHER_STATUS,
                          !snapshot->online ? "OFFLINE" :
                          snapshot->location_stale ? "LOCATION?" :
                          snapshot->stale ? "CACHED" :
                          !snapshot->weather_valid ? (snapshot->last_error == ESP_OK ? "UPDATING" : "RETRY") :
                          snapshot->location_source == WEATHER_LOCATION_MANUAL ? "SELECTED" : "IP ESTIMATE");
    s_rendered_sequence = snapshot->sequence;
    s_rendered_stale = snapshot->stale;

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

static gsp_err_t weather_city_bind_item(esp_gsp_handle_t ui, esp_gsp_row_t row,
                                       uint32_t index, void *ctx)
{
    (void)ctx;
    if (index>=6) return GSP_ERR_INVALID_ARG;
    return gsp_weather_weather_city_row_row_set_title_text(ui,row,s_city_labels[index]);
}

static void weather_keyboard_detach(esp_gsp_handle_t ui)
{
    if (!s_keyboard_active) return;
    s_keyboard_active=false;
    (void)esp_gsp_keyboard_attach(ui,ESP_GSP_KEYBOARD_NONE,GSP_BIND_WEATHER_CITY_QUERY);
    mosaic_app_shell_set_bottom_enabled(ui,true);
}

static void weather_location_refresh(esp_gsp_handle_t ui)
{
#if defined(ESP_PLATFORM)
    weather_city_search_t search;
    if (weather_service_get_city_search(&search)!=ESP_OK) return;
    if (search.revision==s_city_search.revision && search.state==s_city_search.state) return;
    s_city_search=search;
    const char *text=search.state==WEATHER_SEARCH_PENDING ? "Searching..." :
        search.state==WEATHER_SEARCH_FAILED ? "Search unavailable. Try again." :
        search.state==WEATHER_SEARCH_IDLE ? "Search for your city" :
        search.count==0 ? "No cities found. Add a country." : "Select a city";
    (void)esp_gsp_set_text(ui,GSP_BIND_WEATHER_CITY_SEARCH_STATUS,text);
    const uint32_t count=search.state==WEATHER_SEARCH_COMPLETE ? search.count : 0;
    for (uint32_t i=0;i<count;++i) {
        const weather_city_t *c=&search.cities[i];
        snprintf(s_city_labels[i],sizeof(s_city_labels[i]),"%s, %s (%s)",c->city,c->region,c->country);
    }
    if (s_city_list!=ESP_GSP_LIST_NONE) {
        (void)gsp_weather_weather_city_results_set_total(ui,s_city_list,count);
        (void)gsp_weather_weather_city_results_refresh(ui,s_city_list);
    }
#else
    (void)ui;
#endif
}

static bool weather_back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)timestamp_us;
    if (s_keyboard_active) {
        weather_keyboard_detach(ui);
        (void)esp_gsp_stack_view_pop(ui,GSP_OBJ_KEY_WEATHER_STACK,false);
        return true;
    }
    if (s_location_page) {
        s_location_page=false;
        (void)esp_gsp_stack_view_pop(ui,GSP_OBJ_KEY_WEATHER_STACK,false);
        return true;
    }
    return false;
}

static void weather_find_city(esp_gsp_handle_t ui)
{
#if defined(ESP_PLATFORM)
    if (!s_keyboard_active) return;
    char query[WEATHER_SERVICE_QUERY_LEN];
    if (esp_gsp_keyboard_text(ui,query,sizeof(query))!=ESP_GSP_OK ||
            weather_service_search_city(query)!=ESP_OK) {
        (void)esp_gsp_set_text(ui,GSP_BIND_WEATHER_CITY_QUERY_HINT,"Enter at least 2 letters");
        return;
    }
    weather_keyboard_detach(ui);
    (void)esp_gsp_stack_view_pop(ui,GSP_OBJ_KEY_WEATHER_STACK,false);
    s_city_search.revision=UINT32_MAX;
    weather_location_refresh(ui);
#else
    (void)ui;
#endif
}

static void weather_event(esp_gsp_handle_t ui, const struct mosaic_event *raw)
{
    const mosaic_event_t *event=raw;
    if (event==NULL || event->type!=MOSAIC_EVENT_UI_CALL) return;
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_WEATHER_OPEN_LOCATION:
        if (!s_location_page) {
            s_location_page=true;
            (void)esp_gsp_stack_view_push(ui,GSP_OBJ_KEY_WEATHER_STACK,1,false);
#if defined(ESP_PLATFORM)
            s_city_search.revision=UINT32_MAX;
#endif
            weather_location_refresh(ui);
        }
        break;
    case GSP_ACT_ID_WEATHER_LOCATION_BACK:
        (void)weather_back(ui,event->timestamp_us);
        break;
    case GSP_ACT_ID_WEATHER_CITY_SEARCH:
        if (s_location_page && !s_keyboard_active) {
            s_keyboard_active=true;
            (void)esp_gsp_stack_view_push(ui,GSP_OBJ_KEY_WEATHER_STACK,2,false);
            mosaic_app_shell_set_bottom_enabled(ui,false);
            (void)esp_gsp_set_cursor(ui,GSP_BIND_WEATHER_CITY_QUERY);
            (void)esp_gsp_keyboard_attach(ui,GSP_ACT_ID_WEATHER_CITY_KEYBOARD_KEY,GSP_BIND_WEATHER_CITY_QUERY);
        }
        break;
    case GSP_ACT_ID_WEATHER_CITY_FIND:
        weather_find_city(ui);
        break;
    case GSP_ACT_ID_WEATHER_CITY_KEYBOARD_KEY:
        if (event->data.call.arg==13) weather_find_city(ui);
        break;
    case GSP_ACT_ID_WEATHER_USE_NETWORK:
#if defined(ESP_PLATFORM)
        if (weather_service_use_network_location()==ESP_OK) (void)weather_back(ui,event->timestamp_us);
        else (void)esp_gsp_set_text(ui,GSP_BIND_WEATHER_CITY_SEARCH_STATUS,"Could not save location. Retry.");
#endif
        break;
    case GSP_ACT_ID_WEATHER_SELECT_CITY:
#if defined(ESP_PLATFORM)
        if (s_city_search.state==WEATHER_SEARCH_COMPLETE && event->data.call.item<s_city_search.count) {
            if (weather_service_set_city(&s_city_search.cities[event->data.call.item])==ESP_OK) {
                (void)weather_back(ui,event->timestamp_us);
            } else (void)esp_gsp_set_text(ui,GSP_BIND_WEATHER_CITY_SEARCH_STATUS,"Could not save city. Retry.");
        }
#endif
        break;
    default: break;
    }
}

static void weather_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
#if defined(ESP_PLATFORM)
    weather_service_snapshot_t snapshot;
    if (weather_take(&snapshot) || weather_service_get_snapshot(&snapshot)==ESP_OK) {
        if (snapshot.sequence!=s_rendered_sequence || snapshot.stale!=s_rendered_stale)
            weather_render(ui, &snapshot);
    }
    if (s_location_page) weather_location_refresh(ui);
#else
    (void)ui;
#endif
}

static void weather_started(esp_gsp_handle_t ui)
{
    s_location_page=s_keyboard_active=false;
    weather_render_dates(ui);
    weather_forecast_attach(ui);
    s_city_list=gsp_weather_weather_city_results_bind(ui,weather_city_bind_item,NULL);
#if defined(ESP_PLATFORM)
    s_rendered_sequence=UINT32_MAX;
    memset(&s_city_search,0,sizeof(s_city_search));
#endif
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
    weather_keyboard_detach(ui);
    s_location_page=false;
    s_city_list=ESP_GSP_LIST_NONE;
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
    .root_stack_key = GSP_OBJ_KEY_WEATHER_STACK,
    .instance_slots = GSP_TEMPLATE_WEATHER_FORECAST_ROW_MAX_INSTANCES + GSP_TEMPLATE_WEATHER_CITY_ROW_MAX_INSTANCES,
    .dynamic_image_slots = GSP_TEMPLATE_WEATHER_FORECAST_ROW_MAX_INSTANCES,
    .on_started = weather_started,
    .on_stopping = weather_stopping,
    .on_event = weather_event,
    .on_back = weather_back,
};
