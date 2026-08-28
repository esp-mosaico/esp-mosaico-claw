/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lvgl_present_display.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_display_present.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl_private.h"
#include "adapter_internal.h"

static const char *TAG = "esp_lvgl_present";

typedef enum {
    LVGL_PRESENT_RENDER_PARTIAL,
    LVGL_PRESENT_RENDER_FULL,
} lvgl_present_render_mode_t;

static esp_lv_adapter_display_node_t *lvgl_present_find_node(lv_display_t *disp);
static void lvgl_present_destroy_node(esp_lv_adapter_display_node_t *node);

static bool area_valid(const lvgl_present_area_t *area)
{
    return area && area->x1 <= area->x2 && area->y1 <= area->y2;
}

static void merge_area(lvgl_present_area_t *dst, const lvgl_present_area_t *src)
{
    if (src->x1 < dst->x1) {
        dst->x1 = src->x1;
    }
    if (src->y1 < dst->y1) {
        dst->y1 = src->y1;
    }
    if (src->x2 > dst->x2) {
        dst->x2 = src->x2;
    }
    if (src->y2 > dst->y2) {
        dst->y2 = src->y2;
    }
}

static void append_area(lvgl_present_area_t *areas, size_t *count,
                        size_t capacity, const lvgl_present_area_t *area)
{
    if (*count < capacity) {
        areas[(*count)++] = *area;
        return;
    }
    for (size_t i = 1; i < *count; ++i) {
        merge_area(&areas[0], &areas[i]);
    }
    merge_area(&areas[0], area);
    *count = 1;
}

static esp_display_present_area_t to_present_area(const lvgl_present_area_t *area)
{
    return (esp_display_present_area_t) {
        .x1 = area->x1, .y1 = area->y1, .x2 = area->x2, .y2 = area->y2,
    };
}

static bool damage_is_full(const esp_lv_adapter_display_node_t *node,
                           const lvgl_present_area_t *damage,
                           size_t damage_count)
{
    if (!node || !damage || damage_count == 0) {
        return false;
    }
    int32_t x1 = damage[0].x1;
    int32_t y1 = damage[0].y1;
    int32_t x2 = damage[0].x2;
    int32_t y2 = damage[0].y2;
    for (size_t i = 1; i < damage_count; ++i) {
        if (damage[i].x1 < x1) {
            x1 = damage[i].x1;
        }
        if (damage[i].y1 < y1) {
            y1 = damage[i].y1;
        }
        if (damage[i].x2 > x2) {
            x2 = damage[i].x2;
        }
        if (damage[i].y2 > y2) {
            y2 = damage[i].y2;
        }
    }
    return x1 == 0 && y1 == 0 &&
           x2 == (int32_t)node->caps.width - 1 &&
           y2 == (int32_t)node->caps.height - 1;
}

static void clear_frame(esp_lv_adapter_display_node_t *node)
{
    node->frame_active = false;
    node->buffer_held = false;
    node->damage_count = 0;
    node->coverage_count = 0;
    memset(&node->leased_buffer, 0, sizeof(node->leased_buffer));
}

static esp_err_t fail_frame(esp_lv_adapter_display_node_t *node, esp_err_t error)
{
    if (node->frame_active) {
        esp_display_presenter_cancel_frame(node->cfg.presenter);
        memset(&node->acquired, 0, sizeof(node->acquired));
    }
    node->frame_active = false;
    node->buffer_held = false;
    node->fault = true;
    node->damage_count = 0;
    node->coverage_count = 0;
    memset(&node->leased_buffer, 0, sizeof(node->leased_buffer));
    return error == ESP_OK ? ESP_FAIL : error;
}

static bool leased_buffer_valid(const esp_lv_adapter_display_node_t *node,
                                const lvgl_present_region_t *buffer)
{
    return node && buffer && buffer->pixels && buffer->capacity_bytes != 0;
}

