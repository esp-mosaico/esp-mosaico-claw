/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_service_internal.h"
#include "display_service_present.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/semphr.h"

static const char *TAG = "display_raw";
#define DISPLAY_SERVICE_RAW_TOUCH_TASK_STACK 3072
#define DISPLAY_SERVICE_RAW_TOUCH_TASK_PRIO 4
/* Budget for a queued blit to acquire the render gate behind another blit. */
#define DISPLAY_SERVICE_RAW_RENDER_LOCK_TIMEOUT_MS \
    DISPLAY_SERVICE_PRESENT_HANDOFF_TIMEOUT_MS

static esp_err_t display_service_raw_set_closing(
    struct display_service_session_t *session, bool closing)
{
    ESP_RETURN_ON_FALSE(
        session->raw_io_mutex != NULL,
        ESP_ERR_INVALID_STATE, TAG, "raw IO gate unavailable");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(session->raw_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "raw IO gate busy");
    session->raw_closing = closing;
    xSemaphoreGive(session->raw_io_mutex);
    return ESP_OK;
}

static esp_err_t display_service_raw_block_and_wait(
    struct display_service_session_t *session, uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(
        display_service_raw_set_closing(session, true),
        TAG, "block raw submissions");
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        ESP_RETURN_ON_FALSE(
            xSemaphoreTake(session->raw_io_mutex,
                           pdMS_TO_TICKS(1000)) == pdTRUE,
            ESP_ERR_TIMEOUT, TAG, "raw IO gate busy");
        uint32_t in_flight = session->raw_in_flight;
        xSemaphoreGive(session->raw_io_mutex);
        if (in_flight == 0) {
            return ESP_OK;
        }
        if (xTaskGetTickCount() - start >= timeout_ticks) {
            (void)display_service_raw_set_closing(session, false);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

static esp_err_t display_service_raw_io_begin(
    struct display_service_session_t *session)
{
    ESP_RETURN_ON_FALSE(
        session->raw_io_mutex != NULL,
        ESP_ERR_INVALID_STATE, TAG, "raw IO gate unavailable");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(session->raw_io_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "raw IO gate busy");
    if (!session->active || session->raw_closing) {
        xSemaphoreGive(session->raw_io_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    session->raw_in_flight++;
    xSemaphoreGive(session->raw_io_mutex);
    return ESP_OK;
}

static void display_service_raw_io_end(
    struct display_service_session_t *session)
{
    if (session->raw_io_mutex != NULL &&
            xSemaphoreTake(session->raw_io_mutex,
                           pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (session->raw_in_flight > 0) {
            session->raw_in_flight--;
        }
        xSemaphoreGive(session->raw_io_mutex);
    }
}

static void display_service_raw_touch_task(void *arg)
{
    struct display_service_session_t *session = arg;

    while (!session->raw_touch_stop) {
        if (session->touch_irq_sequence == session->touch_irq_consumed) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        session->touch_irq_consumed = session->touch_irq_sequence;
        esp_lcd_touch_point_data_t point = {0};
        uint8_t point_count = 0;
        display_service_touch_sample_t sample = {0};

        if (esp_lcd_touch_read_data(display_service_touch_internal()) == ESP_OK &&
                esp_lcd_touch_get_data(
                    display_service_touch_internal(), &point, &point_count, 1) == ESP_OK &&
                point_count > 0) {
            int32_t x = point.x;
            int32_t y = point.y;
            if (display_service_map_touch_internal(&x, &y) == ESP_OK) {
                sample.pressed = true;
                sample.x = x;
                sample.y = y;
            }
        }
        if (display_service_process_exit_gesture_internal(
                session, &sample, session->raw_height)) {
            /* The gesture belongs to the display shell, not the RAW app. */
            sample.pressed = false;
        }
        display_service_notify_touch_internal(&sample);
    }
    session->raw_touch_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t display_service_raw_touch_start(
    struct display_service_session_t *session)
{
    if (display_service_touch_internal() == NULL || session->raw_touch_task != NULL) {
        return ESP_OK;
    }
    session->raw_touch_stop = false;
    ESP_RETURN_ON_ERROR(
        display_service_touch_forward_start_internal(session),
        TAG, "start raw touch forwarding");
    BaseType_t created = xTaskCreate(
        display_service_raw_touch_task, "display_raw_touch",
        DISPLAY_SERVICE_RAW_TOUCH_TASK_STACK, session,
        DISPLAY_SERVICE_RAW_TOUCH_TASK_PRIO, &session->raw_touch_task);
    if (created != pdPASS) {
        display_service_touch_forward_stop_internal(session);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t display_service_raw_touch_stop(
    struct display_service_session_t *session, uint32_t timeout_ms)
{
    if (session->raw_touch_task == NULL) {
        return ESP_OK;
    }
    session->raw_touch_stop = true;
    xTaskNotifyGive(session->raw_touch_task);
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (session->raw_touch_task != NULL) {
        if (xTaskGetTickCount() - start >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    display_service_touch_forward_stop_internal(session);
    return ESP_OK;
}

static esp_err_t display_service_raw_present_quiesce(
    void *ctx, uint32_t timeout_ms)
{
    struct display_service_session_t *session = ctx;
    ESP_RETURN_ON_FALSE(
        display_service_session_valid_internal(session) &&
        session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
        ESP_ERR_INVALID_STATE, TAG, "raw producer is not active");
    ESP_RETURN_ON_ERROR(
        display_service_raw_block_and_wait(session, timeout_ms),
        TAG, "drain raw submissions");
    esp_err_t ret = display_service_raw_touch_stop(session, timeout_ms);
    if (ret != ESP_OK) {
        (void)display_service_raw_set_closing(session, false);
        ESP_LOGE(TAG, "stop raw touch polling failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }
    ret = esp_display_presenter_quiesce(
        display_service_presenter_internal(), timeout_ms);
    if (ret != ESP_OK) {
        (void)display_service_raw_set_closing(session, false);
        (void)display_service_raw_touch_start(session);
        ESP_LOGE(TAG, "quiesce raw presenter failed: %s", esp_err_to_name(ret));
        return ret;
    }
    session->producer_generation = 0;
    return ESP_OK;
}

/*
 * Fill one dirty rectangle band by band: acquire a region, validate its
 * geometry against the request, then either clear it (source == NULL, memset)
 * or copy rows from source (memcpy). The presenter frame must already be open;
 * on any error the frame is cancelled and the error returned. Callers own the
 * begin/commit around this loop.
 */
static esp_err_t display_service_raw_fill_area(
    esp_display_presenter_t *presenter,
    const esp_display_presenter_caps_t *caps,
    const esp_display_present_area_t *area,
    const uint8_t *source, size_t source_stride, size_t bytes_per_pixel)
{
    const uint16_t width = (uint16_t)(area->x2 - area->x1 + 1);
    esp_err_t ret = ESP_OK;
    int next_y = area->y1;
    while (next_y <= area->y2) {
        esp_display_present_area_t remaining = *area;
        remaining.y1 = next_y;
        esp_display_presenter_buffer_t buffer = {0};
        esp_display_presenter_region_t region = {0};
        ret = display_service_present_acquire_region(
            presenter, caps, &remaining, &buffer, &region);
        if (ret != ESP_OK) {
            esp_display_presenter_cancel_frame(presenter);
            return ret;
        }
        if (region.origin_x != area->x1 || region.origin_y != next_y ||
                region.surface.width != width ||
                region.surface.height == 0 ||
                next_y + region.surface.height - 1 > area->y2 ||
                region.surface.pixel_format != caps->pixel_format) {
            esp_display_presenter_cancel_frame(presenter);
            return ESP_ERR_INVALID_STATE;
        }
        const size_t row_bytes = (size_t)width * bytes_per_pixel;
        const size_t source_y = (size_t)(next_y - area->y1);
        for (uint16_t row = 0; row < region.surface.height; ++row) {
            uint8_t *dst = (uint8_t *)region.surface.pixels +
                           (size_t)row * region.surface.stride_bytes;
            if (source == NULL) {
                memset(dst, 0, row_bytes);
            } else {
                memcpy(dst, source + (source_y + row) * source_stride,
                       row_bytes);
            }
        }
        ret = display_service_present_submit_region(presenter, &buffer, &region);
        if (ret != ESP_OK) {
            esp_display_presenter_cancel_frame(presenter);
            return ret;
        }
        next_y += region.surface.height;
    }
    return ret;
}

static esp_err_t display_service_raw_present_clear(void)
{
    esp_display_presenter_caps_t caps = {0};
    ESP_RETURN_ON_ERROR(
        esp_display_presenter_get_caps(display_service_presenter_internal(), &caps),
        TAG, "get raw presenter caps");
    const esp_display_present_area_t full_area = {
        .x1 = 0,
        .y1 = 0,
        .x2 = caps.width - 1,
        .y2 = caps.height - 1,
    };
    const esp_display_present_surface_request_t request = {0};
    esp_display_present_area_t render_area = {0};
    size_t render_count = 0;
    bool full = false;
    esp_err_t ret = esp_display_presenter_begin_next_frame(
        display_service_presenter_internal(), &request, &render_area, 1,
        &render_count, &full);
    if (ret != ESP_OK) {
        return ret;
    }
    const size_t bytes_per_pixel =
        display_service_present_pixel_bytes(caps.pixel_format);
    ret = display_service_raw_fill_area(
        display_service_presenter_internal(), &caps, &full_area,
        NULL, 0, bytes_per_pixel);
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_display_presenter_submit_t submit = {
        .coverage = ESP_DISPLAY_PRESENT_COVERAGE_FULL,
    };
    ret = esp_display_presenter_commit_frame(display_service_presenter_internal(), &submit);
    if (ret != ESP_OK) {
        esp_display_presenter_cancel_frame(display_service_presenter_internal());
        return ret;
    }
    return ESP_OK;
}

static esp_err_t display_service_raw_present_activate(
    void *ctx, esp_display_presenter_t *presenter, uint32_t generation)
{
    struct display_service_session_t *session = ctx;
    ESP_RETURN_ON_FALSE(
        display_service_session_valid_internal(session) &&
        session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW &&
        presenter == display_service_presenter_internal() &&
        display_service_presenter_validate(session, generation),
        ESP_ERR_INVALID_STATE, TAG, "raw producer activation rejected");
    ESP_RETURN_ON_ERROR(
        esp_display_presenter_rebind_producer(presenter),
        TAG, "bind raw producer task");
    session->producer_generation = generation;
    ESP_RETURN_ON_ERROR(
        display_service_raw_set_closing(session, false),
        TAG, "enable raw submissions");
    ESP_RETURN_ON_ERROR(
        display_service_raw_present_clear(), TAG, "clear raw display");
    return display_service_raw_touch_start(session);
}

static const display_service_present_producer_ops_t s_raw_present_ops = {
    .quiesce = display_service_raw_present_quiesce,
    .activate = display_service_raw_present_activate,
};

esp_err_t display_service_raw_open_internal(
    const display_service_session_config_t *config,
    const char *owner_name,
    display_service_session_handle_t *ret_session)
{
    ESP_RETURN_ON_FALSE(
        display_service_presenter_internal(),
        ESP_ERR_INVALID_STATE, TAG, "service presenter unavailable");
    esp_display_presenter_caps_t caps = {0};
    ESP_RETURN_ON_ERROR(
        esp_display_presenter_get_caps(display_service_presenter_internal(), &caps),
        TAG, "get raw presenter caps");
    struct display_service_session_t *session = NULL;
    ESP_RETURN_ON_ERROR(
        display_service_session_alloc_internal(
            DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW, config, owner_name, &session),
        TAG, "allocate raw session");
    session->raw_width = caps.width;
    session->raw_height = caps.height;
    session->raw_pixel_format = caps.pixel_format;
    session->raw_io_mutex = xSemaphoreCreateMutex();
    if (session->raw_io_mutex == NULL) {
        (void)display_service_session_free_internal(session);
        return ESP_ERR_NO_MEM;
    }
    session->raw_render_mutex = xSemaphoreCreateMutex();
    if (session->raw_render_mutex == NULL) {
        (void)display_service_session_free_internal(session);
        return ESP_ERR_NO_MEM;
    }

    return display_service_session_acquire_producer_internal(
        session, &s_raw_present_ops, ret_session);
}
static esp_err_t display_service_raw_blit_frame(
    display_service_session_handle_t session,
    const display_service_raw_blit_t *blit)
{
    /*
     * session validity, non-NULL blit and RAW mode are already guaranteed by
     * the sole caller display_service_session_raw_blit(); only lease freshness
     * and per-blit geometry are checked here.
     */
    ESP_RETURN_ON_FALSE(
        display_service_presenter_validate(
            session, session->producer_generation),
        ESP_ERR_INVALID_STATE, TAG, "raw presenter lease is stale");
        const int width = blit->x_end - blit->x_start;
        const int height = blit->y_end - blit->y_start;
        ESP_RETURN_ON_FALSE(
            blit->frame_buffer && width > 0 && height > 0 &&
            blit->x_start >= 0 && blit->y_start >= 0 &&
            blit->x_end <= session->raw_width &&
            blit->y_end <= session->raw_height,
            ESP_ERR_INVALID_ARG, TAG, "raw present area invalid");
        const size_t bytes_per_pixel =
            display_service_present_pixel_bytes(session->raw_pixel_format);
        const esp_display_present_area_t area = {
            .x1 = blit->x_start,
            .y1 = blit->y_start,
            .x2 = blit->x_end - 1,
            .y2 = blit->y_end - 1,
        };
        const esp_display_present_surface_request_t request = {
            .dirty_areas = &area,
            .dirty_area_count = 1,
        };
        esp_display_presenter_caps_t caps = {0};
        ESP_RETURN_ON_ERROR(
            esp_display_presenter_get_caps(display_service_presenter_internal(), &caps),
            TAG, "get raw presenter caps");
        esp_display_present_area_t render_area = {0};
        size_t render_count = 0;
        bool full = false;
        esp_err_t ret = esp_display_presenter_rebind_producer(
            display_service_presenter_internal());
        ESP_RETURN_ON_ERROR(ret, TAG, "rebind raw blit task");
        ret = esp_display_presenter_begin_next_frame(
            display_service_presenter_internal(), &request, &render_area, 1,
            &render_count, &full);
        if (ret != ESP_OK) {
            return ret;
        }
        const bool input_is_full =
            area.x1 == 0 && area.y1 == 0 &&
            area.x2 == session->raw_width - 1 &&
            area.y2 == session->raw_height - 1;
        const bool render_area_matches =
            render_area.x1 == area.x1 && render_area.y1 == area.y1 &&
            render_area.x2 == area.x2 && render_area.y2 == area.y2;
        if ((!full && (render_count != 1 || !render_area_matches)) ||
                (full && !input_is_full)) {
            esp_display_presenter_cancel_frame(display_service_presenter_internal());
            return ESP_ERR_INVALID_STATE;
        }

        ret = display_service_raw_fill_area(
            display_service_presenter_internal(), &caps, &area,
            blit->frame_buffer, (size_t)width * bytes_per_pixel,
            bytes_per_pixel);
        if (ret != ESP_OK) {
            return ret;
        }
        const esp_display_presenter_submit_t submit = {
            .coverage = full ? ESP_DISPLAY_PRESENT_COVERAGE_FULL
                             : ESP_DISPLAY_PRESENT_COVERAGE_AREAS,
            .areas = full ? NULL : &area,
            .area_count = full ? 0 : 1,
        };
        ret = esp_display_presenter_commit_frame(display_service_presenter_internal(), &submit);
        if (ret != ESP_OK) {
            esp_display_presenter_cancel_frame(display_service_presenter_internal());
            return ret;
        }
        if (blit->wait) {
            return esp_display_presenter_quiesce(display_service_presenter_internal(), 1000);
        }
        return ESP_OK;
}

esp_err_t display_service_session_raw_blit(
    display_service_session_handle_t session,
    const display_service_raw_blit_t *blit)
{
    ESP_RETURN_ON_FALSE(display_service_session_valid_internal(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    ESP_RETURN_ON_FALSE(blit != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "raw blit config missing");
    ESP_RETURN_ON_FALSE(session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
                        ESP_ERR_INVALID_STATE, TAG,
                        "session is not exclusive raw");
    ESP_RETURN_ON_ERROR(
        display_service_raw_io_begin(session), TAG,
        "raw session is closing");
    /*
     * Serialize the whole blit so concurrent callers cannot interleave
     * rebind_producer/begin_next_frame and violate the presenter's
     * single-task renderer contract. raw_io_mutex is already released here,
     * so this never nests the two gates.
     */
    if (session->raw_render_mutex == NULL ||
            xSemaphoreTake(
                session->raw_render_mutex,
                pdMS_TO_TICKS(DISPLAY_SERVICE_RAW_RENDER_LOCK_TIMEOUT_MS)) !=
                pdTRUE) {
        display_service_raw_io_end(session);
        ESP_LOGE(TAG, "raw render gate busy");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = display_service_raw_blit_frame(session, blit);
    xSemaphoreGive(session->raw_render_mutex);
    display_service_raw_io_end(session);
    return ret;
}

esp_err_t display_service_session_get_raw_info(
    display_service_session_handle_t session,
    display_service_raw_info_t *out_info)
{
    ESP_RETURN_ON_FALSE(
        display_service_session_valid_internal(session) && out_info != NULL,
        ESP_ERR_INVALID_ARG, TAG, "invalid raw session info request");
    ESP_RETURN_ON_FALSE(
        session->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
        ESP_ERR_INVALID_STATE, TAG, "session is not exclusive raw");
    ESP_RETURN_ON_FALSE(
        display_service_presenter_validate(
            session, session->producer_generation),
        ESP_ERR_INVALID_STATE, TAG, "raw presenter lease is stale");
    *out_info = (display_service_raw_info_t) {
        .width = session->raw_width,
        .height = session->raw_height,
        .pixel_format = session->raw_pixel_format,
    };
    return ESP_OK;
}
