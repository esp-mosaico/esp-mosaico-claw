/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_setup.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_gsp.h"
#include "esp_log.h"
#include "qrcodegen.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif
#include "mosaic_app_catalog.h"
#include "mosaic_app_shell.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "mosaic_settings.h"
#include "mosaic_top_notice.h"
#include "wechat_binding_flow.h"
#include "setup_center_actions.h"
#include "setup_center_binds.h"
#include "setup_center_objects.h"
#include "setup_center_wechat_queue.h"
#if defined(ESP_PLATFORM)
#include "mosaic_loader.h"
#endif

#define SETUP_CENTER_APP_ID 10U
#define SETUP_PAGE_OVERVIEW 0U
#define SETUP_PAGE_NETWORK  1U
#define SETUP_PAGE_WECHAT   2U
#define SETUP_PAGE_LLM      3U
#define SETUP_PAGE_DONE     4U
#define SETUP_PAGE_INTEGRATIONS 5U
#define SETUP_NETWORK_MOCK_COUNT 10U
#define SETUP_PASSWORD_MIN  8U
#define SETUP_PROGRESS_COUNT 12U
#define SETUP_NAV_GUARD_US  400000LL
#define SETUP_NETWORK_SCAN_US 900000LL
#define SETUP_NETWORK_JOIN_US 3000000LL
#define SETUP_WECHAT_REAL_STAGE_US 500000LL
#define SETUP_LLM_CONFIG_US 2200000LL
#define SETUP_QR_SIZE 256U
#define SETUP_QR_STRIDE (SETUP_QR_SIZE * 2U)
#define SETUP_QR_BYTES ((size_t)SETUP_QR_STRIDE * SETUP_QR_SIZE)
#define SETUP_QR_FRAME_COUNT 3U
#define SETUP_QR_QUIET_MODULES 4
#define SETUP_LLM_QR_SIZE 104U
#define SETUP_LLM_QR_STRIDE (SETUP_LLM_QR_SIZE * 2U)
#define SETUP_LLM_URL_LEN \
    (MOSAIC_SETTINGS_PORTAL_URL_LEN + sizeof("#llm"))
#define SETUP_COLOR_PROGRESS_OFF UINT32_C(0x39E7)
#define SETUP_COLOR_PROGRESS_ON  UINT32_C(0xEA04)

typedef enum {
    SETUP_NETWORK_SCAN = 0,
    SETUP_NETWORK_PASSWORD,
    SETUP_NETWORK_PHONE,
    SETUP_NETWORK_JOINING,
    SETUP_NETWORK_CONNECTED,
} setup_network_phase_t;

typedef enum {
    SETUP_WECHAT_BINDING = 0,
    SETUP_WECHAT_PROGRESS,
    SETUP_WECHAT_SUCCESS,
} setup_wechat_phase_t;

typedef enum {
    SETUP_WECHAT_INTENT_VIEW_STATUS = 0,
    SETUP_WECHAT_INTENT_BIND_ACTIVE,
    SETUP_WECHAT_INTENT_REBIND_ACTIVE,
    SETUP_WECHAT_INTENT_NEWLY_COMPLETED,
} setup_wechat_intent_t;

typedef enum {
    SETUP_WECHAT_ENTRY_APP = 0,
    SETUP_WECHAT_ENTRY_ONBOARDING,
} setup_wechat_entry_t;

typedef enum {
    SETUP_LLM_STATUS = 0,
    SETUP_LLM_CONFIGURING,
    SETUP_LLM_PROGRESS,
    SETUP_LLM_SUCCESS,
} setup_llm_phase_t;

typedef struct {
    uint8_t *pixels;
    bool busy;
} setup_qr_frame_t;

typedef struct {
    esp_gsp_handle_t ui;
    esp_gsp_list_t network_list;
    mosaic_setup_mode_t mode;
    mosaic_setup_route_t route;
    uint16_t page;
    uint16_t overview_page;
    bool onboarding_complete;
    bool network_configured;
    bool network_skipped;
    bool network_newly_connected;
    bool wechat_configured;
    bool wechat_skipped;
    setup_wechat_intent_t wechat_intent;
    setup_wechat_entry_t wechat_entry;
    bool llm_configured;
    bool llm_skipped;
    setup_network_phase_t network_phase;
    setup_wechat_phase_t wechat_phase;
    wechat_binding_flow_t wechat_flow;
    setup_llm_phase_t llm_phase;
    char selected_ssid[MOSAIC_SETTINGS_SSID_LEN];
    char connected_ssid[MOSAIC_SETTINGS_SSID_LEN];
    bool network_join_success;
    bool network_scan_ready;
    int64_t network_scan_deadline_us;
    int64_t operation_started_us;
    int64_t operation_deadline_us;
    int64_t navigation_guard_until_us;
    uint8_t rendered_progress;
    uint8_t rendered_password_length;
    mosaic_settings_wifi_ap_t
        networks[MOSAIC_SETTINGS_WIFI_SCAN_MAX];
    size_t network_count;
    mosaic_settings_network_t network_status;
    uint32_t network_revision;
    uint32_t rendered_network_revision;
    bool network_connect_pending;
    bool wifi_subscribed;
    mosaic_setup_wechat_ops_t wechat_ops;
    mosaic_setup_wechat_status_t wechat_status;
    uint32_t wechat_revision;
    uint32_t rendered_wechat_revision;
    uint32_t wechat_operation_revision;
    setup_wechat_event_queue_t wechat_events;
    int64_t wechat_stage_deadline_us;
    uint8_t *qr_temp;
    uint8_t *qr_code;
    setup_qr_frame_t qr_frames[SETUP_QR_FRAME_COUNT];
    char rendered_wechat_qr[MOSAIC_SETUP_WECHAT_QR_LEN];
    char pending_wechat_qr[MOSAIC_SETUP_WECHAT_QR_LEN];
    char rendered_llm_qr[SETUP_LLM_URL_LEN];
    char rendered_network_phone_qr[MOSAIC_SETTINGS_PHONE_QR_LEN];
    char llm_config_url[SETUP_LLM_URL_LEN];
    mosaic_settings_snapshot_t settings_snapshot;
} setup_center_state_t;

static const char *TAG = "setup_center";
static mosaic_setup_app_request_cb_t s_app_request;
static void *s_app_request_ctx;
static setup_center_state_t s_setup = {
    .network_list = ESP_GSP_LIST_NONE,
};
static bool s_pending_entry;
static mosaic_setup_mode_t s_pending_mode;
static mosaic_setup_route_t s_pending_route;
static uint32_t s_pending_revision;
static char s_return_app[32];

#if defined(ESP_PLATFORM)
static portMUX_TYPE s_setup_model_lock = portMUX_INITIALIZER_UNLOCKED;
#define SETUP_MODEL_LOCK() portENTER_CRITICAL(&s_setup_model_lock)
#define SETUP_MODEL_UNLOCK() portEXIT_CRITICAL(&s_setup_model_lock)
#else
#define SETUP_MODEL_LOCK() ((void)0)
#define SETUP_MODEL_UNLOCK() ((void)0)
#endif

static const mosaic_top_notice_config_t s_notice = {
    .visible_bind = GSP_BIND_SETUP_TOP_NOTICE_VISIBLE,
    .title_bind = GSP_BIND_SETUP_TOP_NOTICE_TITLE,
    .message_bind = GSP_BIND_SETUP_TOP_NOTICE_MESSAGE,
};

static const uint16_t s_network_progress[SETUP_PROGRESS_COUNT] = {
    GSP_BIND_SETUP_NETWORK_PROGRESS_01,
    GSP_BIND_SETUP_NETWORK_PROGRESS_02,
    GSP_BIND_SETUP_NETWORK_PROGRESS_03,
    GSP_BIND_SETUP_NETWORK_PROGRESS_04,
    GSP_BIND_SETUP_NETWORK_PROGRESS_05,
    GSP_BIND_SETUP_NETWORK_PROGRESS_06,
    GSP_BIND_SETUP_NETWORK_PROGRESS_07,
    GSP_BIND_SETUP_NETWORK_PROGRESS_08,
    GSP_BIND_SETUP_NETWORK_PROGRESS_09,
    GSP_BIND_SETUP_NETWORK_PROGRESS_10,
    GSP_BIND_SETUP_NETWORK_PROGRESS_11,
    GSP_BIND_SETUP_NETWORK_PROGRESS_12,
};

static const uint16_t s_wechat_progress[SETUP_PROGRESS_COUNT] = {
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_01,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_02,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_03,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_04,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_05,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_06,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_07,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_08,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_09,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_10,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_11,
    GSP_BIND_SETUP_WECHAT_PROGRESS_BAR_12,
};

static const uint16_t s_llm_progress[SETUP_PROGRESS_COUNT] = {
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_01,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_02,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_03,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_04,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_05,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_06,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_07,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_08,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_09,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_10,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_11,
    GSP_BIND_SETUP_LLM_PROGRESS_BAR_12,
};

