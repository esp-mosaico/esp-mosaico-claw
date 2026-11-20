/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bluetooth_avrcp_compat.h"

#include <stdatomic.h>

#include "esp_avrc_api.h"
#include "esp_timer.h"

#define AVRCP_PLAY_POSITION_INTERVAL_SECONDS 1U
#define AVRCP_PLAY_STATUS_TRANSACTION_LABEL  15U
#define AVRCP_PLAY_STATUS_TIMEOUT_MS         UINT32_C(2000)

static esp_avrc_ct_cb_t s_avrcp_callback;
static atomic_uint s_status_sequence;
static atomic_bool s_request_pending;
static atomic_uint_fast32_t s_request_timestamp_ms;
static atomic_uint_least32_t s_status_duration_ms;
static atomic_uint_least32_t s_status_position_ms;
static atomic_bool s_status_playing;

esp_err_t __real_esp_avrc_ct_register_callback(esp_avrc_ct_cb_t callback);

esp_err_t __real_esp_avrc_ct_send_register_notification_cmd(
    uint8_t tl, uint8_t event_id, uint32_t event_parameter);

static void bluetooth_avrcp_callback(esp_avrc_ct_cb_event_t event,
                                     esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_PLAY_STATUS_RSP_EVT && param) {
        atomic_fetch_add_explicit(&s_status_sequence, 1U,
                                  memory_order_acq_rel);
        atomic_store_explicit(&s_status_duration_ms,
                              param->play_status_rsp.song_length,
                              memory_order_relaxed);
        atomic_store_explicit(&s_status_position_ms,
                              param->play_status_rsp.song_position,
                              memory_order_relaxed);
        bool playing = param->play_status_rsp.play_status ==
                ESP_AVRC_PLAYBACK_PLAYING ||
            param->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_FWD_SEEK ||
            param->play_status_rsp.play_status == ESP_AVRC_PLAYBACK_REV_SEEK;
        atomic_store_explicit(&s_status_playing, playing,
                              memory_order_relaxed);
        atomic_fetch_add_explicit(&s_status_sequence, 1U,
                                  memory_order_release);
        atomic_store_explicit(&s_request_pending, false,
                              memory_order_release);
    }
    if (s_avrcp_callback) {
        s_avrcp_callback(event, param);
    }
}

esp_err_t __wrap_esp_avrc_ct_register_callback(esp_avrc_ct_cb_t callback)
{
    s_avrcp_callback = callback;
    return __real_esp_avrc_ct_register_callback(bluetooth_avrcp_callback);
}

esp_err_t __wrap_esp_avrc_ct_send_register_notification_cmd(
    uint8_t tl, uint8_t event_id, uint32_t event_parameter)
{
    /* Some targets reject zero-interval play-position notifications. */
    if (event_id == ESP_AVRC_RN_PLAY_POS_CHANGED && event_parameter == 0) {
        event_parameter = AVRCP_PLAY_POSITION_INTERVAL_SECONDS;
    }
    return __real_esp_avrc_ct_send_register_notification_cmd(
        tl, event_id, event_parameter);
}

void bluetooth_avrcp_compat_reset(void)
{
    atomic_store_explicit(&s_status_sequence, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_request_pending, false, memory_order_relaxed);
    atomic_store_explicit(&s_request_timestamp_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_status_duration_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_status_position_ms, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_status_playing, false, memory_order_relaxed);
}

esp_err_t bluetooth_avrcp_compat_request_play_status(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool pending = atomic_load_explicit(&s_request_pending,
                                        memory_order_acquire);
    uint32_t requested_at = atomic_load_explicit(&s_request_timestamp_ms,
                                                 memory_order_relaxed);
    if (pending && now_ms - requested_at < AVRCP_PLAY_STATUS_TIMEOUT_MS) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store_explicit(&s_request_pending, true, memory_order_release);
    atomic_store_explicit(&s_request_timestamp_ms, now_ms,
                          memory_order_relaxed);
    esp_err_t err = esp_avrc_ct_send_get_play_status_cmd(
        AVRCP_PLAY_STATUS_TRANSACTION_LABEL);
    if (err != ESP_OK) {
        atomic_store_explicit(&s_request_pending, false,
                              memory_order_release);
    }
    return err;
}

bool bluetooth_avrcp_compat_get_play_status(
    bluetooth_avrcp_play_status_t *out_status, uint32_t *out_revision)
{
    if (!out_status || !out_revision) {
        return false;
    }
    uint32_t before;
    uint32_t after;
    do {
        before = atomic_load_explicit(&s_status_sequence,
                                      memory_order_acquire);
        if (before == 0 || (before & 1U) != 0) {
            return false;
        }
        out_status->duration_ms = atomic_load_explicit(
            &s_status_duration_ms, memory_order_relaxed);
        out_status->position_ms = atomic_load_explicit(
            &s_status_position_ms, memory_order_relaxed);
        out_status->playing = atomic_load_explicit(
            &s_status_playing, memory_order_relaxed);
        after = atomic_load_explicit(&s_status_sequence,
                                     memory_order_acquire);
    } while (before != after);
    *out_revision = after;
    return true;
}
