/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw.h"
#include "app_capabilities.h"
#include "app_settings_service.h"
#include "app_fs.h"
#include "app_factory_reset.h"
#include "claw_version.h"
#include "claw_paths.h"
#include "claw_hw_registry.h"
#include "edge_agent_version.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "wifi_manager.h"
#include "cap_im_ai_create.h"
#include "claw_event_router.h"
#include "mosaic_setup.h"
#include "mosaic_ui.h"
#include "mosaic_capability.h"
#include "mosaic_button_platform.h"
#include "weather_service.h"
#include "update_check_service.h"
#include "network_provisioning_service.h"
#include "wechat_binding_service.h"
#include "audio_hub.h"
#include "time.h"
#include "nvs_flash.h"
#include "http_server.h"
#include "hot_plug_register.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_board_manager_includes.h"
#include "captive_dns.h"
#include "cmd_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_config.h"
#include "trial_auth.h"
#include "cap_system_platform.h"
#include "mosaic_net_weather.h"
#include "mosaic_device_ops.h"
#include "mosaico_board_variant.h"
#ifdef ESP_MOSAICO_REMOTE_DEBUG
#include "esp_remote_mosaic_adapter.h"
#endif

#define APP_ENABLE_MEM_LOG        (0)
#define APP_ENABLE_CLAW_TASKS     (1)
#define APP_WEATHER_USER_AGENT \
    "esp-mosaico-claw/1.0 https://github.com/espressif/esp-claw"
#define APP_UPDATE_USER_AGENT "esp-mosaico-claw/update-check"

static const char *TAG = "app";

static EXT_RAM_BSS_ATTR app_config_t *s_config;
static EXT_RAM_BSS_ATTR app_claw_config_t *s_claw_config;
static EXT_RAM_BSS_ATTR app_settings_service_handle_t s_app_settings;
static EXT_RAM_BSS_ATTR network_provisioning_service_handle_t s_network_provisioning;
static EXT_RAM_BSS_ATTR wechat_binding_service_handle_t s_wechat_binding;
static EXT_RAM_BSS_ATTR asr_service_handle_t s_asr_service;
static SemaphoreHandle_t s_asr_config_lock;

