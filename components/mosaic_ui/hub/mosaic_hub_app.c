/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_app_catalog.h"
#include "mosaic_hub_app.h"
#include "mosaic_hub_backdrop.h"
#include "mosaic_hub_actions.h"
#include "mosaic_hub_binds.h"
#include "mosaic_hub_objects.h"
#include "mosaic_hub_templates.h"
#include "gsp_analog_clock.h"
#include "mosaic_ui.h"
#include "mosaic_settings.h"
#include "mosaic_module_notice.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "mosaic_loader.h"
#include "weather_service.h"
#endif

#define MOSAIC_CLOCK_TICK_MS        1000U
#define MOSAIC_CHARGE_LEVELS         10U
#define MOSAIC_BATTERY_APPLY_MS      200U
#define MOSAIC_WIFI_POLL_TICKS       5U   /* 5 * 200 ms ≈ 1 s RSSI refresh */
#define MOSAIC_STATUS_WIFI_LEVELS    4U
#define MOSAIC_INSERT_ENTER_MS       260U
#define MOSAIC_INSERT_BLINK_HALF_MS  110U
#define MOSAIC_INSERT_EXIT_MS        240U
#define MOSAIC_INSERT_PAUSE_MS        90U
#define MOSAIC_INSERT_APP_NAME_MAX    32U
#define MOSAIC_QUICK_TAP_SLOP          12
#define MOSAIC_QUICK_FEEDBACK_MS      2000U

static const char *TAG = "mosaic_hub";

typedef enum {
    MOSAIC_LOCK_HIDDEN = 0,
    MOSAIC_LOCK_AOD,
    MOSAIC_LOCK_CHARGING,
} mosaic_lock_mode_t;

static uint8_t s_charge_percent;
static mosaic_lock_mode_t s_lock_mode;
static esp_gsp_list_t s_notif_list = ESP_GSP_LIST_NONE;
static esp_gsp_handle_t s_notif_list_ui;
static esp_gsp_handle_t s_hub_ui;
static void* s_clock_timer;
static void* s_battery_timer;
static void* s_aod_hint_timer;
static void* s_lock_poll_timer;
static bool s_aod_hint_dim;
static bool s_pointer_down;
static bool s_quick_brightness_drag;
static bool s_quick_volume_drag;
static uint16_t s_quick_tap_action = UINT16_MAX;
static int32_t s_quick_tap_x0;
static int32_t s_quick_tap_y0;
static int32_t s_quick_level;
static bool s_quick_wlan = true;
static bool s_quick_vibration = true;
static bool s_agent_configured;
static mosaic_ui_haptic_cb_t s_haptic_callback;
static void *s_haptic_user_ctx;
static mosaic_settings_battery_t s_battery_info;
static bool s_battery_pending;
static bool s_battery_subscribed;
static mosaic_settings_network_t s_wifi_info;
static bool s_wifi_pending;
static bool s_wifi_subscribed;
static uint8_t s_wifi_poll_ticks;
static bool s_wifi_visual_valid;
static bool s_wifi_visual_show;
static uint8_t s_wifi_visual_level;
static bool s_clock_refresh_pending;
static struct {
    bool valid;
    int year, day, hour, minute;
} s_clock_text;
#if defined(ESP_PLATFORM)
static weather_service_snapshot_t s_weather_info;
static bool s_weather_pending;
static bool s_weather_seen;
static bool s_weather_subscribed;
#endif
static void *s_quick_feedback_timer;
/* Render-context state. Both sides share one sequence and at most one timer. */
static struct {
    esp_gsp_handle_t owner;
    void *timer;
    uintptr_t token;
    uint8_t phase;
    uint8_t dirty_mask;
    bool automatic;
    mosaic_module_notice_view_t cycle;
    mosaic_module_notice_view_t completed;
} s_insert_fx;
static char s_insert_open_app_name[MOSAIC_INSERT_APP_NAME_MAX];
/* Last charging sample used for edge-triggered lock follow (not continuous). */
static bool s_lock_charge_known;
static bool s_lock_was_charging;
static mosaic_module_notice_t s_module_notice;
static mosaic_module_notice_view_t s_module_notice_applied;
static bool s_module_notice_applied_valid;
static bool s_module_intro_active;
static bool s_module_notice_foreground;
static bool s_module_notice_accepting;
static bool s_module_notice_deferred;
static uint8_t s_quick_slot_pending;
static bool s_quick_slot_module[2];
static bool s_quick_slot_l_camera;
#if defined(ESP_PLATFORM)
static portMUX_TYPE s_battery_stage_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_HUB_BATT_LOCK()   portENTER_CRITICAL(&s_battery_stage_lock)
#define MOSAIC_HUB_BATT_UNLOCK() portEXIT_CRITICAL(&s_battery_stage_lock)
static portMUX_TYPE s_wifi_stage_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_HUB_WIFI_LOCK()   portENTER_CRITICAL(&s_wifi_stage_lock)
#define MOSAIC_HUB_WIFI_UNLOCK() portEXIT_CRITICAL(&s_wifi_stage_lock)
static portMUX_TYPE s_weather_stage_lock = portMUX_INITIALIZER_UNLOCKED;
#define MOSAIC_HUB_WEATHER_LOCK()   portENTER_CRITICAL(&s_weather_stage_lock)
#define MOSAIC_HUB_WEATHER_UNLOCK() portEXIT_CRITICAL(&s_weather_stage_lock)
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

static void mosaic_hub_module_notice_tick(esp_gsp_handle_t ui);
static bool mosaic_hub_defer_refresh(esp_gsp_handle_t ui);
static esp_gsp_err_t mosaic_hub_insert_schedule(uint32_t delay_ms);

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

static gsp_component_key_t mosaic_hub_insert_strip(unsigned side)
{
    return side == 0 ? GSP_OBJ_KEY_INSERT_FX_LEFT : GSP_OBJ_KEY_INSERT_FX_RIGHT;
}

static uint16_t mosaic_hub_insert_strip_visible(unsigned side)
{
    return side == 0 ? GSP_BIND_INSERT_FX_LEFT_VISIBLE : GSP_BIND_INSERT_FX_RIGHT_VISIBLE;
}

static esp_gsp_err_t mosaic_hub_insert_hide(esp_gsp_handle_t ui, unsigned side)
{
    const gsp_component_key_t strip = mosaic_hub_insert_strip(side);
    esp_gsp_err_t err = esp_gsp_component_stop_position_animation(ui, strip);
    esp_gsp_err_t next = esp_gsp_component_set_position(ui, strip, side == 0 ? -16 : 480, 80);
    if (err == ESP_GSP_OK) err = next;
    next = esp_gsp_set_visible(ui, mosaic_hub_insert_strip_visible(side), false);
    if (err == ESP_GSP_OK) err = next;
    if (err == ESP_GSP_OK) s_insert_fx.dirty_mask &= (uint8_t)~(1U << side);
    return err;
}

