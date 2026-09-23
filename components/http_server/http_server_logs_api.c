/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_log.h"
#include "app_system_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "Live logs require CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y"
#endif

#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "Live logs require CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y"
#endif

#define LOG_WS_MAX_CLIENTS 2
#define LOG_QUEUE_DEPTH 8
#define LOG_LINE_SIZE 384
#define LOG_MAX_PENDING_WORK 4

typedef struct {
    int fd;
    uint32_t generation;
} log_client_t;

typedef struct {
    size_t len;
    size_t client_count;
    log_client_t clients[LOG_WS_MAX_CLIENTS];
    char text[LOG_LINE_SIZE];
} log_item_t;

typedef struct {
    httpd_handle_t server;
    log_item_t item;
} log_send_job_t;

typedef struct {
    portMUX_TYPE lock;
    httpd_handle_t server;
    QueueHandle_t queue;
    TaskHandle_t sender_task;
    log_client_t clients[LOG_WS_MAX_CLIENTS];
    atomic_uint active_clients;
    atomic_uint capture_users;
    atomic_uint pending_work;
    atomic_bool enabled;
    atomic_flag lifecycle_lock;
    uint32_t next_generation;
    vprintf_like_t previous_vprintf;
} log_state_t;

static log_state_t s_log = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .lifecycle_lock = ATOMIC_FLAG_INIT,
    .previous_vprintf = vprintf,
};

static size_t log_utf8_prefix_len(const char *text, size_t len)
{
    size_t start = len;
    while (start > 0 && ((unsigned char)text[start - 1] & 0xc0) == 0x80) {
        --start;
    }
    if (start == 0) {
        return 0;
    }

    unsigned char lead = (unsigned char)text[start - 1];
    size_t char_size = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    return len - start + 1 < char_size ? start - 1 : len;
}

static bool log_client_is_current(const log_client_t *client, httpd_handle_t server)
{
    bool current = false;
    portENTER_CRITICAL(&s_log.lock);
    if (s_log.server == server && atomic_load_explicit(&s_log.enabled, memory_order_relaxed)) {
        for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
            if (s_log.clients[i].fd == client->fd && s_log.clients[i].generation == client->generation) {
                current = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_log.lock);
    return current;
}

static void __attribute__((noinline)) log_capture(const char *format, va_list args)
{
    log_item_t item = {0};

    portENTER_CRITICAL(&s_log.lock);
    if (s_log.server && s_log.queue && atomic_load_explicit(&s_log.enabled, memory_order_relaxed)) {
        for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
            if (s_log.clients[i].fd >= 0) {
                item.clients[item.client_count++] = s_log.clients[i];
            }
        }
    }
    portEXIT_CRITICAL(&s_log.lock);

    if (item.client_count) {
        int written = vsnprintf(item.text, sizeof(item.text), format, args);
        if (written > 0) {
            item.len = (size_t)written < sizeof(item.text) ? (size_t)written : sizeof(item.text) - 1;
            if ((size_t)written >= sizeof(item.text)) {
                item.len = log_utf8_prefix_len(item.text, item.len);
            }
            /* No waiting, allocation, or logging on the producer path. */
            (void)xQueueSend(s_log.queue, &item, 0);
        }
    }
}

static int logs_vprintf(const char *format, va_list args)
{
    atomic_fetch_add_explicit(&s_log.capture_users, 1, memory_order_acquire);
    if (!atomic_load_explicit(&s_log.enabled, memory_order_relaxed) ||
        atomic_load_explicit(&s_log.active_clients, memory_order_relaxed) == 0) {
        int result = s_log.previous_vprintf(format, args);
        atomic_fetch_sub_explicit(&s_log.capture_users, 1, memory_order_release);
        return result;
    }

    /* The original sink consumes args. Keep an independent copy for the web stream. */
    va_list copy;
    va_copy(copy, args);
    int result = s_log.previous_vprintf(format, args);
    log_capture(format, copy);
    va_end(copy);
    atomic_fetch_sub_explicit(&s_log.capture_users, 1, memory_order_release);
    return result;
}

static void log_send_job_run(void *arg)
{
    log_send_job_t *job = arg;
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)job->item.text,
        .len = job->item.len,
    };

    for (size_t i = 0; i < job->item.client_count; ++i) {
        log_client_t *client = &job->item.clients[i];
        if (!log_client_is_current(client, job->server)) {
            continue;
        }
        if (httpd_ws_send_frame_async(job->server, client->fd, &frame) != ESP_OK) {
            http_server_logs_ws_fd_remove(client->fd);
            (void)httpd_sess_trigger_close(job->server, client->fd);
        }
    }

    atomic_fetch_sub_explicit(&s_log.pending_work, 1, memory_order_relaxed);
    free(job);
}

