/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_service.h"
#include "display_service_internal.h"
#include "display_present_lease.h"
#include "display_service_target.h"
#include "display_service_present.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "display_service";

#define DISPLAY_SERVICE_MAX_SESSIONS 1
#define DISPLAY_SERVICE_BRIGHTNESS_FADE_STEPS 15U

typedef struct {
    esp_lcd_touch_handle_t touch;
    display_service_touch_sample_t touch_sample;
    display_service_touch_observer_cb_t touch_observer_cb;
    void *touch_observer_user_ctx;
    display_service_state_observer_cb_t state_observer_cb;
    void *state_observer_user_ctx;
    struct display_service_session_t sessions[DISPLAY_SERVICE_MAX_SESSIONS];
    esp_display_presenter_t *presenter;
    display_present_lease_t present_lease;
    display_service_present_producer_t baseline_producer;
    /*
     * The at-most-one non-baseline producer. The lease (present_lease) is the
     * sole authority for which identity is active; the active producer triple
     * is derived from it via display_service_active_producer(). Never treat
     * this field as "the active producer" on its own.
     */
    display_service_present_producer_t exclusive_producer;
    SemaphoreHandle_t present_handoff_mutex;
    SemaphoreHandle_t touch_observer_mutex;
    uint32_t width;
    uint32_t height;
    uint8_t brightness_percent;
    uint16_t rotation_degrees;
    bool initial_brightness_configured;
    bool initial_brightness_fade_configured;
    uint8_t initial_brightness_fade_start;
    uint32_t initial_brightness_fade_duration_ms;
    bool initial_rotation_configured;
    bool panel_enabled;
    display_service_control_provider_t control_provider;
    lv_obj_t *default_screen;
} display_service_state_t;

EXT_RAM_BSS_ATTR static display_service_state_t s_display;
static display_service_brightness_provider_t s_brightness_provider;
static void *s_brightness_provider_ctx;

static esp_err_t display_service_apply_brightness(uint8_t percent);

static esp_err_t display_service_apply_initial_brightness(uint8_t target)
{
    if (!s_display.initial_brightness_fade_configured) {
        return display_service_apply_brightness(target);
    }

    const int start = s_display.initial_brightness_fade_start;
    const int delta = (int)target - start;
    if (delta == 0) {
        s_display.initial_brightness_fade_configured = false;
        return display_service_apply_brightness(target);
    }
    const uint32_t duration_ms =
        s_display.initial_brightness_fade_duration_ms;
    const TickType_t step_delay = pdMS_TO_TICKS(
        duration_ms /
        DISPLAY_SERVICE_BRIGHTNESS_FADE_STEPS);
    for (uint32_t step = 1;
         step <= DISPLAY_SERVICE_BRIGHTNESS_FADE_STEPS; ++step) {
        const int value = start +
            (delta * (int)step) /
                (int)DISPLAY_SERVICE_BRIGHTNESS_FADE_STEPS;
        ESP_RETURN_ON_ERROR(
            display_service_apply_brightness((uint8_t)value), TAG,
            "apply startup brightness fade");
        if (step < DISPLAY_SERVICE_BRIGHTNESS_FADE_STEPS && step_delay > 0) {
            vTaskDelay(step_delay);
        }
    }
    s_display.initial_brightness_fade_configured = false;
    ESP_LOGI(TAG, "startup brightness fade: %d%% -> %u%% in %" PRIu32 " ms",
             start, target, duration_ms);
    return ESP_OK;
}

esp_display_presenter_t *display_service_presenter_internal(void)
{
    return s_display.presenter;
}
esp_lcd_touch_handle_t display_service_touch_internal(void)
{
    return s_display.touch;
}

void IRAM_ATTR display_service_touch_wake_from_isr(void)
{
    struct display_service_session_t *session = &s_display.sessions[0];
    if (!session->active || !session->touch_irq_active) {
        return;
    }
    ++session->touch_irq_sequence;
    TaskHandle_t task = session->raw_touch_task;
    if (task != NULL) {
        BaseType_t higher_priority_woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &higher_priority_woken);
        portYIELD_FROM_ISR(higher_priority_woken);
    }
}

esp_err_t display_service_touch_forward_start_internal(
    struct display_service_session_t *session)
{
    ESP_RETURN_ON_FALSE(
        session != NULL && display_service_touch_internal() != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "touch IRQ arguments missing");
    /* GSP permanently owns the hardware IRQ and forwards its external wake
     * callback through display_service_touch_wake_from_isr(). */
    session->touch_irq_sequence = 1; /* Prime one read for an already-held touch. */
    session->touch_irq_consumed = 0;
    session->touch_cached_count = 0;
    session->touch_irq_active = true;
    return ESP_OK;
}

void display_service_touch_forward_stop_internal(
    struct display_service_session_t *session)
{
    if (session != NULL) {
        session->touch_irq_active = false;
    }
}

esp_err_t display_service_map_touch_internal(int32_t *x, int32_t *y)
{
    ESP_RETURN_ON_FALSE(x && y, ESP_ERR_INVALID_ARG, TAG,
                        "touch coordinates are NULL");
    ESP_RETURN_ON_FALSE(s_display.presenter, ESP_ERR_INVALID_STATE, TAG,
                        "presenter is unavailable");
    return esp_display_presenter_map_point(s_display.presenter, x, y);
}

/*
 * Resolve the currently active producer triple from the lease authority.
 * Returns NULL only if the lease has never been activated. The lease keeps
 * active_producer pointing at the last activated identity even in HANDOFF and
 * FAULT states, which is exactly the producer recovery must quiesce.
 */
