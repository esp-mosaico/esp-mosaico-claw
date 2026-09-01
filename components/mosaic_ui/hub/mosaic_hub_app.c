/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_app_catalog.h"
#include "mosaic_hub_app.h"
#include "mosaic_hub_actions.h"
#include "mosaic_hub_binds.h"
#include "mosaic_hub_objects.h"
#include "mosaic_hub_templates.h"
#include "gsp_analog_clock.h"
#include "mosaic_ui.h"
#include "mosaic_capability.h"
#include "mosaic_capability_contracts.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "mosaic_loader.h"
#endif

#define MOSAIC_CLOCK_TICK_MS        1000U
#define MOSAIC_AOD_SWIPE_UP_MIN_PX  60
#define MOSAIC_AOD_TAP_SLOP_PX      32
#define MOSAIC_AOD_UNLOCK_X_MIN      120
#define MOSAIC_AOD_UNLOCK_X_MAX      360
#define MOSAIC_AOD_UNLOCK_Y_MIN      384
#define MOSAIC_HOME_WEATHER_X_MIN     13
#define MOSAIC_HOME_WEATHER_X_MAX    260
#define MOSAIC_HOME_WEATHER_Y_MIN     10
#define MOSAIC_HOME_WEATHER_Y_MAX    219
#define MOSAIC_HOME_WEATHER_TAP_SLOP  16
#define MOSAIC_CHARGE_LEVELS         10U
#define MOSAIC_BATTERY_APPLY_MS      200U
#define MOSAIC_WIFI_POLL_TICKS       5U   /* 5 * 200 ms ≈ 1 s RSSI refresh */
#define MOSAIC_STATUS_WIFI_LEVELS    4U

/* Everything the launcher shell is allowed to reach on the device. Listing
 * it here keeps the root App under the same permission check as every other
 * App instead of quietly holding an implicit full grant. */
#define MOSAIC_HUB_CAPABILITIES ( \
    MOSAIC_CAP_SYSTEM_DISPLAY_READ | MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL | \
    MOSAIC_CAP_SYSTEM_AUDIO_READ | MOSAIC_CAP_SYSTEM_AUDIO_CONTROL | \
    MOSAIC_CAP_SYSTEM_HAPTIC_READ | MOSAIC_CAP_SYSTEM_HAPTIC_CONTROL | \
    MOSAIC_CAP_SYSTEM_POWER_READ | \
    MOSAIC_CAP_NET_WIFI_READ | MOSAIC_CAP_NET_WIFI_CONTROL | \
    MOSAIC_CAP_CONFIG_AGENT_READ | MOSAIC_CAP_NET_WEATHER_READ)
#define MOSAIC_INSERT_ENTER_MS       260U
#define MOSAIC_INSERT_BLINK_HALF_MS  110U
#define MOSAIC_INSERT_EXIT_MS        240U
#define MOSAIC_INSERT_PAUSE_MS        90U
#define MOSAIC_HUB_INSERT_PAGE       2U
#define MOSAIC_INSERT_NAME_MAX        48U
#define MOSAIC_INSERT_CAPABILITY_MAX  96U
#define MOSAIC_INSERT_APP_NAME_MAX    32U
#define MOSAIC_QUICK_FEEDBACK_MS      2000U

static const char *TAG = "mosaic_hub";

typedef enum {
    MOSAIC_LOCK_HIDDEN = 0,
    MOSAIC_LOCK_AOD,
    MOSAIC_LOCK_CHARGING,
} mosaic_lock_mode_t;

typedef struct {
    bool pending;
    char side;
    char friendly_name[MOSAIC_INSERT_NAME_MAX];
    char capability[MOSAIC_INSERT_CAPABILITY_MAX];
    char open_app_name[MOSAIC_INSERT_APP_NAME_MAX];
} mosaic_hub_insert_request_t;

static uint8_t s_charge_percent;
static bool s_aod_tracking;
static int32_t s_aod_x0;
static int32_t s_aod_y0;
static mosaic_lock_mode_t s_lock_mode;
static esp_gsp_list_t s_notif_list = ESP_GSP_LIST_NONE;
static esp_gsp_handle_t s_notif_list_ui;
static esp_gsp_handle_t s_hub_ui;
static void* s_clock_timer;
static void* s_battery_timer;
static void* s_aod_hint_timer;
static bool s_aod_hint_dim;
static bool s_pointer_down;
static bool s_quick_brightness_drag;
static bool s_quick_volume_drag;
static bool s_home_weather_tracking;
static bool s_home_weather_tap_valid;
static int32_t s_home_weather_x0;
static int32_t s_home_weather_y0;
static int32_t s_quick_level;
static bool s_quick_wlan = true;
static bool s_quick_vibration = true;
static bool s_agent_configured;
static mosaic_cap_power_t s_battery_info;
static bool s_battery_pending;
static mosaic_capability_subscription_handle_t s_battery_subscription;
static mosaic_cap_wifi_t s_wifi_info;
static bool s_wifi_pending;
static mosaic_capability_subscription_handle_t s_wifi_subscription;
static uint8_t s_wifi_poll_ticks;
static void *s_insert_timer;
static void *s_quick_feedback_timer;
static char s_insert_side = 'R';
static uint8_t s_insert_phase;
static char s_insert_open_app_name[MOSAIC_INSERT_APP_NAME_MAX];
/* Last charging sample used for edge-triggered lock follow (not continuous). */
static bool s_lock_charge_known;
static bool s_lock_was_charging;
static mosaic_hub_insert_request_t s_insert_request;
static bool s_quick_slot_l_camera_pending;
static bool s_quick_slot_l_camera_occupied;
#if defined(ESP_PLATFORM)
static portMUX_TYPE s_battery_stage_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_HUB_BATT_LOCK()   portENTER_CRITICAL(&s_battery_stage_lock)
#define MOSAIC_HUB_BATT_UNLOCK() portEXIT_CRITICAL(&s_battery_stage_lock)
static portMUX_TYPE s_wifi_stage_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_HUB_WIFI_LOCK()   portENTER_CRITICAL(&s_wifi_stage_lock)
#define MOSAIC_HUB_WIFI_UNLOCK() portEXIT_CRITICAL(&s_wifi_stage_lock)
static portMUX_TYPE s_insert_notice_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_HUB_INSERT_LOCK()   portENTER_CRITICAL(&s_insert_notice_lock)
#define MOSAIC_HUB_INSERT_UNLOCK() portEXIT_CRITICAL(&s_insert_notice_lock)
#else
#define MOSAIC_HUB_BATT_LOCK()   ((void)0)
#define MOSAIC_HUB_BATT_UNLOCK() ((void)0)
#define MOSAIC_HUB_WIFI_LOCK()   ((void)0)
#define MOSAIC_HUB_WIFI_UNLOCK() ((void)0)
#define MOSAIC_HUB_INSERT_LOCK()   ((void)0)
#define MOSAIC_HUB_INSERT_UNLOCK() ((void)0)
#endif

static void mosaic_hub_set_lock_mode(esp_gsp_handle_t ui,
                                     mosaic_lock_mode_t mode);
static bool mosaic_hub_lock_visible(void);
static bool mosaic_hub_quick_drawer_open(esp_gsp_handle_t ui);

static void mosaic_hub_insert_schedule(uint32_t delay_ms);

static void mosaic_hub_quick_feedback_timer_cb(
    esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    void *timer = s_quick_feedback_timer;
    s_quick_feedback_timer = NULL;
    if (timer != NULL) {
        (void)esp_gsp_timer_delete(ui, timer);
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_FEEDBACK_VISIBLE, false);
}

static void mosaic_hub_quick_feedback(const char *text)
{
    if (s_hub_ui == NULL || text == NULL) {
        return;
    }
    if (s_quick_feedback_timer != NULL) {
        (void)esp_gsp_timer_delete(s_hub_ui, s_quick_feedback_timer);
        s_quick_feedback_timer = NULL;
    }
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_QUICK_FEEDBACK_TEXT, text);
    (void)esp_gsp_set_visible(
        s_hub_ui, GSP_BIND_QUICK_FEEDBACK_VISIBLE, true);
    s_quick_feedback_timer = esp_gsp_timer_create(
        s_hub_ui, MOSAIC_QUICK_FEEDBACK_MS,
        mosaic_hub_quick_feedback_timer_cb, NULL);
}