/* Keep the Setup Center model enum locked to the binding service contract. */
#define ASSERT_WECHAT_STATE_MATCH(name)                                      \
    _Static_assert((int)WECHAT_BINDING_STATE_##name ==                       \
                       (int)MOSAIC_SETUP_WECHAT_##name,                      \
                   "Setup Center WeChat state mismatch: " #name)
ASSERT_WECHAT_STATE_MATCH(IDLE);
ASSERT_WECHAT_STATE_MATCH(WAITING_SCAN);
ASSERT_WECHAT_STATE_MATCH(SCANNED);
ASSERT_WECHAT_STATE_MATCH(SAVING);
ASSERT_WECHAT_STATE_MATCH(AWAITING_SAVE);
ASSERT_WECHAT_STATE_MATCH(COMPLETE);
ASSERT_WECHAT_STATE_MATCH(CANCELLED);
ASSERT_WECHAT_STATE_MATCH(EXPIRED);
ASSERT_WECHAT_STATE_MATCH(ERROR);
#undef ASSERT_WECHAT_STATE_MATCH

static esp_err_t main_register_ai_create_capability(
    const app_claw_config_t *config,
    const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;
    ESP_RETURN_ON_ERROR(cap_im_ai_create_register_group(), TAG,
                        "Failed to register Mosaico AI Create capability");
    return claw_event_router_register_outbound_binding(
        CAP_IM_AI_CREATE_CHANNEL, "ai_create_send_message");
}

static bool main_asr_config_changed(
    const app_config_t *before,
    const app_config_t *after)
{
    return before == NULL || after == NULL ||
           strcmp(before->asr_provider, after->asr_provider) != 0 ||
           strcmp(before->asr_api_key, after->asr_api_key) != 0 ||
           strcmp(before->asr_workspace_id, after->asr_workspace_id) != 0 ||
           strcmp(before->asr_language_hint, after->asr_language_hint) != 0 ||
           strcmp(before->asr_model, after->asr_model) != 0 ||
           strcmp(before->asr_endpoint, after->asr_endpoint) != 0;
}

static esp_err_t main_set_asr_unavailable(
    ai_create_voice_readiness_t readiness,
    esp_err_t error)
{
    return mosaic_ui_set_ai_create_voice_status(
        (ai_create_voice_status_t) {
            .readiness = readiness,
            .error = error,
            .retryable = readiness == AI_CREATE_VOICE_READINESS_PROVIDER_ERROR,
        });
}

static esp_err_t main_apply_asr_config(const app_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG,
                        "ASR config is NULL");
    if (!s_asr_config_lock) {
        s_asr_config_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_asr_config_lock, ESP_ERR_NO_MEM, TAG,
                            "Failed to create ASR config lock");
    }

    xSemaphoreTake(s_asr_config_lock, portMAX_DELAY);
    asr_service_handle_t previous = s_asr_service;
    asr_service_handle_t replacement = NULL;
    audio_capture_handle_t capture = NULL;
    esp_err_t err;
    const bool trial_provider = config->asr_provider[0] &&
                                strcmp(config->asr_provider, "trial") == 0;

    if (!trial_provider && !config->asr_api_key[0]) {
        err = main_set_asr_unavailable(
            AI_CREATE_VOICE_READINESS_DISABLED, ESP_ERR_NOT_SUPPORTED);
    } else if (config->asr_provider[0] &&
               !trial_provider && strcmp(config->asr_provider, "qwen") != 0) {
        err = main_set_asr_unavailable(
            AI_CREATE_VOICE_READINESS_PROVIDER_ERROR,
            ESP_ERR_NOT_SUPPORTED);
    } else if (audio_hub_get_capture(&capture) != ESP_OK || capture == NULL) {
        err = main_set_asr_unavailable(
            AI_CREATE_VOICE_READINESS_AUDIO_ERROR,
            ESP_ERR_INVALID_STATE);
    } else {
        err = asr_service_create(&(asr_service_config_t) {
            .capture = capture,
            .provider = trial_provider ? ASR_SERVICE_PROVIDER_TRIAL
                                       : ASR_SERVICE_PROVIDER_QWEN,
            .api_key = config->asr_api_key,
            .workspace_id = config->asr_workspace_id[0]
                                ? config->asr_workspace_id : NULL,
            .endpoint = config->asr_endpoint[0]
                            ? config->asr_endpoint
                            : ASR_SERVICE_DEFAULT_ENDPOINT,
            .model = config->asr_model[0]
                         ? config->asr_model : ASR_SERVICE_DEFAULT_MODEL,
            .language_hint = config->asr_language_hint[0]
                                 ? config->asr_language_hint
                                 : ASR_SERVICE_DEFAULT_LANGUAGE_HINT,
            .connect_timeout_ms = 30000,
            .send_timeout_ms = 3000,
        }, &replacement);
        if (err == ESP_OK) {
            err = mosaic_ui_set_ai_create_asr(replacement);
        } else {
            ESP_LOGW(TAG, "ASR service unavailable: %s",
                     esp_err_to_name(err));
            esp_err_t status_err = main_set_asr_unavailable(
                AI_CREATE_VOICE_READINESS_PROVIDER_ERROR, err);
            err = status_err;
        }
    }

    if (err == ESP_OK) {
        s_asr_service = replacement;
        replacement = NULL;
    }
    if (replacement) asr_service_delete(replacement);
    if (err == ESP_OK && previous) asr_service_delete(previous);
    xSemaphoreGive(s_asr_config_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ASR runtime configuration applied: enabled=%d provider=%s",
                 s_asr_service != NULL,
                 config->asr_provider[0] ? config->asr_provider : "qwen");
    }
    return err;
}

