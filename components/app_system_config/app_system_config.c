/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_system_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "settings_store.h"

#define SYSTEM_BOOT_STAGE_KEY "sys_boot_stage"
#define SYSTEM_ROTATION_KEY   "sys_rotation"
#define SYSTEM_BRIGHTNESS_KEY "sys_brightness"
#define SYSTEM_VOLUME_KEY     "sys_volume"
#define SYSTEM_SCREEN_TIMEOUT_KEY "sys_screen_to"
#define SYSTEM_VIBRATION_KEY  "sys_vibration"
#define SYSTEM_WIFI_ENABLED_KEY "sys_wifi_en"

#define SYSTEM_SCREEN_TIMEOUT_DEFAULT_MS 30000
#define SYSTEM_SCREEN_TIMEOUT_MAX_MS     1800000

static bool screen_timeout_valid(uint32_t timeout_ms)
{
    return timeout_ms == 0U || timeout_ms == 10000U ||
           timeout_ms == 30000U || timeout_ms == 60000U ||
           timeout_ms == 120000U || timeout_ms == 300000U;
}

static const char *TAG = "system_config";

typedef struct {
    const char *key;
    const char *legacy_key;
    int min_value;
    int max_value;
    int default_value;
    int *value;
} system_int_field_t;

static esp_err_t read_int(const system_int_field_t *field)
{
    char text[16] = {0};
    char fallback[16];
    char *end = NULL;

    snprintf(fallback, sizeof(fallback), "%d", field->default_value);
    const char *key = field->key;
    if (field->legacy_key != NULL) {
        bool has_current = false;
        ESP_RETURN_ON_ERROR(settings_store_has_key(field->key, &has_current),
                            TAG, "check %s", field->key);
        if (!has_current) {
            key = field->legacy_key;
        }
    }
    ESP_RETURN_ON_ERROR(settings_store_get_string(
                            key, text, sizeof(text), fallback),
                        TAG, "read %s", key);
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < field->min_value ||
            parsed > field->max_value) {
        parsed = field->default_value;
    }
    *field->value = (int)parsed;
    return ESP_OK;
}

static esp_err_t write_int(const char *key, int value)
{
    char text[16];
    int length = snprintf(text, sizeof(text), "%d", value);
    ESP_RETURN_ON_FALSE(length > 0 && (size_t)length < sizeof(text),
                        ESP_ERR_INVALID_SIZE, TAG, "format %s", key);
    return settings_store_set_string(key, text);
}

void app_system_config_defaults(app_system_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (app_system_config_t) {
        .boot_stage = APP_SYSTEM_BOOT_SETUP,
        .rotation = 0,
        .brightness = 100,
        .volume = 80,
        .screen_timeout_ms = SYSTEM_SCREEN_TIMEOUT_DEFAULT_MS,
        .vibration_enabled = true,
        .wifi_enabled = true,
    };
}

esp_err_t app_system_config_load(app_system_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "output config missing");
    app_system_config_defaults(config);
    int boot_stage = config->boot_stage;
    int rotation = config->rotation;
    int brightness = config->brightness;
    int volume = config->volume;
    int screen_timeout = config->screen_timeout_ms;
    int vibration = config->vibration_enabled;
    int wifi_enabled = config->wifi_enabled;
    system_int_field_t fields[] = {
        {SYSTEM_BOOT_STAGE_KEY, NULL,
         APP_SYSTEM_BOOT_SETUP, APP_SYSTEM_BOOT_HOME,
         APP_SYSTEM_BOOT_SETUP, &boot_stage},
        {SYSTEM_ROTATION_KEY, "ui_rotation", 0, 270, 0, &rotation},
        {SYSTEM_BRIGHTNESS_KEY, NULL, 0, 100, 100, &brightness},
        {SYSTEM_VOLUME_KEY, "audio_volume", 0, 100, 80, &volume},
        {SYSTEM_SCREEN_TIMEOUT_KEY, NULL, 0, SYSTEM_SCREEN_TIMEOUT_MAX_MS,
         SYSTEM_SCREEN_TIMEOUT_DEFAULT_MS, &screen_timeout},
        {SYSTEM_VIBRATION_KEY, NULL, 0, 1, 1, &vibration},
        {SYSTEM_WIFI_ENABLED_KEY, NULL, 0, 1, 1, &wifi_enabled},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        ESP_RETURN_ON_ERROR(read_int(&fields[i]), TAG, "load system config");
    }
    if (rotation != 0 && rotation != 90 && rotation != 180 &&
            rotation != 270) {
        rotation = 0;
    }
    if (!screen_timeout_valid((uint32_t)screen_timeout)) {
        screen_timeout = SYSTEM_SCREEN_TIMEOUT_DEFAULT_MS;
    }
    config->boot_stage = (app_system_boot_stage_t)boot_stage;
    config->rotation = (uint16_t)rotation;
    config->brightness = (uint8_t)brightness;
    config->volume = (uint8_t)volume;
    config->screen_timeout_ms = (uint32_t)screen_timeout;
    config->vibration_enabled = vibration != 0;
    config->wifi_enabled = wifi_enabled != 0;
    return ESP_OK;
}

esp_err_t app_system_config_save(const app_system_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL &&
                            config->boot_stage <= APP_SYSTEM_BOOT_HOME &&
                            (config->rotation == 0 || config->rotation == 90 ||
                             config->rotation == 180 || config->rotation == 270) &&
                            config->brightness <= 100 && config->volume <= 100 &&
                            screen_timeout_valid(config->screen_timeout_ms),
                        ESP_ERR_INVALID_ARG, TAG, "invalid system config");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_BOOT_STAGE_KEY, config->boot_stage),
                        TAG, "save boot stage");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_ROTATION_KEY, config->rotation),
                        TAG, "save rotation");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_BRIGHTNESS_KEY, config->brightness),
                        TAG, "save brightness");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_VOLUME_KEY, config->volume),
                        TAG, "save volume");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_SCREEN_TIMEOUT_KEY,
                                  (int)config->screen_timeout_ms),
                        TAG, "save screen timeout");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_VIBRATION_KEY,
                                  config->vibration_enabled ? 1 : 0),
                        TAG, "save vibration");
    ESP_RETURN_ON_ERROR(write_int(SYSTEM_WIFI_ENABLED_KEY,
                                  config->wifi_enabled ? 1 : 0),
                        TAG, "save Wi-Fi enabled");
    return settings_store_commit();
}

esp_err_t app_system_config_reset(void)
{
    static const char *const keys[] = {
        SYSTEM_BOOT_STAGE_KEY,
        SYSTEM_ROTATION_KEY,
        SYSTEM_BRIGHTNESS_KEY,
        SYSTEM_VOLUME_KEY,
        SYSTEM_SCREEN_TIMEOUT_KEY,
        SYSTEM_VIBRATION_KEY,
        SYSTEM_WIFI_ENABLED_KEY,
        /* Remove the previous scattered keys during migration/reset. */
        "ui_rotation",
        "audio_volume",
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        ESP_RETURN_ON_ERROR(settings_store_erase_key(keys[i]), TAG,
                            "erase %s", keys[i]);
    }
    return settings_store_commit();
}
