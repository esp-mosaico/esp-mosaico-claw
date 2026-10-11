/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* REST API for the Air Voice (whisper capture) test page.
 *
 *   GET    /api/airvoice/status?monitor=1   live meters (monitor keeps the mic open)
 *   GET    /api/airvoice/config             settings + preset names
 *   POST   /api/airvoice/config             partial settings update
 *   POST   /api/airvoice/preset             {"name": "..."}
 *   POST   /api/airvoice/record/start|stop|toggle
 *   POST   /api/airvoice/play               {"name": "...", "track": "processed|raw"}
 *   POST   /api/airvoice/play/stop|pause
 *   POST   /api/airvoice/volume             {"volume": 0..100}
 *   GET    /api/airvoice/recordings         newest first
 *   DELETE /api/airvoice/recordings?name=
 *   GET    /api/airvoice/audio?name=&track= WAV with HTTP Range support
 *   GET    /api/airvoice/meta?name=         recording metadata JSON
 *   GET    /api/airvoice/logs?after=SEQ     log lines newer than SEQ
 *   POST   /api/airvoice/logs/clear
 */
#include "http_server_priv.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "air_voice_json.h"
#include "air_voice_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define AV_API_PREFIX       "/api/airvoice/"
#define AV_LIST_MAX         200U
#define AV_LOG_BATCH        120U
#define AV_RANGE_HDR_MAX    64U
#define AV_META_MAX         8192U
#define AV_RESP_HDR_MAX     320U
#define AV_WAV_HEADER       44U

static const char *TAG = "http_air_voice";

static void *psram_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddStringToObject(root, "error", message);
    httpd_resp_set_status(req, status);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_esp_error(httpd_req_t *req, esp_err_t err, const char *what)
{
    char message[96];
    snprintf(message, sizeof(message), "%s: %s", what, esp_err_to_name(err));
    switch (err) {
    case ESP_ERR_INVALID_ARG: return send_error(req, "400 Bad Request", message);
    case ESP_ERR_NOT_FOUND: return send_error(req, "404 Not Found", message);
    case ESP_ERR_INVALID_STATE: return send_error(req, "409 Conflict", message);
    case ESP_ERR_NO_MEM: return send_error(req, "507 Insufficient Storage", message);
    case ESP_ERR_TIMEOUT: return send_error(req, "504 Gateway Timeout", message);
    default: return send_error(req, "500 Internal Server Error", message);
    }
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    return http_server_send_json_response(req, root);
}

