/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_camera.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_binds.h"
#include "camera_vision.h"
#if defined(ESP_PLATFORM)
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_camera.h"
#endif

#define MOSAIC_CAMERA_W 480U
#define MOSAIC_CAMERA_H 480U
#define MOSAIC_CAMERA_STRIDE (MOSAIC_CAMERA_W * 2U)
#define MOSAIC_CAMERA_FRAME_BYTES ((size_t)MOSAIC_CAMERA_STRIDE * MOSAIC_CAMERA_H)
#define MOSAIC_CAMERA_BUFFERS 2U
#define MOSAIC_CAMERA_PPA_SCALE_STEPS 16U
#define MOSAIC_CAMERA_OUTPUT_W 450U
#define MOSAIC_CAMERA_OUTPUT_X ((MOSAIC_CAMERA_W - MOSAIC_CAMERA_OUTPUT_W) / 2U)

#if defined(ESP_PLATFORM)

#define MOSAIC_CAMERA_BUFFER_ALIGN 64U
#define MOSAIC_CAMERA_STOP_TIMEOUT_MS 35000
#define MOSAIC_CAMERA_TASK_STACK 6144
#define MOSAIC_CAMERA_TASK_PRIORITY 3
#define MOSAIC_CAMERA_SKIP_FRAMES 5U
#define MOSAIC_CAMERA_FRAME_DELAY_MS 5U
#define MOSAIC_CAMERA_CAPTURE_PATH_MAX 256U
#define MOSAIC_CAMERA_JPEG_QUALITY 90U
#define MOSAIC_CAMERA_JPEG_TIMEOUT_MS 1000
#define MOSAIC_CAMERA_FLASH_WAIT_FRAMES 10U
#define MOSAIC_CAMERA_CAPTURE_FREEZE_MS 300
#define MOSAIC_CAMERA_QR_INTERVAL_US 100000
#define MOSAIC_CAMERA_COLOR_INTERVAL_US 50000

static const char *TAG = "mosaic_camera";

typedef struct {
    uint16_t input_width;
    uint16_t input_height;
    uint16_t crop_width;
    uint16_t crop_height;
    uint8_t scale;
} camera_preview_profile_t;

static const camera_preview_profile_t s_preview_profiles[] = {
    {1280, 720, 768, 720, 10},
    {640, 480, 512, 480, 15},
};

typedef enum {
    MOSAIC_FRAME_FREE = 0,
    MOSAIC_FRAME_WRITING,
    MOSAIC_FRAME_READY,
    MOSAIC_FRAME_IN_FLIGHT,
} mosaic_frame_state_t;

typedef struct {
    uint8_t *pixels;
    mosaic_frame_state_t state;
    uint32_t sequence;
} mosaic_frame_slot_t;

typedef struct {
    portMUX_TYPE lock;
    mosaic_frame_slot_t frames[MOSAIC_CAMERA_BUFFERS];
    SemaphoreHandle_t task_done;
    SemaphoreHandle_t submit_lock;
    TaskHandle_t task;
    uint32_t sequence;
    int64_t preview_resume_time_us;
    char capture_path[MOSAIC_CAMERA_CAPTURE_PATH_MAX];
    bool started;
    bool stopping;
    bool capture_pending;
    bool capture_use_flash;
    bool flash_enabled;
    bool mirror_x;
    camera_vision_mode_t recognition_mode;
} mosaic_camera_state_t;

static mosaic_camera_state_t s_camera = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static bool camera_is_stopping(void)
{
    bool stopping;
    portENTER_CRITICAL(&s_camera.lock);
    stopping = s_camera.stopping;
    portEXIT_CRITICAL(&s_camera.lock);
    return stopping;
}

static bool camera_board_present(void)
{
    return mosaico_camera_is_available();
}

static void camera_set_missing_hint(esp_gsp_handle_t ui, bool visible)
{
    if (ui == NULL) {
        return;
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_MISSING_VISIBLE, visible);
}

