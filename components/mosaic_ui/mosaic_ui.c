/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_ui.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_check.h"
#include "display_service.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hot_plug_register.h"
#include "mosaic_welcome.h"
#include "mosaic_hub_actions.h"
#include "mosaic_hub_app.h"
#include "mosaic_loader.h"
#include "mosaic_logic.h"
#include "mosaic_settings.h"
#include "mosaic_system.h"
#include "mosaic_system_flow.h"
#include "mosaic_ai_create_runtime.h"

static const char *TAG = "mosaic_ui";

#define SCREEN_CMD_SLEEP  (1U << 0)
#define SCREEN_CMD_WAKE   (1U << 1)
#define SCREEN_CMD_REARM  (1U << 2)
#define SCREEN_PAUSE_TIMEOUT_MS 1500U
#define SCREEN_POWER_TASK_STACK 4096U

static bool s_started;
static const char s_present_producer;
static mosaic_system_flow_t s_system_flow;
static bool s_system_flow_ready;
static bool s_battery_notice_subscribed;
static bool s_low_battery_notice_issued;
static bool s_critical_battery_notice_issued;

#define MOSAIC_BATTERY_LOW_NOTICE_SOC 10U
#define MOSAIC_BATTERY_CRITICAL_NOTICE_SOC 2U
#define MOSAIC_BATTERY_LOW_NOTICE_MS 2500U
#define MOSAIC_BATTERY_CRITICAL_NOTICE_MS 1500U

static void on_battery_notice(
    const mosaic_settings_battery_t *battery, void *user_ctx)
{
    (void)user_ctx;
    if (battery == NULL || !battery->available) {
        return;
    }
    if (battery->state_of_charge >= MOSAIC_BATTERY_LOW_NOTICE_SOC) {
        s_low_battery_notice_issued = false;
    }
    if (battery->charging ||
            battery->state_of_charge >= MOSAIC_BATTERY_CRITICAL_NOTICE_SOC) {
        s_critical_battery_notice_issued = false;
    }
    if (battery->charging) {
        return;
    }
    if (battery->state_of_charge < MOSAIC_BATTERY_CRITICAL_NOTICE_SOC) {
        if (!s_critical_battery_notice_issued &&
                mosaic_loader_show_system_notice(
                    MOSAIC_SYSTEM_NOTICE_BATTERY_CRITICAL,
                    MOSAIC_BATTERY_CRITICAL_NOTICE_MS) == ESP_OK) {
            s_critical_battery_notice_issued = true;
            s_low_battery_notice_issued = true;
        }
        return;
    }
    if (battery->state_of_charge < MOSAIC_BATTERY_LOW_NOTICE_SOC &&
            !s_low_battery_notice_issued &&
            mosaic_loader_show_system_notice(
                MOSAIC_SYSTEM_NOTICE_BATTERY_LOW,
                MOSAIC_BATTERY_LOW_NOTICE_MS) == ESP_OK) {
        s_low_battery_notice_issued = true;
    }
}

static uint32_t s_screen_timeout_ms = 30000;
static atomic_uint s_screen_cmd;
static atomic_bool s_screen_asleep;
static atomic_bool s_ignore_wake_pointer;
static atomic_bool s_hub_foreground;
static atomic_bool s_hub_presenter_active = true;
static TaskHandle_t s_screen_task;
static esp_timer_handle_t s_screen_idle_timer;

static void screen_post(uint32_t cmd, bool from_isr)
{
    if (s_screen_task == NULL) {
        return;
    }
    atomic_fetch_or(&s_screen_cmd, cmd);
    if (from_isr) {
        BaseType_t task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_screen_task, &task_woken);
        if (task_woken == pdTRUE) {
            portYIELD_FROM_ISR();
        }
        return;
    }
    xTaskNotifyGive(s_screen_task);
}

static void screen_rearm_idle_timer(void)
{
    if (s_screen_idle_timer == NULL) {
        return;
    }
    (void)esp_timer_stop(s_screen_idle_timer);
    if (s_screen_timeout_ms == 0U || atomic_load(&s_screen_asleep) ||
            !atomic_load(&s_hub_foreground) ||
            !atomic_load(&s_hub_presenter_active)) {
        return;
    }
    esp_err_t err = esp_timer_start_once(
        s_screen_idle_timer, (uint64_t)s_screen_timeout_ms * 1000U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "arm screen idle timer failed: %s", esp_err_to_name(err));
    }
}

