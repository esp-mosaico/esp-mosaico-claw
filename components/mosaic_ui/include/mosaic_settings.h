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

#define MOSAIC_SETTINGS_SSID_LEN       33U
#define MOSAIC_SETTINGS_IP_LEN         16U
#define MOSAIC_SETTINGS_PORTAL_URL_LEN 48U
#define MOSAIC_SETTINGS_LLM_BACKEND_LEN 32U
#define MOSAIC_SETTINGS_LLM_MODEL_LEN   64U
#define MOSAIC_SETTINGS_LLM_URL_LEN     160U
#define MOSAIC_SETTINGS_VERSION_LEN      32U
#define MOSAIC_SETTINGS_UPDATE_TITLE_LEN 96U
#define MOSAIC_SETTINGS_UPDATE_SUMMARY_LEN 256U
#define MOSAIC_SETTINGS_UPDATE_PUBLISHED_AT_LEN 40U
#define MOSAIC_SETTINGS_WIFI_SCAN_MAX   20U
#define MOSAIC_SETTINGS_PHONE_QR_LEN    512U
#define MOSAIC_SETTINGS_BATTERY_SUBSCRIBER_MAX 4U
#define MOSAIC_SETTINGS_WIFI_SUBSCRIBER_MAX    4U

typedef struct {
    char ssid[MOSAIC_SETTINGS_SSID_LEN];
    int8_t rssi;
    bool secured;
} mosaic_settings_wifi_ap_t;

typedef enum {
    MOSAIC_SETTINGS_WIFI_DISABLED = 0,
    MOSAIC_SETTINGS_WIFI_IDLE,
    MOSAIC_SETTINGS_WIFI_SCANNING,
    MOSAIC_SETTINGS_WIFI_CONNECTING,
    MOSAIC_SETTINGS_WIFI_CONNECTED,
    MOSAIC_SETTINGS_WIFI_RETRY_WAIT,
    MOSAIC_SETTINGS_WIFI_AUTH_FAILED,
    MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND,
    MOSAIC_SETTINGS_WIFI_FAILED,
} mosaic_settings_wifi_state_t;

typedef enum {
    MOSAIC_SETTINGS_WIFI_RADIO_OFF = 0,
    MOSAIC_SETTINGS_WIFI_RADIO_STARTING,
    MOSAIC_SETTINGS_WIFI_RADIO_ON,
    MOSAIC_SETTINGS_WIFI_RADIO_STOPPING,
    MOSAIC_SETTINGS_WIFI_RADIO_ERROR,
} mosaic_settings_wifi_radio_state_t;

typedef enum {
    MOSAIC_SETTINGS_WIFI_STA_UNCONFIGURED = 0,
    MOSAIC_SETTINGS_WIFI_STA_IDLE,
    MOSAIC_SETTINGS_WIFI_STA_CONNECTING,
    MOSAIC_SETTINGS_WIFI_STA_CONNECTED,
    MOSAIC_SETTINGS_WIFI_STA_RETRY_WAIT,
    MOSAIC_SETTINGS_WIFI_STA_AUTH_FAILED,
    MOSAIC_SETTINGS_WIFI_STA_AP_NOT_FOUND,
    MOSAIC_SETTINGS_WIFI_STA_ERROR,
} mosaic_settings_wifi_sta_state_t;

typedef enum {
    MOSAIC_SETTINGS_WIFI_SCAN_IDLE = 0,
    MOSAIC_SETTINGS_WIFI_SCAN_RUNNING,
    MOSAIC_SETTINGS_WIFI_SCAN_READY,
    MOSAIC_SETTINGS_WIFI_SCAN_ERROR,
} mosaic_settings_wifi_scan_state_t;

typedef struct {
    /** Requested switch position. `enabled` is the actual radio state. */
    bool desired_enabled;
    bool enabled;
    bool connected;
    bool configured;
    bool ap_active;
    char ssid[MOSAIC_SETTINGS_SSID_LEN];
    char ip[MOSAIC_SETTINGS_IP_LEN];
    char portal_url[MOSAIC_SETTINGS_PORTAL_URL_LEN];
    mosaic_settings_wifi_state_t state;
    mosaic_settings_wifi_radio_state_t radio_state;
    mosaic_settings_wifi_sta_state_t sta_state;
    mosaic_settings_wifi_scan_state_t scan_state;
    /** Monotonic completion counter for the Settings scan provider. */
    uint32_t scan_revision;
    /** Result associated with scan_revision; ESP_OK also covers zero APs. */
    esp_err_t scan_error;
    uint32_t operation_id;
    uint16_t disconnect_reason;
    esp_err_t last_error;
    /** STA RSSI in dBm when connected; 0 when unknown / disconnected. */
    int8_t rssi;
} mosaic_settings_network_t;

