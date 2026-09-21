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
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "Live logs require CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y"
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

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t s_httpd;
static QueueHandle_t s_queue;
static TaskHandle_t s_sender_task;
static log_client_t s_clients[LOG_WS_MAX_CLIENTS];
static atomic_uint s_active_clients;
static uint32_t s_next_generation;
static unsigned s_pending_work;
static bool s_sender_stop_requested;
static bool s_hook_installed;
static vprintf_like_t s_previous_vprintf = vprintf;

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
    portENTER_CRITICAL(&s_lock);
    if (s_httpd == server) {
        for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
            if (s_clients[i].fd == client->fd && s_clients[i].generation == client->generation) {
                current = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static void __attribute__((noinline)) log_capture(const char *format, va_list args)
{
    log_item_t item = {0};

    portENTER_CRITICAL(&s_lock);
    if (s_httpd && s_queue) {
        for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
            if (s_clients[i].fd >= 0) {
                item.clients[item.client_count++] = s_clients[i];
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (item.client_count) {
        int written = vsnprintf(item.text, sizeof(item.text), format, args);
        if (written > 0) {
            item.len = (size_t)written < sizeof(item.text) ? (size_t)written : sizeof(item.text) - 1;
            if ((size_t)written >= sizeof(item.text)) {
                item.len = log_utf8_prefix_len(item.text, item.len);
            }
            /* No waiting, allocation, or logging on the producer path. */
            (void)xQueueSend(s_queue, &item, 0);
        }
    }
}

static int logs_vprintf(const char *format, va_list args)
{
    if (atomic_load_explicit(&s_active_clients, memory_order_relaxed) == 0) {
        return s_previous_vprintf(format, args);
    }

    /* The original sink consumes args. Keep an independent copy for the web stream. */
    va_list copy;
    va_copy(copy, args);
    int result = s_previous_vprintf(format, args);
    log_capture(format, copy);
    va_end(copy);
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

    portENTER_CRITICAL(&s_lock);
    --s_pending_work;
    portEXIT_CRITICAL(&s_lock);
    free(job);
}

static void log_sender_task(void *arg)
{
    log_item_t item;
    (void)arg;

    while (true) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            bool stop_requested;
            portENTER_CRITICAL(&s_lock);
            stop_requested = s_sender_stop_requested;
            portEXIT_CRITICAL(&s_lock);
            if (stop_requested) {
                break;
            }
            continue;
        }

        httpd_handle_t server;
        portENTER_CRITICAL(&s_lock);
        if (s_sender_stop_requested) {
            portEXIT_CRITICAL(&s_lock);
            break;
        }
        server = s_httpd;
        if (server && s_pending_work < LOG_MAX_PENDING_WORK) {
            ++s_pending_work;
        } else {
            server = NULL;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!server) {
            continue;
        }

        log_send_job_t *job = malloc(sizeof(*job));
        if (job) {
            job->server = server;
            job->item = item;
        }
        if (!job || httpd_queue_work(server, log_send_job_run, job) != ESP_OK) {
            free(job);
            portENTER_CRITICAL(&s_lock);
            --s_pending_work;
            portEXIT_CRITICAL(&s_lock);
        }
    }

    portENTER_CRITICAL(&s_lock);
    if (s_sender_task == xTaskGetCurrentTaskHandle()) {
        s_sender_task = NULL;
    }
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static bool log_ws_add(int fd)
{
    bool added = false;
    portENTER_CRITICAL(&s_lock);
    if (s_httpd) {
        for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
            if (s_clients[i].fd == fd) {
                added = true;
                break;
            }
        }
        if (!added) {
            for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
                if (s_clients[i].fd < 0) {
                    s_clients[i].fd = fd;
                    s_clients[i].generation = ++s_next_generation;
                    atomic_fetch_add_explicit(&s_active_clients, 1, memory_order_relaxed);
                    added = true;
                    break;
                }
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return added;
}

void http_server_logs_ws_fd_remove(int fd)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        if (s_clients[i].fd == fd) {
            s_clients[i].fd = -1;
            atomic_fetch_sub_explicit(&s_active_clients, 1, memory_order_relaxed);
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
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
        .ws_post_handshake_cb = log_ws_connected,
    };
    return httpd_register_uri_handler(server, &handler);
}

esp_err_t http_server_logs_start(httpd_handle_t server)
{
    if (!s_queue) {
        s_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_item_t));
        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_sender_stop_requested = false;
    portEXIT_CRITICAL(&s_lock);

    if (!s_sender_task && xTaskCreate(log_sender_task, "http_log", 4096, NULL, 3, &s_sender_task) != pdPASS) {
        portENTER_CRITICAL(&s_lock);
        s_sender_stop_requested = true;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    xQueueReset(s_queue);

    portENTER_CRITICAL(&s_lock);
    s_httpd = server;
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        s_clients[i].fd = -1;
    }
    atomic_store_explicit(&s_active_clients, 0, memory_order_relaxed);
    portEXIT_CRITICAL(&s_lock);

    if (!s_hook_installed) {
        s_previous_vprintf = esp_log_set_vprintf(logs_vprintf);
        s_hook_installed = true;
    }
    return ESP_OK;
}

void http_server_logs_stop(void)
{
    bool sender_running;
    bool hook_installed;

    portENTER_CRITICAL(&s_lock);
    s_httpd = NULL;
    s_sender_stop_requested = true;
    for (size_t i = 0; i < LOG_WS_MAX_CLIENTS; ++i) {
        s_clients[i].fd = -1;
    }
    atomic_store_explicit(&s_active_clients, 0, memory_order_relaxed);
    sender_running = s_sender_task != NULL;
    hook_installed = s_hook_installed;
    portEXIT_CRITICAL(&s_lock);

    if (s_queue) {
        xQueueReset(s_queue);
    }

    while (sender_running) {
        vTaskDelay(1);
        portENTER_CRITICAL(&s_lock);
        sender_running = s_sender_task != NULL;
        portEXIT_CRITICAL(&s_lock);
    }

    if (hook_installed) {
        esp_log_set_vprintf(s_previous_vprintf);
        s_hook_installed = false;
    }
}
