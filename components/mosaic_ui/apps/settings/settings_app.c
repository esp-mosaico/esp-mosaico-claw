/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"
#include "qrcodegen.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_gsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mosaic_app_catalog.h"
#include "mosaic_app_shell.h"
#include "mosaic_top_notice.h"
#include "mosaic_hub_actions.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "mosaic_loader.h"
#endif
#include "mosaic_runtime.h"
#include "settings_actions.h"
#include "settings_binds.h"
#include "settings_objects.h"
#include "settings_root_icon_data.h"
#include "settings_templates.h"

/* Settings is the one App allowed to write every device domain, so it holds
 * the full device grant. It still holds no media, storage, or AI capability. */
#define SETTINGS_CAPABILITIES ( \
    MOSAIC_CAP_SYSTEM_DISPLAY_READ | MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL | \
    MOSAIC_CAP_SYSTEM_AUDIO_READ | MOSAIC_CAP_SYSTEM_AUDIO_CONTROL | \
    MOSAIC_CAP_SYSTEM_HAPTIC_READ | MOSAIC_CAP_SYSTEM_HAPTIC_CONTROL | \
    MOSAIC_CAP_SYSTEM_POWER_READ | \
    MOSAIC_CAP_SYSTEM_UPDATE_READ | MOSAIC_CAP_SYSTEM_UPDATE_CONTROL | \
    MOSAIC_CAP_SYSTEM_LIFECYCLE_READ | MOSAIC_CAP_SYSTEM_LIFECYCLE_CONTROL | \
    MOSAIC_CAP_NET_WIFI_READ | MOSAIC_CAP_NET_WIFI_CONTROL | \
    MOSAIC_CAP_NET_PROVISIONING_READ | \
    MOSAIC_CAP_CONFIG_AGENT_READ)

#define SETTINGS_TOGGLE_OFF_COLOR UINT32_C(0x39E7)
#define SETTINGS_TOGGLE_ON_COLOR  UINT32_C(0xFA60)
#define SETTINGS_TEXT_COLOR       UINT32_C(0xFFFF)
#define SETTINGS_MUTED_COLOR      UINT32_C(0x9493)
#define SETTINGS_LEVEL_OFF_COLOR  UINT32_C(0x18C3)
#define SETTINGS_LEVEL_ON_COLOR   UINT32_C(0xFA60)
#define SETTINGS_DEFAULT_VOLUME 80
#define SETTINGS_DEFAULT_BRIGHTNESS 100
#define SETTINGS_INTEGRATIONS_MODEL_PAGE 9
#define SETTINGS_INTEGRATIONS_CHANNELS_PAGE 10
#define SETTINGS_DISPLAY_PAGE 1
#define SETTINGS_SOUND_PAGE 11
#define SETTINGS_ABOUT_PAGE 2
#define SETTINGS_SECURITY_PAGE 12
#define SETTINGS_UPDATE_PAGE 13
#define SETTINGS_DETAIL_PAGE 7
#define SETTINGS_FACTORY_PAGE 8
#define SETTINGS_FACTORY_HOLD_US INT64_C(3000000)
#define SETTINGS_FACTORY_HOLD_X_MIN 64
#define SETTINGS_FACTORY_HOLD_X_MAX 416
#define SETTINGS_FACTORY_HOLD_Y_MIN 300
#define SETTINGS_FACTORY_HOLD_Y_MAX 386
#define SETTINGS_FACTORY_HOLD_WIDTH 352
#define SETTINGS_TRACK_X_MIN 12
#define SETTINGS_TRACK_X_MAX 468
#define SETTINGS_LEVEL_COUNT 24
#define SETTINGS_DISPLAY_VIEWPORT_Y 64
#define SETTINGS_DISPLAY_VIEWPORT_H 400
#define SETTINGS_BRIGHTNESS_TRACK_Y_MIN 176
#define SETTINGS_BRIGHTNESS_TRACK_Y_MAX 238
#define SETTINGS_VOLUME_TRACK_Y_MIN 176
#define SETTINGS_VOLUME_TRACK_Y_MAX 238
#define SETTINGS_NOTIFICATION_Y_MIN 279
#define SETTINGS_NOTIFICATION_Y_MAX 327
#define SETTINGS_DISPLAY_TAP_SLOP 16
#define SETTINGS_DISPLAY_RENDER_PERIOD_MS 16
#define SETTINGS_QR_SIZE 104U
#define SETTINGS_CHANNEL_QR_SIZE 104U
#define SETTINGS_QR_STRIDE (SETTINGS_QR_SIZE * 2U)
#define SETTINGS_QR_BYTES ((size_t)SETTINGS_QR_STRIDE * SETTINGS_QR_SIZE)
#define SETTINGS_WLAN_PAGE 3
#define SETTINGS_WLAN_DETAIL_PAGE 4
#define SETTINGS_WLAN_PASSWORD_PAGE 5
#define SETTINGS_WLAN_PHONE_PAGE 6
#define SETTINGS_WLAN_PHONE_QR_SIZE 184U
#define SETTINGS_WLAN_PASSWORD_MIN 8
#define SETTINGS_NOTICE_DURATION_MS 3200U
#define SETTINGS_UPDATE_NOTICE_DURATION_MS 8000U
#define SETTINGS_UPDATE_NOTE_MAX_ROWS 12U
#define SETTINGS_UPDATE_NOTE_LINE_LEN 192U
#define SETTINGS_UPDATE_NOTE_CODEPOINTS 46U
#define SETTINGS_WLAN_LOADING_FRAME_COUNT 8U
#define SETTINGS_WLAN_LOADING_PERIOD_MS 125U
#define SETTINGS_WLAN_SCAN_REFRESH_US INT64_C(15000000)
#define SETTINGS_WLAN_SCAN_RETRY_US INT64_C(3000000)
#define SETTINGS_WLAN_SCAN_TIMEOUT_US INT64_C(12000000)
#define SETTINGS_WLAN_CONNECT_TIMEOUT_US INT64_C(35000000)
#define SETTINGS_INTEGRATION_PROGRESS_COUNT 12U
#define SETTINGS_INTEGRATION_PROGRESS_OFF UINT32_C(0x39E7)
#define SETTINGS_INTEGRATION_PROGRESS_ON UINT32_C(0xEA04)
#define SETTINGS_INTEGRATION_CONFIG_US 2200000LL
#define SETTINGS_INTEGRATION_QR_QUIET 2
#define SETTINGS_INTEGRATION_QR_FRAMES 2U
#define SETTINGS_INTEGRATION_QR_SIZE 256U
#define SETTINGS_INTEGRATION_QR_STRIDE (SETTINGS_INTEGRATION_QR_SIZE * 2U)
#define SETTINGS_INTEGRATION_QR_BYTES \
    ((size_t)SETTINGS_INTEGRATION_QR_STRIDE * SETTINGS_INTEGRATION_QR_SIZE)

typedef enum {
    SETTINGS_LLM_STATUS = 0,
    SETTINGS_LLM_CONFIGURING,
    SETTINGS_LLM_PROGRESS,
} settings_llm_phase_t;

typedef struct {
    uint8_t *pixels;
    atomic_bool busy;
} settings_qr_frame_t;

static const mosaic_top_notice_config_t s_top_notice = {
    .visible_bind = GSP_BIND_SETTINGS_TOP_NOTICE_VISIBLE,
    .title_bind = GSP_BIND_SETTINGS_TOP_NOTICE_TITLE,
    .message_bind = GSP_BIND_SETTINGS_TOP_NOTICE_MESSAGE,
};

typedef struct {
    bool enabled;
    bool connected;
    bool auto_join;
    mosaic_cap_wifi_state_t state;
    char current_ssid[33];
    char selected_ssid[33];
    char password[65];
} settings_wlan_state_t;

typedef enum {
    SETTINGS_DISPLAY_TAP_NONE = 0,
    SETTINGS_DISPLAY_TAP_NOTIFICATION,
} settings_display_tap_t;

/* Cached copy of every device capability Settings renders. Each record is
 * refreshed from its own provider, so a page only invalidates the domains it
 * actually shows. */
typedef struct {
    mosaic_cap_display_t display;
    mosaic_cap_audio_t audio;
    mosaic_cap_haptic_t haptic;
    mosaic_cap_power_t power;
    mosaic_cap_update_t update;
    mosaic_cap_lifecycle_t lifecycle;
    mosaic_cap_wifi_t network;
    mosaic_cap_agent_config_t agent;
} settings_device_t;

typedef struct {
    settings_device_t device;
    esp_gsp_handle_t ui;
    int32_t brightness;
    int32_t volume;
    bool brightness_hw_available;
    bool sound_effect_enabled;
    bool factory_hold_active;
    bool factory_hold_completed;
    int64_t factory_hold_started_us;
    bool display_page_active;
    bool display_reset_pending;
    bool display_pointer_pressed;
    bool display_pointer_scrolled;
    int32_t display_press_y;
    int32_t display_last_y;
    settings_display_tap_t display_tap;
    bool brightness_drag_active;
    bool brightness_dirty;
    bool volume_drag_active;
    bool volume_dirty;
    settings_llm_phase_t llm_phase;
    uint16_t integration_page;
    int64_t integration_started_us;
    int64_t integration_deadline_us;
    uint8_t integration_rendered_progress;
    uint8_t *qr_temp;
    uint8_t *qr_code;
    settings_qr_frame_t qr_frames[SETTINGS_INTEGRATION_QR_FRAMES];
    char rendered_channel_qr[MOSAIC_CAP_LLM_URL_LEN];
    char rendered_llm_qr[MOSAIC_CAP_LLM_URL_LEN];
    char rendered_phone_qr[MOSAIC_CAP_PHONE_QR_LEN];
    char provisioning_ap_ssid[MOSAIC_CAP_SSID_LEN];
    atomic_bool display_render_pending;
    void *display_render_timer;
    bool wlan_page_active;
    bool wlan_entry_pending;
    bool wlan_leave_pending;
    bool wlan_detail_active;
    bool wlan_detail_leave_pending;
    bool wlan_password_active;
    bool wlan_phone_active;
    bool wlan_phone_exit_pending;
    uint32_t wlan_phone_operation_id;
    bool wlan_forget_pending;
    bool wlan_connect_pending;
    uint32_t wlan_connect_operation_id;
    int64_t wlan_connect_deadline_us;
    uint32_t wlan_scan_revision;
    int64_t wlan_scan_next_us;
    bool wlan_scan_waiting;
    void *wlan_loading_timer;
    uint8_t wlan_loading_frame;
    bool wlan_loading_visible;
    bool wlan_notice_pending;
    const char *wlan_notice_title;
    const char *wlan_notice_message;
    bool wlan_scan_compact;
    bool wlan_scan_compact_valid;
    int64_t wlan_toggle_last_us;
    uint32_t update_notice_sequence;
    bool update_page_active;
    bool update_leave_pending;
    settings_wlan_state_t wlan;
} settings_ui_state_t;

static mosaic_cap_wifi_ap_t
    s_wlan_networks[MOSAIC_CAP_WIFI_SCAN_MAX];
static size_t s_wlan_network_count;

static esp_gsp_list_t s_wlan_list = ESP_GSP_LIST_NONE;
static esp_gsp_list_t s_root_list = ESP_GSP_LIST_NONE;
static esp_gsp_list_t s_detail_list = ESP_GSP_LIST_NONE;
static esp_gsp_list_t s_update_notes_list = ESP_GSP_LIST_NONE;

typedef struct {
    char text[SETTINGS_UPDATE_NOTE_LINE_LEN];
    bool title;
} settings_update_note_line_t;

static settings_update_note_line_t
    s_update_note_lines[SETTINGS_UPDATE_NOTE_MAX_ROWS];
static size_t s_update_note_count;
static char s_rendered_update_title[MOSAIC_CAP_UPDATE_TITLE_LEN];
static char s_rendered_update_summary[MOSAIC_CAP_UPDATE_SUMMARY_LEN];

typedef enum {
    SETTINGS_DETAIL_ABOUT,
    SETTINGS_DETAIL_SAFE_MODE,
    SETTINGS_DETAIL_BATTERY,
} settings_detail_kind_t;

static settings_detail_kind_t s_detail_kind = SETTINGS_DETAIL_ABOUT;
static const char *TAG = "settings_app";
static void settings_wlan_request_scan(void);
static void settings_wlan_phone_refresh(esp_gsp_handle_t ui);
static void settings_wlan_complete_phone_setup(esp_gsp_handle_t ui);
static void settings_wlan_apply_phone_status(
    esp_gsp_handle_t ui, const mosaic_cap_wifi_t *network);
static settings_ui_state_t s_state = {
    .brightness = SETTINGS_DEFAULT_BRIGHTNESS,
    .volume = SETTINGS_DEFAULT_VOLUME,
    .brightness_hw_available = true,
    .sound_effect_enabled = true,
    .wlan = {
        .enabled = true,
        .auto_join = true,
    },
};

static void keep_first_error(esp_err_t *result, esp_err_t candidate)
{
    if (*result == ESP_OK && candidate != ESP_OK) {
        *result = candidate;
    }
}

/* Boards without a Wi-Fi radio leave net.wifi unregistered, which is what
 * puts the WLAN pages into their mock/demo presentation. */
static bool settings_wifi_backend_available(void)
{
    return mosaic_capability_available("net.wifi") &&
        mosaic_capability_available("net.wifi.scan");
}

static esp_err_t settings_wifi_status(mosaic_cap_wifi_t *out)
{
    return mosaic_capability_read("net.wifi", SETTINGS_CAPABILITIES,
        out, sizeof(*out));
}

static esp_err_t settings_wifi_set_enabled(bool enabled)
{
    const mosaic_cap_wifi_enable_args_t args = { .enabled = enabled };
    return mosaic_capability_invoke("net.wifi", SETTINGS_CAPABILITIES,
        "set_enabled", &args, sizeof(args), NULL, 0);
}

static esp_err_t settings_wifi_connect(const char *ssid, const char *password)
{
    mosaic_cap_wifi_connect_args_t args = {0};
    strlcpy(args.ssid, ssid, sizeof(args.ssid));
    if (password != NULL) {
        strlcpy(args.password, password, sizeof(args.password));
    }
    return mosaic_capability_invoke("net.wifi", SETTINGS_CAPABILITIES,
        "connect", &args, sizeof(args), NULL, 0);
}

static esp_err_t settings_wifi_forget(void)
{
    return mosaic_capability_invoke("net.wifi", SETTINGS_CAPABILITIES,
        "forget", NULL, 0, NULL, 0);
}

static esp_err_t settings_wifi_request_scan(void)
{
    return mosaic_capability_invoke("net.wifi.scan", SETTINGS_CAPABILITIES,
        "start", NULL, 0, NULL, 0);
}

/* Scan results are ~900 bytes, so they are staged on the heap and copied out
 * into the caller's fixed-size list. */