static const display_service_present_producer_t *
display_service_active_producer(void)
{
    const void *id = s_display.present_lease.active_producer;
    if (id == NULL) {
        return NULL;
    }
    if (id == s_display.exclusive_producer.identity) {
        return &s_display.exclusive_producer;
    }
    if (id == s_display.baseline_producer.identity) {
        return &s_display.baseline_producer;
    }
    return NULL;
}

static void display_service_clear_exclusive_producer(void)
{
    memset(&s_display.exclusive_producer, 0,
           sizeof(s_display.exclusive_producer));
}

static bool display_service_session_slot_contains(const struct display_service_session_t *session)
{
    return session >= &s_display.sessions[0] &&
           session < &s_display.sessions[DISPLAY_SERVICE_MAX_SESSIONS];
}

bool display_service_session_valid_internal(display_service_session_handle_t session)
{
    return session != NULL && display_service_session_slot_contains(session) && session->active;
}

bool display_service_process_exit_gesture_internal(
    struct display_service_session_t *session,
    const display_service_touch_sample_t *sample,
    int32_t display_height)
{
    if (!display_service_session_valid_internal(session) || sample == NULL ||
            display_height <= 0) {
        return false;
    }

    if (sample->pressed && !session->exit_gesture_tracking) {
        session->exit_gesture_tracking = true;
        session->exit_gesture_captured = false;
        session->exit_request_sent = false;
        session->exit_gesture_start_y = sample->y;
    }
    if (sample->pressed && session->exit_gesture_tracking &&
            session->exit_gesture_start_y >=
                display_height - DISPLAY_SERVICE_EXIT_GESTURE_START_HEIGHT &&
            session->exit_gesture_start_y - sample->y >=
                DISPLAY_SERVICE_EXIT_GESTURE_MIN_DY) {
        session->exit_gesture_captured = true;
    }

    const bool captured = session->exit_gesture_captured;
    if (captured && !session->exit_request_sent &&
            session->exit_request_cb != NULL) {
        session->exit_request_sent = true;
        ESP_LOGI(TAG, "display shell exit gesture: owner=%s",
                 session->owner_name);
        session->exit_request_cb(session, session->cleanup_user_ctx);
    }
    if (!sample->pressed) {
        session->exit_gesture_tracking = false;
        session->exit_gesture_captured = false;
    }
    return captured;
}

