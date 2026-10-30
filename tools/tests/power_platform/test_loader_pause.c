/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifndef ESP_ERR_NOT_FINISHED
#define ESP_ERR_NOT_FINISHED 0x10C
#endif
#define ESP_GSP_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)

static void *s_runtime_lock = (void *)(uintptr_t)1;
static void *s_runtime = (void *)(uintptr_t)2;
static void *s_platform = (void *)(uintptr_t)3;
static void *test_ui = (void *)(uintptr_t)4;
static bool s_runtime_started = true, lock_available = true, active_hub = true;
static bool paused;
static unsigned takes, gives, hides;
static esp_err_t pause_error, hide_error;

static int xSemaphoreTake(void *lock, unsigned timeout)
{ ++takes; return lock_available; }
static void xSemaphoreGive(void *lock) { ++gives; }
static void *mosaic_app_root(void) { return (void *)(uintptr_t)5; }
static void *mosaic_runtime_active_app(void *runtime)
{ return active_hub ? mosaic_app_root() : NULL; }
static esp_err_t mosaic_esp_platform_pause_screen(void *platform, unsigned timeout)
{ paused = pause_error == ESP_OK; return pause_error; }
static void *mosaic_esp_platform_ui(void *platform) { return test_ui; }
static int mosaic_hub_quiet_module_notice(void *ui)
{ assert(paused && ui == test_ui); ++hides; return hide_error; }

#include "loader_pause_under_test.h"

int main(void)
{
    assert(mosaic_loader_pause_screen(0) == ESP_ERR_INVALID_STATE && takes == 0);
    lock_available = false;
    assert(mosaic_loader_pause_screen(100) == ESP_ERR_TIMEOUT && hides == 0 && gives == 0);
    lock_available = true;
    pause_error = ESP_ERR_NOT_FINISHED;
    assert(mosaic_loader_pause_screen(100) == ESP_ERR_NOT_FINISHED && hides == 0 && gives == 1);
    puts("PASS an incomplete pause never touches retained Hub UI");

    pause_error = ESP_OK;
    active_hub = false;
    assert(mosaic_loader_pause_screen(100) == ESP_OK && hides == 0 && gives == 2);
    active_hub = true;
    assert(mosaic_loader_pause_screen(100) == ESP_OK && hides == 1 && gives == 3);
    puts("PASS only a fully paused Hub quiets pending module feedback; other Apps are untouched");

    hide_error = ESP_FAIL;
    assert(mosaic_loader_pause_screen(100) == ESP_FAIL && paused && gives == 4);
    puts("PASS a failed module-feedback lifecycle boundary reaches the existing screen recovery path");
    return 0;
}
