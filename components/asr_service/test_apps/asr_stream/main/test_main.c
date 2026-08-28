/*
 * Smoke tests for Qwen-backed ASR streaming. The app boots into the Unity
 * console menu so the Qwen stream case only runs when selected.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "asr_provider.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"
#include "unity.h"
#include "unity_test_runner.h"

static const char *TAG = "test_asr";

#define ASR_TEST_FAKE_API_KEY      "fake-qwen-asr-api-key"
#define ASR_TEST_DEFAULT_ENDPOINT  "wss://dashscope.aliyuncs.com/api-ws/v1/inference"
#define ASR_TEST_DEFAULT_MODEL     "fun-asr-realtime"
#define ASR_TEST_LANGUAGE_HINT     "zh"
#define ASR_TEST_RESULT_BUF_SIZE   512
#define ASR_TEST_PCM_FRAME_BYTES   ((ASR_PROVIDER_SAMPLE_RATE * 20 / 1000) * ASR_PROVIDER_CHANNELS * (ASR_PROVIDER_BITS / 8))

extern const uint8_t asr_test_audio_pcm_start[] asm("_binary_asr_test_audio_pcm_start");
extern const uint8_t asr_test_audio_pcm_end[] asm("_binary_asr_test_audio_pcm_end");

typedef struct {
    uint32_t partial;
    uint32_t final;
} asr_test_event_counts_t;

static bool asr_test_has_qwen_key(void)
{
    return CONFIG_ASR_TEST_QWEN_API_KEY[0] != '\0';
}

static esp_err_t asr_test_init_network(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init requires erase: %s", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(example_connect(), TAG, "example_connect failed");
    return ESP_OK;
}

static void asr_test_provider_result_cb(asr_provider_handle_t handle, asr_provider_result_type_t type, const char *text, void *user_ctx)
{
    (void)handle;
    asr_test_event_counts_t *counts = (asr_test_event_counts_t *)user_ctx;
    if (!counts || !text) {
        return;
    }
    if (type == ASR_PROVIDER_RESULT_FINAL) {
        counts->final++;
        ESP_LOGI(TAG, "Qwen PCM final: %s", text);
    } else {
        counts->partial++;
        ESP_LOGI(TAG, "Qwen PCM partial: %s", text);
    }
}

TEST_CASE("asr provider rejects invalid arguments", "[asr][provider]")
{
    const asr_provider_ops_t *ops = asr_provider_qwen_ops();
    asr_provider_handle_t provider = NULL;
    asr_provider_config_t cfg = {
        .api_key = ASR_TEST_FAKE_API_KEY,
        .endpoint = ASR_TEST_DEFAULT_ENDPOINT,
        .model = ASR_TEST_DEFAULT_MODEL,
        .language_hint = ASR_TEST_LANGUAGE_HINT,
        .connect_timeout_ms = 30000,
        .send_timeout_ms = 3000,
    };

    TEST_ASSERT_NOT_NULL(ops);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->create(NULL, &provider));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->create(&cfg, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->connect(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->start_stream(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->send_audio(NULL, (const uint8_t *)"a", 1));
    TEST_ASSERT_FALSE(ops->has_final_result(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->finish_stream(NULL));
    char text[2] = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->get_final_text(NULL, text, sizeof(text)));

    cfg.api_key = "";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->create(&cfg, &provider));

    cfg.api_key = ASR_TEST_FAKE_API_KEY;
    cfg.endpoint = "";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->create(&cfg, &provider));

    cfg.endpoint = ASR_TEST_DEFAULT_ENDPOINT;
    cfg.model = "";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ops->create(&cfg, &provider));
}

static esp_err_t send_embedded_pcm_to_qwen(asr_provider_handle_t provider, const asr_provider_ops_t *ops)
{
    const uint8_t *pcm = asr_test_audio_pcm_start;
    size_t pcm_len = (size_t)(asr_test_audio_pcm_end - asr_test_audio_pcm_start);
    if (pcm_len == 0) {
        ESP_LOGE(TAG, "embedded PCM file is empty");
        return ESP_ERR_INVALID_SIZE;
    }
    if (pcm_len % (ASR_PROVIDER_CHANNELS * (ASR_PROVIDER_BITS / 8)) != 0) {
        ESP_LOGW(TAG, "embedded PCM byte length is not sample aligned: %u", (unsigned)pcm_len);
    }

    size_t offset = 0;
    while (offset < pcm_len) {
        size_t remain = pcm_len - offset;
        size_t chunk = remain > ASR_TEST_PCM_FRAME_BYTES ? ASR_TEST_PCM_FRAME_BYTES : remain;
        esp_err_t err = ops->send_audio(provider, pcm + offset, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PCM audio send failed at %u/%u: %s", (unsigned)offset, (unsigned)pcm_len, esp_err_to_name(err));
            return err;
        }
        offset += chunk;
        if (CONFIG_ASR_TEST_PCM_CHUNK_DELAY_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ASR_TEST_PCM_CHUNK_DELAY_MS));
        }
    }
    return ESP_OK;
}

TEST_CASE("asr qwen streams embedded mandarin pcm", "[asr][qwen][pcm]")
{
    if (!asr_test_has_qwen_key()) {
        TEST_IGNORE_MESSAGE("CONFIG_ASR_TEST_QWEN_API_KEY is empty");
    }
    const asr_provider_ops_t *ops = asr_provider_qwen_ops();
    if (!ops) {
        TEST_FAIL_MESSAGE("Qwen provider ops unavailable");
    }

    TEST_ESP_OK(asr_test_init_network());

    asr_test_event_counts_t counts = {0};
    asr_provider_handle_t provider = NULL;
    asr_provider_config_t provider_cfg = {
        .api_key = CONFIG_ASR_TEST_QWEN_API_KEY,
        .workspace_id = NULL,
        .endpoint = ASR_TEST_DEFAULT_ENDPOINT,
        .model = ASR_TEST_DEFAULT_MODEL,
        .language_hint = ASR_TEST_LANGUAGE_HINT,
        .connect_timeout_ms = 30000,
        .send_timeout_ms = 3000,
        .result_cb = asr_test_provider_result_cb,
        .result_user_ctx = &counts,
    };
    char text[ASR_TEST_RESULT_BUF_SIZE] = {0};

    esp_err_t err = ops->create(&provider_cfg, &provider);
    if (err == ESP_OK) err = ops->connect(provider);
    if (err == ESP_OK) err = ops->start_stream(provider);
    if (err == ESP_OK) err = send_embedded_pcm_to_qwen(provider, ops);
    if (err == ESP_OK) err = ops->finish_stream(provider);
    ESP_LOGI(TAG, "Qwen PCM stream returned %s", esp_err_to_name(err));

    esp_err_t text_err = ESP_ERR_INVALID_STATE;
    if (provider) {
        text_err = ops->get_final_text(provider, text, sizeof(text));
        ESP_LOGI(TAG, "Qwen PCM final text returned %s, final=\"%s\"", esp_err_to_name(text_err), text);
        ops->delete(provider);
    }
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE);
    TEST_ASSERT_TRUE(text_err == ESP_OK || text_err == ESP_ERR_NOT_FOUND || text_err == ESP_ERR_INVALID_SIZE);
}

void app_main(void)
{
    unity_run_menu();
}