static esp_gsp_err_t mosaic_hub_insert_reset(esp_gsp_handle_t ui)
{
    /* Invalidate callbacks before deleting. A queued callback must never touch
     * the timer or UI belonging to a newer insertion / Hub instance. */
    ++s_insert_fx.token;
    s_insert_fx.cycle = (mosaic_module_notice_view_t) {0};
    s_insert_fx.phase = 0;
    s_insert_fx.automatic = false;
    if (ui == NULL) return ESP_GSP_OK;
    esp_gsp_err_t err = ESP_GSP_OK;
    if (s_insert_fx.timer != NULL) {
        /* Context teardown owns its remaining timers. Never submit an old
         * context's timer handle to a newly created UI. */
        if (s_insert_fx.owner == ui) err = esp_gsp_timer_delete(ui, s_insert_fx.timer);
        if (err == ESP_GSP_OK) s_insert_fx.timer = NULL;
    }
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        const esp_gsp_err_t next = mosaic_hub_insert_hide(ui, i);
        if (err == ESP_GSP_OK) err = next;
    }
    return err;
}

static void mosaic_hub_insert_finish(esp_gsp_handle_t ui)
{
    const bool automatic = s_insert_fx.automatic;
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (s_insert_fx.cycle.mask & (1U << i)) {
            s_insert_fx.completed.mask |= 1U << i;
            s_insert_fx.completed.generation[i] = s_insert_fx.cycle.generation[i];
        }
    }
    (void)mosaic_hub_insert_reset(ui);
    if (automatic) {
        if (!mosaic_hub_defer_refresh(ui)) mosaic_hub_module_notice_tick(ui);
    } else {
        (void)esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_HUB_STACK, MOSAIC_HUB_INSERT_PAGE, true);
    }
}

static void mosaic_hub_insert_timer_cb(esp_gsp_handle_t ui, void *user_ctx)
{
    if (ui != s_hub_ui || (uintptr_t)user_ctx != s_insert_fx.token ||
            s_insert_fx.timer == NULL || s_insert_fx.cycle.mask == 0) return;
    uint16_t page = 0;
    bool allowed = !mosaic_hub_lock_visible() && !mosaic_hub_quick_drawer_open(ui);
    if (s_insert_fx.automatic) {
        allowed = allowed && s_module_notice_foreground &&
            esp_gsp_stack_view_get_top(ui, GSP_OBJ_KEY_HUB_STACK, &page) == ESP_GSP_OK &&
            (page == 0 || (page == MOSAIC_HUB_INSERT_PAGE && s_module_intro_active));
    }
#if defined(ESP_PLATFORM)
    allowed = allowed && !mosaic_ui_screen_asleep();
#endif
    if (!allowed) {
        (void)mosaic_hub_quiet_module_notice(ui);
        return;
    }
    esp_gsp_err_t err = esp_gsp_timer_delete(ui, s_insert_fx.timer);
    if (err != ESP_GSP_OK) {
        mosaic_hub_insert_finish(ui);
        return;
    }
    s_insert_fx.timer = NULL;
    const uint8_t phase = s_insert_fx.phase++;
    if (phase > 3) {
        mosaic_hub_insert_finish(ui);
        return;
    }
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT && err == ESP_GSP_OK; ++i) {
        if (!(s_insert_fx.cycle.mask & (1U << i))) continue;
        if (phase == 2) {
            err = esp_gsp_component_animate_position_to(ui, mosaic_hub_insert_strip(i),
                i == 0 ? -16 : 480, 80, MOSAIC_INSERT_EXIT_MS, ESP_GSP_EASE_IN_OUT);
        } else {
            err = esp_gsp_set_visible(ui, mosaic_hub_insert_strip_visible(i), phase == 1);
        }
    }
    const uint32_t delay = phase < 2 ? MOSAIC_INSERT_BLINK_HALF_MS
        : phase == 2 ? MOSAIC_INSERT_EXIT_MS : MOSAIC_INSERT_PAUSE_MS;
    if (err == ESP_GSP_OK) err = mosaic_hub_insert_schedule(delay);
    if (err != ESP_GSP_OK) mosaic_hub_insert_finish(ui);
}

static esp_gsp_err_t mosaic_hub_insert_schedule(uint32_t delay_ms)
{
    if (++s_insert_fx.token == 0) ++s_insert_fx.token;
    s_insert_fx.owner = s_hub_ui;
    s_insert_fx.timer = esp_gsp_timer_create(s_hub_ui, delay_ms,
        mosaic_hub_insert_timer_cb, (void *)s_insert_fx.token);
    return s_insert_fx.timer != NULL ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
}

static esp_gsp_err_t mosaic_hub_insert_start(esp_gsp_handle_t ui,
    const mosaic_module_notice_view_t *view, bool automatic)
{
    esp_gsp_err_t err = mosaic_hub_insert_reset(ui);
    if (err != ESP_GSP_OK) return err;
    s_insert_fx.cycle = *view;
    s_insert_fx.automatic = automatic;
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT && err == ESP_GSP_OK; ++i) {
        if (!(view->mask & (1U << i))) continue;
        s_insert_fx.dirty_mask |= 1U << i;
        err = esp_gsp_set_visible(ui, mosaic_hub_insert_strip_visible(i), true);
        if (err == ESP_GSP_OK) err = esp_gsp_component_animate_position_to(
            ui, mosaic_hub_insert_strip(i), i == 0 ? 0 : 464, 80,
            MOSAIC_INSERT_ENTER_MS, ESP_GSP_EASE_OUT);
    }
    if (err == ESP_GSP_OK) err = mosaic_hub_insert_schedule(MOSAIC_INSERT_ENTER_MS);
    if (err == ESP_GSP_OK && automatic) {
        ESP_LOGI(TAG, "Runtime module side feedback started: slots=0x%x", (unsigned)view->mask);
    }
    return err;
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
    (void)mosaic_hub_insert_reset(s_hub_ui);
    s_insert_fx.completed = (mosaic_module_notice_view_t) {0};
    MOSAIC_HUB_INSERT_LOCK();
    s_module_intro_active = false;
    MOSAIC_HUB_INSERT_UNLOCK();
    strlcpy(s_insert_open_app_name, open_app_name,
            sizeof(s_insert_open_app_name));
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_INSERT_PORT,
        side == 'L' ? "Left port" : "Right port");
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_INSERT_BOARD_NAME, friendly_name);
    (void)esp_gsp_set_text(
        s_hub_ui, GSP_BIND_INSERT_BOARD_CAP, capability);
    const mosaic_module_notice_view_t view = {.mask = side == 'L' ? 1U : 2U};
    if (mosaic_hub_insert_start(s_hub_ui, &view, false) != ESP_GSP_OK) {
        (void)mosaic_hub_insert_reset(s_hub_ui);
        (void)esp_gsp_stack_view_push(s_hub_ui, GSP_OBJ_KEY_HUB_STACK, MOSAIC_HUB_INSERT_PAGE, true);
    }
}