/* Copies the URI path below the API prefix (query stripped) into @p out. */
static void api_route(httpd_req_t *req, char *out, size_t out_size)
{
    const char *path = req->uri + strlen(AV_API_PREFIX);
    size_t len = strcspn(path, "?");
    if (len >= out_size) {
        len = out_size - 1U;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

static esp_err_t ensure_service(httpd_req_t *req)
{
    const esp_err_t err = air_voice_service_start();
    if (err != ESP_OK) {
        (void)send_esp_error(req, err, "Air Voice service unavailable");
    }
    return err;
}

static air_voice_track_t parse_track(const char *value)
{
    return value && strcmp(value, "raw") == 0 ? AIR_VOICE_TRACK_RAW : AIR_VOICE_TRACK_PROCESSED;
}

/* ---------------------------- GET ---------------------------------- */

static esp_err_t get_status(httpd_req_t *req)
{
    char monitor[8] = {0};
    if (http_server_query_get(req, "monitor", monitor, sizeof(monitor)) == ESP_OK &&
        strcmp(monitor, "1") == 0) {
        (void)air_voice_hold(AIR_VOICE_CLIENT_WEB, true);
    }
    air_voice_status_t status;
    esp_err_t err = air_voice_get_status(&status);
    if (err != ESP_OK) {
        return send_esp_error(req, err, "status");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }
    air_voice_status_to_json(&status, root);
    return http_server_send_json_response(req, root);
}

static esp_err_t get_config(httpd_req_t *req)
{
    air_voice_config_t config;
    esp_err_t err = air_voice_get_config(&config);
    if (err != ESP_OK) {
        return send_esp_error(req, err, "config");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *cfg = cJSON_CreateObject();
    cJSON *presets = cJSON_CreateArray();
    if (!root || !cfg || !presets) {
        cJSON_Delete(root);
        cJSON_Delete(cfg);
        cJSON_Delete(presets);
        return httpd_resp_send_500(req);
    }
    air_voice_config_to_json(&config, cfg);
    cJSON_AddItemToObject(root, "config", cfg);
    const char *const *names = NULL;
    const size_t count = air_voice_preset_names(&names);
    for (size_t i = 0; i < count; ++i) {
        cJSON_AddItemToArray(presets, cJSON_CreateString(names[i]));
    }
    cJSON_AddItemToObject(root, "presets", presets);
    return http_server_send_json_response(req, root);
}

static esp_err_t get_recordings(httpd_req_t *req)
{
    air_voice_recording_t *items = psram_calloc(AV_LIST_MAX, sizeof(*items));
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (!items || !root || !array) {
        free(items);
        cJSON_Delete(root);
        cJSON_Delete(array);
        return httpd_resp_send_500(req);
    }
    size_t total = 0;
    const size_t count = air_voice_list_recordings(items, AV_LIST_MAX, &total);
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item) {
            air_voice_recording_to_json(&items[i], item);
            cJSON_AddItemToArray(array, item);
        }
    }
    free(items);
    cJSON_AddItemToObject(root, "items", array);
    cJSON_AddNumberToObject(root, "total", (double)total);
    return http_server_send_json_response(req, root);
}

static esp_err_t get_logs(httpd_req_t *req)
{
    char after_text[16] = {0};
    uint32_t after = 0;
    if (http_server_query_get(req, "after", after_text, sizeof(after_text)) == ESP_OK) {
        after = (uint32_t)strtoul(after_text, NULL, 10);
    }
    air_voice_log_line_t *lines = psram_calloc(AV_LOG_BATCH, sizeof(*lines));
    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (!lines || !root || !array) {
        free(lines);
        cJSON_Delete(root);
        cJSON_Delete(array);
        return httpd_resp_send_500(req);
    }
    const size_t count = air_voice_log_read(after, lines, AV_LOG_BATCH);
    uint32_t last = after;
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        const char level[2] = {lines[i].level, '\0'};
        cJSON_AddNumberToObject(item, "seq", lines[i].seq);
        cJSON_AddNumberToObject(item, "ts", lines[i].ts_ms);
        cJSON_AddStringToObject(item, "level", level);
        cJSON_AddStringToObject(item, "text", lines[i].text);
        cJSON_AddItemToArray(array, item);
        last = lines[i].seq;
    }
    free(lines);
    cJSON_AddItemToObject(root, "lines", array);
    cJSON_AddNumberToObject(root, "last_seq", last);
    cJSON_AddBoolToObject(root, "more", count == AV_LOG_BATCH);
    return http_server_send_json_response(req, root);
}

