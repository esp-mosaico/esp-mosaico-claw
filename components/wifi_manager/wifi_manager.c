/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_manager.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted_misc.h"
#endif

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1

#ifndef CONFIG_APP_WIFI_AP_SSID_PREFIX
#define CONFIG_APP_WIFI_AP_SSID_PREFIX "esp-claw"
#endif
#ifndef CONFIG_APP_WIFI_AP_CHANNEL
#define CONFIG_APP_WIFI_AP_CHANNEL 1
#endif
#ifndef CONFIG_APP_WIFI_AP_MAX_CONN
#define CONFIG_APP_WIFI_AP_MAX_CONN 4
#endif
/*
 * Fixed reconnect cadence. Whenever the STA interface disconnects (or the
 * connect attempt fails), we schedule the next attempt this many ms later
 * and just keep trying. There is no retry counter, no exponential backoff
 * and no "give up" state: the AP is always reachable while STA is not
 * connected, so users can always fix credentials via the portal.
 */
#ifndef CONFIG_APP_WIFI_RETRY_MS
#define CONFIG_APP_WIFI_RETRY_MS 10000
#endif
#define WIFI_RETRY_MS CONFIG_APP_WIFI_RETRY_MS
#define WIFI_MANAGER_COMMAND_DEPTH 12
#define WIFI_MANAGER_TASK_STACK 5120
#define WIFI_MANAGER_TASK_PRIORITY 5

typedef enum {
    WM_STATE_OFF = 0,
    WM_STATE_PROVISION_AP, /* STA not configured, AP only. */
    WM_STATE_APSTA,        /* STA configured; AP + STA both up. */
    WM_STATE_STA_ONLY,     /* STA connected + close_on_sta closed the AP. */
} wifi_mode_state_t;

static EventGroupHandle_t s_wifi_event_group;
static bool s_connected;
static bool s_ap_active;
static bool s_sta_configured;
static EXT_RAM_BSS_ATTR char s_ip_addr[16];
static EXT_RAM_BSS_ATTR char s_ap_ip[16];
static EXT_RAM_BSS_ATTR char s_ap_ssid[33];
static EXT_RAM_BSS_ATTR char s_sta_ssid[33];
static EXT_RAM_BSS_ATTR char s_sta_password[65];
static EXT_RAM_BSS_ATTR char s_ap_ssid_override[33];
static EXT_RAM_BSS_ATTR char s_ap_password[65];
static EXT_RAM_BSS_ATTR char s_ap_behavior[16];
static EXT_RAM_BSS_ATTR char s_ap_ssid_prefix[33];
static wifi_mode_state_t s_mode = WM_STATE_OFF;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_manager_state_cb_t s_state_cb;
static void *s_state_cb_user_ctx;
#define WIFI_MANAGER_EVENT_SUBSCRIBER_MAX 4
typedef struct {
    wifi_manager_event_cb_t callback;
    void *user_ctx;
} wifi_manager_event_subscriber_t;
static wifi_manager_event_subscriber_t
    EXT_RAM_BSS_ATTR s_event_subscribers[WIFI_MANAGER_EVENT_SUBSCRIBER_MAX];
static uint32_t s_event_sequence;
static uint32_t s_operation_id;
static esp_timer_handle_t s_reconnect_timer;
static EXT_RAM_BSS_ATTR wifi_manager_config_t s_config;
static bool s_wifi_started;
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_DISABLED;
static uint16_t s_disconnect_reason;
static int8_t s_sta_rssi;
static bool s_desired_enabled;
static wifi_manager_radio_state_t s_radio_state = WIFI_MANAGER_RADIO_OFF;
static wifi_manager_sta_state_t s_sta_state = WIFI_MANAGER_STA_UNCONFIGURED;
static wifi_manager_scan_state_t s_scan_state = WIFI_MANAGER_SCAN_IDLE;
/* A credential replacement has not yet reached GOT_IP or a terminal
 * authentication/discovery failure.  Ordinary post-connect disconnects do
 * not set this flag and keep the normal background reconnect policy. */
static bool s_sta_config_validation_pending;
/* esp_wifi_disconnect() used while replacing a live configuration emits one
 * STA_DISCONNECTED event for the old association.  It belongs to the handoff,
 * not to validation of the new credentials. */
static bool s_sta_config_handoff_disconnect_pending;
static esp_err_t s_last_error;
static QueueHandle_t s_command_queue;

typedef enum {
    WIFI_MANAGER_CMD_START,
    WIFI_MANAGER_CMD_APPLY_CONFIG,
    WIFI_MANAGER_CMD_SET_ENABLED,
    WIFI_MANAGER_CMD_SCAN,
    WIFI_MANAGER_CMD_SCAN_DONE,
    WIFI_MANAGER_CMD_RETRY,
    WIFI_MANAGER_CMD_STA_START,
    WIFI_MANAGER_CMD_STA_DISCONNECTED,
    WIFI_MANAGER_CMD_AP_START,
    WIFI_MANAGER_CMD_AP_STOP,
    WIFI_MANAGER_CMD_GOT_IP,
} wifi_manager_command_kind_t;

typedef struct {
    wifi_manager_command_kind_t kind;
    bool enabled;
    uint16_t reason;
    char ip[16];
    wifi_manager_config_t config;
    char sta_ssid[33];
    char sta_password[65];
    char ap_ssid_prefix[33];
    char ap_ssid[33];
    char ap_password[65];
    char ap_behavior[16];
    wifi_manager_scan_record_t *scan_records;
    uint16_t scan_capacity;
    uint16_t *scan_count;
    SemaphoreHandle_t completion;
    esp_err_t *result;
} wifi_manager_command_t;