void mosaic_hub_update_module(const mosaico_module_mgr_info_t *info)
{
    if (info == NULL || (unsigned)info->slot >= MOSAICO_MODULE_MGR_SLOT_COUNT) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    MOSAIC_HUB_INSERT_LOCK();
    bool allow_notice = s_module_notice_foreground && s_module_notice_accepting;
#if defined(ESP_PLATFORM)
    allow_notice = allow_notice && !mosaic_ui_screen_asleep();
#endif
    if (mosaic_module_notice_observe(&s_module_notice, info, now_us, allow_notice) &&
            info->presence != MOSAICO_MODULE_PRESENCE_UNKNOWN) {
        const bool occupied =
            info->presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
            !(info->descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
              info->eeprom.board_type == MOSAICO_BOARD_TYPE_CAMERA);
        if (s_quick_slot_module[info->slot] != occupied) {
            s_quick_slot_module[info->slot] = occupied;
            s_quick_slot_pending |= 1U << info->slot;
        }
    }
    MOSAIC_HUB_INSERT_UNLOCK();
}

esp_gsp_err_t mosaic_hub_quiet_module_notice(esp_gsp_handle_t ui)
{
    s_module_notice_deferred = false;
    MOSAIC_HUB_INSERT_LOCK();
    s_module_notice_accepting = false;
    mosaic_module_notice_quiet(&s_module_notice);
    MOSAIC_HUB_INSERT_UNLOCK();
    /* Discard transient work, but retain an already open introduction just
     * like other foreground pages across idle screen-off. */
    s_insert_fx.completed = (mosaic_module_notice_view_t) {0};
    return s_insert_fx.cycle.mask != 0 || s_insert_fx.timer != NULL || s_insert_fx.dirty_mask != 0
        ? mosaic_hub_insert_reset(ui) : ESP_GSP_OK;
}

void mosaic_hub_request_quick_slot_camera(char side, bool occupied)
{
    if (side != 'L') {
        return;
    }
    MOSAIC_HUB_INSERT_LOCK();
    s_quick_slot_l_camera = occupied;
    s_quick_slot_pending |= 1U;
    MOSAIC_HUB_INSERT_UNLOCK();
}

static void mosaic_hub_apply_quick_slot(esp_gsp_handle_t ui, uint8_t index, bool module_present, bool camera_available)
{
    if (ui == NULL || index > 1U) {
        return;
    }
    const bool camera_visible = index == 0U && camera_available;
    const bool module_visible = module_present && !camera_visible;
    const uint16_t empty_bind = index == 0U ? GSP_BIND_QUICK_SLOT_L_EMPTY_VISIBLE : GSP_BIND_QUICK_SLOT_R_EMPTY_VISIBLE;
    const uint16_t module_bind = index == 0U ? GSP_BIND_QUICK_SLOT_L_MODULE_VISIBLE : GSP_BIND_QUICK_SLOT_R_MODULE_VISIBLE;
    (void)esp_gsp_set_visible(ui, empty_bind, !camera_visible && !module_visible);
    (void)esp_gsp_set_visible(ui, module_bind, module_visible);
    if (index == 0U) {
        (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_SLOT_L_CAMERA_VISIBLE, camera_visible);
    }
}

static uint8_t mosaic_hub_take_quick_slots(bool modules[2], bool *camera_available, bool force)
{
    MOSAIC_HUB_INSERT_LOCK();
    const uint8_t pending = force ? 0x03U : s_quick_slot_pending;
    if (pending != 0U) {
        modules[0] = s_quick_slot_module[0];
        modules[1] = s_quick_slot_module[1];
        *camera_available = s_quick_slot_l_camera;
        s_quick_slot_pending &= (uint8_t)~pending;
    }
    MOSAIC_HUB_INSERT_UNLOCK();
    return pending;
}

static const char *mosaic_hub_module_name(uint8_t type)
{
    return type == MOSAICO_BOARD_TYPE_CAMERA ? "Camera" : "Interaction";
}

static const char *mosaic_hub_module_app(uint8_t type)
{
    return type == MOSAICO_BOARD_TYPE_CAMERA ? "camera" : "interact";
}

static esp_gsp_err_t mosaic_hub_apply_module_intro(esp_gsp_handle_t ui,
    const mosaic_module_notice_view_t *view)
{
    const unsigned first = (view->mask & 1U) ? 0U : 1U;
    const uint8_t type = view->board_type[first];
    const struct { uint16_t bind; const char *text; } fields[] = {
        {GSP_BIND_INSERT_PORT, first == 0 ? "Left port" : "Right port"},
        {GSP_BIND_INSERT_BOARD_NAME, mosaic_hub_module_name(type)},
        {GSP_BIND_INSERT_BOARD_CAP, type == MOSAICO_BOARD_TYPE_CAMERA
            ? "Photo / Video" : "LED / Touch / IR / Sensors"},
    };
    esp_gsp_err_t err = ESP_GSP_OK;
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]) && err == ESP_GSP_OK; ++i) {
        err = esp_gsp_set_text(ui, fields[i].bind, fields[i].text);
    }
    return err;
}

/* Return the newly inserted sides still awaiting feedback. A second arrival
 * during a sequence gets its own side animation without restarting the first;
 * deliveries then share the original page, one application at a time. */
static uint8_t mosaic_hub_insert_prepare(esp_gsp_handle_t ui,
    const mosaic_module_notice_view_t *ready)
{
    if (s_insert_fx.cycle.mask != 0 && !s_insert_fx.automatic) return ready->mask;
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        const uint8_t bit = 1U << i;
        if (!(ready->mask & bit) ||
                ready->generation[i] != s_insert_fx.completed.generation[i]) {
            s_insert_fx.completed.mask &= (uint8_t)~bit;
        }
        if ((s_insert_fx.cycle.mask & bit) && (!(ready->mask & bit) ||
                ready->generation[i] != s_insert_fx.cycle.generation[i])) {
            (void)mosaic_hub_insert_hide(ui, i);
            s_insert_fx.cycle.mask &= (uint8_t)~bit;
        }
    }
    uint8_t waiting = ready->mask & (uint8_t)~s_insert_fx.completed.mask;
    if (s_insert_fx.cycle.mask == 0) {
        if (s_insert_fx.timer != NULL || s_insert_fx.dirty_mask != 0) {
            (void)mosaic_hub_insert_reset(ui);
        }
        if (waiting != 0) {
            mosaic_module_notice_view_t todo = *ready;
            todo.mask = waiting;
            const esp_gsp_err_t err = mosaic_hub_insert_start(ui, &todo, true);
            if (err != ESP_GSP_OK) {
                ESP_LOGW(TAG, "Side feedback unavailable: 0x%x; showing introduction", (unsigned)err);
                (void)mosaic_hub_insert_reset(ui);
                for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
                    if (waiting & (1U << i)) s_insert_fx.completed.generation[i] = ready->generation[i];
                }
                s_insert_fx.completed.mask |= waiting;
                waiting = 0;
            }
        }
    }
    return waiting;
}