static esp_err_t app_allocate_runtime_state(void)
{
    if (!s_config) {
        s_config = calloc(1, sizeof(*s_config));
    }
    if (!s_claw_config) {
        s_claw_config = calloc(1, sizeof(*s_claw_config));
    }

    ESP_RETURN_ON_FALSE(s_config && s_claw_config, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate runtime state");

    return ESP_OK;
}

static void app_free_runtime_state(void)
{
    free(s_claw_config);
    s_claw_config = NULL;

    free(s_config);
    s_config = NULL;
}

static void log_wifi_startup_config(const app_config_t *config)
{
    ESP_LOGI(TAG,
             "Wi-Fi startup STA: ssid=%s pwd_len=%u",
             config->wifi_ssid[0] ? config->wifi_ssid : "(empty)",
             (unsigned)strlen(config->wifi_password));

    ESP_LOGI(TAG,
             "Wi-Fi startup AP: ssid=%s pwd_len=%u behavior=%s",
             config->ap_ssid[0] ? config->ap_ssid : "(auto:mac-suffix)",
             (unsigned)strlen(config->ap_password),
             config->ap_behavior[0] ? config->ap_behavior : "keep");
}

static bool main_network_ready(void *user_ctx)
{
    wifi_manager_status_t status = {0};

    (void)user_ctx;
    wifi_manager_get_status(&status);
    return status.sta_connected;
}

static void on_network_provisioning_changed(
    network_provisioning_service_handle_t service,
    const network_provisioning_status_t *status,
    void *user_ctx)
{
    (void)service;
    (void)status;
    (void)user_ctx;

    // ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
    //          status->sta_connected,
    //          status->ap_active,
    //          status->sta_connected ? "sta+ap" : "ap",
    //          status->ap_ssid[0] ? status->ap_ssid : "(none)");

    mosaic_device_ops_notify_network_changed();
}

static void on_device_setup_wechat(
    wechat_binding_service_handle_t service,
    const wechat_binding_status_t *status,
    void *user_ctx)
{
    (void)service;
    (void)user_ctx;
    mosaic_setup_wechat_status_t *model = calloc(1, sizeof(*model));
    if (model == NULL) {
        ESP_LOGE(TAG, "Failed to allocate Setup Center WeChat model");
        return;
    }
    model->active = status->active;
    model->configured = status->configured;
    model->persisted = status->persisted;
    model->state = (mosaic_setup_wechat_state_t)status->state;
    strlcpy(model->message, status->message, sizeof(model->message));
    strlcpy(model->qr_payload, status->qr_payload,
            sizeof(model->qr_payload));
    strlcpy(model->account_id, status->account_id,
            sizeof(model->account_id));
    (void)mosaic_setup_set_wechat_status(model);
    free(model);
}

static esp_err_t device_setup_start_wechat(
    void *user_ctx, const char *account_id, bool force)
{
    (void)user_ctx;
    return wechat_binding_service_start(s_wechat_binding, account_id, force,
        WECHAT_BINDING_PERSIST_AUTOMATIC);
}

static esp_err_t device_setup_cancel_wechat(void *user_ctx)
{
    (void)user_ctx;
    return wechat_binding_service_cancel(s_wechat_binding);
}

static esp_err_t main_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static bool main_wifi_config_changed(
    const app_config_t *before,
    const app_config_t *after)
{
    return before == NULL || after == NULL ||
           strcmp(before->wifi_ssid, after->wifi_ssid) != 0 ||
           strcmp(before->wifi_password, after->wifi_password) != 0 ||
           strcmp(before->ap_ssid, after->ap_ssid) != 0 ||
           strcmp(before->ap_password, after->ap_password) != 0 ||
           strcmp(before->ap_behavior, after->ap_behavior) != 0;
}

static esp_err_t main_save_config(const app_config_t *config)
{
    esp_err_t err;
    app_claw_config_t *claw_config = NULL;
    app_config_t *previous_config = NULL;
    bool wifi_changed = true;
    bool asr_changed = true;

    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_ERROR(app_config_validate_wifi(config, NULL), TAG, "Invalid Wi-Fi config");

    previous_config = calloc(1, sizeof(*previous_config));
    if (previous_config != NULL &&
            app_config_load(previous_config) == ESP_OK) {
        wifi_changed = main_wifi_config_changed(
                           previous_config, config);
        asr_changed = main_asr_config_changed(
                          previous_config, config);
    }
    free(previous_config);

    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }

    claw_config = calloc(1, sizeof(*claw_config));
    if (!claw_config) {
        ESP_LOGW(TAG, "Failed to allocate Claw config for runtime update");
        return ESP_OK;
    }
    app_config_to_claw(config, claw_config);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to update running Claw config: %s", esp_err_to_name(err));
    }
    audio_capture_handle_t capture = NULL;
    if (asr_changed && audio_hub_get_capture(&capture) == ESP_OK && capture != NULL) {
        err = main_apply_asr_config(config);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to apply saved ASR config: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    if (wifi_changed && s_network_provisioning != NULL) {
        err = network_provisioning_service_reload_and_apply(
                  s_network_provisioning);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to apply saved Wi-Fi config: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void main_copy_claw_to_app_config(const app_claw_config_t *src, app_config_t *dst)
{
    strlcpy(dst->llm_api_key, src->llm_api_key, sizeof(dst->llm_api_key));
    strlcpy(dst->llm_backend_type, src->llm_backend_type, sizeof(dst->llm_backend_type));
    strlcpy(dst->llm_model, src->llm_model, sizeof(dst->llm_model));
    strlcpy(dst->llm_base_url, src->llm_base_url, sizeof(dst->llm_base_url));
    strlcpy(dst->llm_auth_type, src->llm_auth_type, sizeof(dst->llm_auth_type));
    strlcpy(dst->llm_timeout_ms, src->llm_timeout_ms, sizeof(dst->llm_timeout_ms));
    strlcpy(dst->llm_max_tokens, src->llm_max_tokens, sizeof(dst->llm_max_tokens));
    strlcpy(dst->llm_default_image_max_bytes,
            src->llm_default_image_max_bytes,
            sizeof(dst->llm_default_image_max_bytes));
    strlcpy(dst->llm_max_tokens_field, src->llm_max_tokens_field, sizeof(dst->llm_max_tokens_field));
    strlcpy(dst->llm_reasoning_effort, src->llm_reasoning_effort, sizeof(dst->llm_reasoning_effort));
    strlcpy(dst->llm_supports_tools, src->llm_supports_tools, sizeof(dst->llm_supports_tools));
    strlcpy(dst->llm_supports_vision, src->llm_supports_vision, sizeof(dst->llm_supports_vision));
    strlcpy(dst->llm_image_remote_url_only,
            src->llm_image_remote_url_only,
            sizeof(dst->llm_image_remote_url_only));
}

static esp_err_t main_save_claw_config(const app_claw_config_t *config, void *user_ctx)
{
    esp_err_t err;
    app_config_t *app_config = NULL;

    (void)user_ctx;
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");

    app_config = calloc(1, sizeof(*app_config));
    ESP_RETURN_ON_FALSE(app_config, ESP_ERR_NO_MEM, TAG, "Failed to allocate app config for Claw save");

    err = app_config_load(app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load config for Claw save: %s", esp_err_to_name(err));
        free(app_config);
        return err;
    }
    main_copy_claw_to_app_config(config, app_config);
    err = app_config_save(app_config);
    free(app_config);
    return err;
}

static esp_err_t main_get_wifi_status(http_server_wifi_status_t *status)
{
    ESP_RETURN_ON_FALSE(status, ESP_ERR_INVALID_ARG, TAG, "status is NULL");

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}

static void main_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t main_restart_device(void)
{
    BaseType_t ok = xTaskCreate(main_restart_task, "http_restart", 2048, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create restart task");
    return ESP_OK;
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t main_wechat_binding_persist(
    const wechat_binding_credentials_t *credentials,
    void *user_ctx)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(credentials != NULL &&
                        credentials->token[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG,
                        "WeChat credentials are invalid");

    app_config_t *config = calloc(1, sizeof(*config));
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate WeChat config");
    esp_err_t err = app_config_load(config);
    if (err == ESP_OK) {
        strlcpy(config->wechat_token,
                credentials->token,
                sizeof(config->wechat_token));
        strlcpy(config->wechat_base_url,
                credentials->base_url,
                sizeof(config->wechat_base_url));
        strlcpy(config->wechat_account_id,
                credentials->account_id,
                sizeof(config->wechat_account_id));
        err = main_save_config(config);
    }
    free(config);
    return err;
}

static esp_err_t main_wechat_login_start(
    const char *account_id, bool force)
{
    ESP_RETURN_ON_FALSE(s_wechat_binding != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "WeChat binding service is not ready");
    return wechat_binding_service_start(
               s_wechat_binding, account_id, force,
               WECHAT_BINDING_PERSIST_MANUAL);
}

static esp_err_t main_wechat_login_get_status(
    http_server_wechat_login_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "status is NULL");
    ESP_RETURN_ON_FALSE(s_wechat_binding != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "WeChat binding service is not ready");

    wechat_binding_session_t *session =
        calloc(1, sizeof(*session));
    ESP_RETURN_ON_FALSE(session != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate login status");
    esp_err_t err = wechat_binding_service_get_session(
                        s_wechat_binding, session);
    if (err == ESP_OK) {
        memset(status, 0, sizeof(*status));
        status->active = session->active;
        status->configured = session->configured;
        status->completed = session->completed;
        status->persisted = session->persisted;
        strlcpy(status->session_key, session->session_key,
                sizeof(status->session_key));
        strlcpy(status->status, session->status,
                sizeof(status->status));
        strlcpy(status->message, session->message,
                sizeof(status->message));
        strlcpy(status->qr_data_url, session->qr_data_url,
                sizeof(status->qr_data_url));
        strlcpy(status->account_id, session->account_id,
                sizeof(status->account_id));
        strlcpy(status->user_id, session->user_id,
                sizeof(status->user_id));
        strlcpy(status->token, session->token,
                sizeof(status->token));
        strlcpy(status->base_url, session->base_url,
                sizeof(status->base_url));
    }
    free(session);
    return err;
}

static esp_err_t main_wechat_login_cancel(void)
{
    ESP_RETURN_ON_FALSE(s_wechat_binding != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "WeChat binding service is not ready");
    return wechat_binding_service_cancel(s_wechat_binding);
}
#endif

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_timezone(const char *timezone)
{
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(timezone && timezone[0] != '\0', ESP_ERR_INVALID_ARG, tz_default, TAG,
                      "Timezone is empty.");
    ESP_GOTO_ON_FALSE(setenv("TZ", timezone, 1) == 0, ESP_FAIL, tz_default, TAG,
                      "Failed to set TZ env");
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s", timezone);
    return ESP_OK;

tz_default:
    assert(setenv("TZ", "CST-8", 1) == 0);
    tzset();
    ESP_LOGI(TAG, "Timezone set to default: CST-8");
    return ret;
}

#if APP_ENABLE_MEM_LOG

#include "esp_gmf_oal_sys.h"

void print_memory_info(const char *tag)
{
    // Get basic heap memory information
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    
    // Get memory region information
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_free_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    
    size_t total_spiram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_free_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    
    size_t total_dma = heap_caps_get_total_size(MALLOC_CAP_DMA);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    
    // FreeRTOS heap information
    size_t free_rtos_heap = xPortGetFreeHeapSize();
    size_t min_free_rtos_heap = xPortGetMinimumEverFreeHeapSize();
    
    // Calculate usage percentage
    size_t used_heap = total_internal - free_internal;
    float usage_percent = total_internal > 0 ? (float)used_heap * 100.0f / total_internal : 0.0f;
    
    // Print header
    ESP_LOGI(tag, "╔══════════════════════════════════════════════════════════════╗");
    ESP_LOGI(tag, "║              Memory Detail Report                            ║");
    ESP_LOGI(tag, "╠══════════════════════════════════════════════════════════════╣");
    
    // Overall heap memory information
    ESP_LOGI(tag, "║ Overall Heap Memory:                                         ║");
    ESP_LOGI(tag, "║   Current Free Heap:     %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)free_heap, free_heap / 1024.0f);
    ESP_LOGI(tag, "║   Min Free Ever:         %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)min_free_heap, min_free_heap / 1024.0f);
    ESP_LOGI(tag, "╠══════════════════════════════════════════════════════════════╣");
    
    // Internal memory region (SRAM)
    ESP_LOGI(tag, "║ Internal Memory (SRAM):                                      ║");
    ESP_LOGI(tag, "║   Total Capacity:        %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)total_internal, total_internal / 1024.0f);
    ESP_LOGI(tag, "║   Used:                  %8" PRIu32 " bytes (%6.1f KB) [%5.1f%%] ║", 
             (uint32_t)used_heap, used_heap / 1024.0f, usage_percent);
    ESP_LOGI(tag, "║   Free:                  %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)free_internal, free_internal / 1024.0f);
    ESP_LOGI(tag, "║   Largest Free Block:    %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)largest_free_internal, largest_free_internal / 1024.0f);
    
    // SPI RAM region (if exists)
    if (total_spiram > 0) {
        ESP_LOGI(tag, "╠══════════════════════════════════════════════════════════════╣");
        ESP_LOGI(tag, "║ SPI RAM Memory:                                              ║");
        ESP_LOGI(tag, "║   Total Capacity:        %8" PRIu32 " bytes (%6.1f KB)          ║", 
                 (uint32_t)total_spiram, total_spiram / 1024.0f);
        ESP_LOGI(tag, "║   Free:                  %8" PRIu32 " bytes (%6.1f KB)          ║", 
                 (uint32_t)free_spiram, free_spiram / 1024.0f);
        ESP_LOGI(tag, "║   Largest Free Block:    %8" PRIu32 " bytes (%6.1f KB)          ║", 
                 (uint32_t)largest_free_spiram, largest_free_spiram / 1024.0f);
    }
    
    // DMA memory region
    if (total_dma > 0) {
        ESP_LOGI(tag, "╠══════════════════════════════════════════════════════════════╣");
        ESP_LOGI(tag, "║ DMA Memory:                                                  ║");
        ESP_LOGI(tag, "║   Total Capacity:        %8" PRIu32 " bytes (%6.1f KB)          ║", 
                 (uint32_t)total_dma, total_dma / 1024.0f);
        ESP_LOGI(tag, "║   Free:                  %8" PRIu32 " bytes (%6.1f KB)          ║", 
                 (uint32_t)free_dma, free_dma / 1024.0f);
    }
    
    // FreeRTOS heap information
    ESP_LOGI(tag, "╠══════════════════════════════════════════════════════════════╣");
    ESP_LOGI(tag, "║ FreeRTOS Heap:                                               ║");
    ESP_LOGI(tag, "║   Current Free:          %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)free_rtos_heap, free_rtos_heap / 1024.0f);
    ESP_LOGI(tag, "║   Min Free Ever:         %8" PRIu32 " bytes (%6.1f KB)          ║", 
             (uint32_t)min_free_rtos_heap, min_free_rtos_heap / 1024.0f);
    
    // Memory health status
    ESP_LOGI(tag, "╠══════════════════════════════════════════════════════════════╣");
    if (free_heap < 10 * 1024) {
        ESP_LOGW(tag, "║ WARNING: Free memory below 10KB, possible memory leak!       ║");
    } else if (free_heap < 50 * 1024) {
        ESP_LOGW(tag, "║ CAUTION: Free memory below 50KB, monitor usage carefully     ║");
    } else {
        ESP_LOGI(tag, "║ Status: OK                                                   ║");
    }
    
    // Footer
    ESP_LOGI(tag, "╚══════════════════════════════════════════════════════════════╝");
}

static void memory_monitor_task(void *arg)
{
    (void)arg;
    #define PERF_REPORT_INTERVAL_MS 5000
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PERF_REPORT_INTERVAL_MS));
        
       esp_gmf_oal_sys_get_real_time_stats(PERF_REPORT_INTERVAL_MS, 0);
       print_memory_info(TAG);
        
        ESP_LOGI(TAG, "=====================================================\n");
    }
}
#endif

static void main_restore_display_rotation(void)
{
    esp_err_t err = app_settings_service_restore_display(s_app_settings);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saved display rotation was not restored: %s",
                 esp_err_to_name(err));
    }
}

#define MOSAICO_BOOT_SPLASH_BRIGHTNESS_PERCENT 40U
#define MOSAICO_BOOT_BRIGHTNESS_FADE_MS        240U

static void main_prepare_display_brightness_fade(void)
{
    esp_err_t err = app_settings_service_prepare_brightness_fade(
        s_app_settings, MOSAICO_BOOT_SPLASH_BRIGHTNESS_PERCENT,
        MOSAICO_BOOT_BRIGHTNESS_FADE_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Startup display brightness fade was not prepared: %s",
                 esp_err_to_name(err));
    }
}

void app_main(void)
{
#if defined(ESP_MOSAICO_REMOTE_DEBUG) && APP_ENABLE_CLAW_TASKS
    (void)esp_remote_mosaic_adapter_start();
#endif
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("http_reuse", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Starting app");
    ESP_LOGI(TAG, "ESP-Claw version: %s", claw_get_version());
    ESP_LOGI(TAG, "ESP-Claw git version: %s", claw_get_git_version());
    ESP_LOGI(TAG, "Edge Agent version: %s", edge_agent_get_version());
    ESP_ERROR_CHECK(app_allocate_runtime_state());
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(app_config_init());
    bool factory_reset_pending = false;
    ESP_ERROR_CHECK(app_factory_reset_is_pending(&factory_reset_pending));
    if (factory_reset_pending) {
        /* Retry the settings erase in case power was lost after the durable
         * reset request but before the UI-side erase completed. */
        ESP_ERROR_CHECK(app_config_reset_all());
    }
    ESP_ERROR_CHECK(app_config_load(s_config));
    app_config_to_claw(s_config, s_claw_config);
    init_timezone(app_config_get_timezone(s_config)); // no need to check error
    ESP_ERROR_CHECK(mosaico_board_variant_prepare());
    ESP_ERROR_CHECK(trial_auth_validate_hmac_efuse_key());
    ESP_ERROR_CHECK(trial_auth_init());
    ESP_ERROR_CHECK(esp_board_manager_init());
    /* Audio mixer/capture acquire their board devices through the hardware
     * registry. Initialize it before either service attempts a claim. */
    ESP_ERROR_CHECK(claw_hw_registry_init());
    ESP_ERROR_CHECK(hot_plug_register_init());
    ESP_ERROR_CHECK(app_fs_init());

    if (factory_reset_pending) {
        ESP_LOGW(TAG, "Completing pending factory data reset");
        /* No data-consuming service has started yet, so files cannot be
         * recreated while the reset is in progress. Keep the pending flag
         * until every operation succeeds so an interrupted reset is retried. */
        ESP_ERROR_CHECK(app_fs_factory_reset());
        ESP_ERROR_CHECK(weather_service_erase_cache());
        ESP_ERROR_CHECK(app_factory_reset_complete());
        ESP_LOGW(TAG, "Factory data reset complete");
    }

    /* Publish the resolved storage roots so any component can compose paths
     * without knowing whether data lives on flash or an SD card. */
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_DATA, app_fs_storage_base_path()));
    ESP_ERROR_CHECK(claw_paths_set(CLAW_PATH_SYSTEM, app_fs_system_base_path()));
    ESP_ERROR_CHECK(claw_paths_set_space_provider(CLAW_PATH_DATA,
                                                  app_fs_get_storage_space,
                                                  NULL));

    if (APP_ENABLE_CLAW_TASKS) {
        ESP_ERROR_CHECK(wifi_manager_init());
    }
    ESP_ERROR_CHECK(app_settings_service_create(&s_app_settings));

    ESP_ERROR_CHECK(weather_service_init(&(weather_service_config_t) {
        .user_agent = APP_WEATHER_USER_AGENT,
        .refresh_interval_ms = 60U * 60U * 1000U,
        .stale_after_ms = 6U * 60U * 60U * 1000U,
    }));
    ESP_ERROR_CHECK(mosaic_net_weather_init());
    ESP_ERROR_CHECK(update_check_service_init(
        &(update_check_service_config_t) {
            .manifest_url = CONFIG_APP_UPDATE_MANIFEST_URL,
            .product = "esp-mosaico",
            .current_version = edge_agent_get_version(),
            .user_agent = APP_UPDATE_USER_AGENT,
            .timeout_ms = 10000U,
            .max_body_size = 4096U,
        }));
    if (APP_ENABLE_CLAW_TASKS) {
        ESP_ERROR_CHECK(network_provisioning_service_create(
            &s_network_provisioning));
        ESP_ERROR_CHECK(network_provisioning_service_register_cb(
            s_network_provisioning,
            on_network_provisioning_changed,
            NULL));
        ESP_ERROR_CHECK(wechat_binding_service_create(
            &(wechat_binding_service_config_t) {
                .persist = main_wechat_binding_persist,
                .initially_configured =
                    s_config->wechat_token[0] != '\0' &&
                    s_config->wechat_base_url[0] != '\0',
                .initial_account_id = s_config->wechat_account_id,
            },
            &s_wechat_binding));
        ESP_ERROR_CHECK(mosaic_setup_configure_wechat(
            &(mosaic_setup_wechat_ops_t) {
                .start = device_setup_start_wechat,
                .cancel = device_setup_cancel_wechat,
            }));
        ESP_ERROR_CHECK(wechat_binding_service_register_cb(
            s_wechat_binding,
            on_device_setup_wechat,
            NULL));
        wechat_binding_status_t setup_wechat = {0};
        ESP_ERROR_CHECK(wechat_binding_service_get_status(
            s_wechat_binding, &setup_wechat));
        on_device_setup_wechat(s_wechat_binding, &setup_wechat, NULL);
    }
    ESP_ERROR_CHECK(mosaic_device_ops_init(
        &(mosaic_device_ops_config_t) {
            .settings = s_app_settings,
            .network_provisioning = s_network_provisioning,
            .save_config = main_save_config,
        }));

    /* Cache display settings before presenter/GSP startup. The presenter
     * applies the target brightness once, before rendering can use the LCD
     * SPI bus. */
    main_restore_display_rotation();
    main_prepare_display_brightness_fade();

    if (!APP_ENABLE_CLAW_TASKS) {
        ESP_LOGW(TAG,
                 "Claw background services disabled: Wi-Fi, network, HTTP, "
                 "bindings, audio/ASR, capabilities, and agent not started");
        ESP_ERROR_CHECK(mosaic_ui_start());
        esp_err_t battery_monitor_err =
            mosaic_device_ops_start_battery_monitor();
        if (battery_monitor_err != ESP_OK) {
            ESP_LOGW(TAG, "battery monitor unavailable: %s",
                     esp_err_to_name(battery_monitor_err));
        }
        esp_err_t button_err = mosaic_button_platform_init();
        if (button_err != ESP_OK) {
            ESP_LOGW(TAG, "Mosaic buttons unavailable: %s",
                     esp_err_to_name(button_err));
        }
        app_free_runtime_state();
        return;
    }

    ESP_ERROR_CHECK(http_server_init(&(http_server_config_t) {
        .storage_base_path = app_fs_storage_base_path(),
        .services = {
            .load_config = main_load_config,
            .save_config = main_save_config,
            .get_wifi_status = main_get_wifi_status,
            .restart_device = main_restart_device,
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
            .wechat_login_start = main_wechat_login_start,
            .wechat_login_get_status = main_wechat_login_get_status,
            .wechat_login_cancel = main_wechat_login_cancel,
#endif
        },
    }));

    log_wifi_startup_config(s_config);

    bool wifi_enabled = true;
    esp_err_t wifi_enabled_err = app_settings_service_get_wifi_enabled(
        s_app_settings, &wifi_enabled);
    if (wifi_enabled_err != ESP_OK) {
        ESP_LOGW(TAG, "Saved Wi-Fi state unavailable, defaulting on: %s",
                 esp_err_to_name(wifi_enabled_err));
        wifi_enabled = true;
    }

    esp_err_t wifi_err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = s_config->wifi_ssid,
        .sta_password = s_config->wifi_password,
        .ap_ssid = s_config->ap_ssid[0] ? s_config->ap_ssid : NULL,
        .ap_password = s_config->ap_password[0] ? s_config->ap_password : NULL,
        .ap_behavior = s_config->ap_behavior,
        .start_disabled = !wifi_enabled,
    });
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(wifi_err));
    } else {
        ESP_ERROR_CHECK(network_provisioning_service_start(
            s_network_provisioning));
        if (captive_dns_start(&(captive_dns_config_t) {
                .ap_netif = wifi_manager_get_ap_netif(),
                .configure_dhcp_dns = true,
            }) != ESP_OK) {
            ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
        }

        wifi_manager_status_t status = {0};
        wifi_manager_get_status(&status);
        if (status.ap_active) {
            const char *portal_auth = s_config->ap_password[0] ? "wpa2" : "open";
            ESP_LOGW(TAG,
                     "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
                     status.ap_ssid,
                     portal_auth,
                     status.ap_ip,
                     status.ap_ip);
        }
    }

    ESP_ERROR_CHECK(app_capabilities_register_external_group(
        &(app_capability_external_group_t) {
            .group_id = "cap_im_ai_create",
            .display_name = "Mosaico AI Create",
            .llm_visible_by_default = false,
            .reg = main_register_ai_create_capability,
        }));
    ESP_ERROR_CHECK(app_cap_system_platform_init(s_app_settings));
    ESP_ERROR_CHECK(app_claw_set_save_config_callback(main_save_claw_config, NULL));
    ESP_ERROR_CHECK(app_claw_set_network_ready_callback(main_network_ready, NULL));
    ESP_ERROR_CHECK(app_claw_start(s_claw_config));
    ESP_ERROR_CHECK(weather_service_start());
    ESP_ERROR_CHECK(main_apply_asr_config(s_config));
    esp_err_t restore_err = app_settings_service_restore_audio(s_app_settings);
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "Saved audio volume was not restored: %s",
                 esp_err_to_name(restore_err));
    }
    if (wifi_err == ESP_OK) {
        ESP_ERROR_CHECK(http_server_start());
    }
