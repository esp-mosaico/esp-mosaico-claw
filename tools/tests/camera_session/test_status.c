/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_PLATFORM 1
typedef int esp_err_t;
typedef void *esp_gsp_handle_t;
enum { ESP_OK = 0, ESP_GSP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101,
       ESP_ERR_NOT_FINISHED = 0x10c,
       CAMERA_VISION_MODE_OFF = 0, CAMERA_VISION_MODE_QRCODE,
       CAMERA_VISION_MODE_COLOR, CAMERA_VISION_MODE_MOTION };
static bool s_recognition_menu_open, s_startup_notice, s_status_dirty, s_start_pending;
static int s_recognition_mode = CAMERA_VISION_MODE_QRCODE;
static uint32_t s_capture_sequence_seen;
static esp_err_t s_capture_result_seen;
static const char *s_capture_notice, *s_status_text;
static int64_t s_capture_notice_until_us, now_us;
static unsigned rejected_writes;
static char label[128];
static int64_t camera_now_us(void) { return now_us; }
static int esp_gsp_set_text(void *ui, int bind, const char *text)
{
    (void)bind; assert(ui && text);
    if (rejected_writes) { --rejected_writes; return ESP_FAIL; }
    snprintf(label, sizeof(label), "%s", text);
    return ESP_OK;
}
static int esp_gsp_set_visible(void *ui, int bind, bool visible)
{ (void)ui; (void)bind; (void)visible; return ESP_OK; }
/* The public status type is extracted before these declarations. */
#include "camera_status_type.h"
static mosaic_camera_capture_status_t current;
static int mosaic_camera_get_capture_status(mosaic_camera_capture_status_t *out)
{ *out = current; return ESP_OK; }
#include "camera_status_functions.h"

int main(void)
{
    void *ui = (void *)(uintptr_t)1;
    current.startup_result = ESP_ERR_NOT_FINISHED;
    rejected_writes = 1;
    camera_capture_status_tick(ui, NULL);
    assert(s_status_dirty && !strcmp(s_status_text, "Starting camera..."));
    camera_capture_status_tick(ui, NULL);
    assert(!s_status_dirty && !strcmp(label, "Starting camera..."));
    current.startup_result = 0x103; s_start_pending = true;
    camera_capture_status_tick(ui, NULL);
    assert(!strcmp(label, "Starting camera..."));
    s_start_pending = false;
    current.startup_result = ESP_ERR_NO_MEM;
    camera_capture_status_tick(ui, NULL);
    assert(!strcmp(label, "Tap shutter to retry"));
    current.startup_result = ESP_OK; current.ready = true;
    camera_capture_status_tick(ui, NULL);
    assert(!strcmp(label, "QR scanning") && !s_startup_notice);
    puts("PASS startup failure and recovery preserve the existing recognition status");

    rejected_writes = 3;
    camera_notice(ui, "Saving photo...", true);
    current.sequence = 1; current.result = ESP_OK;
    camera_capture_status_tick(ui, NULL);
    assert(s_status_dirty && !strcmp(s_status_text, "Photo saved"));
    camera_capture_status_tick(ui, NULL);
    assert(!s_status_dirty && !strcmp(label, "Photo saved"));
    now_us += 2000001; rejected_writes = 1;
    camera_capture_status_tick(ui, NULL);
    assert(s_status_dirty && !strcmp(s_status_text, "QR scanning"));
    camera_capture_status_tick(ui, NULL);
    assert(!s_status_dirty && !strcmp(label, "QR scanning"));
    puts("PASS queue-full retries apply the latest completion and expiry text");
    return 0;
}
