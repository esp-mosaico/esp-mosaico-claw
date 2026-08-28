/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRIAL_AUTH_DEFAULT_LLM_BASE_URL "https://as.esp-claw.com"
#define TRIAL_AUTH_DEFAULT_ASR_BASE_URL "https://as1.esp-claw.com"
#define TRIAL_AUTH_DEFAULT_ASR_ENDPOINT "wss://as1.esp-claw.com/v1/asr/realtime"
#define TRIAL_AUTH_FIRMWARE_VERSION_HEADER "X-Firmware-Version"

typedef esp_err_t (*trial_auth_hmac_provider_fn)(uint32_t key_id,
                                                 const void *message,
                                                 size_t message_len,
                                                 uint8_t out[32]);

esp_err_t trial_auth_init(void);
/**
 * @brief Validate the configured eFuse HMAC key for Trial authentication.
 *
 * The validation is skipped when CONFIG_TRIAL_DEVICE_HMAC_KEY_ID is undefined
 * or set to -1. Otherwise it verifies that the configured key block is
 * programmed for HMAC upstream use and can calculate an HMAC.
 */
esp_err_t trial_auth_validate_hmac_efuse_key(void);
esp_err_t trial_auth_set_hmac_provider(trial_auth_hmac_provider_fn provider);

/** @brief Return the running firmware version used in Trial request headers. */
const char *trial_auth_get_firmware_version(void);

/**
 * @brief Get the trial access token for LLM requests.
 *
 * The LLM token is issued by TRIAL_AUTH_DEFAULT_LLM_BASE_URL and is cached
 * independently from the ASR token.
 */
esp_err_t trial_auth_get_llm_token(bool force_refresh, char **out_token);

/**
 * @brief Get the trial access token for ASR requests.
 *
 * The ASR token is issued by TRIAL_AUTH_DEFAULT_ASR_BASE_URL and is cached
 * independently from the LLM token.
 */
esp_err_t trial_auth_get_asr_token(bool force_refresh, char **out_token);

#ifdef __cplusplus
}
#endif