static void log_sender_task(void *arg)
{
    log_item_t item;
    (void)arg;

    while (xQueueReceive(s_log.queue, &item, portMAX_DELAY) == pdTRUE) {
        if (item.client_count == 0) {
            break;
        }

        httpd_handle_t server;
        portENTER_CRITICAL(&s_log.lock);
        server = s_log.server;
        portEXIT_CRITICAL(&s_log.lock);
        if (!server) {
            continue;
        }
        unsigned pending = atomic_fetch_add_explicit(&s_log.pending_work, 1, memory_order_relaxed);
        if (pending >= LOG_MAX_PENDING_WORK) {
            atomic_fetch_sub_explicit(&s_log.pending_work, 1, memory_order_relaxed);
            continue;
        }

        log_send_job_t *job = malloc(sizeof(*job));
        if (job) {
            job->server = server;
            job->item = item;
        }
        if (!job || httpd_queue_work(server, log_send_job_run, job) != ESP_OK) {
            free(job);
            atomic_fetch_sub_explicit(&s_log.pending_work, 1, memory_order_relaxed);
        }
    }

    portENTER_CRITICAL(&s_log.lock);
    if (s_log.sender_task == xTaskGetCurrentTaskHandle()) {
        s_log.sender_task = NULL;
    }
    portEXIT_CRITICAL(&s_log.lock);
    vTaskDelete(NULL);
}

static bool log_ws_add(int fd)
{
    bool added = false;
    portENTER_CRITICAL(&s_log.lock);
    if (s_log.server && atomic_load_explicit(&s_log.enabled, memory_order_relaxed)) {
        for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
            if (s_log.clients[i].fd == fd) {
                added = true;
                break;
            }
        }
        if (!added) {
            for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
                if (s_log.clients[i].fd < 0) {
                    s_log.clients[i].fd = fd;
                    s_log.clients[i].generation = ++s_log.next_generation;
                    atomic_fetch_add_explicit(&s_log.active_clients, 1, memory_order_relaxed);
                    added = true;
                    break;
                }
            }
        }
    }
    portEXIT_CRITICAL(&s_log.lock);
    return added;
}

void http_server_logs_ws_fd_remove(int fd)
{
    portENTER_CRITICAL(&s_log.lock);
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        if (s_log.clients[i].fd == fd) {
            s_log.clients[i].fd = -1;
            atomic_fetch_sub_explicit(&s_log.active_clients, 1, memory_order_relaxed);
            break;
        }
    }
    portEXIT_CRITICAL(&s_log.lock);
}

static void log_close_disabled_clients(void *arg)
{
    httpd_handle_t server = arg;
    log_client_t clients[LOG_WS_MAX_CLIENTS];
    portENTER_CRITICAL(&s_log.lock);
    if (s_log.server != server || atomic_load_explicit(&s_log.enabled, memory_order_relaxed)) {
        portEXIT_CRITICAL(&s_log.lock);
        return;
    }
    memcpy(clients, s_log.clients, sizeof(clients));
    portEXIT_CRITICAL(&s_log.lock);
    /* Executed on HTTPD's task so descriptors cannot be reused mid-close. */
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0) {
            http_server_logs_ws_fd_remove(clients[i].fd);
            (void)httpd_sess_trigger_close(server, clients[i].fd);
        }
    }
}

static esp_err_t log_resources_start(void)
{
    if (s_log.queue) {
        return ESP_OK;
    }

    s_log.queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_item_t));
    if (!s_log.queue) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(log_sender_task, "http_log", 4096, NULL, 3, &s_log.sender_task) != pdPASS) {
        vQueueDelete(s_log.queue);
        s_log.queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_log.previous_vprintf = esp_log_set_vprintf(logs_vprintf);
    atomic_store_explicit(&s_log.enabled, true, memory_order_release);
    return ESP_OK;
}

static void log_resources_stop(void)
{
    atomic_store_explicit(&s_log.enabled, false, memory_order_release);

    portENTER_CRITICAL(&s_log.lock);
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        s_log.clients[i].generation = ++s_log.next_generation;
    }
    portEXIT_CRITICAL(&s_log.lock);

    esp_log_set_vprintf(s_log.previous_vprintf);
    while (atomic_load_explicit(&s_log.capture_users, memory_order_acquire) != 0) {
        vTaskDelay(1);
    }

    log_item_t stop_item = {0};
    xQueueReset(s_log.queue);
    (void)xQueueSend(s_log.queue, &stop_item, portMAX_DELAY);

    portENTER_CRITICAL(&s_log.lock);
    bool sender_running = s_log.sender_task != NULL;
    portEXIT_CRITICAL(&s_log.lock);
    while (sender_running) {
        vTaskDelay(1);
        portENTER_CRITICAL(&s_log.lock);
        sender_running = s_log.sender_task != NULL;
        portEXIT_CRITICAL(&s_log.lock);
    }

    vQueueDelete(s_log.queue);
    s_log.queue = NULL;
}

