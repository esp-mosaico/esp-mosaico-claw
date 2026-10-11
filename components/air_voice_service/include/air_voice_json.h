/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* JSON views of the Air Voice service types, shared by settings persistence
 * and the HTTP API so field names are defined in exactly one place. */
#pragma once

#include "cJSON.h"
#include "air_voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Add every configuration field to @p obj. */
void air_voice_config_to_json(const air_voice_config_t *config, cJSON *obj);
/** Overwrite the fields present in @p obj (partial update); others are kept.
 * Values are clamped to their valid ranges afterwards. */
void air_voice_config_from_json(air_voice_config_t *config, const cJSON *obj);
void air_voice_status_to_json(const air_voice_status_t *status, cJSON *obj);
void air_voice_recording_to_json(const air_voice_recording_t *rec, cJSON *obj);

#ifdef __cplusplus
}
#endif