/** Immutable battery info snapshot for get()/subscriber callbacks. */
typedef struct {
    bool available;
    /** True when the gauge reports charge inbound. */
    bool charging;
    uint16_t state_of_charge;
    /** Stable gauge telemetry for the Battery details page. */
    uint16_t voltage_mv;
    int16_t current_ma;
    /** Gauge estimates in minutes; UINT16_MAX means unavailable/inapplicable. */
    uint16_t time_to_empty_min;
    uint16_t time_to_full_min;
    /** UINT16_MAX means the gauge did not provide a valid value. */
    uint16_t cycle_count;
    uint16_t state_of_health;
    /** Monotonic publish counter; bumps on every notify. */
    uint32_t sequence;
} mosaic_settings_battery_t;

typedef void (*mosaic_battery_event_cb_t)(
    const mosaic_settings_battery_t *info, void *user_ctx);

typedef void (*mosaic_wifi_event_cb_t)(
    const mosaic_settings_network_t *info, void *user_ctx);

typedef enum {
    MOSAIC_SETTINGS_UPDATE_IDLE = 0,
    MOSAIC_SETTINGS_UPDATE_CHECKING,
    MOSAIC_SETTINGS_UPDATE_UP_TO_DATE,
    MOSAIC_SETTINGS_UPDATE_AVAILABLE,
    MOSAIC_SETTINGS_UPDATE_DEVICE_AHEAD,
    MOSAIC_SETTINGS_UPDATE_FAILED,
} mosaic_settings_update_state_t;

typedef struct {
    mosaic_settings_update_state_t state;
    char latest_version[MOSAIC_SETTINGS_VERSION_LEN];
    char title[MOSAIC_SETTINGS_UPDATE_TITLE_LEN];
    char summary[MOSAIC_SETTINGS_UPDATE_SUMMARY_LEN];
    char published_at[MOSAIC_SETTINGS_UPDATE_PUBLISHED_AT_LEN];
    uint32_t sequence;
    esp_err_t last_error;
} mosaic_settings_update_t;

typedef struct {
    char software_version[MOSAIC_SETTINGS_VERSION_LEN];
    uint16_t rotation;
    int brightness;
    int volume;
    uint32_t screen_timeout_ms;
    bool vibration_enabled;
    bool display_available;
    bool audio_available;
    mosaic_settings_network_t network;
    struct {
        bool wechat_configured;
        bool qq_configured;
        bool feishu_configured;
        bool telegram_configured;
    } im;
    struct {
        char backend[MOSAIC_SETTINGS_LLM_BACKEND_LEN];
        char model[MOSAIC_SETTINGS_LLM_MODEL_LEN];
        char base_url[MOSAIC_SETTINGS_LLM_URL_LEN];
        bool api_key_configured;
        bool supports_tools;
        bool supports_vision;
    } llm;
    mosaic_settings_battery_t battery;
    mosaic_settings_update_t update;
} mosaic_settings_snapshot_t;