static bool s_scan_pending;
static bool s_scan_resume_sta;
static EXT_RAM_BSS_ATTR wifi_manager_command_t s_pending_scan;
static wifi_mode_t s_scan_original_mode = WIFI_MODE_NULL;
static wifi_mode_t s_scan_active_mode = WIFI_MODE_NULL;

static void notify_state_changed(bool force);
static esp_err_t configure_sta_mode(const wifi_manager_config_t *config);
static void reset_sta_runtime_state(void);
static void reconnect_timer_cb(void *arg);
static void arm_reconnect(void);
static void reopen_ap_if_needed(void);
static void wifi_manager_worker(void *arg);
static esp_err_t submit_command(wifi_manager_command_t *command,
                                bool wait);
static esp_err_t begin_scan_on_worker(
    const wifi_manager_command_t *command);
static esp_err_t finish_scan_on_worker(void);
static void cancel_scan_on_worker(esp_err_t reason);

static const char *wifi_manager_mode_string(wifi_mode_state_t mode)
{
    switch (mode) {
    case WM_STATE_PROVISION_AP: return "provision";
    case WM_STATE_APSTA:        return "apsta";
    case WM_STATE_STA_ONLY:     return "sta_only";
    default:                    return "off";
    }
}

const char *wifi_manager_state_string(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_STATE_DISABLED: return "disabled";
    case WIFI_MANAGER_STATE_IDLE: return "idle";
    case WIFI_MANAGER_STATE_SCANNING: return "scanning";
    case WIFI_MANAGER_STATE_CONNECTING: return "connecting";
    case WIFI_MANAGER_STATE_CONNECTED: return "connected";
    case WIFI_MANAGER_STATE_RETRY_WAIT: return "retry_wait";
    case WIFI_MANAGER_STATE_AUTH_FAILED: return "auth_failed";
    case WIFI_MANAGER_STATE_AP_NOT_FOUND: return "ap_not_found";
    default: return "failed";
    }
}

static wifi_manager_state_t legacy_state(void)
{
    if (s_radio_state == WIFI_MANAGER_RADIO_OFF ||
            s_radio_state == WIFI_MANAGER_RADIO_STOPPING) {
        return WIFI_MANAGER_STATE_DISABLED;
    }
    if (s_scan_state == WIFI_MANAGER_SCAN_RUNNING) {
        return WIFI_MANAGER_STATE_SCANNING;
    }
    switch (s_sta_state) {
    case WIFI_MANAGER_STA_UNCONFIGURED:
    case WIFI_MANAGER_STA_IDLE:
        return WIFI_MANAGER_STATE_IDLE;
    case WIFI_MANAGER_STA_CONNECTING:
        return WIFI_MANAGER_STATE_CONNECTING;
    case WIFI_MANAGER_STA_CONNECTED:
        return WIFI_MANAGER_STATE_CONNECTED;
    case WIFI_MANAGER_STA_RETRY_WAIT:
        return WIFI_MANAGER_STATE_RETRY_WAIT;
    case WIFI_MANAGER_STA_AUTH_FAILED:
        return WIFI_MANAGER_STATE_AUTH_FAILED;
    case WIFI_MANAGER_STA_AP_NOT_FOUND:
        return WIFI_MANAGER_STATE_AP_NOT_FOUND;
    default:
        return WIFI_MANAGER_STATE_FAILED;
    }
}

static wifi_manager_sta_state_t sta_state_from_disconnect_reason(
    uint16_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return WIFI_MANAGER_STA_AUTH_FAILED;
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WIFI_MANAGER_STA_AP_NOT_FOUND;
    default:
        return WIFI_MANAGER_STA_RETRY_WAIT;
    }
}

static bool wifi_manager_ap_behavior_is_valid(const char *ap_behavior)
{
    return !ap_behavior || ap_behavior[0] == '\0' ||
           strcmp(ap_behavior, "keep") == 0 ||
           strcmp(ap_behavior, "close_on_sta") == 0;
}

static void copy_owned_string(char *dst, size_t dst_size, const char *src)
{
    strlcpy(dst, src ? src : "", dst_size);
}

static void sync_owned_config(const wifi_manager_config_t *config)
{
    copy_owned_string(s_sta_ssid, sizeof(s_sta_ssid), config->sta_ssid);
    copy_owned_string(s_sta_password, sizeof(s_sta_password), config->sta_password);
    copy_owned_string(s_ap_ssid_prefix, sizeof(s_ap_ssid_prefix), config->ap_ssid_prefix);
    copy_owned_string(s_ap_ssid_override, sizeof(s_ap_ssid_override), config->ap_ssid);
    copy_owned_string(s_ap_password, sizeof(s_ap_password), config->ap_password);
    copy_owned_string(s_ap_behavior, sizeof(s_ap_behavior), config->ap_behavior);

    s_config = *config;
    s_config.sta_ssid = s_sta_ssid[0] ? s_sta_ssid : NULL;
    s_config.sta_password = s_sta_password[0] ? s_sta_password : NULL;
    s_config.ap_ssid_prefix = s_ap_ssid_prefix[0] ? s_ap_ssid_prefix : NULL;
    s_config.ap_ssid = s_ap_ssid_override[0] ? s_ap_ssid_override : NULL;
    s_config.ap_password = s_ap_password[0] ? s_ap_password : NULL;
    s_config.ap_behavior = s_ap_behavior[0] ? s_ap_behavior : NULL;
}

