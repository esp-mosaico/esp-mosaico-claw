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
#include "esp_gsp.h"
#include "asr_service.h"
#include "ai_create_voice_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*mosaic_ui_haptic_cb_t)(void *user_ctx,
                                           uint32_t duration_ms);
/**
 * Start the ported esp-gsp mosaic example UI.
 *
 * Uses board-manager display_lcd / lcd_touch handles (already created by
 * esp_board_manager_init). Does not start LVGL / display_service.
 */
esp_err_t mosaic_ui_start(void);
/** Queue one Back operation; safe to call from a board-button callback. */
esp_err_t mosaic_ui_back(void);
/** Queue AI Create as the active App; safe to call from a board-button callback. */
esp_err_t mosaic_ui_open_ai_create(void);

/** Set Hub inactivity timeout used to lock and turn the panel off (0 = never). */
void mosaic_ui_set_screen_timeout(uint32_t timeout_ms);
/** Reset the inactivity timer after one mapped Mosaic pointer sample. */
void mosaic_ui_note_screen_activity(void);
/** Loader-owned foreground notification; only Hub participates in screen timeout. */
void mosaic_ui_set_hub_foreground(bool foreground);
/** ISR-safe CST9217 interrupt hook; posts a wake only while the panel is off. */
void mosaic_ui_screen_wake_from_isr(void *user_ctx);
/** Drop pointer samples while the panel is asleep or resuming. */
bool mosaic_ui_absorb_wake_pointer(bool pressed);

/** Configure the shared ASR instance before or after mosaic_ui_start(). */
esp_err_t mosaic_ui_set_ai_create_asr(asr_service_handle_t asr);
/** Publish a structured unavailable state when no ASR instance can be built. */
esp_err_t mosaic_ui_set_ai_create_voice_status(ai_create_voice_status_t status);

/** Register optional device haptics. Simulators may leave this unset. */
void mosaic_ui_set_haptic_callback(
    mosaic_ui_haptic_cb_t callback, void *user_ctx);

/** Request one bounded haptic pulse; a missing backend is a no-op. */
esp_err_t mosaic_ui_haptic_feedback(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
