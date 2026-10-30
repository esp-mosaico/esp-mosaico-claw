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
#include "mosaico_camera_service.h"
#include "mosaico_module_mgr.h"
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
static mosaico_module_subscription_t s_module_subscription;

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

static atomic_uint s_screen_timeout_ms = 30000;
static atomic_uint s_screen_cmd;
static atomic_bool s_screen_asleep;
static atomic_bool s_ignore_wake_pointer;
static atomic_bool s_contact_pressed;
static atomic_bool s_presenter_active = true;
static atomic_bool s_camera_preview_active;
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
    if (atomic_load(&s_screen_timeout_ms) == 0U || atomic_load(&s_screen_asleep) ||
            atomic_load(&s_camera_preview_active) || atomic_load(&s_contact_pressed) ||
            !atomic_load(&s_presenter_active)) {
        return;
    }
    esp_err_t err = esp_timer_start_once(
        s_screen_idle_timer, (uint64_t)atomic_load(&s_screen_timeout_ms) * 1000U);
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
    /* A touch which woke the panel still belongs to the wake gesture. Only
     * its UP sample may release the input guard. Software/button wakes do
     * not set the guard and therefore do not consume a later, new touch. */
    atomic_store(&s_screen_asleep, false);
    screen_rearm_idle_timer();
    ESP_LOGI(TAG, "screen wake: Display On complete");
}

static void screen_recover_failed_sleep(void)
{
    /* A failed SDK pause can retain a suspended producer and its recovery
     * handle (ESP_ERR_NOT_FINISHED). Do not declare that producer awake. */
    const esp_err_t err = mosaic_loader_resume_screen();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen sleep recovery failed: %s", esp_err_to_name(err));
        (void)display_service_set_panel_enabled(false);
        return; /* Keep asleep/input guard set so a later wake retries resume. */
    }
    atomic_store(&s_screen_asleep, false);
    screen_rearm_idle_timer();
}

static void screen_apply_sleep(void)
{
    if (atomic_load(&s_screen_timeout_ms) == 0U || atomic_load(&s_screen_asleep) ||
            !display_service_panel_enabled() ||
            atomic_load(&s_camera_preview_active) || atomic_load(&s_contact_pressed) ||
            !atomic_load(&s_presenter_active)) {
        return;
    }
    (void)esp_timer_stop(s_screen_idle_timer);
    atomic_store(&s_screen_asleep, true);
    /* Close the race with a DOWN accepted immediately before this store. */
    if (atomic_load(&s_contact_pressed)) {
        atomic_store(&s_screen_asleep, false);
        screen_rearm_idle_timer();
        return;
    }
    const esp_err_t pause_err =
        mosaic_loader_pause_screen(SCREEN_PAUSE_TIMEOUT_MS);
    if (pause_err != ESP_OK) {
        ESP_LOGE(TAG, "screen sleep GSP pause failed: %s",
                 esp_err_to_name(pause_err));
        screen_recover_failed_sleep();
        return;
    }
    const esp_err_t err = display_service_set_panel_enabled(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "screen sleep Display Off failed: %s",
                 esp_err_to_name(err));
        screen_recover_failed_sleep();
        return;
    }
    ESP_LOGI(TAG, "screen sleep: Display Off complete");
}

void mosaic_ui_note_screen_activity(void)
{
    screen_post(SCREEN_CMD_REARM, false);
}

bool mosaic_ui_screen_asleep(void) { return atomic_load(&s_screen_asleep); }

void mosaic_ui_set_camera_preview_active(bool active)
{
    if (atomic_exchange(&s_camera_preview_active, active) != active) {
        screen_post(SCREEN_CMD_REARM, false);
    }
}

esp_err_t mosaic_ui_wake_screen(uint32_t timeout_ms)
{
    if (!atomic_load(&s_screen_asleep)) return ESP_OK;
    screen_post(SCREEN_CMD_WAKE, false);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (atomic_load(&s_screen_asleep) && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return atomic_load(&s_screen_asleep) ? ESP_ERR_TIMEOUT : ESP_OK;
}

void mosaic_ui_screen_wake_from_isr(void *user_ctx)
{
    (void)user_ctx;
    display_service_touch_wake_from_isr();
    if (!atomic_load(&s_screen_asleep)) {
        return;
    }
    atomic_store(&s_ignore_wake_pointer, true);
    screen_post(SCREEN_CMD_WAKE, true);
}

static bool absorb_wake_pointer(bool pressed)
{
    if (!atomic_load(&s_ignore_wake_pointer) &&
            !atomic_load(&s_screen_asleep)) {
        return false;
    }
    mosaic_ui_note_screen_activity();
    if (pressed) {
        atomic_store(&s_ignore_wake_pointer, true);
    }
    if (!pressed) {
        atomic_store(&s_ignore_wake_pointer, false);
    }
    return true;
}

bool mosaic_ui_pointer_guarded(void)
{
    return atomic_load(&s_screen_asleep) || atomic_load(&s_ignore_wake_pointer);
}

/* Called at the GSP input entry, before hit testing, gestures or observers. */
bool mosaic_ui_filter_pointer(bool pressed)
{
    if (atomic_exchange(&s_contact_pressed, pressed) != pressed) {
        mosaic_ui_note_screen_activity();
    }
    return absorb_wake_pointer(pressed);
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
    atomic_store(&s_screen_timeout_ms, timeout_ms);
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

static void on_module_insert_notice(const mosaico_module_mgr_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL) return;
    mosaic_hub_update_module(&event->info);
}

static void on_camera_availability_notice(char slot, bool available, void *user_ctx)
{
    (void)user_ctx;
    mosaic_hub_request_quick_slot_camera(slot, available);
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
    if (mosaic_ui_screen_asleep()) {
        screen_post(SCREEN_CMD_WAKE, false);
        return ESP_OK;
    }
    esp_err_t err = display_service_request_exit();
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }
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
        atomic_store(&s_presenter_active, false);
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
        atomic_store(&s_presenter_active, true);
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

    ESP_RETURN_ON_ERROR(mosaico_module_mgr_init(NULL), TAG, "initialize module manager");
    ESP_RETURN_ON_ERROR(mosaico_module_mgr_subscribe(on_module_insert_notice, NULL, &s_module_subscription), TAG,
                        "subscribe to module insertions");
    for (mosaico_module_mgr_slot_t slot = MOSAICO_MODULE_MGR_SLOT_LEFT; slot < MOSAICO_MODULE_MGR_SLOT_COUNT; ++slot) {
        mosaico_module_mgr_info_t info = {0};
        esp_err_t err = mosaico_module_mgr_get_info(slot, &info);
        if (err == ESP_OK) {
            mosaic_hub_update_module(&info);
        } else {
            ESP_LOGW(TAG, "Read initial module slot %d failed: %s", slot, esp_err_to_name(err));
        }
    }
    s_started = true;
    mosaico_camera_service_set_callback(on_camera_availability_notice, NULL);
    mosaic_hub_request_quick_slot_camera('L', mosaico_camera_service_is_available());
    ESP_LOGI(TAG, "mosaic hub live (ported esp-gsp example)");
    return ESP_OK;
}
