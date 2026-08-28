/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_service_internal.h"
#include "display_service_target.h"
#include "display_service_present.h"

#include <string.h>

#include "devices/dev_display_lcd/dev_display_lcd.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

static const char *TAG = "display_lvgl";
#define DISPLAY_SERVICE_DEFAULT_TICK_MS 5
#define DISPLAY_SERVICE_DEFAULT_TASK_PERIOD_MS 10
#define DISPLAY_SERVICE_TASK_STACK 8192
#define DISPLAY_SERVICE_TASK_PRIO 5
#define DISPLAY_SERVICE_HOME_INDICATOR_WIDTH 58
#define DISPLAY_SERVICE_HOME_INDICATOR_HEIGHT 4
#define DISPLAY_SERVICE_HOME_INDICATOR_BOTTOM_OFFSET 8
#define DISPLAY_SERVICE_HOME_INDICATOR_COLOR 0x3B3C3D

static bool s_adapter_initialized;
static bool s_adapter_started;

static esp_err_t display_service_lvgl_lock(void)
{
    return esp_lv_adapter_lock(1000);
}

static void display_service_lvgl_unlock(void)
{
    esp_lv_adapter_unlock();
}


static esp_err_t display_service_lvgl_touch_read(
    esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *points,
    uint8_t *count, uint8_t max_count, void *user_ctx)
{
    struct display_service_session_t *session = user_ctx;
    esp_err_t err;

    if (tp == NULL || points == NULL || count == NULL || max_count == 0 ||
            !display_service_session_valid_internal(session)) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    const uint32_t sequence = session->touch_irq_sequence;
    if (sequence == session->touch_irq_consumed) {
        if (session->touch_cached_count > 0) {
            points[0] = session->touch_cached_point;
            *count = 1;
        }
        return ESP_OK;
    }
    session->touch_irq_consumed = sequence;
    err = esp_lcd_touch_read_data(tp);
    if (err != ESP_OK) {
        session->touch_cached_count = 0;
        return err;
    }
    err = esp_lcd_touch_get_data(tp, points, count, max_count);
    if (err != ESP_OK) {
        session->touch_cached_count = 0;
        return err;
    }
    for (uint8_t i = 0; i < *count; ++i) {
        int32_t x = points[i].x;
        int32_t y = points[i].y;
        err = display_service_map_touch_internal(&x, &y);
        if (err != ESP_OK) {
            return err;
        }
        points[i].x = x;
        points[i].y = y;
    }
    session->touch_cached_count = *count > 0 ? 1 : 0;
    if (session->touch_cached_count > 0) {
        session->touch_cached_point = points[0];
    }

    const display_service_touch_sample_t sample = {
        .pressed = *count > 0,
        .x = *count > 0 ? points[0].x : 0,
        .y = *count > 0 ? points[0].y : 0,
    };
    display_service_notify_touch_internal(&sample);

    const int32_t display_height = session->lvgl_display != NULL ?
        lv_display_get_vertical_resolution(session->lvgl_display) : 0;
    if (display_service_process_exit_gesture_internal(
            session, &sample, display_height)) {
        /* Cancel the active LVGL pointer before requesting session exit. */
        *count = 0;
    }
    return ESP_OK;
}

#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT || \
    CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || \
    CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
static esp_lv_adapter_rotation_t display_service_get_rotation(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg->swap_xy) {
        return lcd_cfg->mirror_x ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_270;
    }
    return (lcd_cfg->mirror_x || lcd_cfg->mirror_y) ?
           ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
}
#endif

static bool display_service_make_lvgl_display_config(
    const dev_display_lcd_config_t *lcd_cfg,
    uint32_t buffer_lines,
    esp_lv_adapter_display_config_t *out_config)
{
    esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    uint16_t default_lines = 50;
#ifdef CONFIG_SPIRAM
    /*
     * The present-only bridge redirects LVGL's active draw buffer to the
     * presenter lease at refresh start. This allocation is only LVGL's
     * registered fallback buffer, so keeping it in internal DMA memory wastes
     * scarce contiguous SRAM and makes repeated init/deinit fail after heap
     * fragmentation.
     */
    bool use_psram = true;
#else
    bool use_psram = false;
#endif

    if (!lcd_cfg || !out_config) {
        return false;
    }

    if (strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_DSI) == 0 ||
            strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0 ||
            strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB_3WIRE_SPI) == 0) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_DSI_SUPPORT || \
    CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT || \
    CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_3WIRE_SPI_SUPPORT
        rotation = display_service_get_rotation(lcd_cfg);