static uint8_t *camera_alloc_preview_pixels(void)
{
    uint8_t *pixels = heap_caps_aligned_calloc(
        MOSAIC_CAMERA_BUFFER_ALIGN, 1, MOSAIC_CAMERA_FRAME_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        pixels = heap_caps_aligned_calloc(
            MOSAIC_CAMERA_BUFFER_ALIGN, 1, MOSAIC_CAMERA_FRAME_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    return pixels;
}

static void camera_clear_preview_borders(uint8_t *pixels)
{
    if (pixels == NULL) {
        return;
    }
    const size_t left_bytes = MOSAIC_CAMERA_OUTPUT_X * sizeof(uint16_t);
    const size_t right_offset = (MOSAIC_CAMERA_OUTPUT_X + MOSAIC_CAMERA_OUTPUT_W) * sizeof(uint16_t);
    const size_t right_bytes = MOSAIC_CAMERA_STRIDE - right_offset;
    for (uint32_t y = 0; y < MOSAIC_CAMERA_H; ++y) {
        uint8_t *row = pixels + (size_t)y * MOSAIC_CAMERA_STRIDE;
        memset(row, 0, left_bytes);
        memset(row + right_offset, 0, right_bytes);
    }
}

static esp_err_t camera_ensure_preview_buffers(size_t count)
{
    if (count > MOSAIC_CAMERA_BUFFERS) {
        count = MOSAIC_CAMERA_BUFFERS;
    }
    for (size_t i = 0; i < count; ++i) {
        if (s_camera.frames[i].pixels != NULL) {
            continue;
        }
        uint8_t *pixels = camera_alloc_preview_pixels();
        if (pixels == NULL) {
            ESP_LOGE(TAG, "failed to allocate preview frame %u", (unsigned)i);
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_camera.lock);
        if (s_camera.frames[i].pixels == NULL) {
            s_camera.frames[i].pixels = pixels;
            s_camera.frames[i].state = MOSAIC_FRAME_FREE;
            s_camera.frames[i].sequence = 0;
            pixels = NULL;
        }
        portEXIT_CRITICAL(&s_camera.lock);
        heap_caps_free(pixels);
    }
    return ESP_OK;
}

static void frame_released(void *ctx)
{
    const size_t slot = (size_t)(uintptr_t)ctx;
    if (slot >= MOSAIC_CAMERA_BUFFERS) {
        return;
    }

    portENTER_CRITICAL(&s_camera.lock);
    if (s_camera.frames[slot].state == MOSAIC_FRAME_IN_FLIGHT) {
        s_camera.frames[slot].state = MOSAIC_FRAME_FREE;
    }
    portEXIT_CRITICAL(&s_camera.lock);
}

static int frame_acquire_for_write(void)
{
    int slot = -1;
    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.stopping) {
        for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
            if (s_camera.frames[i].pixels != NULL &&
                    s_camera.frames[i].state == MOSAIC_FRAME_FREE) {
                s_camera.frames[i].state = MOSAIC_FRAME_WRITING;
                slot = (int)i;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_camera.lock);
    return slot;
}

static void frame_finish_write(size_t slot, bool ready)
{
    portENTER_CRITICAL(&s_camera.lock);
    if (slot < MOSAIC_CAMERA_BUFFERS &&
            s_camera.frames[slot].state == MOSAIC_FRAME_WRITING) {
        if (ready && !s_camera.stopping) {
            s_camera.frames[slot].sequence = ++s_camera.sequence;
            s_camera.frames[slot].state = MOSAIC_FRAME_READY;
        } else {
            s_camera.frames[slot].state = MOSAIC_FRAME_FREE;
        }
    }
    portEXIT_CRITICAL(&s_camera.lock);
}

static ppa_srm_color_mode_t camera_ppa_color_mode(uint32_t pixel_format)
{
    switch (pixel_format) {
    case V4L2_PIX_FMT_UYVY:
        return PPA_SRM_COLOR_MODE_YUV422_UYVY;
    case V4L2_PIX_FMT_YUYV:
        return PPA_SRM_COLOR_MODE_YUV422_YUYV;
    case V4L2_PIX_FMT_RGB565:
        return PPA_SRM_COLOR_MODE_RGB565;
    default:
        return (ppa_srm_color_mode_t)-1;
    }
}

static const camera_preview_profile_t *camera_preview_profile(uint32_t width, uint32_t height)
{
    for (size_t i = 0; i < sizeof(s_preview_profiles) / sizeof(s_preview_profiles[0]); ++i) {
        if (s_preview_profiles[i].input_width == width && s_preview_profiles[i].input_height == height) {
            return &s_preview_profiles[i];
        }
    }
    return NULL;
}

static esp_err_t camera_convert_frame(
    ppa_client_handle_t ppa,
    const mosaico_camera_frame_t *frame,
    uint8_t *output,
    bool mirror_y)
{
    if (ppa == NULL || frame == NULL || frame->data == NULL || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const camera_preview_profile_t *profile = camera_preview_profile(frame->width, frame->height);
    if (profile == NULL) {
        ESP_LOGE(TAG, "unsupported camera resolution: %ux%u", (unsigned)frame->width, (unsigned)frame->height);
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint64_t expected_bytes =
        (uint64_t)frame->width * (uint64_t)frame->height * 2U;
    if (expected_bytes > frame->size) {
        ESP_LOGE(TAG, "camera frame truncated: got=%u expected=%llu",
                 (unsigned)frame->size, (unsigned long long)expected_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    const ppa_srm_color_mode_t input_mode =
        camera_ppa_color_mode(frame->pixel_format);
    if ((int)input_mode < 0) {
        ESP_LOGE(TAG, "unsupported camera pixel format: 0x%08x",
                 (unsigned)frame->pixel_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Keep both profiles on the same 450x480 preview area after rotation. */
    const uint32_t source_x = ((frame->width - profile->crop_width) / 2U) & ~1U;
    const uint32_t source_y = (frame->height - profile->crop_height) / 2U;
    const float scale = (float)profile->scale / MOSAIC_CAMERA_PPA_SCALE_STEPS;

    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = frame->data,
            .pic_w = frame->width,
            .pic_h = frame->height,
            .block_w = profile->crop_width,
            .block_h = profile->crop_height,
            .block_offset_x = source_x,
            .block_offset_y = source_y,
            .srm_cm = input_mode,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = output,
            .buffer_size = MOSAIC_CAMERA_FRAME_BYTES,
            .pic_w = MOSAIC_CAMERA_W,
            .pic_h = MOSAIC_CAMERA_H,
            .block_offset_x = MOSAIC_CAMERA_OUTPUT_X,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = scale,
        .scale_y = scale,
        .mirror_x = mirror_y,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(ppa, &operation);
}

static bool camera_take_capture_request(
    char *path, size_t path_size, bool *use_flash)
{
    bool pending = false;
    portENTER_CRITICAL(&s_camera.lock);
    if (s_camera.capture_pending) {
        const size_t copy_size =
            path_size < sizeof(s_camera.capture_path)
                ? path_size : sizeof(s_camera.capture_path);
        memcpy(path, s_camera.capture_path, copy_size);
        path[copy_size - 1U] = '\0';
        *use_flash = s_camera.capture_use_flash;
        s_camera.capture_pending = false;
        s_camera.capture_use_flash = false;
        s_camera.capture_path[0] = '\0';
        pending = true;
    }
    portEXIT_CRITICAL(&s_camera.lock);
    return pending;
}

static bool camera_mirror_x_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_camera.lock);
    enabled = s_camera.mirror_x;
    portEXIT_CRITICAL(&s_camera.lock);
    return enabled;
}

static bool camera_flash_enabled(void)
{
    bool enabled;
    portENTER_CRITICAL(&s_camera.lock);
    enabled = s_camera.flash_enabled;
    portEXIT_CRITICAL(&s_camera.lock);
    return enabled;
}

static camera_vision_mode_t camera_recognition_mode(void)
{
    camera_vision_mode_t mode;
    portENTER_CRITICAL(&s_camera.lock);
    mode = s_camera.recognition_mode;
    portEXIT_CRITICAL(&s_camera.lock);
    return mode;
}

static void camera_update_recognition_result(esp_gsp_handle_t ui, const camera_vision_result_t *result)
{
    if (ui == NULL || result == NULL) {
        return;
    }
    const bool detected = result->count > 0;
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_RECOGNITION_RESULT_VISIBLE, detected);
    if (!detected) {
        return;
    }
    if (result->mode == CAMERA_VISION_MODE_QRCODE) {
        (void)esp_gsp_set_text(ui, GSP_BIND_CAMERA_RECOGNITION_RESULT_TEXT,
                               result->qrcodes[0].payload[0] != '\0' ? result->qrcodes[0].payload : "QR code detected");
    } else {
        (void)esp_gsp_set_text(ui, GSP_BIND_CAMERA_RECOGNITION_RESULT_TEXT, "Green color detected");
    }
}

static void camera_stop_registered_stream(void)
{
    mosaico_camera_handle_t camera = NULL;
    esp_err_t err = mosaico_camera_get_default(&camera);
    if (err == ESP_OK) {
        err = mosaico_camera_close(camera);
    }
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND &&
            err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to close camera: %s",
                 esp_err_to_name(err));
    }
}

static esp_err_t camera_save_jpeg(const char *path, const uint8_t *pixels)
{
    const jpeg_encode_memory_alloc_cfg_t memory = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_capacity = 0;
    uint8_t *output = jpeg_alloc_encoder_mem(
        MOSAIC_CAMERA_FRAME_BYTES, &memory, &output_capacity);
    if (output == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const jpeg_encode_engine_cfg_t engine_config = {
        .intr_priority = 0,
        .timeout_ms = MOSAIC_CAMERA_JPEG_TIMEOUT_MS,
    };
    jpeg_encoder_handle_t encoder = NULL;
    esp_err_t ret = jpeg_new_encoder_engine(&engine_config, &encoder);
    uint32_t output_size = 0;
    if (ret == ESP_OK) {
        const jpeg_encode_cfg_t encode_config = {
            .height = MOSAIC_CAMERA_H,
            .width = MOSAIC_CAMERA_W,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = MOSAIC_CAMERA_JPEG_QUALITY,
            .pixel_reverse = false,
        };
        ret = jpeg_encoder_process(
            encoder, &encode_config, pixels, MOSAIC_CAMERA_FRAME_BYTES,
            output, output_capacity, &output_size);
    }

    FILE *file = NULL;
    if (ret == ESP_OK) {
        file = fopen(path, "wb");
        if (file == NULL) {
            ret = ESP_FAIL;
        } else if (fwrite(output, 1, output_size, file) != output_size) {
            ret = ESP_FAIL;
        }
    }
    if (file != NULL && fclose(file) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (encoder != NULL) {
        const esp_err_t delete_err = jpeg_del_encoder_engine(encoder);
        if (ret == ESP_OK) {
            ret = delete_err;
        }
    }
    free(output);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Photo saved: %s (%" PRIu32 " bytes)",
                 path, output_size);
    } else {
        ESP_LOGE(TAG, "Save photo failed: %s (%s)",
                 path, esp_err_to_name(ret));
    }
    return ret;
}

static void camera_restore_after_flash(
    mosaico_camera_handle_t camera,
    const mosaico_camera_flash_state_t *state)
{
    const esp_err_t err =
        mosaico_camera_restore_flash_capture(camera, state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to restore camera after flash: %s",
                 esp_err_to_name(err));
    }
}

static void camera_capture_task(void *ctx)
{
    esp_gsp_handle_t ui = ctx;
    ppa_client_handle_t ppa = NULL;
    mosaico_camera_handle_t last_camera = NULL;
    uint32_t skip_frames = 0;
    bool waiting_logged = false;
    bool detection_error_logged = false;
    camera_vision_mode_t previous_mode = CAMERA_VISION_MODE_OFF;
    camera_vision_result_t recognition = {0};
    int64_t last_detection_us = 0;

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&ppa_config, &ppa);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register PPA client: %s", esp_err_to_name(err));
        goto done;
    }

    while (!camera_is_stopping()) {
        mosaico_camera_handle_t camera = NULL;
        err = mosaico_camera_get_default(&camera);
        if (err != ESP_OK) {
            if (!waiting_logged && !camera_is_stopping()) {
                ESP_LOGI(TAG, "Waiting for CameraBoard insertion");
                camera_set_missing_hint(ui, true);
                waiting_logged = true;
            }
            last_camera = NULL;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (waiting_logged) {
            camera_set_missing_hint(ui, false);
            waiting_logged = false;
        }

        if (camera != last_camera) {
            err = mosaico_camera_open(camera);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to open camera device: %s",
                         esp_err_to_name(err));
                last_camera = NULL;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            err = camera_ensure_preview_buffers(MOSAIC_CAMERA_BUFFERS);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to allocate preview buffers: %s", esp_err_to_name(err));
                last_camera = NULL;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            err = mosaico_camera_start_stream(camera);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "failed to start camera stream: %s",
                         esp_err_to_name(err));
                last_camera = NULL;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (camera_flash_enabled()) {
                err = mosaico_camera_flash_trigger(camera);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "failed to enable continuous flash: %s", esp_err_to_name(err));
                }
            }
            mosaico_camera_info_t info = {0};
            if (mosaico_camera_get_info(camera, &info) == ESP_OK) {
                ESP_LOGI(TAG,
                         "Preview camera ready: size=%ux%u format=0x%08x",
                         (unsigned)info.width, (unsigned)info.height,
                         (unsigned)info.pixel_format);
            }
            last_camera = camera;
            skip_frames = MOSAIC_CAMERA_SKIP_FRAMES;
        }

        if (skip_frames > 0) {
            mosaico_camera_frame_t frame = {0};
            err = mosaico_camera_get_frame(camera, &frame);
            if (err != ESP_OK) {
                if (err != ESP_ERR_TIMEOUT && !camera_is_stopping()) {
                    ESP_LOGE(TAG, "camera warm-up failed: %s",
                             esp_err_to_name(err));
                    last_camera = NULL;
                }
                continue;
            }
            const esp_err_t release_err =
                mosaico_camera_return_frame(camera, &frame);
            if (release_err != ESP_OK) {
                ESP_LOGE(TAG, "camera warm-up frame release failed: %s",
                         esp_err_to_name(release_err));
                last_camera = NULL;
                continue;
            }
            skip_frames--;
            continue;
        }

        const int slot = frame_acquire_for_write();
        if (slot < 0) {
            vTaskDelay(pdMS_TO_TICKS(3));
            continue;
        }

        char capture_path[MOSAIC_CAMERA_CAPTURE_PATH_MAX] = {0};
        bool use_flash = false;
        const bool capture_requested = camera_take_capture_request(capture_path, sizeof(capture_path), &use_flash);
        mosaico_camera_flash_state_t flash_state = {0};
        if (capture_requested && use_flash) {
            err = mosaico_camera_prepare_flash_capture(
                camera, &flash_state);
            if (err == ESP_OK) {
                err = mosaico_camera_flash_trigger(camera);
            }
            if (err == ESP_OK) {
                err = mosaico_camera_discard_frames(
                    camera, MOSAIC_CAMERA_FLASH_WAIT_FRAMES);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "prepare flash capture failed: %s",
                         esp_err_to_name(err));
                camera_restore_after_flash(camera, &flash_state);
                frame_finish_write((size_t)slot, false);
                continue;
            }
        }

        mosaico_camera_frame_t frame = {0};
        err = mosaico_camera_get_frame(camera, &frame);
        if (err != ESP_OK) {
            if (capture_requested && use_flash) {
                (void)mosaico_camera_flash_stop(camera);
                camera_restore_after_flash(camera, &flash_state);
            }
            frame_finish_write((size_t)slot, false);
            if (err != ESP_ERR_TIMEOUT && !camera_is_stopping()) {
                ESP_LOGE(TAG, "camera capture failed: %s",
                         esp_err_to_name(err));
                last_camera = NULL;
            }
            continue;
        }

        err = camera_convert_frame(
            ppa, &frame, s_camera.frames[slot].pixels,
            camera_mirror_x_enabled());
        if (err == ESP_OK) {
            camera_clear_preview_borders(s_camera.frames[slot].pixels);
        }
        const esp_err_t release_err =
            mosaico_camera_return_frame(camera, &frame);
        if (release_err != ESP_OK) {
            ESP_LOGE(TAG, "camera frame release failed: %s",
                     esp_err_to_name(release_err));
        }
        if (err == ESP_OK && capture_requested) {
            (void)camera_save_jpeg(
                capture_path, s_camera.frames[slot].pixels);
        }
        if (capture_requested && use_flash) {
            (void)mosaico_camera_flash_stop(camera);
            camera_restore_after_flash(camera, &flash_state);
        }
        const camera_vision_mode_t mode = camera_recognition_mode();
        if (mode != previous_mode) {
            memset(&recognition, 0, sizeof(recognition));
            recognition.mode = mode;
            previous_mode = mode;
            last_detection_us = 0;
            detection_error_logged = false;
        }
        const int64_t now_us = esp_timer_get_time();
        const int64_t interval_us = mode == CAMERA_VISION_MODE_QRCODE ? MOSAIC_CAMERA_QR_INTERVAL_US : MOSAIC_CAMERA_COLOR_INTERVAL_US;
        if (err == ESP_OK && mode != CAMERA_VISION_MODE_OFF && (last_detection_us == 0 || now_us - last_detection_us >= interval_us)) {
            last_detection_us = now_us;
            const esp_err_t detect_err = camera_vision_detect(mode, s_camera.frames[slot].pixels, MOSAIC_CAMERA_W, MOSAIC_CAMERA_H,
                                                              &recognition);
            if (detect_err == ESP_OK && camera_recognition_mode() == mode) {
                detection_error_logged = false;
                camera_update_recognition_result(ui, &recognition);
            } else if (detect_err != ESP_OK && !detection_error_logged) {
                ESP_LOGE(TAG, "recognition failed: %s", esp_err_to_name(detect_err));
                detection_error_logged = true;
            }
        }
        if (err == ESP_OK && mode != CAMERA_VISION_MODE_OFF && camera_recognition_mode() == mode) {
            camera_vision_draw_result(s_camera.frames[slot].pixels, MOSAIC_CAMERA_W, MOSAIC_CAMERA_H, &recognition);
        }
        frame_finish_write((size_t)slot, err == ESP_OK);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "camera frame conversion failed: %s",
                     esp_err_to_name(err));
            break;
        }
        mosaic_camera_tick(ui);
        vTaskDelay(pdMS_TO_TICKS(MOSAIC_CAMERA_FRAME_DELAY_MS));
    }