esp_err_t display_service_session_alloc_internal(
    display_service_mode_t mode, const display_service_session_config_t *config,
    const char *owner_name, struct display_service_session_t **out_session)
{
    ESP_RETURN_ON_FALSE(config && owner_name && out_session,
                        ESP_ERR_INVALID_ARG, TAG,
                        "session allocation arguments missing");
    *out_session = NULL;
    ESP_RETURN_ON_FALSE(s_display.present_handoff_mutex,
                        ESP_ERR_INVALID_STATE, TAG,
                        "session allocator unavailable");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_display.present_handoff_mutex,
                       pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "session allocation busy");
    struct display_service_session_t *session = &s_display.sessions[0];
    if (session->active) {
        ESP_LOGW(TAG, "exclusive display already owned by %s",
                 session->owner_name);
        xSemaphoreGive(s_display.present_handoff_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memset(session, 0, sizeof(*session));
    session->active = true;
    session->mode = mode;
    session->flags = config->flags;
    session->cleanup_cb = config->cleanup_cb;
    session->exit_request_cb = config->exit_request_cb;
    session->cleanup_user_ctx = config->user_ctx;
    strlcpy(session->owner_name, owner_name, sizeof(session->owner_name));
    xSemaphoreGive(s_display.present_handoff_mutex);
    *out_session = session;
    return ESP_OK;
}

esp_err_t display_service_session_free_internal(
    struct display_service_session_t *session)
{
    ESP_RETURN_ON_FALSE(
        session && display_service_session_slot_contains(session) &&
        s_display.present_handoff_mutex,
        ESP_ERR_INVALID_ARG, TAG, "invalid session release");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_display.present_handoff_mutex,
                       pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "session release busy");
    if (!session->active) {
        xSemaphoreGive(s_display.present_handoff_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    SemaphoreHandle_t raw_io_mutex = session->raw_io_mutex;
    SemaphoreHandle_t raw_render_mutex = session->raw_render_mutex;
    memset(session, 0, sizeof(*session));
    xSemaphoreGive(s_display.present_handoff_mutex);
    if (raw_io_mutex != NULL) {
        vSemaphoreDelete(raw_io_mutex);
    }
    if (raw_render_mutex != NULL) {
        vSemaphoreDelete(raw_render_mutex);
    }
    return ESP_OK;
}

esp_err_t display_service_session_begin_close_internal(
    struct display_service_session_t *session)
{
    ESP_RETURN_ON_FALSE(
        session && display_service_session_slot_contains(session) &&
        s_display.present_handoff_mutex,
        ESP_ERR_INVALID_ARG, TAG, "invalid session close");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_display.present_handoff_mutex,
                       pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "session close busy");
    if (!session->active || session->closing) {
        xSemaphoreGive(s_display.present_handoff_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    session->closing = true;
    xSemaphoreGive(s_display.present_handoff_mutex);
    return ESP_OK;
}

void display_service_session_abort_close_internal(
    struct display_service_session_t *session)
{
    if (session == NULL || !display_service_session_slot_contains(session) ||
            s_display.present_handoff_mutex == NULL ||
            xSemaphoreTake(s_display.present_handoff_mutex,
                           pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (session->active) {
        session->closing = false;
    }
    xSemaphoreGive(s_display.present_handoff_mutex);
}

display_service_session_cleanup_cb_t
display_service_session_take_cleanup_internal(
    struct display_service_session_t *session, void **out_user_ctx)
{
    if (out_user_ctx != NULL) {
        *out_user_ctx = NULL;
    }
    if (session == NULL || out_user_ctx == NULL ||
            !display_service_session_slot_contains(session) ||
            s_display.present_handoff_mutex == NULL ||
            xSemaphoreTake(s_display.present_handoff_mutex,
                           pdMS_TO_TICKS(1000)) != pdTRUE) {
        return NULL;
    }
    display_service_session_cleanup_cb_t cleanup_cb = NULL;
    if (session->active && session->closing) {
        cleanup_cb = session->cleanup_cb;
        *out_user_ctx = session->cleanup_user_ctx;
        session->cleanup_cb = NULL;
    }
    xSemaphoreGive(s_display.present_handoff_mutex);
    return cleanup_cb;
}

void display_service_notify_touch_internal(const display_service_touch_sample_t *sample)
{
    display_service_touch_observer_cb_t cb = NULL;
    void *user_ctx = NULL;

    if (sample == NULL) {
        return;
    }
    if (s_display.touch_observer_mutex == NULL ||
            xSemaphoreTake(s_display.touch_observer_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    s_display.touch_sample = *sample;
    cb = s_display.touch_observer_cb;
    user_ctx = s_display.touch_observer_user_ctx;
    xSemaphoreGive(s_display.touch_observer_mutex);
    if (cb != NULL) {
        cb(sample, user_ctx);
    }
}

/* Tear down the freshly created presenter after a failed baseline bring-up. */
static void display_service_presenter_teardown(void)
{
    (void)esp_display_presenter_stop(s_display.presenter);
    (void)esp_display_presenter_delete(s_display.presenter);
    s_display.presenter = NULL;
}

esp_err_t display_service_presenter_start_baseline(
    const display_service_present_producer_t *producer,
    esp_display_presenter_t **out_presenter,
    esp_lcd_touch_handle_t *out_touch,
    uint32_t *out_generation)
{
    ESP_RETURN_ON_FALSE(producer && producer->identity && producer->ops &&
                        producer->ops->quiesce && producer->ops->activate &&
                        out_presenter && out_touch && out_generation,
                        ESP_ERR_INVALID_ARG, TAG, "baseline presenter arguments missing");
    *out_presenter = NULL;
    *out_touch = NULL;
    *out_generation = 0;

    if (s_display.present_handoff_mutex == NULL) {
        s_display.present_handoff_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(
            s_display.present_handoff_mutex != NULL,
            ESP_ERR_NO_MEM, TAG, "create presenter handoff mutex");
    }
    if (s_display.touch_observer_mutex == NULL) {
        s_display.touch_observer_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(
            s_display.touch_observer_mutex != NULL,
            ESP_ERR_NO_MEM, TAG, "create touch observer mutex");
    }

    if (s_display.presenter == NULL) {
        dev_display_lcd_config_t *lcd_cfg = NULL;
        dev_display_lcd_handles_t *lcd_handles = NULL;
        esp_display_present_target_config_t target = {0};
        ESP_RETURN_ON_ERROR(
            display_service_target_load_display(&lcd_cfg, &lcd_handles),
            TAG, "load baseline display");
        s_display.touch = display_service_target_load_touch();
        ESP_RETURN_ON_ERROR(
            display_service_target_build(lcd_cfg, lcd_handles, &target),
            TAG, "build present target");
        const uint8_t initial_brightness =
            s_display.initial_brightness_configured
                ? s_display.brightness_percent : 100;
        const uint16_t initial_rotation =
            s_display.initial_rotation_configured
                ? s_display.rotation_degrees : 0;
        target.hw.rotation =
            (esp_display_present_rotation_t)initial_rotation;
        s_display.width = lcd_cfg->lcd_width;
        s_display.height = lcd_cfg->lcd_height;
        s_display.brightness_percent = initial_brightness;
        s_display.rotation_degrees = initial_rotation;
        s_display.panel_enabled = true;
        /* Apply panel brightness before presenter creation. At this point the
         * LCD IO exists, but no render producer can submit SPI transactions. */
        if (s_display.initial_brightness_configured) {
            ESP_RETURN_ON_ERROR(
                display_service_apply_initial_brightness(initial_brightness),
                TAG, "apply initial display brightness");
        }
        const esp_display_presenter_config_t config = {
            .width = lcd_cfg->lcd_width,
            .height = lcd_cfg->lcd_height,
            .pixel_format = target.hw.input_pixel_format,
            .max_damage_areas = 32,
            .target = target,
        };
        ESP_RETURN_ON_ERROR(
            esp_display_presenter_create(&config, &s_display.presenter),
            TAG, "create service presenter");
        if (s_display.initial_brightness_configured) {
            ESP_LOGI(TAG,
                     "initial display settings: rotation=%u brightness=%u%%",
                     initial_rotation, initial_brightness);
        } else {
            ESP_LOGI(TAG,
                     "initial display settings: rotation=%u brightness retained",
                     initial_rotation);
        }
        uint32_t configured_lines = 0;
        esp_err_t lines_ret = display_service_present_buffer_lines(
            s_display.presenter, &configured_lines);
        if (lines_ret != ESP_OK) {
            display_service_presenter_teardown();
            return lines_ret;
        }
        if (configured_lines != DISPLAY_SERVICE_PRESENT_DRAWBUF_LINES) {
            ESP_LOGW(TAG,
                     "present drawbuf lines=%" PRIu32 " target=%u",
                     configured_lines, DISPLAY_SERVICE_PRESENT_DRAWBUF_LINES);
        }
        display_present_lease_init(&s_display.present_lease);
        esp_err_t ret = display_present_lease_activate(
            &s_display.present_lease, producer->identity, out_generation);
        if (ret != ESP_OK) {
            display_service_presenter_teardown();
            return ret;
        }
        s_display.baseline_producer = *producer;
    } else {
        ESP_RETURN_ON_FALSE(
            s_display.present_lease.active_producer == producer->identity &&
            s_display.present_lease.state == DISPLAY_PRESENT_LEASE_ACTIVE,
            ESP_ERR_INVALID_STATE, TAG, "baseline producer already owned");
        *out_generation = s_display.present_lease.generation;
    }

    *out_presenter = s_display.presenter;
    *out_touch = s_display.touch;
    return ESP_OK;
}

/*
 * Distinguish a recoverable lease fault from a terminal presenter fault.
 * A lease fault only means the software handoff state machine tripped; the
 * presenter can still be driven back to baseline. A presenter FAULTED state
 * is terminal ("never reopens in place"), so quiesce/activate can never
 * succeed and must not be attempted in a recovery loop.
 */
static bool display_service_presenter_terminally_faulted(void)
{
    esp_display_presenter_state_t state = ESP_DISPLAY_PRESENTER_STATE_OPEN;
    if (esp_display_presenter_get_state(s_display.presenter, &state) != ESP_OK) {
        return false;
    }
    if (state != ESP_DISPLAY_PRESENTER_STATE_FAULTED) {
        return false;
    }
    esp_display_present_fault_reason_t reason = ESP_DISPLAY_PRESENT_FAULT_NONE;
    (void)esp_display_presenter_get_fault_reason(s_display.presenter, &reason);
    ESP_LOGE(TAG,
             "presenter terminally FAULTED (reason=%d); in-place baseline "
             "recovery is impossible, presenter must be recreated",
             (int)reason);
    return true;
}

static esp_err_t display_service_recover_baseline(uint32_t timeout_ms)
{
    uint32_t generation = 0;

    ESP_LOGE(TAG, "present lease fault: forcing baseline recovery");
    if (display_service_presenter_terminally_faulted()) {
        display_present_lease_fault(&s_display.present_lease);
        return ESP_ERR_INVALID_STATE;
    }
    const display_service_present_producer_t *faulted =
        display_service_active_producer();
    if (faulted != NULL &&
            faulted->identity != s_display.baseline_producer.identity &&
            faulted->ops != NULL && faulted->ops->quiesce != NULL) {
        esp_err_t quiesce_ret = faulted->ops->quiesce(
            faulted->ctx, timeout_ms);
        if (quiesce_ret != ESP_OK) {
            ESP_LOGE(TAG, "faulted producer quiesce failed: %s",
                     esp_err_to_name(quiesce_ret));
        }
    }
    esp_err_t ret = esp_display_presenter_quiesce(
        s_display.presenter, timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "baseline recovery quiesce failed: %s",
                 esp_err_to_name(ret));
        display_present_lease_fault(&s_display.present_lease);
        return ret;
    }
    /*
     * Same two-phase rule as handoff: activate/render against a reserved
     * generation while authority is still pending, then commit. Activate
     * failure must fault again — abort_handoff would incorrectly return to
     * ACTIVE with a stale producer after a FAULT recovery attempt.
     */
    ret = display_present_lease_begin_recover(
        &s_display.present_lease, s_display.baseline_producer.identity);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "baseline recovery begin failed: %s",
                 esp_err_to_name(ret));
        display_present_lease_fault(&s_display.present_lease);
        return ret;
    }
    const uint32_t pending_generation =
        display_present_lease_pending_generation(&s_display.present_lease);
    ret = s_display.baseline_producer.ops->activate(
        s_display.baseline_producer.ctx, s_display.presenter,
        pending_generation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "baseline recovery activate failed: %s",
                 esp_err_to_name(ret));
        display_present_lease_fault(&s_display.present_lease);
        return ret;
    }
    ret = display_present_lease_commit_handoff(
        &s_display.present_lease, s_display.baseline_producer.identity,
        &generation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "baseline recovery commit failed: %s",
                 esp_err_to_name(ret));
        display_present_lease_fault(&s_display.present_lease);
        return ret;
    }
    display_service_clear_exclusive_producer();
    ESP_LOGW(TAG, "present baseline recovered: generation=%" PRIu32,
             generation);
    return ESP_OK;
}

static esp_err_t display_service_handoff(
    const display_service_present_producer_t *next,
    uint32_t timeout_ms,
    uint32_t *out_generation)
{
    const display_service_present_producer_t *previous_p =
        display_service_active_producer();
    if (previous_p == NULL) {
        ESP_LOGE(TAG, "present handoff has no active producer");
        return ESP_ERR_INVALID_STATE;
    }
    display_service_present_producer_t previous = *previous_p;
    const bool next_is_baseline =
        next->identity == s_display.baseline_producer.identity;
    uint32_t previous_generation = s_display.present_lease.generation;
    ESP_LOGI(TAG, "present handoff begin: previous=%p next=%p generation=%" PRIu32,
             previous.identity, next->identity, previous_generation);
    ESP_RETURN_ON_ERROR(
        display_present_lease_begin_handoff(
            &s_display.present_lease, previous.identity,
            previous_generation, next->identity),
        TAG, "begin presenter handoff");

    esp_err_t ret = previous.ops->quiesce(previous.ctx, timeout_ms);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "present handoff quiesce failed: previous=%p timeout_ms=%" PRIu32
                 " err=%s",
                 previous.identity, timeout_ms, esp_err_to_name(ret));
        display_present_lease_abort_handoff(&s_display.present_lease);
        return ret;
    }
    ESP_LOGI(TAG, "present handoff quiesced: previous=%p",
             previous.identity);

    /*
     * Two-phase activation. The incoming producer activates and renders its
     * first frame while the lease is still HANDOFF, validated against the
     * generation reserved at begin_handoff. Only after that succeeds do we
     * commit the lease authority to next, so the lease never advertises a
     * producer that has not rendered (closing the old commit-before-activate
     * window). The exclusive-producer identity is likewise published only on
     * success, keeping the rollback path from having to unwind it.
     */
    const uint32_t pending_generation =
        display_present_lease_pending_generation(&s_display.present_lease);
    ret = next->ops->activate(
        next->ctx, s_display.presenter, pending_generation);
    if (ret == ESP_OK) {
        esp_err_t commit_ret = display_present_lease_commit_handoff(
            &s_display.present_lease, next->identity, out_generation);
        if (commit_ret == ESP_OK) {
            if (next_is_baseline) {
                display_service_clear_exclusive_producer();
            } else {
                s_display.exclusive_producer = *next;
            }
            ESP_LOGI(TAG,
                     "present handoff complete: active=%p generation=%" PRIu32,
                     next->identity, *out_generation);
            return ESP_OK;
        }
        /* Unreachable while holding the handoff mutex, but never leave next
         * half-owned: fall through and roll back to previous. */
        ESP_LOGE(TAG, "present handoff commit rejected: next=%p err=%s",
                 next->identity, esp_err_to_name(commit_ret));
        ret = commit_ret;
    } else {
        ESP_LOGE(TAG, "present handoff activate failed: next=%p err=%s",
                 next->identity, esp_err_to_name(ret));
    }

    /*
     * Roll back to previous. next is quiesced/torn down, the lease is aborted
     * back to previous at its original generation, and previous is
     * re-activated against it. That re-activation is the only fallible step
     * left; if it fails the lease faults and baseline recovery takes over.
     */
    esp_err_t failed_quiesce = next->ops->quiesce(next->ctx, timeout_ms);
    if (failed_quiesce != ESP_OK) {
        ESP_LOGW(TAG,
                 "failed producer cleanup did not quiesce: next=%p err=%s",
                 next->identity, esp_err_to_name(failed_quiesce));
    }
    display_present_lease_abort_handoff(&s_display.present_lease);
    esp_err_t restore_ret = previous.ops->activate(
        previous.ctx, s_display.presenter, previous_generation);
    if (restore_ret != ESP_OK) {
        ESP_LOGE(TAG, "restore previous producer failed: previous=%p err=%s",
                 previous.identity, esp_err_to_name(restore_ret));
        display_present_lease_fault(&s_display.present_lease);
        (void)display_service_recover_baseline(timeout_ms);
    }
    return ret;
}