static gsp_component_key_t mosaic_hub_insert_strip(void)
{
    return s_insert_side == 'L' ? GSP_OBJ_KEY_INSERT_FX_LEFT
                                : GSP_OBJ_KEY_INSERT_FX_RIGHT;
}

static uint16_t mosaic_hub_insert_strip_visible(void)
{
    return s_insert_side == 'L' ? GSP_BIND_INSERT_FX_LEFT_VISIBLE
                                : GSP_BIND_INSERT_FX_RIGHT_VISIBLE;
}

static void mosaic_hub_insert_timer_cb(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    void *timer = s_insert_timer;
    s_insert_timer = NULL;
    if (timer != NULL) {
        (void)esp_gsp_timer_delete(ui, timer);
    }
    const gsp_component_key_t strip = mosaic_hub_insert_strip();
    const uint16_t visible = mosaic_hub_insert_strip_visible();
    if (mosaic_hub_lock_visible()) {
        (void)esp_gsp_set_visible(ui, visible, false);
        s_insert_phase = 0;
        return;
    }
    switch (s_insert_phase++) {
    case 0: /* entered: first half of the single blink */
        (void)esp_gsp_set_visible(ui, visible, false);
        mosaic_hub_insert_schedule(MOSAIC_INSERT_BLINK_HALF_MS);
        break;
    case 1: /* blink back on */
        (void)esp_gsp_set_visible(ui, visible, true);
        mosaic_hub_insert_schedule(MOSAIC_INSERT_BLINK_HALF_MS);
        break;
    case 2: /* return through the physical side */
        (void)esp_gsp_component_animate_position_to(
            ui, strip, s_insert_side == 'L' ? -16 : 480, 80,
            MOSAIC_INSERT_EXIT_MS, ESP_GSP_EASE_IN_OUT);
        mosaic_hub_insert_schedule(MOSAIC_INSERT_EXIT_MS);
        break;
    case 3:
        (void)esp_gsp_set_visible(ui, visible, false);
        mosaic_hub_insert_schedule(MOSAIC_INSERT_PAUSE_MS);
        break;
    default:
        s_insert_phase = 0;
        (void)esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_HUB_STACK, MOSAIC_HUB_INSERT_PAGE, true);
        break;
    }
}

static void mosaic_hub_insert_schedule(uint32_t delay_ms)
{
    if (s_hub_ui == NULL) {
        return;
    }
    s_insert_timer = esp_gsp_timer_create(
        s_hub_ui, delay_ms, mosaic_hub_insert_timer_cb, NULL);
}

void mosaic_hub_show_board_insert(
    char side, const char *friendly_name, const char *capability,
    const char *open_app_name)
{
    if (s_hub_ui == NULL || (side != 'L' && side != 'R') ||
            friendly_name == NULL || capability == NULL ||
            open_app_name == NULL ||
            mosaic_hub_lock_visible() ||
            mosaic_hub_quick_drawer_open(s_hub_ui)) {
        return;
    }
    if (s_insert_timer != NULL) {
        (void)esp_gsp_timer_delete(s_hub_ui, s_insert_timer);
        s_insert_timer = NULL;
    }
    (void)esp_gsp_set_visible(
        s_hub_ui, GSP_BIND_INSERT_FX_LEFT_VISIBLE, false);
    (void)esp_gsp_set_visible(
        s_hub_ui, GSP_BIND_INSERT_FX_RIGHT_VISIBLE, false);
    s_insert_side = side;
    s_insert_phase = 0;
    strlcpy(s_insert_open_app_name, open_app_name,
            sizeof(s_insert_open_app_name));
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_INSERT_PORT,
        side == 'L' ? "Left port" : "Right port");
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_INSERT_BOARD_NAME, friendly_name);
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_INSERT_BOARD_CAP, capability);
    const gsp_component_key_t strip = mosaic_hub_insert_strip();
    (void)esp_gsp_component_set_position(
        s_hub_ui, strip, side == 'L' ? -16 : 480, 80);
    (void)esp_gsp_set_visible(
        s_hub_ui, mosaic_hub_insert_strip_visible(), true);
    (void)esp_gsp_component_animate_position_to(
        s_hub_ui, strip, side == 'L' ? 0 : 464, 80,
        MOSAIC_INSERT_ENTER_MS, ESP_GSP_EASE_OUT);
    mosaic_hub_insert_schedule(MOSAIC_INSERT_ENTER_MS);
}

void mosaic_hub_request_board_insert(
    char side, const char *friendly_name, const char *capability,
    const char *open_app_name)
{
    if ((side != 'L' && side != 'R') ||
            friendly_name == NULL || capability == NULL ||
            open_app_name == NULL) {
        return;
    }
    mosaic_hub_insert_request_t request = {
        .pending = true,
        .side = side,
    };
    strlcpy(request.friendly_name, friendly_name,
            sizeof(request.friendly_name));
    strlcpy(request.capability, capability,
            sizeof(request.capability));
    strlcpy(request.open_app_name, open_app_name,
            sizeof(request.open_app_name));

    MOSAIC_HUB_INSERT_LOCK();
    s_insert_request = request;
    MOSAIC_HUB_INSERT_UNLOCK();
}

void mosaic_hub_request_quick_slot_camera(char side, bool occupied)
{
    if (side != 'L') {
        return;
    }
    MOSAIC_HUB_INSERT_LOCK();
    s_quick_slot_l_camera_pending = true;
    s_quick_slot_l_camera_occupied = occupied;
    MOSAIC_HUB_INSERT_UNLOCK();
}

static void mosaic_hub_apply_quick_slot_camera(esp_gsp_handle_t ui, bool occupied)
{
    if (ui == NULL) {
        return;
    }
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_QUICK_SLOT_L_EMPTY_VISIBLE, !occupied);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_QUICK_SLOT_L_CAMERA_VISIBLE, occupied);
}

static bool mosaic_hub_take_quick_slot_camera(bool *out_occupied)
{
    if (out_occupied == NULL) {
        return false;
    }
    MOSAIC_HUB_INSERT_LOCK();
    const bool pending = s_quick_slot_l_camera_pending;
    if (pending) {
        *out_occupied = s_quick_slot_l_camera_occupied;
        s_quick_slot_l_camera_pending = false;
    }
    MOSAIC_HUB_INSERT_UNLOCK();
    return pending;
}

static bool mosaic_hub_take_insert_request(
    mosaic_hub_insert_request_t *out_request)
{
    if (out_request == NULL) {
        return false;
    }
    MOSAIC_HUB_INSERT_LOCK();
    const bool pending = s_insert_request.pending;
    if (pending) {
        *out_request = s_insert_request;
        s_insert_request.pending = false;
    }
    MOSAIC_HUB_INSERT_UNLOCK();
    return pending;
}

esp_err_t mosaic_ui_haptic_feedback(uint32_t duration_ms)
{
    (void)duration_ms;
    if (!s_quick_vibration) {
        return ESP_OK;
    }
    return mosaic_capability_invoke("system.haptic",
        MOSAIC_HUB_CAPABILITIES, "pulse", NULL, 0, NULL, 0);
}

static void mosaic_hub_quick_toggle_render(
    esp_gsp_handle_t ui, gsp_component_key_t tile,
    gsp_component_key_t off_icon, gsp_component_key_t on_icon, bool enabled,
    uint32_t enabled_color)
{
    /* Hub bundles use the RGB565 profile; runtime color updates must use the
     * scene-native representation rather than the source RGB888 literals. */
    (void)esp_gsp_component_set_color(
        ui, tile, enabled ? enabled_color : UINT32_C(0x39E7));
    /* The *_active assets are orange and disappear against the selected
     * orange tile. Keep the white glyph for both states; tile color alone
     * communicates enabled versus disabled, matching the web control center. */
    (void)esp_gsp_component_set_visible(ui, off_icon, true);
    (void)esp_gsp_component_set_visible(ui, on_icon, false);
}