#else
        return false;
#endif
    } else if (strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_SPI) == 0 ||
               strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_I80) == 0 ||
               strcmp(lcd_cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_PARLIO) == 0) {
#ifdef CONFIG_SPIRAM
        default_lines = lcd_cfg->lcd_height;
#else
        default_lines = 10;
#endif
    } else {
        ESP_LOGE(TAG, "unsupported LCD sub_type: %s",
                 lcd_cfg->sub_type ? lcd_cfg->sub_type : "(null)");
        return false;
    }

    uint16_t lines = buffer_lines > 0 ? (uint16_t)buffer_lines : default_lines;
    if (lines > lcd_cfg->lcd_height) {
        lines = lcd_cfg->lcd_height;
    }

    *out_config = ESP_LV_ADAPTER_DISPLAY_PROFILE(
        lcd_cfg->lcd_width, lcd_cfg->lcd_height, rotation, lines, use_psram);
    ESP_LOGI(TAG, "register LVGL present display: sub_type=%s size=%ux%u rotation=%d lines=%u",
             lcd_cfg->sub_type, lcd_cfg->lcd_width, lcd_cfg->lcd_height,
             (int)rotation, (unsigned)lines);
    return true;
}

static bool display_service_lvgl_generation_valid(
    void *user_ctx, uint32_t generation)
{
    return display_service_presenter_validate(user_ctx, generation);
}

static void display_service_lvgl_present_teardown(
    struct display_service_session_t *session)
{
    display_service_touch_forward_stop_internal(session);
    if (session->lvgl_home_indicator) {
        if (esp_lv_adapter_lock(1000) == ESP_OK) {
            if (lv_obj_is_valid(session->lvgl_home_indicator)) {
                lv_obj_delete(session->lvgl_home_indicator);
            }
            session->lvgl_home_indicator = NULL;
            esp_lv_adapter_unlock();
        } else {
            ESP_LOGW(TAG, "lock LVGL shell teardown failed");
        }
    }
    if (session->lvgl_touch_indev) {
        (void)esp_lv_adapter_unregister_touch(session->lvgl_touch_indev);
        session->lvgl_touch_indev = NULL;
    }
    if (session->lvgl_display) {
        (void)esp_lv_adapter_unregister_display(session->lvgl_display);
        session->lvgl_display = NULL;
    }
    if (s_adapter_initialized) {
        (void)esp_lv_adapter_deinit();
        s_adapter_initialized = false;
        s_adapter_started = false;
    }
    session->producer_generation = 0;
}

static esp_err_t display_service_lvgl_create_home_indicator(
    struct display_service_session_t *session)
{
    ESP_RETURN_ON_ERROR(
        esp_lv_adapter_lock(1000), TAG, "lock LVGL shell creation");