esp_err_t mosaic_setup_configure_wechat(
    const mosaic_setup_wechat_ops_t *ops)
{
    if (ops == NULL || ops->start == NULL || ops->cancel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SETUP_MODEL_LOCK();
    s_setup.wechat_ops = *ops;
    SETUP_MODEL_UNLOCK();
    return ESP_OK;
}

esp_err_t mosaic_setup_set_wechat_status(
    const mosaic_setup_wechat_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SETUP_MODEL_LOCK();
    s_setup.wechat_status = *status;
    const uint32_t revision = ++s_setup.wechat_revision;
    /* The binding worker may publish SCANNED, SAVING and COMPLETE before
     * the next UI tick. Preserve every observable transition while this
     * page is active instead of reducing the stream to its latest value. */
    const bool observable =
        status->state != MOSAIC_SETUP_WECHAT_IDLE;
    if (observable && s_setup.ui != NULL &&
            s_setup.page == SETUP_PAGE_WECHAT) {
        setup_wechat_event_queue_push(
            &s_setup.wechat_events, status, revision);
    }
    SETUP_MODEL_UNLOCK();
    return ESP_OK;
}

esp_err_t mosaic_setup_get_wechat_service(
    mosaic_setup_wechat_ops_t *ret_ops,
    mosaic_setup_wechat_status_t *ret_status,
    uint32_t *ret_revision)
{
    if (ret_ops == NULL || ret_status == NULL || ret_revision == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SETUP_MODEL_LOCK();
    *ret_ops = s_setup.wechat_ops;
    *ret_status = s_setup.wechat_status;
    *ret_revision = s_setup.wechat_revision;
    SETUP_MODEL_UNLOCK();
    return ESP_OK;
}

static bool setup_wechat_has_backend(void)
{
    bool configured;
    SETUP_MODEL_LOCK();
    configured = wechat_binding_flow_has_backend(&s_setup.wechat_ops);
    SETUP_MODEL_UNLOCK();
    return configured;
}

static void *setup_qr_alloc(size_t bytes)
{
#if defined(ESP_PLATFORM)
    void *memory = heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_malloc(
            bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return memory;
#else
    return malloc(bytes);
#endif
}

static esp_err_t setup_qr_ensure_buffers(void)
{
    if (s_setup.qr_temp == NULL) {
        s_setup.qr_temp = setup_qr_alloc(qrcodegen_BUFFER_LEN_MAX);
    }
    if (s_setup.qr_code == NULL) {
        s_setup.qr_code = setup_qr_alloc(qrcodegen_BUFFER_LEN_MAX);
    }
    if (s_setup.qr_temp == NULL || s_setup.qr_code == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < SETUP_QR_FRAME_COUNT; ++i) {
        if (s_setup.qr_frames[i].pixels == NULL) {
            s_setup.qr_frames[i].pixels = setup_qr_alloc(SETUP_QR_BYTES);
        }
        if (s_setup.qr_frames[i].pixels == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void setup_qr_released(void *user_ctx)
{
    setup_qr_frame_t *frame = user_ctx;
    if (frame == NULL) {
        return;
    }
    SETUP_MODEL_LOCK();
    frame->busy = false;
    SETUP_MODEL_UNLOCK();
}

static setup_qr_frame_t *setup_qr_acquire(void)
{
    setup_qr_frame_t *frame = NULL;
    SETUP_MODEL_LOCK();
    for (size_t i = 0; i < SETUP_QR_FRAME_COUNT; ++i) {
        if (!s_setup.qr_frames[i].busy &&
                s_setup.qr_frames[i].pixels != NULL) {
            s_setup.qr_frames[i].busy = true;
            frame = &s_setup.qr_frames[i];
            break;
        }
    }
    SETUP_MODEL_UNLOCK();
    return frame;
}

static bool setup_qr_render(
    const char *payload, uint8_t *pixels, uint16_t size)
{
    if (payload == NULL || payload[0] == '\0' || pixels == NULL ||
            !qrcodegen_encodeText(payload, s_setup.qr_temp, s_setup.qr_code,
                qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return false;
    }
    const int modules = qrcodegen_getSize(s_setup.qr_code);
    const int extent = modules + SETUP_QR_QUIET_MODULES * 2;
    if (extent > size) {
        return false;
    }
    uint16_t *frame = (uint16_t *)pixels;
    for (size_t i = 0; i < (size_t)size * size; ++i) {
        frame[i] = UINT16_MAX;
    }
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(s_setup.qr_code, x, y)) {
                continue;
            }
            const int px0 =
                (x + SETUP_QR_QUIET_MODULES) * size / extent;
            const int px1 =
                (x + SETUP_QR_QUIET_MODULES + 1) * size / extent;
            const int py0 =
                (y + SETUP_QR_QUIET_MODULES) * size / extent;
            const int py1 =
                (y + SETUP_QR_QUIET_MODULES + 1) * size / extent;
            for (int py = py0; py < py1; ++py) {
                uint16_t *row = frame + (size_t)py * size;
                for (int px = px0; px < px1; ++px) {
                    row[px] = 0;
                }
            }
        }
    }
    return true;
}

static void setup_wechat_push_qr(const char *payload, int64_t now_us)
{
    if (s_setup.ui == NULL || payload == NULL || payload[0] == '\0' ||
            strcmp(payload, s_setup.rendered_wechat_qr) == 0) {
        return;
    }
    /* StackView captures its transition target when the navigation command
     * is applied.  A Canvas push in the same transition window creates a
     * second full-resource present on top of that frozen target.  Keep the
     * verified payload, then publish it once after the page animation has
     * retired so the high-contrast QR reaches the panel in one stable frame. */
    if (payload != s_setup.pending_wechat_qr) {
        strlcpy(s_setup.pending_wechat_qr, payload,
                sizeof(s_setup.pending_wechat_qr));
    }
    if (now_us < s_setup.navigation_guard_until_us) {
        return;
    }
    /* Operations may be registered after the App instance was created.
     * Allocate lazily as well as at start so the real service can always
     * replace the authored placeholder. */
    if (setup_qr_ensure_buffers() != ESP_OK) {
        ESP_LOGW(TAG, "allocate WeChat QR buffers failed");
        return;
    }
    setup_qr_frame_t *frame = setup_qr_acquire();
    if (frame == NULL) {
        ESP_LOGW(TAG, "no free WeChat QR frame");
        return;
    }
    if (!setup_qr_render(payload, frame->pixels, SETUP_QR_SIZE)) {
        setup_qr_released(frame);
        ESP_LOGW(TAG, "render WeChat QR failed");
        return;
    }
    const esp_gsp_err_t err = esp_gsp_canvas_push(
        s_setup.ui, GSP_BIND_SETUP_WECHAT_QR_CANVAS,
        frame->pixels, SETUP_QR_STRIDE, setup_qr_released, frame);
    if (err != ESP_GSP_OK) {
        setup_qr_released(frame);
        ESP_LOGW(TAG, "push WeChat QR failed: %d", (int)err);
        return;
    }
    strlcpy(s_setup.rendered_wechat_qr, payload,
            sizeof(s_setup.rendered_wechat_qr));
    s_setup.pending_wechat_qr[0] = '\0';
}

static void setup_llm_push_qr(const char *payload)
{
    if (s_setup.ui == NULL || payload == NULL || payload[0] == '\0' ||
            strcmp(payload, s_setup.rendered_llm_qr) == 0) {
        return;
    }
    if (setup_qr_ensure_buffers() != ESP_OK) {
        return;
    }
    setup_qr_frame_t *frame = setup_qr_acquire();
    if (frame == NULL) {
        return;
    }
    if (!setup_qr_render(payload, frame->pixels, SETUP_LLM_QR_SIZE)) {
        setup_qr_released(frame);
        return;
    }
    const esp_gsp_err_t err = esp_gsp_canvas_push(
        s_setup.ui, GSP_BIND_SETUP_LLM_CONFIG_QR_CANVAS,
        frame->pixels, SETUP_LLM_QR_STRIDE, setup_qr_released, frame);
    if (err != ESP_GSP_OK) {
        setup_qr_released(frame);
        return;
    }
    strlcpy(s_setup.rendered_llm_qr, payload,
            sizeof(s_setup.rendered_llm_qr));
}

static esp_err_t setup_network_phone_refresh(void)
{
    char ap_ssid[MOSAIC_SETTINGS_SSID_LEN] = {0};
    char *payload = calloc(1, MOSAIC_SETTINGS_PHONE_QR_LEN);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = mosaic_settings_get_phone_setup(
        ap_ssid, sizeof(ap_ssid), payload, MOSAIC_SETTINGS_PHONE_QR_LEN);
#if !defined(ESP_PLATFORM)
    if (err == ESP_ERR_NOT_SUPPORTED) {
        strlcpy(ap_ssid, "esp-claw-SIM", sizeof(ap_ssid));
        strlcpy(payload, "WIFI:T:nopass;S:esp-claw-SIM;;",
                MOSAIC_SETTINGS_PHONE_QR_LEN);
        err = ESP_OK;
    }
#endif
    if (err == ESP_OK) {
        (void)gsp_setup_center_setup_network_phone_ap_ssid_set_text(
            s_setup.ui, ap_ssid);
        if (strcmp(payload, s_setup.rendered_network_phone_qr) != 0 &&
                setup_qr_ensure_buffers() == ESP_OK) {
            setup_qr_frame_t *frame = setup_qr_acquire();
            if (frame != NULL &&
                    setup_qr_render(payload, frame->pixels, SETUP_QR_SIZE)) {
                if (esp_gsp_canvas_push(
                        s_setup.ui,
                        GSP_BIND_SETUP_NETWORK_PHONE_QR_CANVAS,
                        frame->pixels, SETUP_QR_STRIDE,
                        setup_qr_released, frame) == ESP_GSP_OK) {
                    strlcpy(s_setup.rendered_network_phone_qr, payload,
                            sizeof(s_setup.rendered_network_phone_qr));
                } else {
                    setup_qr_released(frame);
                }
            } else if (frame != NULL) {
                setup_qr_released(frame);
            }
        }
    }
    free(payload);
    return err;
}

static void setup_network_event(
    const mosaic_settings_network_t *status, void *user_ctx)
{
    (void)user_ctx;
    if (status == NULL) {
        return;
    }
    SETUP_MODEL_LOCK();
    if (memcmp(&s_setup.network_status, status, sizeof(*status)) != 0) {
        s_setup.network_status = *status;
        ++s_setup.network_revision;
    }
    SETUP_MODEL_UNLOCK();
}

static void setup_network_seed_mock(void)
{
    static const struct {
        const char *ssid;
        bool secured;
    } networks[SETUP_NETWORK_MOCK_COUNT] = {
        { "AWifi-2.4G", true },
        { "BWifi-2.4G", true },
        { "CWifi-2.4G", true },
        { "Orange-2.4G", true },
        { "Orange-5G", true },
        { "ESP-Guest", false },
        { "Mosaico-Lab", true },
        { "Studio-IoT", true },
        { "Mobile-Hotspot", true },
        { "Guest-WiFi", false },
    };
    SETUP_MODEL_LOCK();
    s_setup.network_count = SETUP_NETWORK_MOCK_COUNT;
    for (size_t i = 0; i < SETUP_NETWORK_MOCK_COUNT; ++i) {
        strlcpy(s_setup.networks[i].ssid, networks[i].ssid,
                sizeof(s_setup.networks[i].ssid));
        s_setup.networks[i].secured = networks[i].secured;
        s_setup.networks[i].rssi = (int8_t)(-35 - (int)i * 4);
    }
    SETUP_MODEL_UNLOCK();
}

static bool setup_network_refresh_scan(void)
{
    mosaic_settings_wifi_ap_t records[MOSAIC_SETTINGS_WIFI_SCAN_MAX] = {0};
    size_t count = 0;
    if (!mosaic_settings_wifi_backend_available()) {
        setup_network_seed_mock();
        return true;
    }
    if (mosaic_settings_scan_wifi(records, MOSAIC_SETTINGS_WIFI_SCAN_MAX,
            &count) != ESP_OK) {
        return false;
    }
    if (count > MOSAIC_SETTINGS_WIFI_SCAN_MAX) {
        count = MOSAIC_SETTINGS_WIFI_SCAN_MAX;
    }
    SETUP_MODEL_LOCK();
    if (count > 0) {
        memcpy(s_setup.networks, records, count * sizeof(records[0]));
    }
    s_setup.network_count = count;
    SETUP_MODEL_UNLOCK();
    return true;
}

static void setup_network_request_scan(int64_t now_us)
{
    s_setup.network_scan_ready = false;
    s_setup.network_scan_deadline_us = now_us + SETUP_NETWORK_SCAN_US;
    SETUP_MODEL_LOCK();
    s_setup.network_count = 0;
    SETUP_MODEL_UNLOCK();
    const esp_err_t err = mosaic_settings_request_wifi_scan();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "request Wi-Fi scan failed: %s",
                 esp_err_to_name(err));
    }
}

static void setup_navigation_render(void)
{
    if (s_setup.ui == NULL) {
        return;
    }
    /* Onboarding is deliberately forward-only: Skip advances without
     * pretending that a step was configured.  The same pages are reused by
     * Setup management, where their back affordances become visible. */
    const bool visible = s_setup.mode == MOSAIC_SETUP_MODE_MANAGE;
    const bool onboarding = !visible;
    (void)gsp_setup_center_setup_network_back_visible_set_visible(
        s_setup.ui, visible);
    (void)gsp_setup_center_setup_network_skip_visible_set_visible(
        s_setup.ui, onboarding);
    (void)gsp_setup_center_setup_wechat_binding_back_visible_set_visible(
        s_setup.ui, visible);
    (void)gsp_setup_center_setup_wechat_binding_skip_visible_set_visible(
        s_setup.ui, onboarding);
    (void)gsp_setup_center_setup_llm_status_back_visible_set_visible(
        s_setup.ui, visible);
    (void)gsp_setup_center_setup_llm_status_skip_visible_set_visible(
        s_setup.ui, onboarding);
    (void)gsp_setup_center_setup_llm_configuring_back_visible_set_visible(
        s_setup.ui, visible);
    (void)gsp_setup_center_setup_llm_configuring_skip_visible_set_visible(
        s_setup.ui, onboarding);
}

static gsp_err_t setup_network_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item, void *user_ctx)
{
    (void)user_ctx;
    char ssid[MOSAIC_SETTINGS_SSID_LEN];
    SETUP_MODEL_LOCK();
    if (item >= s_setup.network_count) {
        SETUP_MODEL_UNLOCK();
        return GSP_ERR_INVALID_ARG;
    }
    strlcpy(ssid, s_setup.networks[item].ssid, sizeof(ssid));
    SETUP_MODEL_UNLOCK();
    const gsp_err_t err = esp_gsp_row_text(ui, row, ssid);
    if (err != GSP_OK) {
        ESP_LOGW(TAG, "bind mock WLAN row %u failed: %d",
                 (unsigned)item, (int)err);
    }
    return err;
}

static void setup_network_list_attach(void)
{
    if (s_setup.ui == NULL) {
        return;
    }
    if (s_setup.network_list == ESP_GSP_LIST_NONE) {
        s_setup.network_list = esp_gsp_list_bind_component(
            s_setup.ui, GSP_OBJ_KEY_SETUP_NETWORK_LIST,
            setup_network_bind_item, NULL);
    }
    if (s_setup.network_list == ESP_GSP_LIST_NONE) {
        ESP_LOGW(TAG, "bind mock WLAN list failed");
        return;
    }
    if (esp_gsp_list_set_total(s_setup.ui, s_setup.network_list,
            s_setup.network_count) == ESP_GSP_OK) {
        (void)esp_gsp_list_refresh(s_setup.ui, s_setup.network_list);
    }
}

static void setup_network_list_park(void)
{
    if (s_setup.ui != NULL &&
            s_setup.network_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_set_total(s_setup.ui, s_setup.network_list, 0);
        (void)esp_gsp_list_refresh(s_setup.ui, s_setup.network_list);
    }
}

static void setup_progress_render(const uint16_t *binds, uint8_t progress)
{
    if (s_setup.ui == NULL) {
        return;
    }
    if (progress > SETUP_PROGRESS_COUNT) {
        progress = SETUP_PROGRESS_COUNT;
    }
    for (uint8_t index = 0; index < SETUP_PROGRESS_COUNT; ++index) {
        (void)esp_gsp_set_color(s_setup.ui, binds[index],
            index < progress ? SETUP_COLOR_PROGRESS_ON
                             : SETUP_COLOR_PROGRESS_OFF);
    }
    s_setup.rendered_progress = progress;
}

static void setup_overview_render(void)
{
    if (s_setup.ui == NULL) {
        return;
    }
    const char *network = "Not configured";
    if (s_setup.network_configured) {
        network = s_setup.connected_ssid;
    } else if (s_setup.network_skipped) {
        network = "Skipped · Not configured";
    }
    const char *wechat = s_setup.wechat_configured
        ? "Mosaico-Lab · Linked"
        : (s_setup.wechat_skipped ? "Skipped · Not linked" : "Not linked");
    const char *llm = s_setup.llm_configured
        ? (s_setup.settings_snapshot.llm.model[0]
            ? s_setup.settings_snapshot.llm.model : "Configured")
        : (s_setup.llm_skipped ? "Skipped · Not configured"
                               : "Not configured");
    (void)gsp_setup_center_setup_network_summary_set_text(
        s_setup.ui, network);
    (void)gsp_setup_center_setup_wechat_summary_set_text(
        s_setup.ui, wechat);
    (void)gsp_setup_center_setup_llm_summary_set_text(s_setup.ui, llm);
    (void)gsp_setup_center_setup_integrations_wechat_summary_set_text(
        s_setup.ui, wechat);
    (void)gsp_setup_center_setup_integrations_llm_summary_set_text(
        s_setup.ui, llm);
}

static void setup_password_detach(void)
{
    if (s_setup.ui == NULL) {
        return;
    }
    mosaic_app_shell_set_bottom_enabled(s_setup.ui, true);
    (void)esp_gsp_set_cursor(s_setup.ui, ESP_GSP_NO_CURSOR);
    (void)esp_gsp_keyboard_attach(s_setup.ui, ESP_GSP_KEYBOARD_NONE,
                                  GSP_BIND_SETUP_PASSWORD_VALUE);
    (void)esp_gsp_set_text(s_setup.ui, GSP_BIND_SETUP_PASSWORD_VALUE, "");
}

static void setup_network_render(void)
{
    const bool scan = s_setup.network_phase == SETUP_NETWORK_SCAN;
    const bool password = s_setup.network_phase == SETUP_NETWORK_PASSWORD;
    const bool phone = s_setup.network_phase == SETUP_NETWORK_PHONE;
    const bool joining = s_setup.network_phase == SETUP_NETWORK_JOINING;
    const bool connected = s_setup.network_phase == SETUP_NETWORK_CONNECTED;
    (void)gsp_setup_center_setup_network_scan_set_visible(s_setup.ui, scan);
    (void)gsp_setup_center_setup_network_password_set_visible(
        s_setup.ui, password);
    (void)gsp_setup_center_setup_network_phone_set_visible(
        s_setup.ui, phone);
    (void)gsp_setup_center_setup_network_joining_set_visible(
        s_setup.ui, joining);
    (void)gsp_setup_center_setup_network_connected_set_visible(
        s_setup.ui, connected);
    (void)gsp_setup_center_setup_network_connected_back_visible_set_visible(
        s_setup.ui, connected && !s_setup.network_newly_connected);
    (void)gsp_setup_center_setup_network_connected_continue_visible_set_visible(
        s_setup.ui, connected && s_setup.network_newly_connected);
    (void)gsp_setup_center_setup_network_connected_change_visible_set_visible(
        s_setup.ui, connected && !s_setup.network_newly_connected);
    (void)gsp_setup_center_setup_network_spinner_set_visible(
        s_setup.ui, scan && !s_setup.network_scan_ready);
    (void)gsp_setup_center_setup_network_refresh_set_visible(
        s_setup.ui, scan && s_setup.network_scan_ready);
    (void)gsp_setup_center_setup_network_panel_set_visible(
        s_setup.ui, scan && s_setup.network_scan_ready);
    if (scan && s_setup.network_scan_ready) {
        setup_network_list_attach();
    } else {
        setup_network_list_park();
    }
    if (joining) {
        (void)gsp_setup_center_setup_joining_ssid_set_text(
            s_setup.ui, s_setup.selected_ssid);
    }
    if (connected) {
        (void)gsp_setup_center_setup_connected_ssid_set_text(
            s_setup.ui, s_setup.connected_ssid);
    }
}

static void setup_wechat_render(void)
{
    const bool binding = s_setup.wechat_phase == SETUP_WECHAT_BINDING;
    const bool progress = s_setup.wechat_phase == SETUP_WECHAT_PROGRESS;
    const bool success = s_setup.wechat_phase == SETUP_WECHAT_SUCCESS;
    const bool onboarding =
        s_setup.wechat_entry == SETUP_WECHAT_ENTRY_ONBOARDING;
    (void)gsp_setup_center_setup_wechat_binding_set_visible(
        s_setup.ui, binding);
    (void)gsp_setup_center_setup_wechat_progress_set_visible(
        s_setup.ui, progress);
    (void)gsp_setup_center_setup_wechat_success_set_visible(
        s_setup.ui, success);
    (void)gsp_setup_center_setup_wechat_success_back_visible_set_visible(
        s_setup.ui, success && !onboarding);
    (void)gsp_setup_center_setup_wechat_success_get_started_visible_set_visible(
        s_setup.ui, success && onboarding);
    (void)gsp_setup_center_setup_wechat_success_rebind_visible_set_visible(
        s_setup.ui, success && !onboarding);
}

static setup_wechat_intent_t setup_wechat_success_intent(void)
{
    return s_setup.wechat_entry == SETUP_WECHAT_ENTRY_ONBOARDING
        ? SETUP_WECHAT_INTENT_NEWLY_COMPLETED
        : SETUP_WECHAT_INTENT_VIEW_STATUS;
}

static esp_err_t setup_wechat_start_service(bool force)
{
    mosaic_setup_wechat_ops_t ops;
    mosaic_setup_wechat_status_t status;
    SETUP_MODEL_LOCK();
    ops = s_setup.wechat_ops;
    setup_wechat_event_queue_reset(&s_setup.wechat_events);
    s_setup.wechat_stage_deadline_us = 0;
    /* A configured service retains its previous COMPLETE snapshot while a
     * forced login worker is being created. Only revisions published after
     * this boundary belong to the new bind/rebind operation. */
    s_setup.wechat_operation_revision = s_setup.wechat_revision;
    status = s_setup.wechat_status;
    SETUP_MODEL_UNLOCK();
    const esp_err_t err =
        wechat_binding_flow_start_backend(&ops, &status, force);
    if (err != ESP_OK && s_setup.ui != NULL) {
        (void)mosaic_top_notice_show(
            s_setup.ui, &s_notice, "Unable to bind WeChat",
            esp_err_to_name(err), 2600);
    }
    return err;
}

#if defined(ESP_PLATFORM)
static void setup_wechat_cancel_service(void)
{
    mosaic_setup_wechat_ops_t ops;
    SETUP_MODEL_LOCK();
    ops = s_setup.wechat_ops;
    SETUP_MODEL_UNLOCK();
    (void)wechat_binding_flow_cancel_backend(&ops);
}
#endif

static void setup_wechat_disarm_operation(void)
{
    SETUP_MODEL_LOCK();
    setup_wechat_event_queue_reset(&s_setup.wechat_events);
    s_setup.wechat_stage_deadline_us = 0;
    s_setup.wechat_operation_revision = s_setup.wechat_revision;
    SETUP_MODEL_UNLOCK();
    s_setup.operation_deadline_us = 0;
    s_setup.wechat_intent = SETUP_WECHAT_INTENT_VIEW_STATUS;
    s_setup.wechat_phase = s_setup.wechat_configured
        ? SETUP_WECHAT_SUCCESS : SETUP_WECHAT_BINDING;
}

#if defined(ESP_PLATFORM)
static void setup_wechat_cancel_deferred(void *user_ctx)
{
    (void)user_ctx;
    setup_wechat_cancel_service();
}

static void setup_wechat_schedule_cancel(void)
{
    const esp_err_t err = mosaic_loader_defer(
        setup_wechat_cancel_deferred, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "defer WeChat cancellation failed: %s",
                 esp_err_to_name(err));
        /* Never fall back to a synchronous service call here: this action is
         * dispatched while GSP owns the runtime lock, while cancellation may
         * wait for the binding worker/gateway.  Keeping the UI route usable is
         * safer than turning an exceptional deferred-slot failure into a
         * loader deadlock. */
    }
}
#else
static void setup_wechat_schedule_cancel(void)
{
}
#endif