typedef struct {
    esp_err_t (*get_snapshot)(void *user_ctx,
                              mosaic_settings_snapshot_t *ret_snapshot);
    esp_err_t (*set_rotation)(void *user_ctx, uint16_t degrees);
    esp_err_t (*set_brightness)(void *user_ctx, int brightness, bool persist);
    esp_err_t (*set_volume)(void *user_ctx, int volume, bool persist);
    esp_err_t (*set_vibration)(void *user_ctx, bool enabled);
    esp_err_t (*set_screen_timeout)(void *user_ctx, uint32_t timeout_ms);
    esp_err_t (*factory_reset)(void *user_ctx);
    /** Start a metadata-only asynchronous update manifest check. */
    esp_err_t (*request_update_check)(void *user_ctx);
    esp_err_t (*set_wifi_enabled)(void *user_ctx, bool enabled);
    /** Optional cached/live battery read. When NULL, get_battery falls back
     * to the last notify cache, then get_snapshot. */
    esp_err_t (*get_battery)(void *user_ctx,
                             mosaic_settings_battery_t *ret_battery);
    /** Read only the live Wi-Fi state without collecting the full Settings
     * snapshot (battery/config providers may block). Fields such as the
     * configured SSID may be retained from the caller's current model. */
    esp_err_t (*get_wifi_status)(void *user_ctx,
                                 mosaic_settings_network_t *inout_network);
    /** Start one asynchronous scan. Results are later read through
     * scan_wifi; providers without an async implementation may leave this
     * NULL and perform their scan in scan_wifi. */
    esp_err_t (*request_wifi_scan)(void *user_ctx);
    esp_err_t (*scan_wifi)(void *user_ctx, mosaic_settings_wifi_ap_t *records,
                           size_t capacity, size_t *out_count);
    esp_err_t (*connect_wifi)(void *user_ctx, const char *ssid,
                              const char *password);
    esp_err_t (*forget_wifi)(void *user_ctx);
    /** Read the live provisioning AP identity and its Wi-Fi join QR. */
    esp_err_t (*get_phone_setup)(void *user_ctx,
                                 char *ap_ssid, size_t ap_ssid_size,
                                 char *qr_payload, size_t qr_payload_size);
    esp_err_t (*request_network_reconfigure)(void *user_ctx);
    void *user_ctx;
} mosaic_settings_ops_t;

esp_err_t mosaic_settings_configure(const mosaic_settings_ops_t *ops);
esp_err_t mosaic_settings_set_brightness(int brightness, bool persist);
esp_err_t mosaic_settings_set_volume(int volume, bool persist);
esp_err_t mosaic_settings_set_vibration(bool enabled);
esp_err_t mosaic_settings_set_screen_timeout(uint32_t timeout_ms);
esp_err_t mosaic_settings_factory_reset(void);
esp_err_t mosaic_settings_request_update_check(void);
esp_err_t mosaic_settings_set_wifi_enabled(bool enabled);

/** Read the shared Settings model used by Settings and setup flows. */
esp_err_t mosaic_settings_get_snapshot(
    mosaic_settings_snapshot_t *ret_snapshot);

/** Trial obtains credentials on-device; other LLM backends need an API key. */
bool mosaic_settings_llm_is_configured(
    const mosaic_settings_snapshot_t *snapshot);

/** Shared Wi-Fi service facade used by Settings and setup flows. */
bool mosaic_settings_wifi_backend_available(void);
esp_err_t mosaic_settings_request_wifi_scan(void);
esp_err_t mosaic_settings_scan_wifi(mosaic_settings_wifi_ap_t *records,
                                    size_t capacity, size_t *out_count);
esp_err_t mosaic_settings_connect_wifi(const char *ssid,
                                       const char *password);
esp_err_t mosaic_settings_forget_wifi(void);
/** Read the provisioning AP name and Wi-Fi join QR used by phone setup. */
esp_err_t mosaic_settings_get_phone_setup(
    char *ap_ssid, size_t ap_ssid_size,
    char *qr_payload, size_t qr_payload_size);

/** Sync get of the latest battery info (cache / provider). */
esp_err_t mosaic_settings_get_battery(mosaic_settings_battery_t *ret_battery);

/** Subscribe to battery publishes. Registration immediately delivers the
 * current cache when valid. Callbacks must not touch GSP directly. */
esp_err_t mosaic_settings_subscribe_battery(mosaic_battery_event_cb_t cb,
                                            void *user_ctx);
esp_err_t mosaic_settings_unsubscribe_battery(mosaic_battery_event_cb_t cb,
                                              void *user_ctx);

/** Platform publishes a new sample; fans out to subscribers. */
void mosaic_settings_notify_battery(const mosaic_settings_battery_t *info);

/** Sync get of the latest Wi-Fi network info (cache / provider). */
esp_err_t mosaic_settings_get_wifi(mosaic_settings_network_t *ret_network);

/** Subscribe to Wi-Fi publishes. Registration immediately delivers the
 * current cache when valid. Callbacks must not touch GSP directly. */
esp_err_t mosaic_settings_subscribe_wifi(mosaic_wifi_event_cb_t cb,
                                         void *user_ctx);
esp_err_t mosaic_settings_unsubscribe_wifi(mosaic_wifi_event_cb_t cb,
                                           void *user_ctx);

/** Platform publishes a new Wi-Fi sample; fans out to subscribers. */
void mosaic_settings_notify_wifi(const mosaic_settings_network_t *info);

#ifdef __cplusplus
}
#endif
