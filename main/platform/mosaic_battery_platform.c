/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_battery_platform.h"
#include "mosaic_power_platform.h"

#include <stdbool.h>
#include <string.h>
#include "cJSON.h"

#include "esp_board_manager_includes.h"
#include "bq27220.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MOSAIC_BATTERY_SAMPLE_MS 5000U
#define MOSAIC_BATTERY_CHARGE_ON_MA 8
#define MOSAIC_BATTERY_CHARGE_CONFIRM_SAMPLES 3U
#define MOSAIC_BATTERY_LOW_SHUTDOWN_SOC 2U
#define MOSAIC_BATTERY_LOW_SHUTDOWN_CONFIRM_SAMPLES 3U
#define MOSAIC_BATTERY_TELEMETRY_SAMPLES 8U
#define MOSAIC_BATTERY_MIN_VALID_MV 2500U
#define MOSAIC_BATTERY_MAX_VALID_MV 5000U

static const char *TAG = "mosaic_battery";
static esp_timer_handle_t s_timer;
static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static mosaic_settings_battery_t s_sample;
static bool s_sample_valid;
static bool s_charging_latched;
static bool s_charge_candidate;
static uint8_t s_charge_candidate_samples;
static uint8_t s_telemetry_div;
static uint8_t s_low_shutdown_samples;
static bool s_low_shutdown_requested;
static void *s_battery;

static void battery_low_shutdown_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "battery SoC below %u%%; requesting hardware power-off",
             (unsigned)MOSAIC_BATTERY_LOW_SHUTDOWN_SOC);
    esp_err_t err = mosaic_power_platform_request_shutdown();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board power-off request failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "power-off pulse sent through BSP open-drain control");
    }
    vTaskDelete(NULL);
}

static void battery_low_shutdown_check(
    const mosaic_settings_battery_t *battery)
{
    if (battery == NULL || !battery->available || battery->charging ||
            battery->stale ||
            battery->state_of_charge >= MOSAIC_BATTERY_LOW_SHUTDOWN_SOC) {
        s_low_shutdown_samples = 0;
        return;
    }

    if (s_low_shutdown_requested) {
        return;
    }
    if (s_low_shutdown_samples <
            MOSAIC_BATTERY_LOW_SHUTDOWN_CONFIRM_SAMPLES) {
        ++s_low_shutdown_samples;
    }
    if (s_low_shutdown_samples <
            MOSAIC_BATTERY_LOW_SHUTDOWN_CONFIRM_SAMPLES) {
        return;
    }

    s_low_shutdown_requested = true;
    BaseType_t task_ok = xTaskCreate(
        battery_low_shutdown_task, "batt_shutdown", 2048, NULL,
        tskIDLE_PRIORITY + 1, NULL);
    if (task_ok != pdPASS) {
        s_low_shutdown_requested = false;
        ESP_LOGE(TAG, "failed to schedule low-battery shutdown");
    }
}