static void setup_wechat_apply_model(int64_t now_us)
{
    if (s_setup.ui == NULL || !setup_wechat_has_backend()) {
        return;
    }
    mosaic_setup_wechat_status_t status;
    uint32_t revision;
    bool queued = false;
    SETUP_MODEL_LOCK();
    revision = s_setup.wechat_revision;
    status = s_setup.wechat_status;
    if (s_setup.wechat_events.count > 0U) {
        if (s_setup.wechat_stage_deadline_us > now_us) {
            SETUP_MODEL_UNLOCK();
            return;
        }
        setup_wechat_event_t event;
        queued = setup_wechat_event_queue_pop(
            &s_setup.wechat_events, &event);
        status = event.status;
        revision = event.revision;
        const bool intermediate =
            status.state == MOSAIC_SETUP_WECHAT_SCANNED ||
            status.state == MOSAIC_SETUP_WECHAT_SAVING ||
            status.state == MOSAIC_SETUP_WECHAT_AWAITING_SAVE;
        s_setup.wechat_stage_deadline_us = intermediate
            ? now_us + SETUP_WECHAT_REAL_STAGE_US : 0;
    }
    SETUP_MODEL_UNLOCK();
    const bool operation_active =
        s_setup.wechat_intent == SETUP_WECHAT_INTENT_BIND_ACTIVE ||
        s_setup.wechat_intent == SETUP_WECHAT_INTENT_REBIND_ACTIVE;
    if (!queued && operation_active &&
            revision <= s_setup.wechat_operation_revision) {
        /* Do not let the previous configured/COMPLETE snapshot terminate a
         * newly started operation before its worker publishes a new state. */
        return;
    }
    if (!queued && revision == s_setup.rendered_wechat_revision) {
        /* A canvas push can temporarily fail while StackView is changing
         * pages.  The service model has not changed in that case, so retry
         * the same verified login URL until the QR frame is accepted instead
         * of leaving the authored placeholder visible on the device. */
        if (s_setup.page == SETUP_PAGE_WECHAT &&
                status.state == MOSAIC_SETUP_WECHAT_WAITING_SCAN &&
                status.qr_payload[0] != '\0' &&
                strcmp(status.qr_payload,
                       s_setup.rendered_wechat_qr) != 0) {
            setup_wechat_push_qr(status.qr_payload, now_us);
        }
        return;
    }
    s_setup.rendered_wechat_revision = revision;
    (void)wechat_binding_flow_apply_status(
        &s_setup.wechat_flow, &status, operation_active);
    s_setup.wechat_configured = s_setup.wechat_flow.configured;
    s_setup.wechat_phase = s_setup.wechat_flow.phase ==
            WECHAT_BINDING_FLOW_SUCCESS
        ? SETUP_WECHAT_SUCCESS
        : (s_setup.wechat_flow.phase == WECHAT_BINDING_FLOW_PROGRESS
            ? SETUP_WECHAT_PROGRESS : SETUP_WECHAT_BINDING);
    if (s_setup.wechat_flow.stage[0] != '\0') {
        (void)gsp_setup_center_setup_wechat_stage_text_set_text(
            s_setup.ui, s_setup.wechat_flow.stage);
    }
    setup_progress_render(
        s_wechat_progress, s_setup.wechat_flow.progress);
    /* Setup Center keeps all steps in one StackView. Never consume a QR
     * payload while its page is hidden. */
    if (s_setup.page == SETUP_PAGE_WECHAT &&
            s_setup.wechat_flow.qr_payload[0] != '\0') {
        setup_wechat_push_qr(s_setup.wechat_flow.qr_payload, now_us);
    }
    if (status.state == MOSAIC_SETUP_WECHAT_COMPLETE) {
        s_setup.wechat_intent = setup_wechat_success_intent();
        if (s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING) {
            s_setup.onboarding_complete = true;
        }
    } else if ((status.state == MOSAIC_SETUP_WECHAT_CANCELLED ||
                status.state == MOSAIC_SETUP_WECHAT_EXPIRED ||
                status.state == MOSAIC_SETUP_WECHAT_ERROR) &&
            s_setup.wechat_configured) {
        s_setup.wechat_intent = setup_wechat_success_intent();
    }
    (void)gsp_setup_center_setup_wechat_bind_status_set_text(
        s_setup.ui, s_setup.wechat_flow.bind_status);
    setup_wechat_render();
    if (status.state == MOSAIC_SETUP_WECHAT_EXPIRED ||
            status.state == MOSAIC_SETUP_WECHAT_ERROR) {
        (void)mosaic_top_notice_show(
            s_setup.ui, &s_notice, "WeChat binding failed",
            s_setup.wechat_flow.bind_status, 2600);
    }
    setup_overview_render();
}