static void mosaic_hub_module_notice_tick(esp_gsp_handle_t ui)
{
    uint16_t page = 0;
    bool allow_notice = esp_gsp_stack_view_get_top(
            ui, GSP_OBJ_KEY_HUB_STACK, &page) == ESP_GSP_OK &&
        (page == 0 || (page == MOSAIC_HUB_INSERT_PAGE && s_module_intro_active)) &&
        !mosaic_hub_lock_visible() &&
        !mosaic_hub_quick_drawer_open(ui);
#if defined(ESP_PLATFORM)
    allow_notice = allow_notice && !mosaic_ui_screen_asleep();
    /* Initial ABSENT is intentionally silent in the manager. Hydrate unknown
     * slots from snapshots until their first stable state, then stop polling. */
    uint8_t unknown_mask = 0;
    MOSAIC_HUB_INSERT_LOCK();
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (s_module_notice.slots[i].presence == MOSAICO_MODULE_PRESENCE_UNKNOWN) {
            unknown_mask |= 1U << i;
        }
    }
    MOSAIC_HUB_INSERT_UNLOCK();
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        mosaico_module_mgr_info_t info = {0};
        if ((unknown_mask & (1U << i)) &&
                mosaico_module_mgr_get_info(i, &info) == ESP_OK) {
            mosaic_hub_update_module(&info);
        }
    }
#endif
    MOSAIC_HUB_INSERT_LOCK();
    s_module_notice_accepting = allow_notice && s_module_notice_foreground;
    const int64_t now_us = esp_timer_get_time();
    if (s_module_notice_deferred && s_module_notice_accepting &&
            s_module_notice.pending_mask != 0) {
        s_module_notice.pending_since_us = now_us;
    }
    /* Keep a second application's notice queued while the original page is
     * being read, and preserve feedback that waited for a contact to end. */
    if ((s_module_intro_active || (s_module_notice_deferred && s_module_notice_accepting)) &&
            s_module_notice.visible_mask != 0) {
        s_module_notice.visible_until_us = now_us + MOSAIC_MODULE_NOTICE_DURATION_US;
    }
    s_module_notice_deferred = false;
    mosaic_module_notice_view_t ready = mosaic_module_notice_poll(
        &s_module_notice, now_us, s_module_notice_accepting);
    mosaic_module_notice_view_t view = ready;
    if (s_module_intro_active && page == MOSAIC_HUB_INSERT_PAGE) {
        view.mask |= s_module_notice_applied.mask;
    }
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        const mosaic_module_notice_slot_t *slot = &s_module_notice.slots[i];
        if (slot->presence != MOSAICO_MODULE_PRESENCE_PRESENT || !slot->valid ||
                (slot->board_type != MOSAICO_BOARD_TYPE_CAMERA &&
                 slot->board_type != MOSAICO_BOARD_TYPE_INTERACT)) {
            view.mask &= (uint8_t)~(1U << i);
        }
        if (view.mask & (1U << i)) {
            view.board_type[i] = slot->board_type;
            view.generation[i] = slot->notice_generation;
        } else {
            view.board_type[i] = 0;
            view.generation[i] = 0;
        }
    }
    MOSAIC_HUB_INSERT_UNLOCK();
    if (!allow_notice) {
        if (s_insert_fx.cycle.mask || s_insert_fx.timer || s_insert_fx.dirty_mask) {
            (void)mosaic_hub_insert_reset(ui);
        }
        s_insert_fx.completed = (mosaic_module_notice_view_t) {0};
        return;
    }
    if (s_module_intro_active || ready.mask != 0) {
        bool animating = false;
        if (esp_gsp_stack_view_is_animating(ui, GSP_OBJ_KEY_HUB_STACK,
                &animating) != ESP_GSP_OK || animating) {
            return; /* Do not interrupt the original push/Done/Back animation. */
        }
    }
    if (s_module_intro_active && page == MOSAIC_HUB_INSERT_PAGE) {
        bool still_present = false;
        for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            const uint8_t bit = 1U << i;
            still_present |= (view.mask & s_module_notice_applied.mask & bit) != 0 &&
                view.board_type[i] == s_module_notice_applied.board_type[i] &&
                view.generation[i] == s_module_notice_applied.generation[i];
        }
        /* Removal closes the old presentation before another application's
         * Open target can be installed. Never retarget it in place. */
        if (!still_present) {
            if (esp_gsp_stack_view_pop(ui, GSP_OBJ_KEY_HUB_STACK, true) == ESP_GSP_OK) {
                MOSAIC_HUB_INSERT_LOCK();
                s_module_intro_active = false;
                s_module_notice_applied = (mosaic_module_notice_view_t) {0};
                MOSAIC_HUB_INSERT_UNLOCK();
            }
            return; /* Retain completed feedback for the queued application. */
        }
    }
    ready.mask &= view.mask;
    const uint8_t waiting = mosaic_hub_insert_prepare(ui, &ready);
    if (waiting != 0) {
        /* Keep already published entries live, but publish a batch of new
         * deliveries only after every corresponding side has finished. */
        view.mask &= s_module_intro_active
            ? s_module_notice_applied.mask & (uint8_t)~ready.mask : 0;
    }
    if (s_module_intro_active && page != MOSAIC_HUB_INSERT_PAGE) {
        MOSAIC_HUB_INSERT_LOCK();
        s_module_intro_active = false;
        s_module_notice_applied = (mosaic_module_notice_view_t) {0};
        MOSAIC_HUB_INSERT_UNLOCK();
    }
    if (view.mask != 0) {
        /* Retain the visible application until Done. Both ports with the
         * same application share one notice; a different application waits
         * for the original page to close instead of adding new UI controls. */
        uint8_t current = 0;
        if (s_module_intro_active) {
            for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
                const uint8_t bit = 1U << i;
                if ((view.mask & s_module_notice_applied.mask & bit) != 0 &&
                        view.board_type[i] == s_module_notice_applied.board_type[i] &&
                        view.generation[i] == s_module_notice_applied.generation[i]) {
                    current |= bit;
                }
            }
        }
        const unsigned first = ((current != 0 ? current : view.mask) & 1U) ? 0U : 1U;
        for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            if (view.board_type[i] != view.board_type[first]) {
                view.mask &= (uint8_t)~(1U << i);
            }
            if (!(view.mask & (1U << i))) {
                view.board_type[i] = 0;
                view.generation[i] = 0;
            }
        }
    }
    if (view.mask == 0) {
        if (s_module_intro_active && esp_gsp_stack_view_pop(
                ui, GSP_OBJ_KEY_HUB_STACK, true) == ESP_GSP_OK) {
            MOSAIC_HUB_INSERT_LOCK();
            s_module_intro_active = false;
            s_module_notice_applied = view;
            MOSAIC_HUB_INSERT_UNLOCK();
        }
        return;
    }
    esp_gsp_err_t err = ESP_GSP_OK;
    if (!s_module_intro_active || !s_module_notice_applied_valid ||
            view.mask != s_module_notice_applied.mask ||
            memcmp(view.board_type, s_module_notice_applied.board_type, sizeof(view.board_type)) != 0) {
        err = mosaic_hub_apply_module_intro(ui, &view);
    }
    const bool opening = !s_module_intro_active;
    if (err == ESP_GSP_OK && opening) {
        err = esp_gsp_stack_view_push(ui, GSP_OBJ_KEY_HUB_STACK, MOSAIC_HUB_INSERT_PAGE, true);
    }
    if (err != ESP_GSP_OK && s_module_notice_applied_valid) {
        ESP_LOGW(TAG, "Module introduction publication failed: 0x%x", (unsigned)err);
    }
    s_module_notice_applied_valid = err == ESP_GSP_OK;
    if (err == ESP_GSP_OK) {
        if (opening) ESP_LOGI(TAG, "Runtime module introduction opened: slots=0x%x", (unsigned)view.mask);
        MOSAIC_HUB_INSERT_LOCK();
        s_module_intro_active = true;
        s_module_notice_applied = view;
        mosaic_module_notice_view_t shown = ready;
        shown.mask &= view.mask;
        mosaic_module_notice_ack(&s_module_notice, &shown);
        MOSAIC_HUB_INSERT_UNLOCK();
        s_insert_fx.completed.mask &= (uint8_t)~shown.mask;
    }
}

