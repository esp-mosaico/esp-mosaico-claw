/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "camera_actions.h"
#include "camera_binds.h"
#include "camera_objects.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "claw_paths.h"
#include "mosaic_camera.h"
#if defined(ESP_PLATFORM)
#include "mosaic_loader.h"
#include "nvs.h"
#include "esp_timer.h"
#endif

#define CAMERA_PHOTO_DIR "photos"
#define CAMERA_PHOTO_PATH_MAX 256U
#define CAMERA_NVS_NAMESPACE "mosaic_camera"
#define CAMERA_NVS_FLASH_MODE_KEY "flash_mode"

extern const mosaic_app_descriptor_t mosaic_camera_app;

typedef enum {
    CAMERA_FLASH_OFF = 0,
    CAMERA_FLASH_AUTO,
    CAMERA_FLASH_ON,
    CAMERA_FLASH_MODE_COUNT,
} camera_flash_mode_t;

static camera_flash_mode_t s_flash_mode;
static camera_vision_mode_t s_recognition_mode;
static bool s_recognition_menu_open;
static uint32_t s_capture_sequence_seen;
static esp_err_t s_capture_result_seen;
static const char *s_capture_notice;
static int64_t s_capture_notice_until_us;
static uint32_t s_photo_sequence;
static bool s_startup_notice;
static const char *s_status_text;
static bool s_status_dirty;
static bool s_start_pending;
#if defined(ESP_PLATFORM)
static uint32_t s_start_epoch;
#endif

static int64_t camera_now_us(void)
{
#if defined(ESP_PLATFORM)
    return esp_timer_get_time();
#else
    return (int64_t)clock() * 1000000 / CLOCKS_PER_SEC;
#endif
}

static void camera_status_text(esp_gsp_handle_t ui, const char *text)
{
    s_status_text = text;
    s_status_dirty = ui == NULL ||
        esp_gsp_set_text(ui, GSP_BIND_CAMERA_RECOGNITION_STATUS, text) != ESP_GSP_OK;
}

static void camera_notice(esp_gsp_handle_t ui, const char *text, bool persistent)
{
    s_capture_notice = text;
    s_capture_notice_until_us = persistent ? INT64_MAX : camera_now_us() + 2000000;
    camera_status_text(ui, text);
}


static void camera_load_flash_mode(void)
{
    s_flash_mode = CAMERA_FLASH_OFF;
#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CAMERA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        printf("mosaic_camera: open flash settings failed err=%d\n", (int)err);
        return;
    }

    uint8_t value = CAMERA_FLASH_OFF;
    err = nvs_get_u8(nvs, CAMERA_NVS_FLASH_MODE_KEY, &value);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK || value >= CAMERA_FLASH_MODE_COUNT) {
        printf("mosaic_camera: load flash mode failed err=%d value=%u\n", (int)err, (unsigned)value);
        return;
    }
    s_flash_mode = (camera_flash_mode_t)value;
#endif
}

static esp_err_t camera_save_flash_mode(void)
{
#if defined(ESP_PLATFORM)
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(CAMERA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, CAMERA_NVS_FLASH_MODE_KEY, (uint8_t)s_flash_mode);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
#else
    return ESP_OK;
#endif
}

static void camera_render_flash_mode(esp_gsp_handle_t ui)
{
    if (ui == NULL) {
        return;
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_FLASH_OFF_VISIBLE, s_flash_mode == CAMERA_FLASH_OFF);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_FLASH_AUTO_VISIBLE, s_flash_mode == CAMERA_FLASH_AUTO);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_FLASH_ON_VISIBLE, s_flash_mode == CAMERA_FLASH_ON);
}

static void camera_render_recognition(esp_gsp_handle_t ui)
{
    if (ui == NULL) {
        return;
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_RECOGNITION_MENU_VISIBLE, s_recognition_menu_open);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_RECOGNITION_ACTIVE_VISIBLE, s_recognition_mode != CAMERA_VISION_MODE_OFF);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_QRCODE_SELECTED_VISIBLE, s_recognition_mode == CAMERA_VISION_MODE_QRCODE);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_COLOR_SELECTED_VISIBLE, s_recognition_mode == CAMERA_VISION_MODE_COLOR);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_MOTION_SELECTED_VISIBLE, s_recognition_mode == CAMERA_VISION_MODE_MOTION);
    camera_status_text(ui,
                           s_capture_notice != NULL ? s_capture_notice :
                           s_recognition_mode == CAMERA_VISION_MODE_QRCODE ? "QR scanning" :
                           s_recognition_mode == CAMERA_VISION_MODE_COLOR ? "Color scanning" :
                           s_recognition_mode == CAMERA_VISION_MODE_MOTION ? "Motion scanning" : "");
    if (s_recognition_mode == CAMERA_VISION_MODE_OFF) {
        (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_RECOGNITION_RESULT_VISIBLE, false);
    }
}