static void screen_idle_timer_cb(void *arg)
{
    (void)arg;
    screen_post(SCREEN_CMD_SLEEP, false);
}

static void screen_apply_wake(void)
{
    if (!atomic_load(&s_screen_asleep) && display_service_panel_enabled()) {
        screen_rearm_idle_timer();
        return;
    }
    const esp_err_t err = display_service_set_panel_enabled(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen wake Display On failed: %s", esp_err_to_name(err));
        return;
    }
    /* CO5300 restarts TE after Display On. Resume GSP only after that. */
    vTaskDelay(pdMS_TO_TICKS(20));
    const esp_err_t resume_err = mosaic_loader_resume_screen();
    if (resume_err != ESP_OK) {
        ESP_LOGE(TAG, "screen wake GSP resume failed: %s",
                 esp_err_to_name(resume_err));
        (void)display_service_set_panel_enabled(false);
        return;
    }
    atomic_store(&s_ignore_wake_pointer, false);
    atomic_store(&s_screen_asleep, false);
    screen_rearm_idle_timer();
    ESP_LOGI(TAG, "screen wake: Display On complete");
}

static void screen_apply_sleep(void)
{
    if (s_screen_timeout_ms == 0U || atomic_load(&s_screen_asleep) ||
            !display_service_panel_enabled() ||
            !atomic_load(&s_hub_foreground) ||
            !atomic_load(&s_hub_presenter_active)) {
        return;
    }
    (void)esp_timer_stop(s_screen_idle_timer);
    atomic_store(&s_screen_asleep, true);
    atomic_store(&s_ignore_wake_pointer, true);
    const esp_err_t pause_err =
        mosaic_loader_lock_and_pause_hub(SCREEN_PAUSE_TIMEOUT_MS);
    if (pause_err != ESP_OK) {
        ESP_LOGE(TAG, "screen sleep GSP pause failed: %s",
                 esp_err_to_name(pause_err));
        atomic_store(&s_screen_asleep, false);
        atomic_store(&s_ignore_wake_pointer, false);
        screen_rearm_idle_timer();
        return;
    }
    const esp_err_t err = display_service_set_panel_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen sleep Display Off failed: %s",
                 esp_err_to_name(err));
        (void)mosaic_loader_resume_screen();
        atomic_store(&s_screen_asleep, false);
        atomic_store(&s_ignore_wake_pointer, false);
        screen_rearm_idle_timer();
        return;
    }
    ESP_LOGI(TAG, "screen sleep: Display Off complete");
}

void mosaic_ui_note_screen_activity(void)
{
    screen_post(SCREEN_CMD_REARM, false);
}

void mosaic_ui_set_hub_foreground(bool foreground)
{
    atomic_store(&s_hub_foreground, foreground);
    screen_post(SCREEN_CMD_REARM, false);
}

void mosaic_ui_screen_wake_from_isr(void *user_ctx)
{
    (void)user_ctx;
    display_service_touch_wake_from_isr();
    if (!atomic_load(&s_screen_asleep)) {
        return;
    }
    screen_post(SCREEN_CMD_WAKE, true);
}

bool mosaic_ui_absorb_wake_pointer(bool pressed)
{
    if (!atomic_load(&s_ignore_wake_pointer) &&
            !atomic_load(&s_screen_asleep)) {
        return false;
    }
    mosaic_ui_note_screen_activity();
    if (!pressed) {
        atomic_store(&s_ignore_wake_pointer, false);
    }
    return true;
}

static void screen_power_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t cmd = atomic_exchange(&s_screen_cmd, 0U);
        /* Wake wins over a stale idle-timer SLEEP posted in the same batch. */
        if ((cmd & (SCREEN_CMD_WAKE | SCREEN_CMD_REARM)) != 0U) {
            screen_apply_wake();
        } else if ((cmd & SCREEN_CMD_SLEEP) != 0U) {
            screen_apply_sleep();
        }
    }
}

