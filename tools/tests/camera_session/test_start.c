/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef int esp_err_t;
typedef void *esp_gsp_handle_t;
enum { ESP_OK = 0, ESP_FAIL = -1, CAMERA_FLASH_ON = 1 };
static const int mosaic_camera_app;
static const void *active_app = &mosaic_camera_app;
static void *active_ui = (void *)(uintptr_t)1;
static bool s_start_pending;
static uint32_t s_start_epoch = 42;
static int s_recognition_mode = 3, s_flash_mode = CAMERA_FLASH_ON;
static unsigned starts, stops, restored_modes, restored_flash;
static esp_err_t stop_error;
static const void *mosaic_loader_app(void) { return active_app; }
static void *mosaic_loader_ui(void) { return active_ui; }
static int mosaic_camera_start(void *ui) { assert(ui == active_ui); ++starts; return ESP_OK; }
static int mosaic_camera_stop(void *ui) { assert(ui == active_ui); ++stops; return stop_error; }
static int mosaic_camera_set_recognition_mode(int mode)
{ assert(mode == s_recognition_mode); ++restored_modes; return ESP_OK; }
static int mosaic_camera_set_flash_enabled(bool enabled)
{ assert(enabled); ++restored_flash; return ESP_OK; }
#include "camera_start_under_test.h"

int main(void)
{
    s_start_pending = true;
    camera_start_deferred((void *)(uintptr_t)41);
    camera_retry_deferred((void *)(uintptr_t)41);
    assert(s_start_pending && starts == 0 && stops == 0);
    active_app = NULL;
    camera_start_deferred((void *)(uintptr_t)42);
    assert(starts == 0);
    active_app = &mosaic_camera_app;
    camera_start_deferred((void *)(uintptr_t)42);
    assert(!s_start_pending && starts == 1 && restored_modes == 1 && restored_flash == 1);
    stop_error = ESP_FAIL; s_start_pending = true;
    camera_retry_deferred((void *)(uintptr_t)42);
    assert(!s_start_pending && stops == 1 && starts == 1);
    stop_error = ESP_OK;
    camera_retry_deferred((void *)(uintptr_t)42);
    assert(stops == 2 && starts == 2 && restored_modes == 2 && restored_flash == 2);
    ++s_start_epoch;
    camera_retry_deferred((void *)(uintptr_t)42);
    assert(stops == 2 && starts == 2);
    puts("PASS deferred generations reject stale work and restore recognition/flash only after checked cleanup");
    return 0;
}