done:
    camera_stop_registered_stream();
    if (ppa != NULL) {
        const esp_err_t ppa_err = ppa_unregister_client(ppa);
        if (ppa_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to unregister PPA client: %s",
                     esp_err_to_name(ppa_err));
        }
    }
    if (s_camera.task_done != NULL) {
        xSemaphoreGive(s_camera.task_done);
    }
    vTaskDelete(NULL);
}

esp_err_t mosaic_camera_start(esp_gsp_handle_t ui)
{
    portENTER_CRITICAL(&s_camera.lock);
    if (s_camera.started) {
        portEXIT_CRITICAL(&s_camera.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_camera.started = true;
    s_camera.stopping = false;
    s_camera.sequence = 0;
    s_camera.preview_resume_time_us = 0;
    s_camera.capture_pending = false;
    s_camera.capture_use_flash = false;
    s_camera.flash_enabled = false;
    s_camera.capture_path[0] = '\0';
    s_camera.mirror_x = false;
    s_camera.recognition_mode = CAMERA_VISION_MODE_OFF;
    portEXIT_CRITICAL(&s_camera.lock);

    camera_set_missing_hint(ui, !camera_board_present());

    s_camera.task_done = xSemaphoreCreateBinary();
    s_camera.submit_lock = xSemaphoreCreateMutex();
    if (s_camera.task_done == NULL || s_camera.submit_lock == NULL) {
        ESP_LOGE(TAG, "failed to create camera synchronization objects");
        goto fail;
    }

    if (xTaskCreatePinnedToCore(camera_capture_task, "mosaic_cam",
                    MOSAIC_CAMERA_TASK_STACK, ui,
                    MOSAIC_CAMERA_TASK_PRIORITY, &s_camera.task, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create camera capture task");
        goto fail;
    }
    return ESP_OK;

fail:
    if (s_camera.submit_lock != NULL) {
        vSemaphoreDelete(s_camera.submit_lock);
        s_camera.submit_lock = NULL;
    }
    if (s_camera.task_done != NULL) {
        vSemaphoreDelete(s_camera.task_done);
        s_camera.task_done = NULL;
    }
    for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
        heap_caps_free(s_camera.frames[i].pixels);
        s_camera.frames[i].pixels = NULL;
        s_camera.frames[i].state = MOSAIC_FRAME_FREE;
    }
    portENTER_CRITICAL(&s_camera.lock);
    s_camera.started = false;
    s_camera.stopping = false;
    portEXIT_CRITICAL(&s_camera.lock);
    return ESP_ERR_NO_MEM;
}

esp_err_t mosaic_camera_capture_photo(const char *path, bool use_flash)
{
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t path_length =
        strnlen(path, MOSAIC_CAMERA_CAPTURE_PATH_MAX);
    if (path_length >= MOSAIC_CAMERA_CAPTURE_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int64_t preview_resume_time_us = esp_timer_get_time() + (int64_t)MOSAIC_CAMERA_CAPTURE_FREEZE_MS * 1000;
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.started || s_camera.stopping ||
            s_camera.capture_pending) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        memcpy(s_camera.capture_path, path, path_length + 1U);
        s_camera.capture_use_flash = use_flash;
        s_camera.capture_pending = true;
        s_camera.preview_resume_time_us = preview_resume_time_us;
    }
    portEXIT_CRITICAL(&s_camera.lock);
    return ret;
}

esp_err_t mosaic_camera_set_flash_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.started || s_camera.stopping) {
        portEXIT_CRITICAL(&s_camera.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_camera.flash_enabled = enabled;
    portEXIT_CRITICAL(&s_camera.lock);

    mosaico_camera_handle_t camera = NULL;
    esp_err_t ret = mosaico_camera_get_default(&camera);
    if (ret != ESP_OK) {
        return ret;
    }
    return enabled ? mosaico_camera_flash_trigger(camera) : mosaico_camera_flash_stop(camera);
}

esp_err_t mosaic_camera_toggle_flip(bool *out_enabled)
{
    esp_err_t ret = ESP_OK;
    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.started || s_camera.stopping) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        s_camera.mirror_x = !s_camera.mirror_x;
        if (out_enabled != NULL) {
            *out_enabled = s_camera.mirror_x;
        }
    }
    portEXIT_CRITICAL(&s_camera.lock);
    return ret;
}