static esp_err_t present_begin_frame(esp_lv_adapter_display_node_t *node,
                                     const lvgl_present_area_t *damage,
                                     size_t damage_count)
{
    if (node->cfg.validate_generation &&
            !node->cfg.validate_generation(
                node->cfg.validation_user_ctx, node->cfg.producer_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!node->producer_bound) {
        esp_err_t ret = esp_display_presenter_rebind_producer(node->cfg.presenter);
        if (ret != ESP_OK) {
            return ret;
        }
        node->producer_bound = true;
    }

    esp_display_present_area_t request_areas[LVGL_PRESENT_MAX_AREAS];
    esp_display_present_area_t render_areas[LVGL_PRESENT_MAX_AREAS];
    for (size_t i = 0; i < damage_count; ++i) {
        request_areas[i] = to_present_area(&damage[i]);
    }
    const esp_display_present_surface_request_t request = {
        .dirty_areas = request_areas,
        .dirty_area_count = damage_count,
        .render_alignment = node->cfg.render_alignment,
    };
    size_t render_count = 0;
    bool full = false;
    esp_err_t ret = esp_display_presenter_begin_next_frame(
                        node->cfg.presenter, &request, render_areas,
                        LVGL_PRESENT_MAX_AREAS, &render_count, &full);
    if (ret != ESP_OK) {
        return ret;
    }
    if (node->caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL) {
        if (!full || render_count != 0 ||
                !damage_is_full(node, damage, damage_count)) {
            ESP_LOGE(TAG, "FULL presenter requires full LVGL damage");
            esp_display_presenter_cancel_frame(node->cfg.presenter);
            return ESP_ERR_INVALID_STATE;
        }
    } else if ((full && !damage_is_full(node, damage, damage_count)) ||
               (!full && (render_count != damage_count ||
                memcmp(render_areas, request_areas,
                       damage_count * sizeof(request_areas[0])) != 0))) {
        ESP_LOGE(TAG, "PARTITION presenter changed LVGL damage plan");
        esp_display_presenter_cancel_frame(node->cfg.presenter);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t present_acquire_region(esp_lv_adapter_display_node_t *node,
                                        const lvgl_present_area_t *remaining,
                                        lvgl_present_region_t *region)
{
    esp_err_t ret = esp_display_presenter_acquire_buffer(
                        node->cfg.presenter, &node->acquired);
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t width = (size_t)(remaining->x2 - remaining->x1 + 1);
    const size_t stride = width * node->bytes_per_pixel;
    const size_t requested_rows = (size_t)(remaining->y2 - remaining->y1 + 1);
    size_t rows = 0;
    ret = node->acquired.resolve_rows != NULL
          ? node->acquired.resolve_rows(
              node->acquired.resolve_rows_ctx, node->acquired.lease_id,
              stride, requested_rows, &rows)
          : ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK || rows == 0 || rows > UINT16_MAX) {
        esp_display_presenter_cancel_frame(node->cfg.presenter);
        memset(&node->acquired, 0, sizeof(node->acquired));
        return ESP_ERR_INVALID_SIZE;
    }

    *region = (lvgl_present_region_t) {
        .pixels = node->acquired.surface.pixels,
        .capacity_bytes = node->acquired.capacity_bytes,
        .stride_bytes = stride,
        .width = width,
        .height = rows,
        .x = remaining->x1,
        .y = remaining->y1,
    };
    return ESP_OK;
}

static esp_err_t present_submit_region(esp_lv_adapter_display_node_t *node,
                                       const lvgl_present_region_t *region)
{
    const esp_display_present_area_t area = {
        .x1 = region->x,
        .y1 = region->y,
        .x2 = (int32_t)region->x + region->width - 1,
        .y2 = (int32_t)region->y + region->height - 1,
    };
    esp_err_t ret = esp_display_presenter_submit_buffer(
                        node->cfg.presenter, &node->acquired, &area,
                        region->stride_bytes);
    if (ret == ESP_OK) {
        memset(&node->acquired, 0, sizeof(node->acquired));
    }
    return ret;
}

static esp_err_t present_commit_frame(esp_lv_adapter_display_node_t *node,
                                      const lvgl_present_area_t *coverage,
                                      size_t coverage_count)
{
    esp_display_present_area_t areas[LVGL_PRESENT_MAX_AREAS];
    for (size_t i = 0; i < coverage_count; ++i) {
        areas[i] = to_present_area(&coverage[i]);
    }
    const esp_display_presenter_submit_t submit = {
        .coverage = node->caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL
                    ? ESP_DISPLAY_PRESENT_COVERAGE_FULL
                    : ESP_DISPLAY_PRESENT_COVERAGE_AREAS,
        .areas = node->caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL
                 ? NULL : areas,
        .area_count = node->caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL
                      ? 0 : coverage_count,
    };
    return esp_display_presenter_commit_frame(node->cfg.presenter, &submit);
}

static void restore_draw_buffer(esp_lv_adapter_display_node_t *node)
{
    if (!node->redirect_draw_buf) {
        return;
    }
    node->redirect_draw_buf->data = node->redirect_fallback_data;
    node->redirect_draw_buf->data_size = node->redirect_fallback_size;
    node->redirect_draw_buf->header.h = node->redirect_fallback_height;
    node->redirect_draw_buf = NULL;
    node->redirect_fallback_data = NULL;
    node->redirect_fallback_size = 0;
    node->redirect_fallback_height = 0;
}

static esp_err_t frame_prepare(esp_lv_adapter_display_node_t *node,
                               const lvgl_present_area_t *full_area,
                               lvgl_present_region_t *out_buffer)
{
    if (!area_valid(full_area) || !out_buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (node->frame_active || node->fault) {
        return ESP_ERR_INVALID_STATE;
    }

    node->frame_active = true;
    node->full_area = *full_area;
    const lvgl_present_area_t *damage =
        node->damage_count ? node->damage : full_area;
    const size_t damage_count = node->damage_count ? node->damage_count : 1;

    esp_err_t ret = present_begin_frame(node, damage, damage_count);
    if (ret != ESP_OK) {
        return fail_frame(node, ret);
    }
    ret = present_acquire_region(node, full_area, &node->leased_buffer);
    if (ret != ESP_OK || !leased_buffer_valid(node, &node->leased_buffer)) {
        return fail_frame(node, ret == ESP_OK ? ESP_ERR_INVALID_SIZE : ret);
    }
    node->buffer_held = true;
    *out_buffer = node->leased_buffer;
    return ESP_OK;
}

static esp_err_t frame_flush(esp_lv_adapter_display_node_t *node,
                             const lvgl_present_area_t *area,
                             const void *pixels,
                             size_t stride_bytes,
                             bool is_last,
                             lvgl_present_region_t *out_next_buffer,
                             bool *out_has_next_buffer)
{
    if (!node || !out_next_buffer || !out_has_next_buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_next_buffer, 0, sizeof(*out_next_buffer));
    *out_has_next_buffer = false;
    esp_err_t result = ESP_OK;

    if (node->fault || !node->frame_active || !node->buffer_held) {
        result = ESP_ERR_INVALID_STATE;
        goto ready;
    }
    if (!area_valid(area) || !pixels || pixels != node->leased_buffer.pixels) {
        result = fail_frame(node, ESP_ERR_INVALID_ARG);
        goto ready;
    }

    const size_t width = (size_t)(area->x2 - area->x1 + 1);
    const size_t height = (size_t)(area->y2 - area->y1 + 1);
    if (stride_bytes < width * node->bytes_per_pixel ||
            height > node->leased_buffer.capacity_bytes / stride_bytes) {
        result = fail_frame(node, ESP_ERR_INVALID_SIZE);
        goto ready;
    }

    lvgl_present_region_t rendered = {
        .pixels = (void *)pixels,
        .capacity_bytes = node->leased_buffer.capacity_bytes,
        .stride_bytes = stride_bytes,
        .width = (uint16_t)width,
        .height = (uint16_t)height,
        .x = area->x1,
        .y = area->y1,
    };
    result = present_submit_region(node, &rendered);
    if (result != ESP_OK) {
        result = fail_frame(node, result);
        goto ready;
    }
    node->buffer_held = false;
    memset(&node->leased_buffer, 0, sizeof(node->leased_buffer));
    append_area(node->coverage, &node->coverage_count, node->area_capacity, area);

    if (is_last) {
        result = present_commit_frame(node, node->coverage, node->coverage_count);
        if (result != ESP_OK) {
            result = fail_frame(node, result);
            goto ready;
        }
        clear_frame(node);
    } else {
        result = present_acquire_region(node, &node->full_area, &node->leased_buffer);
        if (result != ESP_OK || !leased_buffer_valid(node, &node->leased_buffer)) {
            result = fail_frame(node, result == ESP_OK ? ESP_ERR_INVALID_SIZE : result);
            goto ready;
        }
        node->buffer_held = true;
        *out_next_buffer = node->leased_buffer;
        *out_has_next_buffer = true;
    }

ready:
    lv_display_flush_ready(node->lv_disp);
    return result;
}

static void frame_finish(esp_lv_adapter_display_node_t *node)
{
    if (!node || !node->frame_active) {
        return;
    }
    esp_display_presenter_cancel_frame(node->cfg.presenter);
    memset(&node->acquired, 0, sizeof(node->acquired));
    clear_frame(node);
}

static esp_err_t frame_add_damage(esp_lv_adapter_display_node_t *node,
                                  const lvgl_present_area_t *area)
{
    if (!node || !area_valid(area) || node->frame_active || node->fault) {
        return ESP_ERR_INVALID_STATE;
    }
    append_area(node->damage, &node->damage_count, node->area_capacity, area);
    return ESP_OK;
}

static esp_lv_adapter_display_node_t *lvgl_present_find_node(lv_display_t *disp)
{
    esp_lv_adapter_context_t *ctx = esp_lv_adapter_get_context();
    if (!ctx) {
        return NULL;
    }
    for (esp_lv_adapter_display_node_t *node = ctx->display_list; node; node = node->next) {
        if (node->lv_disp == disp) {
            return node;
        }
    }
    return NULL;
}

static void *alloc_draw_buffer(size_t size, bool use_psram)
{
    const uint32_t caps = use_psram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                    : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    void *buf = heap_caps_aligned_alloc(ESP_LVGL_PRESENT_BUF_ALIGN, size, caps);
    return buf ? buf : heap_caps_malloc(size, caps);
}

static size_t calc_draw_buf_bytes(size_t draw_buf_pixels,
                                  lv_color_format_t color_format)
{
    const size_t bpp = lv_color_format_get_bpp(color_format);
    return (draw_buf_pixels * bpp + 7U) / 8U;
}

static bool prepare_draw_buffer(esp_lv_adapter_display_node_t *node,
                                lvgl_present_render_mode_t mode,
                                lv_color_format_t color_format)
{
    const esp_lv_adapter_display_profile_t *profile = &node->cfg.base.profile;
    if (!profile->hor_res || !profile->ver_res) {
        ESP_LOGE(TAG, "invalid resolution %ux%u", profile->hor_res, profile->ver_res);
        return false;
    }

    size_t draw_buf_pixels = (size_t)profile->hor_res * profile->buffer_height;
    if (mode == LVGL_PRESENT_RENDER_FULL) {
        draw_buf_pixels = (size_t)profile->hor_res * profile->ver_res;
    }
    if (!draw_buf_pixels) {
        draw_buf_pixels = profile->hor_res;
    }

    const size_t allocation_bytes = calc_draw_buf_bytes(draw_buf_pixels, color_format);
    void *primary = alloc_draw_buffer(allocation_bytes, profile->use_psram);
    if (!primary) {
        ESP_LOGE(TAG, "alloc draw buffer %zu bytes failed", allocation_bytes);
        return false;
    }
    node->cfg.draw_buf_primary = primary;
    return true;
}

static lvgl_present_render_mode_t pick_render_mode(
    const esp_display_presenter_caps_t *caps)
{
    return caps->contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL
           ? LVGL_PRESENT_RENDER_FULL
           : LVGL_PRESENT_RENDER_PARTIAL;
}

static esp_err_t on_invalidate_area(esp_lv_adapter_display_node_t *node,
                                    lv_area_t *area)
{
    const uint16_t x_align = node->cfg.render_alignment.x_pixels != 0
                             ? node->cfg.render_alignment.x_pixels : 1;
    const uint16_t y_align = node->cfg.render_alignment.y_pixels != 0
                             ? node->cfg.render_alignment.y_pixels : 1;
    const uint16_t width_align = node->cfg.render_alignment.width_pixels != 0
                                 ? node->cfg.render_alignment.width_pixels : 1;
    const uint16_t height_align = node->cfg.render_alignment.height_pixels != 0
                                  ? node->cfg.render_alignment.height_pixels : 1;

    area->x1 -= area->x1 % x_align;
    area->y1 -= area->y1 % y_align;
    int32_t width = lv_area_get_width(area);
    int32_t height = lv_area_get_height(area);
    width += (width_align - width % width_align) % width_align;
    height += (height_align - height % height_align) % height_align;
    area->x2 = area->x1 + width - 1;
    area->y2 = area->y1 + height - 1;
    if (area->x2 >= node->caps.width) {
        area->x2 = node->caps.width - 1;
    }
    if (area->y2 >= node->caps.height) {
        area->y2 = node->caps.height - 1;
    }
    const lvgl_present_area_t damage = {area->x1, area->y1, area->x2, area->y2};
    return frame_add_damage(node, &damage);
}

static esp_err_t on_refresh_start(esp_lv_adapter_display_node_t *node,
                                  lv_display_t *disp)
{
    if (node->frame_active || node->fault) {
        return ESP_ERR_INVALID_STATE;
    }

    const lvgl_present_area_t full = {
        .x1 = 0, .y1 = 0,
        .x2 = node->caps.width - 1,
        .y2 = node->caps.height - 1,
    };
    if (node->caps.contract == ESP_DISPLAY_PRESENT_CONTRACT_FULL) {
        node->damage_count = 0;
    }

    lvgl_present_region_t buffer;
    esp_err_t ret = frame_prepare(node, &full, &buffer);
    if (ret != ESP_OK) {
        return ret;
    }

    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!draw_buf || draw_buf->header.stride == 0 ||
            buffer.capacity_bytes > UINT32_MAX) {
        frame_finish(node);
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t rows = buffer.height;
    if (rows == 0 || rows > UINT32_MAX) {
        frame_finish(node);
        return ESP_ERR_INVALID_SIZE;
    }

    node->redirect_draw_buf = draw_buf;
    node->redirect_fallback_data = draw_buf->data;
    node->redirect_fallback_size = draw_buf->data_size;
    node->redirect_fallback_height = draw_buf->header.h;
    draw_buf->data = buffer.pixels;
    draw_buf->data_size = (uint32_t)buffer.capacity_bytes;
    draw_buf->header.h = (uint32_t)rows;
    return ESP_OK;
}

static void on_refresh_finish(esp_lv_adapter_display_node_t *node)
{
    if (!node->frame_active) {
        return;
    }
    frame_finish(node);
    restore_draw_buffer(node);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_map)
{
    esp_lv_adapter_display_node_t *node = lv_display_get_user_data(disp);
    if (!node) {
        lv_display_flush_ready(disp);
        return;
    }

    const lvgl_present_area_t flush_area = {area->x1, area->y1, area->x2, area->y2};
    const size_t stride = (size_t)lv_area_get_width(area) * node->bytes_per_pixel;
    lvgl_present_region_t next_buffer;
    bool has_next_buffer = false;
    esp_err_t ret = frame_flush(
                          node, &flush_area, color_map, stride,
                          lv_display_flush_is_last(disp), &next_buffer,
                          &has_next_buffer);
    if (ret == ESP_OK && has_next_buffer) {
        node->redirect_draw_buf->data = next_buffer.pixels;
        node->redirect_draw_buf->data_size = (uint32_t)next_buffer.capacity_bytes;
        node->redirect_draw_buf->header.h = next_buffer.height;
    } else {
        restore_draw_buffer(node);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(ret));
    }
}

static void invalidate_wake_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    esp_lv_adapter_display_node_t *node = disp ? lv_display_get_user_data(disp) : NULL;
    lv_area_t *area = lv_event_get_invalidated_area(e);

    if (node && area) {
        (void)on_invalidate_area(node, area);
    }

    esp_lv_adapter_context_t *ctx = esp_lv_adapter_get_context();
    if (ctx && ctx->task && ctx->task != xTaskGetCurrentTaskHandle()) {
        xTaskNotifyGive(ctx->task);
    }
}

static void refresh_start_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    esp_lv_adapter_display_node_t *node = disp ? lv_display_get_user_data(disp) : NULL;
    if (!node) {
        return;
    }
    esp_err_t ret = on_refresh_start(node, disp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "prepare present refresh failed: %s", esp_err_to_name(ret));
    }
}

