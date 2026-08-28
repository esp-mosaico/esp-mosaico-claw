/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_system_platform.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_settings_service.h"
#include "cJSON.h"
#include "cap_system.h"
#include "esp_board_manager_includes.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings_store.h"
#include "wifi_manager.h"

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
#include "display_service.h"
#endif

#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
#include "devices/dev_audio_codec/dev_audio_codec.h"
#include "esp_codec_dev.h"
#endif

#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
#include "devices/dev_ledc_ctrl/dev_ledc_ctrl.h"
#include "driver/ledc.h"
#include "freertos/timers.h"
#endif

static const char *TAG = "cap_system_platform";

#define APP_CAP_SYSTEM_POWER_PROFILE_KEY "sys_pwr_prof"
#define APP_CAP_SYSTEM_DEFAULT_POWER_PROFILE "balanced"
#define APP_CAP_SYSTEM_WIFI_SCAN_DEFAULT_LIMIT 15
#define APP_CAP_SYSTEM_SLEEP_DELAY_MS 500
#define APP_CAP_SYSTEM_MAX_SLEEP_MS (24ULL * 60ULL * 60ULL * 1000ULL)
#define APP_CAP_SYSTEM_VIBRATION_DEVICE_NAME "vibration_motor"

typedef struct {
    bool deep;
    uint64_t duration_ms;
} app_cap_system_sleep_args_t;

#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
static EXT_RAM_BSS_ATTR periph_ledc_handle_t *s_vibration_ledc;
static EXT_RAM_BSS_ATTR periph_ledc_config_t *s_vibration_ledc_config;
static EXT_RAM_BSS_ATTR TimerHandle_t s_vibration_timer;

static esp_err_t app_cap_system_vibration_set_percent(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_vibration_ledc && s_vibration_ledc_config &&
                        (uint32_t)s_vibration_ledc_config->duty_resolution < 32,
                        ESP_ERR_INVALID_STATE, TAG,
                        "vibration LEDC is unavailable");
    uint32_t max_duty = (uint32_t)(
        (1ULL << (uint32_t)s_vibration_ledc_config->duty_resolution) - 1ULL);
    uint32_t duty = (uint32_t)(((uint64_t)percent * max_duty) / 100U);
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(s_vibration_ledc->speed_mode,
                      s_vibration_ledc->channel, duty),
        TAG, "set vibration duty");
    return ledc_update_duty(s_vibration_ledc->speed_mode,
                            s_vibration_ledc->channel);
}

static void app_cap_system_vibration_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    esp_err_t err = app_cap_system_vibration_set_percent(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop vibration failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t app_cap_system_vibration_init(void)
{
    dev_ledc_ctrl_config_t *device_config = NULL;

    if (s_vibration_timer) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(
        esp_board_manager_check_name(APP_CAP_SYSTEM_VIBRATION_DEVICE_NAME),
        ESP_ERR_NOT_SUPPORTED, TAG, "vibration device is unavailable");
    esp_err_t err = esp_board_manager_get_device_handle(
        APP_CAP_SYSTEM_VIBRATION_DEVICE_NAME, (void **)&s_vibration_ledc);
    if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(
            esp_board_manager_init_device_by_name(
                APP_CAP_SYSTEM_VIBRATION_DEVICE_NAME),
            TAG, "initialize vibration device");
        ESP_RETURN_ON_ERROR(
            esp_board_manager_get_device_handle(
                APP_CAP_SYSTEM_VIBRATION_DEVICE_NAME,
                (void **)&s_vibration_ledc),
            TAG, "get vibration device handle");
    }
    ESP_RETURN_ON_ERROR(
        esp_board_manager_get_device_config(
            APP_CAP_SYSTEM_VIBRATION_DEVICE_NAME,
            (void **)&device_config),
        TAG, "get vibration device config");
    ESP_RETURN_ON_FALSE(device_config && device_config->ledc_name,
                        ESP_ERR_INVALID_STATE, TAG,
                        "vibration LEDC config is invalid");
    ESP_RETURN_ON_ERROR(
        esp_board_periph_get_config(device_config->ledc_name,
                                    (void **)&s_vibration_ledc_config),
        TAG, "get vibration peripheral config");
    s_vibration_timer = xTimerCreate(
        "cap_vibration", pdMS_TO_TICKS(25), pdFALSE, NULL,
        app_cap_system_vibration_timer_cb);
    ESP_RETURN_ON_FALSE(s_vibration_timer, ESP_ERR_NO_MEM, TAG,
                        "create vibration timer");
    return app_cap_system_vibration_set_percent(0);
}

