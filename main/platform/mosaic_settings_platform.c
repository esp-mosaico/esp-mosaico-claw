/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaic_settings_platform.h"

#include "app_factory_reset.h"

#if CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE

#include <stdlib.h>
#include <string.h>

#include "cap_system_platform.h"
#include "display_service.h"
#include "edge_agent_version.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mosaic_battery_platform.h"
#include "mosaic_setup.h"
#include "mosaic_loader.h"
#include "mosaic_settings.h"
#include "mosaic_system.h"
#include "mosaic_ui.h"
#include "update_check_service.h"
#include "esp_wifi.h"
#include "wifi_manager.h"

static const char *TAG = "mosaic_settings_platform";
static mosaic_settings_platform_config_t s_config;
static TaskHandle_t s_rotation_task;

static void delayed_factory_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;
static mosaic_settings_wifi_ap_t s_scan_cache[MOSAIC_SETTINGS_WIFI_SCAN_MAX];
static size_t s_scan_count;
static bool s_scan_valid;
static bool s_scan_running;
static uint32_t s_scan_revision;
static esp_err_t s_scan_error = ESP_ERR_NOT_FINISHED;
static portMUX_TYPE s_wifi_state_lock = portMUX_INITIALIZER_UNLOCKED;
static mosaic_settings_network_t s_wifi_state_cache;
static bool s_wifi_state_valid;
static uint32_t s_update_revision;
static void invalidate_settings_app(uint32_t revision);

static void update_check_event_cb(
    const update_check_snapshot_t *snapshot, void *user_ctx)
{
    (void)snapshot;
    (void)user_ctx;
    invalidate_settings_app(++s_update_revision);
}

static void copy_scan_status(mosaic_settings_network_t *network)
{
    portENTER_CRITICAL(&s_scan_lock);
    network->scan_revision = s_scan_revision;
    network->scan_error = s_scan_error;
    portEXIT_CRITICAL(&s_scan_lock);
}

static void invalidate_settings_app(uint32_t revision)
{
    const mosaic_app_descriptor_t *settings =
        mosaic_app_descriptor_for_name("settings");
    if (settings != NULL) {
        (void)mosaic_loader_invalidate_app(settings->id, revision);
    }
}

static mosaic_settings_wifi_state_t mosaic_wifi_sta_state(
    wifi_manager_radio_state_t radio, wifi_manager_sta_state_t sta)
{
    if (radio == WIFI_MANAGER_RADIO_OFF ||
            radio == WIFI_MANAGER_RADIO_STOPPING) {
        return MOSAIC_SETTINGS_WIFI_DISABLED;
    }
    switch (sta) {
    case WIFI_MANAGER_STA_UNCONFIGURED:
    case WIFI_MANAGER_STA_IDLE:
        return MOSAIC_SETTINGS_WIFI_IDLE;
    case WIFI_MANAGER_STA_CONNECTING:
        return MOSAIC_SETTINGS_WIFI_CONNECTING;
    case WIFI_MANAGER_STA_CONNECTED:
        return MOSAIC_SETTINGS_WIFI_CONNECTED;
    case WIFI_MANAGER_STA_RETRY_WAIT:
        return MOSAIC_SETTINGS_WIFI_RETRY_WAIT;
    case WIFI_MANAGER_STA_AUTH_FAILED:
        return MOSAIC_SETTINGS_WIFI_AUTH_FAILED;
    case WIFI_MANAGER_STA_AP_NOT_FOUND:
        return MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND;
    default:
        return MOSAIC_SETTINGS_WIFI_FAILED;
    }
}