static void refresh_finish_cb(lv_event_t *e)
{
    lv_display_t *disp = lv_event_get_target(e);
    esp_lv_adapter_display_node_t *node = disp ? lv_display_get_user_data(disp) : NULL;
    if (node) {
        on_refresh_finish(node);
    }
}

static bool init_present_state(esp_lv_adapter_display_node_t *node)
{
    if (!node->cfg.presenter) {
        ESP_LOGE(TAG, "presenter is required");
        return false;
    }
    if (esp_display_presenter_get_caps(node->cfg.presenter, &node->caps) != ESP_OK) {
        ESP_LOGE(TAG, "failed to query presenter capabilities");
        return false;
    }
    if (node->caps.contract != ESP_DISPLAY_PRESENT_CONTRACT_PARTITION &&
            node->caps.contract != ESP_DISPLAY_PRESENT_CONTRACT_FULL) {
        ESP_LOGE(TAG, "unsupported presenter contract");
        return false;
    }
    if (node->caps.width != node->cfg.base.profile.hor_res ||
            node->caps.height != node->cfg.base.profile.ver_res ||
            (node->caps.pixel_format != ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565 &&
             node->caps.pixel_format != ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB888)) {
        ESP_LOGE(TAG, "presenter caps mismatch profile");
        return false;
    }

    node->bytes_per_pixel =
        node->caps.pixel_format == ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565 ? 2U : 3U;
    node->area_capacity = node->caps.max_damage_areas < LVGL_PRESENT_MAX_AREAS
                          ? node->caps.max_damage_areas
                          : LVGL_PRESENT_MAX_AREAS;
    return true;
}