void mosaic_ui_set_haptic_callback(
    mosaic_ui_haptic_cb_t callback, void *user_ctx)
{
    s_haptic_callback = callback;
    s_haptic_user_ctx = user_ctx;
}

esp_err_t mosaic_ui_haptic_feedback(uint32_t duration_ms)
{
    if (!s_quick_vibration || s_haptic_callback == NULL) {
        return ESP_OK;
    }
    return s_haptic_callback(s_haptic_user_ctx, duration_ms);
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

static bool mosaic_hub_agent_is_configured(
    const mosaic_settings_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }
    const bool llm_configured =
        mosaic_settings_llm_is_configured(snapshot);
    const bool im_configured = snapshot->im.wechat_configured ||
        snapshot->im.qq_configured || snapshot->im.feishu_configured ||
        snapshot->im.telegram_configured;
    return llm_configured && im_configured;
}

static void mosaic_hub_system_status_apply(
    esp_gsp_handle_t ui, const mosaic_settings_snapshot_t *snapshot)
{
    if (ui == NULL || snapshot == NULL) {
        return;
    }
    s_quick_vibration = snapshot->vibration_enabled;
    s_agent_configured = mosaic_hub_agent_is_configured(snapshot);
    /* Non-persistent slider updates apply immediately to hardware, while the
     * settings snapshot intentionally keeps the last persisted value until
     * release. Do not let the periodic status refresh pull an active drag
     * back to that stale value. */
    if (!s_quick_brightness_drag) {
        mosaic_hub_quick_level_render(
            ui, GSP_OBJ_KEY_QUICK_BRIGHTNESS_FILL,
            GSP_OBJ_KEY_QUICK_BRIGHTNESS_INPUT, snapshot->brightness);
    }
    if (!s_quick_volume_drag) {
        mosaic_hub_quick_level_render(
            ui, GSP_OBJ_KEY_QUICK_VOLUME_FILL,
            GSP_OBJ_KEY_QUICK_VOLUME_INPUT, snapshot->volume);
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

static void mosaic_hub_system_status_refresh(esp_gsp_handle_t ui)
{
    mosaic_settings_snapshot_t snapshot = {0};
    if (mosaic_settings_get_snapshot(&snapshot) == ESP_OK) {
        mosaic_hub_system_status_apply(ui, &snapshot);
    }
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

static void mosaic_hub_battery_event_cb(
    const mosaic_settings_battery_t *info, void *user_ctx)
{
    (void)user_ctx;
    if (info == NULL) {
        return;
    }
    /* Sampler/notify threads must not touch GSP; stage for the apply timer. */
    MOSAIC_HUB_BATT_LOCK();
    s_battery_info = *info;
    s_battery_pending = true;
    MOSAIC_HUB_BATT_UNLOCK();
}

static uint8_t mosaic_hub_wifi_rssi_level(
    bool connected, int8_t rssi)
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
    esp_gsp_handle_t ui, const mosaic_settings_network_t *network)
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
    if (s_wifi_visual_valid && s_wifi_visual_show == show &&
            s_wifi_visual_level == level) {
        return;
    }
    s_wifi_visual_valid = false;
    bool published = esp_gsp_set_visible(
        ui, GSP_BIND_STATUS_WIFI, show) == ESP_GSP_OK;
    published &= esp_gsp_set_visible(
        ui, GSP_BIND_QUICK_STATUS_WIFI, show) == ESP_GSP_OK;
    for (uint8_t index = 0; index <= MOSAIC_STATUS_WIFI_LEVELS; ++index) {
        published &= esp_gsp_set_visible(
            ui, bar_binds[index], show && index == level) == ESP_GSP_OK;
        published &= esp_gsp_set_visible(
            ui, quick_bar_binds[index], show && index == level) == ESP_GSP_OK;
    }
    if (published) {
        s_wifi_visual_show = show;
        s_wifi_visual_level = level;
        s_wifi_visual_valid = true;
    }
}

static void mosaic_hub_wifi_apply(
    esp_gsp_handle_t ui, const mosaic_settings_network_t *network)
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

static void mosaic_hub_wifi_event_cb(
    const mosaic_settings_network_t *info, void *user_ctx)
{
    (void)user_ctx;
    if (info == NULL) {
        return;
    }
    MOSAIC_HUB_WIFI_LOCK();
    s_wifi_info = *info;
    s_wifi_pending = true;
    MOSAIC_HUB_WIFI_UNLOCK();
}

static bool mosaic_hub_wifi_take_pending(mosaic_settings_network_t *out)
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
    mosaic_settings_network_t network = {0};
    if (mosaic_hub_wifi_take_pending(&network) ||
            mosaic_settings_get_wifi(&network) == ESP_OK) {
        mosaic_hub_wifi_apply(ui, &network);
    }
}

#if defined(ESP_PLATFORM)
static void mosaic_hub_weather_apply(
    esp_gsp_handle_t ui, const weather_service_snapshot_t *weather)
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
    char city_label[WEATHER_SERVICE_CITY_LEN + 12];
    city = weather->location_valid && weather->city[0] != '\0'
        ? weather->city : "--";
    snprintf(city_label, sizeof(city_label), "%s%s", city,
        weather->location_valid && weather->location_source == WEATHER_LOCATION_IP ? " (IP)" : "");
    city = city_label;
    if (weather->location_stale) description = "Check location";
    else if (weather->weather_valid && weather->stale) description = "Cached weather";
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HOME_WEATHER_TEMP, temperature);
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HOME_WEATHER_DESC, description);
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HOME_WEATHER_CITY, city);
}

static void mosaic_hub_weather_event_cb(
    const weather_service_snapshot_t *info, void *user_ctx)
{
    (void)user_ctx;
    if (info == NULL) {
        return;
    }
    MOSAIC_HUB_WEATHER_LOCK();
    if (!s_weather_seen || (int32_t)(info->sequence - s_weather_info.sequence) >= 0) {
        s_weather_info = *info;
        s_weather_pending = s_weather_seen = true;
    }
    MOSAIC_HUB_WEATHER_UNLOCK();
}

static bool mosaic_hub_weather_take_pending(
    weather_service_snapshot_t *out)
{
    MOSAIC_HUB_WEATHER_LOCK();
    if (!s_weather_pending) {
        MOSAIC_HUB_WEATHER_UNLOCK();
        return false;
    }
    *out = s_weather_info;
    s_weather_pending = false;
    MOSAIC_HUB_WEATHER_UNLOCK();
    return true;
}

