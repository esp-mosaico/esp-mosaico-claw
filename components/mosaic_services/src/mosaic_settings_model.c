/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_settings.h"

#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

typedef struct {
    mosaic_battery_event_cb_t callback;
    void *user_ctx;
} mosaic_battery_subscriber_t;

typedef struct {
    mosaic_wifi_event_cb_t callback;
    void *user_ctx;
} mosaic_wifi_subscriber_t;

#if defined(ESP_PLATFORM)
static portMUX_TYPE s_battery_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_BATTERY_LOCK()   portENTER_CRITICAL(&s_battery_lock)
#define MOSAIC_BATTERY_UNLOCK() portEXIT_CRITICAL(&s_battery_lock)
static portMUX_TYPE s_wifi_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_WIFI_LOCK()   portENTER_CRITICAL(&s_wifi_lock)
#define MOSAIC_WIFI_UNLOCK() portEXIT_CRITICAL(&s_wifi_lock)
#else
#define MOSAIC_BATTERY_LOCK()   ((void)0)
#define MOSAIC_BATTERY_UNLOCK() ((void)0)
#define MOSAIC_WIFI_LOCK()   ((void)0)
#define MOSAIC_WIFI_UNLOCK() ((void)0)
#endif
static mosaic_settings_battery_t s_battery_cache;
static bool s_battery_cache_valid;
static mosaic_battery_subscriber_t
    s_battery_subscribers[MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX];
static mosaic_settings_network_t s_wifi_cache;
static bool s_wifi_cache_valid;
static mosaic_wifi_subscriber_t
    s_wifi_subscribers[MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX];

static mosaic_settings_ops_t s_ops;