static const char *wifi_manager_ap_ssid_prefix(void)
{
    return (s_config.ap_ssid_prefix && s_config.ap_ssid_prefix[0] != '\0')
           ? s_config.ap_ssid_prefix : CONFIG_APP_WIFI_AP_SSID_PREFIX;
}

static uint8_t wifi_manager_ap_channel(void)
{
    return s_config.ap_channel ? s_config.ap_channel : CONFIG_APP_WIFI_AP_CHANNEL;
}

static uint8_t wifi_manager_ap_max_conn(void)
{
    return s_config.ap_max_conn ? s_config.ap_max_conn : CONFIG_APP_WIFI_AP_MAX_CONN;
}

static bool wifi_manager_sta_password_is_set(void)
{
    return s_config.sta_password && s_config.sta_password[0] != '\0';
}

static bool wifi_manager_close_on_sta(void)
{
    return s_config.ap_behavior && strcmp(s_config.ap_behavior, "close_on_sta") == 0;
}

static void compose_ap_ssid(void)
{
    if (s_config.ap_ssid && s_config.ap_ssid[0] != '\0') {
        strlcpy(s_ap_ssid, s_config.ap_ssid, sizeof(s_ap_ssid));
        ESP_LOGI(TAG, "Custom AP SSID: %s", s_ap_ssid);
        return;
    }
    uint8_t mac[6] = {0};
#ifdef CONFIG_IDF_TARGET_ESP32P4
    size_t mac_len = esp_hosted_iface_mac_addr_len_get(ESP_MAC_WIFI_SOFTAP);
    esp_err_t ret = esp_hosted_iface_mac_addr_get(mac, mac_len, ESP_MAC_WIFI_SOFTAP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP MAC address: %s", esp_err_to_name(ret));
    }
#else
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
#endif
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X",
             wifi_manager_ap_ssid_prefix(), mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Provisioning AP SSID: %s", s_ap_ssid);
}

static esp_err_t apply_ap_config(void)
{
    wifi_config_t ap_cfg = {0};
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_cfg.ap.ssid_len);
    ap_cfg.ap.channel = wifi_manager_ap_channel();
    ap_cfg.ap.max_connection = wifi_manager_ap_max_conn();
    if (s_config.ap_password && s_config.ap_password[0] != '\0') {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_cfg.ap.password, s_config.ap_password, sizeof(ap_cfg.ap.password));
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    return esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static void refresh_ap_ip_str(void)
{
    if (!s_ap_netif) return;
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip_info.ip));
    }
}

static void reset_sta_runtime_state(void)
{
    strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
    s_connected = false;
    s_disconnect_reason = 0;
    xEventGroupClearBits(s_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_FAILED_BIT);
}

esp_err_t wifi_manager_validate_config(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->sta_ssid && config->sta_ssid[0] != '\0') {
        if (strlen(config->sta_ssid) >= sizeof(((wifi_config_t *)0)->sta.ssid)) return ESP_ERR_INVALID_ARG;
    }
    if (config->sta_password && config->sta_password[0] != '\0') {
        size_t n = strlen(config->sta_password);
        if (n < 8 || n >= sizeof(((wifi_config_t *)0)->sta.password)) return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_password && config->ap_password[0] != '\0') {
        size_t n = strlen(config->ap_password);
        if (n < 8 || n >= sizeof(((wifi_config_t *)0)->ap.password)) return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_ssid && strlen(config->ap_ssid) > sizeof(((wifi_config_t *)0)->ap.ssid)) return ESP_ERR_INVALID_ARG;
    if (config->ap_ssid_prefix && strlen(config->ap_ssid_prefix) >= sizeof(s_ap_ssid) - 7) return ESP_ERR_INVALID_ARG;
    if (!wifi_manager_ap_behavior_is_valid(config->ap_behavior)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t configure_sta_mode(const wifi_manager_config_t *config)
{
    esp_err_t err = wifi_manager_validate_config(config);
    if (err != ESP_OK) return err;

    /* worker_start_radio() reuses the already-owned config. Avoid an
     * overlapping strlcpy(dst, dst) in that path. */
    if (config != &s_config) {
        sync_owned_config(config);
    }
    compose_ap_ssid();
    s_sta_configured = (s_config.sta_ssid && s_config.sta_ssid[0] != '\0');
    ESP_LOGI(TAG, "Applying Wi-Fi config: sta_configured=%d sta_ssid_len=%u sta_password_empty=%d ap_password_empty=%d ap_behavior=%s",
             s_sta_configured,
             (unsigned)(s_config.sta_ssid ? strlen(s_config.sta_ssid) : 0U),
             wifi_manager_sta_password_is_set() ? 0 : 1,
             (s_config.ap_password && s_config.ap_password[0] != '\0') ? 0 : 1,
             s_config.ap_behavior ? s_config.ap_behavior : "keep");

    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    reset_sta_runtime_state();

    if (s_sta_configured) {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid, s_config.sta_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password,
                s_config.sta_password ? s_config.sta_password : "",
                sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = wifi_manager_sta_password_is_set() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;

        s_mode = WM_STATE_APSTA;
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
        err = apply_ap_config();
        if (err != ESP_OK) return err;
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) return err;
        return ESP_OK;
    }

    s_mode = WM_STATE_PROVISION_AP;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    return apply_ap_config();
}

static void arm_reconnect(void)
{
    if (!s_reconnect_timer) return;
    esp_timer_stop(s_reconnect_timer);
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)WIFI_RETRY_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm reconnect timer: %s", esp_err_to_name(err));
    }
}

/*
 * When close_on_sta closed the AP after a successful connect and STA later
 * drops, bring the AP back so users can always fall through to the portal.
 * Called only from the disconnect path.
 */