esp_err_t app_cap_system_platform_vibrate(uint32_t duration_ms)
{
    ESP_RETURN_ON_ERROR(app_cap_system_vibration_init(), TAG,
                        "initialize vibration control");
    ESP_RETURN_ON_ERROR(app_cap_system_vibration_set_percent(100), TAG,
                        "start vibration");
    (void)xTimerStop(s_vibration_timer, 0);
    if (xTimerChangePeriod(s_vibration_timer,
                           pdMS_TO_TICKS(duration_ms),
                           pdMS_TO_TICKS(20)) != pdPASS) {
        (void)app_cap_system_vibration_set_percent(0);
        return ESP_FAIL;
    }
    return ESP_OK;
}
#else
esp_err_t app_cap_system_platform_vibrate(uint32_t duration_ms)
{
    (void)duration_ms;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

static esp_err_t app_cap_system_render(cJSON *root, char *output, size_t output_size)
{
    char *json;
    int written;

    ESP_RETURN_ON_FALSE(root && output && output_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid JSON output");
    json = cJSON_PrintUnformatted(root);
    ESP_RETURN_ON_FALSE(json, ESP_ERR_NO_MEM, TAG, "render JSON failed");
    written = snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return written >= 0 && (size_t)written < output_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t app_cap_system_render_error(char *output,
                                             size_t output_size,
                                             esp_err_t err,
                                             const char *message)
{
    cJSON *root = cJSON_CreateObject();

    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message ? message : esp_err_to_name(err));
    cJSON_AddStringToObject(root, "code", esp_err_to_name(err));
    (void)app_cap_system_render(root, output, output_size);
    cJSON_Delete(root);
    return err;
}

static const char *app_cap_system_auth_mode(wifi_auth_mode_t mode)
{
    switch (mode) {
        case WIFI_AUTH_OPEN:
            return "open";
        case WIFI_AUTH_WEP:
            return "wep";
        case WIFI_AUTH_WPA_PSK:
            return "wpa_psk";
        case WIFI_AUTH_WPA2_PSK:
            return "wpa2_psk";
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "wpa_wpa2_psk";
        case WIFI_AUTH_WPA3_PSK:
            return "wpa3_psk";
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return "wpa2_wpa3_psk";
        default:
            return "unknown";
    }
}

#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
static esp_err_t app_cap_system_get_audio_codec(bool initialize,
                                                dev_audio_codec_handles_t **out_handles)
{
    dev_audio_codec_handles_t *handles = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out_handles, ESP_ERR_INVALID_ARG, TAG, "audio output is NULL");
    if (!esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_AUDIO_DAC)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC,
                                              (void **)&handles);
    if (err != ESP_OK) {
        if (!initialize) {
            return err;
        }
        ESP_RETURN_ON_ERROR(
            esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_AUDIO_DAC),
            TAG, "init audio DAC failed");
        err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_AUDIO_DAC,
                                                  (void **)&handles);
        ESP_RETURN_ON_ERROR(err, TAG, "get audio DAC handle failed");
    }
    ESP_RETURN_ON_FALSE(handles && handles->codec_dev,
                        ESP_ERR_INVALID_STATE, TAG, "audio DAC handle is invalid");
    *out_handles = handles;
    return ESP_OK;
}
#endif

