/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Recording storage layout under <DATA>/air_voice/:
 *   <name>.wav       processed track      <name>_raw.wav  microphone track
 *   <name>.json      settings + statistics captured with the recording
 *   <name>.part / <name>_raw.part  while recording (recovered on boot)
 *   config.json      persisted service settings
 * WAV headers use the streaming form (sizes 0xFFFFFFFF), see av_wav_begin().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "air_voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AIR_VOICE_PATH_MAX   256U
#define AIR_VOICE_WAV_HEADER 44U

typedef enum {
    AV_FILE_PROCESSED = 0,
    AV_FILE_RAW,
    AV_FILE_META,
    AV_FILE_PART,
    AV_FILE_RAW_PART,
} av_file_kind_t;

esp_err_t av_store_init(void);
const char *av_store_dir(void);
esp_err_t av_store_config_path(char *out, size_t out_size);
esp_err_t av_store_path(const char *name, av_file_kind_t kind, char *out, size_t out_size);
bool av_store_exists(const char *path);
/** Create a fresh unique base name from the wall clock (or uptime). */
esp_err_t av_store_new_name(char *out, size_t out_size);

/** Write a streaming WAV header (sizes unknown, 0xFFFFFFFF). */
esp_err_t av_wav_begin(FILE *file);
/** Fill the real sizes into a header written by av_wav_begin() (in memory,
 * for serving); other layouts are left untouched. */
void av_wav_patch_header(uint8_t header[AIR_VOICE_WAV_HEADER], uint64_t file_bytes);

/** Finish any recording interrupted by a reset (".part" files). */
void av_store_recover_partials(void);

size_t av_store_list(air_voice_recording_t *out, size_t max, size_t *out_total);
esp_err_t av_store_delete(const char *name);
bool av_store_newest(char *out_name, size_t out_size, uint32_t *out_duration_ms);
uint32_t av_store_duration_ms(uint64_t file_bytes);
void av_store_space(uint64_t *out_free, uint64_t *out_total);

#ifdef __cplusplus
}
#endif