static void reopen_ap_if_needed(void)
{
    if (s_mode != WM_STATE_STA_ONLY) return;
    ESP_LOGI(TAG, "Reopening AP after STA disconnect (was closed via close_on_sta)");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reopen AP: %s", esp_err_to_name(err));
        return;
    }
    err = apply_ap_config();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore AP config: %s",
                 esp_err_to_name(err));
        return;
    }
    s_mode = WM_STATE_APSTA;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    wifi_manager_command_t command = {0};
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            command.kind = WIFI_MANAGER_CMD_STA_START;
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disc = event_data;
            command.kind = WIFI_MANAGER_CMD_STA_DISCONNECTED;
            command.reason = disc ? disc->reason : 0;
            break;
        }
        case WIFI_EVENT_AP_START:
            command.kind = WIFI_MANAGER_CMD_AP_START;
            break;
        case WIFI_EVENT_AP_STOP:
            command.kind = WIFI_MANAGER_CMD_AP_STOP;
            break;
        case WIFI_EVENT_SCAN_DONE:
            command.kind = WIFI_MANAGER_CMD_SCAN_DONE;
            break;
        default:
            return;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        command.kind = WIFI_MANAGER_CMD_GOT_IP;
        snprintf(command.ip, sizeof(command.ip), IPSTR,
                 IP2STR(&event->ip_info.ip));
    } else {
        return;
    }
    if (s_command_queue == NULL ||
            xQueueSend(s_command_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "drop Wi-Fi event %ld: worker queue full",
                 (long)event_id);
    }
}

static void notify_state_changed(bool force)
{
    static bool s_last_connected;
    static bool s_last_ap_active;
    static bool s_initialized;

    if (!force && s_initialized && s_last_connected == s_connected && s_last_ap_active == s_ap_active) return;
    s_last_connected = s_connected;
    s_last_ap_active = s_ap_active;
    s_initialized = true;
    if (s_state_cb) s_state_cb(s_connected, s_state_cb_user_ctx);

    s_state = legacy_state();
    wifi_manager_event_t event = {
        .sequence = ++s_event_sequence,
        .operation_id = s_operation_id,
        .state = s_state,
        .radio_state = s_radio_state,
        .sta_state = s_sta_state,
        .scan_state = s_scan_state,
        .desired_enabled = s_desired_enabled,
        .enabled = s_wifi_started,
        .sta_connected = s_connected,
        .sta_configured = s_sta_configured,
        .ap_active = s_ap_active,
        .disconnect_reason = s_disconnect_reason,
        .rssi = s_sta_rssi,
        .last_error = s_last_error,
    };
    strlcpy(event.sta_ssid, s_sta_ssid, sizeof(event.sta_ssid));
    strlcpy(event.sta_ip, s_ip_addr, sizeof(event.sta_ip));
    strlcpy(event.ap_ssid, s_ap_ssid, sizeof(event.ap_ssid));
    strlcpy(event.ap_ip, s_ap_ip, sizeof(event.ap_ip));
    strlcpy(event.mode, wifi_manager_mode_string(s_mode), sizeof(event.mode));
    for (size_t i = 0; i < WIFI_MANAGER_EVENT_SUBSCRIBER_MAX; ++i) {
        if (s_event_subscribers[i].callback != NULL) {
            s_event_subscribers[i].callback(
                &event, s_event_subscribers[i].user_ctx);
        }
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    const wifi_manager_command_t command = {
        .kind = WIFI_MANAGER_CMD_RETRY,
    };
    if (s_command_queue != NULL) {
        (void)xQueueSend(s_command_queue, &command, 0);
    }
}

static void command_copy_config(wifi_manager_command_t *command,
                                const wifi_manager_config_t *config)
{
    command->config = *config;
#define COPY_CONFIG_FIELD(field) do {                                      \
        strlcpy(command->field, config->field ? config->field : "",       \
                sizeof(command->field));                                   \
        command->config.field = command->field[0] ? command->field : NULL; \
    } while (0)
    COPY_CONFIG_FIELD(sta_ssid);
    COPY_CONFIG_FIELD(sta_password);
    COPY_CONFIG_FIELD(ap_ssid_prefix);
    COPY_CONFIG_FIELD(ap_ssid);
    COPY_CONFIG_FIELD(ap_password);
    COPY_CONFIG_FIELD(ap_behavior);
#undef COPY_CONFIG_FIELD
}

