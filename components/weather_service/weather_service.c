/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "weather_service.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gzip_miniz.h"
#include "nvs.h"
#include "wifi_manager.h"

#define WEATHER_IP_URL                                                                             \
    "http://ip-api.com/json/?fields=status,message,query,lat,lon,city,"                            \
    "regionName,countryCode,timezone&lang=en"
#define WEATHER_MET_URL_FORMAT                                                                     \
    "https://api.met.no/weatherapi/locationforecast/2.0/"                                          \
    "compact?lat=%.4f&lon=%.4f"

#define WEATHER_DEFAULT_REFRESH_MS (60U * 60U * 1000U)
#define WEATHER_DEFAULT_STALE_MS   (6U * 60U * 60U * 1000U)
#define WEATHER_RETRY_1_MS         (60U * 1000U)
#define WEATHER_RETRY_2_MS         (5U * 60U * 1000U)
#define WEATHER_RETRY_3_MS         (15U * 60U * 1000U)
#define WEATHER_RETRY_MAX_MS       (30U * 60U * 1000U)
#define WEATHER_JITTER_MAX_MS      (2U * 60U * 1000U)
#define WEATHER_TIME_RETRY_MS      5000U
#define WEATHER_HTTP_TIMEOUT_MS    15000
#define WEATHER_HTTP_INITIAL_CAP   2048U
#define WEATHER_HTTP_BODY_MAX      (96U * 1024U)
#define WEATHER_GZIP_OUTPUT_MAX    (96U * 1024U)
#define WEATHER_TASK_STACK         (10U * 1024U)
#define WEATHER_TASK_PRIORITY      4U
#define WEATHER_MIN_VALID_EPOCH    INT64_C(1704067200)
#define WEATHER_NVS_NAMESPACE      "weather_cache"
#define WEATHER_NVS_KEY            "snapshot"
#define WEATHER_CACHE_MAGIC        UINT32_C(0x57454154)
#define WEATHER_CACHE_VERSION      3U
#define WEATHER_HEADER_LEN         64U

typedef struct {
    weather_service_event_cb_t callback;
    void *user_ctx;
} weather_observer_t;

typedef struct {
    char *body;
    size_t len;
    size_t cap;
    size_t max;
    bool gzip;
    esp_err_t append_error;
    int status_code;
    int rate_remaining;
    int rate_reset_seconds;
    char last_modified[WEATHER_HEADER_LEN];
    char expires[WEATHER_HEADER_LEN];
} weather_http_response_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;
} weather_memory_reader_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    weather_service_snapshot_t snapshot;
    char last_modified[WEATHER_HEADER_LEN];
} weather_cache_record_t;

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    weather_service_config_t config;
    char user_agent[160];
    weather_service_snapshot_t snapshot;
    weather_observer_t observers[WEATHER_SERVICE_SUBSCRIBER_MAX];
    char last_modified[WEATHER_HEADER_LEN];
    bool initialized;
    bool running;
    bool connected;
} weather_service_state_t;

static const char *TAG = "weather_service";
static weather_service_state_t s_service;

static bool weather_time_valid(void)
{
    return (int64_t)time(NULL) >= WEATHER_MIN_VALID_EPOCH;
}

static bool weather_location_valid(double latitude, double longitude)
{
    return isfinite(latitude) && isfinite(longitude) && latitude >= -90.0 && latitude <= 90.0 &&
           longitude >= -180.0 && longitude <= 180.0;
}

static void weather_copy_header(char *destination, size_t destination_size, const char *value)
{
    if (destination == NULL || destination_size == 0U) {
        return;
    }
    strlcpy(destination, value != NULL ? value : "", destination_size);
}