static void camera_capture_status_tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (s_status_dirty && s_status_text != NULL) {
        camera_status_text(ui, s_status_text);
    }
    mosaic_camera_capture_status_t status = {0};
    if (mosaic_camera_get_capture_status(&status) != ESP_OK) {
        return;
    }
    if (!status.ready && !status.pending &&
            (s_start_pending || status.startup_result != ESP_OK)) {
        const char *text = "Tap shutter to retry";
#if defined(ESP_PLATFORM)
        if (s_start_pending || status.startup_result == ESP_ERR_NOT_FINISHED) {
            text = "Starting camera...";
        }
#endif
        if (!s_startup_notice || s_capture_notice != text) {
            camera_notice(ui, text, true);
        }
        s_startup_notice = true;
        return;
    }
    if (s_startup_notice) {
        s_startup_notice = false;
        s_capture_notice = NULL;
        camera_render_recognition(ui);
    }
    if (status.sequence != s_capture_sequence_seen ||
            status.result != s_capture_result_seen) {
        s_capture_sequence_seen = status.sequence;
        s_capture_result_seen = status.result;
        if (status.sequence != 0 && !status.pending) {
            camera_notice(ui, status.result == ESP_OK ? "Photo saved" : "Save failed. Try again", false);
        }
    }
    if (s_capture_notice != NULL && camera_now_us() >= s_capture_notice_until_us) {
        s_capture_notice = NULL;
        camera_render_recognition(ui);
    }
}

static void camera_select_recognition_mode(esp_gsp_handle_t ui, camera_vision_mode_t selected)
{
    const camera_vision_mode_t next = s_recognition_mode == selected ? CAMERA_VISION_MODE_OFF : selected;
    const esp_err_t err = mosaic_camera_set_recognition_mode(next);
    if (err != ESP_OK) {
        printf("mosaic_camera: switch recognition mode failed err=%d\n", (int)err);
        return;
    }
    s_recognition_mode = next;
    s_recognition_menu_open = false;
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_RECOGNITION_RESULT_VISIBLE, false);
    camera_render_recognition(ui);
}

static void camera_reset_capture_notice(esp_gsp_handle_t ui)
{
    s_capture_notice = NULL;
    s_capture_notice_until_us = 0;
    s_capture_sequence_seen = 0;
    s_capture_result_seen = ESP_OK;
    s_startup_notice = false;
    s_status_text = NULL;
    s_status_dirty = false;
    s_start_pending = false;
    (void)ui;
}

#if defined(ESP_PLATFORM)
static void camera_start_deferred(void *user_ctx)
{
    if ((uint32_t)(uintptr_t)user_ctx != s_start_epoch ||
            mosaic_loader_app() != &mosaic_camera_app) {
        return;
    }
    s_start_pending = false;

    esp_gsp_handle_t ui = mosaic_loader_ui();
    if (ui == NULL) {
        printf("mosaic_camera: active UI unavailable\n");
        return;
    }
    const esp_err_t err = mosaic_camera_start(ui);
    if (err != ESP_OK) {
        printf("mosaic_camera: start failed err=%d\n", (int)err);
        return;
    }
    (void)mosaic_camera_set_recognition_mode(s_recognition_mode);
    if (s_flash_mode == CAMERA_FLASH_ON) {
        const esp_err_t flash_err = mosaic_camera_set_flash_enabled(true);
        if (flash_err != ESP_OK) {
            printf("mosaic_camera: restore continuous flash failed err=%d\n", (int)flash_err);
        }
    }
}

static void camera_retry_deferred(void *user_ctx)
{
    if ((uint32_t)(uintptr_t)user_ctx != s_start_epoch ||
            mosaic_loader_app() != &mosaic_camera_app) {
        return;
    }
    s_start_pending = false;
    esp_gsp_handle_t ui = mosaic_loader_ui();
    if (ui == NULL) {
        return;
    }
    const esp_err_t err = mosaic_camera_stop(ui);
    if (err != ESP_OK) {
        printf("mosaic_camera: retry cleanup retained err=%d\n", (int)err);
        return;
    }
    camera_start_deferred(user_ctx);
}