static bool init_display_node(esp_lv_adapter_display_node_t *node)
{
    const esp_lv_adapter_display_config_t *pub = &node->cfg.base;
    if (!init_present_state(node)) {
        return false;
    }

    const esp_lv_adapter_rotation_t rot = pub->profile.rotation;
    if (rot != ESP_LV_ADAPTER_ROTATE_0 && rot != ESP_LV_ADAPTER_ROTATE_90 &&
            rot != ESP_LV_ADAPTER_ROTATE_180 && rot != ESP_LV_ADAPTER_ROTATE_270) {
        ESP_LOGE(TAG, "invalid rotation value %d", (int)rot);
        return false;
    }

    const lv_coord_t hor_res = (rot == ESP_LV_ADAPTER_ROTATE_90 || rot == ESP_LV_ADAPTER_ROTATE_270)
                               ? pub->profile.ver_res : pub->profile.hor_res;
    const lv_coord_t ver_res = (rot == ESP_LV_ADAPTER_ROTATE_90 || rot == ESP_LV_ADAPTER_ROTATE_270)
                               ? pub->profile.hor_res : pub->profile.ver_res;

    lv_display_t *disp = lv_display_create(hor_res, ver_res);
    if (!disp) {
        return false;
    }

    const lv_color_format_t color_format = lv_display_get_color_format(disp);
    if ((color_format == LV_COLOR_FORMAT_RGB565 &&
         node->caps.pixel_format != ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB565) ||
            (color_format == LV_COLOR_FORMAT_RGB888 &&
             node->caps.pixel_format != ESP_DISPLAY_PRESENT_PIXEL_FORMAT_RGB888) ||
            (color_format != LV_COLOR_FORMAT_RGB565 &&
             color_format != LV_COLOR_FORMAT_RGB888)) {
        lv_display_delete(disp);
        return false;
    }

    const lvgl_present_render_mode_t render_mode = pick_render_mode(&node->caps);
    if (!prepare_draw_buffer(node, render_mode, color_format)) {
        lv_display_delete(disp);
        return false;
    }

    const size_t draw_buf_pixels =
        (render_mode == LVGL_PRESENT_RENDER_FULL)
        ? ((size_t)pub->profile.hor_res * pub->profile.ver_res)
        : ((size_t)pub->profile.hor_res * pub->profile.buffer_height);
    const size_t buf_bytes = calc_draw_buf_bytes(
                                   draw_buf_pixels ? draw_buf_pixels : pub->profile.hor_res,
                                   color_format);

    lv_display_set_buffers(disp,
                           node->cfg.draw_buf_primary,
                           NULL,
                           buf_bytes,
                           render_mode == LVGL_PRESENT_RENDER_FULL
                           ? LV_DISPLAY_RENDER_MODE_FULL
                           : LV_DISPLAY_RENDER_MODE_PARTIAL);

    node->lv_disp = disp;
    node->cfg.lv_disp = disp;
    lv_display_set_user_data(disp, node);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, invalidate_wake_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, refresh_start_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, refresh_finish_cb, LV_EVENT_REFR_READY, NULL);
    return true;
}