static esp_err_t weather_http_append(weather_http_response_t *response, const void *data,
                                     size_t len)
{
    if (response == NULL || data == NULL || len == 0U) {
        return ESP_OK;
    }
    if (response->len > response->max || len > response->max - response->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t needed = response->len + len + 1U;
    if (needed > response->cap) {
        size_t next = response->cap != 0U ? response->cap : WEATHER_HTTP_INITIAL_CAP;
        while (next < needed && next < response->max + 1U) {
            next *= 2U;
        }
        if (next > response->max + 1U) {
            next = response->max + 1U;
        }
        char *grown = heap_caps_realloc(response->body, next, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) {
            return ESP_ERR_NO_MEM;
        }
        response->body = grown;
        response->cap = next;
    }
    memcpy(response->body + response->len, data, len);
    response->len += len;
    response->body[response->len] = '\0';
    return ESP_OK;
}

static esp_err_t weather_http_event(esp_http_client_event_t *event)
{
    weather_http_response_t *response =
        event != NULL ? (weather_http_response_t *)event->user_data : NULL;
    if (response == NULL) {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != NULL &&
        event->header_value != NULL) {
        if (strcasecmp(event->header_key, "Content-Encoding") == 0) {
            response->gzip = strcasecmp(event->header_value, "gzip") == 0;
        } else if (strcasecmp(event->header_key, "Last-Modified") == 0) {
            weather_copy_header(response->last_modified, sizeof(response->last_modified),
                                event->header_value);
        } else if (strcasecmp(event->header_key, "Expires") == 0) {
            weather_copy_header(response->expires, sizeof(response->expires), event->header_value);
        } else if (strcasecmp(event->header_key, "X-Rl") == 0) {
            response->rate_remaining = atoi(event->header_value);
        } else if (strcasecmp(event->header_key, "X-Ttl") == 0) {
            response->rate_reset_seconds = atoi(event->header_value);
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data != NULL &&
               event->data_len > 0) {
        response->append_error =
            weather_http_append(response, event->data, (size_t)event->data_len);
        return response->append_error;
    }
    return ESP_OK;
}

static void weather_http_response_free(weather_http_response_t *response)
{
    if (response == NULL) {
        return;
    }
    free(response->body);
    memset(response, 0, sizeof(*response));
}

static esp_err_t weather_http_get(const char *url, const char *if_modified, bool accept_gzip,
                                  weather_http_response_t *response)
{
    ESP_RETURN_ON_FALSE(url != NULL && response != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "HTTP arguments missing");
    memset(response, 0, sizeof(*response));
    response->max = WEATHER_HTTP_BODY_MAX;
    response->rate_remaining = -1;
    response->rate_reset_seconds = -1;

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = weather_http_event,
        .user_data = response,
        .user_agent = s_service.user_agent,
        .timeout_ms = WEATHER_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_NO_MEM, TAG, "HTTP client allocation failed");

    (void)esp_http_client_set_header(client, "Accept", "application/json");
    if (accept_gzip) {
        (void)esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    }
    if (if_modified != NULL && if_modified[0] != '\0') {
        (void)esp_http_client_set_header(client, "If-Modified-Since", if_modified);
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && response->append_error != ESP_OK) {
        err = response->append_error;
    }
    if (err == ESP_OK) {
        response->status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        weather_http_response_free(response);
    }
    return err;
}

static int weather_memory_read(uint8_t *data, int size, void *ctx)
{
    weather_memory_reader_t *reader = (weather_memory_reader_t *)ctx;
    if (reader == NULL || data == NULL || size <= 0) {
        return -1;
    }
    size_t remaining = reader->len - reader->offset;
    size_t count = remaining < (size_t)size ? remaining : (size_t)size;
    if (count > 0U) {
        memcpy(data, reader->data + reader->offset, count);
        reader->offset += count;
    }
    return (int)count;
}

static esp_err_t weather_http_decode_body(weather_http_response_t *response, char **ret_json)
{
    ESP_RETURN_ON_FALSE(response != NULL && ret_json != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "decode arguments missing");
    *ret_json = NULL;
    if (!response->gzip) {
        ESP_RETURN_ON_FALSE(response->body != NULL, ESP_ERR_INVALID_RESPONSE, TAG,
                            "empty HTTP response");
        *ret_json = response->body;
        response->body = NULL;
        return ESP_OK;
    }

    weather_memory_reader_t reader = {
        .data = (const uint8_t *)response->body,
        .len = response->len,
    };
    gzip_miniz_cfg_t gzip_config = {
        .read_cb = weather_memory_read,
        .chunk_size = 1024,
        .ctx = &reader,
    };
    gzip_miniz_handle_t gzip = gzip_miniz_init(&gzip_config);
    ESP_RETURN_ON_FALSE(gzip != NULL, ESP_ERR_NO_MEM, TAG, "gzip decoder allocation failed");

    char *decoded =
        heap_caps_malloc(WEATHER_GZIP_OUTPUT_MAX + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoded == NULL) {
        gzip_miniz_deinit(gzip);
        return ESP_ERR_NO_MEM;
    }
    size_t written = 0U;
    esp_err_t err = ESP_OK;
    while (written < WEATHER_GZIP_OUTPUT_MAX) {
        int count = gzip_miniz_read(gzip, (uint8_t *)decoded + written,
                                    (int)(WEATHER_GZIP_OUTPUT_MAX - written));
        if (count < 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (count == 0) {
            break;
        }
        written += (size_t)count;
    }
    gzip_miniz_deinit(gzip);
    if (err == ESP_OK && written == WEATHER_GZIP_OUTPUT_MAX) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        free(decoded);
        return err;
    }
    decoded[written] = '\0';
    *ret_json = decoded;
    return ESP_OK;
}

static const char *weather_json_string(cJSON *object, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
}

static esp_err_t weather_parse_location(const char *json, weather_service_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(json != NULL && snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "location parse arguments");
    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "location JSON invalid");

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const char *status = weather_json_string(root, "status");
    cJSON *latitude = cJSON_GetObjectItemCaseSensitive(root, "lat");
    cJSON *longitude = cJSON_GetObjectItemCaseSensitive(root, "lon");
    if (status != NULL && strcmp(status, "success") == 0 && cJSON_IsNumber(latitude) &&
        cJSON_IsNumber(longitude) &&
        weather_location_valid(latitude->valuedouble, longitude->valuedouble)) {
        snapshot->latitude = latitude->valuedouble;
        snapshot->longitude = longitude->valuedouble;
        snapshot->location_valid = true;

        const char *city = weather_json_string(root, "city");
        const char *region = weather_json_string(root, "regionName");
        const char *country = weather_json_string(root, "countryCode");
        const char *timezone = weather_json_string(root, "timezone");
        const char *public_ip = weather_json_string(root, "query");
        strlcpy(snapshot->city,
                city != NULL && city[0] != '\0'       ? city
                : region != NULL && region[0] != '\0' ? region
                : country != NULL                     ? country
                                                      : "--",
                sizeof(snapshot->city));
        strlcpy(snapshot->region, region != NULL ? region : "", sizeof(snapshot->region));
        strlcpy(snapshot->timezone, timezone != NULL ? timezone : "", sizeof(snapshot->timezone));
        strlcpy(snapshot->public_ip, public_ip != NULL ? public_ip : "",
                sizeof(snapshot->public_ip));
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}

static const char *weather_condition_for_symbol(const char *symbol)
{
    if (symbol == NULL || symbol[0] == '\0') {
        return "Weather";
    }
    if (strstr(symbol, "thunder") != NULL) {
        return "Thunder";
    }
    if (strstr(symbol, "snow") != NULL) {
        return "Snow";
    }
    if (strstr(symbol, "sleet") != NULL) {
        return "Sleet";
    }
    if (strstr(symbol, "rain") != NULL) {
        return "Rain";
    }
    if (strstr(symbol, "fog") != NULL) {
        return "Fog";
    }
    if (strstr(symbol, "cloud") != NULL) {
        return "Cloudy";
    }
    if (strstr(symbol, "fair") != NULL) {
        return "Sunny";
    }
    if (strstr(symbol, "clear") != NULL) {
        return "Sunny";
    }
    return "Weather";
}

static const char *weather_summary_symbol(cJSON *data, const char *period)
{
    cJSON *period_data = cJSON_GetObjectItemCaseSensitive(data, period);
    cJSON *summary = cJSON_IsObject(period_data)
                         ? cJSON_GetObjectItemCaseSensitive(period_data, "summary")
                         : NULL;
    return cJSON_IsObject(summary) ? weather_json_string(summary, "symbol_code") : NULL;
}

static void weather_day_add_temperature(weather_service_day_t *day,
                                        double value)
{
    if (!isfinite(value) || value < -100.0 || value > 100.0) return;
    const int16_t rounded = (int16_t)lround(value);
    if (!day->valid) {
        day->temperature_min_c = rounded;
        day->temperature_max_c = rounded;
        day->valid = true;
    } else {
        if (rounded < day->temperature_min_c) day->temperature_min_c = rounded;
        if (rounded > day->temperature_max_c) day->temperature_max_c = rounded;
    }
    if (day->temperature_sample_count < UINT8_MAX) {
        ++day->temperature_sample_count;
    }
}

static void weather_day_add_period_extrema(weather_service_day_t *day,
                                           cJSON *data,
                                           const char *period)
{
    cJSON *period_data = cJSON_GetObjectItemCaseSensitive(data, period);
    cJSON *details = cJSON_IsObject(period_data)
                         ? cJSON_GetObjectItemCaseSensitive(period_data, "details")
                         : NULL;
    if (!cJSON_IsObject(details)) return;
    cJSON *minimum = cJSON_GetObjectItemCaseSensitive(details,
                                                       "air_temperature_min");
    cJSON *maximum = cJSON_GetObjectItemCaseSensitive(details,
                                                       "air_temperature_max");
    if (cJSON_IsNumber(minimum)) {
        weather_day_add_temperature(day, minimum->valuedouble);
    }
    if (cJSON_IsNumber(maximum)) {
        weather_day_add_temperature(day, maximum->valuedouble);
    }
}

/* Convert a UTC civil date to Unix days without relying on timegm(), which is
 * not consistently available across all supported ESP-IDF/newlib builds. */
static int64_t weather_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned doy = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool weather_parse_time(const char *value, time_t *ret_time)
{
    int year, month, day, hour, minute, second;
    if (value == NULL || ret_time == NULL ||
            sscanf(value, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day,
                   &hour, &minute, &second) != 6 ||
            month < 1 || month > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
            second < 0 || second > 60) {
        return false;
    }
    *ret_time = (time_t)(weather_days_from_civil(year, (unsigned)month,
                                                  (unsigned)day) * 86400 +
                         hour * 3600 + minute * 60 + second);
    return true;
}

static void weather_prepare_days(weather_service_snapshot_t *snapshot,
                                 struct tm keys[WEATHER_SERVICE_FORECAST_DAYS])
{
    const time_t now = time(NULL);
    memset(snapshot->forecast, 0, sizeof(snapshot->forecast));
    for (size_t i = 0; i < WEATHER_SERVICE_FORECAST_DAYS; ++i) {
        const time_t value = now + (time_t)i * 86400;
        localtime_r(&value, &keys[i]);
        const unsigned month = (unsigned)(keys[i].tm_mon + 1);
        const unsigned day = (unsigned)keys[i].tm_mday;
        snapshot->forecast[i].date[0] = (char)('0' + month / 10U);
        snapshot->forecast[i].date[1] = (char)('0' + month % 10U);
        snapshot->forecast[i].date[2] = '/';
        snapshot->forecast[i].date[3] = (char)('0' + day / 10U);
        snapshot->forecast[i].date[4] = (char)('0' + day % 10U);
        snapshot->forecast[i].date[5] = '\0';
    }
}

static esp_err_t weather_parse_forecast(const char *json, weather_service_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(json != NULL && snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "forecast parse arguments");
    cJSON *root = cJSON_Parse(json);
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_RESPONSE, TAG, "forecast JSON invalid");

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(root, "properties");
    cJSON *timeseries = cJSON_IsObject(properties)
                            ? cJSON_GetObjectItemCaseSensitive(properties, "timeseries")
                            : NULL;
    cJSON *entry = cJSON_IsArray(timeseries) ? cJSON_GetArrayItem(timeseries, 0) : NULL;
    cJSON *data = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "data") : NULL;
    cJSON *instant =
        cJSON_IsObject(data) ? cJSON_GetObjectItemCaseSensitive(data, "instant") : NULL;
    cJSON *details =
        cJSON_IsObject(instant) ? cJSON_GetObjectItemCaseSensitive(instant, "details") : NULL;
    cJSON *temperature = cJSON_IsObject(details)
                             ? cJSON_GetObjectItemCaseSensitive(details, "air_temperature")
                             : NULL;
    if (cJSON_IsNumber(temperature) && isfinite(temperature->valuedouble) &&
        temperature->valuedouble >= -100.0 && temperature->valuedouble <= 100.0) {
        const char *symbol = weather_summary_symbol(data, "next_1_hours");
        if (symbol == NULL) {
            symbol = weather_summary_symbol(data, "next_6_hours");
        }
        if (symbol == NULL) {
            symbol = weather_summary_symbol(data, "next_12_hours");
        }
        snapshot->temperature_c = (int16_t)lround(temperature->valuedouble);
        strlcpy(snapshot->symbol_code, symbol != NULL ? symbol : "", sizeof(snapshot->symbol_code));
        strlcpy(snapshot->condition, weather_condition_for_symbol(symbol),
                sizeof(snapshot->condition));
        snapshot->weather_valid = true;
        err = ESP_OK;
    }

    if (cJSON_IsArray(timeseries) && weather_time_valid()) {
        struct tm keys[WEATHER_SERVICE_FORECAST_DAYS] = {0};
        int symbol_score[WEATHER_SERVICE_FORECAST_DAYS];
        weather_prepare_days(snapshot, keys);
        for (size_t i = 0; i < WEATHER_SERVICE_FORECAST_DAYS; ++i) {
            symbol_score[i] = 25;
        }
        const int count = cJSON_GetArraySize(timeseries);
        for (int index = 0; index < count; ++index) {
            cJSON *item = cJSON_GetArrayItem(timeseries, index);
            const char *timestamp = cJSON_IsObject(item)
                                        ? weather_json_string(item, "time") : NULL;
            time_t epoch;
            struct tm local;
            if (!weather_parse_time(timestamp, &epoch) ||
                    localtime_r(&epoch, &local) == NULL) {
                continue;
            }
            size_t day_index = WEATHER_SERVICE_FORECAST_DAYS;
            for (size_t i = 0; i < WEATHER_SERVICE_FORECAST_DAYS; ++i) {
                if (local.tm_year == keys[i].tm_year &&
                        local.tm_yday == keys[i].tm_yday) {
                    day_index = i;
                    break;
                }
            }
            if (day_index == WEATHER_SERVICE_FORECAST_DAYS) {
                continue;
            }
            cJSON *item_data = cJSON_GetObjectItemCaseSensitive(item, "data");
            cJSON *item_instant = cJSON_IsObject(item_data)
                                      ? cJSON_GetObjectItemCaseSensitive(item_data, "instant") : NULL;
            cJSON *item_details = cJSON_IsObject(item_instant)
                                      ? cJSON_GetObjectItemCaseSensitive(item_instant, "details") : NULL;
            cJSON *item_temp = cJSON_IsObject(item_details)
                                   ? cJSON_GetObjectItemCaseSensitive(item_details, "air_temperature") : NULL;
            if (!cJSON_IsNumber(item_temp) || !isfinite(item_temp->valuedouble) ||
                    item_temp->valuedouble < -100.0 || item_temp->valuedouble > 100.0) {
                continue;
            }
            weather_service_day_t *day = &snapshot->forecast[day_index];
            weather_day_add_temperature(day, item_temp->valuedouble);
            weather_day_add_period_extrema(day, item_data, "next_6_hours");
            weather_day_add_period_extrema(day, item_data, "next_12_hours");
            const char *item_symbol = weather_summary_symbol(item_data, "next_1_hours");
            if (item_symbol == NULL) item_symbol = weather_summary_symbol(item_data, "next_6_hours");
            if (item_symbol == NULL) item_symbol = weather_summary_symbol(item_data, "next_12_hours");
            const int score = abs(local.tm_hour - 12);
            if (item_symbol != NULL && score < symbol_score[day_index]) {
                symbol_score[day_index] = score;
                strlcpy(day->symbol_code, item_symbol, sizeof(day->symbol_code));
                strlcpy(day->condition, weather_condition_for_symbol(item_symbol),
                        sizeof(day->condition));
            }
        }
    }
    cJSON_Delete(root);
    return err;
}

