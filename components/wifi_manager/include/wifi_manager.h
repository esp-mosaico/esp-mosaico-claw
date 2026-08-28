/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MANAGER_STATE_DISABLED = 0,
    WIFI_MANAGER_STATE_IDLE,
    WIFI_MANAGER_STATE_SCANNING,
    WIFI_MANAGER_STATE_CONNECTING,
    WIFI_MANAGER_STATE_CONNECTED,
    WIFI_MANAGER_STATE_RETRY_WAIT,
    WIFI_MANAGER_STATE_AUTH_FAILED,
    WIFI_MANAGER_STATE_AP_NOT_FOUND,
    WIFI_MANAGER_STATE_FAILED,
} wifi_manager_state_t;

typedef enum {
    WIFI_MANAGER_RADIO_OFF = 0,
    WIFI_MANAGER_RADIO_STARTING,
    WIFI_MANAGER_RADIO_ON,
    WIFI_MANAGER_RADIO_STOPPING,
    WIFI_MANAGER_RADIO_ERROR,
} wifi_manager_radio_state_t;

typedef enum {
    WIFI_MANAGER_STA_UNCONFIGURED = 0,
    WIFI_MANAGER_STA_IDLE,
    WIFI_MANAGER_STA_CONNECTING,
    WIFI_MANAGER_STA_CONNECTED,
    WIFI_MANAGER_STA_RETRY_WAIT,
    WIFI_MANAGER_STA_AUTH_FAILED,
    WIFI_MANAGER_STA_AP_NOT_FOUND,
    WIFI_MANAGER_STA_ERROR,
} wifi_manager_sta_state_t;

typedef enum {
    WIFI_MANAGER_SCAN_IDLE = 0,
    WIFI_MANAGER_SCAN_RUNNING,
    WIFI_MANAGER_SCAN_READY,
    WIFI_MANAGER_SCAN_ERROR,
} wifi_manager_scan_state_t;

typedef void (*wifi_manager_state_cb_t)(bool connected, void *user_ctx);

typedef struct {
    const char *sta_ssid;
    const char *sta_password;
    const char *ap_ssid_prefix;
    const char *ap_ssid;
    const char *ap_password;
    const char *ap_behavior;
    uint8_t ap_channel;
    uint8_t ap_max_conn;
    /** Load configuration but keep the radio off until set_enabled(true). */
    bool start_disabled;
} wifi_manager_config_t;

typedef struct {
    bool desired_enabled;
    bool enabled;
    bool sta_connected;
    bool ap_active;
    bool sta_configured;
    const char *sta_ssid;
    const char *sta_ip;
    const char *ap_ip;
    const char *ap_ssid;
    const char *mode;
    wifi_manager_state_t state;
    wifi_manager_radio_state_t radio_state;
    wifi_manager_sta_state_t sta_state;
    wifi_manager_scan_state_t scan_state;
    uint32_t operation_id;
    uint16_t disconnect_reason;
    int8_t rssi;
    esp_err_t last_error;
} wifi_manager_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_manager_scan_record_t;

/** Structured, immutable snapshot delivered for every Wi-Fi state change.
 * String storage belongs to the event and is valid for the callback duration.
 */
typedef struct {
    uint32_t sequence;
    uint32_t operation_id;
    wifi_manager_state_t state;
    wifi_manager_radio_state_t radio_state;
    wifi_manager_sta_state_t sta_state;
    wifi_manager_scan_state_t scan_state;
    bool desired_enabled;
    bool enabled;
    bool sta_connected;
    bool sta_configured;
    bool ap_active;
    uint16_t disconnect_reason;
    int8_t rssi;
    esp_err_t last_error;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ssid[33];
    char ap_ip[16];
    char mode[16];
} wifi_manager_event_t;

typedef void (*wifi_manager_event_cb_t)(const wifi_manager_event_t *event,
                                        void *user_ctx);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(const wifi_manager_config_t *config);
esp_err_t wifi_manager_apply_sta_config(const wifi_manager_config_t *config);
/** Queue a radio enable/disable request. Completion and failure are reported
 * through wifi_manager_event_t; this function does not wait for the driver. */
esp_err_t wifi_manager_set_enabled(bool enabled);
esp_err_t wifi_manager_validate_config(const wifi_manager_config_t *config);
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);
esp_err_t wifi_manager_register_state_callback(wifi_manager_state_cb_t cb, void *user_ctx);
/** Add/remove independent event subscribers. Unlike the legacy state callback,
 * registering one listener does not replace other consumers. Registration
 * immediately publishes the current snapshot to the new listener. */
esp_err_t wifi_manager_register_event_callback(wifi_manager_event_cb_t cb,
                                                void *user_ctx);
esp_err_t wifi_manager_unregister_event_callback(wifi_manager_event_cb_t cb,
                                                  void *user_ctx);
void wifi_manager_get_status(wifi_manager_status_t *status);
const char *wifi_manager_state_string(wifi_manager_state_t state);
esp_netif_t *wifi_manager_get_ap_netif(void);
esp_err_t wifi_manager_scan_aps(wifi_manager_scan_record_t *records,
                                uint16_t max_records,
                                uint16_t *out_count);

#ifdef __cplusplus
}
#endif