static void camera_started(esp_gsp_handle_t ui)
{
    camera_reset_capture_notice(ui);
    camera_load_flash_mode();
    s_recognition_mode = CAMERA_VISION_MODE_OFF;
    s_recognition_menu_open = false;
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_ALBUM_THUMBNAIL_VISIBLE, false);
    camera_render_flash_mode(ui);
    camera_render_recognition(ui);
    ++s_start_epoch;
    camera_notice(ui, "Starting camera...", true);
    s_startup_notice = true;
    const esp_err_t err = mosaic_loader_defer(camera_start_deferred,
        (void *)(uintptr_t)s_start_epoch);
    s_start_pending = err == ESP_OK;
    if (err != ESP_OK) {
        printf("mosaic_camera: defer start failed err=%d\n", (int)err);
    }
}
#else
static void camera_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    mosaic_camera_tick(ui);
}

static void camera_started(esp_gsp_handle_t ui)
{
    camera_reset_capture_notice(ui);
    camera_load_flash_mode();
    s_recognition_mode = CAMERA_VISION_MODE_OFF;
    s_recognition_menu_open = false;
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_ALBUM_THUMBNAIL_VISIBLE, false);
    camera_render_flash_mode(ui);
    camera_render_recognition(ui);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_MISSING_VISIBLE, false);
    mosaic_camera_tick(ui);
    (void)esp_gsp_timer_create(ui, 40, camera_tick, NULL);
}
#endif

static void camera_stopping(esp_gsp_handle_t ui)
{
    s_capture_notice = NULL;
    s_status_text = NULL;
    s_status_dirty = false;
    s_start_pending = false;
#if defined(ESP_PLATFORM)
    ++s_start_epoch;
#endif

    if (s_flash_mode == CAMERA_FLASH_ON) {
        const esp_err_t err = mosaic_camera_set_flash_enabled(false);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            printf("mosaic_camera: disable continuous flash failed err=%d\n", (int)err);
        }
    }
    s_recognition_mode = CAMERA_VISION_MODE_OFF;
    s_recognition_menu_open = false;
    mosaic_camera_stop(ui);
}

static const mosaic_app_route_t s_camera_routes[] = {
    { .action_id = GSP_ACT_ID_CAMERA_ALBUM, .target_name = "album" },
};

static void camera_capture_photo(esp_gsp_handle_t ui)
{
    mosaic_camera_capture_status_t status = {0};
    if (mosaic_camera_get_capture_status(&status) != ESP_OK ||
            !status.ready || status.pending) {
        camera_notice(ui, status.pending ? "Saving photo..." : "Camera not ready", status.pending);
        return;
    }
    char directory[CAMERA_PHOTO_PATH_MAX] = {0};
    esp_err_t err = claw_paths_join(
        CLAW_PATH_DATA, CAMERA_PHOTO_DIR,
        directory, sizeof(directory));
    if (err != ESP_OK) {
        printf("mosaic_camera: compose photo directory failed err=%d\n",
               (int)err);
        camera_notice(ui, "Save failed. Try again", false);
        return;
    }
    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        printf("mosaic_camera: create photo directory failed errno=%d\n",
               errno);
        camera_notice(ui, "Save failed. Try again", false);
        return;
    }

    const time_t now = time(NULL);
    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) {
        printf("mosaic_camera: resolve local time failed\n");
        camera_notice(ui, "Save failed. Try again", false);
        return;
    }

    char stamp[32] = {0};
    if (strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local_time) == 0) {
        printf("mosaic_camera: compose photo name failed\n");
        camera_notice(ui, "Save failed. Try again", false);
        return;
    }

    char relative[96] = {0};
    const int name_size = snprintf(relative, sizeof(relative),
        CAMERA_PHOTO_DIR "/camera_%s_%016" PRIx64 "_%010" PRIu32 ".jpg",
        stamp, (uint64_t)camera_now_us(), ++s_photo_sequence);
    if (name_size < 0 || (size_t)name_size >= sizeof(relative)) {
        camera_notice(ui, "Save failed. Try again", false);
        return;
    }
    char path[CAMERA_PHOTO_PATH_MAX] = {0};
    err = claw_paths_join(
        CLAW_PATH_DATA, relative, path, sizeof(path));
    if (err == ESP_OK) {
        err = mosaic_camera_capture_photo(path, s_flash_mode == CAMERA_FLASH_AUTO);
    }
    if (err != ESP_OK) {
        printf("mosaic_camera: request photo failed err=%d\n", (int)err);
        mosaic_camera_capture_status_t status = {0};
        (void)mosaic_camera_get_capture_status(&status);
        camera_notice(ui, status.pending ? "Saving photo..." : "Camera not ready", status.pending);
    } else {
        printf("mosaic_camera: photo request queued: %s\n", path);
        camera_notice(ui, "Saving photo...", true);
    }
}