static void weather_cache_load(void)
{
    nvs_handle_t nvs = 0;
    if (nvs_open(WEATHER_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    weather_cache_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        nvs_close(nvs);
        return;
    }
    size_t size = sizeof(*record);
    esp_err_t err = nvs_get_blob(nvs, WEATHER_NVS_KEY, record, &size);
    nvs_close(nvs);
    if (err == ESP_OK && size == sizeof(*record) && record->magic == WEATHER_CACHE_MAGIC &&
        record->version == WEATHER_CACHE_VERSION &&
        (!record->snapshot.location_valid ||
         weather_location_valid(record->snapshot.latitude, record->snapshot.longitude))) {
        s_service.snapshot = record->snapshot;
        s_service.snapshot.online = false;
        s_service.snapshot.stale = true;
        s_service.snapshot.last_error = ESP_ERR_INVALID_STATE;
        s_service.snapshot.sequence++;
        strlcpy(s_service.last_modified, record->last_modified, sizeof(s_service.last_modified));
        ESP_LOGI(TAG, "Loaded cached weather for %s",
                 s_service.snapshot.city[0] != '\0' ? s_service.snapshot.city : "unknown location");
    }
    free(record);
}

static void weather_cache_store(const weather_service_snapshot_t *snapshot,
                                const char *last_modified)
{
    if (snapshot == NULL || !snapshot->weather_valid) {
        return;
    }
    weather_cache_record_t *record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return;
    }
    record->magic = WEATHER_CACHE_MAGIC;
    record->version = WEATHER_CACHE_VERSION;
    record->snapshot = *snapshot;
    strlcpy(record->last_modified, last_modified != NULL ? last_modified : "",
            sizeof(record->last_modified));

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(WEATHER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, WEATHER_NVS_KEY, record, sizeof(*record));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist weather cache failed: %s", esp_err_to_name(err));
    }
    free(record);
}