static void mosaic_hub_weather_refresh(esp_gsp_handle_t ui)
{
    weather_service_snapshot_t weather = {0};
    if (weather_service_get_snapshot(&weather) == ESP_OK) {
        mosaic_hub_weather_apply(ui, &weather);
    }
}
#endif

static void mosaic_hub_battery_apply(esp_gsp_handle_t ui,
                                     const mosaic_settings_battery_t *battery)
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

    const uint8_t battery_percent = (uint8_t)battery->state_of_charge;
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

static bool mosaic_hub_battery_take_pending(mosaic_settings_battery_t *out)
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
    mosaic_settings_battery_t battery = {0};
    if (mosaic_hub_battery_take_pending(&battery) ||
            mosaic_settings_get_battery(&battery) == ESP_OK) {
        mosaic_hub_battery_apply(ui, &battery);
    }
}

static void mosaic_hub_charge_refresh(esp_gsp_handle_t ui)
{
#if !defined(ESP_PLATFORM)
    s_charge_percent = 0U;
    mosaic_hub_charge_apply(ui, s_charge_percent);
#else
    mosaic_settings_battery_t battery = {0};
    if (mosaic_settings_get_battery(&battery) == ESP_OK &&
            battery.available) {
        s_charge_percent = (uint8_t)battery.state_of_charge;
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
    /* Text/visibility publications can invalidate viewport snapshots even
     * when their payload is unchanged. Seconds only move the analogue hand. */
    if (!s_clock_text.valid || s_clock_text.year != local.tm_year ||
            s_clock_text.day != local.tm_yday ||
            s_clock_text.hour != hour || s_clock_text.minute != minute) {
        s_clock_text.valid = false;
        bool published = gsp_mosaic_hub_aod_clock_set_text(
            ui, aod_clock) == ESP_GSP_OK;
        char aod_date[32];
        snprintf(aod_date, sizeof(aod_date), "%d/%d · ",
                 local.tm_mon + 1, local.tm_mday);
        published &= gsp_mosaic_hub_aod_date_prefix_set_text(
            ui, aod_date) == ESP_GSP_OK;
        published &= gsp_mosaic_hub_aod_date_day_set_text(
            ui, weekdays_title[local.tm_wday]) == ESP_GSP_OK;
        published &= gsp_mosaic_hub_status_time_set_text(
            ui, status) == ESP_GSP_OK;
        published &= gsp_mosaic_hub_quick_status_time_set_text(
            ui, status) == ESP_GSP_OK;
        if (published) {
            s_clock_text.year = local.tm_year;
            s_clock_text.day = local.tm_yday;
            s_clock_text.hour = hour;
            s_clock_text.minute = minute;
            s_clock_text.valid = true;
        }
    }
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

static void mosaic_hub_lock_tick(esp_gsp_handle_t ui, void *user_ctx);

static void mosaic_hub_set_lock_mode(esp_gsp_handle_t ui,
                                     mosaic_lock_mode_t mode)
{
    if (ui == NULL || mode == s_lock_mode) {
        return;
    }
    if (mode == MOSAIC_LOCK_HIDDEN) {
        if (s_lock_poll_timer != NULL) {
            (void)esp_gsp_timer_delete(ui, s_lock_poll_timer);
            s_lock_poll_timer = NULL;
        }
        /* Called after the native close has settled; restore quick controls
         * while their shared drawer is offscreen. */
        (void)esp_gsp_drawer_close(ui, GSP_OBJ_KEY_QUICK_DRAWER, false);
        (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_VISIBLE, false);
        (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_MAIN_VISIBLE, true);
        s_lock_mode = mode;
        s_pointer_down = false;
        return;
    }

    (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_MAIN_VISIBLE, false);
    if (mode == MOSAIC_LOCK_CHARGING) {
        (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_CHARGE_VISIBLE, true);
        (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_AOD_VISIBLE, false);
        mosaic_hub_charge_refresh(ui);
    } else {
        (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_AOD_VISIBLE, true);
        (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_CHARGE_VISIBLE, false);
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_LOCK_SCREEN_VISIBLE, true);
    esp_gsp_err_t ret = esp_gsp_drawer_open(ui, GSP_OBJ_KEY_QUICK_DRAWER, false);
    if (ret != ESP_GSP_OK) {
        ESP_LOGE(TAG, "Open lock drawer failed: %d", (int)ret);
    }
    s_lock_mode = mode;
    s_pointer_down = false;
    s_quick_brightness_drag = false;
    s_quick_volume_drag = false;
    s_quick_tap_action = UINT16_MAX;
    if (s_lock_poll_timer == NULL) {
        s_lock_poll_timer = esp_gsp_timer_create(ui, 16, mosaic_hub_lock_tick, NULL);
        if (s_lock_poll_timer == NULL) {
            ESP_LOGE(TAG, "Create lock completion timer failed");
        }
    }
}

static void mosaic_hub_lock_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    bool open = true;
    if (mosaic_hub_lock_visible() &&
        esp_gsp_drawer_is_open(ui, GSP_OBJ_KEY_QUICK_DRAWER, &open) == ESP_GSP_OK && !open) {
        mosaic_hub_set_lock_mode(ui, MOSAIC_LOCK_HIDDEN);
    }
}

static void mosaic_hub_enter_lock(esp_gsp_handle_t ui)
{
    mosaic_settings_battery_t battery = {0};
    const bool charging =
        mosaic_settings_get_battery(&battery) == ESP_OK &&
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
        if (s_hub_ui == NULL || esp_gsp_stack_view_get_top(
                s_hub_ui, GSP_OBJ_KEY_HUB_STACK, &page) != ESP_GSP_OK ||
                page != MOSAIC_HUB_INSERT_PAGE) {
            return true;
        }
        const char *app_name = s_insert_open_app_name;
        MOSAIC_HUB_INSERT_LOCK();
        const bool automatic = s_module_intro_active;
        const mosaic_module_notice_view_t view = s_module_notice_applied;
        MOSAIC_HUB_INSERT_UNLOCK();
        if (automatic) {
            const unsigned first = (view.mask & 1U) ? 0U : 1U;
            const uint8_t type = view.board_type[first];
            unsigned side = 0;
            while (side < MOSAICO_MODULE_MGR_SLOT_COUNT &&
                    (!(view.mask & (1U << side)) || view.board_type[side] != type)) ++side;
            if (side == MOSAICO_MODULE_MGR_SLOT_COUNT) return true;
            MOSAIC_HUB_INSERT_LOCK();
            const mosaic_module_notice_slot_t slot = s_module_notice.slots[side];
            MOSAIC_HUB_INSERT_UNLOCK();
            if (slot.presence != MOSAICO_MODULE_PRESENCE_PRESENT || !slot.valid ||
                    slot.board_type != type) return true;
#if defined(ESP_PLATFORM)
            mosaico_module_mgr_info_t info = {0};
            if (mosaico_module_mgr_get_info(side, &info) != ESP_OK ||
                    info.presence != MOSAICO_MODULE_PRESENCE_PRESENT ||
                    info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID ||
                    info.eeprom.board_type != type) return true;
#endif
            app_name = mosaic_hub_module_app(type);
        }
        (void)esp_gsp_stack_view_pop(s_hub_ui, GSP_OBJ_KEY_HUB_STACK, false);
#if defined(ESP_PLATFORM)
        const mosaic_app_descriptor_t *app =
            mosaic_app_descriptor_for_name(app_name);
        if (app != NULL) {
            (void)mosaic_loader_request(app);
        }
#else
        (void)app_name;
#endif
        return true;
    }
    if (action_id == GSP_ACT_ID_QUICK_WLAN_TOGGLE) {
        const bool next = !s_quick_wlan;
        s_quick_wlan = next;
        mosaic_hub_quick_render(s_hub_ui);
        if (mosaic_settings_set_wifi_enabled(next) != ESP_OK) {
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
        const esp_err_t err = mosaic_settings_set_vibration(next);
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

static uint16_t mosaic_hub_quick_action_at(int32_t x, int32_t y)
{
    static const struct {
        int16_t x;
        int16_t y;
        uint16_t action;
    } buttons[] = {
        { 34,  62, GSP_ACT_ID_QUICK_WLAN_TOGGLE },
        { 138, 62, GSP_ACT_ID_QUICK_JOIN_TOGGLE },
        { 34, 154, GSP_ACT_ID_QUICK_BLUETOOTH_TOGGLE },
        { 138, 154, GSP_ACT_ID_QUICK_LOW_POWER_TOGGLE },
        { 34, 246, GSP_ACT_ID_QUICK_RINGTONE_TOGGLE },
        { 138, 246, GSP_ACT_ID_QUICK_VIBRATION_TOGGLE },
    };
    for (size_t index = 0; index < sizeof(buttons) / sizeof(buttons[0]);
         ++index) {
        if (x >= buttons[index].x && x < buttons[index].x + 72 &&
            y >= buttons[index].y && y < buttons[index].y + 72) {
            return buttons[index].action;
        }
    }
    return UINT16_MAX;
}

static bool mosaic_hub_intercept_pointer(
    esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *user_ctx)
{
    (void)user_ctx;
    if (mosaic_hub_lock_visible()) {
        mosaic_hub_backdrop_contact(ui,pressed,false);
        /* Native Drawer owns the entire lock viewport, its gesture kinetics,
         * and snapshot composition. Do not consume its pointer samples. */
        s_pointer_down = pressed;
        return false;
    }
    bool drawer_open = false;
    bool prepare_backdrop=false;
    if (pressed && !s_pointer_down && y<=24) {
        esp_gsp_component_motion_t drawer={0}, launcher={0};
        uint16_t page=0;
        prepare_backdrop=esp_gsp_component_get_motion(ui,GSP_OBJ_KEY_QUICK_DRAWER,&drawer)==ESP_GSP_OK &&
            !drawer.dragging && !drawer.settling && drawer.value==0 &&
            esp_gsp_component_get_motion(ui,GSP_OBJ_KEY_LAUNCHER_FLOW,&launcher)==ESP_GSP_OK &&
            !launcher.dragging && !launcher.settling &&
            esp_gsp_stack_view_get_top(ui,GSP_OBJ_KEY_HUB_STACK,&page)==ESP_GSP_OK && page==0;
    }
    mosaic_hub_backdrop_contact(ui,pressed,prepare_backdrop);
    if (pressed && !s_pointer_down) {
        if (esp_gsp_drawer_is_open(ui, GSP_OBJ_KEY_QUICK_DRAWER,
                                   &drawer_open) != ESP_GSP_OK) {
            drawer_open = false;
        }
        if (drawer_open && y >= 298 && y <= 450) {
            s_quick_volume_drag = x >= 270 && x < 342;
            s_quick_brightness_drag = x >= 374 && x < 446;
        }
        if (drawer_open && !s_quick_volume_drag &&
            !s_quick_brightness_drag) {
            s_quick_tap_action = mosaic_hub_quick_action_at(x, y);
            if (s_quick_tap_action != UINT16_MAX) {
                s_quick_tap_x0 = x;
                s_quick_tap_y0 = y;
            }
        }
    }

    if (s_quick_tap_action != UINT16_MAX) {
        const int32_t dx = x - s_quick_tap_x0;
        const int32_t dy = y - s_quick_tap_y0;
        if (dx < -MOSAIC_QUICK_TAP_SLOP || dx > MOSAIC_QUICK_TAP_SLOP ||
            dy < -MOSAIC_QUICK_TAP_SLOP || dy > MOSAIC_QUICK_TAP_SLOP) {
            s_quick_tap_action = UINT16_MAX;
            s_pointer_down = pressed;
            return true;
        }
        if (!pressed) {
            const uint16_t action = s_quick_tap_action;
            s_quick_tap_action = UINT16_MAX;
            s_pointer_down = false;
            if (action != GSP_ACT_ID_QUICK_VIBRATION_TOGGLE ||
                    s_quick_vibration) {
                (void)mosaic_ui_haptic_feedback(25U);
            }
            (void)mosaic_hub_handle_action(action);
            return true;
        }
        s_pointer_down = true;
        return true;
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
            (void)mosaic_settings_set_brightness(s_quick_level, false);
        } else {
            mosaic_hub_quick_level_render(
                ui, GSP_OBJ_KEY_QUICK_VOLUME_FILL,
                GSP_OBJ_KEY_QUICK_VOLUME_INPUT, s_quick_level);
            (void)mosaic_settings_set_volume(s_quick_level, false);
        }
    }
    if (!pressed && s_pointer_down) {
        if (s_quick_brightness_drag) {
            (void)mosaic_settings_set_brightness(s_quick_level, true);
        } else if (s_quick_volume_drag) {
            (void)mosaic_settings_set_volume(s_quick_level, true);
            mosaic_hub_quick_render(ui);
        }
        s_quick_brightness_drag = false;
        s_quick_volume_drag = false;
    }
    s_pointer_down = pressed;

    return false;
}

static bool mosaic_hub_defer_refresh(esp_gsp_handle_t ui)
{
    if (s_pointer_down) {
        return true;
    }
    esp_gsp_component_motion_t motion = {0};
    return esp_gsp_component_get_motion(
        ui, GSP_OBJ_KEY_QUICK_DRAWER, &motion) == ESP_GSP_OK &&
        (motion.dragging || motion.settling);
}

static void mosaic_hub_clock_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    if (mosaic_hub_defer_refresh(ui)) {
        s_clock_refresh_pending = true;
        return;
    }
    s_clock_refresh_pending = false;
    mosaic_hub_clock_apply(ui);
}

static void mosaic_hub_battery_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    /* Keep render-task service reads and snapshot-invalidating publications
     * out of a drag/settle. The latest pending model samples remain staged;
     * battery sampling and shutdown policy run independently of this UI timer. */
    if (mosaic_hub_defer_refresh(ui)) {
        /* An otherwise eligible insertion waits for this contact to end;
         * renew it under the existing model lock only after motion ends. */
        s_module_notice_deferred = true;
        return;
    }
    if (s_clock_refresh_pending) {
        s_clock_refresh_pending = false;
        mosaic_hub_clock_apply(ui);
    }
    mosaic_hub_module_notice_tick(ui);
    bool slot_modules[2] = {false, false};
    bool camera_available = false;
    const uint8_t slot_pending = mosaic_hub_take_quick_slots(slot_modules, &camera_available, false);
    for (uint8_t i = 0; i < 2U; ++i) {
        if (slot_pending & (1U << i)) {
            mosaic_hub_apply_quick_slot(ui, i, slot_modules[i], camera_available);
        }
    }
    mosaic_settings_battery_t battery = {0};
    if (mosaic_hub_battery_take_pending(&battery)) {
        mosaic_hub_battery_apply(ui, &battery);
    }
    mosaic_settings_network_t network = {0};
    if (mosaic_hub_wifi_take_pending(&network)) {
        s_wifi_poll_ticks = 0;
        mosaic_hub_wifi_apply(ui, &network);
    } else if (++s_wifi_poll_ticks >= MOSAIC_WIFI_POLL_TICKS) {
        /* RSSI moves without Wi-Fi state events; refresh on a slow cadence. */
        s_wifi_poll_ticks = 0;
        if (mosaic_settings_get_wifi(&network) == ESP_OK) {
            mosaic_hub_wifi_apply(ui, &network);
        }
        mosaic_hub_system_status_refresh(ui);
    }
#if defined(ESP_PLATFORM)
    weather_service_snapshot_t weather = {0};
    if (mosaic_hub_weather_take_pending(&weather)) {
        mosaic_hub_weather_apply(ui, &weather);
    }
#endif
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
    if (s_lock_poll_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_lock_poll_timer);
        s_lock_poll_timer = NULL;
    }
    if (s_quick_feedback_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_quick_feedback_timer);
        s_quick_feedback_timer = NULL;
    }
    (void)esp_gsp_set_visible(ui, GSP_BIND_QUICK_FEEDBACK_VISIBLE, false);
    (void)mosaic_hub_insert_reset(ui);
    s_insert_fx.completed = (mosaic_module_notice_view_t) {0};
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