static esp_err_t submit_command(wifi_manager_command_t *command, bool wait)
{
    if (s_command_queue == NULL || command == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = ESP_OK;
    SemaphoreHandle_t completion = NULL;
    if (wait) {
        completion = xSemaphoreCreateBinary();
        if (completion == NULL) {
            return ESP_ERR_NO_MEM;
        }
        command->completion = completion;
        command->result = &result;
    }
    if (xQueueSend(s_command_queue, command, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (completion != NULL) {
            vSemaphoreDelete(completion);
        }
        return ESP_ERR_TIMEOUT;
    }
    if (!wait) {
        return ESP_OK;
    }
    if (xSemaphoreTake(completion, pdMS_TO_TICKS(30000)) != pdTRUE) {
        /* The command contains pointers into the caller's stack, so a timeout
         * cannot safely return while the worker may still touch them. */
        xSemaphoreTake(completion, portMAX_DELAY);
    }
    vSemaphoreDelete(completion);
    return result;
}

static void complete_command(wifi_manager_command_t *command,
                             esp_err_t result)
{
    if (command->result != NULL) {
        *command->result = result;
    }
    if (command->completion != NULL) {
        xSemaphoreGive(command->completion);
    }
}

static esp_err_t worker_start_radio(void)
{
    if (s_wifi_started) {
        s_radio_state = WIFI_MANAGER_RADIO_ON;
        notify_state_changed(true);
        return ESP_OK;
    }
    s_radio_state = WIFI_MANAGER_RADIO_STARTING;
    s_last_error = ESP_OK;
    notify_state_changed(true);
    esp_err_t err = configure_sta_mode(&s_config);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        s_wifi_started = true;
        s_radio_state = WIFI_MANAGER_RADIO_ON;
        s_sta_state = s_sta_configured ? WIFI_MANAGER_STA_CONNECTING
                                       : WIFI_MANAGER_STA_UNCONFIGURED;
    } else {
        s_radio_state = WIFI_MANAGER_RADIO_ERROR;
        s_sta_state = WIFI_MANAGER_STA_ERROR;
        s_last_error = err;
    }
    notify_state_changed(true);
    return err;
}

static esp_err_t worker_stop_radio(void)
{
    cancel_scan_on_worker(ESP_ERR_INVALID_STATE);
    if (!s_wifi_started) {
        s_radio_state = WIFI_MANAGER_RADIO_OFF;
        s_sta_state = s_sta_configured ? WIFI_MANAGER_STA_IDLE
                                       : WIFI_MANAGER_STA_UNCONFIGURED;
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
        notify_state_changed(true);
        return ESP_OK;
    }
    if (s_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_reconnect_timer);
    }
    s_radio_state = WIFI_MANAGER_RADIO_STOPPING;
    s_sta_config_validation_pending = false;
    s_sta_config_handoff_disconnect_pending = false;
    s_scan_state = WIFI_MANAGER_SCAN_IDLE;
    notify_state_changed(true);
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_OK) {
        s_wifi_started = false;
        s_connected = false;
        s_ap_active = false;
        s_mode = WM_STATE_OFF;
        s_radio_state = WIFI_MANAGER_RADIO_OFF;
        s_sta_state = s_sta_configured ? WIFI_MANAGER_STA_IDLE
                                       : WIFI_MANAGER_STA_UNCONFIGURED;
        s_disconnect_reason = 0;
        s_sta_rssi = 0;
        strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
    } else {
        s_radio_state = WIFI_MANAGER_RADIO_ERROR;
        s_last_error = err;
    }
    notify_state_changed(true);
    return err;
}

static esp_err_t worker_apply_config(
    const wifi_manager_config_t *config, bool validate_credentials)
{
    cancel_scan_on_worker(ESP_ERR_INVALID_STATE);
    const bool was_connected = s_connected;
    const char *next_ssid = config->sta_ssid ? config->sta_ssid : "";
    const char *next_password =
        config->sta_password ? config->sta_password : "";
    const bool credentials_changed =
        strcmp(s_sta_ssid, next_ssid) != 0 ||
        strcmp(s_sta_password, next_password) != 0;
    esp_err_t err = configure_sta_mode(config);
    if (err != ESP_OK) {
        s_last_error = err;
        s_sta_state = WIFI_MANAGER_STA_ERROR;
        notify_state_changed(true);
        return err;
    }
    if (!s_wifi_started) {
        return s_desired_enabled ? worker_start_radio() : ESP_OK;
    }
    if (!s_sta_configured) {
        s_sta_config_validation_pending = false;
        s_sta_config_handoff_disconnect_pending = false;
        s_sta_state = WIFI_MANAGER_STA_UNCONFIGURED;
        notify_state_changed(true);
        return ESP_OK;
    }
    if (validate_credentials || credentials_changed) {
        s_sta_config_validation_pending = true;
        s_sta_config_handoff_disconnect_pending = false;
    }
    if (was_connected) {
        s_connected = false;
    }
    s_sta_state = WIFI_MANAGER_STA_CONNECTING;
    s_disconnect_reason = 0;
    notify_state_changed(true);
    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }
    if (err == ESP_OK && s_sta_config_validation_pending) {
        s_sta_config_handoff_disconnect_pending = true;
    }
    err = esp_wifi_connect();
    return (err == ESP_ERR_WIFI_CONN) ? ESP_OK : err;
}