static bool read_battery(mosaic_settings_battery_t *out_battery)
{
    if (out_battery == NULL) {
        return false;
    }
    memset(out_battery, 0, sizeof(*out_battery));
    if (!esp_board_manager_check_name("battery_monitor")) {
        return false;
    }
    if (s_battery == NULL) {
        if (esp_board_manager_init_device_by_name("battery_monitor") != ESP_OK || esp_board_manager_get_device_handle("battery_monitor", &s_battery) != ESP_OK) {
            return false;
        }
    }

    battery_status_t status = {0};
    if (bq27220_get_battery_status(
            (bq27220_handle_t)s_battery, &status) != ESP_OK) {
        return false;
    }

    const uint16_t voltage_mv = bq27220_get_voltage(s_battery);
    const uint16_t state_of_charge = bq27220_get_state_of_charge(s_battery);
    if (voltage_mv < MOSAIC_BATTERY_MIN_VALID_MV ||
            voltage_mv > MOSAIC_BATTERY_MAX_VALID_MV ||
            state_of_charge > 100U) {
        ESP_LOGW(TAG, "reject invalid sample: V=%u mV SoC=%u%%",
                 (unsigned)voltage_mv, (unsigned)state_of_charge);
        return false;
    }

    const int16_t current_ma = bq27220_get_current(s_battery);
    const int16_t avg_ma = bq27220_get_avgcurrent(s_battery);
    /* Current is the net current at the cell, not an external-power signal.
     * While USB is connected the running system can consume slightly more
     * than the charger supplies, producing a small negative current even
     * though the gauge remains outside DISCHARGE state. Therefore DSG is the
     * authoritative direction signal. Positive current may enter charging
     * early while DSG is still catching up, but negative current alone must
     * never clear an established charging state. */
    const bool positive_current =
        current_ma >= MOSAIC_BATTERY_CHARGE_ON_MA ||
        avg_ma >= MOSAIC_BATTERY_CHARGE_ON_MA;
    const bool charge_in = !status.DSG || positive_current;
    const bool charge_out = status.DSG && !positive_current;
    const bool was_charging = s_charging_latched;
    if (charge_in != charge_out) {
        const bool candidate = charge_in;
        if (candidate != s_charge_candidate) {
            s_charge_candidate = candidate;
            s_charge_candidate_samples = 1U;
        } else if (s_charge_candidate_samples <
                   MOSAIC_BATTERY_CHARGE_CONFIRM_SAMPLES) {
            ++s_charge_candidate_samples;
        }
        if (s_charge_candidate_samples >=
                MOSAIC_BATTERY_CHARGE_CONFIRM_SAMPLES) {
            s_charging_latched = candidate;
        }
    } else {
        s_charge_candidate_samples = 0U;
    }
    if (was_charging != s_charging_latched) {
        ESP_LOGI(TAG, "charging %s (I=%d mA avg=%d mA DSG=%d FC=%d)",
                 s_charging_latched ? "on" : "off", (int)current_ma,
                 (int)avg_ma, (int)status.DSG, (int)status.FC);
    }

    out_battery->available = true;
    out_battery->sampled_at_us = esp_timer_get_time();
    out_battery->charging = s_charging_latched;
    out_battery->state_of_charge = state_of_charge;
    out_battery->voltage_mv = voltage_mv;
    /* AverageCurrent is intentionally published: the instantaneous channel
     * jitters with display/Wi-Fi load and is unsuitable for a settings row. */
    out_battery->current_ma = avg_ma;
    out_battery->time_to_empty_min = out_battery->charging
        ? UINT16_MAX : bq27220_get_time_to_empty(s_battery);
    out_battery->time_to_full_min = out_battery->charging
        ? bq27220_get_time_to_full(s_battery) : UINT16_MAX;
    out_battery->cycle_count = bq27220_get_cycle_count(s_battery);
    out_battery->state_of_health = bq27220_get_state_of_health(s_battery);
    if (out_battery->state_of_health > 100U) {
        out_battery->state_of_health = UINT16_MAX;
    }

    if (++s_telemetry_div >= MOSAIC_BATTERY_TELEMETRY_SAMPLES) {
        s_telemetry_div = 0;
        const uint16_t rem_mah = bq27220_get_remaining_capacity(s_battery);
        const uint16_t full_mah = bq27220_get_full_charge_capacity(s_battery);
        const uint16_t design_mah = bq27220_get_design_capacity(s_battery);
        ESP_LOGD(TAG,
                 "raw V=%umV I=%d/%d mA SoC=%u%% rem=%u full=%u design=%u "
                 "DSG=%d FC=%d latch=%d",
                 (unsigned)voltage_mv, (int)current_ma, (int)avg_ma,
                 (unsigned)state_of_charge, (unsigned)rem_mah,
                 (unsigned)full_mah, (unsigned)design_mah, (int)status.DSG,
                 (int)status.FC, (int)s_charging_latched);
    }
    return true;
}

static bool battery_changed(const mosaic_settings_battery_t *a,
                            const mosaic_settings_battery_t *b)
{
    return a->available != b->available ||
           a->stale != b->stale || a->last_error != b->last_error ||
           a->charging != b->charging ||
           a->state_of_charge != b->state_of_charge ||
           a->voltage_mv != b->voltage_mv ||
           a->current_ma != b->current_ma ||
           a->time_to_empty_min != b->time_to_empty_min ||
           a->time_to_full_min != b->time_to_full_min ||
           a->cycle_count != b->cycle_count ||
           a->state_of_health != b->state_of_health;
}