esp_err_t mosaic_settings_configure(const mosaic_settings_ops_t *ops)
{
    if (ops == NULL || ops->get_snapshot == NULL ||
            ops->set_rotation == NULL || ops->set_brightness == NULL ||
            ops->set_volume == NULL ||
            ops->request_network_reconfigure == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ops = *ops;
    return ESP_OK;
}

esp_err_t mosaic_settings_set_rotation(uint16_t degrees)
{
    if (degrees > 270U || (degrees % 90U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops.set_rotation == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.set_rotation(s_ops.user_ctx, degrees);
}

esp_err_t mosaic_settings_set_brightness(int brightness, bool persist)
{
    if (brightness < 0 || brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops.set_brightness == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.set_brightness(
        s_ops.user_ctx, brightness, persist);
}

esp_err_t mosaic_settings_set_volume(int volume, bool persist)
{
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops.set_volume == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.set_volume(s_ops.user_ctx, volume, persist);
}

esp_err_t mosaic_settings_set_vibration(bool enabled)
{
    if (s_ops.set_vibration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.set_vibration(s_ops.user_ctx, enabled);
}

esp_err_t mosaic_settings_set_screen_timeout(uint32_t timeout_ms)
{
    if (s_ops.set_screen_timeout == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.set_screen_timeout(s_ops.user_ctx, timeout_ms);
}

esp_err_t mosaic_settings_factory_reset(void)
{
    if (s_ops.factory_reset == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.factory_reset(s_ops.user_ctx);
}

esp_err_t mosaic_settings_request_update_check(void)
{
    if (s_ops.request_update_check == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.request_update_check(s_ops.user_ctx);
}

esp_err_t mosaic_settings_get_snapshot(
    mosaic_settings_snapshot_t *ret_snapshot)
{
    if (ret_snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ret_snapshot, 0, sizeof(*ret_snapshot));
    if (s_ops.get_snapshot == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.get_snapshot(s_ops.user_ctx, ret_snapshot);
}

esp_err_t mosaic_settings_set_wifi_enabled(bool enabled)
{
    if (s_ops.set_wifi_enabled == NULL) {
        return ESP_OK;
    }
    return s_ops.set_wifi_enabled(s_ops.user_ctx, enabled);
}

bool mosaic_settings_wifi_backend_available(void)
{
    return s_ops.get_wifi_status != NULL &&
        s_ops.scan_wifi != NULL &&
        s_ops.connect_wifi != NULL;
}

esp_err_t mosaic_settings_request_wifi_scan(void)
{
    if (s_ops.request_wifi_scan == NULL) {
        /* Synchronous/fake providers expose their results through scan_wifi
         * directly and need no separate request operation. */
        return s_ops.scan_wifi != NULL ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.request_wifi_scan(s_ops.user_ctx);
}

esp_err_t mosaic_settings_scan_wifi(mosaic_settings_wifi_ap_t *records,
                                    size_t capacity, size_t *out_count)
{
    if (records == NULL || capacity == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops.scan_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.scan_wifi(
        s_ops.user_ctx, records, capacity, out_count);
}

esp_err_t mosaic_settings_connect_wifi(
    const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops.connect_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.connect_wifi(s_ops.user_ctx, ssid, password);
}

esp_err_t mosaic_settings_forget_wifi(void)
{
    if (s_ops.forget_wifi == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.forget_wifi(s_ops.user_ctx);
}

esp_err_t mosaic_settings_request_network_reconfigure(void)
{
    if (s_ops.request_network_reconfigure == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.request_network_reconfigure(s_ops.user_ctx);
}

esp_err_t mosaic_settings_get_phone_setup(
    char *ap_ssid, size_t ap_ssid_size,
    char *qr_payload, size_t qr_payload_size)
{
    if (ap_ssid == NULL || ap_ssid_size == 0U ||
            qr_payload == NULL || qr_payload_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ops.get_phone_setup == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_ops.get_phone_setup(
        s_ops.user_ctx, ap_ssid, ap_ssid_size,
        qr_payload, qr_payload_size);
}

esp_err_t mosaic_settings_get_battery(mosaic_settings_battery_t *ret_battery)
{
    if (ret_battery == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ret_battery, 0, sizeof(*ret_battery));

    MOSAIC_BATTERY_LOCK();
    const bool cached = s_battery_cache_valid;
    mosaic_settings_battery_t local = s_battery_cache;
    MOSAIC_BATTERY_UNLOCK();
    if (cached) {
        *ret_battery = local;
        return ESP_OK;
    }

    if (s_ops.get_battery != NULL) {
        esp_err_t err =
            s_ops.get_battery(s_ops.user_ctx, ret_battery);
        if (err == ESP_OK) {
            mosaic_settings_notify_battery(ret_battery);
        }
        return err;
    }
    if (s_ops.get_snapshot == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    mosaic_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = s_ops.get_snapshot(s_ops.user_ctx, snapshot);
    if (err == ESP_OK) {
        *ret_battery = snapshot->battery;
        mosaic_settings_notify_battery(ret_battery);
    }
    free(snapshot);
    return err;
}

void mosaic_settings_notify_battery(const mosaic_settings_battery_t *info)
{
    if (info == NULL) {
        return;
    }
    mosaic_settings_battery_t published = *info;
    mosaic_battery_subscriber_t listeners[MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX];
    size_t listener_count = 0;

    MOSAIC_BATTERY_LOCK();
    if (!s_battery_cache_valid) {
        published.sequence = 1U;
    } else if (published.sequence == 0U ||
               published.sequence <= s_battery_cache.sequence) {
        published.sequence = s_battery_cache.sequence + 1U;
    }
    s_battery_cache = published;
    s_battery_cache_valid = true;
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback != NULL) {
            listeners[listener_count++] = s_battery_subscribers[i];
        }
    }
    MOSAIC_BATTERY_UNLOCK();

    for (size_t i = 0; i < listener_count; ++i) {
        listeners[i].callback(&published, listeners[i].user_ctx);
    }
}

esp_err_t mosaic_settings_subscribe_battery(mosaic_battery_event_cb_t cb,
                                            void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool delivered = false;
    mosaic_settings_battery_t snapshot = {0};

    MOSAIC_BATTERY_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback == cb &&
                s_battery_subscribers[i].user_ctx == user_ctx) {
            if (s_battery_cache_valid) {
                snapshot = s_battery_cache;
                delivered = true;
            }
            MOSAIC_BATTERY_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback == NULL) {
            s_battery_subscribers[i].callback = cb;
            s_battery_subscribers[i].user_ctx = user_ctx;
            if (s_battery_cache_valid) {
                snapshot = s_battery_cache;
                delivered = true;
            }
            MOSAIC_BATTERY_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    MOSAIC_BATTERY_UNLOCK();
    return ESP_ERR_NO_MEM;
}

esp_err_t mosaic_settings_unsubscribe_battery(mosaic_battery_event_cb_t cb,
                                              void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    MOSAIC_BATTERY_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX; ++i) {
        if (s_battery_subscribers[i].callback == cb &&
                s_battery_subscribers[i].user_ctx == user_ctx) {
            s_battery_subscribers[i].callback = NULL;
            s_battery_subscribers[i].user_ctx = NULL;
            MOSAIC_BATTERY_UNLOCK();
            return ESP_OK;
        }
    }
    MOSAIC_BATTERY_UNLOCK();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mosaic_settings_get_wifi(mosaic_settings_network_t *ret_network)
{
    if (ret_network == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ret_network, 0, sizeof(*ret_network));

    /* Prefer the live provider so Hub can refresh RSSI without waiting for
     * a Wi-Fi state event. Cache is updated quietly (no subscriber fan-out). */
    if (s_ops.get_wifi_status != NULL) {
        esp_err_t err =
            s_ops.get_wifi_status(s_ops.user_ctx, ret_network);
        if (err == ESP_OK) {
            MOSAIC_WIFI_LOCK();
            s_wifi_cache = *ret_network;
            s_wifi_cache_valid = true;
            MOSAIC_WIFI_UNLOCK();
        }
        return err;
    }

    MOSAIC_WIFI_LOCK();
    const bool cached = s_wifi_cache_valid;
    mosaic_settings_network_t local = s_wifi_cache;
    MOSAIC_WIFI_UNLOCK();
    if (cached) {
        *ret_network = local;
        return ESP_OK;
    }
    if (s_ops.get_snapshot == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    mosaic_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = s_ops.get_snapshot(s_ops.user_ctx, snapshot);
    if (err == ESP_OK) {
        *ret_network = snapshot->network;
        mosaic_settings_notify_wifi(ret_network);
    }
    free(snapshot);
    return err;
}

void mosaic_settings_notify_wifi(const mosaic_settings_network_t *info)
{
    if (info == NULL) {
        return;
    }
    mosaic_settings_network_t published = *info;
    mosaic_wifi_subscriber_t listeners[MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX];
    size_t listener_count = 0;

    MOSAIC_WIFI_LOCK();
    s_wifi_cache = published;
    s_wifi_cache_valid = true;
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback != NULL) {
            listeners[listener_count++] = s_wifi_subscribers[i];
        }
    }
    MOSAIC_WIFI_UNLOCK();

    for (size_t i = 0; i < listener_count; ++i) {
        listeners[i].callback(&published, listeners[i].user_ctx);
    }
}

esp_err_t mosaic_settings_subscribe_wifi(mosaic_wifi_event_cb_t cb,
                                         void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool delivered = false;
    mosaic_settings_network_t snapshot = {0};

    MOSAIC_WIFI_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback == cb &&
                s_wifi_subscribers[i].user_ctx == user_ctx) {
            if (s_wifi_cache_valid) {
                snapshot = s_wifi_cache;
                delivered = true;
            }
            MOSAIC_WIFI_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback == NULL) {
            s_wifi_subscribers[i].callback = cb;
            s_wifi_subscribers[i].user_ctx = user_ctx;
            if (s_wifi_cache_valid) {
                snapshot = s_wifi_cache;
                delivered = true;
            }
            MOSAIC_WIFI_UNLOCK();
            if (delivered) {
                cb(&snapshot, user_ctx);
            }
            return ESP_OK;
        }
    }
    MOSAIC_WIFI_UNLOCK();
    return ESP_ERR_NO_MEM;
}

esp_err_t mosaic_settings_unsubscribe_wifi(mosaic_wifi_event_cb_t cb,
                                           void *user_ctx)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    MOSAIC_WIFI_LOCK();
    for (size_t i = 0; i < MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX; ++i) {
        if (s_wifi_subscribers[i].callback == cb &&
                s_wifi_subscribers[i].user_ctx == user_ctx) {
            s_wifi_subscribers[i].callback = NULL;
            s_wifi_subscribers[i].user_ctx = NULL;
            MOSAIC_WIFI_UNLOCK();
            return ESP_OK;
        }
    }
    MOSAIC_WIFI_UNLOCK();
    return ESP_ERR_NOT_FOUND;
}

bool mosaic_settings_llm_is_configured(
    const mosaic_settings_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->llm.backend[0] == '\0' ||
            snapshot->llm.model[0] == '\0') {
        return false;
    }

    return snapshot->llm.api_key_configured ||
           strcmp(snapshot->llm.backend, "trial") == 0;
}