esp_err_t display_service_presenter_acquire_internal(
    const display_service_present_producer_t *producer,
    uint32_t timeout_ms,
    uint32_t *out_generation)
{
    ESP_RETURN_ON_FALSE(
        producer && producer->identity && producer->ops &&
        producer->ops->quiesce && producer->ops->activate &&
        out_generation && timeout_ms != 0 && s_display.presenter,
        ESP_ERR_INVALID_ARG, TAG, "presenter acquire arguments missing");
    ESP_RETURN_ON_FALSE(
        producer->identity != s_display.baseline_producer.identity,
        ESP_ERR_INVALID_ARG, TAG, "baseline cannot acquire exclusively");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(
            s_display.present_handoff_mutex,
            pdMS_TO_TICKS(timeout_ms)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "presenter handoff busy");
    if (s_display.present_lease.state == DISPLAY_PRESENT_LEASE_FAULT) {
        esp_err_t recovery_ret = display_service_recover_baseline(timeout_ms);
        if (recovery_ret != ESP_OK) {
            xSemaphoreGive(s_display.present_handoff_mutex);
            return recovery_ret;
        }
    }
    if (s_display.present_lease.active_producer !=
            s_display.baseline_producer.identity ||
            !display_present_lease_validate(
                &s_display.present_lease,
                s_display.baseline_producer.identity,
                s_display.present_lease.generation)) {
        xSemaphoreGive(s_display.present_handoff_mutex);
        ESP_LOGW(TAG, "exclusive presenter already active: owner=%p",
                 s_display.present_lease.active_producer);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret =
        display_service_handoff(producer, timeout_ms, out_generation);
    xSemaphoreGive(s_display.present_handoff_mutex);
    return ret;
}

esp_err_t display_service_session_acquire_producer_internal(
    struct display_service_session_t *session,
    const display_service_present_producer_ops_t *ops,
    display_service_session_handle_t *ret_session)
{
    const display_service_present_producer_t producer = {
        .identity = session,
        .ops = ops,
        .ctx = session,
    };
    uint32_t generation = 0;
    esp_err_t ret = display_service_presenter_acquire_internal(
        &producer, DISPLAY_SERVICE_PRESENT_HANDOFF_TIMEOUT_MS, &generation);
    if (ret != ESP_OK) {
        (void)display_service_session_free_internal(session);
        return ret;
    }
    session->producer_generation = generation;
    *ret_session = session;
    return ESP_OK;
}

static esp_err_t display_service_presenter_release(
    const void *producer,
    uint32_t generation,
    uint32_t timeout_ms,
    uint32_t *out_baseline_generation)
{
    ESP_RETURN_ON_FALSE(
        producer && out_baseline_generation && timeout_ms != 0,
        ESP_ERR_INVALID_ARG, TAG, "presenter release arguments missing");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(
            s_display.present_handoff_mutex,
            pdMS_TO_TICKS(timeout_ms)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "presenter handoff busy");
    if (s_display.present_lease.state == DISPLAY_PRESENT_LEASE_FAULT) {
        esp_err_t recovery_ret = display_service_recover_baseline(timeout_ms);
        if (recovery_ret != ESP_OK) {
            xSemaphoreGive(s_display.present_handoff_mutex);
            return recovery_ret;
        }
    }
    if (s_display.present_lease.active_producer ==
            s_display.baseline_producer.identity &&
            display_present_lease_validate(
                &s_display.present_lease,
                s_display.baseline_producer.identity,
                s_display.present_lease.generation)) {
        *out_baseline_generation = s_display.present_lease.generation;
        xSemaphoreGive(s_display.present_handoff_mutex);
        return ESP_OK;
    }
    if (!display_present_lease_validate(
            &s_display.present_lease, producer, generation) ||
            s_display.present_lease.active_producer != producer ||
            s_display.baseline_producer.identity == NULL) {
        xSemaphoreGive(s_display.present_handoff_mutex);
        ESP_LOGE(TAG, "presenter release lease mismatch");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = display_service_handoff(
        &s_display.baseline_producer, timeout_ms,
        out_baseline_generation);
    xSemaphoreGive(s_display.present_handoff_mutex);
    return ret;
}

bool display_service_presenter_validate(
    const void *producer,
    uint32_t generation)
{
    return display_present_lease_validate(
        &s_display.present_lease, producer, generation);
}

bool display_service_is_started(void)
{
    return s_display.presenter != NULL;
}

uint32_t display_service_width(void)
{
    return s_display.width;
}

uint32_t display_service_height(void)
{
    return s_display.height;
}

esp_err_t display_service_set_brightness(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(percent <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "brightness must be 0..100");
    if (!display_service_is_started()) {
        s_display.brightness_percent = percent;
        s_display.initial_brightness_configured = true;
        s_display.initial_brightness_fade_configured = false;
        return ESP_OK;
    }
    return display_service_apply_brightness(percent);
}

esp_err_t display_service_prepare_brightness_fade(
    uint8_t start_percent, uint8_t target_percent, uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(start_percent <= 100 && target_percent <= 100 &&
                            duration_ms > 0,
                        ESP_ERR_INVALID_ARG, TAG,
                        "invalid startup brightness fade");
    ESP_RETURN_ON_FALSE(!display_service_is_started(), ESP_ERR_INVALID_STATE,
                        TAG, "display is already started");
    s_display.brightness_percent = target_percent;
    s_display.initial_brightness_configured = true;
    s_display.initial_brightness_fade_configured = true;
    s_display.initial_brightness_fade_start = start_percent;
    s_display.initial_brightness_fade_duration_ms = duration_ms;
    return ESP_OK;
}

static esp_err_t display_service_apply_brightness(uint8_t percent)
{
    dev_display_lcd_handles_t *lcd_handles = NULL;

    if (s_display.control_provider.set_brightness != NULL) {
        ESP_RETURN_ON_ERROR(
            s_display.control_provider.set_brightness(
                percent, s_display.control_provider.user_ctx),
            TAG, "product brightness provider failed");
        s_display.brightness_percent = percent;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        display_service_target_load_display(NULL, &lcd_handles), TAG,
        "load display for brightness control");
    ESP_RETURN_ON_FALSE(lcd_handles && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG,
                        "display panel is unavailable");
    esp_err_t err = s_brightness_provider != NULL
        ? s_brightness_provider(lcd_handles->panel_handle, percent,
                                s_brightness_provider_ctx)
        : esp_lcd_panel_set_brightness(
            lcd_handles->panel_handle, ((int)percent * 255) / 100);
    ESP_RETURN_ON_ERROR(err, TAG, "set display brightness");
    s_display.brightness_percent = percent;
    return ESP_OK;
}

void display_service_set_brightness_provider(
    display_service_brightness_provider_t provider, void *user_ctx)
{
    s_brightness_provider = provider;
    s_brightness_provider_ctx = user_ctx;
}

esp_err_t display_service_get_brightness(uint8_t *percent)
{
    ESP_RETURN_ON_FALSE(percent, ESP_ERR_INVALID_ARG, TAG,
                        "brightness output is NULL");
    ESP_RETURN_ON_FALSE(display_service_is_started(), ESP_ERR_INVALID_STATE,
                        TAG, "display is not started");
    if (s_display.control_provider.get_brightness != NULL) {
        return s_display.control_provider.get_brightness(
            percent, s_display.control_provider.user_ctx);
    }
    *percent = s_display.brightness_percent;
    return ESP_OK;
}

esp_err_t display_service_set_panel_enabled(bool enabled)
{
    dev_display_lcd_handles_t *lcd_handles = NULL;

    ESP_RETURN_ON_FALSE(display_service_is_started(), ESP_ERR_INVALID_STATE,
                        TAG, "display is not started");
    if (enabled == s_display.panel_enabled) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        display_service_target_load_display(NULL, &lcd_handles), TAG,
        "load display for panel power control");
    ESP_RETURN_ON_FALSE(lcd_handles && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG,
                        "display panel is unavailable");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(lcd_handles->panel_handle, enabled), TAG,
        "switch display panel %s", enabled ? "on" : "off");
    s_display.panel_enabled = enabled;
    return ESP_OK;
}