static esp_err_t settings_wifi_scan_results(
    mosaic_cap_wifi_ap_t *entries, size_t *out_count)
{
    mosaic_cap_wifi_scan_t *scan = calloc(1, sizeof(*scan));
    if (scan == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = mosaic_capability_read("net.wifi.scan",
        SETTINGS_CAPABILITIES, scan, sizeof(*scan));
    if (err == ESP_OK) {
        size_t count = scan->count;
        if (count > MOSAIC_CAP_WIFI_SCAN_MAX) {
            count = MOSAIC_CAP_WIFI_SCAN_MAX;
        }
        if (count > 0) {
            memcpy(entries, scan->entries, count * sizeof(scan->entries[0]));
        }
        *out_count = count;
    }
    free(scan);
    return err;
}

static esp_err_t settings_set_brightness(int32_t brightness, bool persist)
{
    const mosaic_cap_display_brightness_args_t args = {
        .brightness = brightness,
        .persist = persist,
    };
    return mosaic_capability_invoke("system.display", SETTINGS_CAPABILITIES,
        "set_brightness", &args, sizeof(args), NULL, 0);
}

static esp_err_t settings_set_volume(int32_t volume, bool persist)
{
    const mosaic_cap_audio_volume_args_t args = {
        .volume = volume,
        .persist = persist,
    };
    return mosaic_capability_invoke("system.audio", SETTINGS_CAPABILITIES,
        "set_volume", &args, sizeof(args), NULL, 0);
}


static esp_err_t settings_display_render(esp_gsp_handle_t ui)
{
    char value[8];
    const uint32_t timeout_ms = s_state.device.display.screen_timeout_ms;
    uint32_t timeout_index = 5U;
    if (timeout_ms == 10000U) {
        timeout_index = 0U;
    } else if (timeout_ms == 30000U) {
        timeout_index = 1U;
    } else if (timeout_ms == 60000U) {
        timeout_index = 2U;
    } else if (timeout_ms == 120000U) {
        timeout_index = 3U;
    } else if (timeout_ms == 300000U) {
        timeout_index = 4U;
    }
    uint32_t rotation_index = s_state.device.display.rotation_degrees / 90U;
    if (rotation_index > 3U) {
        rotation_index = 0U;
    }
    esp_gsp_err_t err = gsp_settings_settings_rotation_dropdown_set_selected(ui, rotation_index);
    (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                   s_state.brightness);
    if (err == ESP_GSP_OK) {
        err =
        gsp_settings_settings_brightness_value_set_text(ui, value);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_screen_timeout_dropdown_set_selected(ui, timeout_index);
    }
    if (err == ESP_GSP_OK) {
        (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                       s_state.volume);
        err = gsp_settings_settings_volume_value_set_text(ui, value);
    }
    int32_t brightness_width =
        (s_state.brightness * (SETTINGS_TRACK_X_MAX - SETTINGS_TRACK_X_MIN) +
         50) / 100;
    int32_t volume_width =
        (s_state.volume * (SETTINGS_TRACK_X_MAX - SETTINGS_TRACK_X_MIN) +
         50) / 100;
    if (brightness_width < 1) {
        brightness_width = 1;
    }
    if (volume_width < 1) {
        volume_width = 1;
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_brightness_fill_set_w(
            ui, brightness_width);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_volume_fill_set_w(ui, volume_width);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_notification_sound_set_checked(
            ui, s_state.sound_effect_enabled);
    }
    return err == ESP_GSP_OK ? ESP_OK : ESP_FAIL;
}

static void settings_display_request_render(void)
{
    atomic_store_explicit(
        &s_state.display_render_pending, true, memory_order_release);
}

static void settings_display_render_timer_cb(
    esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    if (ui != s_state.ui) {
        return;
    }
    if (s_state.factory_hold_active) {
        uint16_t top = 0;
        if (esp_gsp_stack_view_get_top(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, &top) != ESP_GSP_OK ||
                top != SETTINGS_FACTORY_PAGE) {
            s_state.factory_hold_active = false;
            (void)gsp_settings_settings_factory_hold_progress_set_value(
                ui, 0);
        } else {
            const int64_t elapsed =
                esp_timer_get_time() - s_state.factory_hold_started_us;
            int32_t progress = (int32_t)(elapsed * 100 /
                                         SETTINGS_FACTORY_HOLD_US);
            if (progress < 0) progress = 0;
            if (progress > 100) {
                progress = 100;
            }
            (void)gsp_settings_settings_factory_hold_progress_set_value(
                ui, progress);
            if (elapsed >= SETTINGS_FACTORY_HOLD_US) {
                s_state.factory_hold_active = false;
                s_state.factory_hold_completed = true;
                esp_err_t err = mosaic_capability_invoke(
                    "system.lifecycle", SETTINGS_CAPABILITIES,
                    "factory_reset", NULL, 0, NULL, 0);
                if (err == ESP_OK) {
                    (void)mosaic_top_notice_show(
                        ui, &s_top_notice, "Erase Started",
                        "Restarting to finish reset",
                        SETTINGS_NOTICE_DURATION_MS);
                } else {
                    s_state.factory_hold_completed = false;
                    (void)gsp_settings_settings_factory_hold_progress_set_value(
                        ui, 0);
                    (void)mosaic_top_notice_show(
                        ui, &s_top_notice, "Reset Failed",
                        "Please try again", SETTINGS_NOTICE_DURATION_MS);
                }
            }
        }
    }
    if (atomic_exchange_explicit(
            &s_state.display_render_pending, false,
            memory_order_acq_rel)) {
        esp_err_t err = settings_display_render(ui);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "deferred Display render failed: %s",
                     esp_err_to_name(err));
        }
    }
}

typedef struct {
    const char *title;
    const char *value;
    bool ok;
    bool danger;
    bool action;
} settings_detail_row_t;

static const settings_detail_row_t s_about_rows[] = {
    {"Model", "ESP-MOSAICO", false, false, false},
    {"Software", "--", false, false, false},
    {"GSP", "--", false, false, false},
    {"Display", "2.16\" 480×480", false, false, false},
    {"Serial", "MSC-0427-8831", false, false, false},
    {"Update", "", false, true, true},
};

static const settings_detail_row_t s_safe_rows[] = {
    {"Third-Party Skills", "Disabled", false, false, false},
    {"Network", "Wi-Fi Only", false, false, false},
    {"Accessories", "Read Only", false, false, false},
    {"Exit Safe Mode", "Restart", false, true, true},
};

static const settings_detail_row_t s_battery_rows[] = {
    {"Charge", "--", false, false, false},
    {"Voltage", "--", false, false, false},
    {"Current", "--", false, false, false},
};

static const settings_detail_row_t *settings_detail_rows(size_t *count)
{
    switch (s_detail_kind) {
    case SETTINGS_DETAIL_SAFE_MODE:
        *count = sizeof(s_safe_rows) / sizeof(s_safe_rows[0]);
        return s_safe_rows;
    case SETTINGS_DETAIL_BATTERY:
        *count = sizeof(s_battery_rows) / sizeof(s_battery_rows[0]);
        return s_battery_rows;
    case SETTINGS_DETAIL_ABOUT:
    default:
        *count = sizeof(s_about_rows) / sizeof(s_about_rows[0]);
        return s_about_rows;
    }
}

static gsp_err_t settings_detail_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    size_t count = 0;
    const settings_detail_row_t *rows = settings_detail_rows(&count);
    if (item_index >= count) {
        return GSP_ERR_INVALID_ARG;
    }
    const settings_detail_row_t *item = &rows[item_index];
    char value_text[64];
    const char *value = item->value;
    if (s_detail_kind == SETTINGS_DETAIL_ABOUT) {
        switch (item_index) {
        case 1:
            value = s_state.device.lifecycle.software_version[0] != '\0'
                ? s_state.device.lifecycle.software_version : "--";
            break;
        case 2:
            (void)snprintf(value_text, sizeof(value_text), "v%u.%u.%u",
                           GSP_VERSION_MAJOR, GSP_VERSION_MINOR,
                           GSP_VERSION_PATCH);
            value = value_text;
            break;
        case 5:
            switch (s_state.device.update.state) {
            case MOSAIC_CAP_UPDATE_CHECKING:
                value = "Checking...";
                break;
            case MOSAIC_CAP_UPDATE_UP_TO_DATE:
                value = "Up to date";
                break;
            case MOSAIC_CAP_UPDATE_AVAILABLE:
                if (s_state.device.update.latest_version[0] != '\0') {
                    (void)snprintf(value_text, sizeof(value_text), "v%s available",
                                   s_state.device.update.latest_version);
                    value = value_text;
                }
                break;
            case MOSAIC_CAP_UPDATE_DEVICE_AHEAD:
                value = "Device is newer";
                break;
            case MOSAIC_CAP_UPDATE_FAILED:
                value = "Check failed";
                break;
            case MOSAIC_CAP_UPDATE_IDLE:
            default:
                value = "Check now";
                break;
            }
            break;
        default:
            break;
        }
    } else if (s_detail_kind == SETTINGS_DETAIL_BATTERY &&
            s_state.device.power.available) {
        const mosaic_cap_power_t *battery = &s_state.device.power;
        switch (item_index) {
        case 0:
            snprintf(value_text, sizeof(value_text), "%u%%",
                     battery->percent);
            value = value_text;
            break;
        case 1:
            snprintf(value_text, sizeof(value_text), "%u mV",
                     battery->voltage_mv);
            value = value_text;
            break;
        case 2:
            snprintf(value_text, sizeof(value_text), "%d mA",
                     (int)battery->current_ma);
            value = value_text;
            break;
        default:
            break;
        }
    }
    esp_gsp_err_t err = gsp_settings_settings_detail_row_row_set_title_text(
        ui, row, item->title);
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_title_fg_color(
            ui, row, item->danger ? SETTINGS_TOGGLE_ON_COLOR
                                  : SETTINGS_TEXT_COLOR);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_value_text(
            ui, row, value);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_value_fg_color(
            ui, row, item->ok ? UINT32_C(0x6610) : SETTINGS_MUTED_COLOR);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_detail_row_row_set_chevron_text(
            ui, row, item->action ? "›" : "");
    }
    return err == ESP_GSP_OK ? GSP_OK : GSP_ERR_INVALID_STATE;
}

static void settings_detail_list_attach(esp_gsp_handle_t ui)
{
    if (s_detail_list == ESP_GSP_LIST_NONE) {
        s_detail_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_SETTINGS_DETAIL_LIST,
            settings_detail_bind_item, NULL);
    }
}

static void settings_open_detail(esp_gsp_handle_t ui,
                                 settings_detail_kind_t kind)
{
    static const char *titles[] = {"About", "Safe Mode", "Battery"};
    s_detail_kind = kind;
    size_t count = 0;
    (void)settings_detail_rows(&count);
    settings_detail_list_attach(ui);
    (void)gsp_settings_nav_settings_detail_title_set_text(ui, titles[kind]);
    (void)esp_gsp_list_set_total(ui, s_detail_list, count);
    (void)esp_gsp_list_refresh(ui, s_detail_list);
    (void)esp_gsp_list_scroll_to(ui, s_detail_list, 0);
    if (esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_SETTINGS_STACK,
                               SETTINGS_DETAIL_PAGE, true) == ESP_GSP_OK) {
        mosaic_app_shell_set_root_visible(ui, false);
    }
}

typedef struct {
    const char *title;
    const uint8_t *icon;
    size_t icon_size;
} settings_root_row_t;

#define SETTINGS_ROOT_ROW(_title, _name) { \
    .title = (_title), \
    .icon = settings_root_icon_##_name, \
    .icon_size = sizeof(settings_root_icon_##_name), \
}

static const settings_root_row_t s_root_rows[] = {
    SETTINGS_ROOT_ROW("AI Model", ai),
    SETTINGS_ROOT_ROW("IM Channels", channels),
    SETTINGS_ROOT_ROW("Network", network),
    SETTINGS_ROOT_ROW("Display", display),
    SETTINGS_ROOT_ROW("Sound", sound),
    SETTINGS_ROOT_ROW("Security", security),
    SETTINGS_ROOT_ROW("Battery", battery),
    SETTINGS_ROOT_ROW("About", about),
};

static const char *settings_wlan_summary(void)
{
    const settings_wlan_state_t *wlan = &s_state.wlan;
    if (!wlan->enabled || wlan->state == MOSAIC_CAP_WIFI_DISABLED) {
        return "Off";
    }
    if (wlan->state == MOSAIC_CAP_WIFI_SCANNING) {
        return "Scanning";
    }
    if (wlan->state == MOSAIC_CAP_WIFI_CONNECTING) {
        return "Connecting";
    }
    if (wlan->state == MOSAIC_CAP_WIFI_AUTH_FAILED) {
        return "Wrong password";
    }
    if (wlan->state == MOSAIC_CAP_WIFI_AP_NOT_FOUND) {
        return "Not found";
    }
    if (wlan->state == MOSAIC_CAP_WIFI_RETRY_WAIT) {
        return "Retrying";
    }
    if (wlan->connected && wlan->current_ssid[0] != '\0') {
        return wlan->current_ssid;
    }
    return "Offline";
}

static bool settings_llm_is_bound(void)
{
    return s_state.device.agent.llm_configured;
}

static bool settings_any_im_is_bound(void)
{
    return s_state.device.agent.im_wechat_configured ||
           s_state.device.agent.im_qq_configured ||
           s_state.device.agent.im_feishu_configured ||
           s_state.device.agent.im_telegram_configured;
}

static gsp_err_t settings_root_list_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    if (item_index >= sizeof(s_root_rows) / sizeof(s_root_rows[0])) {
        return GSP_ERR_INVALID_ARG;
    }
    char value[16] = "";
    const char *value_text = value;
    switch (item_index) {
    case 0:
        value_text = settings_llm_is_bound() ? "Bound" : "Unbound";
        break;
    case 1:
        value_text = settings_any_im_is_bound() ? "Bound" : "Unbound";
        break;
    case 2:
        value_text = settings_wlan_summary();
        break;
    case 3:
        (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                       s_state.brightness);
        break;
    case 4:
        (void)snprintf(value, sizeof(value), "%" PRId32 "%%",
                       s_state.volume);
        break;
    case 6:
        if (s_state.device.power.available) {
            (void)snprintf(value, sizeof(value), "%u%%",
                           s_state.device.power.percent);
        } else {
            value_text = "--";
        }
        break;
    default:
        break;
    }
    esp_gsp_err_t err =
        gsp_settings_settings_root_row_row_set_icon_resource(
            ui, row, s_root_rows[item_index].icon,
            s_root_rows[item_index].icon_size);
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_root_row_row_set_title_text(
            ui, row, s_root_rows[item_index].title);
    }
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_root_row_row_set_value_text(
            ui, row, value_text);
    }
    return err;
}

static void settings_root_list_refresh(esp_gsp_handle_t ui)
{
    if (s_root_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_refresh(ui, s_root_list);
    }
}

static void settings_root_list_attach(esp_gsp_handle_t ui)
{
    if (s_root_list == ESP_GSP_LIST_NONE) {
        s_root_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_SETTINGS_ROOT_LIST,
            settings_root_list_bind_item, NULL);
    }
    if (s_root_list == ESP_GSP_LIST_NONE) {
        ESP_LOGW(TAG, "bind Settings root list failed");
        return;
    }
    (void)esp_gsp_list_set_total(
        ui, s_root_list, sizeof(s_root_rows) / sizeof(s_root_rows[0]));
    settings_root_list_refresh(ui);
}

static gsp_err_t settings_wlan_list_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    if (item_index >= s_wlan_network_count) {
        /* set_total(0) can retire a materialized row one update later. Keep
         * that transient row empty instead of turning normal List teardown
         * into a binding error. */
        return esp_gsp_row_text(ui, row, "");
    }
    return esp_gsp_row_text(ui, row, s_wlan_networks[item_index].ssid);
}

static bool settings_wlan_show_current(void)
{
    return s_state.wlan_page_active && s_state.wlan.enabled &&
           (s_state.wlan.connected || s_state.wlan_connect_pending) &&
           s_state.wlan.current_ssid[0] != '\0';
}

static void settings_wlan_refresh_list(esp_gsp_handle_t ui)
{
    if (s_wlan_list == ESP_GSP_LIST_NONE) {
        return;
    }
    esp_gsp_err_t err = esp_gsp_list_set_total(
        ui, s_wlan_list, s_wlan_network_count);
    if (err == ESP_GSP_OK) {
        err = esp_gsp_list_refresh(ui, s_wlan_list);
    }
    if (err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "refresh WLAN List failed: 0x%x", (unsigned)err);
    }
}