esp_err_t mosaic_camera_set_recognition_mode(camera_vision_mode_t mode)
{
    if (mode != CAMERA_VISION_MODE_OFF && mode != CAMERA_VISION_MODE_QRCODE && mode != CAMERA_VISION_MODE_COLOR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode != CAMERA_VISION_MODE_OFF) {
        const esp_err_t err = camera_vision_init();
        if (err != ESP_OK) {
            return err;
        }
    }
    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.started || s_camera.stopping) {
        portEXIT_CRITICAL(&s_camera.lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_camera.recognition_mode = mode;
    portEXIT_CRITICAL(&s_camera.lock);
    return ESP_OK;
}

void mosaic_camera_tick(esp_gsp_handle_t ui)
{
    size_t selected = MOSAIC_CAMERA_BUFFERS;
    uint32_t newest_sequence = 0;
    const int64_t now_us = esp_timer_get_time();
    SemaphoreHandle_t submit_lock = s_camera.submit_lock;
    if (submit_lock == NULL ||
            xSemaphoreTake(submit_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.started || s_camera.stopping) {
        portEXIT_CRITICAL(&s_camera.lock);
        xSemaphoreGive(submit_lock);
        return;
    }
    if (now_us < s_camera.preview_resume_time_us) {
        /* Keep the displayed frame while allowing capture buffers to recycle. */
        for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
            if (s_camera.frames[i].state == MOSAIC_FRAME_READY) {
                s_camera.frames[i].state = MOSAIC_FRAME_FREE;
            }
        }
        portEXIT_CRITICAL(&s_camera.lock);
        xSemaphoreGive(submit_lock);
        return;
    }
    for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
        if (s_camera.frames[i].pixels != NULL &&
                s_camera.frames[i].state == MOSAIC_FRAME_READY &&
                (selected == MOSAIC_CAMERA_BUFFERS ||
                 s_camera.frames[i].sequence > newest_sequence)) {
            selected = i;
            newest_sequence = s_camera.frames[i].sequence;
        }
    }
    if (selected < MOSAIC_CAMERA_BUFFERS) {
        for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
            if (i != selected &&
                    s_camera.frames[i].state == MOSAIC_FRAME_READY) {
                s_camera.frames[i].state = MOSAIC_FRAME_FREE;
            }
        }
        s_camera.frames[selected].state = MOSAIC_FRAME_IN_FLIGHT;
    }
    portEXIT_CRITICAL(&s_camera.lock);

    if (selected >= MOSAIC_CAMERA_BUFFERS) {
        xSemaphoreGive(submit_lock);
        return;
    }

    const esp_gsp_err_t err = esp_gsp_canvas_try_push(
        ui, GSP_BIND_CAMERA_CANVAS, s_camera.frames[selected].pixels,
        MOSAIC_CAMERA_STRIDE, frame_released, (void *)(uintptr_t)selected);
    if (err != ESP_GSP_OK) {
        frame_released((void *)(uintptr_t)selected);
    }
    xSemaphoreGive(submit_lock);
}