void mosaic_ui_set_screen_timeout(uint32_t timeout_ms)
{
    s_screen_timeout_ms = timeout_ms;
    screen_post(SCREEN_CMD_REARM, false);
}

esp_err_t mosaic_system_configure(const mosaic_system_ops_t *ops)
{
    if (ops == NULL || ops->get_boot_stage == NULL ||
            ops->set_boot_stage == NULL || s_started) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = mosaic_system_flow_init(&s_system_flow, ops);
    if (err == ESP_OK) {
        s_system_flow_ready = true;
    }
    return err;
}

static void on_subboard_insert_notice(
    const char *subboard_name, char slot, bool present, void *user_ctx)
{
    (void)user_ctx;
    if (subboard_name == NULL ||
            strcmp(subboard_name, HOT_PLUG_SUBBOARD_CAMERA_NAME) != 0) {
        return;
    }
    mosaic_hub_request_quick_slot_camera(slot, present);
    if (!present) {
        return;
    }
    if (mosaic_loader_app() != mosaic_app_root()) {
        return;
    }
    mosaic_hub_request_board_insert(
        slot == 'R' ? 'R' : 'L', "Camera", "Photo / Video", "camera");
}

esp_err_t mosaic_ui_set_ai_create_asr(asr_service_handle_t asr)
{
    return mosaic_ai_create_runtime_set_asr(asr);
}

esp_err_t mosaic_ui_set_ai_create_voice_status(ai_create_voice_status_t status)
{
    return mosaic_ai_create_runtime_set_voice_status(status);
}