static void wifi_manager_worker(void *arg)
{
    (void)arg;
    wifi_manager_command_t command;
    while (xQueueReceive(s_command_queue, &command, portMAX_DELAY) == pdTRUE) {
        esp_err_t result = ESP_OK;
        bool complete_current = true;
        switch (command.kind) {
        case WIFI_MANAGER_CMD_START:
            s_desired_enabled = !command.config.start_disabled;
            ++s_operation_id;
            /* start() is also the compatibility entry point for replacing
             * credentials.  Do not silently ignore a new config merely
             * because the radio is already running. */
            result = worker_apply_config(&command.config, false);
            break;
        case WIFI_MANAGER_CMD_APPLY_CONFIG:
            ++s_operation_id;
            /* APPLY_CONFIG is an explicit credential operation even when the
             * same failed password was persisted by an earlier attempt. */
            result = worker_apply_config(&command.config, true);
            break;
        case WIFI_MANAGER_CMD_SET_ENABLED:
            s_desired_enabled = command.enabled;
            ++s_operation_id;
            result = command.enabled ? worker_start_radio()
                                     : worker_stop_radio();
            if (result != ESP_OK) {
                s_last_error = result;
                notify_state_changed(true);
            }
            break;
        case WIFI_MANAGER_CMD_SCAN:
            if (!s_desired_enabled ||
                    s_radio_state != WIFI_MANAGER_RADIO_ON) {
                result = ESP_ERR_INVALID_STATE;
            } else {
                result = begin_scan_on_worker(&command);
                complete_current = result != ESP_OK;
            }
            break;
        case WIFI_MANAGER_CMD_SCAN_DONE:
            if (s_scan_pending) {
                result = finish_scan_on_worker();
                wifi_manager_command_t pending = s_pending_scan;
                s_scan_pending = false;
                memset(&s_pending_scan, 0, sizeof(s_pending_scan));
                complete_command(&pending, result);
            }
            break;
        case WIFI_MANAGER_CMD_RETRY:
            if (s_scan_pending) {
                /* Scanning and connecting compete for the same radio state.
                 * finish_scan_on_worker() resumes the retry after the scan
                 * result has been collected. */
                break;
            }
            if (s_desired_enabled && s_wifi_started && s_sta_configured &&
                    s_sta_state != WIFI_MANAGER_STA_CONNECTED) {
                s_sta_state = WIFI_MANAGER_STA_CONNECTING;
                notify_state_changed(true);
                result = esp_wifi_connect();
                if (result == ESP_ERR_WIFI_CONN) {
                    result = ESP_OK;
                } else if (result != ESP_OK) {
                    s_last_error = result;
                    s_sta_state = WIFI_MANAGER_STA_RETRY_WAIT;
                    notify_state_changed(true);
                    arm_reconnect();
                }
            }
            break;
        case WIFI_MANAGER_CMD_STA_START:
            if (!s_desired_enabled) {
                break;
            }
            s_radio_state = WIFI_MANAGER_RADIO_ON;
            if (s_sta_configured) {
                s_sta_state = WIFI_MANAGER_STA_CONNECTING;
                notify_state_changed(true);
                result = esp_wifi_connect();
                if (result == ESP_ERR_WIFI_CONN) {
                    result = ESP_OK;
                } else if (result != ESP_OK) {
                    s_last_error = result;
                    s_sta_state = WIFI_MANAGER_STA_RETRY_WAIT;
                    notify_state_changed(true);
                    arm_reconnect();
                }
            } else {
                s_sta_state = WIFI_MANAGER_STA_UNCONFIGURED;
                notify_state_changed(true);
            }
            break;
        case WIFI_MANAGER_CMD_STA_DISCONNECTED:
            if (!s_desired_enabled ||
                    s_radio_state == WIFI_MANAGER_RADIO_STOPPING ||
                    s_radio_state == WIFI_MANAGER_RADIO_OFF) {
                break;
            }
            s_disconnect_reason = command.reason;
            s_sta_rssi = 0;
            s_connected = false;
            strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            if (!s_sta_configured) {
                s_sta_state = WIFI_MANAGER_STA_UNCONFIGURED;
            } else {
                reopen_ap_if_needed();
                const bool config_handoff_disconnect =
                    s_sta_config_handoff_disconnect_pending &&
                    (command.reason == WIFI_REASON_AUTH_LEAVE ||
                     command.reason == WIFI_REASON_ASSOC_LEAVE);
                s_sta_config_handoff_disconnect_pending = false;
                if (config_handoff_disconnect) {
                    /* worker_apply_config() has already issued connect() for
                     * the replacement config.  Do not turn the old link's
                     * intentional disconnect into a retry/failure. */
                    s_sta_state = WIFI_MANAGER_STA_CONNECTING;
                    notify_state_changed(true);
                    break;
                }
                s_sta_state = sta_state_from_disconnect_reason(command.reason);
                /* ESP targets/firmware revisions do not always report a bad
                 * PSK with the same reason code.  Before the replacement
                 * credentials ever reach GOT_IP, any non-handoff disconnect
                 * is terminal for this validation operation. */
                if (s_sta_config_validation_pending &&
                        s_sta_state == WIFI_MANAGER_STA_RETRY_WAIT) {
                    s_sta_state = WIFI_MANAGER_STA_ERROR;
                }
                if (s_sta_state == WIFI_MANAGER_STA_AUTH_FAILED ||
                        s_sta_state == WIFI_MANAGER_STA_AP_NOT_FOUND ||
                        s_sta_state == WIFI_MANAGER_STA_ERROR) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
                }
                /* A failure while validating newly submitted credentials is
                 * terminal for that Join operation.  Preserve it long enough
                 * for subscribers to consume it.  Disconnects from a network
                 * that had already reached GOT_IP retain normal reconnect. */
                const bool validation_failed =
                    s_sta_config_validation_pending &&
                    (s_sta_state == WIFI_MANAGER_STA_AUTH_FAILED ||
                     s_sta_state == WIFI_MANAGER_STA_AP_NOT_FOUND ||
                     s_sta_state == WIFI_MANAGER_STA_ERROR);
                if (validation_failed) {
                    s_sta_config_validation_pending = false;
                } else if (!s_scan_pending) {
                    arm_reconnect();
                }
            }
            notify_state_changed(true);
            break;
        case WIFI_MANAGER_CMD_AP_START:
            s_ap_active = true;
            refresh_ap_ip_str();
            notify_state_changed(true);
            break;
        case WIFI_MANAGER_CMD_AP_STOP:
            s_ap_active = false;
            notify_state_changed(true);
            break;
        case WIFI_MANAGER_CMD_GOT_IP:
            if (!s_desired_enabled) {
                break;
            }
            strlcpy(s_ip_addr, command.ip, sizeof(s_ip_addr));
            s_connected = true;
            s_sta_config_validation_pending = false;
            s_sta_config_handoff_disconnect_pending = false;
            s_sta_state = WIFI_MANAGER_STA_CONNECTED;
            s_disconnect_reason = 0;
            s_last_error = ESP_OK;
            wifi_ap_record_t ap = {0};
            s_sta_rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK
                ? ap.rssi : 0;
            if (s_reconnect_timer != NULL) {
                (void)esp_timer_stop(s_reconnect_timer);
            }
            if (wifi_manager_close_on_sta() && s_ap_active) {
                esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_STA);
                if (mode_err == ESP_OK) {
                    s_ap_active = false;
                    s_mode = WM_STATE_STA_ONLY;
                }
            }
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            notify_state_changed(true);
            break;
        }
        s_state = legacy_state();
        if (complete_current) {
            complete_command(&command, result);
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_event_group) return ESP_OK;

    strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
    strlcpy(s_ap_ip, "192.168.4.1", sizeof(s_ap_ip));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;
    s_command_queue = xQueueCreate(
        WIFI_MANAGER_COMMAND_DEPTH, sizeof(wifi_manager_command_t));
    if (s_command_queue == NULL) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t timer_args = { .callback = reconnect_timer_cb, .name = "wifi_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    memset(&s_config, 0, sizeof(s_config));
    s_wifi_started = false;
    s_desired_enabled = false;
    s_radio_state = WIFI_MANAGER_RADIO_OFF;
    s_sta_state = WIFI_MANAGER_STA_UNCONFIGURED;
    s_scan_state = WIFI_MANAGER_SCAN_IDLE;
    s_last_error = ESP_OK;
    s_state = WIFI_MANAGER_STATE_DISABLED;
    compose_ap_ssid();
    if (xTaskCreate(wifi_manager_worker, "wifi_manager",
                    WIFI_MANAGER_TASK_STACK, NULL,
                    WIFI_MANAGER_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(wifi_manager_validate_config(config), TAG,
                        "invalid Wi-Fi config");
    wifi_manager_command_t command = {.kind = WIFI_MANAGER_CMD_START};
    command_copy_config(&command, config);
    return submit_command(&command, true);
}

esp_err_t wifi_manager_apply_sta_config(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(wifi_manager_validate_config(config), TAG,
                        "invalid Wi-Fi config");
    wifi_manager_command_t command = {
        .kind = WIFI_MANAGER_CMD_APPLY_CONFIG,
    };
    command_copy_config(&command, config);
    return submit_command(&command, true);
}

esp_err_t wifi_manager_set_enabled(bool enabled)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_command_t command = {
        .kind = WIFI_MANAGER_CMD_SET_ENABLED,
        .enabled = enabled,
    };
    return submit_command(&command, false);
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_sta_configured) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, ticks);
    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    if (bits & WIFI_FAILED_BIT) return ESP_FAIL;
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_register_state_callback(wifi_manager_state_cb_t cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_user_ctx = user_ctx;
    if (s_state_cb) s_state_cb(s_connected, s_state_cb_user_ctx);
    return ESP_OK;
}

