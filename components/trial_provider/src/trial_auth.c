/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "trial_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "llm/claw_llm_auth.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"
#include "psa_crypto_driver_esp_hmac_opaque.h"
#include "psa_crypto_driver_esp_hmac_opaque_contexts.h"

static const char *TAG = "trial_auth";

#define TRIAL_TOKEN_MAX_LEN 2048
#define TRIAL_HTTP_TIMEOUT_MS 15000
#define TRIAL_TOKEN_REFRESH_MARGIN_MS (60LL * 1000LL)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buffer_t;

typedef struct {
    char token[TRIAL_TOKEN_MAX_LEN];
    int64_t expires_at_ms;
} token_cache_t;

static SemaphoreHandle_t s_lock;
static token_cache_t s_llm_token_cache;
static token_cache_t s_asr_token_cache;
static trial_auth_hmac_provider_fn s_hmac_provider;

static esp_err_t trial_auth_resolver(const char *auth_type, bool force_refresh,
                                     char **out_token, void *user_ctx);

const char *trial_auth_get_firmware_version(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    return description && description->version[0] ? description->version : "unknown";
}

static bool response_append(response_buffer_t *buffer, const char *data, size_t len)
{
    if (!buffer || !data || len == 0) {
        return true;
    }
    if (buffer->len + len + 1 > buffer->cap) {
        size_t next = buffer->cap ? buffer->cap : 1024;
        while (next < buffer->len + len + 1) {
            next *= 2;
        }
        char *grown = realloc(buffer->data, next);
        if (!grown) {
            return false;
        }
        buffer->data = grown;
        buffer->cap = next;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return true;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = event ? event->user_data : NULL;
    if (!buffer || event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    return response_append(buffer, event->data, (size_t)event->data_len)
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

static esp_err_t post_json(const char *url, const char *body,
                           response_buffer_t *response, int *status_code)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = response,
        .timeout_ms = TRIAL_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, TRIAL_AUTH_FIRMWARE_VERSION_HEADER,
                               trial_auth_get_firmware_version());
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK && status_code) {
        *status_code = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t get_device_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    if (!out || out_size < 18 || esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    int written = snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t default_hmac_provider(uint32_t efuse_key_id, const void *message,
                                       size_t message_len, uint8_t out[32])
{
    if (!message || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_ESP_HMAC_VOLATILE);

    esp_hmac_opaque_key_t opaque_key = {
        .efuse_key_id = (hmac_key_id_t)efuse_key_id,
    };
    psa_key_id_t psa_key_id = 0;
    psa_status_t status = psa_import_key(&attributes, (const uint8_t *)&opaque_key,
                                         sizeof(opaque_key), &psa_key_id);
    if (status != PSA_SUCCESS) {
        psa_reset_key_attributes(&attributes);
        return ESP_ERR_INVALID_STATE;
    }

    size_t output_len = 0;
    status = psa_mac_compute(psa_key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                             (const uint8_t *)message, message_len,
                             out, 32, &output_len);
    psa_destroy_key(psa_key_id);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS && output_len == 32 ? ESP_OK : ESP_FAIL;
}

static esp_err_t hmac_message(const char *message, size_t message_len, uint8_t out[32])
{
    trial_auth_hmac_provider_fn provider = s_hmac_provider
                                              ? s_hmac_provider
                                              : default_hmac_provider;
    return provider(CONFIG_TRIAL_DEVICE_HMAC_KEY_ID, message, message_len, out);
}

static size_t base64url_encode(const uint8_t *input, size_t input_len,
                               char *output, size_t output_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t required = (input_len * 4 + 2) / 3;
    if (!input || !output || output_size <= required) {
        return 0;
    }
    size_t in = 0;
    size_t out = 0;
    while (in < input_len) {
        uint32_t value = (uint32_t)input[in++] << 16;
        if (in < input_len) value |= (uint32_t)input[in++] << 8;
        if (in < input_len) value |= input[in++];
        output[out++] = alphabet[(value >> 18) & 0x3f];
        output[out++] = alphabet[(value >> 12) & 0x3f];
        if (out < required) output[out++] = alphabet[(value >> 6) & 0x3f];
        if (out < required) output[out++] = alphabet[value & 0x3f];
    }
    output[out] = '\0';
    return out;
}

static esp_err_t refresh_token_locked(const char *base_url, token_cache_t *token_cache)
{
    if (!base_url || !token_cache) {
        return ESP_ERR_INVALID_ARG;
    }

    char device_id[18] = {0};
    esp_err_t err = get_device_id(device_id, sizeof(device_id));
    if (err != ESP_OK) return err;

    cJSON *challenge_request = cJSON_CreateObject();
    if (!challenge_request || !cJSON_AddStringToObject(challenge_request, "device_id", device_id)) {
        cJSON_Delete(challenge_request);
        return ESP_ERR_NO_MEM;
    }
    char *challenge_body = cJSON_PrintUnformatted(challenge_request);
    cJSON_Delete(challenge_request);
    if (!challenge_body) return ESP_ERR_NO_MEM;

    response_buffer_t challenge_response = {0};
    int status = 0;
    char challenge_url[128];
    snprintf(challenge_url, sizeof(challenge_url), "%s/v1/device/challenge",
             base_url);
    err = post_json(challenge_url, challenge_body, &challenge_response, &status);
    cJSON_free(challenge_body);
    if (err != ESP_OK || status != 200 || !challenge_response.data) {
        ESP_LOGW(TAG, "Trial challenge request failed (status=%d)", status);
        free(challenge_response.data);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    cJSON *challenge_root = cJSON_Parse(challenge_response.data);
    const char *challenge_id = challenge_root
        ? cJSON_GetStringValue(cJSON_GetObjectItem(challenge_root, "challenge_id")) : NULL;
    const char *nonce = challenge_root
        ? cJSON_GetStringValue(cJSON_GetObjectItem(challenge_root, "nonce")) : NULL;
    if (!challenge_id || !nonce) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return ESP_FAIL;
    }

    size_t message_len = strlen(challenge_id) + strlen(nonce) + strlen(device_id) + 7;
    char *message = malloc(message_len + 1);
    uint8_t digest[32] = {0};
    char signature[48] = {0};
    if (!message) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return ESP_ERR_NO_MEM;
    }
    snprintf(message, message_len + 1, "v1\n%s\n%s\n%s",
             challenge_id, nonce, device_id);
    err = hmac_message(message, strlen(message), digest);
    free(message);
    if (err != ESP_OK || base64url_encode(digest, sizeof(digest), signature, sizeof(signature)) == 0) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    cJSON *token_request = cJSON_CreateObject();
    bool token_request_ok = token_request &&
        cJSON_AddStringToObject(token_request, "device_id", device_id) &&
        cJSON_AddStringToObject(token_request, "challenge_id", challenge_id) &&
        cJSON_AddStringToObject(token_request, "signature", signature);
    char *token_body = token_request_ok ? cJSON_PrintUnformatted(token_request) : NULL;
    cJSON_Delete(token_request);
    if (!token_body) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return ESP_ERR_NO_MEM;
    }

    response_buffer_t token_response = {0};
    char token_url[128];
    snprintf(token_url, sizeof(token_url), "%s/v1/device/token",
             base_url);
    status = 0;
    err = post_json(token_url, token_body, &token_response, &status);
    cJSON_free(token_body);
    cJSON_Delete(challenge_root);
    free(challenge_response.data);
    if (err != ESP_OK || status != 200 || !token_response.data) {
        free(token_response.data);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    cJSON *token_root = cJSON_Parse(token_response.data);
    const char *access_token = token_root
        ? cJSON_GetStringValue(cJSON_GetObjectItem(token_root, "access_token")) : NULL;
    cJSON *expires_item = token_root ? cJSON_GetObjectItem(token_root, "expires_in") : NULL;
    int expires_in = cJSON_IsNumber(expires_item) ? (int)expires_item->valuedouble : 0;
    if (!access_token || !access_token[0] ||
        strlen(access_token) >= sizeof(token_cache->token) || expires_in <= 0) {
        cJSON_Delete(token_root);
        free(token_response.data);
        return ESP_FAIL;
    }
    strlcpy(token_cache->token, access_token, sizeof(token_cache->token));
    token_cache->expires_at_ms = esp_timer_get_time() / 1000 +
                                 (int64_t)expires_in * 1000;
    cJSON_Delete(token_root);
    free(token_response.data);
    return ESP_OK;
}

esp_err_t trial_auth_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    return claw_llm_register_auth_resolver("trial", trial_auth_resolver, NULL);
}

esp_err_t trial_auth_validate_hmac_efuse_key(void)
{
#if !defined(CONFIG_TRIAL_DEVICE_HMAC_KEY_ID)
    ESP_LOGI(TAG, "eFuse HMAC key check skipped: key ID is not configured");
    return ESP_OK;
#elif CONFIG_TRIAL_DEVICE_HMAC_KEY_ID == -1
    ESP_LOGI(TAG, "eFuse HMAC key check skipped: key ID is disabled");
    return ESP_OK;
#else
    const int configured_key_id = CONFIG_TRIAL_DEVICE_HMAC_KEY_ID;
    if (configured_key_id < 0 || configured_key_id >= HMAC_KEY_MAX) {
        ESP_LOGE(TAG, "configured eFuse HMAC key ID is out of range: %d",
                 configured_key_id);
        return ESP_ERR_INVALID_ARG;
    }

    const hmac_key_id_t key_id = (hmac_key_id_t)configured_key_id;
    const esp_efuse_block_t key_block =
        (esp_efuse_block_t)(EFUSE_BLK_KEY0 + configured_key_id);
    const esp_efuse_purpose_t purpose = esp_efuse_get_key_purpose(key_block);
    if (purpose != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        ESP_LOGE(TAG,
                 "eFuse key block %d for HMAC key ID %d has purpose %d, expected HMAC_UP",
                 key_block, configured_key_id, purpose);
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_efuse_get_key_dis_read(key_block) &&
        esp_efuse_block_is_empty(key_block)) {
        ESP_LOGE(TAG, "eFuse key block %d for HMAC key ID %d is empty",
                 key_block, configured_key_id);
        return ESP_ERR_NOT_FOUND;
    }

    static const uint8_t probe[] = "trial-auth-hmac-efuse-check";
    uint8_t digest[32];
    ESP_RETURN_ON_ERROR(esp_hmac_calculate(key_id, probe, sizeof(probe) - 1U,
                                            digest),
                        TAG, "HMAC calculation using eFuse key ID %d failed",
                        configured_key_id);
    ESP_LOGI(TAG, "eFuse HMAC key ID %d in block %d validated",
             configured_key_id, key_block);
    return ESP_OK;
#endif
}

esp_err_t trial_auth_set_hmac_provider(trial_auth_hmac_provider_fn provider)
{
    if (!provider) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hmac_provider = provider;
    return ESP_OK;
}

static esp_err_t get_token(const char *base_url, token_cache_t *token_cache,
                           bool force_refresh, char **out_token)
{
    if (!base_url || !token_cache || !out_token) return ESP_ERR_INVALID_ARG;
    *out_token = NULL;
    if (!s_lock) {
        esp_err_t err = trial_auth_init();
        if (err != ESP_OK) return err;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time() / 1000;
    if (force_refresh || !token_cache->token[0] ||
        now + TRIAL_TOKEN_REFRESH_MARGIN_MS >= token_cache->expires_at_ms) {
        esp_err_t err = refresh_token_locked(base_url, token_cache);
        if (err != ESP_OK) {
            xSemaphoreGive(s_lock);
            return err;
        }
    }
    *out_token = strdup(token_cache->token);
    xSemaphoreGive(s_lock);
    return *out_token ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t trial_auth_get_llm_token(bool force_refresh, char **out_token)
{
    return get_token(TRIAL_AUTH_DEFAULT_LLM_BASE_URL, &s_llm_token_cache,
                     force_refresh, out_token);
}

esp_err_t trial_auth_get_asr_token(bool force_refresh, char **out_token)
{
    return get_token(TRIAL_AUTH_DEFAULT_ASR_BASE_URL, &s_asr_token_cache,
                     force_refresh, out_token);
}

static esp_err_t trial_auth_resolver(const char *auth_type, bool force_refresh,
                                     char **out_token, void *user_ctx)
{
    (void)auth_type;
    (void)user_ctx;
    return trial_auth_get_llm_token(force_refresh, out_token);
}