static void settings_wlan_scan(esp_gsp_handle_t ui)
{
    mosaic_cap_wifi_ap_t
        networks[MOSAIC_CAP_WIFI_SCAN_MAX] = {0};
    size_t count = 0;
    esp_err_t scan_err = settings_wifi_scan_results(networks, &count);
    if (scan_err != ESP_OK) {
        if (!s_state.wlan_scan_waiting &&
                s_state.wlan_scan_next_us == 0) {
            s_state.wlan_scan_next_us = esp_timer_get_time();
        }
        return;
    }
    /* Host providers can publish a synchronous result without scan metadata.
     * Device providers complete through scan_revision below. */
    if (s_state.wlan_scan_waiting &&
            s_state.device.network.scan_revision == 0U) {
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = esp_timer_get_time() +
            SETTINGS_WLAN_SCAN_REFRESH_US;
    }
    if (count > MOSAIC_CAP_WIFI_SCAN_MAX) {
        count = MOSAIC_CAP_WIFI_SCAN_MAX;
    }
    const bool changed = count != s_wlan_network_count ||
        (count != 0 && memcmp(s_wlan_networks, networks,
                              count * sizeof(networks[0])) != 0);
    if (!changed) {
        return;
    }
    memcpy(s_wlan_networks, networks, count * sizeof(networks[0]));
    s_wlan_network_count = count;
    settings_wlan_refresh_list(ui);
}

static void settings_wlan_request_scan(void)
{
    if (!settings_wifi_backend_available() || !s_state.wlan.enabled ||
            s_state.device.network.radio_state !=
                MOSAIC_CAP_WIFI_RADIO_ON) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    esp_err_t err = settings_wifi_request_scan();
    if (err == ESP_OK) {
        s_state.wlan_scan_waiting = true;
        s_state.wlan_scan_next_us = now + SETTINGS_WLAN_SCAN_TIMEOUT_US;
    } else {
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = now + SETTINGS_WLAN_SCAN_RETRY_US;
        ESP_LOGW(TAG, "request WLAN scan failed: %s",
                 esp_err_to_name(err));
    }
}

static void settings_wlan_update_summary(esp_gsp_handle_t ui)
{
    const settings_wlan_state_t *wlan = &s_state.wlan;
    settings_root_list_refresh(ui);
    s_state.device.network.connected =
        wlan->enabled && wlan->connected;
    if (wlan->connected && wlan->current_ssid[0] != '\0') {
        strlcpy(s_state.device.network.ssid, wlan->current_ssid,
                sizeof(s_state.device.network.ssid));
    }
}

static void settings_wlan_list_attach(esp_gsp_handle_t ui)
{
    if (s_wlan_list == ESP_GSP_LIST_NONE) {
        s_wlan_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_SETTINGS_WLAN_SCAN_LIST,
            settings_wlan_list_bind_item, NULL);
    }
    if (s_wlan_list == ESP_GSP_LIST_NONE) {
        ESP_LOGW(TAG, "bind WLAN scan list failed");
        return;
    }
    if (s_wlan_network_count == 0) {
        settings_wlan_scan(ui);
    }
    settings_wlan_refresh_list(ui);
}

static void settings_wlan_list_park(esp_gsp_handle_t ui)
{
    /* Dynamic row instances are owned by the List driver, not by the scene
     * layer that authored the viewport. Emptying the List is the explicit
     * lifecycle boundary that removes rows before the Settings root returns. */
    if (s_wlan_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_set_total(ui, s_wlan_list, 0);
        (void)esp_gsp_list_refresh(ui, s_wlan_list);
    }
}

static void settings_wlan_password_attach(esp_gsp_handle_t ui)
{
    s_state.wlan_password_active = true;
    mosaic_app_shell_set_bottom_enabled(ui, false);
    (void)esp_gsp_set_cursor(ui, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE);
    (void)esp_gsp_keyboard_attach(
        ui, GSP_ACT_ID_SETTINGS_WLAN_PASSWORD_KEYBOARD_KEY,
        GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE);
}

static void settings_wlan_password_detach(esp_gsp_handle_t ui)
{
    /* Close the logical input session before touching framework state. A key
     * release already queued by the previous frame must not submit or edit
     * after Cancel/Join starts navigating away. */
    s_state.wlan_password_active = false;
    mosaic_app_shell_set_bottom_enabled(ui, true);
    mosaic_top_notice_hide(ui);
    (void)esp_gsp_set_cursor(ui, ESP_GSP_NO_CURSOR);
    (void)esp_gsp_keyboard_attach(
        ui, ESP_GSP_KEYBOARD_NONE, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE);
    /* Clear the retained scene value while this page is still current. The
     * next StackView push may snapshot the password page before attach(), so
     * leaving the bind populated causes the previous password to flash. */
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE, "");
    s_state.wlan.password[0] = '\0';
    s_state.wlan.selected_ssid[0] = '\0';
}

static uint32_t settings_wlan_network_index(const mosaic_event_t *event)
{
    if (event != NULL && event->data.call.list != ESP_GSP_LIST_NONE) {
        return event->data.call.item;
    }
    return event != NULL ? event->data.call.arg : 0U;
}

static esp_gsp_err_t settings_wlan_loading_frame_set_visible(
    esp_gsp_handle_t ui, uint8_t frame, bool visible)
{
    switch (frame) {
    case 0:
        return gsp_settings_settings_wlan_current_loading_0_set_visible(
            ui, visible);
    case 1:
        return gsp_settings_settings_wlan_current_loading_1_set_visible(
            ui, visible);
    case 2:
        return gsp_settings_settings_wlan_current_loading_2_set_visible(
            ui, visible);
    case 3:
        return gsp_settings_settings_wlan_current_loading_3_set_visible(
            ui, visible);
    case 4:
        return gsp_settings_settings_wlan_current_loading_4_set_visible(
            ui, visible);
    case 5:
        return gsp_settings_settings_wlan_current_loading_5_set_visible(
            ui, visible);
    case 6:
        return gsp_settings_settings_wlan_current_loading_6_set_visible(
            ui, visible);
    case 7:
        return gsp_settings_settings_wlan_current_loading_7_set_visible(
            ui, visible);
    default:
        return ESP_GSP_ERR_INVALID_ARG;
    }
}

static void settings_wlan_loading_timer_cb(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (ui != s_state.ui || !s_state.wlan_loading_visible) {
        return;
    }
    const uint8_t previous = s_state.wlan_loading_frame;
    const uint8_t next =
        (uint8_t)((previous + 1U) % SETTINGS_WLAN_LOADING_FRAME_COUNT);
    (void)settings_wlan_loading_frame_set_visible(ui, previous, false);
    (void)settings_wlan_loading_frame_set_visible(ui, next, true);
    s_state.wlan_loading_frame = next;
}

static esp_gsp_err_t settings_wlan_set_loading(
    esp_gsp_handle_t ui, bool visible)
{
    if (visible == s_state.wlan_loading_visible) {
        return ESP_GSP_OK;
    }
    if (visible) {
        s_state.wlan_loading_frame = 0;
        esp_gsp_err_t err = settings_wlan_loading_frame_set_visible(
            ui, 0, true);
        if (err != ESP_GSP_OK) {
            return err;
        }
        s_state.wlan_loading_timer = esp_gsp_timer_create(
            ui, SETTINGS_WLAN_LOADING_PERIOD_MS,
            settings_wlan_loading_timer_cb, NULL);
        if (s_state.wlan_loading_timer == NULL) {
            (void)settings_wlan_loading_frame_set_visible(ui, 0, false);
            return ESP_GSP_ERR_NO_MEM;
        }
        s_state.wlan_loading_visible = true;
        return ESP_GSP_OK;
    }
    if (s_state.wlan_loading_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_state.wlan_loading_timer);
        s_state.wlan_loading_timer = NULL;
    }
    esp_gsp_err_t err = settings_wlan_loading_frame_set_visible(
        ui, s_state.wlan_loading_frame, false);
    s_state.wlan_loading_visible = false;
    s_state.wlan_loading_frame = 0;
    return err;
}

static esp_err_t settings_wlan_render(esp_gsp_handle_t ui)
{
    const settings_wlan_state_t *wlan = &s_state.wlan;
    const bool page_enabled = s_state.wlan_page_active && wlan->enabled;
    const bool show_current = settings_wlan_show_current();
    const char *status = "Not Connected";
    if (s_state.wlan_connect_pending && !wlan->connected) {
        /* Provider setup can briefly publish IDLE while credentials are
         * persisted and the STA is restarted. Keep the user-facing operation
         * in Joining until a terminal success/failure event arrives. */
        status = "Joining...";
    } else switch (wlan->state) {
    case MOSAIC_CAP_WIFI_CONNECTING:
        status = "Joining...";
        break;
    case MOSAIC_CAP_WIFI_CONNECTED:
        status = "Current Network";
        break;
    case MOSAIC_CAP_WIFI_RETRY_WAIT:
        status = "Waiting to reconnect";
        break;
    case MOSAIC_CAP_WIFI_AUTH_FAILED:
        status = "Incorrect password";
        break;
    case MOSAIC_CAP_WIFI_AP_NOT_FOUND:
        status = "Network not found";
        break;
    case MOSAIC_CAP_WIFI_FAILED:
        status = "Connection failed";
        break;
    default:
        break;
    }
    esp_err_t result = ESP_OK;

    keep_first_error(&result,
        gsp_settings_settings_wlan_enabled_set_checked(ui, wlan->enabled));
    keep_first_error(&result,
        gsp_settings_settings_wlan_off_layer_set_visible(
            ui, false));
    keep_first_error(&result,
        gsp_settings_settings_wlan_list_layer_set_visible(
            ui, page_enabled));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_layer_set_visible(
            ui, show_current));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_check_set_visible(
            ui, show_current && wlan->connected));
    keep_first_error(&result, settings_wlan_set_loading(
        ui, show_current && !wlan->connected));
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_error_set_visible(
            ui, false));
    const bool compact = !show_current;
    if (!s_state.wlan_scan_compact_valid ||
            s_state.wlan_scan_compact != compact) {
        keep_first_error(&result, esp_gsp_component_set_position(
            ui, GSP_OBJ_KEY_SETTINGS_WLAN_SCAN_LAYER,
            0, compact ? 0 : 100));
        s_state.wlan_scan_compact = compact;
        s_state.wlan_scan_compact_valid = true;
    }
    keep_first_error(&result,
        gsp_settings_settings_wlan_current_status_set_text(ui, status));
    if (wlan->current_ssid[0] != '\0') {
        keep_first_error(&result,
            gsp_settings_settings_wlan_current_ssid_set_text(
                ui, wlan->current_ssid));
    } else {
        keep_first_error(&result,
            gsp_settings_settings_wlan_current_ssid_set_text(ui, "--"));
    }
    if (wlan->connected && wlan->current_ssid[0] != '\0') {
        keep_first_error(&result,
            gsp_settings_settings_wlan_detail_ssid_set_text(
                ui, wlan->current_ssid));
    } else {
        keep_first_error(&result,
            gsp_settings_settings_wlan_detail_ssid_set_text(ui, "Network"));
    }
    keep_first_error(&result,
        gsp_settings_settings_wlan_auto_join_set_checked(
            ui, wlan->auto_join));
    if (wlan->selected_ssid[0] != '\0') {
        keep_first_error(&result,
            gsp_settings_settings_wlan_password_ssid_set_text(
                ui, wlan->selected_ssid));
    }
    settings_wlan_update_summary(ui);
    if (page_enabled) {
        settings_wlan_refresh_list(ui);
    }
    return result;
}

static void settings_wlan_refresh_status(esp_gsp_handle_t ui)
{
    if (!settings_wifi_backend_available()) {
        return;
    }
    mosaic_cap_wifi_t network = s_state.device.network;
    if (settings_wifi_status(&network) != ESP_OK) {
        return;
    }
    const bool was_enabled = s_state.wlan.enabled;
    const mosaic_cap_wifi_radio_state_t previous_radio =
        s_state.device.network.radio_state;
    const bool host_terminal_without_operation =
        network.operation_id == 0U &&
        s_state.wlan_connect_operation_id == 0U &&
        (network.state == MOSAIC_CAP_WIFI_CONNECTED ||
         network.state == MOSAIC_CAP_WIFI_AUTH_FAILED ||
         network.state == MOSAIC_CAP_WIFI_AP_NOT_FOUND ||
         network.state == MOSAIC_CAP_WIFI_FAILED);
    const bool stale_connect_snapshot = s_state.wlan_connect_pending &&
        network.operation_id == s_state.wlan_connect_operation_id &&
        !host_terminal_without_operation;
    const bool presentation_changed =
        !stale_connect_snapshot &&
        (s_state.device.network.desired_enabled != network.desired_enabled ||
         s_state.device.network.connected != network.connected ||
         s_state.device.network.state != network.state ||
         strcmp(s_state.device.network.ssid, network.ssid) != 0);
    s_state.device.network = network;
    s_state.wlan.enabled = network.desired_enabled;
    if (!stale_connect_snapshot) {
        s_state.wlan.connected = network.connected;
        s_state.wlan.state = network.state;
    }
    /* Saving credentials may restart Wi-Fi through an unconfigured/IDLE
     * snapshot whose SSID is temporarily empty. Preserve the selected target
     * while the connection transaction is pending so the Connecting card
     * does not blink out before the terminal event. */
    if (!s_state.wlan_connect_pending ||
            (network.operation_id != s_state.wlan_connect_operation_id &&
             network.ssid[0] != '\0')) {
        strlcpy(s_state.wlan.current_ssid, network.ssid,
                sizeof(s_state.wlan.current_ssid));
    }
    const int64_t now = esp_timer_get_time();
    if (network.scan_revision != s_state.wlan_scan_revision) {
        s_state.wlan_scan_revision = network.scan_revision;
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = now +
            (network.scan_error == ESP_OK
                ? SETTINGS_WLAN_SCAN_REFRESH_US
                : SETTINGS_WLAN_SCAN_RETRY_US);
        if (network.scan_error == ESP_OK) {
            settings_wlan_scan(ui);
        }
    }
    if (!s_state.wlan.enabled) {
        s_state.wlan_scan_waiting = false;
        s_state.wlan_scan_next_us = 0;
        if (s_state.wlan_page_active) {
            settings_wlan_list_park(ui);
        }
    } else if (s_state.wlan_page_active &&
            (!was_enabled ||
             (previous_radio != MOSAIC_CAP_WIFI_RADIO_ON &&
              network.radio_state == MOSAIC_CAP_WIFI_RADIO_ON))) {
        s_state.wlan_scan_next_us = now;
        settings_wlan_list_attach(ui);
    }
    bool stack_animating = false;
    const bool transition_owns_snapshot = s_state.wlan_page_active &&
        esp_gsp_stack_view_is_animating(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &stack_animating) == ESP_GSP_OK &&
        stack_animating;
    /* Keep authored objects unchanged while StackView owns a viewport
     * snapshot. Device Wi-Fi events can otherwise mutate only the bound lower
     * layer midway through the transition and leave a half-page image. */
    if (presentation_changed && !transition_owns_snapshot) {
        (void)settings_wlan_render(ui);
    }
    settings_wlan_apply_phone_status(ui, &network);
    if (s_state.wlan_connect_pending &&
            (network.operation_id != s_state.wlan_connect_operation_id ||
             host_terminal_without_operation)) {
        const char *title = NULL;
        const char *message = NULL;
        switch (network.state) {
        case MOSAIC_CAP_WIFI_CONNECTED:
            if (network.connected) {
                s_state.wlan_connect_pending = false;
                s_state.wlan_connect_operation_id = network.operation_id;
                s_state.wlan_scan_next_us = now;
            }
            break;
        case MOSAIC_CAP_WIFI_AUTH_FAILED:
            title = "Incorrect password";
            message = "Check the password and try again";
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            (void)settings_wifi_forget();
            s_state.wlan.connected = false;
            s_state.wlan.current_ssid[0] = '\0';
            s_state.device.network.connected = false;
            s_state.device.network.configured = false;
            s_state.device.network.ssid[0] = '\0';
            break;
        case MOSAIC_CAP_WIFI_AP_NOT_FOUND:
            title = "Network unavailable";
            message = "The selected WLAN was not found";
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            break;
        case MOSAIC_CAP_WIFI_FAILED:
            title = "Connection failed";
            message = "Check the network and try again";
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            break;
        case MOSAIC_CAP_WIFI_DISABLED:
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_operation_id = network.operation_id;
            break;
        default:
            break;
        }
        if (!s_state.wlan_connect_pending) {
            s_state.wlan_connect_deadline_us = 0;
        }
        if (title != NULL && s_state.wlan_page_active) {
            s_state.wlan_scan_next_us = now;
            (void)settings_wlan_render(ui);
            /* Rendering the compact List and publishing the provider's
             * follow-up IDLE state can fill the device update queue in this
             * frame. Keep the terminal failure latched and submit the notice
             * by itself from a stable UI tick; SDL usually has enough room,
             * which previously hid this device-only loss. */
            s_state.wlan_notice_title = title;
            s_state.wlan_notice_message = message;
            s_state.wlan_notice_pending = true;
        }
    }
}