esp_err_t wifi_manager_register_event_callback(
    wifi_manager_event_cb_t cb, void *user_ctx)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < WIFI_MANAGER_EVENT_SUBSCRIBER_MAX; ++i) {
        if (s_event_subscribers[i].callback == cb &&
                s_event_subscribers[i].user_ctx == user_ctx) {
            return ESP_OK;
        }
    }
    for (size_t i = 0; i < WIFI_MANAGER_EVENT_SUBSCRIBER_MAX; ++i) {
        if (s_event_subscribers[i].callback == NULL) {
            s_event_subscribers[i].callback = cb;
            s_event_subscribers[i].user_ctx = user_ctx;
            /* Publish through the same path so the first event has exactly
             * the same snapshot semantics as subsequent state changes. */
            notify_state_changed(true);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t wifi_manager_unregister_event_callback(
    wifi_manager_event_cb_t cb, void *user_ctx)
{
    if (cb == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < WIFI_MANAGER_EVENT_SUBSCRIBER_MAX; ++i) {
        if (s_event_subscribers[i].callback == cb &&
                s_event_subscribers[i].user_ctx == user_ctx) {
            s_event_subscribers[i] = (wifi_manager_event_subscriber_t){0};
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (!status) return;
    status->desired_enabled = s_desired_enabled;
    status->sta_connected = s_connected;
    status->enabled = s_wifi_started;
    status->ap_active = s_ap_active;
    status->sta_configured = s_sta_configured;
    status->sta_ssid = s_sta_ssid;
    status->sta_ip = s_ip_addr;
    status->ap_ip = s_ap_ip;
    status->ap_ssid = s_ap_ssid;
    status->mode = wifi_manager_mode_string(s_mode);
    status->state = legacy_state();
    status->radio_state = s_radio_state;
    status->sta_state = s_sta_state;
    status->scan_state = s_scan_state;
    status->operation_id = s_operation_id;
    status->disconnect_reason = s_disconnect_reason;
    status->rssi = s_sta_rssi;
    status->last_error = s_last_error;
}

esp_netif_t *wifi_manager_get_ap_netif(void)
{
    return s_ap_netif;
}

static esp_err_t begin_scan_on_worker(
    const wifi_manager_command_t *command)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = true };
    if (command == NULL || command->scan_records == NULL ||
            command->scan_capacity == 0 || command->scan_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_scan_pending) {
        return ESP_ERR_INVALID_STATE;
    }
    *command->scan_count = 0;
    esp_err_t err = esp_wifi_get_mode(&s_scan_original_mode);
    if (err != ESP_OK) return err;

    s_scan_active_mode = s_scan_original_mode;
    if (s_scan_original_mode == WIFI_MODE_AP) {
        s_scan_active_mode = WIFI_MODE_APSTA;
        err = esp_wifi_set_mode(s_scan_active_mode);
        if (err != ESP_OK) return err;
    } else if (s_scan_original_mode != WIFI_MODE_STA &&
            s_scan_original_mode != WIFI_MODE_APSTA) {
        return ESP_ERR_INVALID_STATE;
    }

    /* esp_wifi_scan_start() and an in-flight esp_wifi_connect() cancel one
     * another. Pause only an unconnected STA retry; an established STA link
     * can scan without being torn down. */
    s_scan_resume_sta = false;
    if (!s_connected && s_sta_configured &&
            (s_sta_state == WIFI_MANAGER_STA_CONNECTING ||
             s_sta_state == WIFI_MANAGER_STA_RETRY_WAIT)) {
        if (s_reconnect_timer != NULL) {
            (void)esp_timer_stop(s_reconnect_timer);
        }
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK &&
                disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
            ESP_LOGW(TAG, "pause STA retry for scan failed: %s",
                     esp_err_to_name(disconnect_err));
        }
        s_scan_resume_sta = true;
        s_sta_state = WIFI_MANAGER_STA_RETRY_WAIT;
    }

    s_scan_state = WIFI_MANAGER_SCAN_RUNNING;
    s_last_error = ESP_OK;
    notify_state_changed(true);
    err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        if (s_scan_active_mode != s_scan_original_mode) {
            (void)esp_wifi_set_mode(s_scan_original_mode);
        }
        s_scan_state = WIFI_MANAGER_SCAN_ERROR;
        s_last_error = err;
        if (s_scan_resume_sta) {
            s_scan_resume_sta = false;
            arm_reconnect();
        }
        notify_state_changed(true);
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi scan started%s",
             s_scan_resume_sta ? " (STA retry paused)" : "");
    s_pending_scan = *command;
    s_scan_pending = true;
    return ESP_OK;
}