static void lvgl_present_destroy_node(esp_lv_adapter_display_node_t *node)
{
    if (!node) {
        return;
    }

    if (node->frame_active) {
        esp_display_presenter_cancel_frame(node->cfg.presenter);
        memset(&node->acquired, 0, sizeof(node->acquired));
    }
    restore_draw_buffer(node);

    if (node->lv_disp) {
        lv_display_set_user_data(node->lv_disp, NULL);
        lv_display_delete(node->lv_disp);
        node->lv_disp = NULL;
    }
    if (node->cfg.draw_buf_primary) {
        free(node->cfg.draw_buf_primary);
        node->cfg.draw_buf_primary = NULL;
    }
    free(node);
}

lv_display_t *lvgl_present_display_register(
    const esp_lv_adapter_display_config_t *cfg,
    const esp_lv_adapter_presenter_config_t *presenter_cfg)
{
    if (!cfg || !presenter_cfg || !presenter_cfg->presenter) {
        return NULL;
    }

    esp_lv_adapter_context_t *ctx = esp_lv_adapter_get_context();
    if (!ctx || !ctx->inited) {
        ESP_LOGE(TAG, "adapter not initialized");
        return NULL;
    }

    esp_lv_adapter_display_node_t *node = calloc(1, sizeof(*node));
    if (!node) {
        ESP_LOGE(TAG, "no memory for display node");
        return NULL;
    }

    node->cfg.base = *cfg;
    node->cfg.presenter = presenter_cfg->presenter;
    node->cfg.producer_generation = presenter_cfg->producer_generation;
    node->cfg.render_alignment = presenter_cfg->render_alignment;
    node->cfg.validate_generation = presenter_cfg->validate_generation;
    node->cfg.validation_user_ctx = presenter_cfg->validation_user_ctx;

    if (!init_display_node(node)) {
        lvgl_present_destroy_node(node);
        return NULL;
    }

    node->next = ctx->display_list;
    ctx->display_list = node;
    return node->lv_disp;
}