esp_err_t http_server_set_web_logs_enabled(bool enabled)
{
    while (atomic_flag_test_and_set_explicit(&s_log.lifecycle_lock, memory_order_acquire)) {
        vTaskDelay(1);
    }
    if (enabled == atomic_load_explicit(&s_log.enabled, memory_order_acquire)) {
        atomic_flag_clear_explicit(&s_log.lifecycle_lock, memory_order_release);
        return ESP_OK;
    }

    httpd_handle_t server;
    portENTER_CRITICAL(&s_log.lock);
    server = s_log.server;
    portEXIT_CRITICAL(&s_log.lock);
    if (enabled) {
        esp_err_t err = server ? log_resources_start() : ESP_ERR_INVALID_STATE;
        atomic_flag_clear_explicit(&s_log.lifecycle_lock, memory_order_release);
        return err;
    }

    log_resources_stop();
    if (server) {
        (void)httpd_queue_work(server, log_close_disabled_clients, server);
    }
    atomic_flag_clear_explicit(&s_log.lifecycle_lock, memory_order_release);
    return ESP_OK;
}

static esp_err_t log_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, atomic_load_explicit(&s_log.enabled, memory_order_relaxed)
        ? "{\"enabled\":true}" : "{\"enabled\":false}");
}

static esp_err_t log_ws_pre_handshake(httpd_req_t *req)
{
    if (atomic_load_explicit(&s_log.enabled, memory_order_relaxed)) {
        return ESP_OK;
    }
    (void)httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Live logs are disabled in Settings > Debug");
    return ESP_FAIL;
}

static esp_err_t log_ws_connected(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (!log_ws_add(fd)) {
        return ESP_FAIL;
    }
    /* A stalled log viewer must not hold the HTTP server task for seconds. */
    struct timeval send_timeout = { .tv_sec = 0, .tv_usec = 100000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    /* Exercise the same capture and asynchronous send path as device logs. */
    ESP_LOGI("http_logs", "Live log client connected (fd=%d)", fd);
    return ESP_OK;
}

static esp_err_t log_ws_handler(httpd_req_t *req)
{
    /* A plain HTTP GET is not a subscription. IDF completes WS upgrades via
     * ws_post_handshake_cb without invoking this handler. */
    if (req->method == HTTP_GET) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WebSocket upgrade required");
    }

    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        http_server_logs_ws_fd_remove(fd);
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        http_server_logs_ws_fd_remove(fd);
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_PING || frame.len > 125) {
        return ESP_FAIL;
    }

    uint8_t payload[125];
    frame.payload = payload;
    if (frame.len && httpd_ws_recv_frame(req, &frame, sizeof(payload)) != ESP_OK) {
        return ESP_FAIL;
    }
    frame.type = HTTPD_WS_TYPE_PONG;
    return httpd_ws_send_frame(req, &frame);
}

esp_err_t http_server_register_logs_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri = "/ws/logs",
        .method = HTTP_GET,
        .handler = log_ws_handler,
        .is_websocket = true,
        .ws_pre_handshake_cb = log_ws_pre_handshake,
        .ws_post_handshake_cb = log_ws_connected,
    };
    esp_err_t err = httpd_register_uri_handler(server, &handler);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t status = {
        .uri = "/api/logs/status",
        .method = HTTP_GET,
        .handler = log_status_handler,
    };
    return httpd_register_uri_handler(server, &status);
}

esp_err_t http_server_logs_start(httpd_handle_t server)
{
    portENTER_CRITICAL(&s_log.lock);
    s_log.server = server;
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        s_log.clients[i].fd = -1;
    }
    atomic_store_explicit(&s_log.active_clients, 0, memory_order_relaxed);
    portEXIT_CRITICAL(&s_log.lock);

    app_system_config_t config;
    esp_err_t config_err = app_system_config_load(&config);
    /* Missing or corrupt storage must never opt the device into log exposure. */
    return config_err == ESP_OK && config.web_logs_enabled ? http_server_set_web_logs_enabled(true) : ESP_OK;
}

void http_server_logs_stop(void)
{
    (void)http_server_set_web_logs_enabled(false);

    portENTER_CRITICAL(&s_log.lock);
    s_log.server = NULL;
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        s_log.clients[i].fd = -1;
    }
    atomic_store_explicit(&s_log.active_clients, 0, memory_order_relaxed);
    portEXIT_CRITICAL(&s_log.lock);
}