esp_err_t mosaic_welcome_open(void)
{
    const mosaic_app_descriptor_t *app =
        mosaic_app_descriptor_for_name("welcome");
    if (app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return mosaic_loader_request(app);
}

esp_err_t mosaic_ui_back(void)
{
    return mosaic_loader_request_back();
}

esp_err_t mosaic_ui_open_ai_create(void)
{
    const mosaic_app_descriptor_t *app =
        mosaic_app_descriptor_for_name("ai_create");
    if (app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return mosaic_loader_request(app);
}

static esp_err_t present_quiesce(void *ctx, uint32_t timeout_ms)
{
    (void)ctx;
    const esp_err_t err = mosaic_loader_quiesce(timeout_ms);
    if (err == ESP_OK) {
        atomic_store(&s_hub_presenter_active, false);
        screen_post(SCREEN_CMD_REARM, false);
    }
    return err;
}

static esp_err_t present_activate(
    void *ctx, esp_display_presenter_t *presenter, uint32_t generation)
{
    (void)ctx;
    if (!display_service_presenter_validate(
            &s_present_producer, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = mosaic_loader_activate(presenter, generation);
    if (err == ESP_OK) {
        atomic_store(&s_hub_presenter_active, true);
        screen_post(SCREEN_CMD_REARM, false);
    }
    return err;
}

static const display_service_present_producer_ops_t s_present_ops = {
    .quiesce = present_quiesce,
    .activate = present_activate,
};

static void on_loader_event(esp_gsp_handle_t ui,
    const mosaic_app_descriptor_t *app, const esp_gsp_event_t *ev,
    void *user_ctx)
{
    (void)user_ctx;
    (void)ui;
    if (app != NULL && ev != NULL && ev->type == ESP_GSP_EVENT_CALL) {
        if (ev->action_id == app->back_action) {
            bool handled = false;
            const char *next_app = NULL;
            esp_err_t err = mosaic_system_flow_handle_exit(
                &s_system_flow, app->name, &handled, &next_app);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "advance system flow failed: %s",
                         esp_err_to_name(err));
            } else if (handled && next_app != NULL &&
                       strcmp(next_app, "mosaic-hub") != 0) {
                const mosaic_app_descriptor_t *next =
                    mosaic_app_descriptor_for_name(next_app);
                if (next == NULL || mosaic_loader_request(next) != ESP_OK) {
                    ESP_LOGE(TAG, "open system App %s failed", next_app);
                }
            }
        }
    }
    /* GSP action ids are local to each App bundle. Stub launchers belong to
     * the Hub, so never interpret a child App's same-numbered action here. */
    if (app != mosaic_app_root() || ev == NULL) {
        return;
    }
    if (ev->type != ESP_GSP_EVENT_CALL) {
        return;
    }
    if (mosaic_hub_handle_action(ev->action_id)) {
        return;
    }
    if (ev->action_id == 0
        || mosaic_app_descriptor_for_action(ev->action_id) != NULL) {
        return;
    }
    ESP_LOGI(TAG, "call scene=%u action=%u arg=%lu",
             (unsigned)ev->scene_id, (unsigned)ev->action_id,
             (unsigned long)ev->arg);
}

esp_err_t mosaic_ui_start(void)
{
    ESP_LOGI(TAG, "mosaic_ui_start");
    if (s_started) {
        ESP_LOGI(TAG, "mosaic_ui_start already started");
        return ESP_OK;
    }

    if (!s_system_flow_ready) {
        ESP_RETURN_ON_ERROR(mosaic_system_flow_init(&s_system_flow, NULL),
                            TAG, "init volatile system flow");
        s_system_flow_ready = true;
    }

    ESP_RETURN_ON_ERROR(mosaic_ai_create_runtime_init(), TAG,
                        "init AI Create runtime failed");

    esp_display_presenter_t *presenter = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    uint32_t producer_generation = 0;
    const display_service_present_producer_t producer = {
        .identity = &s_present_producer,
        .ops = &s_present_ops,
    };
    ESP_RETURN_ON_ERROR(
        display_service_presenter_start_baseline(
            &producer, &presenter, &touch, &producer_generation),
        TAG, "start baseline presenter failed");

    const mosaic_loader_config_t loader_config = {
        .presenter = presenter,
        .render_alignment = {
            .x_pixels = 4,
            .y_pixels = 4,
            .width_pixels = 4,
            .height_pixels = 4,
        },
        .touch = touch,
        .producer_generation = producer_generation,
        .on_event = on_loader_event,
    };
    ESP_RETURN_ON_ERROR(mosaic_loader_init(&loader_config), TAG, "mosaic_loader_init failed");
    const esp_timer_create_args_t idle_timer_args = {
        .callback = screen_idle_timer_cb,
        .name = "screen_idle",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&idle_timer_args, &s_screen_idle_timer),
                        TAG, "create screen idle timer");
    if (xTaskCreate(screen_power_task, "screen_power",
                    SCREEN_POWER_TASK_STACK, NULL, 4,
                    &s_screen_task) != pdPASS) {
        s_screen_task = NULL;
        (void)esp_timer_delete(s_screen_idle_timer);
        s_screen_idle_timer = NULL;
        return ESP_ERR_NO_MEM;
    }
    screen_rearm_idle_timer();
    ESP_RETURN_ON_ERROR(mosaic_loader_start_hub(), TAG, "mosaic_loader_start_hub failed");
    if (!s_battery_notice_subscribed) {
        ESP_RETURN_ON_ERROR(mosaic_settings_subscribe_battery(
                                on_battery_notice, NULL),
                            TAG, "subscribe to battery notices");
        s_battery_notice_subscribed = true;
    }

    const char *initial_app = NULL;
    ESP_RETURN_ON_ERROR(mosaic_system_flow_initial_app(
                            &s_system_flow, &initial_app),
                        TAG, "resolve initial system App");
    if (strcmp(initial_app, "mosaic-hub") != 0) {
        const mosaic_app_descriptor_t *app =
            mosaic_app_descriptor_for_name(initial_app);
        ESP_RETURN_ON_FALSE(app != NULL, ESP_ERR_NOT_FOUND, TAG,
                            "initial app %s missing", initial_app);
        ESP_RETURN_ON_ERROR(mosaic_loader_request(app), TAG,
                            "open initial app %s", initial_app);
    }

    s_started = true;
    hot_plug_register_set_insert_notice_callback(
        on_subboard_insert_notice, NULL);
    ESP_LOGI(TAG, "mosaic hub live (ported esp-gsp example)");
    return ESP_OK;
}