static void mosaic_hub_quick_level_render(
    esp_gsp_handle_t ui, gsp_component_key_t fill,
    gsp_component_key_t input, int level)
{
    const int clamped = level < 0 ? 0 : level > 100 ? 100 : level;
    (void)esp_gsp_component_set_value(ui, fill, clamped);
    (void)esp_gsp_component_set_value(ui, input, clamped);
}

static void mosaic_hub_quick_render(esp_gsp_handle_t ui)
{
    mosaic_hub_quick_toggle_render(ui, GSP_OBJ_KEY_QUICK_WLAN,
        GSP_OBJ_KEY_QUICK_WLAN_OFF, GSP_OBJ_KEY_QUICK_WLAN_ON, s_quick_wlan,
        UINT32_C(0xFA60));
    mosaic_hub_quick_toggle_render(ui, GSP_OBJ_KEY_QUICK_JOIN,
        GSP_OBJ_KEY_QUICK_JOIN_OFF, GSP_OBJ_KEY_QUICK_JOIN_ON, false,
        UINT32_C(0xFA60));
    mosaic_hub_quick_toggle_render(ui, GSP_OBJ_KEY_QUICK_BLUETOOTH,
        GSP_OBJ_KEY_QUICK_BLUETOOTH_OFF, GSP_OBJ_KEY_QUICK_BLUETOOTH_ON,
        false, UINT32_C(0xFA60));
    (void)esp_gsp_component_set_color(
        ui, GSP_OBJ_KEY_QUICK_LOW_POWER,
        UINT32_C(0x39E7));
    mosaic_hub_quick_toggle_render(ui, GSP_OBJ_KEY_QUICK_RINGTONE,
        GSP_OBJ_KEY_QUICK_RINGTONE_OFF, GSP_OBJ_KEY_QUICK_RINGTONE_ON,
        false, UINT32_C(0xF9C6));
    (void)esp_gsp_component_set_visible(
        ui, GSP_OBJ_KEY_QUICK_RINGTONE_OFF, true);
    (void)esp_gsp_component_set_visible(
        ui, GSP_OBJ_KEY_QUICK_RINGTONE_ON, false);
    mosaic_hub_quick_toggle_render(ui, GSP_OBJ_KEY_QUICK_VIBRATION,
        GSP_OBJ_KEY_QUICK_VIBRATION_OFF, GSP_OBJ_KEY_QUICK_VIBRATION_ON,
        s_quick_vibration, UINT32_C(0xFA60));
}

/* The agent config record is a few hundred bytes of strings the launcher
 * does not need, so it is collected off the task stack and reduced to the
 * single flag the status bar shows. */
static bool mosaic_hub_agent_is_configured(void)
{
    mosaic_cap_agent_config_t *config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return false;
    }
    bool configured = false;
    if (mosaic_capability_read("config.agent", MOSAIC_HUB_CAPABILITIES,
            config, sizeof(*config)) == ESP_OK) {
        configured = config->llm_configured &&
            (config->im_wechat_configured || config->im_qq_configured ||
             config->im_feishu_configured || config->im_telegram_configured);
    }
    free(config);
    return configured;
}

static void mosaic_hub_system_status_refresh(esp_gsp_handle_t ui)
{
    if (ui == NULL) {
        return;
    }
    mosaic_cap_haptic_t haptic = {0};
    if (mosaic_capability_read("system.haptic", MOSAIC_HUB_CAPABILITIES,
            &haptic, sizeof(haptic)) == ESP_OK) {
        s_quick_vibration = haptic.enabled;
    }
    s_agent_configured = mosaic_hub_agent_is_configured();

    /* Non-persistent slider updates apply immediately to hardware, while the
     * device model intentionally keeps the last persisted value until
     * release. Do not let the periodic status refresh pull an active drag
     * back to that stale value. */
    mosaic_cap_display_t display = {0};
    if (!s_quick_brightness_drag &&
            mosaic_capability_read("system.display", MOSAIC_HUB_CAPABILITIES,
                &display, sizeof(display)) == ESP_OK) {
        mosaic_hub_quick_level_render(
            ui, GSP_OBJ_KEY_QUICK_BRIGHTNESS_FILL,
            GSP_OBJ_KEY_QUICK_BRIGHTNESS_INPUT, display.brightness);
    }
    mosaic_cap_audio_t audio = {0};
    if (!s_quick_volume_drag &&
            mosaic_capability_read("system.audio", MOSAIC_HUB_CAPABILITIES,
                &audio, sizeof(audio)) == ESP_OK) {
        mosaic_hub_quick_level_render(
            ui, GSP_OBJ_KEY_QUICK_VOLUME_FILL,
            GSP_OBJ_KEY_QUICK_VOLUME_INPUT, audio.volume);
    }
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_STATUS_AGENT, s_agent_configured);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_QUICK_STATUS_AGENT, s_agent_configured);
    mosaic_hub_quick_toggle_render(
        ui, GSP_OBJ_KEY_QUICK_VIBRATION,
        GSP_OBJ_KEY_QUICK_VIBRATION_OFF,
        GSP_OBJ_KEY_QUICK_VIBRATION_ON,
        s_quick_vibration, UINT32_C(0xFA60));
}

static esp_err_t mosaic_hub_set_brightness(int32_t brightness, bool persist)
{
    const mosaic_cap_display_brightness_args_t args = {
        .brightness = brightness,
        .persist = persist,
    };
    return mosaic_capability_invoke("system.display",
        MOSAIC_HUB_CAPABILITIES, "set_brightness", &args, sizeof(args),
        NULL, 0);
}

static esp_err_t mosaic_hub_set_volume(int32_t volume, bool persist)
{
    const mosaic_cap_audio_volume_args_t args = {
        .volume = volume,
        .persist = persist,
    };
    return mosaic_capability_invoke("system.audio", MOSAIC_HUB_CAPABILITIES,
        "set_volume", &args, sizeof(args), NULL, 0);
}

static bool mosaic_hub_lock_visible(void)
{
    return s_lock_mode != MOSAIC_LOCK_HIDDEN;
}

static bool mosaic_hub_quick_drawer_open(esp_gsp_handle_t ui)
{
    bool drawer_open = false;
    return ui != NULL &&
           esp_gsp_drawer_is_open(ui, GSP_OBJ_KEY_QUICK_DRAWER,
                                  &drawer_open) == ESP_GSP_OK &&
           drawer_open;
}

static bool mosaic_hub_is_charging(void)
{
    return s_lock_mode == MOSAIC_LOCK_CHARGING;
}

static uint8_t mosaic_hub_charge_fill_level(uint8_t percent)
{
    if (percent == 0U) {
        return 0U;
    }
    if (percent >= 100U) {
        return (uint8_t)MOSAIC_CHARGE_LEVELS;
    }
    /* Ceil to 1..10 so 1-10% still lights the first segment. */
    return (uint8_t)((percent + 9U) / 10U);
}

static void mosaic_hub_charge_apply(esp_gsp_handle_t ui, uint8_t percent)
{
    static const uint16_t binds[MOSAIC_CHARGE_LEVELS] = {
        GSP_BIND_CHARGE_FILL_1,
        GSP_BIND_CHARGE_FILL_2,
        GSP_BIND_CHARGE_FILL_3,
        GSP_BIND_CHARGE_FILL_4,
        GSP_BIND_CHARGE_FILL_5,
        GSP_BIND_CHARGE_FILL_6,
        GSP_BIND_CHARGE_FILL_7,
        GSP_BIND_CHARGE_FILL_8,
        GSP_BIND_CHARGE_FILL_9,
        GSP_BIND_CHARGE_FILL_10,
    };
    if (percent > 100U) {
        percent = 100U;
    }
    const uint8_t level = mosaic_hub_charge_fill_level(percent);
    for (uint8_t index = 0; index < MOSAIC_CHARGE_LEVELS; ++index) {
        (void)esp_gsp_set_visible(ui, binds[index], index < level);
    }
    char percent_text[8];
    snprintf(percent_text, sizeof(percent_text), "%u", (unsigned)percent);
    (void)gsp_mosaic_hub_charge_percent_set_text(ui, percent_text);

    /* HTML CHRG uses flex+gap; show the "%" slot for the current width. */
    const unsigned digits =
        (percent >= 100U) ? 3U : (percent >= 10U) ? 2U : 1U;
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_CHARGE_PERCENT_STANDARD, digits != 3U);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_CHARGE_PERCENT_100, digits == 3U);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_CHARGE_PERCENT_UNIT_1, digits == 1U);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_CHARGE_PERCENT_UNIT_2, digits == 2U);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_CHARGE_PERCENT_UNIT_3, digits == 3U);
}