static bool settings_wlan_show_pending_notice(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_notice_pending || !s_state.wlan_page_active ||
            s_state.wlan_password_active || s_state.wlan_leave_pending) {
        return false;
    }
    bool animating = false;
    if (esp_gsp_stack_view_is_animating(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &animating) != ESP_GSP_OK ||
            animating) {
        return false;
    }
    if (mosaic_top_notice_show(
            ui, &s_top_notice, s_state.wlan_notice_title,
            s_state.wlan_notice_message,
            SETTINGS_NOTICE_DURATION_MS) == ESP_GSP_OK) {
        s_state.wlan_notice_pending = false;
        s_state.wlan_notice_title = NULL;
        s_state.wlan_notice_message = NULL;
        return true;
    }
    return false;
}

static void settings_wlan_prepare_password(const char *ssid)
{
    strlcpy(s_state.wlan.selected_ssid, ssid,
            sizeof(s_state.wlan.selected_ssid));
}

static void settings_wlan_fake_connect(esp_gsp_handle_t ui, const char *ssid)
{
    strlcpy(s_state.wlan.current_ssid, ssid,
            sizeof(s_state.wlan.current_ssid));
    s_state.wlan.connected = true;
    s_state.wlan.state = MOSAIC_CAP_WIFI_CONNECTED;
    s_state.wlan.selected_ssid[0] = '\0';
    (void)settings_wlan_render(ui);
    settings_wlan_refresh_list(ui);
}

static void settings_wlan_connection_started(
    esp_gsp_handle_t ui, const char *ssid)
{
    s_state.wlan_connect_pending = true;
    s_state.wlan_connect_deadline_us = esp_timer_get_time() +
        SETTINGS_WLAN_CONNECT_TIMEOUT_US;
    s_state.wlan.connected = false;
    s_state.wlan.state = MOSAIC_CAP_WIFI_CONNECTING;
    strlcpy(s_state.wlan.current_ssid, ssid,
            sizeof(s_state.wlan.current_ssid));
    (void)settings_wlan_render(ui);
}

static void settings_wlan_pop_to_wlan(esp_gsp_handle_t ui)
{
    /* Each UI path calls this exactly once. StackView queues the command when
     * invoked from an input callback and applies it at the next frame. */
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, true) != ESP_GSP_OK) {
        ESP_LOGW(TAG, "return from WLAN password page failed");
    } else if (s_state.wlan_page_active && s_state.wlan.enabled) {
        settings_wlan_list_attach(ui);
    }
}

static void settings_wlan_cancel_password(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_password_active) {
        return;
    }
    settings_wlan_password_detach(ui);
    settings_wlan_pop_to_wlan(ui);
}

static void settings_wlan_finish_leave(esp_gsp_handle_t ui)
{
    s_state.wlan_page_active = false;
    s_state.wlan_entry_pending = false;
    s_state.wlan_leave_pending = false;
    s_state.wlan_detail_active = false;
    s_state.wlan_detail_leave_pending = false;
    s_state.wlan_forget_pending = false;
    /* StackView/List owns page visibility. Do not rewrite the now-offscreen
     * WLAN bindings here: on retained/partition displays those incremental
     * writes can race the landing frame and preserve half of the old page.
     * The bindings are refreshed before the next WLAN push. */
    settings_wlan_list_park(ui);
    mosaic_app_shell_sync(ui);
}

static bool settings_wlan_reconcile_navigation(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_page_active) {
        return false;
    }
    uint16_t page = UINT16_MAX;
    bool animating = false;
    if (esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK ||
            esp_gsp_stack_view_is_animating(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &animating) != ESP_GSP_OK) {
        return false;
    }
    if (s_state.wlan_phone_exit_pending &&
            page == SETTINGS_WLAN_PASSWORD_PAGE && !animating) {
        if (esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, true) == ESP_GSP_OK) {
            s_state.wlan_phone_exit_pending = false;
            s_state.wlan_phone_active = false;
            s_state.wlan_phone_operation_id = 0;
        }
        return true;
    }
    if (page == SETTINGS_WLAN_PAGE && !animating) {
        s_state.wlan_entry_pending = false;
        if (s_state.wlan_detail_leave_pending ||
                s_state.wlan_forget_pending) {
            s_state.wlan_detail_active = false;
            s_state.wlan_detail_leave_pending = false;
            s_state.wlan_forget_pending = false;
            if (s_state.wlan.enabled) {
                settings_wlan_list_attach(ui);
            }
            (void)settings_wlan_render(ui);
        }
    } else if (page == SETTINGS_WLAN_DETAIL_PAGE) {
        s_state.wlan_detail_active = true;
    }
    if (s_state.wlan_leave_pending && page == 0 && !animating) {
        settings_wlan_finish_leave(ui);
        return true;
    }
    return false;
}

static void settings_wlan_open_detail(esp_gsp_handle_t ui)
{
    if (s_state.wlan_detail_active ||
            s_state.wlan_detail_leave_pending ||
            s_state.wlan_forget_pending ||
            !s_state.wlan.connected ||
            s_state.wlan.current_ssid[0] == '\0') {
        return;
    }
    /* Claim the detail transaction before queueing. Several overlapping
     * visuals intentionally share this callback, and device touch can also
     * report a duplicate release before StackView updates its top. */
    s_state.wlan_detail_active = true;
    settings_wlan_list_park(ui);
    (void)settings_wlan_render(ui);
    if (esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_SETTINGS_STACK,
            SETTINGS_WLAN_DETAIL_PAGE, true) != ESP_GSP_OK) {
        s_state.wlan_detail_active = false;
        settings_wlan_list_attach(ui);
    }
}

static void settings_wlan_handle_network_select(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    /* Overlapping row visuals can deliver the same release more than once
     * before StackView reports its new top. Claim the input session before
     * pushing so one SSID tap cannot add the password page twice. */
    if (s_state.wlan_password_active) {
        return;
    }
    const uint32_t index = settings_wlan_network_index(event);
    if (index >= s_wlan_network_count) {
        return;
    }
    const char *ssid = s_wlan_networks[index].ssid;
    if (s_state.wlan.connected &&
            strcmp(s_state.wlan.current_ssid, ssid) == 0) {
        settings_wlan_open_detail(ui);
        return;
    }
    settings_wlan_prepare_password(ssid);
    if (!s_wlan_networks[index].secured) {
        if (!settings_wifi_backend_available()) {
            settings_wlan_fake_connect(ui, ssid);
        } else {
            s_state.wlan_connect_operation_id =
                s_state.device.network.operation_id;
            if (settings_wifi_connect(ssid, "") == ESP_OK) {
                settings_wlan_connection_started(ui, ssid);
                settings_wlan_refresh_status(ui);
            } else {
                (void)mosaic_top_notice_show(
                    ui, &s_top_notice, "Connection failed",
                    "Unable to start the connection",
                    SETTINGS_NOTICE_DURATION_MS);
            }
        }
        return;
    }
    /* Queue the empty value before StackView captures the incoming page.
     * attach() intentionally runs after push and only activates input. */
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_WLAN_PASSWORD_VALUE, "");
    (void)settings_wlan_render(ui);
    settings_wlan_list_park(ui);
    esp_gsp_err_t push_err = esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_SETTINGS_STACK,
            SETTINGS_WLAN_PASSWORD_PAGE, true);
    if (push_err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "open WLAN password page failed");
        settings_wlan_list_attach(ui);
        return;
    }
    settings_wlan_password_attach(ui);
}

static bool settings_wlan_handle_connect(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_password_active ||
            s_state.wlan.selected_ssid[0] == '\0') {
        return false;
    }
    char password[sizeof(s_state.wlan.password)] = {0};
    if (esp_gsp_keyboard_text(ui, password, sizeof(password)) != ESP_GSP_OK) {
        return false;
    }
    if (strlen(password) < SETTINGS_WLAN_PASSWORD_MIN) {
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, "Password too short",
            "Enter at least 8 characters",
            SETTINGS_NOTICE_DURATION_MS);
        return false;
    }
    char ssid[sizeof(s_state.wlan.selected_ssid)];
    strlcpy(ssid, s_state.wlan.selected_ssid, sizeof(ssid));
    strlcpy(s_state.wlan.password, password, sizeof(s_state.wlan.password));
    const bool simulated = !settings_wifi_backend_available();
    s_state.wlan_connect_pending = !simulated;
    s_state.wlan_connect_operation_id =
        s_state.device.network.operation_id;
    if (!simulated &&
            settings_wifi_connect(ssid, password) != ESP_OK) {
        s_state.wlan_connect_pending = false;
        ESP_LOGW(TAG, "connect WLAN %s failed", ssid);
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, "Connection failed",
            "Unable to start the connection",
            SETTINGS_NOTICE_DURATION_MS);
        return false;
    }
    settings_wlan_password_detach(ui);
    if (simulated) {
        settings_wlan_fake_connect(ui, ssid);
    } else {
        settings_wlan_connection_started(ui, ssid);
        /* Providers may complete synchronously (the SDL host intentionally
         * does). Consume that terminal state before returning from Join;
         * asynchronous device providers simply report CONNECTING/IDLE here
         * and deliver their terminal event through the normal model path. */
        settings_wlan_refresh_status(ui);
    }
    return true;
}

static void settings_wlan_complete_phone_setup(esp_gsp_handle_t ui)
{
    if (!s_state.wlan_phone_active || s_state.wlan_phone_exit_pending) {
        return;
    }
    /* The phone page is nested above the password page. Pop one level now;
     * reconciliation pops the password page after this transition settles. */
    s_state.wlan_phone_exit_pending = true;
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, true) != ESP_GSP_OK) {
        s_state.wlan_phone_exit_pending = false;
        return;
    }
    settings_wlan_request_scan();
}

static void settings_wlan_apply_phone_status(
    esp_gsp_handle_t ui, const mosaic_cap_wifi_t *network)
{
    if (!s_state.wlan_phone_active || s_state.wlan_phone_exit_pending ||
            network == NULL ||
            network->operation_id == s_state.wlan_phone_operation_id) {
        return;
    }

    const char *title = NULL;
    const char *message = NULL;
    switch (network->state) {
    case MOSAIC_CAP_WIFI_CONNECTED:
        if (network->connected) {
            settings_wlan_complete_phone_setup(ui);
        }
        return;
    case MOSAIC_CAP_WIFI_AUTH_FAILED:
        title = "Incorrect password";
        message = "Update the password on your phone";
        break;
    case MOSAIC_CAP_WIFI_AP_NOT_FOUND:
        title = "Network unavailable";
        message = "Choose another WLAN on your phone";
        break;
    case MOSAIC_CAP_WIFI_FAILED:
        title = "Connection failed";
        message = "Check the settings and try again";
        break;
    default:
        return;
    }

    /* Consume this failed portal submission exactly once. A later retry
     * receives a new Wi-Fi manager operation id and is evaluated normally. */
    s_state.wlan_phone_operation_id = network->operation_id;
    s_state.wlan_notice_title = title;
    s_state.wlan_notice_message = message;
    s_state.wlan_notice_pending = true;
}

static void settings_wlan_handle_phone_submitted(esp_gsp_handle_t ui)
{
    settings_wlan_refresh_status(ui);
    if (!s_state.wlan_phone_active || s_state.wlan_phone_exit_pending) {
        return;
    }
    if (s_state.wlan_notice_pending) {
        (void)settings_wlan_show_pending_notice(ui);
    } else {
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, "Waiting for phone",
            "Submit Wi-Fi settings in the portal",
            SETTINGS_NOTICE_DURATION_MS);
    }
}

static void settings_wlan_handle_forget(esp_gsp_handle_t ui)
{
    if (s_state.wlan_forget_pending) {
        return;
    }
    uint16_t page = 0;
    if (esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK ||
            page != SETTINGS_WLAN_DETAIL_PAGE) {
        return;
    }
    /* A hardware release can enqueue the same click again before the
     * asynchronous StackView pop has changed its reported top. Latch the
     * navigation before calling the provider so exactly one pop is queued. */
    s_state.wlan_forget_pending = true;
    if (settings_wifi_backend_available() &&
            settings_wifi_forget() != ESP_OK) {
        ESP_LOGW(TAG, "forget WLAN failed");
        s_state.wlan_forget_pending = false;
        return;
    }
    s_state.wlan.connected = false;
    s_state.wlan.current_ssid[0] = '\0';
    s_state.wlan.selected_ssid[0] = '\0';
    s_state.device.network.connected = false;
    s_state.device.network.ssid[0] = '\0';
    (void)settings_wlan_render(ui);
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, true) != ESP_GSP_OK) {
        s_state.wlan_forget_pending = false;
    }
}

static esp_err_t settings_render_snapshot(esp_gsp_handle_t ui)
{
    const settings_device_t *device = &s_state.device;
    char text[48];
    esp_err_t result = ESP_OK;
    s_state.wlan.enabled = device->network.desired_enabled;
    const bool stale_connect_snapshot = s_state.wlan_connect_pending &&
        device->network.operation_id ==
            s_state.wlan_connect_operation_id;
    if (!stale_connect_snapshot) {
        s_state.wlan.connected = device->network.connected;
        s_state.wlan.state = device->network.state;
        strlcpy(s_state.wlan.current_ssid, device->network.ssid,
                sizeof(s_state.wlan.current_ssid));
    }
    if (!s_state.brightness_drag_active) {
        s_state.brightness = device->display.brightness;
    }
    if (!s_state.volume_drag_active) {
        s_state.volume = device->audio.volume;
    }
    settings_display_request_render();

    keep_first_error(&result,
        settings_wlan_render(ui));

    (void)snprintf(text, sizeof(text), "v%u.%u.%u",
                   GSP_VERSION_MAJOR, GSP_VERSION_MINOR, GSP_VERSION_PATCH);
    keep_first_error(&result, esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_ABOUT_GSP_VERSION, text));

    return result;
}