static void camera_event(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    (void)ui;
    if (event == NULL) {
        return;
    }
    if (event->type == MOSAIC_EVENT_TIMER) {
        camera_capture_status_tick(ui, NULL);
        return;
    }
    if (event->type != MOSAIC_EVENT_UI_CALL) {
        return;
    }
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_CAMERA_ALBUM:
        break;
    case GSP_ACT_ID_CAMERA_SHUTTER:
#if defined(ESP_PLATFORM)
        {
            mosaic_camera_capture_status_t status = {0};
            if (mosaic_camera_get_capture_status(&status) == ESP_OK &&
                    !status.ready && !status.pending && !s_start_pending &&
                    status.startup_result != ESP_OK &&
                    status.startup_result != ESP_ERR_NOT_FINISHED) {
                s_start_pending = mosaic_loader_defer(camera_retry_deferred,
                    (void *)(uintptr_t)s_start_epoch) == ESP_OK;
                break;
            }
        }
#endif
        camera_capture_photo(ui);
        break;
    case GSP_ACT_ID_CAMERA_FLIP: {
        bool enabled = false;
        const esp_err_t err = mosaic_camera_toggle_flip(&enabled);
        if (err != ESP_OK) {
            printf("mosaic_camera: flip failed err=%d\n", (int)err);
        }
        break;
    }
    case GSP_ACT_ID_CAMERA_FLASH_TOGGLE: {
        if (s_flash_mode == CAMERA_FLASH_ON) {
            const esp_err_t err = mosaic_camera_set_flash_enabled(false);
            if (err != ESP_OK) {
                printf("mosaic_camera: disable continuous flash failed err=%d\n", (int)err);
            }
        }
        s_flash_mode = (camera_flash_mode_t)((s_flash_mode + 1U) % CAMERA_FLASH_MODE_COUNT);
        if (s_flash_mode == CAMERA_FLASH_ON) {
            const esp_err_t err = mosaic_camera_set_flash_enabled(true);
            if (err != ESP_OK) {
                printf("mosaic_camera: enable continuous flash failed err=%d\n", (int)err);
            }
        }
        camera_render_flash_mode(ui);
        const esp_err_t err = camera_save_flash_mode();
        if (err != ESP_OK) {
            printf("mosaic_camera: save flash mode failed err=%d\n", (int)err);
        }
        break;
    }
    case GSP_ACT_ID_CAMERA_RECOGNITION_MENU:
        s_recognition_menu_open = !s_recognition_menu_open;
        camera_render_recognition(ui);
        break;
    case GSP_ACT_ID_CAMERA_QRCODE_MODE:
        camera_select_recognition_mode(ui, CAMERA_VISION_MODE_QRCODE);
        break;
    case GSP_ACT_ID_CAMERA_COLOR_MODE:
        camera_select_recognition_mode(ui, CAMERA_VISION_MODE_COLOR);
        break;
    case GSP_ACT_ID_CAMERA_MOTION_MODE:
        camera_select_recognition_mode(ui, CAMERA_VISION_MODE_MOTION);
        break;
    default:
        break;
    }
}

const mosaic_app_descriptor_t mosaic_camera_app = {
    .id = 1,
    .launch_action = GSP_ACT_ID_APP_CAMERA,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .routes = s_camera_routes,
    .route_count = sizeof(s_camera_routes) / sizeof(s_camera_routes[0]),
    .name = "camera",
    .title = "Camera",
    .directory = &gsp_obj_directory_camera,
    .disable_swipe = true,
    .root_header_in_stack = true,
    .on_started = camera_started,
    .on_stopping = camera_stopping,
    .on_event = camera_event,
};