static cJSON *app_cap_system_build_network_info(
    app_settings_service_handle_t settings)
{
    cJSON *root = cJSON_CreateObject();
    wifi_manager_status_t status = {0};
    wifi_ap_record_t ap = {0};

    if (!root) {
        return NULL;
    }
    wifi_manager_get_status(&status);
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddBoolToObject(root, "sta_connected", status.sta_connected);
    cJSON_AddBoolToObject(root, "sta_configured", status.sta_configured);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    cJSON_AddStringToObject(root, "mode", status.mode ? status.mode : "off");
    if (status.sta_ip) {
        cJSON_AddStringToObject(root, "sta_ip", status.sta_ip);
    }
    if (status.ap_ip) {
        cJSON_AddStringToObject(root, "ap_ip", status.ap_ip);
    }
    if (status.ap_ssid) {
        cJSON_AddStringToObject(root, "ap_ssid", status.ap_ssid);
    }
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (const char *)ap.ssid);
        cJSON_AddNumberToObject(root, "rssi", ap.rssi);
        cJSON_AddNumberToObject(root, "channel", ap.primary);
        cJSON_AddStringToObject(root, "auth_mode", app_cap_system_auth_mode(ap.authmode));
    }
    if (settings != NULL) {
        app_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        if (snapshot != NULL &&
                app_settings_service_get_snapshot(settings, snapshot) == ESP_OK &&
                snapshot->network.portal_url[0] != '\0') {
            cJSON_AddStringToObject(root, "portal_url",
                                    snapshot->network.portal_url);
        }
        free(snapshot);
    }
    return root;
}

static cJSON *app_cap_system_build_power_info(void)
{
    cJSON *root = cJSON_CreateObject();
    char profile[16] = {0};
    wifi_ps_type_t wifi_ps = WIFI_PS_NONE;

    if (!root) {
        return NULL;
    }
    (void)settings_store_get_string(APP_CAP_SYSTEM_POWER_PROFILE_KEY,
                                    profile,
                                    sizeof(profile),
                                    APP_CAP_SYSTEM_DEFAULT_POWER_PROFILE);
    cJSON_AddBoolToObject(root, "supported", true);
    cJSON_AddStringToObject(root, "profile", profile);
    if (esp_wifi_get_ps(&wifi_ps) == ESP_OK) {
        cJSON_AddStringToObject(root, "wifi_power_save",
                               wifi_ps == WIFI_PS_NONE ? "none" :
                               wifi_ps == WIFI_PS_MAX_MODEM ? "max_modem" : "min_modem");
    }
#if CONFIG_PM_ENABLE
    cJSON_AddBoolToObject(root, "dynamic_frequency_scaling", true);
    cJSON_AddBoolToObject(root, "automatic_light_sleep", true);
#else
    cJSON_AddBoolToObject(root, "dynamic_frequency_scaling", false);
    cJSON_AddBoolToObject(root, "automatic_light_sleep", false);
#endif
    cJSON_AddBoolToObject(root, "manual_light_sleep", true);
    cJSON_AddBoolToObject(root, "deep_sleep", true);
    return root;
}