static void settings_refresh_snapshot(esp_gsp_handle_t ui)
{
    /* The aggregate is a couple of kilobytes because of the agent config
     * strings, so it is collected on the heap and published in one step. */
    settings_device_t *device = calloc(1, sizeof(*device));
    if (device == NULL) {
        ESP_LOGE(TAG, "allocate Settings device record failed");
        return;
    }
    static const struct {
        const char *name;
        size_t offset;
        size_t size;
        /* An unregistered domain leaves its record zeroed rather than
         * failing the whole refresh. */
        bool required;
    } domains[] = {
        { "system.display", offsetof(settings_device_t, display),
          sizeof(mosaic_cap_display_t), true },
        { "system.audio", offsetof(settings_device_t, audio),
          sizeof(mosaic_cap_audio_t), true },
        { "system.haptic", offsetof(settings_device_t, haptic),
          sizeof(mosaic_cap_haptic_t), true },
        { "system.power", offsetof(settings_device_t, power),
          sizeof(mosaic_cap_power_t), false },
        { "system.update", offsetof(settings_device_t, update),
          sizeof(mosaic_cap_update_t), false },
        { "system.lifecycle", offsetof(settings_device_t, lifecycle),
          sizeof(mosaic_cap_lifecycle_t), true },
        { "net.wifi", offsetof(settings_device_t, network),
          sizeof(mosaic_cap_wifi_t), false },
        { "config.agent", offsetof(settings_device_t, agent),
          sizeof(mosaic_cap_agent_config_t), true },
    };
    esp_err_t err = ESP_OK;
    for (size_t index = 0;
            index < sizeof(domains) / sizeof(domains[0]); ++index) {
        const esp_err_t read_err = mosaic_capability_read(
            domains[index].name, SETTINGS_CAPABILITIES,
            (uint8_t *)device + domains[index].offset, domains[index].size);
        if (read_err != ESP_OK && domains[index].required) {
            keep_first_error(&err, read_err);
        }
    }
    if (err == ESP_OK) {
        s_state.device = *device;
        err = settings_render_snapshot(ui);
    }
    free(device);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh Settings device state failed: %s",
                 esp_err_to_name(err));
    }
}

static bool settings_detail_visible(
    esp_gsp_handle_t ui, settings_detail_kind_t kind)
{
    uint16_t top = 0U;
    return s_detail_kind == kind &&
        esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &top) == ESP_GSP_OK &&
        top == SETTINGS_DETAIL_PAGE;
}

static void settings_refresh_about_detail(esp_gsp_handle_t ui)
{
    if (settings_detail_visible(ui, SETTINGS_DETAIL_ABOUT) &&
            s_detail_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_refresh(ui, s_detail_list);
    }
}

static bool settings_update_page_visible(esp_gsp_handle_t ui)
{
    uint16_t top = 0U;
    return esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &top) == ESP_GSP_OK &&
        top == SETTINGS_UPDATE_PAGE;
}

static size_t settings_utf8_prefix_bytes(const char *text,
                                         size_t max_codepoints)
{
    size_t offset = 0U;
    size_t count = 0U;
    while (text[offset] != '\0' && count < max_codepoints) {
        const unsigned char lead = (unsigned char)text[offset];
        size_t width = 1U;
        if ((lead & 0xE0U) == 0xC0U) {
            width = 2U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            width = 3U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            width = 4U;
        }
        for (size_t i = 1U; i < width; ++i) {
            if (text[offset + i] == '\0' ||
                    ((unsigned char)text[offset + i] & 0xC0U) != 0x80U) {
                width = 1U;
                break;
            }
        }
        offset += width;
        ++count;
    }
    return offset;
}

static void settings_update_note_append_text(const char *text, bool title)
{
    if (text == NULL) {
        return;
    }

    const char *cursor = text;
    while (*cursor != '\0' &&
            s_update_note_count < SETTINGS_UPDATE_NOTE_MAX_ROWS) {
        while (*cursor == ' ' || *cursor == '\n' || *cursor == '\r') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        size_t cut = settings_utf8_prefix_bytes(
            cursor, SETTINGS_UPDATE_NOTE_CODEPOINTS);
        const char *newline = strchr(cursor, '\n');
        if (newline != NULL && (size_t)(newline - cursor) < cut) {
            cut = (size_t)(newline - cursor);
        } else if (cursor[cut] != '\0') {
            size_t word_cut = cut;
            while (word_cut > cut / 2U && cursor[word_cut] != ' ') {
                --word_cut;
            }
            if (cursor[word_cut] == ' ') {
                cut = word_cut;
            }
        }

        if (cut == 0U) {
            ++cursor;
            continue;
        }

        settings_update_note_line_t *line =
            &s_update_note_lines[s_update_note_count];
        const size_t copy_len = cut < sizeof(line->text) - 1U
            ? cut : sizeof(line->text) - 1U;
        memcpy(line->text, cursor, copy_len);
        line->text[copy_len] = '\0';
        size_t length = strlen(line->text);
        while (length > 0U && line->text[length - 1U] == ' ') {
            line->text[--length] = '\0';
        }
        line->title = title;
        ++s_update_note_count;

        cursor += cut;
    }
}

static gsp_err_t settings_update_note_bind_item(
    esp_gsp_handle_t ui, esp_gsp_row_t row, uint32_t item_index,
    void *user_ctx)
{
    (void)user_ctx;
    if (item_index >= s_update_note_count) {
        return GSP_ERR_INVALID_ARG;
    }

    const settings_update_note_line_t *line =
        &s_update_note_lines[item_index];
    esp_gsp_err_t err =
        gsp_settings_settings_update_note_row_row_set_text_text(
            ui, row, line->text);
    if (err == ESP_GSP_OK) {
        err = gsp_settings_settings_update_note_row_row_set_text_fg_color(
            ui, row, line->title ? SETTINGS_TEXT_COLOR
                                 : SETTINGS_MUTED_COLOR);
    }
    return err == ESP_GSP_OK ? GSP_OK : GSP_ERR_INVALID_STATE;
}

static void settings_update_notes_attach(esp_gsp_handle_t ui)
{
    if (s_update_notes_list == ESP_GSP_LIST_NONE) {
        s_update_notes_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_SETTINGS_UPDATE_NOTES_LIST,
            settings_update_note_bind_item, NULL);
    }
}

static void settings_update_notes_render(esp_gsp_handle_t ui,
                                         const char *title,
                                         const char *summary)
{
    title = title != NULL ? title : "";
    summary = summary != NULL ? summary : "";
    settings_update_notes_attach(ui);
    if (s_update_notes_list == ESP_GSP_LIST_NONE ||
            (strcmp(title, s_rendered_update_title) == 0 &&
             strcmp(summary, s_rendered_update_summary) == 0)) {
        return;
    }

    strlcpy(s_rendered_update_title, title,
            sizeof(s_rendered_update_title));
    strlcpy(s_rendered_update_summary, summary,
            sizeof(s_rendered_update_summary));
    s_update_note_count = 0U;
    settings_update_note_append_text(title, true);
    settings_update_note_append_text(summary, false);
    if (s_update_note_count == 0U) {
        settings_update_note_append_text("No release notes available", false);
    }
    (void)esp_gsp_list_set_total(
        ui, s_update_notes_list, s_update_note_count);
    (void)esp_gsp_list_refresh(ui, s_update_notes_list);
    (void)esp_gsp_list_scroll_to(ui, s_update_notes_list, 0);
}

static void settings_update_notes_park(esp_gsp_handle_t ui)
{
    if (s_update_notes_list != ESP_GSP_LIST_NONE) {
        (void)esp_gsp_list_set_total(ui, s_update_notes_list, 0U);
        (void)esp_gsp_list_refresh(ui, s_update_notes_list);
    }
    s_update_note_count = 0U;
    s_rendered_update_title[0] = '\0';
    s_rendered_update_summary[0] = '\0';
}

static void settings_render_update_page(esp_gsp_handle_t ui)
{
    const mosaic_cap_update_t *update = &s_state.device.update;
    const char *icon = "i";
    const char *status = "Ready to Check";
    const char *status_detail = "Compare with the published release";
    const char *status_detail_2 = "";
    const char *latest = "Not checked";
    const char *heading = "ABOUT UPDATE CHECKS";
    const char *title = "Release information only";
    const char *summary =
        "Checks the latest published software version. No files are installed.";
    const char *action = "Check Again";
    bool published_visible = false;
    switch (update->state) {
    case MOSAIC_CAP_UPDATE_CHECKING:
        icon = "…";
        status = "Checking for Updates";
        status_detail = "Contacting the update server";
        latest = "Checking...";
        heading = "PLEASE WAIT";
        title = "Fetching release information";
        summary = "The device is reading the published version and release notes.";
        action = "Checking...";
        break;
    case MOSAIC_CAP_UPDATE_AVAILABLE:
        icon = "^";
        status = "Update Available";
        status_detail = "Visit https://mosaico.espressif.com/";
        status_detail_2 = "for updates";
        latest = update->latest_version[0] != '\0'
            ? update->latest_version : "Available";
        heading = "WHAT'S NEW";
        title = update->title[0] != '\0'
            ? update->title : "A newer version is available";
        summary = update->summary[0] != '\0'
            ? update->summary : "Review the published release information.";
        published_visible = update->published_at[0] != '\0';
        break;
    case MOSAIC_CAP_UPDATE_UP_TO_DATE:
        icon = "OK";
        status = "Up to Date";
        status_detail = "This device matches the published release";
        latest = update->latest_version[0] != '\0'
            ? update->latest_version : s_state.device.lifecycle.software_version;
        heading = "LATEST RELEASE";
        title = update->title[0] != '\0'
            ? update->title : "Your software is current";
        summary = update->summary[0] != '\0'
            ? update->summary : "No newer published version was found.";
        published_visible = update->published_at[0] != '\0';
        break;
    case MOSAIC_CAP_UPDATE_DEVICE_AHEAD:
        icon = "^";
        status = "Development Version";
        status_detail = "This device is newer than the published release";
        latest = update->latest_version[0] != '\0'
            ? update->latest_version : "Unavailable";
        heading = "PUBLISHED RELEASE";
        title = "Device version is newer";
        summary = update->summary[0] != '\0'
            ? update->summary : "This device is ahead of the published release.";
        published_visible = update->published_at[0] != '\0';
        break;
    case MOSAIC_CAP_UPDATE_FAILED:
        icon = "!";
        status = "Check Failed";
        status_detail = "The update manifest could not be loaded";
        latest = "Unavailable";
        heading = "NEXT STEP";
        title = update->title[0] != '\0' ? update->title
            : "Check the network and update source";
        summary = update->summary[0] != '\0' ? update->summary
            : "Confirm the connection and manifest URL, then try again.";
        break;
    case MOSAIC_CAP_UPDATE_IDLE:
    default:
        break;
    }

    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_BACK, "‹");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_NAV_TITLE,
                           "Software Update");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_CURRENT_LABEL,
                           "CURRENT");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_LATEST_LABEL,
                           "PUBLISHED");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_ARROW, ">");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_ICON, icon);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS, status);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_DETAIL,
                           status_detail);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_DETAIL_2,
                           status_detail_2);
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_UPDATE_CURRENT_VERSION,
        s_state.device.lifecycle.software_version[0] != '\0'
            ? s_state.device.lifecycle.software_version : "--");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_LATEST_VERSION,
                           latest);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_HEADING, heading);
    settings_update_notes_render(ui, title, summary);
    char published_at[MOSAIC_CAP_UPDATE_PUBLISHED_AT_LEN];
    strlcpy(published_at, update->published_at, sizeof(published_at));
    char *time_separator = strchr(published_at, 'T');
    if (time_separator != NULL) {
        *time_separator = '\0';
    }
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_UPDATE_PUBLISHED_AT,
        published_at[0] != '\0' ? published_at : "");
    (void)esp_gsp_set_visible(ui, GSP_BIND_SETTINGS_UPDATE_PUBLISHED_VISIBLE,
                              published_visible);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_DISCLAIMER,
                           "Information only · No automatic installation");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_ACTION, action);
    /* Text updates clear their previous bounds. Re-submit the retained card
     * fills so the compositor redraws each complete card behind new text. */
    (void)esp_gsp_set_color(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_CARD_COLOR,
                            SETTINGS_LEVEL_OFF_COLOR);
    (void)esp_gsp_set_color(ui, GSP_BIND_SETTINGS_UPDATE_ACTION_CARD_COLOR,
                            SETTINGS_LEVEL_OFF_COLOR);
}

static void settings_render_update_error(esp_gsp_handle_t ui,
                                         const char *status,
                                         const char *detail,
                                         const char *title,
                                         const char *message)
{
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_BACK, "‹");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_NAV_TITLE,
                           "Software Update");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_CURRENT_LABEL,
                           "CURRENT");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_LATEST_LABEL,
                           "PUBLISHED");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_ARROW, ">");
    (void)esp_gsp_set_text(
        ui, GSP_BIND_SETTINGS_UPDATE_CURRENT_VERSION,
        s_state.device.lifecycle.software_version[0] != '\0'
            ? s_state.device.lifecycle.software_version : "--");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_ICON, "!");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS, status);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_DETAIL, detail);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_DETAIL_2, "");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_LATEST_VERSION,
                           "Unavailable");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_HEADING, "NEXT STEP");
    settings_update_notes_render(ui, title, message);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_PUBLISHED_AT, "");
    (void)esp_gsp_set_visible(ui, GSP_BIND_SETTINGS_UPDATE_PUBLISHED_VISIBLE,
                              false);
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_DISCLAIMER,
                           "Information only · No automatic installation");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_UPDATE_ACTION, "Try Again");
    (void)esp_gsp_set_color(ui, GSP_BIND_SETTINGS_UPDATE_STATUS_CARD_COLOR,
                            SETTINGS_LEVEL_OFF_COLOR);
    (void)esp_gsp_set_color(ui, GSP_BIND_SETTINGS_UPDATE_ACTION_CARD_COLOR,
                            SETTINGS_LEVEL_OFF_COLOR);
}

static void settings_start_update_check(esp_gsp_handle_t ui)
{
    if (s_state.device.update.state == MOSAIC_CAP_UPDATE_CHECKING) {
        settings_render_update_page(ui);
        return;
    }
    if (!s_state.device.network.connected) {
        settings_render_update_error(
            ui, "Unable to Check", "No internet connection",
            "Connect to Wi-Fi",
            "A network connection is required to check the published version.");
        return;
    }
    const esp_err_t err = mosaic_capability_invoke("system.update",
        SETTINGS_CAPABILITIES, "check", NULL, 0, NULL, 0);
    if (err != ESP_OK) {
        settings_render_update_error(
            ui, "Check Unavailable", "Update source is not configured",
            "Configure the update source",
            err == ESP_ERR_NOT_SUPPORTED
                ? "Configure APP_UPDATE_MANIFEST_URL first."
                : "Unable to start the update check.");
        return;
    }
    s_state.device.update.state = MOSAIC_CAP_UPDATE_CHECKING;
    settings_refresh_about_detail(ui);
    settings_render_update_page(ui);
}

static void settings_open_update_page(esp_gsp_handle_t ui)
{
    if (s_state.update_page_active) {
        return;
    }
    s_state.update_page_active = true;
    s_state.update_leave_pending = false;
    if (esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_SETTINGS_STACK,
            SETTINGS_UPDATE_PAGE, true) != ESP_GSP_OK) {
        s_state.update_page_active = false;
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, "Software Update", "Unable to open page",
            SETTINGS_NOTICE_DURATION_MS);
        return;
    }
    mosaic_app_shell_set_root_visible(ui, false);
    settings_render_update_page(ui);
    settings_start_update_check(ui);
}

static void settings_leave_update_page(esp_gsp_handle_t ui)
{
    if (!s_state.update_page_active || s_state.update_leave_pending ||
            !settings_update_page_visible(ui)) {
        return;
    }
    s_state.update_leave_pending = true;
    if (esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, true) != ESP_GSP_OK) {
        s_state.update_leave_pending = false;
        return;
    }
    s_state.update_page_active = false;
    s_state.update_leave_pending = false;
    settings_update_notes_park(ui);
    /* The destination is the About detail page, not the Settings root. */
    mosaic_app_shell_set_root_visible(ui, false);
}