static void setup_llm_render(void)
{
    const bool status = s_setup.llm_phase == SETUP_LLM_STATUS;
    const bool configuring = s_setup.llm_phase == SETUP_LLM_CONFIGURING;
    const bool progress = s_setup.llm_phase == SETUP_LLM_PROGRESS;
    const bool success = s_setup.llm_phase == SETUP_LLM_SUCCESS;
    (void)gsp_setup_center_setup_llm_status_set_visible(s_setup.ui, status);
    (void)gsp_setup_center_setup_llm_configuring_set_visible(
        s_setup.ui, configuring);
    (void)gsp_setup_center_setup_llm_progress_set_visible(
        s_setup.ui, progress);
    (void)gsp_setup_center_setup_llm_success_set_visible(
        s_setup.ui, success);
}

static esp_err_t setup_llm_refresh_model(bool push_qr)
{
    mosaic_settings_snapshot_t snapshot = {0};
    const esp_err_t err = mosaic_settings_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return err;
    }
    s_setup.settings_snapshot = snapshot;
    s_setup.llm_configured =
        mosaic_settings_llm_is_configured(&snapshot);
    (void)gsp_setup_center_setup_llm_backend_set_text(
        s_setup.ui, snapshot.llm.backend[0]
            ? snapshot.llm.backend : "Not configured");
    (void)gsp_setup_center_setup_llm_model_set_text(
        s_setup.ui, snapshot.llm.model[0]
            ? snapshot.llm.model : "Not configured");
    (void)gsp_setup_center_setup_llm_base_url_set_text(
        s_setup.ui, snapshot.llm.base_url[0]
            ? snapshot.llm.base_url : "Not configured");
    char capabilities[48];
    (void)snprintf(capabilities, sizeof(capabilities),
        "Tools %s · Vision %s",
        snapshot.llm.supports_tools ? "On" : "Off",
        snapshot.llm.supports_vision ? "On" : "Off");
    (void)gsp_setup_center_setup_llm_capabilities_set_text(
        s_setup.ui, capabilities);

    char config_url[SETUP_LLM_URL_LEN] = {0};
    if (snapshot.network.portal_url[0] != '\0') {
        (void)snprintf(config_url, sizeof(config_url), "%s#llm",
                       snapshot.network.portal_url);
    }
    (void)gsp_setup_center_setup_llm_config_url_set_text(
        s_setup.ui, config_url[0] ? config_url : "Unavailable");
    strlcpy(s_setup.llm_config_url, config_url,
            sizeof(s_setup.llm_config_url));
    if (push_qr && config_url[0] != '\0') {
        setup_llm_push_qr(config_url);
    }
    return ESP_OK;
}

static bool setup_navigation_allowed(int64_t now_us)
{
    return now_us >= s_setup.navigation_guard_until_us;
}

static bool setup_push(uint16_t page, bool animated, int64_t now_us)
{
    if (s_setup.ui == NULL || page > SETUP_PAGE_INTEGRATIONS ||
            !setup_navigation_allowed(now_us)) {
        return false;
    }
    if (esp_gsp_stack_view_push(s_setup.ui, GSP_OBJ_KEY_SETUP_STACK,
            page, animated) != ESP_GSP_OK) {
        return false;
    }
    s_setup.page = page;
    s_setup.navigation_guard_until_us = now_us + SETUP_NAV_GUARD_US;
    return true;
}

