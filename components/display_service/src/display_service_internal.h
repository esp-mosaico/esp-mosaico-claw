/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Internal contract shared by display_service producer backends. */
#pragma once

#include "display_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define DISPLAY_SERVICE_PRESENT_HANDOFF_TIMEOUT_MS 5000
#define DISPLAY_SERVICE_EXIT_GESTURE_START_HEIGHT 80
#define DISPLAY_SERVICE_EXIT_GESTURE_MIN_DY 72

struct display_service_session_t {
    bool active;
    bool closing;
    display_service_mode_t mode;
    uint32_t flags;
    display_service_session_cleanup_cb_t cleanup_cb;
    display_service_session_exit_request_cb_t exit_request_cb;
    void *cleanup_user_ctx;
    char owner_name[DISPLAY_SERVICE_OWNER_NAME_LEN];
    /* Presenter lease generation for whichever producer mode this session owns. */
    uint32_t producer_generation;
    bool lvgl_paused;
    lv_display_t *lvgl_display;
    lv_indev_t *lvgl_touch_indev;
    lv_obj_t *lvgl_home_indicator;
    bool exit_gesture_tracking;
    bool exit_gesture_captured;
    bool exit_request_sent;
    int32_t exit_gesture_start_y;
    display_service_config_t lvgl_config;
    esp_display_present_pixel_format_t raw_pixel_format;
    uint16_t raw_width;
    uint16_t raw_height;
    TaskHandle_t raw_touch_task;
    volatile bool raw_touch_stop;
    volatile uint32_t touch_irq_sequence;
    uint32_t touch_irq_consumed;
    volatile bool touch_irq_active;
    esp_lcd_touch_point_data_t touch_cached_point;
    uint8_t touch_cached_count;
    /* Short bookkeeping gate: guards raw_in_flight / raw_closing only. */
    SemaphoreHandle_t raw_io_mutex;
    /*
     * Render serialization gate: held across the whole blit body so the
     * presenter's single-task renderer contract is honored when multiple
     * tasks call display_service_session_raw_blit(). Never held together
     * with raw_io_mutex.
     */
    SemaphoreHandle_t raw_render_mutex;
    uint32_t raw_in_flight;
    bool raw_closing;
};

bool display_service_session_valid_internal(display_service_session_handle_t session);
esp_err_t display_service_session_alloc_internal(
    display_service_mode_t mode, const display_service_session_config_t *config,
    const char *owner_name, struct display_service_session_t **out_session);
esp_err_t display_service_session_free_internal(
    struct display_service_session_t *session);
esp_err_t display_service_session_begin_close_internal(
    struct display_service_session_t *session);
void display_service_session_abort_close_internal(
    struct display_service_session_t *session);
display_service_session_cleanup_cb_t
display_service_session_take_cleanup_internal(
    struct display_service_session_t *session, void **out_user_ctx);
esp_display_presenter_t *display_service_presenter_internal(void);
esp_lcd_touch_handle_t display_service_touch_internal(void);
esp_err_t display_service_touch_forward_start_internal(
    struct display_service_session_t *session);
void display_service_touch_forward_stop_internal(
    struct display_service_session_t *session);
void display_service_notify_touch_internal(const display_service_touch_sample_t *sample);
bool display_service_process_exit_gesture_internal(
    struct display_service_session_t *session,
    const display_service_touch_sample_t *sample,
    int32_t display_height);
esp_err_t display_service_map_touch_internal(int32_t *x, int32_t *y);
esp_err_t display_service_presenter_acquire_internal(
    const display_service_present_producer_t *producer,
    uint32_t timeout_ms, uint32_t *out_generation);

/*
 * Shared open tail for exclusive backends: build the producer descriptor from
 * the freshly allocated session, acquire the presenter lease, record the
 * generation, and publish the session. On failure the session is freed. The
 * caller owns all mode-specific setup done before this call.
 */
esp_err_t display_service_session_acquire_producer_internal(
    struct display_service_session_t *session,
    const display_service_present_producer_ops_t *ops,
    display_service_session_handle_t *ret_session);

esp_err_t display_service_raw_open_internal(
    const display_service_session_config_t *config, const char *owner_name,
    display_service_session_handle_t *ret_session);
esp_err_t display_service_lvgl_open_internal(
    const display_service_session_config_t *config, const char *owner_name,
    display_service_session_handle_t *ret_session);
esp_err_t display_service_lvgl_prepare_close_internal(
    display_service_session_handle_t session);