static void settings_show_update_result(esp_gsp_handle_t ui)
{
    const mosaic_cap_update_t *update = &s_state.device.update;
    if (update->sequence == 0U ||
            update->sequence == s_state.update_notice_sequence ||
            update->state == MOSAIC_CAP_UPDATE_IDLE ||
            update->state == MOSAIC_CAP_UPDATE_CHECKING) {
        return;
    }
    s_state.update_notice_sequence = update->sequence;
    if (s_state.update_page_active) {
        return;
    }
    char title[96];
    char message[256];
    switch (update->state) {
    case MOSAIC_CAP_UPDATE_AVAILABLE:
        if (update->title[0] != '\0') {
            strlcpy(title, update->title, sizeof(title));
        } else {
            (void)snprintf(title, sizeof(title), "Version %s available",
                           update->latest_version);
        }
        if (update->summary[0] != '\0') {
            strlcpy(message, update->summary, sizeof(message));
        } else {
            (void)snprintf(message, sizeof(message), "Current %s · Latest %s",
                           s_state.device.lifecycle.software_version,
                           update->latest_version);
        }
        break;
    case MOSAIC_CAP_UPDATE_UP_TO_DATE:
        strlcpy(title, "Up to Date", sizeof(title));
        (void)snprintf(message, sizeof(message), "Version %s is current",
                       s_state.device.lifecycle.software_version);
        break;
    case MOSAIC_CAP_UPDATE_DEVICE_AHEAD:
        strlcpy(title, "Development Version", sizeof(title));
        (void)snprintf(message, sizeof(message), "Device %s · Published %s",
                       s_state.device.lifecycle.software_version,
                       update->latest_version);
        break;
    case MOSAIC_CAP_UPDATE_FAILED:
    default:
        strlcpy(title, "Check Failed", sizeof(title));
        strlcpy(message, "Check the network and manifest, then try again",
                sizeof(message));
        break;
    }
    (void)mosaic_top_notice_show(
        ui, &s_top_notice, title, message,
        SETTINGS_UPDATE_NOTICE_DURATION_MS);
}

static void settings_refresh_battery_detail(esp_gsp_handle_t ui)
{
    if (s_detail_list == ESP_GSP_LIST_NONE) {
        return;
    }
    mosaic_cap_power_t battery = {0};
    esp_err_t err = mosaic_capability_read("system.power",
        SETTINGS_CAPABILITIES, &battery, sizeof(battery));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh Battery detail failed: %s",
                 esp_err_to_name(err));
        return;
    }
    s_state.device.power = battery;
    esp_gsp_err_t gsp_err = esp_gsp_list_refresh(
        ui, s_detail_list);
    if (gsp_err != ESP_GSP_OK) {
        ESP_LOGW(TAG, "queue Battery list refresh failed: 0x%x",
                 (unsigned)gsp_err);
    }
}

static const uint16_t s_integration_llm_progress[] = {
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_01,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_02,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_03,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_04,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_05,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_06,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_07,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_08,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_09,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_10,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_11,
    GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_BAR_12,
};

static void settings_integration_progress(
    esp_gsp_handle_t ui, const uint16_t *binds, uint8_t count)
{
    if (count > SETTINGS_INTEGRATION_PROGRESS_COUNT) {
        count = SETTINGS_INTEGRATION_PROGRESS_COUNT;
    }
    for (uint8_t i = 0; i < SETTINGS_INTEGRATION_PROGRESS_COUNT; ++i) {
        (void)esp_gsp_set_color(ui, binds[i],
            i < count ? SETTINGS_INTEGRATION_PROGRESS_ON
                      : SETTINGS_INTEGRATION_PROGRESS_OFF);
    }
}

static void *settings_qr_alloc(size_t bytes)
{
#if defined(ESP_PLATFORM)
    void *memory = heap_caps_malloc(bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return malloc(bytes);
#endif
}

static bool settings_qr_prepare(void)
{
    if (s_state.qr_temp == NULL) {
        s_state.qr_temp = settings_qr_alloc(qrcodegen_BUFFER_LEN_MAX);
    }
    if (s_state.qr_code == NULL) {
        s_state.qr_code = settings_qr_alloc(qrcodegen_BUFFER_LEN_MAX);
    }
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        if (s_state.qr_frames[i].pixels == NULL) {
            s_state.qr_frames[i].pixels = settings_qr_alloc(
                SETTINGS_INTEGRATION_QR_BYTES);
            atomic_store_explicit(&s_state.qr_frames[i].busy, false,
                                  memory_order_release);
        }
    }
    return s_state.qr_temp != NULL && s_state.qr_code != NULL &&
        s_state.qr_frames[0].pixels != NULL &&
        s_state.qr_frames[1].pixels != NULL;
}

static void settings_qr_release(void *user_ctx)
{
    settings_qr_frame_t *frame = user_ctx;
    if (frame != NULL) {
        atomic_store_explicit(&frame->busy, false, memory_order_release);
    }
}

static settings_qr_frame_t *settings_qr_acquire(void)
{
    for (size_t i = 0; i < SETTINGS_INTEGRATION_QR_FRAMES; ++i) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &s_state.qr_frames[i].busy, &expected, true,
                memory_order_acq_rel, memory_order_acquire)) {
            return &s_state.qr_frames[i];
        }
    }
    return NULL;
}

static bool settings_qr_render(
    const char *payload, uint8_t *pixels, uint16_t size)
{
    if (payload == NULL || payload[0] == '\0' ||
            !qrcodegen_encodeText(payload, s_state.qr_temp, s_state.qr_code,
                qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return false;
    }
    const int modules = qrcodegen_getSize(s_state.qr_code);
    const int extent = modules + SETTINGS_INTEGRATION_QR_QUIET * 2;
    const int scale = size / extent;
    if (scale < 1) {
        return false;
    }
    const int offset = (size - extent * scale) / 2;
    uint16_t *frame = (uint16_t *)pixels;
    for (size_t i = 0; i < (size_t)size * size; ++i) {
        frame[i] = UINT16_MAX;
    }
    for (int y = 0; y < modules; ++y) {
        for (int x = 0; x < modules; ++x) {
            if (!qrcodegen_getModule(s_state.qr_code, x, y)) continue;
            const int px = offset + (x + SETTINGS_INTEGRATION_QR_QUIET) * scale;
            const int py = offset + (y + SETTINGS_INTEGRATION_QR_QUIET) * scale;
            for (int dy = 0; dy < scale; ++dy) {
                uint16_t *row = frame +
                    (size_t)(py + dy) * size;
                for (int dx = 0; dx < scale; ++dx) row[px + dx] = 0;
            }
        }
    }
    return true;
}

static void settings_qr_push(
    esp_gsp_handle_t ui, uint16_t bind, uint16_t size, const char *payload,
    char *rendered, size_t rendered_size)
{
    if (payload == NULL || payload[0] == '\0' ||
            strcmp(payload, rendered) == 0 || !settings_qr_prepare()) return;
    settings_qr_frame_t *frame = settings_qr_acquire();
    if (frame == NULL || !settings_qr_render(payload, frame->pixels, size)) {
        settings_qr_release(frame);
        return;
    }
    if (esp_gsp_canvas_push(ui, bind, frame->pixels, size * 2U,
            settings_qr_release, frame) != ESP_GSP_OK) {
        settings_qr_release(frame);
        return;
    }
    strlcpy(rendered, payload, rendered_size);
}

static void settings_wlan_phone_refresh(esp_gsp_handle_t ui)
{
    mosaic_cap_provisioning_t *provisioning = calloc(1, sizeof(*provisioning));
    if (provisioning == NULL) {
        return;
    }
    esp_err_t err = mosaic_capability_read("net.provisioning",
        SETTINGS_CAPABILITIES, provisioning, sizeof(*provisioning));
    if (err == ESP_OK) {
        (void)esp_gsp_set_text(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_AP_SSID, provisioning->ap_ssid);
        settings_qr_push(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_QR_CANVAS,
            SETTINGS_WLAN_PHONE_QR_SIZE, provisioning->qr_payload,
            s_state.rendered_phone_qr,
            sizeof(s_state.rendered_phone_qr));
    } else {
        (void)esp_gsp_set_text(
            ui, GSP_BIND_SETTINGS_WLAN_PHONE_AP_SSID,
            "Provisioning AP unavailable");
        ESP_LOGW(TAG, "read phone provisioning QR failed: %s",
                 esp_err_to_name(err));
    }
    free(provisioning);
}

static void settings_llm_render(esp_gsp_handle_t ui)
{
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_STATUS_VISIBLE,
        s_state.llm_phase == SETTINGS_LLM_STATUS);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIGURING_VISIBLE,
        s_state.llm_phase == SETTINGS_LLM_CONFIGURING);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_PROGRESS_VISIBLE,
        s_state.llm_phase == SETTINGS_LLM_PROGRESS);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_SUCCESS_VISIBLE,
        false);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_STATUS_BACK_VISIBLE, true);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_STATUS_SKIP_VISIBLE, false);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIGURING_BACK_VISIBLE, true);
    (void)esp_gsp_set_visible(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIGURING_SKIP_VISIBLE, false);
}

static void settings_build_configuration_url(
    const settings_device_t *device, const char *fragment,
    char *url, size_t url_size)
{
    if (device == NULL || fragment == NULL || url == NULL ||
            url_size == 0U) {
        return;
    }
    if (device->network.connected && device->network.ip[0] != '\0') {
        (void)snprintf(url, url_size, "http://%s/#%s",
                       device->network.ip, fragment);
        return;
    }

    const char *portal = device->network.portal_url[0] != '\0'
        ? device->network.portal_url : "http://192.168.4.1/";
    const size_t portal_len = strlen(portal);
    (void)snprintf(url, url_size, "%s%s#%s", portal,
                   portal_len > 0U && portal[portal_len - 1U] == '/' ? "" : "/",
                   fragment);
}

static void settings_build_connection_hint(
    const settings_device_t *device, char *hint, size_t hint_size)
{
    if (device == NULL || hint == NULL || hint_size == 0U) {
        return;
    }
    if (device->network.connected && device->network.ssid[0] != '\0') {
        (void)snprintf(hint, hint_size, "STA: %s",
                       device->network.ssid);
        return;
    }
    if (s_state.provisioning_ap_ssid[0] == '\0') {
        mosaic_cap_provisioning_t *provisioning =
            calloc(1, sizeof(*provisioning));
        if (provisioning != NULL) {
            if (mosaic_capability_read("net.provisioning",
                    SETTINGS_CAPABILITIES, provisioning,
                    sizeof(*provisioning)) == ESP_OK) {
                strlcpy(s_state.provisioning_ap_ssid, provisioning->ap_ssid,
                        sizeof(s_state.provisioning_ap_ssid));
            }
            free(provisioning);
        }
    }
    (void)snprintf(hint, hint_size, "AP: %s",
                   s_state.provisioning_ap_ssid[0] != '\0'
                       ? s_state.provisioning_ap_ssid : "unavailable");
}

static void settings_llm_refresh(esp_gsp_handle_t ui)
{
    settings_refresh_snapshot(ui);
    const settings_device_t *device = &s_state.device;
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_BACKEND,
        device->agent.llm_backend[0] ? device->agent.llm_backend : "Not configured");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_INTEGRATIONS_LLM_MODEL,
        device->agent.llm_model[0] ? device->agent.llm_model : "Not configured");
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_INTEGRATIONS_LLM_BASE_URL,
        device->agent.llm_base_url[0] ? device->agent.llm_base_url : "Not configured");
    char capabilities[48];
    (void)snprintf(capabilities, sizeof(capabilities), "Tools %s · Vision %s",
        device->agent.llm_supports_tools ? "On" : "Off",
        device->agent.llm_supports_vision ? "On" : "Off");
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CAPABILITIES, capabilities);
    char url[MOSAIC_CAP_LLM_URL_LEN] = {0};
    char connection_hint[MOSAIC_CAP_SSID_LEN + 32U] = {0};
    settings_build_connection_hint(
        device, connection_hint, sizeof(connection_hint));
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONNECTION_HINT,
        connection_hint);
    settings_build_configuration_url(device, "llm", url, sizeof(url));
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIG_URL,
        url[0] ? url : "Unavailable");
    settings_qr_push(ui,
        GSP_BIND_SETTINGS_INTEGRATIONS_LLM_CONFIG_QR_CANVAS,
        SETTINGS_QR_SIZE, url,
        s_state.rendered_llm_qr, sizeof(s_state.rendered_llm_qr));
    /* Settings owns an information page, not Setup's completion ceremony.
     * A successful refresh returns directly to the retained status list so
     * the newly configured backend/model fields become visible immediately. */
    s_state.llm_phase = SETTINGS_LLM_STATUS;
    settings_llm_render(ui);
}

static void settings_channels_refresh(esp_gsp_handle_t ui)
{
    settings_refresh_snapshot(ui);
    const settings_device_t *device = &s_state.device;
    const bool configured[] = {
        device->agent.im_wechat_configured,
        device->agent.im_qq_configured,
        device->agent.im_feishu_configured,
        device->agent.im_telegram_configured,
    };
    const uint16_t on_binds[] = {
        GSP_BIND_SETTINGS_CHANNELS_WECHAT_ON_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_QQ_ON_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_FEISHU_ON_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_TELEGRAM_ON_VISIBLE,
    };
    const uint16_t off_binds[] = {
        GSP_BIND_SETTINGS_CHANNELS_WECHAT_OFF_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_QQ_OFF_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_FEISHU_OFF_VISIBLE,
        GSP_BIND_SETTINGS_CHANNELS_TELEGRAM_OFF_VISIBLE,
    };
    for (size_t i = 0; i < sizeof(configured) / sizeof(configured[0]); ++i) {
        (void)esp_gsp_set_visible(ui, on_binds[i], configured[i]);
        (void)esp_gsp_set_visible(ui, off_binds[i], !configured[i]);
    }

    char url[MOSAIC_CAP_LLM_URL_LEN] = {0};
    char connection_hint[MOSAIC_CAP_SSID_LEN + 32U] = {0};
    settings_build_connection_hint(
        device, connection_hint, sizeof(connection_hint));
    (void)esp_gsp_set_text(ui,
        GSP_BIND_SETTINGS_CHANNELS_CONNECTION_HINT, connection_hint);
    settings_build_configuration_url(device, "im", url, sizeof(url));
    (void)esp_gsp_set_text(ui, GSP_BIND_SETTINGS_CHANNELS_CONFIG_URL,
                           url[0] ? url : "Unavailable");
    settings_qr_push(ui, GSP_BIND_SETTINGS_CHANNELS_CONFIG_QR_CANVAS,
                     SETTINGS_CHANNEL_QR_SIZE, url,
                     s_state.rendered_channel_qr,
                     sizeof(s_state.rendered_channel_qr));
}

static void settings_integrations_pop(esp_gsp_handle_t ui)
{
    settings_refresh_snapshot(ui);
    settings_root_list_refresh(ui);
    (void)esp_gsp_stack_view_pop(ui, GSP_OBJ_KEY_SETTINGS_STACK, true);
    mosaic_app_shell_set_root_visible(ui, true);
    s_state.integration_page = 0;
}

static void settings_open_integration(esp_gsp_handle_t ui, bool llm)
{
    const uint16_t page = llm
        ? SETTINGS_INTEGRATIONS_MODEL_PAGE
        : SETTINGS_INTEGRATIONS_CHANNELS_PAGE;
    if (esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_SETTINGS_STACK,
            page, true) != ESP_GSP_OK) {
        (void)mosaic_top_notice_show(
            ui, &s_top_notice, "AI & Integrations",
            "Unable to open page", SETTINGS_NOTICE_DURATION_MS);
        return;
    }
    mosaic_app_shell_set_root_visible(ui, false);
    s_state.integration_page = page;
    if (llm) {
        s_state.rendered_llm_qr[0] = '\0';
        settings_llm_refresh(ui);
    } else {
        s_state.rendered_channel_qr[0] = '\0';
        settings_channels_refresh(ui);
    }
}