static bool setup_pop(uint16_t landing_page, int64_t now_us)
{
    if (s_setup.ui == NULL || !setup_navigation_allowed(now_us)) {
        return false;
    }
    if (esp_gsp_stack_view_pop(
            s_setup.ui, GSP_OBJ_KEY_SETUP_STACK, true) != ESP_GSP_OK) {
        return false;
    }
    s_setup.page = landing_page;
    s_setup.navigation_guard_until_us = now_us + SETUP_NAV_GUARD_US;
    return true;
}

static void setup_open_network(int64_t now_us)
{
    const bool backend = mosaic_settings_wifi_backend_available();
    mosaic_settings_network_t network = {0};
    if (backend && mosaic_settings_get_wifi(&network) == ESP_OK) {
        setup_network_event(&network, NULL);
        s_setup.network_configured = network.connected;
        if (network.connected && network.ssid[0] != '\0') {
            strlcpy(s_setup.connected_ssid, network.ssid,
                    sizeof(s_setup.connected_ssid));
        }
    }
    /* A device can enter onboarding with credentials provisioned before
     * the Setup UI starts.  Treat that existing connection as a completed
     * onboarding step so the page offers Continue.  The same connection
     * opened from Settings remains a management view with Change Network. */
    s_setup.network_newly_connected =
        s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING &&
        s_setup.network_configured;
    s_setup.network_phase = s_setup.network_configured
        ? SETUP_NETWORK_CONNECTED : SETUP_NETWORK_SCAN;
    if (s_setup.network_phase == SETUP_NETWORK_SCAN) {
        if (backend) {
            setup_network_request_scan(now_us);
        } else {
            s_setup.network_scan_ready = false;
            s_setup.network_scan_deadline_us =
                now_us + SETUP_NETWORK_SCAN_US;
        }
    }
    setup_network_render();
    (void)setup_push(SETUP_PAGE_NETWORK, true, now_us);
}

static void setup_wechat_begin_simulation(int64_t now_us)
{
    /* Host-only behavior selected by the absence of platform operations.
     * It preserves the same visible state order as the real service while
     * keeping all device transitions exclusively event-driven. */
    wechat_binding_flow_begin_simulation(&s_setup.wechat_flow, now_us);
    s_setup.wechat_phase = SETUP_WECHAT_BINDING;
    s_setup.rendered_progress = UINT8_MAX;
    (void)gsp_setup_center_setup_wechat_bind_status_set_text(
        s_setup.ui, s_setup.wechat_flow.bind_status);
    setup_wechat_render();
    setup_wechat_push_qr(s_setup.wechat_flow.qr_payload, now_us);
}

static void setup_open_wechat(
    int64_t now_us, setup_wechat_entry_t entry)
{
    const bool backend = setup_wechat_has_backend();
    mosaic_setup_wechat_status_t status = {0};
    if (backend) {
        SETUP_MODEL_LOCK();
        status = s_setup.wechat_status;
        SETUP_MODEL_UNLOCK();
        s_setup.wechat_configured = status.configured;
    }
    s_setup.wechat_entry = entry;
    s_setup.wechat_intent = s_setup.wechat_configured
        ? setup_wechat_success_intent()
        : SETUP_WECHAT_INTENT_BIND_ACTIVE;
    s_setup.wechat_phase = s_setup.wechat_configured
        ? SETUP_WECHAT_SUCCESS : SETUP_WECHAT_BINDING;
    setup_wechat_render();
    if (!setup_push(SETUP_PAGE_WECHAT, true, now_us)) {
        return;
    }
    /* Rebind after the page becomes current. The service model may already
     * have published WAITING_SCAN while the preceding Wi-Fi page was active;
     * the same payload must therefore be eligible for a fresh canvas push. */
    s_setup.rendered_wechat_qr[0] = '\0';
    s_setup.pending_wechat_qr[0] = '\0';
    if (backend && !s_setup.wechat_configured && !status.active) {
        (void)setup_wechat_start_service(false);
    } else if (!backend && !s_setup.wechat_configured) {
        setup_wechat_begin_simulation(now_us);
    }
    setup_wechat_apply_model(now_us);
}

static void setup_open_llm(int64_t now_us)
{
    s_setup.llm_phase = s_setup.llm_configured
        ? SETUP_LLM_SUCCESS
        : (s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING
            ? SETUP_LLM_CONFIGURING : SETUP_LLM_STATUS);
    setup_llm_render();
    if (setup_push(SETUP_PAGE_LLM, true, now_us)) {
        s_setup.rendered_llm_qr[0] = '\0';
        /* Capture the URL now, but let setup_step publish its Canvas after
         * the StackView transition has completed. */
        (void)setup_llm_refresh_model(false);
        setup_overview_render();
    }
}

static void setup_network_select(const mosaic_event_t *event)
{
    if (s_setup.network_phase != SETUP_NETWORK_SCAN) {
        return;
    }
    const uint32_t item = event->data.call.list != ESP_GSP_LIST_NONE
        ? event->data.call.item : event->data.call.arg;
    bool secured;
    SETUP_MODEL_LOCK();
    if (item >= s_setup.network_count) {
        SETUP_MODEL_UNLOCK();
        return;
    }
    strlcpy(s_setup.selected_ssid, s_setup.networks[item].ssid,
            sizeof(s_setup.selected_ssid));
    secured = s_setup.networks[item].secured;
    SETUP_MODEL_UNLOCK();
    (void)gsp_setup_center_setup_selected_ssid_set_text(
        s_setup.ui, s_setup.selected_ssid);
    if (!secured && mosaic_settings_wifi_backend_available()) {
        s_setup.network_connect_pending = true;
        if (mosaic_settings_connect_wifi(s_setup.selected_ssid, "") == ESP_OK) {
            /* A Join of the already-connected SSID may not publish a new
             * manager revision. Re-evaluate the current snapshot as part of
             * this newly armed transaction. */
            s_setup.rendered_network_revision = UINT32_MAX;
            s_setup.network_phase = SETUP_NETWORK_JOINING;
            setup_network_render();
        } else {
            s_setup.network_connect_pending = false;
        }
        return;
    }
    (void)esp_gsp_set_text(s_setup.ui, GSP_BIND_SETUP_PASSWORD_VALUE, "");
    (void)esp_gsp_set_text(s_setup.ui, GSP_BIND_SETUP_PASSWORD_DISPLAY,
                           "Password");
    s_setup.rendered_password_length = UINT8_MAX;
    s_setup.network_phase = SETUP_NETWORK_PASSWORD;
    setup_network_render();
    mosaic_app_shell_set_bottom_enabled(s_setup.ui, false);
    (void)esp_gsp_set_cursor(s_setup.ui, GSP_BIND_SETUP_PASSWORD_VALUE);
    (void)esp_gsp_keyboard_attach(
        s_setup.ui, GSP_ACT_ID_SETUP_PASSWORD_KEYBOARD_KEY,
        GSP_BIND_SETUP_PASSWORD_VALUE);
}

static bool setup_network_join(int64_t now_us)
{
    if (s_setup.network_phase != SETUP_NETWORK_PASSWORD) {
        return false;
    }
    char password[65] = {0};
    if (esp_gsp_keyboard_text(
            s_setup.ui, password, sizeof(password)) != ESP_GSP_OK) {
        return false;
    }
    const size_t length = strlen(password);
    if (length < SETUP_PASSWORD_MIN) {
        (void)mosaic_top_notice_show(
            s_setup.ui, &s_notice, "Password too short",
            "Enter at least 8 characters", 2400);
        return false;
    }
    const bool backend = mosaic_settings_wifi_backend_available();
    s_setup.network_newly_connected = false;
    if (backend) {
        /* Arm before calling the backend. The device backend applies the new
         * config synchronously enough to publish CONNECTING from inside this
         * call; arming afterwards loses that first state transition. */
        s_setup.network_connect_pending = true;
        const esp_err_t err = mosaic_settings_connect_wifi(
            s_setup.selected_ssid, password);
        if (err != ESP_OK) {
            s_setup.network_connect_pending = false;
            (void)mosaic_top_notice_show(
                s_setup.ui, &s_notice, "Unable to join network",
                esp_err_to_name(err), 2600);
            return false;
        }
        /* Also handles an idempotent Join when this SSID is already active
         * and the manager therefore has no reason to emit another event. */
        s_setup.rendered_network_revision = UINT32_MAX;
    } else {
        s_setup.network_join_success = strcmp(password, "qqqqqqqq") == 0;
    }
    setup_password_detach();
    s_setup.network_phase = SETUP_NETWORK_JOINING;
    s_setup.operation_started_us = now_us;
#if defined(ESP_PLATFORM)
    s_setup.operation_deadline_us = backend
        ? 0 : now_us + SETUP_NETWORK_JOIN_US;
#else
    /* Keep Joining visible long enough to evaluate the transition in SDL.
     * The host provider may publish its terminal result immediately, but the
     * device remains exclusively driven by real Wi-Fi events. */
    s_setup.operation_deadline_us = now_us + SETUP_NETWORK_JOIN_US;
#endif
    s_setup.rendered_progress = UINT8_MAX;
    setup_network_render();
    setup_progress_render(s_network_progress, 0);
    return true;
}

static void setup_network_phone_open(void)
{
    if (s_setup.network_phase != SETUP_NETWORK_PASSWORD) {
        return;
    }
    setup_password_detach();
    s_setup.network_phase = SETUP_NETWORK_PHONE;
    s_setup.rendered_network_phone_qr[0] = '\0';
    setup_network_render();
    const esp_err_t err = setup_network_phone_refresh();
    if (err != ESP_OK) {
        (void)mosaic_top_notice_show(
            s_setup.ui, &s_notice, "Provisioning unavailable",
            "The device AP is not ready", 2600);
    }
}

static void setup_network_phone_cancel(void)
{
    if (s_setup.network_phase != SETUP_NETWORK_PHONE) {
        return;
    }
    s_setup.network_phase = SETUP_NETWORK_PASSWORD;
    setup_network_render();
    mosaic_app_shell_set_bottom_enabled(s_setup.ui, false);
    (void)esp_gsp_set_cursor(s_setup.ui, GSP_BIND_SETUP_PASSWORD_VALUE);
    (void)esp_gsp_keyboard_attach(
        s_setup.ui, GSP_ACT_ID_SETUP_PASSWORD_KEYBOARD_KEY,
        GSP_BIND_SETUP_PASSWORD_VALUE);
}