bool display_service_panel_enabled(void)
{
    return display_service_is_started() && s_display.panel_enabled;
}

esp_err_t display_service_set_rotation(uint16_t degrees)
{
    dev_display_lcd_config_t *lcd_cfg = NULL;
    dev_display_lcd_handles_t *lcd_handles = NULL;
    const uint16_t previous_degrees = s_display.rotation_degrees;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(degrees == 0 || degrees == 90 || degrees == 180 ||
                        degrees == 270, ESP_ERR_INVALID_ARG, TAG,
                        "rotation must be 0, 90, 180, or 270");
    if (!display_service_is_started()) {
        s_display.rotation_degrees = degrees;
        s_display.initial_rotation_configured = true;
        return ESP_OK;
    }
    if (s_display.control_provider.set_rotation != NULL) {
        ESP_RETURN_ON_ERROR(
            s_display.control_provider.set_rotation(
                degrees, s_display.control_provider.user_ctx),
            TAG, "product rotation provider failed");
        s_display.rotation_degrees = degrees;
        return ESP_OK;
    }
    if (degrees == s_display.rotation_degrees) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(
        display_service_target_load_display(&lcd_cfg, &lcd_handles), TAG,
        "load display for rotation control");
    ESP_RETURN_ON_FALSE(lcd_cfg && lcd_handles && lcd_handles->panel_handle,
                        ESP_ERR_INVALID_STATE, TAG,
                        "display panel is unavailable");
    ESP_RETURN_ON_FALSE(lcd_cfg->lcd_width == lcd_cfg->lcd_height,
                        ESP_ERR_NOT_SUPPORTED, TAG,
                        "runtime rotation currently requires a square panel");

    ESP_RETURN_ON_FALSE(
        s_display.present_handoff_mutex != NULL &&
        xSemaphoreTake(s_display.present_handoff_mutex,
                       pdMS_TO_TICKS(2000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "display rotation busy");
    const display_service_present_producer_t *active =
        display_service_active_producer();
    if (active == NULL) {
        xSemaphoreGive(s_display.present_handoff_mutex);
        ESP_LOGE(TAG, "rotation has no active producer");
        return ESP_ERR_INVALID_STATE;
    }
    ret = active->ops->quiesce(active->ctx, 2000);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_display.present_handoff_mutex);
        return ret;
    }

    ret = esp_display_presenter_set_rotation(
        s_display.presenter, (esp_display_present_rotation_t)degrees);
    esp_err_t activate_ret = active->ops->activate(
        active->ctx, s_display.presenter,
        s_display.present_lease.generation);
    if (ret != ESP_OK || activate_ret != ESP_OK) {
        if (ret == ESP_OK) {
            esp_err_t restore_ret = esp_display_presenter_set_rotation(
                s_display.presenter,
                (esp_display_present_rotation_t)previous_degrees);
            if (restore_ret == ESP_OK) {
                restore_ret = active->ops->activate(
                active->ctx, s_display.presenter,
                s_display.present_lease.generation);
            }
            if (restore_ret != ESP_OK) {
                ESP_LOGE(TAG, "restore presenter after rotation failed: %s",
                         esp_err_to_name(restore_ret));
                display_present_lease_fault(&s_display.present_lease);
            }
        }
    } else {
        s_display.rotation_degrees = degrees;
        ESP_LOGI(TAG, "present rotation changed: %u degrees", degrees);
    }
    xSemaphoreGive(s_display.present_handoff_mutex);
    return ret == ESP_OK ? activate_ret : ret;
}