esp_err_t weather_service_erase_cache(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(
        WEATHER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open weather cache");

    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void weather_publish(const weather_service_snapshot_t *snapshot, const char *last_modified)
{
    weather_observer_t observers[WEATHER_SERVICE_SUBSCRIBER_MAX] = {0};
    weather_service_snapshot_t published = *snapshot;
    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    published.online = s_service.connected;
    if (!published.online && published.weather_valid) {
        published.stale = true;
    }
    published.sequence = s_service.snapshot.sequence + 1U;
    s_service.snapshot = published;
    if (last_modified != NULL) {
        strlcpy(s_service.last_modified, last_modified, sizeof(s_service.last_modified));
    }
    memcpy(observers, s_service.observers, sizeof(observers));
    xSemaphoreGive(s_service.lock);

    for (size_t i = 0; i < WEATHER_SERVICE_SUBSCRIBER_MAX; ++i) {
        if (observers[i].callback != NULL) {
            observers[i].callback(&published, observers[i].user_ctx);
        }
    }
}

static esp_err_t weather_fetch_location(weather_service_snapshot_t *snapshot)
{
    weather_http_response_t response = {0};
    ESP_RETURN_ON_ERROR(weather_http_get(WEATHER_IP_URL, NULL, false, &response), TAG,
                        "IP geolocation request failed");
    esp_err_t err = ESP_FAIL;
    if (response.status_code == 200) {
        err = weather_parse_location(response.body, snapshot);
    } else if (response.status_code == 429) {
        ESP_LOGW(TAG, "ip-api throttled; retry after %d seconds", response.rate_reset_seconds);
        err = ESP_ERR_TIMEOUT;
    } else {
        ESP_LOGW(TAG, "ip-api HTTP status %d", response.status_code);
    }
    if (response.rate_remaining == 0) {
        ESP_LOGW(TAG, "ip-api rate window exhausted for %d seconds", response.rate_reset_seconds);
    }
    weather_http_response_free(&response);
    return err;
}

static esp_err_t weather_fetch_forecast(weather_service_snapshot_t *snapshot,
                                        const char *if_modified, char *ret_last_modified,
                                        size_t ret_last_modified_size, bool *ret_not_modified)
{
    char url[192];
    int written =
        snprintf(url, sizeof(url), WEATHER_MET_URL_FORMAT, snapshot->latitude, snapshot->longitude);
    ESP_RETURN_ON_FALSE(written > 0 && (size_t)written < sizeof(url), ESP_ERR_INVALID_SIZE, TAG,
                        "forecast URL too long");

    weather_http_response_t response = {0};
    ESP_RETURN_ON_ERROR(weather_http_get(url, if_modified, true, &response), TAG,
                        "MET Norway request failed");
    *ret_not_modified = response.status_code == 304;
    if (response.last_modified[0] != '\0') {
        strlcpy(ret_last_modified, response.last_modified, ret_last_modified_size);
    }
    if (*ret_not_modified) {
        weather_http_response_free(&response);
        return ESP_OK;
    }
    if (response.status_code != 200) {
        ESP_LOGW(TAG, "MET Norway HTTP status %d", response.status_code);
        weather_http_response_free(&response);
        return response.status_code == 429 ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }

    char *json = NULL;
    esp_err_t err = weather_http_decode_body(&response, &json);
    if (err == ESP_OK) {
        err = weather_parse_forecast(json, snapshot);
    }
    free(json);
    weather_http_response_free(&response);
    return err;
}

static esp_err_t weather_refresh_once(void)
{
    weather_service_snapshot_t next = {0};
    char previous_last_modified[WEATHER_HEADER_LEN] = {0};
    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    next = s_service.snapshot;
    next.online = s_service.connected;
    strlcpy(previous_last_modified, s_service.last_modified, sizeof(previous_last_modified));
    xSemaphoreGive(s_service.lock);

    const double old_latitude = next.latitude;
    const double old_longitude = next.longitude;
    const bool had_location = next.location_valid;
    esp_err_t location_err = weather_fetch_location(&next);
    if (location_err != ESP_OK && !had_location) {
        next.last_error = location_err;
        next.stale = next.weather_valid;
        weather_publish(&next, NULL);
        return location_err;
    }

    const bool location_changed = !had_location || fabs(next.latitude - old_latitude) > 0.00005 ||
                                  fabs(next.longitude - old_longitude) > 0.00005;
    if (location_changed) {
        previous_last_modified[0] = '\0';
        /* Never display the previous location's weather under a new city. */
        next.weather_valid = false;
        next.temperature_c = 0;
        next.symbol_code[0] = '\0';
        next.condition[0] = '\0';
        memset(next.forecast, 0, sizeof(next.forecast));
        next.last_updated_epoch = 0;
    }

    char last_modified[WEATHER_HEADER_LEN] = {0};
    if (!location_changed) {
        strlcpy(last_modified, previous_last_modified, sizeof(last_modified));
    }
    bool not_modified = false;
    esp_err_t forecast_err = weather_fetch_forecast(&next, previous_last_modified, last_modified,
                                                    sizeof(last_modified), &not_modified);
    if (forecast_err != ESP_OK || (not_modified && !next.weather_valid)) {
        if (forecast_err == ESP_OK) {
            forecast_err = ESP_ERR_INVALID_STATE;
        }
        next.last_error = forecast_err;
        next.stale = next.weather_valid;
        weather_publish(&next, NULL);
        return forecast_err;
    }

    next.online = true;
    next.stale = false;
    next.last_error = location_err == ESP_OK ? ESP_OK : location_err;
    next.last_updated_epoch = (int64_t)time(NULL);
    weather_publish(&next, last_modified);
    weather_cache_store(&next, last_modified);
    ESP_LOGI(TAG, "Weather updated: %s %d C %s", next.city, (int)next.temperature_c,
             next.condition);
    return ESP_OK;
}

static uint32_t weather_retry_delay(unsigned retry)
{
    if (retry == 0U) {
        return WEATHER_RETRY_1_MS;
    }
    if (retry == 1U) {
        return WEATHER_RETRY_2_MS;
    }
    if (retry == 2U) {
        return WEATHER_RETRY_3_MS;
    }
    return WEATHER_RETRY_MAX_MS;
}

static void weather_worker(void *arg)
{
    (void)arg;
    unsigned retry = 0U;
    while (true) {
        bool running = false;
        bool connected = false;
        if (xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE) {
            running = s_service.running;
            connected = s_service.connected;
            xSemaphoreGive(s_service.lock);
        }
        if (!running) {
            break;
        }
        if (!connected) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!weather_time_valid()) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WEATHER_TIME_RETRY_MS));
            continue;
        }

        (void)ulTaskNotifyTake(pdTRUE, 0);
        esp_err_t err = weather_refresh_once();
        uint32_t delay_ms;
        if (err == ESP_OK) {
            retry = 0U;
            delay_ms =
                s_service.config.refresh_interval_ms + esp_random() % (WEATHER_JITTER_MAX_MS + 1U);
        } else {
            delay_ms = weather_retry_delay(retry++);
            ESP_LOGW(TAG, "Weather refresh failed: %s; retry in %u ms", esp_err_to_name(err),
                     (unsigned)delay_ms);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }

    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE) {
        s_service.task = NULL;
        xSemaphoreGive(s_service.lock);
    }
    vTaskDelete(NULL);
}