static esp_err_t app_cap_system_get_info(const char *section,
                                         char *output,
                                         size_t output_size,
                                         void *user_ctx)
{
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    app_settings_service_handle_t settings = user_ctx;
    ESP_RETURN_ON_FALSE(section && output && output_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid info request");

    if (strcmp(section, "identity") == 0) {
        esp_board_info_t board = {0};
        uint8_t mac[6] = {0};
        char device_id[18];

        root = cJSON_CreateObject();
        if (root && esp_board_manager_get_board_info(&board) == ESP_OK) {
            cJSON_AddStringToObject(root, "board", board.name ? board.name : "unknown");
            cJSON_AddStringToObject(root, "manufacturer",
                                   board.manufacturer ? board.manufacturer : "unknown");
            cJSON_AddStringToObject(root, "board_version",
                                   board.version ? board.version : "unknown");
        }
        if (root && esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(device_id, sizeof(device_id),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            cJSON_AddStringToObject(root, "device_id", device_id);
        }
        if (root) {
            cJSON_AddBoolToObject(root, "supported", true);
        }
    } else if (strcmp(section, "network") == 0) {
        root = app_cap_system_build_network_info(settings);
    } else if (strcmp(section, "power") == 0) {
        root = app_cap_system_build_power_info();
    } else if (strcmp(section, "display") == 0) {
        root = cJSON_CreateObject();
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
        uint8_t brightness = 0;
        uint16_t rotation = 0;
        bool started = display_service_is_started();

        cJSON_AddBoolToObject(root, "supported", started);
        cJSON_AddBoolToObject(root, "started", started);
        if (started && display_service_get_brightness(&brightness) == ESP_OK) {
            cJSON_AddNumberToObject(root, "brightness", brightness);
        }
        if (started && display_service_get_rotation(&rotation) == ESP_OK) {
            cJSON_AddNumberToObject(root, "rotation", rotation);
        }
#else
        cJSON_AddBoolToObject(root, "supported", false);
#endif
    } else if (strcmp(section, "audio") == 0) {
        root = cJSON_CreateObject();
#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
        dev_audio_codec_handles_t *handles = NULL;
        int volume = 0;
        bool muted = false;

        err = app_cap_system_get_audio_codec(false, &handles);
        cJSON_AddBoolToObject(root, "supported",
                             esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_AUDIO_DAC));
        cJSON_AddBoolToObject(root, "initialized", err == ESP_OK);
        app_settings_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
        if (snapshot != NULL && settings != NULL &&
                app_settings_service_get_snapshot(settings, snapshot) == ESP_OK &&
                snapshot->audio_available) {
            volume = snapshot->volume;
            cJSON_AddNumberToObject(root, "volume", volume);
        }
        free(snapshot);
        if (err == ESP_OK && esp_codec_dev_get_out_mute(handles->codec_dev, &muted) == ESP_CODEC_DEV_OK) {
            cJSON_AddBoolToObject(root, "muted", muted);
        }
#else
        cJSON_AddBoolToObject(root, "supported", false);
#endif
        err = ESP_OK;
    } else if (strcmp(section, "battery") == 0) {
        root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "supported", false);
        cJSON_AddStringToObject(root, "message",
                               "board has no normalized battery provider");
    } else {
        return app_cap_system_render_error(output, output_size,
                                           ESP_ERR_NOT_SUPPORTED, "unknown platform section");
    }

    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "create info JSON failed");
    err = app_cap_system_render(root, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t app_cap_system_set_display(
                                            app_settings_service_handle_t settings,
                                            const cJSON *input,
                                            char *output,
                                            size_t output_size)
{
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUPPORT
    const cJSON *brightness = cJSON_GetObjectItem(input, "brightness");
    const cJSON *rotation = cJSON_GetObjectItem(input, "rotation");
    bool changed = false;
    esp_err_t err;

    if (brightness) {
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(brightness) &&
                            brightness->valueint >= 0 && brightness->valueint <= 100,
                            ESP_ERR_INVALID_ARG, TAG, "invalid brightness");
        err = display_service_set_brightness((uint8_t)brightness->valueint);
        if (err != ESP_OK) {
            return app_cap_system_render_error(output, output_size, err,
                                               "brightness control is not supported by this panel");
        }
        changed = true;
    }
    if (rotation) {
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(rotation),
                            ESP_ERR_INVALID_ARG, TAG, "invalid rotation");
        err = settings != NULL
            ? app_settings_service_set_rotation(
                  settings, (uint16_t)rotation->valueint)
            : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            return app_cap_system_render_error(output, output_size, err,
                                               "failed to set display rotation");
        }
        changed = true;
    }
    ESP_RETURN_ON_FALSE(changed, ESP_ERR_INVALID_ARG, TAG, "no display setting supplied");
    return app_cap_system_get_info("display", output, output_size, settings);
#else
    (void)settings;
    (void)input;
    return app_cap_system_render_error(output, output_size,
                                       ESP_ERR_NOT_SUPPORTED, "display is not supported");
#endif
}

static esp_err_t app_cap_system_set_audio(
                                          app_settings_service_handle_t settings,
                                          const cJSON *input,
                                          char *output,
                                          size_t output_size)
{
#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    const cJSON *volume = cJSON_GetObjectItem(input, "volume");
    const cJSON *muted = cJSON_GetObjectItem(input, "muted");
    dev_audio_codec_handles_t *handles = NULL;
    bool changed = false;
    int codec_err;
    esp_err_t err = app_cap_system_get_audio_codec(true, &handles);

    if (err != ESP_OK) {
        return app_cap_system_render_error(output, output_size, err, "audio output is unavailable");
    }
    if (volume) {
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(volume) &&
                            volume->valueint >= 0 && volume->valueint <= 100,
                            ESP_ERR_INVALID_ARG, TAG, "invalid volume");
        err = settings != NULL
            ? app_settings_service_set_volume(
                  settings, volume->valueint, true)
            : ESP_ERR_INVALID_STATE;
        if (err != ESP_OK) {
            return app_cap_system_render_error(output, output_size,
                                               err, "failed to set speaker volume");
        }
        changed = true;
    }
    if (muted) {
        ESP_RETURN_ON_FALSE(cJSON_IsBool(muted),
                            ESP_ERR_INVALID_ARG, TAG, "invalid mute state");
        codec_err = esp_codec_dev_set_out_mute(handles->codec_dev, cJSON_IsTrue(muted));
        if (codec_err != ESP_CODEC_DEV_OK && codec_err != ESP_CODEC_DEV_NOT_SUPPORT) {
            return app_cap_system_render_error(output, output_size,
                                               ESP_FAIL, "failed to set speaker mute");
        }
        changed = true;
    }
    ESP_RETURN_ON_FALSE(changed, ESP_ERR_INVALID_ARG, TAG, "no audio setting supplied");
    return app_cap_system_get_info("audio", output, output_size, settings);
