/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_top_notice.h"

#include <stdbool.h>

typedef struct {
    esp_gsp_handle_t ui;
    mosaic_top_notice_config_t config;
    void *timer;
} mosaic_top_notice_state_t;

static mosaic_top_notice_state_t s_notice;

static void notice_timer_cb(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (ui != s_notice.ui) {
        return;
    }
    void *timer = s_notice.timer;
    s_notice.timer = NULL;
    if (timer != NULL) {
        (void)esp_gsp_timer_delete(ui, timer);
    }
    (void)esp_gsp_set_visible(ui, s_notice.config.visible_bind, false);
}

esp_gsp_err_t mosaic_top_notice_show(
    esp_gsp_handle_t ui, const mosaic_top_notice_config_t *config,
    const char *title, const char *message, uint32_t duration_ms)
{
    if (ui == NULL || config == NULL || title == NULL || message == NULL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    mosaic_top_notice_detach(s_notice.ui);
    s_notice.ui = ui;
    s_notice.config = *config;
    esp_gsp_err_t err = esp_gsp_set_text(ui, config->title_bind, title);
    if (err == ESP_GSP_OK) {
        err = esp_gsp_set_text(ui, config->message_bind, message);
    }
    if (err == ESP_GSP_OK) {
        err = esp_gsp_set_visible(ui, config->visible_bind, true);
    }
    if (err == ESP_GSP_OK && duration_ms != 0) {
        s_notice.timer = esp_gsp_timer_create(
            ui, duration_ms, notice_timer_cb, NULL);
        if (s_notice.timer == NULL) {
            err = ESP_GSP_ERR_NO_MEM;
        }
    }
    return err;
}

void mosaic_top_notice_hide(esp_gsp_handle_t ui)
{
    if (ui == NULL || ui != s_notice.ui) {
        return;
    }
    if (s_notice.timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_notice.timer);
        s_notice.timer = NULL;
    }
    (void)esp_gsp_set_visible(ui, s_notice.config.visible_bind, false);
}

void mosaic_top_notice_detach(esp_gsp_handle_t ui)
{
    if (ui == NULL || ui != s_notice.ui) {
        return;
    }
    if (s_notice.timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_notice.timer);
    }
    s_notice = (mosaic_top_notice_state_t) {0};
}
