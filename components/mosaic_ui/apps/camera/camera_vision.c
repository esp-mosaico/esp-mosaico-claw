/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_vision.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vision_core.h"
#include "vision_core_runtime.h"

#define CAMERA_VISION_QR_SIZE 240
#define CAMERA_VISION_COLOR_SIZE 100
#define CAMERA_VISION_BLOB_CAPACITY 32U
#define CAMERA_VISION_RGB565_GREEN 0x07E0U

static const char *TAG = "camera_vision";

static uint8_t *s_gray;
static uint16_t *s_color;
static esp_vision_blob_t *s_blobs;

static int scale_floor(int value, int destination_size, int source_size)
{
    return (int)((int64_t)value * destination_size / source_size);
}

static int scale_ceil(int value, int destination_size, int source_size)
{
    return (int)(((int64_t)value * destination_size + source_size - 1) / source_size);
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) {
        return minimum;
    }
    return value > maximum ? maximum : value;
}

static uint8_t rgb565_gray(uint16_t pixel)
{
    const uint32_t red = ((pixel >> 11) & 0x1FU) * 255U / 31U;
    const uint32_t green = ((pixel >> 5) & 0x3FU) * 255U / 63U;
    const uint32_t blue = (pixel & 0x1FU) * 255U / 31U;
    return (uint8_t)((red * 77U + green * 150U + blue * 29U) >> 8);
}

static void resize_rgb565(const uint16_t *source, uint32_t source_width, uint32_t source_height, uint16_t *destination,
                          uint32_t destination_width, uint32_t destination_height)
{
    for (uint32_t y = 0; y < destination_height; ++y) {
        const uint16_t *source_row = source + (size_t)((uint64_t)y * source_height / destination_height) * source_width;
        uint16_t *destination_row = destination + (size_t)y * destination_width;
        for (uint32_t x = 0; x < destination_width; ++x) {
            destination_row[x] = source_row[(uint64_t)x * source_width / destination_width];
        }
    }
}

static void resize_gray(const uint16_t *source, uint32_t source_width, uint32_t source_height, uint8_t *destination,
                        uint32_t destination_width, uint32_t destination_height)
{
    for (uint32_t y = 0; y < destination_height; ++y) {
        const uint16_t *source_row = source + (size_t)((uint64_t)y * source_height / destination_height) * source_width;
        uint8_t *destination_row = destination + (size_t)y * destination_width;
        for (uint32_t x = 0; x < destination_width; ++x) {
            destination_row[x] = rgb565_gray(source_row[(uint64_t)x * source_width / destination_width]);
        }
    }
}