static void weather_wifi_event(const wifi_manager_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL || s_service.lock == NULL) {
        return;
    }
    bool changed = false;
    TaskHandle_t task = NULL;
    weather_service_snapshot_t snapshot = {0};
    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE) {
        changed = s_service.connected != event->sta_connected;
        s_service.connected = event->sta_connected;
        task = s_service.task;
        snapshot = s_service.snapshot;
        xSemaphoreGive(s_service.lock);
    }
    if (!changed) {
        return;
    }
    snapshot.online = event->sta_connected;
    if (!event->sta_connected && snapshot.weather_valid) {
        snapshot.stale = true;
        snapshot.last_error = ESP_ERR_INVALID_STATE;
    }
    weather_publish(&snapshot, NULL);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

esp_err_t weather_service_init(const weather_service_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL && config->user_agent != NULL &&
                            config->user_agent[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "identifiable User-Agent is required");
    if (s_service.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_service, 0, sizeof(s_service));
    s_service.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_service.lock != NULL, ESP_ERR_NO_MEM, TAG,
                        "service mutex allocation failed");
    s_service.config = *config;
    s_service.config.refresh_interval_ms = config->refresh_interval_ms != 0U
                                               ? config->refresh_interval_ms
                                               : WEATHER_DEFAULT_REFRESH_MS;
    s_service.config.stale_after_ms =
        config->stale_after_ms != 0U ? config->stale_after_ms : WEATHER_DEFAULT_STALE_MS;
    strlcpy(s_service.user_agent, config->user_agent, sizeof(s_service.user_agent));
    s_service.config.user_agent = s_service.user_agent;
    s_service.snapshot.last_error = ESP_ERR_INVALID_STATE;
    weather_cache_load();
    s_service.initialized = true;
    return ESP_OK;
}