esp_err_t display_service_get_rotation(uint16_t *degrees)
{
    ESP_RETURN_ON_FALSE(degrees, ESP_ERR_INVALID_ARG, TAG,
                        "rotation output is NULL");
    ESP_RETURN_ON_FALSE(display_service_is_started(), ESP_ERR_INVALID_STATE,
                        TAG, "display is not started");
    if (s_display.control_provider.get_rotation != NULL) {
        return s_display.control_provider.get_rotation(
            degrees, s_display.control_provider.user_ctx);
    }
    *degrees = s_display.rotation_degrees;
    return ESP_OK;
}

esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session)
{
    esp_err_t err;
    const char *owner_name;

    ESP_RETURN_ON_FALSE(config != NULL && ret_session != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "session config/output missing");
    ESP_RETURN_ON_FALSE(config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL ||
                        config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
                        ESP_ERR_INVALID_ARG, TAG, "invalid session mode");
    *ret_session = NULL;
    owner_name = (config->owner_name && config->owner_name[0]) ? config->owner_name : "session";

    if (config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL &&
            s_display.presenter != NULL) {
        err = display_service_lvgl_open_internal(
            config, owner_name, ret_session);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "present LVGL session opened: owner=%s", owner_name);
        }
        return err;
    }
    if (config->mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW &&
            s_display.presenter != NULL) {
        err = display_service_raw_open_internal(
            config, owner_name, ret_session);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "present raw session opened: owner=%s", owner_name);
        }
        return err;
    }

    ESP_LOGE(TAG, "display mode requires a presenter-backed producer");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t display_service_close(display_service_session_handle_t session)
{
    esp_err_t err = ESP_OK;
    display_service_mode_t mode;
    display_service_session_cleanup_cb_t cleanup_cb;
    void *cleanup_user_ctx;
    char owner_name[DISPLAY_SERVICE_OWNER_NAME_LEN];

    ESP_RETURN_ON_FALSE(display_service_session_valid_internal(session),
                        ESP_ERR_INVALID_ARG, TAG, "invalid display session");
    mode = session->mode;
    ESP_RETURN_ON_ERROR(
        display_service_session_begin_close_internal(session),
        TAG, "begin display session close");
    cleanup_cb = NULL;
    cleanup_user_ctx = NULL;
    strlcpy(owner_name, session->owner_name, sizeof(owner_name));

    if (mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        err = display_service_lvgl_prepare_close_internal(session);
        if (err != ESP_OK) {
            display_service_session_abort_close_internal(session);
            ESP_LOGE(TAG, "prepare LVGL session cleanup failed: %s",
                     esp_err_to_name(err));
            return err;
        }
        uint32_t baseline_generation = 0;
        err = display_service_presenter_release(
            session, session->producer_generation,
            DISPLAY_SERVICE_PRESENT_HANDOFF_TIMEOUT_MS, &baseline_generation);
    } else if (mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW) {
        uint32_t baseline_generation = 0;
        err = display_service_presenter_release(
            session, session->producer_generation,
            DISPLAY_SERVICE_PRESENT_HANDOFF_TIMEOUT_MS, &baseline_generation);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        display_service_session_abort_close_internal(session);
        ESP_LOGE(TAG, "close session exclusive exit failed: %s", esp_err_to_name(err));
        return err;
    }
    if (mode != DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        cleanup_cb = display_service_session_take_cleanup_internal(
            session, &cleanup_user_ctx);
        if (cleanup_cb != NULL) {
            cleanup_cb(session, cleanup_user_ctx);
        }
    }

    ESP_RETURN_ON_ERROR(
        display_service_session_free_internal(session),
        TAG, "release display session slot");
    ESP_LOGI(TAG, "display session closed: owner=%s", owner_name);
    return ESP_OK;
}