esp_err_t camera_vision_init(void)
{
    if (s_gray != NULL && s_color != NULL && s_blobs != NULL) {
        return ESP_OK;
    }
    camera_vision_deinit();
    s_gray = heap_caps_malloc(CAMERA_VISION_QR_SIZE * CAMERA_VISION_QR_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_gray == NULL) {
        s_gray = heap_caps_malloc(CAMERA_VISION_QR_SIZE * CAMERA_VISION_QR_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_color = heap_caps_aligned_alloc(16, CAMERA_VISION_COLOR_SIZE * CAMERA_VISION_COLOR_SIZE * sizeof(*s_color),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_color == NULL) {
        s_color = heap_caps_aligned_alloc(16, CAMERA_VISION_COLOR_SIZE * CAMERA_VISION_COLOR_SIZE * sizeof(*s_color),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_blobs = heap_caps_calloc(CAMERA_VISION_BLOB_CAPACITY, sizeof(*s_blobs), MALLOC_CAP_8BIT);
    if (s_gray == NULL || s_color == NULL || s_blobs == NULL) {
        ESP_LOGE(TAG, "failed to allocate recognition buffers");
        camera_vision_deinit();
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = lua_vision_core_runtime_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize vision runtime: %s", esp_err_to_name(err));
        camera_vision_deinit();
    }
    return err;
}

void camera_vision_deinit(void)
{
    heap_caps_free(s_gray);
    s_gray = NULL;
    heap_caps_free(s_color);
    s_color = NULL;
    heap_caps_free(s_blobs);
    s_blobs = NULL;
}

static esp_err_t detect_qrcodes(const uint16_t *pixels, uint32_t width, uint32_t height, camera_vision_result_t *result)
{
    esp_vision_qrcode_t codes[CAMERA_VISION_QR_CAPACITY] = {0};
    for (size_t i = 0; i < CAMERA_VISION_QR_CAPACITY; ++i) {
        codes[i].payload = result->qrcodes[i].payload;
        codes[i].payload_size = sizeof(result->qrcodes[i].payload);
    }
    resize_gray(pixels, width, height, s_gray, CAMERA_VISION_QR_SIZE, CAMERA_VISION_QR_SIZE);
    esp_vision_image_t image = {
        .width = CAMERA_VISION_QR_SIZE,
        .height = CAMERA_VISION_QR_SIZE,
        .pixformat = ESP_VISION_PIXFORMAT_GRAYSCALE,
        .data = s_gray,
        .size = CAMERA_VISION_QR_SIZE * CAMERA_VISION_QR_SIZE,
    };
    esp_vision_rect_t roi = {.x = 0, .y = 0, .w = CAMERA_VISION_QR_SIZE, .h = CAMERA_VISION_QR_SIZE};
    size_t total = 0;
    esp_err_t err = esp_vision_image_find_qrcodes(&image, &roi, codes, CAMERA_VISION_QR_CAPACITY, &total);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        return err;
    }
    const size_t count = total < CAMERA_VISION_QR_CAPACITY ? total : CAMERA_VISION_QR_CAPACITY;
    for (size_t i = 0; i < count; ++i) {
        const esp_vision_rect_t *rect = &codes[i].rect;
        if (rect->x < 0 || rect->y < 0 || rect->w <= 0 || rect->h <= 0 || rect->x + rect->w > CAMERA_VISION_QR_SIZE ||
            rect->y + rect->h > CAMERA_VISION_QR_SIZE) {
            ESP_LOGW(TAG, "invalid QR rectangle skipped");
            continue;
        }
        camera_vision_qrcode_t *output = &result->qrcodes[result->count++];
        if (output != &result->qrcodes[i]) {
            memcpy(output->payload, result->qrcodes[i].payload, sizeof(output->payload));
        }
        output->box = (camera_vision_box_t) {
            .left = scale_floor(rect->x, (int)width, CAMERA_VISION_QR_SIZE),
            .top = scale_floor(rect->y, (int)height, CAMERA_VISION_QR_SIZE),
            .right = scale_ceil(rect->x + rect->w, (int)width, CAMERA_VISION_QR_SIZE) - 1,
            .bottom = scale_ceil(rect->y + rect->h, (int)height, CAMERA_VISION_QR_SIZE) - 1,
        };
    }
    return ESP_OK;
}

static esp_err_t detect_color(const uint16_t *pixels, uint32_t width, uint32_t height, camera_vision_result_t *result)
{
    static const esp_vision_color_threshold_t green = {
        .l_min = 35, .l_max = 97, .a_min = -88, .a_max = -12, .b_min = -3, .b_max = 92,
    };
    resize_rgb565(pixels, width, height, s_color, CAMERA_VISION_COLOR_SIZE, CAMERA_VISION_COLOR_SIZE);
    esp_vision_image_t image = {
        .width = CAMERA_VISION_COLOR_SIZE,
        .height = CAMERA_VISION_COLOR_SIZE,
        .pixformat = ESP_VISION_PIXFORMAT_RGB565,
        .data = (uint8_t *)s_color,
        .size = CAMERA_VISION_COLOR_SIZE * CAMERA_VISION_COLOR_SIZE * sizeof(*s_color),
    };
    esp_vision_rect_t roi = {.x = 0, .y = 0, .w = CAMERA_VISION_COLOR_SIZE, .h = CAMERA_VISION_COLOR_SIZE};
    const esp_vision_find_blobs_config_t config = {
        .thresholds = &green, .threshold_count = 1, .x_stride = 2, .y_stride = 2,
        .area_threshold = 11, .pixels_threshold = 11, .merge = true,
    };
    size_t total = 0;
    esp_err_t err = esp_vision_image_find_blobs(&image, &roi, &config, s_blobs, CAMERA_VISION_BLOB_CAPACITY, &total);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {
        return err;
    }
    const size_t count = total < CAMERA_VISION_BLOB_CAPACITY ? total : CAMERA_VISION_BLOB_CAPACITY;
    int best_area = 0;
    for (size_t i = 0; i < count; ++i) {
        const esp_vision_rect_t *rect = &s_blobs[i].rect;
        if (rect->x < 0 || rect->y < 0 || rect->w <= 0 || rect->h <= 0 || rect->x + rect->w > CAMERA_VISION_COLOR_SIZE ||
            rect->y + rect->h > CAMERA_VISION_COLOR_SIZE) {
            continue;
        }
        const int area = scale_ceil(rect->w, (int)width, CAMERA_VISION_COLOR_SIZE) *
                         scale_ceil(rect->h, (int)height, CAMERA_VISION_COLOR_SIZE);
        if (area <= best_area || area > (int)((uint64_t)width * height * 35U / 100U)) {
            continue;
        }
        result->color_box = (camera_vision_box_t) {
            .left = scale_floor(rect->x, (int)width, CAMERA_VISION_COLOR_SIZE),
            .top = scale_floor(rect->y, (int)height, CAMERA_VISION_COLOR_SIZE),
            .right = scale_ceil(rect->x + rect->w, (int)width, CAMERA_VISION_COLOR_SIZE) - 1,
            .bottom = scale_ceil(rect->y + rect->h, (int)height, CAMERA_VISION_COLOR_SIZE) - 1,
        };
        best_area = area;
    }
    result->count = best_area > 0 ? 1U : 0U;
    return ESP_OK;
}

esp_err_t camera_vision_detect(camera_vision_mode_t mode, const uint8_t *pixels, uint32_t width, uint32_t height,
                               camera_vision_result_t *out_result)
{
    if (pixels == NULL || out_result == NULL || width == 0 || height == 0 || mode == CAMERA_VISION_MODE_OFF) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_result, 0, sizeof(*out_result));
    out_result->mode = mode;
    esp_err_t err = lua_vision_core_lock();
    if (err != ESP_OK) {
        return err;
    }
    if (mode == CAMERA_VISION_MODE_QRCODE) {
        err = detect_qrcodes((const uint16_t *)pixels, width, height, out_result);
    } else if (mode == CAMERA_VISION_MODE_COLOR) {
        err = detect_color((const uint16_t *)pixels, width, height, out_result);
    } else {
        err = ESP_ERR_INVALID_ARG;
    }
    lua_vision_core_unlock();
    return err;
}

static void draw_rectangle(uint16_t *pixels, uint32_t width, uint32_t height, const camera_vision_box_t *box)
{
    const int left = clamp_int(box->left, 0, (int)width - 1);
    const int top = clamp_int(box->top, 0, (int)height - 1);
    const int right = clamp_int(box->right, left, (int)width - 1);
    const int bottom = clamp_int(box->bottom, top, (int)height - 1);
    for (int offset = 0; offset < 3; ++offset) {
        const int x1 = clamp_int(left + offset, 0, (int)width - 1);
        const int y1 = clamp_int(top + offset, 0, (int)height - 1);
        const int x2 = clamp_int(right - offset, x1, (int)width - 1);
        const int y2 = clamp_int(bottom - offset, y1, (int)height - 1);
        for (int x = x1; x <= x2; ++x) {
            pixels[(size_t)y1 * width + x] = CAMERA_VISION_RGB565_GREEN;
            pixels[(size_t)y2 * width + x] = CAMERA_VISION_RGB565_GREEN;
        }
        for (int y = y1; y <= y2; ++y) {
            pixels[(size_t)y * width + x1] = CAMERA_VISION_RGB565_GREEN;
            pixels[(size_t)y * width + x2] = CAMERA_VISION_RGB565_GREEN;
        }
    }
}

void camera_vision_draw_result(uint8_t *pixels, uint32_t width, uint32_t height, const camera_vision_result_t *result)
{
    if (pixels == NULL || result == NULL || width == 0 || height == 0 || result->count == 0) {
        return;
    }
    uint16_t *rgb565 = (uint16_t *)pixels;
    if (result->mode == CAMERA_VISION_MODE_QRCODE) {
        for (size_t i = 0; i < result->count && i < CAMERA_VISION_QR_CAPACITY; ++i) {
            draw_rectangle(rgb565, width, height, &result->qrcodes[i].box);
        }
    } else if (result->mode == CAMERA_VISION_MODE_COLOR) {
        draw_rectangle(rgb565, width, height, &result->color_box);
        const int center_x = (result->color_box.left + result->color_box.right) / 2;
        const int center_y = (result->color_box.top + result->color_box.bottom) / 2;
        for (int offset = -6; offset <= 6; ++offset) {
            const int x = clamp_int(center_x + offset, 0, (int)width - 1);
            const int y = clamp_int(center_y + offset, 0, (int)height - 1);
            rgb565[(size_t)center_y * width + x] = CAMERA_VISION_RGB565_GREEN;
            rgb565[(size_t)y * width + center_x] = CAMERA_VISION_RGB565_GREEN;
        }
    }
}