static void wifi_state_event_cb(
    const wifi_manager_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) {
        return;
    }
    mosaic_settings_network_t network = {
        .desired_enabled = event->desired_enabled,
        .enabled = event->enabled,
        .connected = event->sta_connected,
        .configured = event->sta_configured,
        .ap_active = event->ap_active,
        .state = mosaic_wifi_sta_state(event->radio_state, event->sta_state),
        .radio_state =
            (mosaic_settings_wifi_radio_state_t)event->radio_state,
        .sta_state = (mosaic_settings_wifi_sta_state_t)event->sta_state,
        .scan_state = (mosaic_settings_wifi_scan_state_t)event->scan_state,
        .operation_id = event->operation_id,
        .disconnect_reason = event->disconnect_reason,
        .last_error = event->last_error,
        .rssi = event->sta_connected ? event->rssi : 0,
    };
    strlcpy(network.ssid, event->sta_ssid, sizeof(network.ssid));
    strlcpy(network.ip,
            event->sta_connected ? event->sta_ip : "", sizeof(network.ip));
    if (event->ap_active && event->ap_ip[0] != '\0') {
        snprintf(network.portal_url, sizeof(network.portal_url),
                 "http://%s", event->ap_ip);
    }
    copy_scan_status(&network);
    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_state_cache = network;
    s_wifi_state_valid = true;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    /* Fan-out to Hub/status subscribers. Callbacks must not touch GSP. */
    mosaic_settings_notify_wifi(&network);
    /* Do not read services or touch GSP from the ESP event-loop callback.
     * Invalidation schedules Settings to consume the new snapshot on its
     * normal UI frame. */
    mosaic_settings_platform_notify_network_changed();
}