static void settings_dispatch_call(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_SETTINGS_ROOT_ROW: {
        const uint32_t item = event->data.call.item;
        if (item >= sizeof(s_root_rows) / sizeof(s_root_rows[0])) {
            return;
        }
        if (item <= 1U) {
            settings_open_integration(ui, item == 0U);
            return;
        }
        if (item == 2U) {
            if (s_state.wlan_page_active) {
                return;
            }
            s_state.wlan_page_active = true;
            s_state.wlan_entry_pending = true;
            s_state.wlan_leave_pending = false;
            s_state.wlan_detail_active = false;
            s_state.wlan_detail_leave_pending = false;
            esp_gsp_err_t err = esp_gsp_stack_view_push(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, SETTINGS_WLAN_PAGE, true);
            if (err != ESP_GSP_OK) {
                err = esp_gsp_stack_view_push(
                    ui, GSP_OBJ_KEY_SETTINGS_STACK, SETTINGS_WLAN_PAGE, false);
            }
            if (err != ESP_GSP_OK) {
                s_state.wlan_page_active = false;
                s_state.wlan_entry_pending = false;
                ESP_LOGW(TAG, "open WLAN page failed: 0x%x",
                         (unsigned)err);
                return;
            }
            mosaic_app_shell_set_root_visible(ui, false);
            (void)settings_wlan_render(ui);
            s_state.wlan_scan_next_us = event->timestamp_us;
            settings_wlan_request_scan();
            settings_wlan_scan(ui);
            settings_wlan_list_attach(ui);
            return;
        }
        if (item == 6U) {
            settings_refresh_snapshot(ui);
            settings_open_detail(ui, SETTINGS_DETAIL_BATTERY);
            return;
        }
        if (item == 7U) {
            settings_refresh_snapshot(ui);
            settings_open_detail(ui, SETTINGS_DETAIL_ABOUT);
            s_state.update_notice_sequence =
                s_state.device.update.sequence;
            return;
        }
        static const uint16_t pages[] = {
            [3] = SETTINGS_DISPLAY_PAGE,
            [4] = SETTINGS_SOUND_PAGE,
            [5] = SETTINGS_SECURITY_PAGE,
        };
        const uint16_t page = pages[item];
        if (page == 0U || esp_gsp_stack_view_push(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, page, true) != ESP_GSP_OK) {
            return;
        }
        mosaic_app_shell_set_root_visible(ui, false);
        if (item == 3U || item == 4U) {
            s_state.display_page_active = true;
            s_state.display_reset_pending = false;
            settings_display_request_render();
        }
        return;
    }
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_FLOW_BACK:
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_BACK:
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_CONTINUE:
        settings_integrations_pop(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_RECONFIGURE:
        s_state.llm_phase = SETTINGS_LLM_CONFIGURING;
        settings_llm_render(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_SUBMITTED:
        s_state.llm_phase = SETTINGS_LLM_PROGRESS;
        s_state.integration_started_us = event->timestamp_us;
        s_state.integration_deadline_us = event->timestamp_us +
            SETTINGS_INTEGRATION_CONFIG_US;
        s_state.integration_rendered_progress = UINT8_MAX;
        settings_integration_progress(ui, s_integration_llm_progress, 0);
        settings_llm_render(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_CANCEL:
        settings_llm_refresh(ui);
        return;
    case GSP_ACT_ID_SETTINGS_INTEGRATIONS_LLM_SKIP:
        settings_integrations_pop(ui);
        return;
    case GSP_ACT_ID_SETTINGS_ROTATION_SELECT: {
        const uint32_t index = event->data.call.list != ESP_GSP_LIST_NONE
            ? event->data.call.item : event->data.call.arg;
        if (index > 3U) {
            return;
        }
        const uint16_t degrees = (uint16_t)(index * 90U);
        const mosaic_cap_display_rotation_args_t rotation_args = {
            .degrees = degrees,
        };
        esp_err_t err = mosaic_capability_invoke("system.display",
            SETTINGS_CAPABILITIES, "set_rotation", &rotation_args,
            sizeof(rotation_args), NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set rotation failed: %s", esp_err_to_name(err));
            settings_refresh_snapshot(ui);
            return;
        }
        s_state.device.display.rotation_degrees = degrees;
        settings_display_request_render();
        return;
    }
    case GSP_ACT_ID_SETTINGS_SCREEN_TIMEOUT_SELECT: {
        static const uint32_t timeout_options_ms[] = {
            10000U, 30000U, 60000U, 120000U, 300000U, 0U,
        };
        const uint32_t index = event->data.call.list != ESP_GSP_LIST_NONE
            ? event->data.call.item : event->data.call.arg;
        if (index >= sizeof(timeout_options_ms) /
                     sizeof(timeout_options_ms[0])) {
            return;
        }
        const uint32_t timeout_ms = timeout_options_ms[index];
        const mosaic_cap_display_timeout_args_t timeout_args = {
            .timeout_ms = timeout_ms,
        };
        esp_err_t err = mosaic_capability_invoke("system.display",
            SETTINGS_CAPABILITIES, "set_screen_timeout", &timeout_args,
            sizeof(timeout_args), NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set screen timeout failed: %s", esp_err_to_name(err));
            settings_refresh_snapshot(ui);
            return;
        }
        s_state.device.display.screen_timeout_ms = timeout_ms;
        settings_display_request_render();
        return;
    }
    case GSP_ACT_ID_SETTINGS_WLAN_LEAVE:
        if (!s_state.wlan_page_active || s_state.wlan_leave_pending ||
                s_state.wlan_detail_active ||
                s_state.wlan_password_active) {
            return;
        }
        /* A visible WLAN back target proves the queued entry has landed.
         * Claim this pop before submitting it so duplicate releases cannot
         * consume another StackView depth. */
        s_state.wlan_entry_pending = false;
        s_state.wlan_leave_pending = true;
        if (esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, true) != ESP_GSP_OK) {
            s_state.wlan_leave_pending = false;
            return;
        }
        mosaic_app_shell_set_root_visible(ui, true);
        return;
    case GSP_ACT_ID_SETTINGS_DISPLAY_PAGE:
        s_state.display_page_active = event->data.call.arg != 0;
        mosaic_app_shell_set_root_visible(
            ui, !s_state.display_page_active);
        if (s_state.display_page_active) {
            s_state.display_reset_pending = false;
            settings_display_request_render();
        } else {
            /* Do not reset while the exiting page remains visible; the
             * periodic callback performs it after the StackView pop has
             * completed. */
            s_state.display_reset_pending = true;
            s_state.display_pointer_pressed = false;
            s_state.display_pointer_scrolled = false;
            s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
            s_state.brightness_drag_active = false;
            s_state.brightness_dirty = false;
            s_state.volume_drag_active = false;
            s_state.volume_dirty = false;
            settings_root_list_refresh(ui);
        }
        return;
    case GSP_ACT_ID_SETTINGS_DETAIL_ROW:
        if (s_detail_kind == SETTINGS_DETAIL_ABOUT &&
                event->data.call.item == 5U) {
            settings_open_update_page(ui);
            return;
        }
        if (s_detail_kind == SETTINGS_DETAIL_SAFE_MODE &&
                event->data.call.item == 3U) {
            (void)mosaic_top_notice_show(
                ui, &s_top_notice, "Safe Mode", "Restart required",
                SETTINGS_NOTICE_DURATION_MS);
        }
        return;
    case GSP_ACT_ID_SETTINGS_UPDATE_CHECK:
        if (s_state.update_page_active &&
                settings_update_page_visible(ui)) {
            settings_start_update_check(ui);
        }
        return;
    case GSP_ACT_ID_SETTINGS_UPDATE_LEAVE:
        settings_leave_update_page(ui);
        return;
    case GSP_ACT_ID_WLAN_TOGGLE:
        if (event->timestamp_us - s_state.wlan_toggle_last_us < 100000) {
            return;
        }
        s_state.wlan_toggle_last_us = event->timestamp_us;
        const bool enabled = !s_state.wlan.enabled;
        if (settings_wifi_set_enabled(enabled) != ESP_OK) {
            ESP_LOGW(TAG, "set WLAN enabled state failed");
            return;
        }
        /* Update presentation immediately; the provider remains
         * authoritative and reconciles this optimistic state later. */
        s_state.wlan.enabled = enabled;
        s_state.wlan.state = enabled
            ? MOSAIC_CAP_WIFI_IDLE : MOSAIC_CAP_WIFI_DISABLED;
        if (!enabled) {
            s_state.wlan_connect_pending = false;
            s_state.wlan_connect_deadline_us = 0;
            s_state.wlan_scan_waiting = false;
            s_state.wlan_scan_next_us = 0;
            s_state.wlan.connected = false;
            s_state.wlan.current_ssid[0] = '\0';
            settings_wlan_list_park(ui);
        } else {
            s_state.wlan_scan_next_us = event->timestamp_us;
            settings_wlan_request_scan();
            settings_wlan_list_attach(ui);
        }
        (void)settings_wlan_render(ui);
        return;
    case GSP_ACT_ID_WLAN_NETWORK_SELECT:
        settings_wlan_handle_network_select(ui, event);
        return;
    case GSP_ACT_ID_WLAN_CURRENT_NETWORK:
        settings_wlan_open_detail(ui);
        return;
    case GSP_ACT_ID_WLAN_CONNECT:
        if (s_state.wlan_password_active) {
            if (settings_wlan_handle_connect(ui)) {
                settings_wlan_pop_to_wlan(ui);
            }
        }
        return;
    case GSP_ACT_ID_SETTINGS_WLAN_PASSWORD_KEYBOARD_KEY:
        if (!s_state.wlan_password_active ||
                event->data.call.arg != 13U) {
            return;
        }
        if (settings_wlan_handle_connect(ui)) {
            settings_wlan_pop_to_wlan(ui);
        }
        return;
    case GSP_ACT_ID_WLAN_PASSWORD_DETACH:
        settings_wlan_cancel_password(ui);
        return;
    case GSP_ACT_ID_WLAN_PHONE_SETUP_OPEN:
        settings_wlan_password_detach(ui);
        /* The device may already be connected to another WLAN. Capture the
         * current transaction so that association cannot be mistaken for the
         * result of credentials submitted through this phone portal. */
        settings_wlan_refresh_status(ui);
        s_state.wlan_phone_active = true;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id =
            s_state.device.network.operation_id;
        s_state.rendered_phone_qr[0] = '\0';
        settings_wlan_phone_refresh(ui);
        return;
    case GSP_ACT_ID_WLAN_PHONE_SETUP_CLOSE:
        s_state.wlan_phone_active = false;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id = 0;
        settings_wlan_password_attach(ui);
        return;
    case GSP_ACT_ID_WLAN_PHONE_SUBMITTED:
        settings_wlan_handle_phone_submitted(ui);
        return;
    case GSP_ACT_ID_WLAN_LIST_RESUME:
        if (!s_state.wlan_detail_active ||
                s_state.wlan_detail_leave_pending ||
                s_state.wlan_forget_pending) {
            return;
        }
        s_state.wlan_detail_leave_pending = true;
        if (esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_SETTINGS_STACK, true) != ESP_GSP_OK) {
            s_state.wlan_detail_leave_pending = false;
        }
        return;
    case GSP_ACT_ID_WLAN_AUTO_JOIN_TOGGLE:
        s_state.wlan.auto_join = !s_state.wlan.auto_join;
        (void)settings_wlan_render(ui);
        return;
    case GSP_ACT_ID_WLAN_FORGET_NETWORK:
        settings_wlan_handle_forget(ui);
        return;
    default:
        return;
    }
}

static int32_t settings_level_from_x(int32_t x)
{
    if (x <= SETTINGS_TRACK_X_MIN) {
        return 0;
    }
    if (x >= SETTINGS_TRACK_X_MAX) {
        return 100;
    }
    const int32_t span = SETTINGS_TRACK_X_MAX - SETTINGS_TRACK_X_MIN;
    return ((x - SETTINGS_TRACK_X_MIN) * 100 + span / 2) / span;
}

static bool settings_display_page_is_active(esp_gsp_handle_t ui)
{
    uint16_t page = 0;
    return esp_gsp_stack_view_get_top(
               ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) == ESP_GSP_OK &&
           (page == SETTINGS_DISPLAY_PAGE || page == SETTINGS_SOUND_PAGE);
}

static bool settings_factory_dispatch_pointer(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    uint16_t page = 0;
    if (esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_SETTINGS_STACK, &page) != ESP_GSP_OK ||
            page != SETTINGS_FACTORY_PAGE) {
        return false;
    }
    if (!event->data.pointer.pressed) {
        if (s_state.factory_hold_active) {
            s_state.factory_hold_active = false;
            (void)gsp_settings_settings_factory_hold_progress_set_value(
                ui, 0);
        }
        return true;
    }
    if (s_state.factory_hold_active || s_state.factory_hold_completed) {
        return true;
    }
    const int32_t x = event->data.pointer.x;
    const int32_t y = event->data.pointer.y;
    if (x >= SETTINGS_FACTORY_HOLD_X_MIN &&
            x <= SETTINGS_FACTORY_HOLD_X_MAX &&
            y >= SETTINGS_FACTORY_HOLD_Y_MIN &&
            y <= SETTINGS_FACTORY_HOLD_Y_MAX) {
        s_state.factory_hold_active = true;
        s_state.factory_hold_started_us = event->timestamp_us > 0
            ? event->timestamp_us : esp_timer_get_time();
    }
    return true;
}

