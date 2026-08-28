/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_settings_service.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_system_config.h"
#include "audio_hub.h"
#include "display_service.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

struct app_settings_service_t {
    SemaphoreHandle_t lock;
};

static const char *TAG = "app_settings";

#define APP_SETTINGS_BRIGHTNESS_UI_MAX       100
#define APP_SETTINGS_BRIGHTNESS_HW_MIN        10
#define APP_SETTINGS_BRIGHTNESS_HW_MAX       100

static uint8_t app_settings_brightness_to_hardware(int brightness)
{
    const int span = APP_SETTINGS_BRIGHTNESS_HW_MAX -
        APP_SETTINGS_BRIGHTNESS_HW_MIN;
    return (uint8_t)(APP_SETTINGS_BRIGHTNESS_HW_MIN +
        (brightness * span + APP_SETTINGS_BRIGHTNESS_UI_MAX / 2) /
            APP_SETTINGS_BRIGHTNESS_UI_MAX);
}

static bool app_settings_parse_bool(const char *value)
{
    return value != NULL &&
           (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
}

static bool app_settings_rotation_valid(int rotation)
{
    return rotation == 0 || rotation == 90 ||
           rotation == 180 || rotation == 270;
}

static bool app_settings_screen_timeout_valid(uint32_t timeout_ms)
{
    return timeout_ms == 0U || timeout_ms == 10000U ||
           timeout_ms == 30000U || timeout_ms == 60000U ||
           timeout_ms == 120000U || timeout_ms == 300000U;
}

esp_err_t app_settings_service_create(
    app_settings_service_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(ret_handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "output handle missing");
    *ret_handle = NULL;

    app_settings_service_handle_t handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate service failed");
    handle->lock = xSemaphoreCreateMutex();
    if (handle->lock == NULL) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = handle;
    return ESP_OK;
}

void app_settings_service_delete(app_settings_service_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    vSemaphoreDelete(handle->lock);
    free(handle);
}

esp_err_t app_settings_service_set_rotation(
    app_settings_service_handle_t handle, uint16_t degrees)
{
    ESP_RETURN_ON_FALSE(handle != NULL &&
                        app_settings_rotation_valid(degrees),
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid rotation");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    esp_err_t err = display_service_set_rotation(degrees);
    if (err == ESP_OK) {
        app_system_config_t config;
        err = app_system_config_load(&config);
        if (err == ESP_OK) {
            config.rotation = degrees;
            err = app_system_config_save(&config);
        }
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_set_volume(
    app_settings_service_handle_t handle, int volume, bool persist)
{
    ESP_RETURN_ON_FALSE(handle != NULL && volume >= 0 && volume <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "invalid volume");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    audio_mixer_handle_t mixer = NULL;
    esp_err_t err = audio_hub_get_mixer(&mixer);
    if (err == ESP_OK) {
        err = audio_mixer_set_output_volume(mixer, volume);
    }
    if (err == ESP_OK && persist) {
        app_system_config_t config;
        err = app_system_config_load(&config);
        if (err == ESP_OK) {
            config.volume = (uint8_t)volume;
            err = app_system_config_save(&config);
        }
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_restore_display(
    app_settings_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    app_system_config_t config;
    ESP_RETURN_ON_ERROR(app_system_config_load(&config), TAG,
                        "load display settings");
    if (config.rotation == 0) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(display_service_set_rotation(config.rotation), TAG,
                        "restore display rotation");
    ESP_LOGI(TAG, "Restored rotation: %u degrees", config.rotation);
    return ESP_OK;
}

esp_err_t app_settings_service_restore_audio(
    app_settings_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    app_system_config_t config;
    ESP_RETURN_ON_ERROR(app_system_config_load(&config), TAG,
                        "load audio settings");
    return app_settings_service_set_volume(handle, config.volume, false);
}

esp_err_t app_settings_service_restore_brightness(
    app_settings_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    app_system_config_t config;
    ESP_RETURN_ON_ERROR(app_system_config_load(&config), TAG,
                        "load brightness setting");
    const uint8_t hardware_brightness =
        app_settings_brightness_to_hardware(config.brightness);
    ESP_RETURN_ON_ERROR(display_service_set_brightness(hardware_brightness),
                        TAG, "restore brightness");
    ESP_LOGI(TAG, "Restored brightness: UI=%u%% hardware=%u%%",
             config.brightness, hardware_brightness);
    return ESP_OK;
}

esp_err_t app_settings_service_prepare_brightness_fade(
    app_settings_service_handle_t handle, uint8_t start_percent,
    uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    app_system_config_t config;
    ESP_RETURN_ON_ERROR(app_system_config_load(&config), TAG,
                        "load brightness setting");
    const uint8_t target =
        app_settings_brightness_to_hardware(config.brightness);
    return display_service_prepare_brightness_fade(
        start_percent, target, duration_ms);
}

esp_err_t app_settings_service_set_brightness(
    app_settings_service_handle_t handle, int brightness, bool persist)
{
    ESP_RETURN_ON_FALSE(handle != NULL && brightness >= 0 && brightness <= 100,
                        ESP_ERR_INVALID_ARG, TAG, "invalid brightness");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    esp_err_t err = display_service_set_brightness(
        app_settings_brightness_to_hardware(brightness));
    if (err == ESP_OK && persist) {
        app_system_config_t config;
        err = app_system_config_load(&config);
        if (err == ESP_OK) {
            config.brightness = (uint8_t)brightness;
            err = app_system_config_save(&config);
        }
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_set_vibration(
    app_settings_service_handle_t handle, bool enabled)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    app_system_config_t config;
    esp_err_t err = app_system_config_load(&config);
    if (err == ESP_OK) {
        config.vibration_enabled = enabled;
        err = app_system_config_save(&config);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_set_screen_timeout(
    app_settings_service_handle_t handle, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(handle != NULL &&
                        app_settings_screen_timeout_valid(timeout_ms),
                        ESP_ERR_INVALID_ARG, TAG, "invalid screen timeout");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    app_system_config_t config;
    esp_err_t err = app_system_config_load(&config);
    if (err == ESP_OK) {
        config.screen_timeout_ms = timeout_ms;
        err = app_system_config_save(&config);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_get_wifi_enabled(
    app_settings_service_handle_t handle, bool *ret_enabled)
{
    ESP_RETURN_ON_FALSE(handle != NULL && ret_enabled != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid Wi-Fi enabled read");
    app_system_config_t config;
    ESP_RETURN_ON_ERROR(app_system_config_load(&config), TAG,
                        "load Wi-Fi enabled");
    *ret_enabled = config.wifi_enabled;
    return ESP_OK;
}

esp_err_t app_settings_service_set_wifi_enabled(
    app_settings_service_handle_t handle, bool enabled)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    app_system_config_t config;
    esp_err_t err = app_system_config_load(&config);
    if (err == ESP_OK) {
        config.wifi_enabled = enabled;
        err = app_system_config_save(&config);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_get_boot_stage(
    app_settings_service_handle_t handle,
    app_system_boot_stage_t *ret_stage)
{
    ESP_RETURN_ON_FALSE(handle != NULL && ret_stage != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid boot stage read");
    app_system_config_t config;
    ESP_RETURN_ON_ERROR(app_system_config_load(&config), TAG,
                        "load boot stage");
    *ret_stage = config.boot_stage;
    return ESP_OK;
}

esp_err_t app_settings_service_set_boot_stage(
    app_settings_service_handle_t handle,
    app_system_boot_stage_t stage)
{
    ESP_RETURN_ON_FALSE(handle != NULL && stage <= APP_SYSTEM_BOOT_HOME,
                        ESP_ERR_INVALID_ARG, TAG, "invalid boot stage");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    app_system_config_t config;
    esp_err_t err = app_system_config_load(&config);
    if (err == ESP_OK) {
        config.boot_stage = stage;
        err = app_system_config_save(&config);
    }
    xSemaphoreGive(handle->lock);
    return err;
}

esp_err_t app_settings_service_reset_all(
    app_settings_service_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "service missing");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(handle->lock, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "settings service busy");
    /* App and system settings intentionally share the "app" NVS namespace.
     * Erasing it clears the boot stage, display/audio preferences, Wi-Fi
     * credentials, IM/account bindings, LLM credentials, and the checksum in
     * one committed operation.  Defaults are restored during the next boot. */
    esp_err_t err = app_config_reset_all();
    xSemaphoreGive(handle->lock);
    return err;
}

static void app_settings_fill_network(app_settings_snapshot_t *snapshot,
                                      const app_config_t *config)
{
    wifi_manager_status_t wifi = {0};
    wifi_manager_get_status(&wifi);

    snapshot->network.connected = wifi.sta_connected;
    snapshot->network.configured = wifi.sta_configured;
    snapshot->network.ap_active = wifi.ap_active;
    strlcpy(snapshot->network.ssid,
            config->wifi_ssid,
            sizeof(snapshot->network.ssid));
    strlcpy(snapshot->network.ip,
            wifi.sta_connected && wifi.sta_ip != NULL ? wifi.sta_ip :
            (wifi.ap_ip != NULL ? wifi.ap_ip : ""),
            sizeof(snapshot->network.ip));
    if (snapshot->network.ip[0] != '\0') {
        snprintf(snapshot->network.portal_url,
                 sizeof(snapshot->network.portal_url),
                 "http://%s/", snapshot->network.ip);
    }
}

static void app_settings_fill_im(app_settings_snapshot_t *snapshot,
                                 const app_config_t *config)
{
    snapshot->im.wechat_configured =
        config->wechat_token[0] != '\0' && config->wechat_base_url[0] != '\0';
    snapshot->im.qq_configured =
        config->qq_app_id[0] != '\0' && config->qq_app_secret[0] != '\0';
    snapshot->im.feishu_configured =
        config->feishu_app_id[0] != '\0' && config->feishu_app_secret[0] != '\0';
    snapshot->im.telegram_configured = config->tg_bot_token[0] != '\0';
}

static void app_settings_fill_llm(app_settings_snapshot_t *snapshot,
                                  const app_config_t *config)
{
    strlcpy(snapshot->llm.backend, config->llm_backend_type,
            sizeof(snapshot->llm.backend));
    strlcpy(snapshot->llm.model, config->llm_model,
            sizeof(snapshot->llm.model));
    strlcpy(snapshot->llm.base_url, config->llm_base_url,
            sizeof(snapshot->llm.base_url));
    snapshot->llm.api_key_configured = config->llm_api_key[0] != '\0';
    snapshot->llm.supports_tools =
        app_settings_parse_bool(config->llm_supports_tools);
    snapshot->llm.supports_vision =
        app_settings_parse_bool(config->llm_supports_vision);
}

esp_err_t app_settings_service_get_snapshot(
    app_settings_service_handle_t handle,
    app_settings_snapshot_t *ret_snapshot)
{
    ESP_RETURN_ON_FALSE(handle != NULL && ret_snapshot != NULL,
                        ESP_ERR_INVALID_ARG, TAG,
                        "service or output missing");
    app_config_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate config snapshot failed");
    esp_err_t err = app_config_load(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }

    memset(ret_snapshot, 0, sizeof(*ret_snapshot));
    app_system_config_t system_config;
    err = app_system_config_load(&system_config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }
    ret_snapshot->rotation = system_config.rotation;
    ret_snapshot->brightness = system_config.brightness;
    ret_snapshot->volume = system_config.volume;
    ret_snapshot->screen_timeout_ms = system_config.screen_timeout_ms;
    ret_snapshot->vibration_enabled = system_config.vibration_enabled;
    ret_snapshot->boot_stage = system_config.boot_stage;
    ret_snapshot->display_available = display_service_is_started();
    if (ret_snapshot->display_available) {
        (void)display_service_get_rotation(&ret_snapshot->rotation);
    }
    audio_mixer_handle_t mixer = NULL;
    ret_snapshot->audio_available = audio_hub_is_started() &&
        audio_hub_get_mixer(&mixer) == ESP_OK;
    if (ret_snapshot->audio_available) {
        (void)audio_mixer_get_output_volume(mixer, &ret_snapshot->volume);
    }
    app_settings_fill_network(ret_snapshot, config);
    app_settings_fill_im(ret_snapshot, config);
    app_settings_fill_llm(ret_snapshot, config);
    free(config);
    return ESP_OK;
}