static void mosaic_hub_started(esp_gsp_handle_t ui)
{
    s_hub_ui = ui;
    s_module_notice_deferred = false;
    MOSAIC_HUB_INSERT_LOCK();
    s_module_notice_foreground = true;
    s_module_notice_accepting = false;
    mosaic_module_notice_quiet(&s_module_notice);
    /* The SDK retains Hub component/bind state across child Apps. Keep an
     * open introduction's identity with it; the apply tick revalidates its
     * presence, or clears the identity if navigation already left the page. */
    if (!s_module_intro_active) {
        s_module_notice_applied = (mosaic_module_notice_view_t) {0};
    }
    MOSAIC_HUB_INSERT_UNLOCK();
    s_module_notice_applied_valid = true;
    (void)mosaic_hub_insert_reset(ui);
    s_insert_fx.completed = (mosaic_module_notice_view_t) {0};
    s_charge_percent = 0;
    s_pointer_down = false;
    s_quick_brightness_drag = false;
    s_quick_volume_drag = false;
    s_quick_tap_action = UINT16_MAX;
    s_lock_mode = MOSAIC_LOCK_HIDDEN;
    s_lock_charge_known = false;
    s_lock_was_charging = false;
    s_battery_pending = false;
    s_wifi_pending = false;
    s_wifi_visual_valid = false;
    s_clock_text.valid = false;
    s_clock_refresh_pending = false;
#if defined(ESP_PLATFORM)
    s_weather_pending = false;
    (void)esp_gsp_set_visible(ui, GSP_BIND_AOD_MODE_SWITCH_VISIBLE, false);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CHARGE_MODE_SWITCH_VISIBLE, false);