static esp_err_t get_snapshot(
    void *user_ctx, mosaic_settings_snapshot_t *ret_snapshot)
{
    app_settings_service_handle_t service = user_ctx;
    ESP_RETURN_ON_FALSE(service && ret_snapshot, ESP_ERR_INVALID_ARG, TAG,
                        "invalid Settings snapshot request");
    app_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    ESP_RETURN_ON_FALSE(snapshot, ESP_ERR_NO_MEM, TAG,
                        "allocate Settings snapshot");
    memset(ret_snapshot, 0, sizeof(*ret_snapshot));
    strlcpy(ret_snapshot->software_version, edge_agent_get_version(),
            sizeof(ret_snapshot->software_version));
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err == ESP_OK) {
        (void)snprintf(ret_snapshot->serial_number, sizeof(ret_snapshot->serial_number), "%02X:%02X:%02X:%02X:%02X:%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGW(TAG, "read device MAC failed: %s", esp_err_to_name(err));
    }
    err = app_settings_service_get_snapshot(service, snapshot);
    if (err == ESP_OK) {
        wifi_manager_status_t wifi = {0};
        wifi_manager_get_status(&wifi);
        ret_snapshot->rotation = snapshot->rotation;
        /* Snapshot brightness is the persisted UI value (0..100). Hardware
         * output is independently mapped to the safe 10..100 range. */
        ret_snapshot->brightness = snapshot->brightness;
        ret_snapshot->volume = snapshot->volume;
        ret_snapshot->screen_timeout_ms = snapshot->screen_timeout_ms;
        ret_snapshot->vibration_enabled = snapshot->vibration_enabled;
        ret_snapshot->display_available = snapshot->display_available;
        ret_snapshot->audio_available = snapshot->audio_available;
        ret_snapshot->network.desired_enabled = wifi.desired_enabled;
        ret_snapshot->network.enabled = wifi.enabled;
        ret_snapshot->network.connected = wifi.sta_connected;
        ret_snapshot->network.configured = wifi.sta_configured;
        ret_snapshot->network.ap_active = wifi.ap_active;
        ret_snapshot->network.rssi = wifi.sta_connected ? wifi.rssi : 0;
        strlcpy(ret_snapshot->network.ssid,
                wifi.sta_ssid != NULL ? wifi.sta_ssid : snapshot->network.ssid,
                sizeof(ret_snapshot->network.ssid));
        strlcpy(ret_snapshot->network.ip,
                wifi.sta_connected && wifi.sta_ip != NULL
                    ? wifi.sta_ip : snapshot->network.ip,
                sizeof(ret_snapshot->network.ip));
        strlcpy(ret_snapshot->network.portal_url, snapshot->network.portal_url,
                sizeof(ret_snapshot->network.portal_url));
        ret_snapshot->network.state =
            mosaic_wifi_sta_state(wifi.radio_state, wifi.sta_state);
        ret_snapshot->network.radio_state =
            (mosaic_settings_wifi_radio_state_t)wifi.radio_state;
        ret_snapshot->network.sta_state =
            (mosaic_settings_wifi_sta_state_t)wifi.sta_state;
        ret_snapshot->network.scan_state =
            (mosaic_settings_wifi_scan_state_t)wifi.scan_state;
        ret_snapshot->network.operation_id = wifi.operation_id;
        ret_snapshot->network.disconnect_reason = wifi.disconnect_reason;
        ret_snapshot->network.last_error = wifi.last_error;
        copy_scan_status(&ret_snapshot->network);
        ret_snapshot->im.wechat_configured = snapshot->im.wechat_configured;
        ret_snapshot->im.qq_configured = snapshot->im.qq_configured;
        ret_snapshot->im.feishu_configured = snapshot->im.feishu_configured;
        ret_snapshot->im.telegram_configured =
            snapshot->im.telegram_configured;
        strlcpy(ret_snapshot->llm.backend, snapshot->llm.backend,
                sizeof(ret_snapshot->llm.backend));
        strlcpy(ret_snapshot->llm.model, snapshot->llm.model,
                sizeof(ret_snapshot->llm.model));
        strlcpy(ret_snapshot->llm.base_url, snapshot->llm.base_url,
                sizeof(ret_snapshot->llm.base_url));
        ret_snapshot->llm.api_key_configured =
            snapshot->llm.api_key_configured;
        ret_snapshot->llm.supports_tools = snapshot->llm.supports_tools;
        ret_snapshot->llm.supports_vision = snapshot->llm.supports_vision;
    }

    (void)mosaic_battery_platform_get(&ret_snapshot->battery);
    update_check_snapshot_t update = {0};
    if (update_check_service_get_snapshot(&update) == ESP_OK) {
        ret_snapshot->update.state =
            (mosaic_settings_update_state_t)update.state;
        strlcpy(ret_snapshot->update.latest_version, update.latest_version,
                sizeof(ret_snapshot->update.latest_version));
        strlcpy(ret_snapshot->update.title, update.title,
                sizeof(ret_snapshot->update.title));
        strlcpy(ret_snapshot->update.summary, update.summary,
                sizeof(ret_snapshot->update.summary));
        strlcpy(ret_snapshot->update.published_at, update.published_at,
                sizeof(ret_snapshot->update.published_at));
        ret_snapshot->update.sequence = update.sequence;
        ret_snapshot->update.last_error = update.last_error;
    }
    free(snapshot);
    return err;
}

static esp_err_t request_update_check(void *user_ctx)
{
    (void)user_ctx;
    return update_check_service_request();
}

static esp_err_t get_battery(
    void *user_ctx, mosaic_settings_battery_t *ret_battery)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(ret_battery != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid battery request");
    return mosaic_battery_platform_get(ret_battery);
}