static void settings_dispatch_pointer(
    esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    if (settings_factory_dispatch_pointer(ui, event)) {
        return;
    }
    if (!event->data.pointer.pressed) {
        if (s_state.brightness_drag_active && s_state.brightness_dirty &&
                s_state.brightness_hw_available) {
            esp_err_t err = settings_set_brightness(
                s_state.brightness, true);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                s_state.brightness_hw_available = false;
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "persist brightness failed: %s",
                         esp_err_to_name(err));
                settings_refresh_snapshot(ui);
            }
        }
        if (s_state.volume_drag_active && s_state.volume_dirty) {
            esp_err_t err = settings_set_volume(s_state.volume, true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "persist volume failed: %s",
                         esp_err_to_name(err));
                settings_refresh_snapshot(ui);
            }
        }
        if (s_state.display_pointer_pressed &&
                !s_state.brightness_drag_active &&
                !s_state.volume_drag_active) {
            if (!s_state.display_pointer_scrolled) {
                if (s_state.display_tap ==
                           SETTINGS_DISPLAY_TAP_NOTIFICATION) {
                    s_state.sound_effect_enabled =
                        !s_state.sound_effect_enabled;
                    settings_display_request_render();
                }
            }
        }
        s_state.brightness_drag_active = false;
        s_state.brightness_dirty = false;
        s_state.volume_drag_active = false;
        s_state.volume_dirty = false;
        s_state.display_pointer_pressed = false;
        s_state.display_pointer_scrolled = false;
        s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
        return;
    }
    if (!s_state.brightness_drag_active && !s_state.volume_drag_active) {
        /* Only the initial press may claim a slider. A vertical scroll that
         * later crosses a track must stay a scroll gesture. */
        if (s_state.display_pointer_pressed) {
            const int32_t y = event->data.pointer.y;
            const int32_t distance = y - s_state.display_press_y;
            if (distance > SETTINGS_DISPLAY_TAP_SLOP ||
                    distance < -SETTINGS_DISPLAY_TAP_SLOP) {
                s_state.display_pointer_scrolled = true;
                s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
            }
            s_state.display_last_y = y;
            return;
        }
        if (!settings_display_page_is_active(ui)) {
            return;
        }
        const int32_t y = event->data.pointer.y;
        if (y < SETTINGS_DISPLAY_VIEWPORT_Y ||
                y >= SETTINGS_DISPLAY_VIEWPORT_Y +
                     SETTINGS_DISPLAY_VIEWPORT_H) {
            return;
        }
        s_state.display_pointer_pressed = true;
        s_state.display_pointer_scrolled = false;
        s_state.display_press_y = y;
        s_state.display_last_y = y;
        s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
        uint16_t active_page = 0;
        if (esp_gsp_stack_view_get_top(
                ui, GSP_OBJ_KEY_SETTINGS_STACK,
                &active_page) != ESP_GSP_OK) {
            return;
        }
        if (active_page == SETTINGS_DISPLAY_PAGE &&
                y >= SETTINGS_BRIGHTNESS_TRACK_Y_MIN &&
                y <= SETTINGS_BRIGHTNESS_TRACK_Y_MAX) {
            s_state.brightness_drag_active = true;
        } else if (active_page == SETTINGS_SOUND_PAGE &&
                   y >= SETTINGS_VOLUME_TRACK_Y_MIN &&
                   y <= SETTINGS_VOLUME_TRACK_Y_MAX) {
            s_state.volume_drag_active = true;
        } else if (active_page == SETTINGS_SOUND_PAGE &&
                   y >= SETTINGS_NOTIFICATION_Y_MIN &&
                   y <= SETTINGS_NOTIFICATION_Y_MAX) {
            s_state.display_tap = SETTINGS_DISPLAY_TAP_NOTIFICATION;
        }
    }
    if (!s_state.brightness_drag_active && !s_state.volume_drag_active) {
        return;
    }
    if (!s_state.brightness_drag_active && !s_state.volume_drag_active) {
        return;
    }
    const int32_t level = settings_level_from_x(event->data.pointer.x);
    if (s_state.brightness_drag_active) {
        if (level == s_state.brightness) {
            return;
        }
        if (s_state.brightness_hw_available) {
            esp_err_t err = settings_set_brightness(level, false);
            if (err == ESP_ERR_NOT_SUPPORTED) {
                s_state.brightness_hw_available = false;
                ESP_LOGW(TAG,
                         "panel brightness is unsupported; UI remains active");
            } else if (err != ESP_OK) {
                ESP_LOGE(TAG, "set brightness failed: %s",
                         esp_err_to_name(err));
                return;
            }
        }
        s_state.brightness = level;
        s_state.device.display.brightness = level;
        s_state.brightness_dirty = true;
        settings_display_request_render();
        return;
    }
    const int32_t volume = level;
    if (volume == s_state.volume) {
        return;
    }
    esp_err_t err = settings_set_volume(volume, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set volume failed: %s", esp_err_to_name(err));
        return;
    }
    s_state.volume = volume;
    s_state.device.audio.volume = volume;
    s_state.volume_dirty = true;
    settings_display_request_render();
}

static void settings_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
        s_state.ui = ui;
        atomic_store_explicit(
            &s_state.display_render_pending, false, memory_order_release);
        s_state.display_render_timer = esp_gsp_timer_create(
            ui, SETTINGS_DISPLAY_RENDER_PERIOD_MS,
            settings_display_render_timer_cb, NULL);
        if (s_state.display_render_timer == NULL) {
            ESP_LOGE(TAG, "create Display render timer failed");
        }
        /* App state is static, while every open owns a new GSP handle and
         * List pool. Never carry navigation/list lifecycle across a forced
         * home-swipe close and subsequent reopen. */
        s_state.wlan_page_active = false;
        s_state.wlan_entry_pending = false;
        s_state.wlan_leave_pending = false;
        s_state.wlan_detail_active = false;
        s_state.wlan_detail_leave_pending = false;
        s_state.wlan_password_active = false;
        s_state.wlan_phone_active = false;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id = 0;
        s_state.wlan_forget_pending = false;
        s_state.wlan_connect_pending = false;
        s_state.wlan_connect_operation_id = 0;
        s_state.wlan_connect_deadline_us = 0;
        s_state.wlan_scan_revision = 0;
        s_state.wlan_scan_next_us = 0;
        s_state.wlan_scan_waiting = false;
        s_state.wlan_notice_pending = false;
        s_state.wlan_notice_title = NULL;
        s_state.wlan_notice_message = NULL;
        s_state.factory_hold_active = false;
        s_state.factory_hold_completed = false;
        s_state.factory_hold_started_us = 0;
        s_state.wlan.selected_ssid[0] = '\0';
        s_state.wlan.password[0] = '\0';
        s_root_list = ESP_GSP_LIST_NONE;
        s_wlan_list = ESP_GSP_LIST_NONE;
        s_detail_list = ESP_GSP_LIST_NONE;
        s_update_notes_list = ESP_GSP_LIST_NONE;
        s_update_note_count = 0U;
        s_rendered_update_title[0] = '\0';
        s_rendered_update_summary[0] = '\0';
        s_state.wlan_scan_compact_valid = false;
        s_wlan_network_count = 0;
        s_state.display_page_active = false;
        s_state.display_reset_pending = false;
        s_state.display_pointer_pressed = false;
        s_state.display_pointer_scrolled = false;
        s_state.display_tap = SETTINGS_DISPLAY_TAP_NONE;
        s_state.brightness_drag_active = false;
        s_state.brightness_dirty = false;
        s_state.brightness_hw_available = true;
        s_state.volume_drag_active = false;
        s_state.volume_dirty = false;
        s_state.integration_page = 0;
        s_state.integration_rendered_progress = UINT8_MAX;
        s_state.rendered_channel_qr[0] = '\0';
        s_state.rendered_phone_qr[0] = '\0';
        s_state.update_page_active = false;
        s_state.update_leave_pending = false;
        settings_refresh_snapshot(ui);
        s_state.update_notice_sequence =
            s_state.device.update.sequence;
        s_state.wlan_scan_revision =
            s_state.device.network.scan_revision;
        settings_root_list_attach(ui);
        if (s_state.device.network.ssid[0] != '\0') {
            s_state.wlan.connected = s_state.device.network.connected;
            strlcpy(s_state.wlan.current_ssid,
                    s_state.device.network.ssid,
                    sizeof(s_state.wlan.current_ssid));
        }
        (void)settings_wlan_render(ui);
        break;
    case MOSAIC_EVENT_UI_CALL:
        settings_dispatch_call(ui, event);
        break;
    case MOSAIC_EVENT_POINTER:
        settings_dispatch_pointer(ui, event);
        break;
    case MOSAIC_EVENT_MODEL_CHANGED:
        settings_refresh_snapshot(ui);
        settings_wlan_refresh_status(ui);
        settings_root_list_refresh(ui);
        settings_refresh_about_detail(ui);
        if (s_state.update_page_active) {
            settings_render_update_page(ui);
        }
        settings_show_update_result(ui);
        uint16_t model_top = 0U;
        if (s_detail_kind == SETTINGS_DETAIL_BATTERY &&
                esp_gsp_stack_view_get_top(
                    ui, GSP_OBJ_KEY_SETTINGS_STACK,
                    &model_top) == ESP_GSP_OK &&
                model_top == SETTINGS_DETAIL_PAGE) {
            settings_refresh_battery_detail(ui);
        }
        if (settings_wlan_reconcile_navigation(ui)) {
            break;
        }
        if (s_state.wlan_page_active && !s_state.wlan_leave_pending &&
                !s_state.wlan_detail_active &&
                !s_state.wlan_password_active && s_state.wlan.enabled) {
            /* Scan start/end can emit several provider revisions while the
             * config store is busy. Keep this path lightweight: the WLAN page
             * needs the completed scan cache, not a full battery/config/QR
             * snapshot for every intermediate Wi-Fi state. */
            settings_wlan_scan(ui);
            settings_wlan_list_attach(ui);
        }
        break;
    case MOSAIC_EVENT_TIMER:
        /* Also consume the asynchronous cache on the 1 s App tick. This makes
         * list delivery independent of a transiently full loader command
         * queue. settings_wlan_scan refreshes only when content changed. */
        if (s_state.integration_page == SETTINGS_INTEGRATIONS_CHANNELS_PAGE) {
            settings_channels_refresh(ui);
        } else if (s_state.integration_page ==
                SETTINGS_INTEGRATIONS_MODEL_PAGE) {
            if (s_state.llm_phase == SETTINGS_LLM_STATUS) {
                settings_llm_refresh(ui);
            }
            if (s_state.llm_phase == SETTINGS_LLM_PROGRESS) {
                const int64_t elapsed = event->timestamp_us -
                    s_state.integration_started_us;
                uint8_t progress = elapsed <= 0 ? 0 :
                    (uint8_t)((elapsed * SETTINGS_INTEGRATION_PROGRESS_COUNT) /
                        SETTINGS_INTEGRATION_CONFIG_US);
                if (progress > SETTINGS_INTEGRATION_PROGRESS_COUNT) {
                    progress = SETTINGS_INTEGRATION_PROGRESS_COUNT;
                }
                if (progress != s_state.integration_rendered_progress) {
                    s_state.integration_rendered_progress = progress;
                    settings_integration_progress(
                        ui, s_integration_llm_progress, progress);
                }
                if (event->timestamp_us >= s_state.integration_deadline_us) {
                    settings_llm_refresh(ui);
                    if (!s_state.device.agent.llm_configured) {
                        s_state.llm_phase = SETTINGS_LLM_CONFIGURING;
                        settings_llm_render(ui);
                        (void)mosaic_top_notice_show(
                            ui, &s_top_notice, "Configuration not found",
                            "Submit the form on your phone and try again",
                            SETTINGS_NOTICE_DURATION_MS);
                    }
                }
            }
        }
        if (s_state.device.update.state ==
                MOSAIC_CAP_UPDATE_CHECKING) {
            settings_refresh_snapshot(ui);
            settings_refresh_about_detail(ui);
            if (s_state.update_page_active) {
                settings_render_update_page(ui);
            }
            settings_show_update_result(ui);
        }
        if (settings_wlan_reconcile_navigation(ui)) {
            break;
        }
        /* Give a pending notice this update transaction exclusively. This is
         * what makes the capsule reliable on the smaller ESP update queue. */
        if (settings_wlan_show_pending_notice(ui)) {
            break;
        }
        /* Device events normally invalidate the App immediately. This poll
         * also advances the deterministic host provider and covers a dropped
         * invalidation without changing the asynchronous device contract. */
        if (s_state.wlan_page_active || s_state.wlan_connect_pending ||
                s_state.wlan_phone_active) {
            settings_wlan_refresh_status(ui);
        }
        const int64_t wlan_now = esp_timer_get_time();
        if (s_state.wlan_connect_pending &&
                wlan_now >= s_state.wlan_connect_deadline_us) {
            s_state.wlan_connect_pending = false;
            s_state.wlan_notice_title = "Connection timed out";
            s_state.wlan_notice_message =
                "Check the network and try again";
            s_state.wlan_notice_pending = true;
            s_state.wlan_scan_next_us = wlan_now;
            (void)settings_wlan_render(ui);
        }
        uint16_t settings_top = 0U;
        if (s_detail_kind == SETTINGS_DETAIL_BATTERY &&
                esp_gsp_stack_view_get_top(
                    ui, GSP_OBJ_KEY_SETTINGS_STACK,
                    &settings_top) == ESP_GSP_OK &&
                settings_top == SETTINGS_DETAIL_PAGE) {
            settings_refresh_battery_detail(ui);
        }
        if (s_state.wlan_page_active && !s_state.wlan_leave_pending &&
                !s_state.wlan_detail_active &&
                !s_state.wlan_password_active &&
                !s_state.wlan_connect_pending &&
                !s_state.wlan_phone_active && s_state.wlan.enabled) {
            settings_wlan_scan(ui);
            settings_wlan_list_attach(ui);
            if (s_state.device.network.radio_state ==
                    MOSAIC_CAP_WIFI_RADIO_ON &&
                    wlan_now >= s_state.wlan_scan_next_us) {
                s_state.wlan_scan_waiting = false;
                settings_wlan_request_scan();
            }
        }
        break;
    case MOSAIC_EVENT_SCENE_CHANGED:
        (void)settings_wlan_reconcile_navigation(ui);
        break;
    case MOSAIC_EVENT_STOP:
        if (s_state.display_render_timer != NULL) {
            (void)esp_gsp_timer_delete(
                ui, s_state.display_render_timer);
            s_state.display_render_timer = NULL;
        }
        atomic_store_explicit(
            &s_state.display_render_pending, false, memory_order_release);
        mosaic_top_notice_detach(ui);
        (void)settings_wlan_set_loading(ui, false);
        settings_wlan_password_detach(ui);
        settings_wlan_list_park(ui);
        settings_update_notes_park(ui);
        s_root_list = ESP_GSP_LIST_NONE;
        s_wlan_list = ESP_GSP_LIST_NONE;
        s_detail_list = ESP_GSP_LIST_NONE;
        s_update_notes_list = ESP_GSP_LIST_NONE;
        s_state.wlan_scan_compact_valid = false;
        s_state.wlan_notice_pending = false;
        s_state.wlan_notice_title = NULL;
        s_state.wlan_notice_message = NULL;
        s_state.ui = NULL;
        s_state.wlan_page_active = false;
        s_state.wlan_entry_pending = false;
        s_state.wlan_leave_pending = false;
        s_state.wlan_detail_active = false;
        s_state.wlan_detail_leave_pending = false;
        s_state.wlan_password_active = false;
        s_state.wlan_phone_active = false;
        s_state.wlan_phone_exit_pending = false;
        s_state.wlan_phone_operation_id = 0;
        s_state.wlan_forget_pending = false;
        s_state.wlan_connect_pending = false;
        s_state.wlan_connect_operation_id = 0;
        s_state.wlan_connect_deadline_us = 0;
        s_state.wlan_scan_revision = 0;
        s_state.wlan_scan_next_us = 0;
        s_state.wlan_scan_waiting = false;
        s_state.display_page_active = false;
        s_state.display_pointer_pressed = false;
        s_state.brightness_drag_active = false;
        s_state.brightness_dirty = false;
        s_state.volume_drag_active = false;
        s_state.volume_dirty = false;
        s_state.integration_page = 0;
        break;
    default:
        break;
    }
}

static bool settings_back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)timestamp_us;
    if (settings_update_page_visible(ui)) {
        settings_leave_update_page(ui);
        return true;
    }
    s_state.integration_page = 0;
    /* Return false so the loader performs the normal Settings StackView pop. */
    return false;
}

const mosaic_app_descriptor_t mosaic_settings_app = {
    .id = 6,
    .launch_action = GSP_ACT_ID_APP_SETTINGS,
    /* Settings page navigation uses local action 0 for dynamic detail rows.
     * Keep App exit on the Shell-only sentinel so a row click cannot also be
     * interpreted by the runtime as Back-to-Hub. */
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "settings",
    .title = "SETTINGS",
    .directory = &gsp_obj_directory_settings,
    .root_stack_key = GSP_OBJ_KEY_SETTINGS_STACK,
    .disable_swipe = true,
    .root_header_in_stack = false,
    /* Root, WLAN, Detail and Update StackView pages retain their materialized
     * rows while another page is on top. Reserve the aggregate template
     * capacity so a child list cannot exhaust the shared pool. */
    .instance_slots =
        GSP_TEMPLATE_SETTINGS_ROOT_ROW_MAX_INSTANCES +
        GSP_TEMPLATE_SETTINGS_WLAN_ROW_MAX_INSTANCES +
        GSP_TEMPLATE_SETTINGS_DETAIL_ROW_MAX_INSTANCES +
        GSP_TEMPLATE_SETTINGS_UPDATE_NOTE_ROW_MAX_INSTANCES,
    .dynamic_image_slots =
        GSP_TEMPLATE_SETTINGS_ROOT_ROW_MAX_INSTANCES +
        GSP_SETTINGS_DYNAMIC_IMAGE_SLOTS,
    .on_back = settings_back,
    .on_event = settings_event,
};