static void mosaic_hub_status_battery_apply(
    esp_gsp_handle_t ui, uint8_t percent, bool charging)
{
    typedef struct {
        uint16_t idle_visible;
        uint16_t idle_level;
        uint16_t low_visible;
        uint16_t low_level;
        uint16_t charging_visible;
        uint16_t charging_level;
    } status_battery_surface_t;
    static const status_battery_surface_t surfaces[] = {
        {
            GSP_BIND_STATUS_BATTERY_IDLE,
            GSP_BIND_STATUS_BATTERY_IDLE_LEVEL,
            GSP_BIND_STATUS_BATTERY_LOW,
            GSP_BIND_STATUS_BATTERY_LOW_LEVEL,
            GSP_BIND_STATUS_BATTERY_CHARGING,
            GSP_BIND_STATUS_BATTERY_CHARGING_LEVEL,
        },
        {
            GSP_BIND_QUICK_STATUS_BATTERY_IDLE,
            GSP_BIND_QUICK_STATUS_BATTERY_IDLE_LEVEL,
            GSP_BIND_QUICK_STATUS_BATTERY_LOW,
            GSP_BIND_QUICK_STATUS_BATTERY_LOW_LEVEL,
            GSP_BIND_QUICK_STATUS_BATTERY_CHARGING,
            GSP_BIND_QUICK_STATUS_BATTERY_CHARGING_LEVEL,
        },
    };
    if (ui == NULL) {
        return;
    }
    const int32_t level = percent > 100U ? 100 : (int32_t)percent;
    for (size_t index = 0; index < sizeof(surfaces) / sizeof(surfaces[0]);
            ++index) {
        const status_battery_surface_t *surface = &surfaces[index];
        (void)esp_gsp_set_value(ui, surface->idle_level, level);
        (void)esp_gsp_set_value(ui, surface->low_level, level);
        (void)esp_gsp_set_value(ui, surface->charging_level, level);
        (void)esp_gsp_set_visible(
            ui, surface->idle_visible, !charging && level > 15);
        (void)esp_gsp_set_visible(
            ui, surface->low_visible, !charging && level <= 15);
        (void)esp_gsp_set_visible(
            ui, surface->charging_visible, charging);
    }
}

static void mosaic_hub_battery_event_cb(void *user_ctx, const char *name,
    const void *payload, size_t payload_size)
{
    (void)user_ctx;
    (void)name;
    if (payload == NULL || payload_size != sizeof(mosaic_cap_power_t)) {
        return;
    }
    /* Publishers run on the sampler thread and must not touch GSP; stage the
     * sample for the apply timer. */
    MOSAIC_HUB_BATT_LOCK();
    s_battery_info = *(const mosaic_cap_power_t *)payload;
    s_battery_pending = true;
    MOSAIC_HUB_BATT_UNLOCK();
}

static uint8_t mosaic_hub_wifi_rssi_level(
    bool connected, int32_t rssi)
{
    if (!connected) {
        return 0U;
    }
    if (rssi >= -55) {
        return 4U;
    }
    if (rssi >= -66) {
        return 3U;
    }
    if (rssi >= -77) {
        return 2U;
    }
    return 1U;
}

static void mosaic_hub_status_wifi_apply(
    esp_gsp_handle_t ui, const mosaic_cap_wifi_t *network)
{
    static const uint16_t bar_binds[MOSAIC_STATUS_WIFI_LEVELS + 1U] = {
        GSP_BIND_STATUS_WIFI_BAR_0,
        GSP_BIND_STATUS_WIFI_BAR_1,
        GSP_BIND_STATUS_WIFI_BAR_2,
        GSP_BIND_STATUS_WIFI_BAR_3,
        GSP_BIND_STATUS_WIFI_BAR_4,
    };
    static const uint16_t quick_bar_binds[MOSAIC_STATUS_WIFI_LEVELS + 1U] = {
        GSP_BIND_QUICK_STATUS_WIFI_BAR_0,
        GSP_BIND_QUICK_STATUS_WIFI_BAR_1,
        GSP_BIND_QUICK_STATUS_WIFI_BAR_2,
        GSP_BIND_QUICK_STATUS_WIFI_BAR_3,
        GSP_BIND_QUICK_STATUS_WIFI_BAR_4,
    };
    if (ui == NULL || network == NULL) {
        return;
    }
    const bool show = network->enabled && network->connected;
    const uint8_t level = show
        ? mosaic_hub_wifi_rssi_level(true, network->rssi) : 0U;
    (void)esp_gsp_set_visible(ui, GSP_BIND_STATUS_WIFI, show);
    (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_STATUS_WIFI, show);
    for (uint8_t index = 0; index <= MOSAIC_STATUS_WIFI_LEVELS; ++index) {
        (void)esp_gsp_set_visible(
            ui, bar_binds[index], show && index == level);
        (void)esp_gsp_set_visible(
            ui, quick_bar_binds[index], show && index == level);
    }
}

static void mosaic_hub_wifi_apply(
    esp_gsp_handle_t ui, const mosaic_cap_wifi_t *network)
{
    if (ui == NULL || network == NULL) {
        return;
    }
    mosaic_hub_status_wifi_apply(ui, network);
    /* Keep Control Center WLAN tile aligned with the radio enable bit. */
    if (s_quick_wlan != network->desired_enabled) {
        s_quick_wlan = network->desired_enabled;
        mosaic_hub_quick_toggle_render(
            ui, GSP_OBJ_KEY_QUICK_WLAN,
            GSP_OBJ_KEY_QUICK_WLAN_OFF, GSP_OBJ_KEY_QUICK_WLAN_ON,
            s_quick_wlan, UINT32_C(0xFA60));
    }
}

static void mosaic_hub_wifi_event_cb(void *user_ctx, const char *name,
    const void *payload, size_t payload_size)
{
    (void)user_ctx;
    (void)name;
    if (payload == NULL || payload_size != sizeof(mosaic_cap_wifi_t)) {
        return;
    }
    MOSAIC_HUB_WIFI_LOCK();
    s_wifi_info = *(const mosaic_cap_wifi_t *)payload;
    s_wifi_pending = true;
    MOSAIC_HUB_WIFI_UNLOCK();
}

static bool mosaic_hub_wifi_take_pending(mosaic_cap_wifi_t *out)
{
    MOSAIC_HUB_WIFI_LOCK();
    if (!s_wifi_pending) {
        MOSAIC_HUB_WIFI_UNLOCK();
        return false;
    }
    *out = s_wifi_info;
    s_wifi_pending = false;
    MOSAIC_HUB_WIFI_UNLOCK();
    return true;
}

static void mosaic_hub_wifi_refresh(esp_gsp_handle_t ui)
{
    if (ui == NULL) {
        return;
    }
    mosaic_cap_wifi_t network = {0};
    if (mosaic_hub_wifi_take_pending(&network) ||
            mosaic_capability_read("net.wifi", MOSAIC_HUB_CAPABILITIES,
                &network, sizeof(network)) == ESP_OK) {
        mosaic_hub_wifi_apply(ui, &network);
    }
}