static void rotation_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t degrees = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &degrees, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = app_settings_service_set_rotation(
            s_config.settings, (uint16_t)degrees);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply display rotation failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t set_rotation(void *user_ctx, uint16_t degrees)
{
    (void)user_ctx;
    if (s_rotation_task == NULL && xTaskCreate(
            rotation_task, "display_rotate", 4096, NULL, 5,
            &s_rotation_task) != pdPASS) {
        s_rotation_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return xTaskNotify(s_rotation_task, degrees, eSetValueWithOverwrite) ==
            pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t set_volume(void *user_ctx, int volume, bool persist)
{
    return app_settings_service_set_volume(user_ctx, volume, persist);
}

static esp_err_t set_brightness(void *user_ctx, int brightness, bool persist)
{
    return app_settings_service_set_brightness(
        user_ctx, brightness, persist);
}

static esp_err_t set_vibration(void *user_ctx, bool enabled)
{
    return app_settings_service_set_vibration(user_ctx, enabled);
}

static esp_err_t set_screen_timeout(void *user_ctx, uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(app_settings_service_set_screen_timeout(
                            user_ctx, timeout_ms),
                        TAG, "save screen timeout");
    mosaic_ui_set_screen_timeout(timeout_ms);
    return ESP_OK;
}

static esp_err_t factory_reset(void *user_ctx)
{
    /* app_config is the source of truth, but the Wi-Fi driver also persists
     * esp_wifi_set_config() values in its own NVS storage. Clear both copies
     * so credentials cannot survive a factory reset. */
    ESP_RETURN_ON_ERROR(esp_wifi_restore(), TAG,
                        "clear Wi-Fi driver configuration");
    /* NAND is cleared during the next boot, before any service can recreate
     * session, memory, photo, work, skill, or other user files. Persist this
     * intent before erasing app settings so a power loss cannot leave a
     * partially reset device. */
    ESP_RETURN_ON_ERROR(app_factory_reset_request(), TAG,
                        "schedule boot-time data cleanup");
    esp_err_t err = app_settings_service_reset_all(user_ctx);
    if (err != ESP_OK) {
        esp_err_t cancel_err = app_factory_reset_complete();
        if (cancel_err != ESP_OK) {
            ESP_LOGE(TAG, "cancel data cleanup failed: %s",
                     esp_err_to_name(cancel_err));
        }
        ESP_RETURN_ON_ERROR(err, TAG,
                            "clear persisted device configuration");
    }
    ESP_LOGW(TAG,
             "system, Wi-Fi and app config cleared; data cleanup pending; "
             "rebooting into Setup in 3 seconds");
    if (xTaskCreate(delayed_factory_restart, "factory_restart", 2048,
                    NULL, 5, NULL) != pdPASS) {
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t get_boot_stage(
    void *user_ctx, mosaic_system_boot_stage_t *ret_stage)
{
    app_system_boot_stage_t stage;
    ESP_RETURN_ON_ERROR(app_settings_service_get_boot_stage(user_ctx, &stage),
                        TAG, "get boot stage");
    *ret_stage = (mosaic_system_boot_stage_t)stage;
    return ESP_OK;
}

static esp_err_t set_boot_stage(
    void *user_ctx, mosaic_system_boot_stage_t stage)
{
    return app_settings_service_set_boot_stage(
        user_ctx, (app_system_boot_stage_t)stage);
}

static esp_err_t set_wifi_enabled(void *user_ctx, bool enabled)
{
    bool previous = true;
    ESP_RETURN_ON_ERROR(
        app_settings_service_get_wifi_enabled(user_ctx, &previous), TAG,
        "load saved Wi-Fi state");
    ESP_RETURN_ON_ERROR(
        app_settings_service_set_wifi_enabled(user_ctx, enabled), TAG,
        "save Wi-Fi state");
    esp_err_t err = wifi_manager_set_enabled(enabled);
    if (err != ESP_OK) {
        esp_err_t rollback_err =
            app_settings_service_set_wifi_enabled(user_ctx, previous);
        if (rollback_err != ESP_OK) {
            ESP_LOGE(TAG, "rollback saved Wi-Fi state failed: %s",
                     esp_err_to_name(rollback_err));
        }
    }
    return err;
}

static esp_err_t get_wifi_status(
    void *user_ctx, mosaic_settings_network_t *network)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(network != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid Wi-Fi status request");
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool valid = s_wifi_state_valid;
    mosaic_settings_network_t cached = s_wifi_state_cache;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (valid) {
        *network = cached;
    } else {
        /* Fallback is only needed before the event subscription has published
         * its initial snapshot. */
        wifi_manager_status_t wifi = {0};
        wifi_manager_get_status(&wifi);
        memset(network, 0, sizeof(*network));
        network->desired_enabled = wifi.desired_enabled;
        network->enabled = wifi.enabled;
        network->connected = wifi.sta_connected;
        network->configured = wifi.sta_configured;
        network->ap_active = wifi.ap_active;
        network->state =
            mosaic_wifi_sta_state(wifi.radio_state, wifi.sta_state);
        network->radio_state =
            (mosaic_settings_wifi_radio_state_t)wifi.radio_state;
        network->sta_state =
            (mosaic_settings_wifi_sta_state_t)wifi.sta_state;
        network->scan_state =
            (mosaic_settings_wifi_scan_state_t)wifi.scan_state;
        network->operation_id = wifi.operation_id;
        network->disconnect_reason = wifi.disconnect_reason;
        network->last_error = wifi.last_error;
        network->rssi = wifi.sta_connected ? wifi.rssi : 0;
        strlcpy(network->ssid,
                wifi.sta_ssid != NULL ? wifi.sta_ssid : "",
                sizeof(network->ssid));
        strlcpy(network->ip,
                wifi.sta_connected && wifi.sta_ip != NULL ? wifi.sta_ip : "",
                sizeof(network->ip));
    }
    network->rssi = network->connected ? network->rssi : 0;
    copy_scan_status(network);
    return ESP_OK;
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_manager_scan_record_t *scan = calloc(
        MOSAIC_SETTINGS_WIFI_SCAN_MAX, sizeof(*scan));
    mosaic_settings_wifi_ap_t *next = calloc(
        MOSAIC_SETTINGS_WIFI_SCAN_MAX, sizeof(*next));
    uint16_t count = 0;
    esp_err_t err = (scan == NULL || next == NULL) ? ESP_ERR_NO_MEM :
        wifi_manager_scan_aps(scan, MOSAIC_SETTINGS_WIFI_SCAN_MAX, &count);
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            strlcpy(next[i].ssid, scan[i].ssid, sizeof(next[i].ssid));
            next[i].rssi = scan[i].rssi;
            next[i].secured = scan[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    portENTER_CRITICAL(&s_scan_lock);
    if (err == ESP_OK) {
        memcpy(s_scan_cache, next, count * sizeof(*next));
        s_scan_count = count;
        s_scan_valid = true;
    }
    s_scan_error = err;
    ++s_scan_revision;
    if (s_scan_revision == 0U) {
        ++s_scan_revision;
    }
    s_scan_running = false;
    portEXIT_CRITICAL(&s_scan_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "background Wi-Fi scan failed: %s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "background Wi-Fi scan found %u AP(s)",
                 (unsigned)count);
    }
    free(scan);
    free(next);
    static uint32_t revision;
    invalidate_settings_app(++revision);
    vTaskDelete(NULL);
}

static esp_err_t request_wifi_scan(void *user_ctx)
{
    (void)user_ctx;
    bool start = false;
    portENTER_CRITICAL(&s_scan_lock);
    if (!s_scan_running) {
        /* Keep the last completed snapshot visible while refreshing. If this
         * is the first scan, scan_wifi() still reports NOT_FINISHED until the
         * worker publishes its first result. */
        s_scan_running = true;
        start = true;
    }
    portEXIT_CRITICAL(&s_scan_lock);
    if (start && xTaskCreate(wifi_scan_task, "settings_wifi_scan", 4096,
                            NULL, 4, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_scan_lock);
        s_scan_running = false;
        s_scan_error = ESP_ERR_NO_MEM;
        ++s_scan_revision;
        if (s_scan_revision == 0U) {
            ++s_scan_revision;
        }
        portEXIT_CRITICAL(&s_scan_lock);
        static uint32_t revision;
        invalidate_settings_app(++revision);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t scan_wifi(void *user_ctx,
    mosaic_settings_wifi_ap_t *records, size_t capacity, size_t *out_count)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(records && capacity && out_count,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi scan output");
    portENTER_CRITICAL(&s_scan_lock);
    if (!s_scan_valid) {
        portEXIT_CRITICAL(&s_scan_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    size_t count = s_scan_count;
    if (count > capacity) {
        count = capacity;
    }
    if (count > 0) {
        memcpy(records, s_scan_cache, count * sizeof(*records));
    }
    *out_count = count;
    portEXIT_CRITICAL(&s_scan_lock);
    return ESP_OK;
}

static esp_err_t update_wifi_credentials(const char *ssid, const char *password)
{
    app_config_t *next = malloc(sizeof(*next));
    ESP_RETURN_ON_FALSE(next, ESP_ERR_NO_MEM, TAG, "allocate Wi-Fi config");
    esp_err_t err = app_config_load(next);
    if (err == ESP_OK) {
        const char *requested_ssid = ssid ? ssid : "";
        const char *requested_password = password ? password : "";
        const bool credentials_unchanged =
            strcmp(next->wifi_ssid, requested_ssid) == 0 &&
            strcmp(next->wifi_password, requested_password) == 0;
        strlcpy(next->wifi_ssid, ssid ? ssid : "", sizeof(next->wifi_ssid));
        strlcpy(next->wifi_password, password ? password : "",
                sizeof(next->wifi_password));
        err = s_config.save_config(next);
        if (err == ESP_OK && credentials_unchanged && requested_ssid[0] != '\0') {
            wifi_manager_status_t status = {0};
            wifi_manager_get_status(&status);
            const bool already_connected = status.sta_connected &&
                status.sta_ssid != NULL &&
                strcmp(status.sta_ssid, requested_ssid) == 0;
            if (!already_connected) {
                /* Saving an unchanged credential set intentionally causes no
                 * config-change notification. A user Join action is stronger
                 * than a persistence update, however: retry a prior terminal
                 * failure instead of leaving the UI waiting for an event that
                 * can never be emitted. */
                err = wifi_manager_apply_sta_config(
                    &(wifi_manager_config_t) {
                        .sta_ssid = next->wifi_ssid,
                        .sta_password = next->wifi_password,
                        .ap_ssid = next->ap_ssid[0] ? next->ap_ssid : NULL,
                        .ap_password = next->ap_password[0]
                            ? next->ap_password : NULL,
                        .ap_behavior = next->ap_behavior,
                    });
            }
        }
    }
    free(next);
    return err;
}

static esp_err_t connect_wifi(
    void *user_ctx, const char *ssid, const char *password)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(ssid && ssid[0] && password,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi credentials");
    esp_err_t err = update_wifi_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "submit Settings WLAN credentials failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t get_phone_setup(
    void *user_ctx, char *ap_ssid, size_t ap_ssid_size,
    char *qr_payload, size_t qr_payload_size)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(s_config.network_provisioning != NULL &&
                            ap_ssid != NULL && ap_ssid_size > 0U &&
                            qr_payload != NULL && qr_payload_size > 0U,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid phone provisioning request");
    network_provisioning_status_t *status = calloc(1, sizeof(*status));
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate provisioning snapshot");
    esp_err_t err = network_provisioning_service_get_status(
        s_config.network_provisioning, status);
    if (err == ESP_OK && status->ap_active &&
            status->ap_ssid[0] != '\0' && status->ap_join_qr[0] != '\0') {
        strlcpy(ap_ssid, status->ap_ssid, ap_ssid_size);
        strlcpy(qr_payload, status->ap_join_qr, qr_payload_size);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_STATE;
    }
    free(status);
    return err;
}

static esp_err_t forget_wifi(void *user_ctx)
{
    (void)user_ctx;
    return update_wifi_credentials("", "");
}

static esp_err_t request_network_reconfigure(void *user_ctx)
{
    (void)user_ctx;
    return mosaic_setup_open(
        MOSAIC_SETUP_MODE_MANAGE, MOSAIC_SETUP_ROUTE_NETWORK);
}

static esp_err_t haptic_feedback(void *user_ctx, uint32_t duration_ms)
{
    app_system_config_t system_config;
    if (app_system_config_load(&system_config) == ESP_OK &&
            !system_config.vibration_enabled) {
        return ESP_OK;
    }
    (void)user_ctx;
    return app_cap_system_platform_vibrate(duration_ms);
}

#if CONFIG_ESP_BOARD_ESP_MOSAICO
extern esp_err_t lcd_panel_set_brightness_entry_t(
    esp_lcd_panel_handle_t panel, uint8_t percent);

static esp_err_t board_set_brightness(
    esp_lcd_panel_handle_t panel, uint8_t percent, void *user_ctx)
{
    (void)user_ctx;
    return lcd_panel_set_brightness_entry_t(panel, percent);
}
#endif

esp_err_t mosaic_settings_platform_init(
    const mosaic_settings_platform_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->settings && config->save_config,
                        ESP_ERR_INVALID_ARG, TAG, "invalid platform config");
    s_config = *config;
    app_settings_snapshot_t settings_snapshot = {0};
    if (app_settings_service_get_snapshot(config->settings,
                                          &settings_snapshot) == ESP_OK) {
        mosaic_ui_set_screen_timeout(settings_snapshot.screen_timeout_ms);
    }
    /* esp_board_device_get_handle() logs every successful lookup at INFO.
     * Settings owns long-lived device handles and does not need that chatter;
     * keep BOARD_DEVICE warnings and errors visible. */
    esp_log_level_set("BOARD_DEVICE", ESP_LOG_WARN);
    ESP_RETURN_ON_ERROR(wifi_manager_register_event_callback(
                            wifi_state_event_cb, NULL),
                        TAG, "subscribe to Wi-Fi state");
#if CONFIG_ESP_BOARD_ESP_MOSAICO
    display_service_set_brightness_provider(board_set_brightness, NULL);
#endif
    mosaic_ui_set_haptic_callback(haptic_feedback, NULL);
    ESP_RETURN_ON_ERROR(mosaic_system_configure(&(mosaic_system_ops_t) {
        .get_boot_stage = get_boot_stage,
        .set_boot_stage = set_boot_stage,
        .user_ctx = config->settings,
    }), TAG, "configure system lifecycle");
    ESP_RETURN_ON_ERROR(mosaic_settings_configure(&(mosaic_settings_ops_t) {
        .get_snapshot = get_snapshot,
        .set_rotation = set_rotation,
        .set_brightness = set_brightness,
        .set_volume = set_volume,
        .set_vibration = set_vibration,
        .set_screen_timeout = set_screen_timeout,
        .factory_reset = factory_reset,
        .request_update_check = request_update_check,
        .set_wifi_enabled = set_wifi_enabled,
        .get_battery = get_battery,
        .get_wifi_status = get_wifi_status,
        .request_wifi_scan = request_wifi_scan,
        .scan_wifi = scan_wifi,
        .connect_wifi = connect_wifi,
        .forget_wifi = forget_wifi,
        .get_phone_setup = get_phone_setup,
        .request_network_reconfigure = request_network_reconfigure,
        .user_ctx = config->settings,
    }), TAG, "configure settings ops");
    ESP_RETURN_ON_ERROR(update_check_service_subscribe(
                            update_check_event_cb, NULL),
                        TAG, "subscribe to update checker");
    /* Seed Wi-Fi cache so Hub status bar has a value before the first event. */
    mosaic_settings_network_t wifi = {0};
    if (get_wifi_status(config->settings, &wifi) == ESP_OK) {
        mosaic_settings_notify_wifi(&wifi);
    }
    return ESP_OK;
}

esp_err_t mosaic_settings_platform_start_battery_monitor(void)
{
    /* The UI subscriber must be live before a critical sample can arm shutdown. */
    return mosaic_battery_platform_start();
}

void mosaic_settings_platform_notify_network_changed(void)
{
    static uint32_t revision;
    invalidate_settings_app(++revision);
}

#else

esp_err_t mosaic_settings_platform_init(
    const mosaic_settings_platform_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mosaic_settings_platform_start_battery_monitor(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void mosaic_settings_platform_notify_network_changed(void)
{
}

#endif