#else
    (void)settings;
    (void)input;
    return app_cap_system_render_error(output, output_size,
                                       ESP_ERR_NOT_SUPPORTED, "audio output is not supported");
#endif
}

static esp_err_t app_cap_system_wifi_scan(const cJSON *input,
                                          char *output,
                                          size_t output_size)
{
    const cJSON *limit_item = cJSON_GetObjectItem(input, "limit");
    uint16_t limit = APP_CAP_SYSTEM_WIFI_SCAN_DEFAULT_LIMIT;
    wifi_manager_scan_record_t *records = NULL;
    uint16_t count = 0;
    cJSON *root = NULL;
    cJSON *networks = NULL;
    esp_err_t err;

    if (limit_item) {
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(limit_item) &&
                            limit_item->valueint >= 1 && limit_item->valueint <= 30,
                            ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi scan limit");
        limit = (uint16_t)limit_item->valueint;
    }
    records = calloc(limit, sizeof(*records));
    ESP_RETURN_ON_FALSE(records, ESP_ERR_NO_MEM, TAG, "allocate Wi-Fi scan records failed");
    err = wifi_manager_scan_aps(records, limit, &count);
    if (err != ESP_OK) {
        free(records);
        return app_cap_system_render_error(output, output_size, err, "Wi-Fi scan failed");
    }

    root = cJSON_CreateObject();
    networks = cJSON_CreateArray();
    if (!root || !networks) {
        free(records);
        cJSON_Delete(root);
        cJSON_Delete(networks);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddItemToObject(root, "networks", networks);
    for (uint16_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        cJSON_AddStringToObject(item, "ssid", records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(item, "channel", records[i].primary);
        cJSON_AddStringToObject(item, "auth_mode",
                               app_cap_system_auth_mode(records[i].authmode));
        cJSON_AddItemToArray(networks, item);
    }
    free(records);
    if (err == ESP_OK) {
        err = app_cap_system_render(root, output, output_size);
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t app_cap_system_wifi_configure(const cJSON *input,
                                               char *output,
                                               size_t output_size)
{
    const cJSON *ssid = cJSON_GetObjectItem(input, "ssid");
    const cJSON *password = cJSON_GetObjectItem(input, "password");
    const cJSON *wait_item = cJSON_GetObjectItem(input, "wait_timeout_ms");
    app_config_t *config = NULL;
    uint32_t wait_ms = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(cJSON_IsString(ssid) && ssid->valuestring &&
                        strlen(ssid->valuestring) > 0 && strlen(ssid->valuestring) <= 32,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi SSID");
    ESP_RETURN_ON_FALSE(cJSON_IsString(password) && password->valuestring &&
                        strlen(password->valuestring) <= 64,
                        ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi password");
    if (wait_item) {
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(wait_item) &&
                            wait_item->valuedouble >= 0 && wait_item->valuedouble <= 30000,
                            ESP_ERR_INVALID_ARG, TAG, "invalid Wi-Fi wait timeout");
        wait_ms = (uint32_t)wait_item->valuedouble;
    }

    config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config, ESP_ERR_NO_MEM, TAG, "allocate app config failed");
    err = app_config_load(config);
    if (err == ESP_OK) {
        strlcpy(config->wifi_ssid, ssid->valuestring, sizeof(config->wifi_ssid));
        strlcpy(config->wifi_password, password->valuestring, sizeof(config->wifi_password));
        err = app_config_validate_wifi(config, NULL);
    }
    if (err == ESP_OK) {
        err = app_config_save(config);
    }
    if (err == ESP_OK) {
        err = wifi_manager_apply_sta_config(&(wifi_manager_config_t){
            .sta_ssid = config->wifi_ssid,
            .sta_password = config->wifi_password,
            .ap_ssid = config->ap_ssid[0] ? config->ap_ssid : NULL,
            .ap_password = config->ap_password[0] ? config->ap_password : NULL,
            .ap_behavior = config->ap_behavior,
        });
    }
    free(config);
    if (err != ESP_OK) {
        return app_cap_system_render_error(output, output_size, err,
                                           "failed to persist or apply Wi-Fi configuration");
    }
    if (wait_ms > 0) {
        (void)wifi_manager_wait_connected(wait_ms);
    }
    return app_cap_system_get_info("network", output, output_size, NULL);
}

static esp_err_t app_cap_system_set_power(const cJSON *input,
                                          char *output,
                                          size_t output_size)
{
    const cJSON *profile_item = cJSON_GetObjectItem(input, "profile");
    const char *profile;
    wifi_ps_type_t wifi_ps;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(cJSON_IsString(profile_item) && profile_item->valuestring,
                        ESP_ERR_INVALID_ARG, TAG, "power profile is required");
    profile = profile_item->valuestring;
    if (strcmp(profile, "performance") == 0) {
        wifi_ps = WIFI_PS_NONE;
    } else if (strcmp(profile, "balanced") == 0) {
        wifi_ps = WIFI_PS_MIN_MODEM;
    } else if (strcmp(profile, "low_power") == 0) {
        wifi_ps = WIFI_PS_MAX_MODEM;
    } else {
        return app_cap_system_render_error(output, output_size,
                                           ESP_ERR_INVALID_ARG, "unknown power profile");
    }

    err = esp_wifi_set_ps(wifi_ps);
    if (err != ESP_OK) {
        return app_cap_system_render_error(output, output_size, err,
                                           "failed to apply Wi-Fi power saving");
    }
    err = settings_store_set_string(APP_CAP_SYSTEM_POWER_PROFILE_KEY, profile);
    if (err != ESP_OK) {
        return app_cap_system_render_error(output, output_size, err,
                                           "failed to persist power profile");
    }
    return app_cap_system_get_info("power", output, output_size, NULL);
}

static void app_cap_system_sleep_task(void *arg)
{
    app_cap_system_sleep_args_t *sleep_args = (app_cap_system_sleep_args_t *)arg;
    bool deep = sleep_args->deep;
    uint64_t duration_ms = sleep_args->duration_ms;

    free(sleep_args);
    vTaskDelay(pdMS_TO_TICKS(APP_CAP_SYSTEM_SLEEP_DELAY_MS));
    (void)esp_sleep_enable_timer_wakeup(duration_ms * 1000ULL);
    if (deep) {
        esp_deep_sleep_start();
    } else {
        (void)esp_light_sleep_start();
    }
    vTaskDelete(NULL);
}

static esp_err_t app_cap_system_enter_sleep(const cJSON *input,
                                            char *output,
                                            size_t output_size)
{
    const cJSON *mode = cJSON_GetObjectItem(input, "mode");
    const cJSON *duration = cJSON_GetObjectItem(input, "duration_ms");
    app_cap_system_sleep_args_t *args;
    uint64_t duration_ms;
    BaseType_t task_ok;

    ESP_RETURN_ON_FALSE(cJSON_IsString(mode) && mode->valuestring,
                        ESP_ERR_INVALID_ARG, TAG, "sleep mode is required");
    ESP_RETURN_ON_FALSE(cJSON_IsNumber(duration) &&
                        duration->valuedouble >= 1000 &&
                        duration->valuedouble <= (double)APP_CAP_SYSTEM_MAX_SLEEP_MS,
                        ESP_ERR_INVALID_ARG, TAG, "sleep duration must be 1000..86400000 ms");
    ESP_RETURN_ON_FALSE(strcmp(mode->valuestring, "light") == 0 ||
                        strcmp(mode->valuestring, "deep") == 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sleep mode");

    args = calloc(1, sizeof(*args));
    ESP_RETURN_ON_FALSE(args, ESP_ERR_NO_MEM, TAG, "allocate sleep args failed");
    args->deep = strcmp(mode->valuestring, "deep") == 0;
    args->duration_ms = (uint64_t)duration->valuedouble;
    duration_ms = args->duration_ms;
    task_ok = xTaskCreate(app_cap_system_sleep_task, "cap_system_sleep",
                          3072, args, 5, NULL);
    if (task_ok != pdPASS) {
        free(args);
        return app_cap_system_render_error(output, output_size,
                                           ESP_ERR_NO_MEM, "failed to schedule sleep");
    }
    snprintf(output, output_size,
             "{\"ok\":true,\"mode\":\"%s\",\"duration_ms\":%" PRIu64 ",\"delay_ms\":%u}",
             mode->valuestring, duration_ms, APP_CAP_SYSTEM_SLEEP_DELAY_MS);
    return ESP_OK;
}

static esp_err_t app_cap_system_execute(const char *operation,
                                        const char *input_json,
                                        char *output,
                                        size_t output_size,
                                        void *user_ctx)
{
    cJSON *input = NULL;
    esp_err_t err;

    app_settings_service_handle_t settings = user_ctx;
    ESP_RETURN_ON_FALSE(operation && output && output_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "invalid platform operation");
    input = cJSON_Parse(input_json ? input_json : "{}");
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return app_cap_system_render_error(output, output_size,
                                           ESP_ERR_INVALID_ARG, "invalid input JSON");
    }

    if (strcmp(operation, "set_display") == 0) {
        err = app_cap_system_set_display(
            settings, input, output, output_size);
    } else if (strcmp(operation, "set_audio") == 0) {
        err = app_cap_system_set_audio(
            settings, input, output, output_size);
    } else if (strcmp(operation, "vibrate") == 0) {
#if CONFIG_ESP_BOARD_DEV_LEDC_CTRL_SUPPORT
        const cJSON *duration = cJSON_GetObjectItem(input, "duration_ms");
        if (!cJSON_IsNumber(duration) ||
            duration->valuedouble < 10 || duration->valuedouble > 5000) {
            err = app_cap_system_render_error(output, output_size,
                                              ESP_ERR_INVALID_ARG, "invalid vibration duration");
        } else {
            err = app_cap_system_platform_vibrate(
                (uint32_t)duration->valuedouble);
            if (err == ESP_OK) {
                snprintf(output, output_size,
                         "{\"ok\":true,\"duration_ms\":%" PRIu32 "}",
                         (uint32_t)duration->valuedouble);
            } else {
                err = app_cap_system_render_error(output, output_size, err,
                                                  "vibration motor is unavailable");
            }
        }
#else
        err = app_cap_system_render_error(output, output_size,
                                          ESP_ERR_NOT_SUPPORTED, "vibration motor is unavailable");
#endif
    } else if (strcmp(operation, "scan_wifi_networks") == 0) {
        err = app_cap_system_wifi_scan(input, output, output_size);
    } else if (strcmp(operation, "configure_wifi") == 0) {
        err = app_cap_system_wifi_configure(input, output, output_size);
    } else if (strcmp(operation, "set_power_config") == 0) {
        err = app_cap_system_set_power(input, output, output_size);
    } else if (strcmp(operation, "enter_sleep") == 0) {
        err = app_cap_system_enter_sleep(input, output, output_size);
    } else {
        err = app_cap_system_render_error(output, output_size,
                                          ESP_ERR_NOT_SUPPORTED, "unknown platform operation");
    }
    cJSON_Delete(input);
    return err;
}

esp_err_t app_cap_system_platform_init(
    app_settings_service_handle_t settings)
{
    char profile[16] = {0};
    cJSON *input = NULL;
    char output[128];

    ESP_RETURN_ON_ERROR(
        cap_system_set_platform_provider(&(cap_system_platform_provider_t){
            .get_info = app_cap_system_get_info,
            .execute = app_cap_system_execute,
            .user_ctx = settings,
        }),
        TAG, "register cap_system platform provider failed");

    if (settings_store_get_string(APP_CAP_SYSTEM_POWER_PROFILE_KEY,
                                  profile,
                                  sizeof(profile),
                                  APP_CAP_SYSTEM_DEFAULT_POWER_PROFILE) == ESP_OK) {
        input = cJSON_CreateObject();
        if (input) {
            cJSON_AddStringToObject(input, "profile", profile);
            (void)app_cap_system_set_power(input, output, sizeof(output));
            cJSON_Delete(input);
        }
    }
    return ESP_OK;
}