static void mosaic_hub_weather_apply(
    esp_gsp_handle_t ui, const mosaic_cap_weather_t *weather)
{
    if (ui == NULL || weather == NULL) {
        return;
    }
    char temperature[12];
    const char *description;
    const char *city;
    if (weather->weather_valid) {
        snprintf(temperature, sizeof(temperature), "%d°",
                 (int)weather->temperature_c);
        description = weather->condition[0] != '\0'
            ? weather->condition : "Weather";
    } else {
        strlcpy(temperature, "--", sizeof(temperature));
        description = "--";
    }
    city = weather->location_valid && weather->city[0] != '\0'
        ? weather->city : "--";
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HOME_WEATHER_TEMP, temperature);
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HOME_WEATHER_DESC, description);
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HOME_WEATHER_CITY, city);
}

/* Polling on the clock tick keeps every snapshot handoff on the UI task, so
 * the Hub needs no staging buffer for the weather worker thread. */
static void mosaic_hub_weather_refresh(esp_gsp_handle_t ui)
{
    mosaic_cap_weather_t weather = {0};
    if (mosaic_capability_read("net.weather", MOSAIC_HUB_CAPABILITIES,
            &weather, sizeof(weather)) == ESP_OK) {
        mosaic_hub_weather_apply(ui, &weather);
    }
}

static void mosaic_hub_battery_apply(esp_gsp_handle_t ui,
                                     const mosaic_cap_power_t *battery)
{
    if (ui == NULL || battery == NULL) {
        return;
    }
    if (!battery->available) {
        mosaic_hub_status_battery_apply(ui, 0, false);
        if (mosaic_hub_is_charging()) {
            s_charge_percent = (uint8_t)((s_charge_percent + 10U) % 110U);
            mosaic_hub_charge_apply(ui, s_charge_percent);
        }
        return;
    }

    const uint8_t battery_percent = (uint8_t)battery->percent;
#if defined(ESP_PLATFORM)
    s_charge_percent = battery_percent;
#endif
    {
        static uint8_t s_last_ui_soc = 0xFF;
        static bool s_last_ui_charging;
        if (s_last_ui_soc != battery_percent ||
                s_last_ui_charging != battery->charging) {
            s_last_ui_soc = battery_percent;
            s_last_ui_charging = battery->charging;
            printf("mosaic: ui battery apply SoC=%u%% charging=%d\n",
                   (unsigned)battery_percent, (int)battery->charging);
        }
    }
    mosaic_hub_status_battery_apply(
        ui, battery_percent, battery->charging);
    /* Battery state selects the variant only while the Lock Screen is
     * already visible. Plug/unplug must never navigate away from Home. */
    if (!s_lock_charge_known) {
        s_lock_charge_known = true;
        s_lock_was_charging = battery->charging;
        if (mosaic_hub_lock_visible()) {
            const mosaic_lock_mode_t want = battery->charging
                ? MOSAIC_LOCK_CHARGING : MOSAIC_LOCK_AOD;
            if (want != s_lock_mode) {
                mosaic_hub_set_lock_mode(ui, want);
            }
        }
    } else if (battery->charging != s_lock_was_charging) {
        s_lock_was_charging = battery->charging;
        if (mosaic_hub_lock_visible()) {
            mosaic_hub_set_lock_mode(
                ui, battery->charging ? MOSAIC_LOCK_CHARGING
                                      : MOSAIC_LOCK_AOD);
        }
    }
    if (mosaic_hub_is_charging()) {
        mosaic_hub_charge_apply(ui, s_charge_percent);
    }
}

static bool mosaic_hub_battery_take_pending(mosaic_cap_power_t *out)
{
    MOSAIC_HUB_BATT_LOCK();
    if (!s_battery_pending) {
        MOSAIC_HUB_BATT_UNLOCK();
        return false;
    }
    *out = s_battery_info;
    s_battery_pending = false;
    MOSAIC_HUB_BATT_UNLOCK();
    return true;
}

static void mosaic_hub_battery_refresh(esp_gsp_handle_t ui)
{
    if (ui == NULL) {
        return;
    }
    mosaic_cap_power_t battery = {0};
    if (mosaic_hub_battery_take_pending(&battery) ||
            mosaic_capability_read("system.power", MOSAIC_HUB_CAPABILITIES,
                &battery, sizeof(battery)) == ESP_OK) {
        mosaic_hub_battery_apply(ui, &battery);
    }
}

static void mosaic_hub_charge_refresh(esp_gsp_handle_t ui)
{
#if !defined(ESP_PLATFORM)
    s_charge_percent = 0U;
    mosaic_hub_charge_apply(ui, s_charge_percent);
#else
    mosaic_cap_power_t battery = {0};
    if (mosaic_capability_read("system.power", MOSAIC_HUB_CAPABILITIES,
                &battery, sizeof(battery)) == ESP_OK &&
            battery.available) {
        s_charge_percent = (uint8_t)battery.percent;
        mosaic_hub_charge_apply(ui, s_charge_percent);
        return;
    }
    mosaic_hub_charge_apply(ui, s_charge_percent);
#endif
}

static void mosaic_hub_clock_apply(esp_gsp_handle_t ui)
{
    time_t now = time(NULL);
    struct tm local = {0};
    if (localtime_r(&now, &local) == NULL) {
        return;
    }
    const int hour = local.tm_hour;
    const int minute = local.tm_min;
    const int second = local.tm_sec;

    char aod_clock[8];
    char status[8];

    snprintf(aod_clock, sizeof(aod_clock), "%02d:%02d", hour, minute);
    snprintf(status, sizeof(status), "%02d:%02d", hour, minute);

    static const char *const weekdays_title[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };

    /* Physical clock orb emitted by hub/scene/gen_scenes.py. */
    static const gsp_analog_clock_ops_t s_home_clock = {
        .set_hour_deg = gsp_mosaic_hub_home_clock_hour_set_start_angle,
        .set_minute_deg = gsp_mosaic_hub_home_clock_minute_set_start_angle,
        .set_second_deg = gsp_mosaic_hub_home_clock_second_set_start_angle,
    };
    (void)gsp_analog_clock_set_time(ui, &s_home_clock, hour, minute, second);

    /* AOD = Figma 首页1-熄屏 (single bold HH:MM). */
    (void)gsp_mosaic_hub_aod_clock_set_text(ui, aod_clock);
    char aod_date[32];
    snprintf(aod_date, sizeof(aod_date), "%d/%d · ",
             local.tm_mon + 1, local.tm_mday);
    (void)gsp_mosaic_hub_aod_date_prefix_set_text(ui, aod_date);
    (void)gsp_mosaic_hub_aod_date_day_set_text(
        ui, weekdays_title[local.tm_wday]);

    (void)gsp_mosaic_hub_status_time_set_text(ui, status);
    (void)gsp_mosaic_hub_quick_status_time_set_text(ui, status);
#if !defined(ESP_PLATFORM)
    /* Host-only charging demo: exercise every percentage width, including
     * 100%, without allowing the fixed host battery snapshot to pin it. */
    if (mosaic_hub_is_charging()) {
        s_charge_percent = s_charge_percent >= 100U
            ? 0U : (uint8_t)(s_charge_percent + 10U);
        mosaic_hub_charge_apply(ui, s_charge_percent);
    }
#endif
}

static void mosaic_hub_set_lock_mode(esp_gsp_handle_t ui,
                                     mosaic_lock_mode_t mode)
{
    if (ui == NULL || mode == s_lock_mode) {
        return;
    }
    if (mode == MOSAIC_LOCK_HIDDEN) {
        (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_VISIBLE, false);
        s_lock_mode = mode;
        s_aod_tracking = false;
        printf("mosaic: unlock → resume hub\n");
        return;
    }

    /* Lock Screen is above Drawer. Closing it also prevents a parked Drawer
     * settle from becoming visible immediately after unlock. */
    (void)esp_gsp_drawer_close(ui, GSP_OBJ_KEY_QUICK_DRAWER, false);
    if (mode == MOSAIC_LOCK_CHARGING) {
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_LOCK_SCREEN_CHARGE_VISIBLE, true);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_LOCK_SCREEN_AOD_VISIBLE, false);
        mosaic_hub_charge_refresh(ui);
    } else {
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_LOCK_SCREEN_AOD_VISIBLE, true);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_LOCK_SCREEN_CHARGE_VISIBLE, false);
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_VISIBLE, true);
    s_lock_mode = mode;
    s_aod_tracking = false;
    printf("mosaic: lock screen mode=%s\n",
           mode == MOSAIC_LOCK_CHARGING ? "charging" : "aod");
}