esp_err_t lvgl_present_display_unregister(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_ARG, TAG, "invalid display handle");

    esp_lv_adapter_context_t *ctx = esp_lv_adapter_get_context();
    ESP_RETURN_ON_FALSE(ctx && ctx->inited, ESP_ERR_INVALID_STATE, TAG, "adapter not initialized");

    esp_lv_adapter_display_node_t **cursor = &ctx->display_list;
    while (*cursor && (*cursor)->lv_disp != disp) {
        cursor = &(*cursor)->next;
    }
    ESP_RETURN_ON_FALSE(*cursor, ESP_ERR_NOT_FOUND, TAG, "display not found");

    esp_lv_adapter_display_node_t *node = *cursor;
    *cursor = node->next;
    lvgl_present_destroy_node(node);
    return ESP_OK;
}

void lvgl_present_display_clear(void)
{
    esp_lv_adapter_context_t *ctx = esp_lv_adapter_get_context();
    if (!ctx) {
        return;
    }

    esp_lv_adapter_display_node_t *node = ctx->display_list;
    ctx->display_list = NULL;
    while (node) {
        esp_lv_adapter_display_node_t *next = node->next;
        lvgl_present_destroy_node(node);
        node = next;
    }
}

esp_err_t lvgl_present_display_wait_flush_done(lv_display_t *disp, int32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_ARG, TAG, "invalid display handle");
    ESP_RETURN_ON_FALSE(lvgl_present_find_node(disp), ESP_ERR_NOT_FOUND, TAG, "display not found");

    const int64_t start_time = esp_timer_get_time();
    const int64_t timeout_us = (timeout_ms < 0) ? INT64_MAX : ((int64_t)timeout_ms * 1000);

    while (disp->rendering_in_progress) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (esp_timer_get_time() - start_time > timeout_us) {
            ESP_LOGW(TAG, "timeout waiting for flush completion");
            return ESP_ERR_TIMEOUT;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}