esp_err_t weather_service_start(void)
{
    ESP_RETURN_ON_FALSE(s_service.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "service is not initialized");
    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_service.running) {
        xSemaphoreGive(s_service.lock);
        return ESP_OK;
    }
    s_service.running = true;
    BaseType_t created = xTaskCreate(weather_worker, "weather", WEATHER_TASK_STACK, NULL,
                                     WEATHER_TASK_PRIORITY, &s_service.task);
    if (created != pdPASS) {
        s_service.running = false;
        s_service.task = NULL;
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_service.lock);

    esp_err_t err = wifi_manager_register_event_callback(weather_wifi_event, NULL);
    if (err != ESP_OK) {
        (void)weather_service_stop();
        return err;
    }
    return ESP_OK;
}

esp_err_t weather_service_stop(void)
{
    ESP_RETURN_ON_FALSE(s_service.initialized, ESP_ERR_INVALID_STATE, TAG,
                        "service is not initialized");
    TaskHandle_t task = NULL;
    if (xSemaphoreTake(s_service.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!s_service.running) {
        xSemaphoreGive(s_service.lock);
        return ESP_OK;
    }
    s_service.running = false;
    task = s_service.task;
    xSemaphoreGive(s_service.lock);
    (void)wifi_manager_unregister_event_callback(weather_wifi_event, NULL);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return ESP_OK;
}

esp_err_t weather_service_get_snapshot(weather_service_snapshot_t *ret_snapshot)
{
    ESP_RETURN_ON_FALSE(s_service.initialized && ret_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "snapshot arguments invalid");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE, ESP_ERR_TIMEOUT,
                        TAG, "snapshot lock failed");
    *ret_snapshot = s_service.snapshot;
    if (ret_snapshot->weather_valid && weather_time_valid() &&
        ret_snapshot->last_updated_epoch > 0) {
        int64_t age_ms = ((int64_t)time(NULL) - ret_snapshot->last_updated_epoch) * INT64_C(1000);
        if (age_ms > (int64_t)s_service.config.stale_after_ms) {
            ret_snapshot->stale = true;
        }
    }
    xSemaphoreGive(s_service.lock);
    return ESP_OK;
}