static void mosaic_hub_enter_lock(esp_gsp_handle_t ui)
{
    mosaic_cap_power_t battery = {0};
    const bool charging =
        mosaic_capability_read("system.power", MOSAIC_HUB_CAPABILITIES,
            &battery, sizeof(battery)) == ESP_OK &&
        battery.available && battery.charging;
    mosaic_hub_set_lock_mode(
        ui, charging ? MOSAIC_LOCK_CHARGING : MOSAIC_LOCK_AOD);
}

void mosaic_hub_show_lock_screen(bool charging)
{
    mosaic_hub_set_lock_mode(
        s_hub_ui, charging ? MOSAIC_LOCK_CHARGING : MOSAIC_LOCK_AOD);
}

void mosaic_hub_lock_screen(void)
{
    mosaic_hub_enter_lock(s_hub_ui);
}

void mosaic_hub_set_charging(bool charging)
{
    mosaic_hub_show_lock_screen(charging);
}

bool mosaic_hub_handle_action(uint16_t action_id)
{
    if (action_id == GSP_ACT_ID_INSERT_OPEN) {
        uint16_t page = 0;
        if (s_hub_ui != NULL &&
                esp_gsp_stack_view_get_top(
                    s_hub_ui, GSP_OBJ_KEY_HUB_STACK, &page) == ESP_GSP_OK &&
                page == MOSAIC_HUB_INSERT_PAGE) {
            (void)esp_gsp_stack_view_pop(
                s_hub_ui, GSP_OBJ_KEY_HUB_STACK, false);
        }
#if defined(ESP_PLATFORM)
        const mosaic_app_descriptor_t *app =
            mosaic_app_descriptor_for_name(s_insert_open_app_name);
        if (app != NULL) {
            (void)mosaic_loader_request(app);
        }
#endif
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_WLAN_TOGGLE) {
        const bool next = !s_quick_wlan;
        s_quick_wlan = next;
        mosaic_hub_quick_render(s_hub_ui);
        const mosaic_cap_wifi_enable_args_t wifi_args = { .enabled = next };
        if (mosaic_capability_invoke("net.wifi", MOSAIC_HUB_CAPABILITIES,
                "set_enabled", &wifi_args, sizeof(wifi_args), NULL, 0) !=
                ESP_OK) {
            s_quick_wlan = !next;
            mosaic_hub_quick_render(s_hub_ui);
        }
        mosaic_hub_quick_feedback(
            s_quick_wlan ? "Wi-Fi On" : "Wi-Fi Off");
        /* The Wi-Fi publish drives both the tile and status icon. */
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_JOIN_TOGGLE) {
        /* Reserved control: the current board has no Join backend. */
        mosaic_hub_quick_feedback("Not Supported");
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_BLUETOOTH_TOGGLE) {
        /* Reserved until the system Bluetooth state machine is connected. */
        mosaic_hub_quick_feedback("Not Supported");
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_LOW_POWER_TOGGLE) {
        /* Reserved until a platform low-power policy is available. */
        mosaic_hub_quick_feedback("Not Supported");
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_RINGTONE_TOGGLE) {
        /* Notification/ringtone audio is not implemented on this board. */
        mosaic_hub_quick_feedback("Not Supported");
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_VIBRATION_TOGGLE) {
        const bool next = !s_quick_vibration;
        const mosaic_cap_haptic_enable_args_t haptic_args = {
            .enabled = next,
        };
        const esp_err_t err = mosaic_capability_invoke("system.haptic",
            MOSAIC_HUB_CAPABILITIES, "set_enabled", &haptic_args,
            sizeof(haptic_args), NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set vibration %s failed: %s",
                     next ? "on" : "off", esp_err_to_name(err));
            return true;
        }
        s_quick_vibration = next;
        mosaic_hub_quick_render(s_hub_ui);
        mosaic_hub_quick_feedback(
            next ? "Vibration On" : "Vibration Off");
        if (next) {
            const esp_err_t feedback_err = mosaic_ui_haptic_feedback(40U);
            if (feedback_err != ESP_OK) {
                ESP_LOGE(TAG, "vibration confirmation failed: %s",
                         esp_err_to_name(feedback_err));
            }
        }
        return true;
    }
    if (action_id == GSP_ACT_ID_LOCK_SCREEN_AOD) {
        mosaic_hub_show_lock_screen(false);
        return true;
    }
    if (action_id == GSP_ACT_ID_LOCK_SCREEN_CHARGE) {
        mosaic_hub_show_lock_screen(true);
        return true;
    }
    return false;
}

static bool mosaic_hub_home_surface_visible(esp_gsp_handle_t ui)
{
    uint16_t stack_page = UINT16_MAX;
    uint16_t launcher_page = UINT16_MAX;
    return !mosaic_hub_quick_drawer_open(ui) &&
        esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_HUB_STACK, &stack_page) == ESP_GSP_OK &&
        stack_page == 0 &&
        esp_gsp_page_flow_get_page(
            ui, GSP_OBJ_KEY_LAUNCHER_FLOW, &launcher_page) == ESP_GSP_OK &&
        launcher_page == 0;
}