#else
    (void)esp_gsp_set_visible(ui, GSP_BIND_AOD_MODE_SWITCH_VISIBLE, true);
    (void)esp_gsp_set_visible(ui, GSP_BIND_CHARGE_MODE_SWITCH_VISIBLE, true);
#endif
    mosaic_hub_clock_apply(ui);
    if (!s_battery_subscribed) {
        if (mosaic_settings_subscribe_battery(
                mosaic_hub_battery_event_cb, NULL) == ESP_OK) {
            s_battery_subscribed = true;
        }
    }
    if (!s_wifi_subscribed) {
        if (mosaic_settings_subscribe_wifi(
                mosaic_hub_wifi_event_cb, NULL) == ESP_OK) {
            s_wifi_subscribed = true;
        }
    }
#if defined(ESP_PLATFORM)
    if (!s_weather_subscribed) {
        if (weather_service_subscribe(
                mosaic_hub_weather_event_cb, NULL) == ESP_OK) {
            s_weather_subscribed = true;
        }
    }
#endif
    mosaic_hub_battery_refresh(ui);
    mosaic_hub_wifi_refresh(ui);
    mosaic_hub_system_status_refresh(ui);
#if defined(ESP_PLATFORM)
    mosaic_hub_weather_refresh(ui);
#endif
    mosaic_hub_notif_bind(ui);
    mosaic_hub_quick_render(ui);
    bool slot_modules[2] = {false, false};
    bool camera_available = false;
    (void)mosaic_hub_take_quick_slots(slot_modules, &camera_available, true);
    for (uint8_t i = 0; i < 2U; ++i) {
        mosaic_hub_apply_quick_slot(ui, i, slot_modules[i], camera_available);
    }
    (void)esp_gsp_set_input_interceptor(
        ui, mosaic_hub_intercept_pointer, NULL);
    mosaic_hub_start_timers(ui);
    mosaic_hub_backdrop_start(ui);
}

static void mosaic_hub_stopping(esp_gsp_handle_t ui)
{
    MOSAIC_HUB_INSERT_LOCK();
    s_module_notice_foreground = false;
    s_module_notice_accepting = false;
    mosaic_module_notice_quiet(&s_module_notice);
    MOSAIC_HUB_INSERT_UNLOCK();
    s_module_notice_applied_valid = false;
    mosaic_hub_stop_timers(ui);
    mosaic_hub_backdrop_stop(ui);
    if (s_battery_subscribed) {
        (void)mosaic_settings_unsubscribe_battery(
            mosaic_hub_battery_event_cb, NULL);
        s_battery_subscribed = false;
    }
    if (s_wifi_subscribed) {
        (void)mosaic_settings_unsubscribe_wifi(
            mosaic_hub_wifi_event_cb, NULL);
        s_wifi_subscribed = false;
    }
#if defined(ESP_PLATFORM)
    if (s_weather_subscribed) {
        (void)weather_service_unsubscribe(
            mosaic_hub_weather_event_cb, NULL);
        s_weather_subscribed = false;
    }
#endif
    (void)esp_gsp_set_input_interceptor(ui, NULL, NULL);
    if (s_hub_ui == ui) {
        s_hub_ui = NULL;
    }
}

const mosaic_app_descriptor_t mosaic_hub_app = {
    .id = MOSAIC_APP_ROOT_ID,
    .launch_action = UINT16_MAX,
    .name = "mosaic-hub",
    .directory = &gsp_obj_directory_mosaic_hub,
    .image_cache_bytes = 2U * 1024U * 1024U, /* Hub images decode to about 1.32 MiB. */
    .disable_swipe = false,
    .on_started = mosaic_hub_started,
    .on_stopping = mosaic_hub_stopping,
};