static void sample_and_publish(bool force)
{
    mosaic_settings_battery_t sample = {0};
    if (!read_battery(&sample)) {
        s_low_shutdown_samples = 0;
        portENTER_CRITICAL(&s_sample_lock);
        const bool publish = force || (s_sample_valid && !s_sample.stale) ||
            s_sample.last_error != ESP_FAIL;
        s_sample.stale = s_sample_valid;
        s_sample.last_error = ESP_FAIL;
        const mosaic_settings_battery_t published = s_sample;
        portEXIT_CRITICAL(&s_sample_lock);
        if (publish) {
            mosaic_settings_notify_battery(&published);
        }
        return;
    }

    bool publish = force;
    portENTER_CRITICAL(&s_sample_lock);
    if (!s_sample_valid || battery_changed(&s_sample, &sample)) {
        publish = true;
    }
    /* Keep freshness current even when unchanged values need no UI publish. */
    s_sample = sample;
    s_sample_valid = true;
    mosaic_settings_battery_t published = s_sample;
    portEXIT_CRITICAL(&s_sample_lock);
    if (publish) {
        mosaic_settings_notify_battery(&published);
    }
    battery_low_shutdown_check(&sample);
}

static void battery_timer_cb(void *arg)
{
    (void)arg;
    sample_and_publish(false);
}

esp_err_t mosaic_battery_platform_start(void)
{
    if (s_timer != NULL) {
        return ESP_OK;
    }
    const esp_timer_create_args_t args = {
        .callback = battery_timer_cb,
        .name = "mosaic_batt",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG,
                        "create sampler");
    sample_and_publish(true);
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(
            s_timer, (uint64_t)MOSAIC_BATTERY_SAMPLE_MS * 1000ULL),
        TAG, "start sampler");
    ESP_LOGI(TAG, "sampler started at %u ms",
             (unsigned)MOSAIC_BATTERY_SAMPLE_MS);
    return ESP_OK;
}

esp_err_t mosaic_battery_platform_get(
    mosaic_settings_battery_t *ret_battery)
{
    ESP_RETURN_ON_FALSE(ret_battery != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid output");
    portENTER_CRITICAL(&s_sample_lock);
    *ret_battery = s_sample;
    portEXIT_CRITICAL(&s_sample_lock);
    if (ret_battery->available && esp_timer_get_time() - ret_battery->sampled_at_us >
            (int64_t)MOSAIC_BATTERY_SAMPLE_MS * 3000) {
        ret_battery->stale = true;
        if (ret_battery->last_error == ESP_OK) ret_battery->last_error = ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t mosaic_battery_platform_get_info(const char *section,
    char *output, size_t output_size, void *user_ctx)
{
    (void)section;
    (void)user_ctx;
    if (output == NULL || output_size == 0) return ESP_ERR_INVALID_ARG;
    mosaic_settings_battery_t battery;
    const esp_err_t err = mosaic_battery_platform_get(&battery);
    if (err != ESP_OK) return err;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "supported", esp_board_manager_check_name("battery_monitor"));
    cJSON_AddBoolToObject(root, "available", battery.available);
    cJSON_AddBoolToObject(root, "stale", battery.stale);
    cJSON_AddNumberToObject(root, "last_error", battery.last_error);
    if (battery.available) {
        cJSON_AddBoolToObject(root, "charging", battery.charging);
        cJSON_AddNumberToObject(root, "state_of_charge", battery.state_of_charge);
        cJSON_AddNumberToObject(root, "voltage_mv", battery.voltage_mv);
        cJSON_AddNumberToObject(root, "current_ma", battery.current_ma);
        cJSON_AddNumberToObject(root, "sampled_at_us", battery.sampled_at_us);
        cJSON_AddNumberToObject(root, "sample_age_ms",
            (esp_timer_get_time() - battery.sampled_at_us) / 1000);
    }
    const bool rendered = cJSON_PrintPreallocated(root, output, output_size, false);
    cJSON_Delete(root);
    return rendered ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