static bool mosaic_hub_intercept_pointer(
    esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *user_ctx)
{
    (void)user_ctx;

    /* The Home weather shortcut owns the complete left half of the card:
     * temperature, condition, and city. Handling it before scene hit-testing
     * avoids the generic pressed scrim while the right clock stays inert. */
    if (!mosaic_hub_lock_visible()) {
        if (pressed && !s_pointer_down &&
                mosaic_hub_home_surface_visible(ui) &&
                x >= MOSAIC_HOME_WEATHER_X_MIN &&
                x < MOSAIC_HOME_WEATHER_X_MAX &&
                y >= MOSAIC_HOME_WEATHER_Y_MIN &&
                y < MOSAIC_HOME_WEATHER_Y_MAX) {
            s_home_weather_tracking = true;
            s_home_weather_tap_valid = true;
            s_home_weather_x0 = x;
            s_home_weather_y0 = y;
        }
        if (s_home_weather_tracking) {
            const int32_t dx = x - s_home_weather_x0;
            const int32_t dy = y - s_home_weather_y0;
            if (dx < -MOSAIC_HOME_WEATHER_TAP_SLOP ||
                    dx > MOSAIC_HOME_WEATHER_TAP_SLOP ||
                    dy < -MOSAIC_HOME_WEATHER_TAP_SLOP ||
                    dy > MOSAIC_HOME_WEATHER_TAP_SLOP) {
                s_home_weather_tap_valid = false;
            }
            s_pointer_down = pressed;
            if (!pressed) {
                const bool open_weather = s_home_weather_tap_valid;
                s_home_weather_tracking = false;
                s_home_weather_tap_valid = false;
                if (open_weather) {
                    const mosaic_app_descriptor_t *weather =
                        mosaic_app_descriptor_for_action(
                            GSP_ACT_ID_APP_WEATHER);
                    if (weather != NULL) {
                        (void)mosaic_loader_request(weather);
                    }
                }
            }
            return true;
        }
    } else {
        s_home_weather_tracking = false;
        s_home_weather_tap_valid = false;
    }

    bool drawer_open = false;
    if (pressed && !s_pointer_down) {
        if (esp_gsp_drawer_is_open(ui, GSP_OBJ_KEY_QUICK_DRAWER,
                                   &drawer_open) != ESP_GSP_OK) {
            drawer_open = false;
        }
        if (drawer_open && y >= 298 && y <= 450) {
            s_quick_volume_drag = x >= 270 && x < 342;
            s_quick_brightness_drag = x >= 374 && x < 446;
        }
    }

    /* Mirror the invisible C1 slider hit targets into their flat liquid
     * progress layers. The native slider continues to own drag semantics;
     * these updates only keep the clipped visual fill in sync. */
    if (pressed && (s_quick_brightness_drag || s_quick_volume_drag)) {
        const int32_t value = ((450 - y) * 100 + 76) / 152;
        s_quick_level = value < 0 ? 0 : value > 100 ? 100 : value;
        if (s_quick_brightness_drag) {
            mosaic_hub_quick_level_render(
                ui, GSP_OBJ_KEY_QUICK_BRIGHTNESS_FILL,
                GSP_OBJ_KEY_QUICK_BRIGHTNESS_INPUT, s_quick_level);
            (void)mosaic_hub_set_brightness(s_quick_level, false);
        } else {
            mosaic_hub_quick_level_render(
                ui, GSP_OBJ_KEY_QUICK_VOLUME_FILL,
                GSP_OBJ_KEY_QUICK_VOLUME_INPUT, s_quick_level);
            (void)mosaic_hub_set_volume(s_quick_level, false);
        }
    }
    if (!pressed && s_pointer_down) {
        if (s_quick_brightness_drag) {
            (void)mosaic_hub_set_brightness(s_quick_level, true);
        } else if (s_quick_volume_drag) {
            (void)mosaic_hub_set_volume(s_quick_level, true);
            mosaic_hub_quick_render(ui);
        }
        s_quick_brightness_drag = false;
        s_quick_volume_drag = false;
    }
    s_pointer_down = pressed;

    if (!mosaic_hub_lock_visible()) {
        s_aod_tracking = false;
        return false;
    }

    if (pressed) {
        if (!s_aod_tracking) {
            s_aod_tracking = true;
            s_aod_x0 = x;
            s_aod_y0 = y;
        }
        return true;
    }

    if (!s_aod_tracking) {
        return true;
    }
    s_aod_tracking = false;
    s_pointer_down = false;
    s_quick_brightness_drag = false;
    s_quick_volume_drag = false;
    s_home_weather_tracking = false;
    s_home_weather_tap_valid = false;

    const int32_t dy = s_aod_y0 - y; /* up = positive */
    const int32_t dx = (x > s_aod_x0) ? (x - s_aod_x0) : (s_aod_x0 - x);
    /* Prefer vertical unlock; charging silhouette makes diagonal swipes common. */
    const bool swipe_up = (dy >= (int32_t)MOSAIC_AOD_SWIPE_UP_MIN_PX) &&
                          (dy > dx);
#if defined(ESP_PLATFORM)
    const bool mode_button = false;
#else
    const int32_t ady = (dy >= 0) ? dy : -dy;
    const bool tap = (dx <= (int32_t)MOSAIC_AOD_TAP_SLOP_PX) &&
                     (ady <= (int32_t)MOSAIC_AOD_TAP_SLOP_PX);
    const bool mode_button = s_aod_x0 >= 382 && s_aod_y0 <= 56 &&
                             x >= 382 && y <= 56 && tap;
#endif
    const bool unlock_origin =
        s_aod_x0 >= MOSAIC_AOD_UNLOCK_X_MIN &&
        s_aod_x0 <= MOSAIC_AOD_UNLOCK_X_MAX &&
        s_aod_y0 >= MOSAIC_AOD_UNLOCK_Y_MIN;
    if (mode_button) {
        mosaic_hub_set_lock_mode(
            ui, mosaic_hub_is_charging()
                ? MOSAIC_LOCK_AOD : MOSAIC_LOCK_CHARGING);
        return true;
    }
    if (unlock_origin && swipe_up) {
        mosaic_hub_set_lock_mode(ui, MOSAIC_LOCK_HIDDEN);
    }
    /* A visible Lock Screen is modal: horizontal drags and every unhandled
     * sample are consumed before Drawer/PageFlow/scene routing. */
    return true;
}

static void mosaic_hub_clock_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    mosaic_hub_clock_apply(ui);
}

static void mosaic_hub_battery_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    mosaic_hub_insert_request_t insert_request = {0};
    if (mosaic_hub_take_insert_request(&insert_request)) {
        const bool show_notice =
#if defined(ESP_PLATFORM)
            mosaic_loader_app() == mosaic_app_root() &&
#endif
            !mosaic_hub_quick_drawer_open(ui);
        if (show_notice) {
            mosaic_hub_set_lock_mode(ui, MOSAIC_LOCK_HIDDEN);
#if defined(ESP_PLATFORM)
            mosaic_ui_note_screen_activity();
#endif
            mosaic_hub_show_board_insert(
                insert_request.side, insert_request.friendly_name,
                insert_request.capability, insert_request.open_app_name);
        }
    }
    bool slot_camera_occupied = false;
    if (mosaic_hub_take_quick_slot_camera(&slot_camera_occupied)) {
        mosaic_hub_apply_quick_slot_camera(ui, slot_camera_occupied);
    }
    mosaic_cap_power_t battery = {0};
    if (mosaic_hub_battery_take_pending(&battery)) {
        mosaic_hub_battery_apply(ui, &battery);
    }
    mosaic_cap_wifi_t network = {0};
    if (mosaic_hub_wifi_take_pending(&network)) {
        s_wifi_poll_ticks = 0;
        mosaic_hub_wifi_apply(ui, &network);
    } else if (++s_wifi_poll_ticks >= MOSAIC_WIFI_POLL_TICKS) {
        /* RSSI moves without Wi-Fi state events; refresh on a slow cadence. */
        s_wifi_poll_ticks = 0;
        if (mosaic_capability_read("net.wifi", MOSAIC_HUB_CAPABILITIES,
                &network, sizeof(network)) == ESP_OK) {
            mosaic_hub_wifi_apply(ui, &network);
        }
        mosaic_hub_system_status_refresh(ui);
    }
    mosaic_hub_weather_refresh(ui);
}

static void mosaic_hub_aod_hint_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    if (!mosaic_hub_lock_visible()) {
        s_aod_hint_dim = false;
        return;
    }
    s_aod_hint_dim = !s_aod_hint_dim;
    (void)gsp_mosaic_hub_theme_animate_aod_hint_to(
        ui,
        s_aod_hint_dim ? UINT32_C(0x6B4E) : UINT32_C(0xD6BB),
        1300, ESP_GSP_EASE_IN_OUT);
}

static void mosaic_hub_start_timers(esp_gsp_handle_t ui)
{
    if (s_clock_timer == NULL) {
        s_clock_timer = esp_gsp_timer_create(
            ui, MOSAIC_CLOCK_TICK_MS, mosaic_hub_clock_tick, NULL);
        if (s_clock_timer == NULL) {
            printf("mosaic: clock gsp timer create failed\n");
        } else {
            printf("mosaic: clock gsp timer %ums (system local time)\n",
                   (unsigned)MOSAIC_CLOCK_TICK_MS);
        }
    }
    if (s_battery_timer == NULL) {
        s_battery_timer = esp_gsp_timer_create(
            ui, MOSAIC_BATTERY_APPLY_MS, mosaic_hub_battery_tick, NULL);
        if (s_battery_timer == NULL) {
            printf("mosaic: battery apply timer create failed\n");
        } else {
            printf("mosaic: battery apply timer %ums (subscribe/notify)\n",
                   (unsigned)MOSAIC_BATTERY_APPLY_MS);
        }
    }
    if (s_aod_hint_timer == NULL) {
        s_aod_hint_timer = esp_gsp_timer_create(
            ui, 1300, mosaic_hub_aod_hint_tick, NULL);
        if (s_aod_hint_timer == NULL) {
            printf("mosaic: AOD hint timer create failed\n");
        }
    }
}