bool display_service_session_is_valid(display_service_session_handle_t session)
{
    return display_service_session_valid_internal(session);
}

bool display_service_session_is_active(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session);
}

display_service_mode_t display_service_session_mode(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session)
        ? session->mode : DISPLAY_SERVICE_MODE_SHARED_LVGL;
}

const char *display_service_session_owner_name(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session) ? session->owner_name : NULL;
}

esp_err_t display_service_wait_idle(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_display.present_handoff_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "session service unavailable");
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        ESP_RETURN_ON_FALSE(
            xSemaphoreTake(s_display.present_handoff_mutex,
                           pdMS_TO_TICKS(1000)) == pdTRUE,
            ESP_ERR_TIMEOUT, TAG, "session idle check busy");
        bool idle = !s_display.sessions[0].active;
        xSemaphoreGive(s_display.present_handoff_mutex);
        if (idle) {
            return ESP_OK;
        }
        if (timeout_ms == 0 || xTaskGetTickCount() - start >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t display_service_set_touch_observer(display_service_touch_observer_cb_t cb,
                                             void *user_ctx)
{
    ESP_RETURN_ON_FALSE(s_display.touch_observer_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "touch observer service unavailable");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_display.touch_observer_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "touch observer lock timeout");
    s_display.touch_observer_cb = cb;
    s_display.touch_observer_user_ctx = cb != NULL ? user_ctx : NULL;
    xSemaphoreGive(s_display.touch_observer_mutex);
    return ESP_OK;
}