static void setup_network_phone_submitted(void)
{
    if (s_setup.network_phase != SETUP_NETWORK_PHONE) {
        return;
    }
    mosaic_settings_network_t live = {0};
    const bool backend = mosaic_settings_wifi_backend_available();
    if (backend &&
            (mosaic_settings_get_wifi(&live) != ESP_OK || !live.connected)) {
        (void)mosaic_top_notice_show(
            s_setup.ui, &s_notice, "Waiting for phone",
            "Submit Wi-Fi settings in the portal", 2800);
        return;
    }
    if (backend) {
        strlcpy(s_setup.connected_ssid, live.ssid,
                sizeof(s_setup.connected_ssid));
    } else {
        strlcpy(s_setup.connected_ssid, "Demo Wi-Fi",
                sizeof(s_setup.connected_ssid));
    }
    s_setup.network_configured = true;
    s_setup.network_newly_connected = true;
    s_setup.network_connect_pending = false;
    s_setup.network_phase = SETUP_NETWORK_CONNECTED;
    setup_network_render();
    setup_overview_render();
}

static void setup_network_cancel_password(void)
{
    if (s_setup.network_phase != SETUP_NETWORK_PASSWORD) {
        return;
    }
    setup_password_detach();
    s_setup.selected_ssid[0] = '\0';
    s_setup.network_phase = SETUP_NETWORK_SCAN;
    setup_network_render();
}

static void setup_network_apply_model(int64_t now_us)
{
    if (!mosaic_settings_wifi_backend_available() || s_setup.ui == NULL) {
        return;
    }
    /* Polling also advances the SDL provider; ESP normally arrives here via
     * subscriber events. Both paths publish the same immutable snapshot. */
    mosaic_settings_network_t live = {0};
    if (mosaic_settings_get_wifi(&live) == ESP_OK) {
        setup_network_event(&live, NULL);
    }

    mosaic_settings_network_t network;
    uint32_t revision;
    SETUP_MODEL_LOCK();
    revision = s_setup.network_revision;
    network = s_setup.network_status;
    SETUP_MODEL_UNLOCK();
#if !defined(ESP_PLATFORM)
    const bool terminal = network.state == MOSAIC_SETTINGS_WIFI_CONNECTED ||
        network.state == MOSAIC_SETTINGS_WIFI_AUTH_FAILED ||
        network.state == MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND ||
        network.state == MOSAIC_SETTINGS_WIFI_FAILED;
    if (s_setup.network_connect_pending && terminal &&
            now_us < s_setup.operation_deadline_us) {
        /* Do not consume the revision: the same terminal snapshot is applied
         * when the three-second Joining presentation interval expires. */
        return;
    }
#endif
    if (revision == s_setup.rendered_network_revision) {
        return;
    }
    s_setup.rendered_network_revision = revision;

    if (!s_setup.network_connect_pending &&
            s_setup.network_phase == SETUP_NETWORK_PHONE &&
            network.connected) {
        s_setup.network_configured = true;
        s_setup.network_skipped = false;
        s_setup.network_newly_connected = true;
        strlcpy(s_setup.connected_ssid, network.ssid,
                sizeof(s_setup.connected_ssid));
        s_setup.selected_ssid[0] = '\0';
        s_setup.network_phase = SETUP_NETWORK_CONNECTED;
        setup_network_render();
        setup_overview_render();
        return;
    }

    if (!s_setup.network_connect_pending &&
            s_setup.network_phase == SETUP_NETWORK_PHONE) {
        const char *title = NULL;
        const char *message = NULL;
        switch (network.state) {
        case MOSAIC_SETTINGS_WIFI_AUTH_FAILED:
            title = "Incorrect password";
            message = "Update the password on your phone";
            break;
        case MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND:
            title = "Network unavailable";
            message = "Choose another WLAN on your phone";
            break;
        case MOSAIC_SETTINGS_WIFI_FAILED:
            title = "Connection failed";
            message = "Check the settings and try again";
            break;
        default:
            break;
        }
        if (title != NULL) {
            (void)mosaic_top_notice_show(
                s_setup.ui, &s_notice, title, message, 2800);
            return;
        }
    }

    if (!s_setup.network_connect_pending) {
        if (network.connected && network.ssid[0] != '\0') {
            s_setup.network_configured = true;
            strlcpy(s_setup.connected_ssid, network.ssid,
                    sizeof(s_setup.connected_ssid));
        }
        setup_overview_render();
        return;
    }

    const char *title = NULL;
    const char *message = NULL;
    switch (network.state) {
    case MOSAIC_SETTINGS_WIFI_CONNECTED:
        if (s_setup.selected_ssid[0] != '\0' &&
                strcmp(network.ssid, s_setup.selected_ssid) != 0) {
            /* The old association can still be visible while replacement
             * credentials are being handed to the Wi-Fi worker. It is not
             * completion of the current Join transaction. */
            break;
        }
        s_setup.network_connect_pending = false;
        s_setup.network_configured = true;
        s_setup.network_skipped = false;
        s_setup.network_newly_connected =
            s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING;
        strlcpy(s_setup.connected_ssid,
                network.ssid[0] ? network.ssid : s_setup.selected_ssid,
                sizeof(s_setup.connected_ssid));
        s_setup.network_phase = SETUP_NETWORK_CONNECTED;
        break;
    case MOSAIC_SETTINGS_WIFI_AUTH_FAILED:
        title = "Incorrect password";
        message = "Check the password and try again";
        break;
    case MOSAIC_SETTINGS_WIFI_AP_NOT_FOUND:
        title = "Network unavailable";
        message = "The selected WLAN was not found";
        break;
    case MOSAIC_SETTINGS_WIFI_FAILED:
        title = "Connection failed";
        message = "Check the network and try again";
        break;
    case MOSAIC_SETTINGS_WIFI_CONNECTING:
    case MOSAIC_SETTINGS_WIFI_RETRY_WAIT:
    case MOSAIC_SETTINGS_WIFI_IDLE:
    case MOSAIC_SETTINGS_WIFI_SCANNING:
    default:
        break;
    }
    if (title != NULL) {
        s_setup.network_connect_pending = false;
        s_setup.network_configured = false;
        s_setup.network_newly_connected = false;
        s_setup.connected_ssid[0] = '\0';
        s_setup.selected_ssid[0] = '\0';
        (void)mosaic_settings_forget_wifi();
        s_setup.network_phase = SETUP_NETWORK_SCAN;
        setup_network_request_scan(now_us);
        (void)mosaic_top_notice_show(
            s_setup.ui, &s_notice, title, message, 2600);
    }
    setup_network_render();
    setup_overview_render();
}

static void setup_advance_after_network(int64_t now_us)
{
    if (s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING) {
        setup_open_wechat(now_us, SETUP_WECHAT_ENTRY_ONBOARDING);
    } else {
        setup_overview_render();
        (void)setup_pop(s_setup.overview_page, now_us);
    }
}

static void setup_finish_onboarding(int64_t now_us)
{
    s_setup.onboarding_complete = true;
    setup_overview_render();
    (void)setup_push(SETUP_PAGE_DONE, true, now_us);
}

static bool setup_return_to_requester(void)
{
    if (s_setup.mode != MOSAIC_SETUP_MODE_MANAGE ||
            s_return_app[0] == '\0' || s_app_request == NULL) {
        return false;
    }
    char return_app[sizeof(s_return_app)];
    SETUP_MODEL_LOCK();
    strlcpy(return_app, s_return_app, sizeof(return_app));
    s_return_app[0] = '\0';
    SETUP_MODEL_UNLOCK();
    return s_app_request(s_app_request_ctx, return_app) == ESP_OK;
}

static bool setup_center_back(
    esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)ui;
    (void)timestamp_us;
    return setup_return_to_requester();
}

