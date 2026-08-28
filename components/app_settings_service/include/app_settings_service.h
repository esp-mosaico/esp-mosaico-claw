/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_system_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_SSID_LEN       33U
#define APP_SETTINGS_IP_LEN         16U
#define APP_SETTINGS_PORTAL_URL_LEN 48U
#define APP_SETTINGS_LLM_BACKEND_LEN 32U
#define APP_SETTINGS_LLM_MODEL_LEN   64U
#define APP_SETTINGS_LLM_URL_LEN     160U

typedef struct app_settings_service_t *app_settings_service_handle_t;

typedef struct {
    bool connected;
    bool configured;
    bool ap_active;
    char ssid[APP_SETTINGS_SSID_LEN];
    char ip[APP_SETTINGS_IP_LEN];
    char portal_url[APP_SETTINGS_PORTAL_URL_LEN];
} app_settings_network_t;

typedef struct {
    bool wechat_configured;
    bool qq_configured;
    bool feishu_configured;
    bool telegram_configured;
} app_settings_im_t;

typedef struct {
    char backend[APP_SETTINGS_LLM_BACKEND_LEN];
    char model[APP_SETTINGS_LLM_MODEL_LEN];
    char base_url[APP_SETTINGS_LLM_URL_LEN];
    bool api_key_configured;
    bool supports_tools;
    bool supports_vision;
} app_settings_llm_t;

typedef struct {
    uint16_t rotation;
    int volume;
    int brightness;
    uint32_t screen_timeout_ms;
    bool vibration_enabled;
    app_system_boot_stage_t boot_stage;
    bool display_available;
    bool audio_available;
    app_settings_network_t network;
    app_settings_im_t im;
    app_settings_llm_t llm;
} app_settings_snapshot_t;

esp_err_t app_settings_service_create(
    app_settings_service_handle_t *ret_handle);
void app_settings_service_delete(app_settings_service_handle_t handle);

esp_err_t app_settings_service_restore_display(
    app_settings_service_handle_t handle);
esp_err_t app_settings_service_restore_audio(
    app_settings_service_handle_t handle);
esp_err_t app_settings_service_restore_brightness(
    app_settings_service_handle_t handle);
esp_err_t app_settings_service_prepare_brightness_fade(
    app_settings_service_handle_t handle, uint8_t start_percent,
    uint32_t duration_ms);
esp_err_t app_settings_service_get_snapshot(
    app_settings_service_handle_t handle,
    app_settings_snapshot_t *ret_snapshot);

esp_err_t app_settings_service_set_rotation(
    app_settings_service_handle_t handle, uint16_t degrees);
esp_err_t app_settings_service_set_volume(
    app_settings_service_handle_t handle, int volume, bool persist);
esp_err_t app_settings_service_set_brightness(
    app_settings_service_handle_t handle, int brightness, bool persist);
esp_err_t app_settings_service_set_vibration(
    app_settings_service_handle_t handle, bool enabled);
esp_err_t app_settings_service_set_screen_timeout(
    app_settings_service_handle_t handle, uint32_t timeout_ms);
esp_err_t app_settings_service_get_wifi_enabled(
    app_settings_service_handle_t handle, bool *ret_enabled);
esp_err_t app_settings_service_set_wifi_enabled(
    app_settings_service_handle_t handle, bool enabled);
esp_err_t app_settings_service_get_boot_stage(
    app_settings_service_handle_t handle,
    app_system_boot_stage_t *ret_stage);
esp_err_t app_settings_service_set_boot_stage(
    app_settings_service_handle_t handle,
    app_system_boot_stage_t stage);
esp_err_t app_settings_service_reset_all(
    app_settings_service_handle_t handle);

#ifdef __cplusplus
}
#endif
