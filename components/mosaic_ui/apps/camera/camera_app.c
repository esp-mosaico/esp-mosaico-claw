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

#if defined(ESP_PLATFORM)
static void camera_start_deferred(void *user_ctx)
{
    (void)user_ctx;
    if (mosaic_loader_app() != &mosaic_camera_app) {
        return;
    }

    esp_gsp_handle_t ui = mosaic_loader_ui();
    if (ui == NULL) {
        printf("mosaic_camera: active UI unavailable\n");
        return;
    }
    static const mosaic_camera_binds_t binds = {
        .canvas = GSP_BIND_CAMERA_CANVAS,
        .missing_hint_visible = GSP_BIND_CAMERA_MISSING_VISIBLE,
    };
    const esp_err_t err = mosaic_camera_start(ui, &binds);
    if (err != ESP_OK) {
        printf("mosaic_camera: start failed err=%d\n", (int)err);
        return;
    }
    if (s_flash_mode == CAMERA_FLASH_ON) {
        const esp_err_t flash_err = mosaic_camera_set_flash_enabled(true);
        if (flash_err != ESP_OK) {
            printf("mosaic_camera: restore continuous flash failed err=%d\n", (int)flash_err);
        }
    }
}

static void camera_started(esp_gsp_handle_t ui)
{
    camera_load_flash_mode();
    camera_render_flash_mode(ui);
    const esp_err_t err = mosaic_loader_defer(camera_start_deferred, NULL);
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
    camera_load_flash_mode();
    camera_render_flash_mode(ui);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CAMERA_MISSING_VISIBLE, false);
    mosaic_camera_tick(ui);
    (void)esp_gsp_timer_create(ui, 40, camera_tick, NULL);
}
#endif

static void camera_stopping(esp_gsp_handle_t ui)
{
    if (s_flash_mode == CAMERA_FLASH_ON) {
        const esp_err_t err = mosaic_camera_set_flash_enabled(false);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            printf("mosaic_camera: disable continuous flash failed err=%d\n", (int)err);
        }
    }
    mosaic_camera_stop(ui);
}

static const mosaic_app_route_t s_camera_routes[] = {
    { .action_id = GSP_ACT_ID_CAMERA_ALBUM, .target_name = "album" },
};

static void camera_capture_photo(void)
{
    char directory[CAMERA_PHOTO_PATH_MAX] = {0};
    esp_err_t err = claw_paths_join(
        CLAW_PATH_DATA, CAMERA_PHOTO_DIR,
        directory, sizeof(directory));
    if (err != ESP_OK) {
        printf("mosaic_camera: compose photo directory failed err=%d\n",
               (int)err);
        return;
    }
    if (mkdir(directory, 0775) != 0 && errno != EEXIST) {
        printf("mosaic_camera: create photo directory failed errno=%d\n",
               errno);
        return;
    }

    const time_t now = time(NULL);
    struct tm local_time = {0};
    if (localtime_r(&now, &local_time) == NULL) {
        printf("mosaic_camera: resolve local time failed\n");
        return;
    }

    char relative[96] = {0};
    if (strftime(relative, sizeof(relative), CAMERA_PHOTO_DIR "/camera_%Y%m%d_%H%M%S.jpg", &local_time) == 0) {
        printf("mosaic_camera: compose photo name failed\n");
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
    }
}

static void camera_event(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    (void)ui;
    if (event == NULL || event->type != MOSAIC_EVENT_UI_CALL) {
        return;
    }
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_CAMERA_ALBUM:
        break;
    case GSP_ACT_ID_CAMERA_SHUTTER:
        camera_capture_photo();
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
