/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_MOSAIC_UI_BLUETOOTH_AUDIO

#include "esp_a2dp_api.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define A2DP_PROFILE_INIT_DONE_BIT     BIT0
#define A2DP_PROFILE_DEINIT_DONE_BIT   BIT1
#define A2DP_PROFILE_EVENT_BITS        (A2DP_PROFILE_INIT_DONE_BIT | \
                                        A2DP_PROFILE_DEINIT_DONE_BIT)

#if CONFIG_BT_A2DP_CODEC_AAC_ENABLED && CONFIG_BT_A2DP_SEP_NUM_MAX < 2
#error "AAC plus SBC requires CONFIG_BT_A2DP_SEP_NUM_MAX >= 2"
#endif

/* BlueDroid exposes one process-wide A2DP callback, so this bridge is also a
 * process-wide singleton. The static event group exists for the firmware
 * lifetime and therefore cannot race a late Bluetooth callback during delete.
 */
static DRAM_ATTR StaticEventGroup_t s_profile_event_storage;
static EXT_RAM_BSS_ATTR EventGroupHandle_t s_profile_events;
static EXT_RAM_BSS_ATTR esp_a2d_cb_t s_client_callback;

esp_err_t __real_esp_a2d_sink_init(void);
esp_err_t __real_esp_a2d_sink_deinit(void);
esp_err_t __real_esp_a2d_register_callback(esp_a2d_cb_t callback);

static EventGroupHandle_t profile_events_get(void)
{
    if (!s_profile_events) {
        s_profile_events = xEventGroupCreateStatic(&s_profile_event_storage);
    }
    return s_profile_events;
}

static void profile_event_callback(esp_a2d_cb_event_t event,
                                   esp_a2d_cb_param_t *param)
{
    esp_a2d_cb_t callback = s_client_callback;
    if (callback) {
        callback(event, param);
    }
    if (event != ESP_A2D_PROF_STATE_EVT || !param) {
        return;
    }

    EventBits_t completed =
        param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS ?
            A2DP_PROFILE_INIT_DONE_BIT : A2DP_PROFILE_DEINIT_DONE_BIT;
    xEventGroupSetBits(profile_events_get(), completed);
}

/* Install the lifecycle observer before queuing profile init. This makes the
 * init-complete event impossible to miss, then converts the asynchronous IDF
 * operation into the synchronous contract expected by esp_bt_audio 1.1.0.
 */
esp_err_t __wrap_esp_a2d_sink_init(void)
{
    EventGroupHandle_t events = profile_events_get();
    xEventGroupClearBits(events, A2DP_PROFILE_EVENT_BITS);
    s_client_callback = NULL;

    ESP_RETURN_ON_ERROR(
        __real_esp_a2d_register_callback(profile_event_callback),
        "mosaic_bt_a2dp", "register lifecycle callback");
    ESP_RETURN_ON_ERROR(__real_esp_a2d_sink_init(), "mosaic_bt_a2dp",
                        "start A2DP profile");

    (void)xEventGroupWaitBits(events, A2DP_PROFILE_INIT_DONE_BIT, pdTRUE,
                              pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

/* Keep the lifecycle observer installed and forward subsequent events to the
 * callback supplied by esp_bt_audio. Its init notification has already been
 * consumed solely as the synchronization point above.
 */
esp_err_t __wrap_esp_a2d_register_callback(esp_a2d_cb_t callback)
{
    if (!callback) {
        return ESP_FAIL;
    }
    s_client_callback = callback;
    return ESP_OK;
}

/* Do not let esp_bt_audio disable BlueDroid while A2DP teardown is still
 * queued. Waiting for the matching profile event also keeps its callback
 * context alive until BlueDroid has finished using it.
 */
esp_err_t __wrap_esp_a2d_sink_deinit(void)
{
    EventGroupHandle_t events = profile_events_get();
    xEventGroupClearBits(events, A2DP_PROFILE_DEINIT_DONE_BIT);

    esp_err_t err = __real_esp_a2d_sink_deinit();
    if (err != ESP_OK) {
        return err;
    }
    (void)xEventGroupWaitBits(events, A2DP_PROFILE_DEINIT_DONE_BIT, pdTRUE,
                              pdTRUE, portMAX_DELAY);
    s_client_callback = NULL;
    return ESP_OK;
}

#endif /* CONFIG_MOSAIC_UI_BLUETOOTH_AUDIO */