void mosaic_camera_stop(esp_gsp_handle_t ui)
{
    SemaphoreHandle_t submit_lock = s_camera.submit_lock;
    if (submit_lock != NULL) {
        xSemaphoreTake(submit_lock, portMAX_DELAY);
    }
    portENTER_CRITICAL(&s_camera.lock);
    if (!s_camera.started) {
        portEXIT_CRITICAL(&s_camera.lock);
        if (submit_lock != NULL) {
            xSemaphoreGive(submit_lock);
        }
        return;
    }
    s_camera.stopping = true;
    portEXIT_CRITICAL(&s_camera.lock);

    (void)esp_gsp_canvas_stop(ui, GSP_BIND_CAMERA_CANVAS);
    if (submit_lock != NULL) {
        xSemaphoreGive(submit_lock);
    }

    if (s_camera.task_done != NULL &&
            xSemaphoreTake(
                s_camera.task_done,
                pdMS_TO_TICKS(MOSAIC_CAMERA_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "timed out waiting for camera capture task");
        return;
    }

    if (s_camera.task_done != NULL) {
        vSemaphoreDelete(s_camera.task_done);
        s_camera.task_done = NULL;
    }
    if (s_camera.submit_lock != NULL) {
        vSemaphoreDelete(s_camera.submit_lock);
        s_camera.submit_lock = NULL;
    }
    s_camera.task = NULL;
    for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
        heap_caps_free(s_camera.frames[i].pixels);
        s_camera.frames[i].pixels = NULL;
        s_camera.frames[i].state = MOSAIC_FRAME_FREE;
        s_camera.frames[i].sequence = 0;
    }
    portENTER_CRITICAL(&s_camera.lock);
    s_camera.started = false;
    s_camera.stopping = false;
    s_camera.sequence = 0;
    s_camera.preview_resume_time_us = 0;
    s_camera.capture_pending = false;
    s_camera.capture_use_flash = false;
    s_camera.flash_enabled = false;
    s_camera.capture_path[0] = '\0';
    s_camera.mirror_x = false;
    s_camera.recognition_mode = CAMERA_VISION_MODE_OFF;
    portEXIT_CRITICAL(&s_camera.lock);
    camera_vision_deinit();
}