#if CONFIG_APP_CLAW_CAP_IM_LOCAL
    ESP_ERROR_CHECK(http_server_webim_bind_im());
#endif

    ESP_ERROR_CHECK(mosaic_ui_start());
    esp_err_t battery_monitor_err =
        mosaic_device_ops_start_battery_monitor();
    if (battery_monitor_err != ESP_OK) {
        ESP_LOGW(TAG, "battery monitor unavailable: %s",
                 esp_err_to_name(battery_monitor_err));
    }
    esp_err_t button_err = mosaic_button_platform_init();
    if (button_err != ESP_OK) {
        ESP_LOGW(TAG, "Mosaic buttons unavailable: %s",
                     esp_err_to_name(button_err));
    }

    if (wifi_err == ESP_OK && s_config->wifi_ssid[0] != '\0') {
        esp_err_t wait_err = wifi_manager_wait_connected(30000);
        if (wait_err == ESP_OK) {
            wifi_manager_status_t status = {0};
            wifi_manager_get_status(&status);
            ESP_LOGI(TAG, "Wi-Fi STA ready: %s", status.sta_ip);
        } else if (wait_err == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Wi-Fi is reconnecting in the background");
        } else {
            ESP_LOGW(TAG, "Wi-Fi STA wait returned error: %s",
                     esp_err_to_name(wait_err));
        }
    }

    register_wifi_command();

#if APP_ENABLE_MEM_LOG
    xTaskCreate(memory_monitor_task, "mem_mon", 4096, NULL, 1, NULL);
#endif

    app_free_runtime_state();
}
