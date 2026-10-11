/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
/* Bounded in-memory log for the Air Voice web page. Service messages are
 * written directly; optionally the system log is tee'd so warnings, errors
 * and audio-stack messages (capture, mixer, player, codec) show up too. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "air_voice_service.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t air_voice_log_init(void);
void air_voice_log_capture_system(bool enable);
uint32_t air_voice_log_latest_seq(void);
void air_voice_logf(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define AV_LOGE(...) air_voice_logf('E', __VA_ARGS__)
#define AV_LOGW(...) air_voice_logf('W', __VA_ARGS__)
#define AV_LOGI(...) air_voice_logf('I', __VA_ARGS__)

#ifdef __cplusplus
}
#endif