esp_err_t weather_service_subscribe(weather_service_event_cb_t callback, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_service.initialized && callback != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "subscriber arguments invalid");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE, ESP_ERR_TIMEOUT,
                        TAG, "subscriber lock failed");
    size_t free_index = WEATHER_SERVICE_SUBSCRIBER_MAX;
    for (size_t i = 0; i < WEATHER_SERVICE_SUBSCRIBER_MAX; ++i) {
        if (s_service.observers[i].callback == callback &&
            s_service.observers[i].user_ctx == user_ctx) {
            xSemaphoreGive(s_service.lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (free_index == WEATHER_SERVICE_SUBSCRIBER_MAX &&
            s_service.observers[i].callback == NULL) {
            free_index = i;
        }
    }
    if (free_index == WEATHER_SERVICE_SUBSCRIBER_MAX) {
        xSemaphoreGive(s_service.lock);
        return ESP_ERR_NO_MEM;
    }
    s_service.observers[free_index] = (weather_observer_t){
        .callback = callback,
        .user_ctx = user_ctx,
    };
    weather_service_snapshot_t snapshot = s_service.snapshot;
    xSemaphoreGive(s_service.lock);
    callback(&snapshot, user_ctx);
    return ESP_OK;
}

esp_err_t weather_service_unsubscribe(weather_service_event_cb_t callback, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_service.initialized && callback != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "subscriber arguments invalid");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_service.lock, portMAX_DELAY) == pdTRUE, ESP_ERR_TIMEOUT,
                        TAG, "subscriber lock failed");
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < WEATHER_SERVICE_SUBSCRIBER_MAX; ++i) {
        if (s_service.observers[i].callback == callback &&
            s_service.observers[i].user_ctx == user_ctx) {
            memset(&s_service.observers[i], 0, sizeof(s_service.observers[i]));
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(s_service.lock);
    return err;
}