esp_err_t display_service_get_main_touch_sample(display_service_touch_sample_t *out_sample)
{
    ESP_RETURN_ON_FALSE(out_sample != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "touch sample output is NULL");
    ESP_RETURN_ON_FALSE(display_service_is_started() && s_display.touch != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "main touch is unavailable");
    ESP_RETURN_ON_FALSE(s_display.touch_observer_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG,
                        "touch sample service unavailable");
    ESP_RETURN_ON_FALSE(
        xSemaphoreTake(s_display.touch_observer_mutex,
                       pdMS_TO_TICKS(1000)) == pdTRUE,
        ESP_ERR_TIMEOUT, TAG, "touch sample lock timeout");
    *out_sample = s_display.touch_sample;
    xSemaphoreGive(s_display.touch_observer_mutex);
    return ESP_OK;
}

esp_err_t display_service_set_control_provider(
    const display_service_control_provider_t *provider)
{
    ESP_RETURN_ON_FALSE(!display_service_is_started(), ESP_ERR_INVALID_STATE,
                        TAG, "register display controls before startup");
    if (provider == NULL) {
        memset(&s_display.control_provider, 0,
               sizeof(s_display.control_provider));
    } else {
        s_display.control_provider = *provider;
    }
    return ESP_OK;
}

esp_err_t display_service_set_state_observer(display_service_state_observer_cb_t cb,
                                             void *user_ctx)
{
    s_display.state_observer_cb = cb;
    s_display.state_observer_user_ctx = cb != NULL ? user_ctx : NULL;
    return ESP_OK;
}

bool display_service_has_exclusive_session(void)
{
    return display_service_session_valid_internal(&s_display.sessions[0]);
}

bool display_service_exclusive_allows_system_overlay(void)
{
    return !display_service_has_exclusive_session() ||
           (s_display.sessions[0].flags &
            DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY) != 0;
}

esp_err_t display_service_start(const display_service_config_t *config)
{
    (void)config;
    return display_service_is_started() ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

void display_service_stop(void)
{
    /* The presenter-backed baseline has process lifetime ownership. */
}

esp_err_t display_service_lock(void)
{
    if (display_service_session_valid_internal(&s_display.sessions[0]) &&
        s_display.sessions[0].mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        return display_service_session_lock(&s_display.sessions[0]);
    }
    return ESP_ERR_INVALID_STATE;
}

void display_service_unlock(void)
{
    if (display_service_session_valid_internal(&s_display.sessions[0]) &&
        s_display.sessions[0].mode == DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL) {
        display_service_session_unlock(&s_display.sessions[0]);
    }
}

esp_err_t display_service_set_default_screen(lv_obj_t *screen)
{
    s_display.default_screen = screen;
    return ESP_OK;
}

void display_service_set_default_screen_locked(lv_obj_t *screen)
{
    s_display.default_screen = screen;
}

lv_obj_t *display_service_default_screen(void)
{
    return s_display.default_screen;
}

void display_service_exclusive_raw_suspend_locked(void)
{
}

void display_service_exclusive_raw_resume_locked(void)
{
}