static esp_err_t finish_scan_on_worker(void)
{
    wifi_ap_record_t *ap_records = NULL;
    uint16_t ap_count = 0;
    wifi_manager_scan_record_t *records = s_pending_scan.scan_records;
    const uint16_t max_records = s_pending_scan.scan_capacity;
    uint16_t *out_count = s_pending_scan.scan_count;
    esp_err_t err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) goto cleanup;

    if (ap_count == 0) { err = ESP_OK; goto cleanup; }
    if (ap_count > max_records) ap_count = max_records;

    ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) { err = ESP_ERR_NO_MEM; goto cleanup; }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) goto cleanup;

    uint16_t unique_count = 0;
    for (uint16_t i = 0; i < ap_count; ++i) {
        const char *ssid = (const char *)ap_records[i].ssid;
        if (ssid[0] == '\0') continue;
        uint16_t duplicate = unique_count;
        for (uint16_t j = 0; j < unique_count; ++j) {
            if (strcmp(records[j].ssid, ssid) == 0) {
                duplicate = j;
                break;
            }
        }
        if (duplicate < unique_count) {
            if (ap_records[i].rssi > records[duplicate].rssi) {
                records[duplicate].rssi = ap_records[i].rssi;
                records[duplicate].primary = ap_records[i].primary;
                records[duplicate].authmode = ap_records[i].authmode;
            }
            continue;
        }
        if (unique_count == max_records) break;
        strlcpy(records[unique_count].ssid, ssid,
                sizeof(records[unique_count].ssid));
        records[unique_count].rssi = ap_records[i].rssi;
        records[unique_count].primary = ap_records[i].primary;
        records[unique_count].authmode = ap_records[i].authmode;
        ++unique_count;
    }
    *out_count = unique_count;
    err = ESP_OK;

cleanup:
    free(ap_records);
    if (s_scan_active_mode != s_scan_original_mode) {
        esp_err_t restore_err = esp_wifi_set_mode(s_scan_original_mode);
        if (err == ESP_OK && restore_err != ESP_OK) err = restore_err;
    }
    s_scan_state = err == ESP_OK ? WIFI_MANAGER_SCAN_READY
                                 : WIFI_MANAGER_SCAN_ERROR;
    s_last_error = err;
    ESP_LOGI(TAG, "Wi-Fi scan complete: count=%u result=%s",
             (unsigned)(out_count != NULL ? *out_count : 0U),
             esp_err_to_name(err));
    if (s_scan_resume_sta) {
        s_scan_resume_sta = false;
        arm_reconnect();
    }
    notify_state_changed(true);
    return err;
}

static void cancel_scan_on_worker(esp_err_t reason)
{
    if (!s_scan_pending) {
        return;
    }
    (void)esp_wifi_scan_stop();
    if (s_scan_active_mode != s_scan_original_mode) {
        (void)esp_wifi_set_mode(s_scan_original_mode);
    }
    wifi_manager_command_t pending = s_pending_scan;
    s_scan_pending = false;
    memset(&s_pending_scan, 0, sizeof(s_pending_scan));
    s_scan_state = WIFI_MANAGER_SCAN_IDLE;
    s_last_error = reason;
    /* Cancellation is owned by stop/apply-config; those operations decide
     * the next STA state and must not inherit the old retry timer. */
    s_scan_resume_sta = false;
    notify_state_changed(true);
    complete_command(&pending, reason);
}

esp_err_t wifi_manager_scan_aps(wifi_manager_scan_record_t *records,
                                uint16_t max_records,
                                uint16_t *out_count)
{
    if (records == NULL || max_records == 0 || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_manager_command_t command = {
        .kind = WIFI_MANAGER_CMD_SCAN,
        .scan_records = records,
        .scan_capacity = max_records,
        .scan_count = out_count,
    };
    return submit_command(&command, true);
}
