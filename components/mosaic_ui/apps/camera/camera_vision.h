/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CAMERA_VISION_QR_CAPACITY 4U
#define CAMERA_VISION_QR_PAYLOAD_SIZE 256U

typedef enum {
    CAMERA_VISION_MODE_OFF = 0,
    CAMERA_VISION_MODE_QRCODE,
    CAMERA_VISION_MODE_COLOR,
} camera_vision_mode_t;

typedef struct {
    int left;
    int top;
    int right;
    int bottom;
} camera_vision_box_t;

typedef struct {
    camera_vision_box_t box;
    char payload[CAMERA_VISION_QR_PAYLOAD_SIZE];
} camera_vision_qrcode_t;

typedef struct {
    camera_vision_mode_t mode;
    size_t count;
    camera_vision_qrcode_t qrcodes[CAMERA_VISION_QR_CAPACITY];
    camera_vision_box_t color_box;
} camera_vision_result_t;

esp_err_t camera_vision_init(void);
void camera_vision_deinit(void);
esp_err_t camera_vision_detect(camera_vision_mode_t mode, const uint8_t *pixels, uint32_t width, uint32_t height,
                               camera_vision_result_t *out_result);
void camera_vision_draw_result(uint8_t *pixels, uint32_t width, uint32_t height, const camera_vision_result_t *result);