static esp_err_t get_meta(httpd_req_t *req)
{
    char name[AIR_VOICE_NAME_MAX] = {0};
    char path[HTTP_SERVER_PATH_MAX];
    if (http_server_query_get(req, "name", name, sizeof(name)) != ESP_OK ||
        air_voice_meta_path(name, path, sizeof(path)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid recording name");
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return send_error(req, "404 Not Found", "no metadata for this recording");
    }
    char *text = psram_calloc(1, AV_META_MAX);
    if (!text) {
        fclose(file);
        return httpd_resp_send_500(req);
    }
    const size_t len = fread(text, 1, AV_META_MAX - 1U, file);
    fclose(file);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    const esp_err_t err = httpd_resp_send(req, text, (ssize_t)len);
    free(text);
    return err;
}

static esp_err_t send_all(httpd_req_t *req, const char *buf, size_t len)
{
    while (len > 0) {
        const int sent = httpd_send(req, buf, len);
        if (sent <= 0) {
            return ESP_FAIL;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return ESP_OK;
}

/* Parse "bytes=a-b", "bytes=a-" or "bytes=-n". Returns false if malformed or
 * unsatisfiable. */
static bool parse_range(const char *value, uint64_t size, uint64_t *start, uint64_t *end)
{
    if (strncmp(value, "bytes=", 6) != 0 || size == 0) {
        return false;
    }
    const char *spec = value + 6;
    const char *dash = strchr(spec, '-');
    if (!dash || strchr(spec, ',')) {
        return false;
    }
    char *tail = NULL;
    if (dash == spec) {
        const uint64_t suffix = strtoull(dash + 1, &tail, 10);
        if (suffix == 0) {
            return false;
        }
        *start = suffix >= size ? 0 : size - suffix;
        *end = size - 1U;
        return true;
    }
    *start = strtoull(spec, &tail, 10);
    *end = dash[1] ? strtoull(dash + 1, &tail, 10) : size - 1U;
    if (*end >= size) {
        *end = size - 1U;
    }
    return *start <= *end && *start < size;
}

static esp_err_t get_audio(httpd_req_t *req)
{
    char name[AIR_VOICE_NAME_MAX] = {0};
    char track[16] = {0};
    char path[HTTP_SERVER_PATH_MAX];
    (void)http_server_query_get(req, "track", track, sizeof(track));
    if (http_server_query_get(req, "name", name, sizeof(name)) != ESP_OK ||
        air_voice_recording_path(name, parse_track(track), path, sizeof(path)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid recording name");
    }
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_error(req, "404 Not Found", "recording not found");
    }
    const uint64_t size = (uint64_t)st.st_size;
    uint64_t start = 0;
    uint64_t end = size > 0 ? size - 1U : 0;
    bool partial = false;
    char range[AV_RANGE_HDR_MAX] = {0};
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        if (!parse_range(range, size, &start, &end)) {
            char header[AV_RESP_HDR_MAX];
            const int n = snprintf(header, sizeof(header),
                                   "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                   "Content-Range: bytes */%" PRIu64 "\r\n"
                                   "Content-Length: 0\r\n\r\n", size);
            return send_all(req, header, (size_t)n);
        }
        partial = true;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return send_error(req, "500 Internal Server Error", "failed to open recording");
    }
    /* Recordings use streaming headers; serve the real sizes instead. */
    uint8_t header[AV_WAV_HEADER] = {0};
    const bool fix_header = start < AV_WAV_HEADER && size >= AV_WAV_HEADER &&
        fread(header, 1, sizeof(header), file) == sizeof(header);
    if (fix_header) {
        air_voice_wav_fix_header(header, size);
    }
    char *scratch = http_server_alloc_scratch_buffer();
    if (!scratch || fseek(file, (long)start, SEEK_SET) != 0) {
        free(scratch);
        fclose(file);
        return httpd_resp_send_500(req);
    }
    const uint64_t length = size > 0 ? end - start + 1U : 0;
    char response[AV_RESP_HDR_MAX];
    int n;
    if (partial) {
        n = snprintf(response, sizeof(response),
                     "HTTP/1.1 206 Partial Content\r\n"
                     "Content-Type: audio/wav\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Cache-Control: no-store\r\n"
                     "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n"
                     "Content-Length: %" PRIu64 "\r\n\r\n", start, end, size, length);
    } else {
        n = snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: audio/wav\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Cache-Control: no-store\r\n"
                     "Content-Disposition: inline; filename=\"%s%s.wav\"\r\n"
                     "Content-Length: %" PRIu64 "\r\n\r\n",
                     name, parse_track(track) == AIR_VOICE_TRACK_RAW ? "_raw" : "", length);
    }
    esp_err_t err = send_all(req, response, (size_t)n);
    uint64_t remaining = length;
    uint64_t offset = start;
    while (err == ESP_OK && remaining > 0) {
        const size_t want = remaining < HTTP_SERVER_SCRATCH_SIZE ? (size_t)remaining
                                                                 : HTTP_SERVER_SCRATCH_SIZE;
        const size_t got = fread(scratch, 1, want, file);
        if (got == 0) {
            err = ESP_FAIL; /* truncated: abort the connection */
            break;
        }
        if (fix_header && offset < AV_WAV_HEADER) {
            const size_t overlap = (size_t)(AV_WAV_HEADER - offset) < got
                ? (size_t)(AV_WAV_HEADER - offset) : got;
            memcpy(scratch, header + offset, overlap);
        }
        err = send_all(req, scratch, got);
        remaining -= got;
        offset += got;
    }
    free(scratch);
    fclose(file);
    return err;
}

static esp_err_t air_voice_get_handler(httpd_req_t *req)
{
    if (ensure_service(req) != ESP_OK) {
        return ESP_OK;
    }
    char route[48];
    api_route(req, route, sizeof(route));
    if (strcmp(route, "status") == 0) return get_status(req);
    if (strcmp(route, "config") == 0) return get_config(req);
    if (strcmp(route, "recordings") == 0) return get_recordings(req);
    if (strcmp(route, "logs") == 0) return get_logs(req);
    if (strcmp(route, "audio") == 0) return get_audio(req);
    if (strcmp(route, "meta") == 0) return get_meta(req);
    return send_error(req, "404 Not Found", "unknown Air Voice endpoint");
}

/* ---------------------------- POST --------------------------------- */

static esp_err_t post_config(httpd_req_t *req)
{
    cJSON *body = NULL;
    if (http_server_parse_json_body(req, &body) != ESP_OK) {
        return send_error(req, "400 Bad Request", "invalid JSON body");
    }
    air_voice_config_t config;
    esp_err_t err = air_voice_get_config(&config);
    if (err == ESP_OK) {
        air_voice_config_from_json(&config, body);
        err = air_voice_set_config(&config);
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_esp_error(req, err, "apply settings");
    }
    return get_config(req);
}

static esp_err_t post_with_name(httpd_req_t *req, const char *route)
{
    cJSON *body = NULL;
    char name[AIR_VOICE_NAME_MAX] = {0};
    char track[16] = {0};
    int volume = -1;
    if (req->content_len > 0) {
        if (http_server_parse_json_body(req, &body) != ESP_OK) {
            return send_error(req, "400 Bad Request", "invalid JSON body");
        }
        http_server_json_read_string(body, "name", name, sizeof(name));
        http_server_json_read_string(body, "track", track, sizeof(track));
        const cJSON *vol = cJSON_GetObjectItemCaseSensitive(body, "volume");
        if (cJSON_IsNumber(vol)) {
            volume = vol->valueint;
        }
        cJSON_Delete(body);
    }
    esp_err_t err;
    if (strcmp(route, "preset") == 0) {
        err = air_voice_apply_preset(name);
        return err == ESP_OK ? get_config(req) : send_esp_error(req, err, "apply preset");
    } else if (strcmp(route, "play") == 0) {
        err = air_voice_play(name, parse_track(track));
    } else if (strcmp(route, "volume") == 0) {
        err = volume < 0 ? ESP_ERR_INVALID_ARG : air_voice_set_volume(volume);
    } else if (strcmp(route, "delete") == 0) {
        err = air_voice_delete_recording(name);
    } else {
        return send_error(req, "404 Not Found", "unknown Air Voice endpoint");
    }
    return err == ESP_OK ? send_ok(req) : send_esp_error(req, err, route);
}

static esp_err_t air_voice_post_handler(httpd_req_t *req)
{
    if (ensure_service(req) != ESP_OK) {
        return ESP_OK;
    }
    char route[48];
    api_route(req, route, sizeof(route));
    esp_err_t err;
    if (strcmp(route, "record/start") == 0) {
        err = air_voice_record_start();
    } else if (strcmp(route, "record/stop") == 0) {
        err = air_voice_record_stop();
    } else if (strcmp(route, "record/toggle") == 0) {
        err = air_voice_record_toggle();
    } else if (strcmp(route, "play/stop") == 0) {
        err = air_voice_play_stop();
    } else if (strcmp(route, "play/pause") == 0) {
        err = air_voice_play_pause_toggle();
    } else if (strcmp(route, "logs/clear") == 0) {
        air_voice_log_clear();
        err = ESP_OK;
    } else if (strcmp(route, "config") == 0) {
        return post_config(req);
    } else {
        return post_with_name(req, route);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", route, esp_err_to_name(err));
        return send_esp_error(req, err, route);
    }
    return send_ok(req);
}

/* --------------------------- DELETE -------------------------------- */

static esp_err_t air_voice_delete_handler(httpd_req_t *req)
{
    if (ensure_service(req) != ESP_OK) {
        return ESP_OK;
    }
    char route[48];
    api_route(req, route, sizeof(route));
    if (strcmp(route, "recordings") != 0) {
        return send_error(req, "404 Not Found", "unknown Air Voice endpoint");
    }
    char name[AIR_VOICE_NAME_MAX] = {0};
    if (http_server_query_get(req, "name", name, sizeof(name)) != ESP_OK) {
        return send_error(req, "400 Bad Request", "missing name");
    }
    const esp_err_t err = air_voice_delete_recording(name);
    return err == ESP_OK ? send_ok(req) : send_esp_error(req, err, "delete");
}

esp_err_t http_server_register_air_voice_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = AV_API_PREFIX "*", .method = HTTP_GET, .handler = air_voice_get_handler },
        { .uri = AV_API_PREFIX "*", .method = HTTP_POST, .handler = air_voice_post_handler },
        { .uri = AV_API_PREFIX "*", .method = HTTP_DELETE, .handler = air_voice_delete_handler },
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
