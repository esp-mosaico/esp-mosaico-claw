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
#include "esp_display_present.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t buffer_lines;
    uint32_t tick_ms;
    uint32_t task_period_ms;
} display_service_config_t;

typedef struct display_service_session_t *display_service_session_handle_t;

typedef enum {
    DISPLAY_SERVICE_MODE_SHARED_LVGL = 0,
    DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
    DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
} display_service_mode_t;

typedef enum {
    DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE = 1 << 0,
    DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY       = 1 << 1,
} display_service_session_flags_t;

typedef void (*display_service_session_cleanup_cb_t)(display_service_session_handle_t session, void *user_ctx);
typedef void (*display_service_session_exit_request_cb_t)(
    display_service_session_handle_t session, void *user_ctx);

typedef struct {
    const char *owner_name;
    display_service_mode_t mode;
    uint32_t flags;
    display_service_config_t display_config;
    display_service_session_cleanup_cb_t cleanup_cb;
    display_service_session_exit_request_cb_t exit_request_cb;
    void *user_ctx;
} display_service_session_config_t;

typedef struct {
    int x_start;
    int y_start;
    int x_end;
    int y_end;
    const void *frame_buffer;
    bool wait;
} display_service_raw_blit_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    esp_display_present_pixel_format_t pixel_format;
} display_service_raw_info_t;

#define DISPLAY_SERVICE_OWNER_NAME_LEN 32

typedef struct {
    bool pressed;
    int32_t x;
    int32_t y;
} display_service_touch_sample_t;

typedef void (*display_service_touch_observer_cb_t)(const display_service_touch_sample_t *sample,
                                                    void *user_ctx);

typedef enum {
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_ENTERED = 0,
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_EXITED,
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_ENTERED,
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED,
} display_service_state_event_t;

typedef void (*display_service_state_observer_cb_t)(display_service_state_event_t event,
                                                    void *user_ctx);

typedef struct {
    esp_err_t (*set_brightness)(uint8_t percent, void *user_ctx);
    esp_err_t (*get_brightness)(uint8_t *percent, void *user_ctx);
    esp_err_t (*set_rotation)(uint16_t degrees, void *user_ctx);
    esp_err_t (*get_rotation)(uint16_t *degrees, void *user_ctx);
    void *user_ctx;
} display_service_control_provider_t;

esp_err_t display_service_set_control_provider(
    const display_service_control_provider_t *provider);

esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session);
esp_err_t display_service_close(display_service_session_handle_t session);
bool display_service_session_is_valid(display_service_session_handle_t session);
bool display_service_session_is_active(display_service_session_handle_t session);
display_service_mode_t display_service_session_mode(display_service_session_handle_t session);
const char *display_service_session_owner_name(display_service_session_handle_t session);
/** Wait until no exclusive display session remains active. */
esp_err_t display_service_wait_idle(uint32_t timeout_ms);
esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session, lv_obj_t *screen);
esp_err_t display_service_session_load_screen(display_service_session_handle_t session, lv_obj_t *screen);
lv_display_t *display_service_session_display(display_service_session_handle_t session);
/**
 * Serialize LVGL API access for a presenter-backed LVGL session.
 * Calls must be balanced and made while the session remains valid.
 */
esp_err_t display_service_session_lock(display_service_session_handle_t session);
void display_service_session_unlock(display_service_session_handle_t session);
/**
 * Copy one rectangle through the active RAW session.
 * Calls may originate from multiple tasks. Close prevents new calls and waits
 * for an accepted blit to finish before handing the presenter back.
 */
esp_err_t display_service_session_raw_blit(display_service_session_handle_t session,
                                           const display_service_raw_blit_t *blit);
/** Query the geometry/format negotiated by an active RAW session. */
esp_err_t display_service_session_get_raw_info(
    display_service_session_handle_t session,
    display_service_raw_info_t *out_info);

bool display_service_is_started(void);
esp_err_t display_service_start(const display_service_config_t *config);
void display_service_stop(void);
uint32_t display_service_width(void);
uint32_t display_service_height(void);
esp_err_t display_service_set_brightness(uint8_t percent);
/**
 * Configure a startup brightness transition. The panel commands are sent
 * after LCD IO initialization and before the presenter can submit frames.
 */
esp_err_t display_service_prepare_brightness_fade(
    uint8_t start_percent, uint8_t target_percent, uint32_t duration_ms);
esp_err_t display_service_get_brightness(uint8_t *percent);
typedef esp_err_t (*display_service_brightness_provider_t)(
    esp_lcd_panel_handle_t panel, uint8_t percent, void *user_ctx);
void display_service_set_brightness_provider(
    display_service_brightness_provider_t provider, void *user_ctx);
esp_err_t display_service_set_rotation(uint16_t degrees);
esp_err_t display_service_get_rotation(uint16_t *degrees);
/** Switch panel scanning on/off without changing the saved brightness. */
esp_err_t display_service_set_panel_enabled(bool enabled);
bool display_service_panel_enabled(void);

/**
 * Single touch sample sink for RAW/LVGL producers (e.g. Lua display.poll_touch).
 * Pass cb=NULL to clear. Replaces any previous observer.
 */
esp_err_t display_service_set_touch_observer(display_service_touch_observer_cb_t cb,
                                             void *user_ctx);
/** Copy the latest sample produced by the main display touch path. */
esp_err_t display_service_get_main_touch_sample(display_service_touch_sample_t *out_sample);
/** Forward a hardware touch interrupt from the active touch owner. ISR-safe. */
void display_service_touch_wake_from_isr(void);
esp_err_t display_service_set_state_observer(display_service_state_observer_cb_t cb,
                                             void *user_ctx);
bool display_service_has_exclusive_session(void);
bool display_service_exclusive_allows_system_overlay(void);
esp_err_t display_service_lock(void);
void display_service_unlock(void);
esp_err_t display_service_set_default_screen(lv_obj_t *screen);
void display_service_set_default_screen_locked(lv_obj_t *screen);
lv_obj_t *display_service_default_screen(void);
void display_service_exclusive_raw_suspend_locked(void);
void display_service_exclusive_raw_resume_locked(void);
typedef struct {
    /**
     * Stop submitting new frames and wait for accepted frames to retire.
     * A failed call must leave the producer active or restore it internally.
     */
    esp_err_t (*quiesce)(void *ctx, uint32_t timeout_ms);
    /**
     * Bind to the presenter on the producer's render task, resume rendering,
     * and force the first frame to cover the full display.
     * On failure the producer must remain quiescent.
     */
    esp_err_t (*activate)(void *ctx, esp_display_presenter_t *presenter,
                          uint32_t generation);
} display_service_present_producer_ops_t;

typedef struct {
    const void *identity;
    const display_service_present_producer_ops_t *ops;
    void *ctx;
} display_service_present_producer_t;

/**
 * Initialize the process-lifetime display singleton and activate its baseline
 * producer. Exactly one exclusive LVGL or RAW session may temporarily replace
 * the baseline. The returned presenter and touch handles are borrowed.
 */
esp_err_t display_service_presenter_start_baseline(
    const display_service_present_producer_t *producer,
    esp_display_presenter_t **out_presenter,
    esp_lcd_touch_handle_t *out_touch,
    uint32_t *out_generation);

bool display_service_presenter_validate(
    const void *producer,
    uint32_t generation);

#ifdef __cplusplus
}
#endif