static void mosaic_hub_stop_timers(esp_gsp_handle_t ui)
{
    if (s_quick_feedback_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_quick_feedback_timer);
        s_quick_feedback_timer = NULL;
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_FEEDBACK_VISIBLE, false);
    if (s_insert_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_insert_timer);
        s_insert_timer = NULL;
        s_insert_phase = 0;
    }
    if (s_clock_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_clock_timer);
        s_clock_timer = NULL;
    }
    if (s_battery_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_battery_timer);
        s_battery_timer = NULL;
    }
    if (s_aod_hint_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_aod_hint_timer);
        s_aod_hint_timer = NULL;
    }
    s_aod_hint_dim = false;
}

static void mosaic_hub_notif_bind(esp_gsp_handle_t ui)
{
    /* List bindings survive scene changes. Returning to the hub must reuse
     * the existing quota slot instead of binding the same logical list. */
    if (s_notif_list_ui != ui) {
        s_notif_list = ESP_GSP_LIST_NONE;
    }
    if (s_notif_list == ESP_GSP_LIST_NONE) {
        s_notif_list = esp_gsp_list_bind_component(
            ui, GSP_OBJ_KEY_NOTIF_LIST, NULL, NULL);
    }
    if (s_notif_list == ESP_GSP_LIST_NONE) {
        printf("mosaic: notif_list bind failed\n");
        return;
    }
    s_notif_list_ui = ui;
    (void)esp_gsp_list_set_total(ui, s_notif_list,
                                 GSP_TEMPLATE_NOTIF_LIST_ITEM_COUNT);
    (void)esp_gsp_list_scroll_to(ui, s_notif_list, 0);
    (void)gsp_mosaic_hub_notif_list_set_selected(ui, 0);
}

static void mosaic_hub_sync_app_slots(esp_gsp_handle_t ui)
{
    static const struct {
        uint16_t action;
        uint16_t visible_bind;
        uint16_t title_bind;
    } slots[] = {
        { GSP_ACT_ID_APP_IMU, GSP_BIND_APP_SLOT_IMU_VISIBLE, UINT16_MAX },
        { GSP_ACT_ID_APP_DYNAMIC_1, GSP_BIND_APP_SLOT_BREAKOUT_VISIBLE,
          UINT16_MAX },
        { GSP_ACT_ID_APP_DYNAMIC_2, GSP_BIND_APP_SLOT_DYNAMIC_2_VISIBLE,
          GSP_BIND_APP_SLOT_DYNAMIC_2_TITLE },
        { GSP_ACT_ID_APP_DYNAMIC_3, GSP_BIND_APP_SLOT_DYNAMIC_3_VISIBLE,
          GSP_BIND_APP_SLOT_DYNAMIC_3_TITLE },
        { GSP_ACT_ID_APP_DYNAMIC_4, GSP_BIND_APP_SLOT_DYNAMIC_4_VISIBLE,
          GSP_BIND_APP_SLOT_DYNAMIC_4_TITLE },
        { GSP_ACT_ID_APP_DYNAMIC_5, GSP_BIND_APP_SLOT_DYNAMIC_5_VISIBLE,
          GSP_BIND_APP_SLOT_DYNAMIC_5_TITLE },
    };
    for (size_t index = 0; index < sizeof(slots) / sizeof(slots[0]); ++index) {
        const mosaic_app_descriptor_t *app =
            mosaic_app_descriptor_for_action(slots[index].action);
        (void)esp_gsp_set_visible(ui, slots[index].visible_bind, app != NULL);
        if (app != NULL && slots[index].title_bind != UINT16_MAX) {
            (void)esp_gsp_set_text(ui, slots[index].title_bind, app->title);
        }
    }
}

static void mosaic_hub_started(esp_gsp_handle_t ui)
{
    s_hub_ui = ui;
    s_charge_percent = 0;
    s_aod_tracking = false;
    s_pointer_down = false;
    s_quick_brightness_drag = false;
    s_quick_volume_drag = false;
    s_lock_mode = MOSAIC_LOCK_HIDDEN;
    s_lock_charge_known = false;
    s_lock_was_charging = false;
    s_battery_pending = false;
    s_wifi_pending = false;
#if defined(ESP_PLATFORM)
    (void)esp_gsp_set_visible(ui, GSP_BIND_AOD_MODE_SWITCH_VISIBLE, false);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CHARGE_MODE_SWITCH_VISIBLE, false);
#else
    (void)esp_gsp_set_visible(ui, GSP_BIND_AOD_MODE_SWITCH_VISIBLE, true);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CHARGE_MODE_SWITCH_VISIBLE, true);
#endif
    mosaic_hub_clock_apply(ui);
    if (s_battery_subscription == NULL) {
        (void)mosaic_capability_subscribe("system.power",
            MOSAIC_HUB_CAPABILITIES, mosaic_hub_battery_event_cb, NULL,
            &s_battery_subscription);
    }
    if (s_wifi_subscription == NULL) {
        (void)mosaic_capability_subscribe("net.wifi",
            MOSAIC_HUB_CAPABILITIES, mosaic_hub_wifi_event_cb, NULL,
            &s_wifi_subscription);
    }
    mosaic_hub_battery_refresh(ui);
    mosaic_hub_wifi_refresh(ui);
    mosaic_hub_system_status_refresh(ui);
    mosaic_hub_weather_refresh(ui);
    mosaic_hub_notif_bind(ui);
    mosaic_hub_sync_app_slots(ui);
    mosaic_hub_quick_render(ui);
    bool slot_camera_occupied = false;
    if (mosaic_hub_take_quick_slot_camera(&slot_camera_occupied)) {
        mosaic_hub_apply_quick_slot_camera(ui, slot_camera_occupied);
    }
    (void)esp_gsp_set_input_interceptor(
        ui, mosaic_hub_intercept_pointer, NULL);
    mosaic_hub_start_timers(ui);
}

static void mosaic_hub_stopping(esp_gsp_handle_t ui)
{
    mosaic_hub_stop_timers(ui);
    if (s_battery_subscription != NULL &&
            mosaic_capability_unsubscribe(s_battery_subscription) == ESP_OK) {
        s_battery_subscription = NULL;
    }
    if (s_wifi_subscription != NULL &&
            mosaic_capability_unsubscribe(s_wifi_subscription) == ESP_OK) {
        s_wifi_subscription = NULL;
    }
    (void)esp_gsp_set_pointer_observer(ui, NULL, NULL);
    if (s_hub_ui == ui) {
        s_hub_ui = NULL;
    }
}

/* Back inside the root App walks the launcher's own navigation, which the
 * loader has no view of: first the modal stack, then the page flow, and
 * finally the Lock Screen as the resting surface. */
static bool mosaic_hub_root_back(esp_gsp_handle_t ui)
{
    uint16_t page = 0;
    if (esp_gsp_stack_view_get_top(ui, GSP_OBJ_KEY_HUB_STACK, &page) ==
            ESP_GSP_OK && page != 0) {
        (void)esp_gsp_stack_view_pop(ui, GSP_OBJ_KEY_HUB_STACK, true);
        return true;
    }
    if (esp_gsp_page_flow_get_page(ui, GSP_OBJ_KEY_LAUNCHER_FLOW, &page) ==
            ESP_GSP_OK && page != 0) {
        (void)esp_gsp_page_flow_set_page(
            ui, GSP_OBJ_KEY_LAUNCHER_FLOW, 0, true);
        return true;
    }
    /* Select AOD or CHRG from the current power capability.  Passing false
     * here used to force AOD even when the battery update already reported
     * charging, which made Back disagree with the idle-lock path. */
    mosaic_hub_enter_lock(ui);
    return true;
}

static void mosaic_hub_idle_lock(esp_gsp_handle_t ui)
{
    (void)ui;
    mosaic_hub_lock_screen();
}

const mosaic_app_descriptor_t mosaic_hub_app = {
    .id = MOSAIC_APP_ROOT_ID,
    .launch_action = UINT16_MAX,
    .name = "mosaic-hub",
    .directory = &gsp_obj_directory_mosaic_hub,
    .disable_swipe = false,
    .on_started = mosaic_hub_started,
    .on_stopping = mosaic_hub_stopping,
    .on_root_back = mosaic_hub_root_back,
    .on_idle_lock = mosaic_hub_idle_lock,
};
