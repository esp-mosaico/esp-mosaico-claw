/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "esp_err.h"
#ifndef ESP_ERR_NOT_FINISHED
#define ESP_ERR_NOT_FINISHED 0x10C
#endif

static atomic_uint s_screen_timeout_ms = 30000;
static atomic_bool s_screen_asleep, s_ignore_wake_pointer;
static atomic_bool s_camera_preview_active, s_contact_pressed;
static atomic_bool s_presenter_active = true;
static void *s_screen_idle_timer;
static bool panel_enabled = true, producer_paused;
static esp_err_t pause_error, resume_error;
static unsigned off_failures, rearms, resumes;
#define SCREEN_PAUSE_TIMEOUT_MS 1000
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static void screen_rearm_idle_timer(void) { ++rearms; }
static void vTaskDelay(unsigned ticks) { (void)ticks; }
static esp_err_t esp_timer_stop(void *timer) { (void)timer; return ESP_OK; }
static bool display_service_panel_enabled(void) { return panel_enabled; }
static esp_err_t display_service_set_panel_enabled(bool enabled)
{
    if (!enabled && off_failures) { --off_failures; return ESP_FAIL; }
    panel_enabled = enabled;
    return ESP_OK;
}
static esp_err_t mosaic_loader_pause_screen(unsigned timeout)
{ (void)timeout; producer_paused = true; return pause_error; }
static esp_err_t mosaic_loader_resume_screen(void)
{ ++resumes; if (resume_error == ESP_OK) producer_paused = false; return resume_error; }

/* The runner copies these unchanged production functions, while the full IDF
 * build verifies their complete translation unit and actual SDK signatures. */
#include "screen_power_under_test.h"

int main(void)
{
    screen_apply_sleep();
    assert(atomic_load(&s_screen_asleep) && producer_paused && !panel_enabled);
    screen_apply_wake();
    assert(!atomic_load(&s_screen_asleep) && !producer_paused && panel_enabled);
    puts("PASS normal screen suspension and resume retain a consistent state");

    pause_error = ESP_ERR_NOT_FINISHED;
    resume_error = ESP_FAIL;
    screen_apply_sleep();
    assert(producer_paused && atomic_load(&s_screen_asleep) && !panel_enabled);
    assert(resumes == 2);
    resume_error = ESP_OK;
    screen_apply_wake();
    assert(!atomic_load(&s_screen_asleep) && !producer_paused && panel_enabled);
    puts("PASS a partial pause with failed recovery keeps wake retry available");

    pause_error = ESP_OK;
    resume_error = ESP_FAIL;
    off_failures = 1;
    screen_apply_sleep();
    assert(producer_paused && atomic_load(&s_screen_asleep) && !panel_enabled);
    resume_error = ESP_OK;
    screen_apply_wake();
    assert(!atomic_load(&s_screen_asleep) && !producer_paused && panel_enabled);
    puts("PASS panel-off failure cannot mark a still-paused producer as awake");

    pause_error = ESP_ERR_NOT_FINISHED;
    screen_apply_sleep();
    assert(!atomic_load(&s_screen_asleep) && !producer_paused && panel_enabled);
    puts("PASS successful abort resumes the producer and rearms idle detection");
    return 0;
}