#else

static uint8_t *s_frames[MOSAIC_CAMERA_BUFFERS];
static volatile bool s_busy[MOSAIC_CAMERA_BUFFERS];
static bool s_streaming;
static uint8_t s_phase;

static uint16_t rgb565_cycle(uint8_t phase)
{
    const uint8_t seg = phase / 85U;
    const uint8_t t = (uint8_t)((phase % 85U) * 3U);
    uint8_t r;
    uint8_t g;
    uint8_t b;
    switch (seg) {
    case 0:
        r = 255U;
        g = t;
        b = 0U;
        break;
    case 1:
        r = (uint8_t)(255U - t);
        g = 255U;
        b = 0U;
        break;
    default:
        r = 0U;
        g = (uint8_t)(255U - t);
        b = t;
        break;
    }
    return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                      ((uint16_t)(g >> 2) << 5) |
                      (uint16_t)(b >> 3));
}

static void frame_released(void *ctx)
{
    s_busy[(uintptr_t)ctx] = false;
}

static void fill_frame(uint8_t *pixels, uint8_t phase)
{
    const uint16_t color = rgb565_cycle(phase);
    for (size_t y = 0; y < MOSAIC_CAMERA_H; ++y) {
        uint16_t *row = (uint16_t *)(pixels + (size_t)y * MOSAIC_CAMERA_STRIDE);
        for (size_t x = 0; x < MOSAIC_CAMERA_W; ++x) {
            row[x] = color;
        }
    }
}