    lv_obj_t *indicator = lv_obj_create(lv_layer_top());
    if (indicator == NULL) {
        esp_lv_adapter_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(
        indicator, DISPLAY_SERVICE_HOME_INDICATOR_WIDTH,
        DISPLAY_SERVICE_HOME_INDICATOR_HEIGHT);
    lv_obj_align(
        indicator, LV_ALIGN_BOTTOM_MID, 0,
        -DISPLAY_SERVICE_HOME_INDICATOR_BOTTOM_OFFSET);
    lv_obj_set_style_bg_color(
        indicator, lv_color_hex(DISPLAY_SERVICE_HOME_INDICATOR_COLOR), 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(
        indicator, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(indicator, LV_OBJ_FLAG_FLOATING);
    session->lvgl_home_indicator = indicator;

    esp_lv_adapter_unlock();
    return ESP_OK;
}

static esp_err_t display_service_lvgl_present_quiesce(
    void *ctx, uint32_t timeout_ms)
{
    struct display_service_session_t *session = ctx;
    ESP_RETURN_ON_FALSE(
        display_service_session_valid_internal(session) &&
        session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
        ESP_ERR_INVALID_STATE, TAG, "LVGL producer is not active");
    if (s_adapter_started && !session->lvgl_paused) {
        ESP_RETURN_ON_ERROR(
            esp_lv_adapter_pause((int32_t)timeout_ms),
            TAG, "pause LVGL producer");
        session->lvgl_paused = true;
    }
    esp_err_t ret = esp_display_presenter_quiesce(
        display_service_presenter_internal(), timeout_ms);
    if (ret != ESP_OK) {
        if (s_adapter_started && session->lvgl_paused) {
            esp_err_t resume_ret = esp_lv_adapter_resume();
            if (resume_ret == ESP_OK) {
                session->lvgl_paused = false;
            } else {
                ESP_LOGE(TAG, "resume LVGL after quiesce failure: %s",
                         esp_err_to_name(resume_ret));
            }
        }
        ESP_LOGE(TAG, "quiesce LVGL presenter failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    display_service_lvgl_present_teardown(session);
    return ESP_OK;
}

static esp_err_t display_service_lvgl_present_activate(
    void *ctx, esp_display_presenter_t *presenter, uint32_t generation)
{
    struct display_service_session_t *session = ctx;
    dev_display_lcd_config_t *lcd_cfg = NULL;
    dev_display_lcd_handles_t *lcd_handles = NULL;
    uint32_t buffer_lines = 0;
    ESP_RETURN_ON_FALSE(
        display_service_session_valid_internal(session) &&
        session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL &&
        presenter == display_service_presenter_internal() &&
        display_service_presenter_validate(session, generation),
        ESP_ERR_INVALID_STATE, TAG, "LVGL producer activation rejected");
    ESP_RETURN_ON_ERROR(
        display_service_target_load_display(&lcd_cfg, &lcd_handles),
        TAG, "load LVGL board display");
    ESP_RETURN_ON_FALSE(
        lcd_cfg && lcd_handles && lcd_handles->panel_handle,
        ESP_ERR_INVALID_STATE, TAG, "LVGL board display unavailable");
    ESP_RETURN_ON_ERROR(
        display_service_present_buffer_lines(presenter, &buffer_lines),
        TAG, "resolve LVGL buffer lines from present");

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_priority = DISPLAY_SERVICE_TASK_PRIO;
    adapter_config.task_stack_size = DISPLAY_SERVICE_TASK_STACK;
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    adapter_config.task_core_id = 1;
#else
    adapter_config.task_core_id = 0;
#endif
    adapter_config.tick_period_ms = session->lvgl_config.tick_ms ?
        session->lvgl_config.tick_ms : DISPLAY_SERVICE_DEFAULT_TICK_MS;
    adapter_config.task_max_delay_ms = session->lvgl_config.task_period_ms ?
        session->lvgl_config.task_period_ms :
        DISPLAY_SERVICE_DEFAULT_TASK_PERIOD_MS;
    esp_err_t ret = esp_lv_adapter_init(&adapter_config);
    if (ret != ESP_OK) {
        return ret;
    }
    s_adapter_initialized = true;

    esp_lv_adapter_display_config_t display_config;
    if (!display_service_make_lvgl_display_config(
            lcd_cfg, buffer_lines, &display_config)) {
        display_service_lvgl_present_teardown(session);
        return ESP_ERR_NOT_SUPPORTED;
    }
    session->producer_generation = generation;
    session->lvgl_display = esp_lv_adapter_register_display_with_presenter(
        &display_config, &(esp_lv_adapter_presenter_config_t) {
            .presenter = presenter,
            .producer_generation = generation,
            .render_alignment = {
                .x_pixels = 4,
                .y_pixels = 4,
                .width_pixels = 4,
                .height_pixels = 4,
            },
            .validate_generation = display_service_lvgl_generation_valid,
            .validation_user_ctx = session,
        });
    if (!session->lvgl_display) {
        display_service_lvgl_present_teardown(session);
        return ESP_FAIL;
    }
    if (display_service_touch_internal()) {
        ret = display_service_touch_forward_start_internal(session);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "start presenter touch forwarding failed: %s",
                     esp_err_to_name(ret));
            display_service_lvgl_present_teardown(session);
            return ret;
        }
        session->lvgl_touch_indev = esp_lv_adapter_register_touch(
            &(esp_lv_adapter_touch_config_t) {
                .disp = session->lvgl_display,
                .handle = display_service_touch_internal(),
                .scale = {
                    .x = 1.0f,
                    .y = 1.0f,
                },
                .callbacks = {
                    .custom_touch_read = display_service_lvgl_touch_read,
                    .user_ctx = session,
                },
            });
        if (!session->lvgl_touch_indev) {
            ESP_LOGE(TAG, "register presenter LVGL touch failed");
            display_service_lvgl_present_teardown(session);
            return ESP_FAIL;
        }
    }
    ret = esp_lv_adapter_start();
    if (ret != ESP_OK) {
        display_service_lvgl_present_teardown(session);
        return ret;
    }
    s_adapter_started = true;
    session->lvgl_paused = false;
    ret = display_service_lvgl_create_home_indicator(session);
    if (ret != ESP_OK) {
        display_service_lvgl_present_teardown(session);
        return ret;
    }
    return ESP_OK;
}

static const display_service_present_producer_ops_t s_lvgl_present_ops = {
    .quiesce = display_service_lvgl_present_quiesce,
    .activate = display_service_lvgl_present_activate,
};

esp_err_t display_service_lvgl_open_internal(
    const display_service_session_config_t *config,
    const char *owner_name,
    display_service_session_handle_t *ret_session)
{
    ESP_RETURN_ON_FALSE(
        display_service_presenter_internal(),
        ESP_ERR_INVALID_STATE, TAG, "service presenter unavailable");
    struct display_service_session_t *session = NULL;
    ESP_RETURN_ON_ERROR(
        display_service_session_alloc_internal(
            DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL, config, owner_name, &session),
        TAG, "allocate LVGL session");
    session->lvgl_config = config->display_config;
    return display_service_session_acquire_producer_internal(
        session, &s_lvgl_present_ops, ret_session);
}


esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session, lv_obj_t *screen)
{
    ESP_RETURN_ON_FALSE(display_service_session_valid_internal(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    ESP_RETURN_ON_FALSE(session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
                        ESP_ERR_INVALID_STATE, TAG, "session is not exclusive LVGL");
    ESP_RETURN_ON_FALSE(
        display_service_presenter_validate(
            session, session->producer_generation),
        ESP_ERR_INVALID_STATE, TAG, "LVGL presenter lease is stale");
    lv_screen_load(screen);
    return ESP_OK;
}

esp_err_t display_service_session_load_screen(display_service_session_handle_t session, lv_obj_t *screen)
{
    ESP_RETURN_ON_ERROR(display_service_session_lock(session), TAG,
                        "lock display session");
    esp_err_t err = display_service_session_load_screen_locked(session, screen);
    display_service_session_unlock(session);
    return err;
}

lv_display_t *display_service_session_display(display_service_session_handle_t session)
{
    return display_service_session_valid_internal(session) &&
                   session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL
               ? session->lvgl_display
               : NULL;
}

esp_err_t display_service_session_lock(display_service_session_handle_t session)
{
    ESP_RETURN_ON_FALSE(
        display_service_session_valid_internal(session) &&
        session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL &&
        s_adapter_initialized,
        ESP_ERR_INVALID_STATE, TAG, "LVGL session lock unavailable");
    return display_service_lvgl_lock();
}

void display_service_session_unlock(display_service_session_handle_t session)
{
    if (display_service_session_valid_internal(session) &&
            session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL &&
            s_adapter_initialized) {
        display_service_lvgl_unlock();
    }
}


esp_err_t display_service_lvgl_prepare_close_internal(
    display_service_session_handle_t session)
{
    if (!session->lvgl_paused) {
        ESP_RETURN_ON_ERROR(
            esp_lv_adapter_pause(DISPLAY_SERVICE_PRESENT_HANDOFF_TIMEOUT_MS),
            TAG, "pause LVGL before session cleanup");
        session->lvgl_paused = true;
    }
    void *cleanup_user_ctx = NULL;
    display_service_session_cleanup_cb_t cleanup_cb =
        display_service_session_take_cleanup_internal(
            session, &cleanup_user_ctx);
    if (cleanup_cb != NULL) {
        cleanup_cb(session, cleanup_user_ctx);
    }
    return ESP_OK;
}
