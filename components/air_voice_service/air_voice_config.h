/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "air_voice_dsp.h"
#include "air_voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIR_VOICE_DEFAULT_PRESET "whisper_near"

void air_voice_config_defaults(air_voice_config_t *config);
/** Clamp and quantize every field to the range the hardware accepts. */
void air_voice_config_sanitize(air_voice_config_t *config);
/** Overwrite @p config with a preset. Returns ESP_ERR_NOT_FOUND if unknown. */
esp_err_t air_voice_config_preset(const char *name, air_voice_config_t *config);
void air_voice_config_to_dsp(const air_voice_config_t *config, air_voice_dsp_config_t *out);

esp_err_t air_voice_config_load(const char *path, air_voice_config_t *config);
esp_err_t air_voice_config_save(const char *path, const air_voice_config_t *config);

#ifdef __cplusplus
}
#endif