static void stream_open(void)
{
    for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
        if (s_frames[i] == NULL) {
#if defined(ESP_PLATFORM)
            s_frames[i] = heap_caps_malloc(
                MOSAIC_CAMERA_FRAME_BYTES,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
            if (s_frames[i] == NULL) {
                s_frames[i] = malloc(MOSAIC_CAMERA_FRAME_BYTES);
            }
        }
        s_busy[i] = false;
    }
    s_phase = 0;
    s_streaming = s_frames[0] != NULL && s_frames[1] != NULL;
}

static void stream_close(esp_gsp_handle_t ui)
{
    if (s_streaming) {
        (void)esp_gsp_canvas_stop(ui, GSP_BIND_CAMERA_CANVAS);
    }
    s_streaming = false;
    for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
        free(s_frames[i]);
        s_frames[i] = NULL;
        s_busy[i] = false;
    }
}

static void stream_push(esp_gsp_handle_t ui)
{
    if (!s_streaming) {
        return;
    }
    size_t slot = MOSAIC_CAMERA_BUFFERS;
    for (size_t i = 0; i < MOSAIC_CAMERA_BUFFERS; ++i) {
        if (s_frames[i] != NULL && !s_busy[i]) {
            slot = i;
            break;
        }
    }
    if (slot >= MOSAIC_CAMERA_BUFFERS) {
        return;
    }

    fill_frame(s_frames[slot], s_phase++);
    s_busy[slot] = true;
    esp_gsp_err_t ret = esp_gsp_canvas_push(
        ui, GSP_BIND_CAMERA_CANVAS, s_frames[slot], MOSAIC_CAMERA_STRIDE,
        frame_released, (void *)(uintptr_t)slot);
    if (ret != ESP_GSP_OK) {
        s_busy[slot] = false;
    }
}

esp_err_t mosaic_camera_start(esp_gsp_handle_t ui)
{
    (void)ui;
    if (!s_streaming) {
        stream_open();
    }
    return s_streaming ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t mosaic_camera_capture_photo(const char *path, bool use_flash)
{
    (void)path;
    (void)use_flash;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mosaic_camera_set_flash_enabled(bool enabled)
{
    (void)enabled;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mosaic_camera_toggle_flip(bool *out_enabled)
{
    if (out_enabled != NULL) {
        *out_enabled = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mosaic_camera_set_recognition_mode(camera_vision_mode_t mode)
{
    (void)mode;
    return ESP_ERR_NOT_SUPPORTED;
}

void mosaic_camera_stop(esp_gsp_handle_t ui)
{
    stream_close(ui);
}

void mosaic_camera_tick(esp_gsp_handle_t ui)
{
    if (!s_streaming) {
        stream_open();
    }
    if (s_streaming) {
        stream_push(ui);
    }
}

#endif