static void setup_handle_call(const mosaic_event_t *event)
{
    const uint16_t action = event->data.call.action_id;
    const int64_t now_us = event->timestamp_us;
    switch (action) {
    case GSP_ACT_ID_SETUP_CENTER_CLOSE:
        if (s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING &&
                s_setup.page == SETUP_PAGE_WECHAT) {
            s_setup.onboarding_complete = true;
            s_setup.wechat_skipped = !s_setup.wechat_configured;
        }
        (void)setup_return_to_requester();
        break;
    case GSP_ACT_ID_SETUP_OVERVIEW_NETWORK:
        if (s_setup.page == s_setup.overview_page) {
            setup_open_network(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_OVERVIEW_WECHAT:
        if (s_setup.page == s_setup.overview_page) {
            setup_open_wechat(now_us, SETUP_WECHAT_ENTRY_APP);
        }
        break;
    case GSP_ACT_ID_SETUP_OVERVIEW_LLM:
        if (s_setup.page == s_setup.overview_page) {
            setup_open_llm(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_NETWORK_SELECT:
        setup_network_select(event);
        break;
    case GSP_ACT_ID_SETUP_NETWORK_REFRESH:
        if (s_setup.network_phase == SETUP_NETWORK_SCAN &&
                s_setup.network_scan_ready) {
            setup_network_request_scan(now_us);
            setup_network_render();
        }
        break;
    case GSP_ACT_ID_SETUP_NETWORK_CANCEL:
        setup_network_cancel_password();
        break;
    case GSP_ACT_ID_SETUP_NETWORK_JOIN:
        (void)setup_network_join(now_us);
        break;
    case GSP_ACT_ID_SETUP_NETWORK_PHONE_OPEN:
        setup_network_phone_open();
        break;
    case GSP_ACT_ID_SETUP_NETWORK_PHONE_CANCEL:
        setup_network_phone_cancel();
        break;
    case GSP_ACT_ID_SETUP_NETWORK_PHONE_SUBMITTED:
        setup_network_phone_submitted();
        break;
    case GSP_ACT_ID_SETUP_PASSWORD_KEYBOARD_KEY:
        if (event->data.call.arg == 13U) {
            (void)setup_network_join(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_NETWORK_CHANGE:
        if (s_setup.network_phase == SETUP_NETWORK_CONNECTED) {
            s_setup.network_newly_connected = false;
            s_setup.network_phase = SETUP_NETWORK_SCAN;
            if (mosaic_settings_wifi_backend_available()) {
                setup_network_request_scan(now_us);
            } else {
                s_setup.network_scan_ready = false;
                s_setup.network_scan_deadline_us =
                    now_us + SETUP_NETWORK_SCAN_US;
            }
            setup_network_render();
        }
        break;
    case GSP_ACT_ID_SETUP_NETWORK_CONTINUE:
        if (s_setup.network_phase == SETUP_NETWORK_CONNECTED) {
            setup_advance_after_network(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_NETWORK_SKIP:
        if (s_setup.network_phase == SETUP_NETWORK_SCAN) {
            s_setup.network_skipped = !s_setup.network_configured;
            setup_overview_render();
            setup_advance_after_network(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_NETWORK_BACK:
        if (s_setup.network_phase == SETUP_NETWORK_PASSWORD) {
            setup_network_cancel_password();
        } else if (s_setup.network_phase != SETUP_NETWORK_JOINING) {
            setup_overview_render();
            (void)setup_pop(s_setup.overview_page, now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_WECHAT_REBIND:
        s_setup.wechat_intent = s_setup.wechat_configured
            ? SETUP_WECHAT_INTENT_REBIND_ACTIVE
            : SETUP_WECHAT_INTENT_BIND_ACTIVE;
        s_setup.wechat_phase = SETUP_WECHAT_BINDING;
        setup_wechat_render();
        if (setup_wechat_has_backend()) {
            const esp_err_t err = setup_wechat_start_service(
                s_setup.wechat_configured);
            if (err != ESP_OK && s_setup.wechat_configured) {
                s_setup.wechat_intent = setup_wechat_success_intent();
                s_setup.wechat_phase = SETUP_WECHAT_SUCCESS;
                setup_wechat_render();
            }
        } else {
            setup_wechat_begin_simulation(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_WECHAT_CANCEL:
        if (setup_return_to_requester()) {
            setup_wechat_disarm_operation();
            if (setup_wechat_has_backend()) {
                setup_wechat_schedule_cancel();
            }
            break;
        }
        if (s_setup.wechat_phase == SETUP_WECHAT_BINDING ||
                s_setup.wechat_phase == SETUP_WECHAT_PROGRESS) {
            const bool backend = setup_wechat_has_backend();
            setup_overview_render();
            if (setup_pop(s_setup.overview_page, now_us)) {
                /* Back leaves the WeChat route in one operation.  Disarm the
                 * UI model before the asynchronous service cancellation can
                 * publish CANCELLED (or one last WAITING_SCAN), otherwise the
                 * old operation pulls the hidden page back into Binding. */
                setup_wechat_disarm_operation();
                if (backend) {
                    /* Do not hold GSP's runtime lock while the service waits
                     * on its worker/gateway cancellation path. */
                    setup_wechat_schedule_cancel();
                }
            }
        }
        break;
    case GSP_ACT_ID_SETUP_WECHAT_SUCCESS_BACK:
        if (setup_return_to_requester()) {
            break;
        }
        if (s_setup.wechat_phase == SETUP_WECHAT_SUCCESS &&
                s_setup.wechat_entry == SETUP_WECHAT_ENTRY_APP) {
            setup_overview_render();
            (void)setup_pop(s_setup.overview_page, now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_LLM_RECONFIGURE:
        s_setup.llm_phase = SETUP_LLM_CONFIGURING;
        setup_llm_render();
        break;
    case GSP_ACT_ID_SETUP_LLM_SUBMITTED:
        if (s_setup.llm_phase == SETUP_LLM_CONFIGURING) {
            s_setup.llm_phase = SETUP_LLM_PROGRESS;
            s_setup.operation_started_us = now_us;
            s_setup.operation_deadline_us = now_us + SETUP_LLM_CONFIG_US;
            s_setup.rendered_progress = UINT8_MAX;
            setup_llm_render();
            setup_progress_render(s_llm_progress, 0);
        }
        break;
    case GSP_ACT_ID_SETUP_LLM_CANCEL:
        if (setup_return_to_requester()) {
            break;
        }
        if (s_setup.llm_phase == SETUP_LLM_CONFIGURING ||
                s_setup.llm_phase == SETUP_LLM_PROGRESS) {
            s_setup.llm_phase = s_setup.llm_configured
                ? SETUP_LLM_SUCCESS : SETUP_LLM_STATUS;
            setup_llm_render();
        }
        break;
    case GSP_ACT_ID_SETUP_LLM_CONTINUE:
        if (s_setup.llm_phase == SETUP_LLM_STATUS ||
                s_setup.llm_phase == SETUP_LLM_SUCCESS) {
            if (s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING) {
                setup_finish_onboarding(now_us);
            } else if (setup_return_to_requester()) {
                break;
            } else {
                setup_overview_render();
                (void)setup_pop(s_setup.overview_page, now_us);
            }
        }
        break;
    case GSP_ACT_ID_SETUP_LLM_SKIP:
        if (s_setup.llm_phase == SETUP_LLM_STATUS ||
                s_setup.llm_phase == SETUP_LLM_CONFIGURING) {
            s_setup.llm_skipped = !s_setup.llm_configured;
            setup_finish_onboarding(now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_LLM_BACK:
        if (setup_return_to_requester()) {
            break;
        }
        if (s_setup.llm_phase == SETUP_LLM_CONFIGURING ||
                s_setup.llm_phase == SETUP_LLM_PROGRESS) {
            s_setup.llm_phase = s_setup.llm_configured
                ? SETUP_LLM_SUCCESS : SETUP_LLM_STATUS;
            setup_llm_render();
        } else {
            setup_overview_render();
            (void)setup_pop(s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING
                ? SETUP_PAGE_WECHAT : s_setup.overview_page, now_us);
        }
        break;
    case GSP_ACT_ID_SETUP_VIEW_STATUS:
        s_setup.mode = MOSAIC_SETUP_MODE_MANAGE;
        setup_navigation_render();
        setup_overview_render();
        (void)setup_push(s_setup.overview_page, true, now_us);
        break;
    default:
        break;
    }
}

static uint8_t setup_operation_progress(int64_t now_us)
{
    const int64_t duration =
        s_setup.operation_deadline_us - s_setup.operation_started_us;
    if (duration <= 0 || now_us >= s_setup.operation_deadline_us) {
        return SETUP_PROGRESS_COUNT;
    }
    if (now_us <= s_setup.operation_started_us) {
        return 0;
    }
    return (uint8_t)(((now_us - s_setup.operation_started_us) *
        SETUP_PROGRESS_COUNT) / duration);
}

static void setup_step(int64_t now_us)
{
    setup_wechat_apply_model(now_us);
    setup_network_apply_model(now_us);
    if (s_setup.page == SETUP_PAGE_WECHAT &&
            s_setup.wechat_phase == SETUP_WECHAT_BINDING &&
            now_us >= s_setup.navigation_guard_until_us &&
            s_setup.pending_wechat_qr[0] != '\0') {
        setup_wechat_push_qr(s_setup.pending_wechat_qr, now_us);
    }
    if (s_setup.page == SETUP_PAGE_LLM &&
            (s_setup.llm_phase == SETUP_LLM_STATUS ||
             s_setup.llm_phase == SETUP_LLM_CONFIGURING) &&
            now_us >= s_setup.navigation_guard_until_us &&
            s_setup.llm_config_url[0] != '\0' &&
            strcmp(s_setup.llm_config_url,
                   s_setup.rendered_llm_qr) != 0) {
        /* StackView activation can briefly reject the first canvas update;
         * retry the already captured URL without recollecting Settings. */
        setup_llm_push_qr(s_setup.llm_config_url);
    }
    if (s_setup.network_phase == SETUP_NETWORK_PASSWORD) {
        char password[65] = {0};
        if (esp_gsp_keyboard_text(s_setup.ui, password,
                sizeof(password)) == ESP_GSP_OK) {
            const size_t length = strlen(password);
            if (length != s_setup.rendered_password_length) {
                (void)esp_gsp_set_text(
                    s_setup.ui, GSP_BIND_SETUP_PASSWORD_DISPLAY,
                    length > 0U ? password : "Password");
                s_setup.rendered_password_length = (uint8_t)length;
            }
        }
    }
    if (s_setup.network_phase == SETUP_NETWORK_SCAN &&
            !s_setup.network_scan_ready) {
        if (mosaic_settings_wifi_backend_available()) {
            if (setup_network_refresh_scan()) {
                s_setup.network_scan_ready = true;
                setup_network_render();
            } else if (now_us >= s_setup.network_scan_deadline_us) {
                /* Wi-Fi starts after Mosaic during boot.  Retry until the
                 * radio is ready instead of presenting a pending scan as an
                 * empty network list. */
                setup_network_request_scan(now_us);
            }
        } else if (now_us >= s_setup.network_scan_deadline_us) {
            setup_network_seed_mock();
            s_setup.network_scan_ready = true;
            setup_network_render();
        }
    } else if (s_setup.network_phase == SETUP_NETWORK_JOINING &&
            !mosaic_settings_wifi_backend_available()) {
        const uint8_t progress = setup_operation_progress(now_us);
        if (progress != s_setup.rendered_progress) {
            setup_progress_render(s_network_progress, progress);
        }
        if (now_us >= s_setup.operation_deadline_us) {
            if (s_setup.network_join_success) {
                s_setup.network_configured = true;
                s_setup.network_skipped = false;
                s_setup.network_newly_connected =
                    s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING;
                snprintf(s_setup.connected_ssid,
                         sizeof(s_setup.connected_ssid), "%s",
                         s_setup.selected_ssid);
                s_setup.network_phase = SETUP_NETWORK_CONNECTED;
            } else {
                s_setup.network_configured = false;
                s_setup.network_newly_connected = false;
                s_setup.connected_ssid[0] = '\0';
                s_setup.selected_ssid[0] = '\0';
                s_setup.network_phase = SETUP_NETWORK_SCAN;
                (void)mosaic_top_notice_show(
                    s_setup.ui, &s_notice, "Connection failed",
                    "Incorrect password", 2600);
            }
            setup_network_render();
            setup_overview_render();
        }
    } else if (s_setup.network_phase == SETUP_NETWORK_JOINING &&
            s_setup.network_connect_pending) {
        /* Real service states own completion. Keep the progress mosaic alive
         * without inventing a percentage while CONNECTING/RETRY_WAIT lasts. */
        const uint8_t progress = (uint8_t)(
            ((now_us - s_setup.operation_started_us) / 150000LL) %
            SETUP_PROGRESS_COUNT) + 1U;
        if (progress != s_setup.rendered_progress) {
            setup_progress_render(s_network_progress, progress);
        }
    } else if (!setup_wechat_has_backend() &&
            s_setup.wechat_flow.simulation &&
            wechat_binding_flow_step(&s_setup.wechat_flow, now_us)) {
        s_setup.wechat_phase = s_setup.wechat_flow.phase ==
                WECHAT_BINDING_FLOW_SUCCESS
            ? SETUP_WECHAT_SUCCESS
            : (s_setup.wechat_flow.phase == WECHAT_BINDING_FLOW_PROGRESS
                ? SETUP_WECHAT_PROGRESS : SETUP_WECHAT_BINDING);
        setup_progress_render(
            s_wechat_progress, s_setup.wechat_flow.progress);
        (void)gsp_setup_center_setup_wechat_stage_text_set_text(
            s_setup.ui, s_setup.wechat_flow.stage);
        if (s_setup.wechat_flow.phase == WECHAT_BINDING_FLOW_SUCCESS) {
            s_setup.wechat_intent = setup_wechat_success_intent();
            s_setup.wechat_configured = true;
            s_setup.wechat_skipped = false;
            s_setup.onboarding_complete = true;
        }
        setup_wechat_render();
        setup_overview_render();
    } else if (s_setup.llm_phase == SETUP_LLM_PROGRESS) {
        const uint8_t progress = setup_operation_progress(now_us);
        if (progress != s_setup.rendered_progress) {
            setup_progress_render(s_llm_progress, progress);
        }
        if (now_us >= s_setup.operation_deadline_us) {
            const esp_err_t refresh_err =
                setup_llm_refresh_model(false);
            if (refresh_err == ESP_OK && s_setup.llm_configured) {
                s_setup.llm_skipped = false;
                s_setup.llm_phase = SETUP_LLM_SUCCESS;
            } else {
                s_setup.llm_phase = SETUP_LLM_CONFIGURING;
                (void)mosaic_top_notice_show(
                    s_setup.ui, &s_notice, "Configuration not found",
                    "Save the LLM settings and try again", 3000);
            }
            setup_llm_render();
            setup_overview_render();
        }
    }
}

static void setup_start(esp_gsp_handle_t ui, int64_t now_us)
{
    bool pending_entry;
    mosaic_setup_mode_t pending_mode = MOSAIC_SETUP_MODE_ONBOARDING;
    mosaic_setup_route_t pending_route = MOSAIC_SETUP_ROUTE_NETWORK;
    SETUP_MODEL_LOCK();
    setup_wechat_event_queue_reset(&s_setup.wechat_events);
    s_setup.wechat_stage_deadline_us = 0;
    s_setup.page = SETUP_PAGE_OVERVIEW;
    s_setup.overview_page = SETUP_PAGE_OVERVIEW;
    s_setup.ui = ui;
    pending_entry = s_pending_entry;
    if (pending_entry) {
        pending_mode = s_pending_mode;
        pending_route = s_pending_route;
        s_pending_entry = false;
    }
    SETUP_MODEL_UNLOCK();
    s_setup.network_list = ESP_GSP_LIST_NONE;
    s_setup.navigation_guard_until_us = 0;
    s_setup.operation_deadline_us = 0;
    s_setup.rendered_network_revision = UINT32_MAX;
    s_setup.network_connect_pending = false;
    s_setup.rendered_wechat_revision = UINT32_MAX;
    s_setup.rendered_wechat_qr[0] = '\0';
    s_setup.pending_wechat_qr[0] = '\0';
    s_setup.rendered_llm_qr[0] = '\0';
    s_setup.rendered_network_phone_qr[0] = '\0';
    s_setup.llm_config_url[0] = '\0';
    s_setup.wechat_intent = SETUP_WECHAT_INTENT_VIEW_STATUS;
    s_setup.wechat_entry = SETUP_WECHAT_ENTRY_APP;
    s_setup.wechat_operation_revision = s_setup.wechat_revision;
    if (setup_wechat_has_backend() && setup_qr_ensure_buffers() != ESP_OK) {
        ESP_LOGE(TAG, "allocate WeChat QR buffers failed");
    }
    if (mosaic_settings_wifi_backend_available()) {
        const esp_err_t err = mosaic_settings_subscribe_wifi(
            setup_network_event, NULL);
        s_setup.wifi_subscribed = err == ESP_OK;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "subscribe Wi-Fi state failed: %s",
                     esp_err_to_name(err));
        }
    }
    if (pending_entry) {
        s_setup.mode = pending_mode;
        s_setup.route = pending_route;
    } else if (s_setup.onboarding_complete) {
        s_setup.mode = MOSAIC_SETUP_MODE_MANAGE;
        s_setup.route = MOSAIC_SETUP_ROUTE_OVERVIEW;
    } else {
        s_setup.mode = MOSAIC_SETUP_MODE_ONBOARDING;
        s_setup.route = MOSAIC_SETUP_ROUTE_NETWORK;
    }
    if (s_setup.route == MOSAIC_SETUP_ROUTE_INTEGRATIONS) {
        s_setup.overview_page = SETUP_PAGE_INTEGRATIONS;
        (void)setup_push(SETUP_PAGE_INTEGRATIONS, false, now_us);
    }
    setup_navigation_render();
    (void)setup_llm_refresh_model(false);
    setup_overview_render();
    setup_network_render();
    setup_wechat_render();
    setup_llm_render();
    setup_wechat_apply_model(now_us);
    switch (s_setup.route) {
    case MOSAIC_SETUP_ROUTE_NETWORK:
        setup_open_network(now_us);
        break;
    case MOSAIC_SETUP_ROUTE_WECHAT:
        setup_open_wechat(now_us,
            s_setup.mode == MOSAIC_SETUP_MODE_ONBOARDING
                ? SETUP_WECHAT_ENTRY_ONBOARDING
                : SETUP_WECHAT_ENTRY_APP);
        break;
    case MOSAIC_SETUP_ROUTE_LLM:
        setup_open_llm(now_us);
        break;
    case MOSAIC_SETUP_ROUTE_INTEGRATIONS:
        break;
    case MOSAIC_SETUP_ROUTE_OVERVIEW:
    default:
        break;
    }
}

static void setup_center_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        setup_start(ui, event->timestamp_us);
        break;
    case MOSAIC_EVENT_STOP:
        setup_password_detach();
        setup_network_list_park();
        (void)esp_gsp_canvas_stop(ui, GSP_BIND_SETUP_WECHAT_QR_CANVAS);
        (void)esp_gsp_canvas_stop(ui, GSP_BIND_SETUP_LLM_CONFIG_QR_CANVAS);
        if (setup_wechat_has_backend() &&
                (s_setup.wechat_phase == SETUP_WECHAT_BINDING ||
                 s_setup.wechat_phase == SETUP_WECHAT_PROGRESS)) {
            setup_wechat_disarm_operation();
            setup_wechat_schedule_cancel();
        }
        mosaic_top_notice_detach(ui);
        if (s_setup.wifi_subscribed) {
            (void)mosaic_settings_unsubscribe_wifi(
                setup_network_event, NULL);
            s_setup.wifi_subscribed = false;
        }
        s_setup.network_list = ESP_GSP_LIST_NONE;
        SETUP_MODEL_LOCK();
        setup_wechat_event_queue_reset(&s_setup.wechat_events);
        s_setup.wechat_stage_deadline_us = 0;
        s_setup.ui = NULL;
        SETUP_MODEL_UNLOCK();
        break;
    case MOSAIC_EVENT_UI_CALL:
        setup_handle_call(event);
        break;
    case MOSAIC_EVENT_TIMER:
        setup_step(event->timestamp_us);
        break;
    case MOSAIC_EVENT_MODEL_CHANGED:
        setup_step(event->timestamp_us);
        break;
    case MOSAIC_EVENT_SCENE_CHANGED:
    case MOSAIC_EVENT_POINTER:
    default:
        break;
    }
}

esp_err_t mosaic_setup_open(
    mosaic_setup_mode_t mode, mosaic_setup_route_t route)
{
    if (mode > MOSAIC_SETUP_MODE_MANAGE ||
            route > MOSAIC_SETUP_ROUTE_INTEGRATIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_app_descriptor_t *app =
        mosaic_app_descriptor_for_name("setup_center");
    if (app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    SETUP_MODEL_LOCK();
    const uint32_t revision = ++s_pending_revision;
    s_pending_mode = mode;
    s_pending_route = route;
    s_pending_entry = true;
    SETUP_MODEL_UNLOCK();
    esp_err_t err;
    if (s_app_request != NULL) {
        err = s_app_request(s_app_request_ctx, app->name);
    } else {
#if defined(ESP_PLATFORM)
        err = mosaic_loader_request(app);
#else
        err = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    if (err != ESP_OK) {
        SETUP_MODEL_LOCK();
        if (s_pending_entry && s_pending_revision == revision) {
            s_pending_entry = false;
        }
        SETUP_MODEL_UNLOCK();
    }
    return err;
}

esp_err_t mosaic_setup_set_return_app(const char *app_name)
{
    if (app_name != NULL && strlen(app_name) >= sizeof(s_return_app)) {
        return ESP_ERR_INVALID_ARG;
    }
    SETUP_MODEL_LOCK();
    strlcpy(s_return_app, app_name != NULL ? app_name : "",
            sizeof(s_return_app));
    SETUP_MODEL_UNLOCK();
    return ESP_OK;
}

esp_err_t mosaic_setup_configure_app_request(
    mosaic_setup_app_request_cb_t callback, void *user_ctx)
{
    if (callback == NULL && user_ctx != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    SETUP_MODEL_LOCK();
    s_app_request = callback;
    s_app_request_ctx = user_ctx;
    SETUP_MODEL_UNLOCK();
    return ESP_OK;
}

const mosaic_app_descriptor_t mosaic_setup_center_app = {
    .id = SETUP_CENTER_APP_ID,
    .launch_action = MOSAIC_APP_NO_LAUNCH_ACTION,
    .back_action = GSP_ACT_ID_SETUP_CENTER_CLOSE,
    .name = "setup_center",
    .title = "AI Setup",
    .directory = &gsp_obj_directory_setup_center,
    .root_stack_key = GSP_OBJ_KEY_SETUP_STACK,
    .disable_swipe = true,
    .root_header_in_stack = true,
    .instance_slots = 16,
    .on_back = setup_center_back,
    .on_event = setup_center_event,
};
