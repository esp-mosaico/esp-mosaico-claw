#include "interact_backend.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "mosaico_module_interact.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define INTERACT_NVS_NAMESPACE "mosaic_interact"
#define INTERACT_NVS_INPUT_MODE_KEY "input_mode"

static const char *TAG = "interact_app";
static SemaphoreHandle_t s_lock, s_done;
static bool s_running, s_stop, s_reconnect, s_ir;
static uint8_t s_toggle;
static mosaico_interact_button_mode_t s_button_mode = MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
static interact_snapshot_t s_state;

static const mosaico_interact_rgb_t s_colors[] = {
    {255, 64, 64}, {64, 255, 112}, {64, 128, 255}, {192, 96, 255}, {255, 245, 160}, {255, 245, 160},
};
static const mosaico_interact_rgb_t s_pressed_color = {76, 255, 133}; // Match the #4CFF85 screen overlay.

static void load_input_mode(void)
{
    nvs_handle_t nvs;
    uint8_t mode = MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
    s_button_mode = MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
    esp_err_t err = nvs_open(INTERACT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open input mode: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_get_u8(nvs, INTERACT_NVS_INPUT_MODE_KEY, &mode);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK || (mode != MOSAICO_INTERACT_BUTTON_MODE_GPIO && mode != MOSAICO_INTERACT_BUTTON_MODE_TOUCH)) {
        ESP_LOGW(TAG, "load input mode: %s, value=%u", esp_err_to_name(err), (unsigned)mode);
        return;
    }
    s_button_mode = (mosaico_interact_button_mode_t)mode;
}

static void save_input_mode(mosaico_interact_button_mode_t mode)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INTERACT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open input mode for save: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, INTERACT_NVS_INPUT_MODE_KEY, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "save input mode: %s", esp_err_to_name(err));
}

static void publish(const interact_snapshot_t *state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = *state;
    xSemaphoreGive(s_lock);
}

static void worker(void *arg)
{
    (void)arg;
    mosaico_interact_handle_t board = NULL;
    interact_snapshot_t state = {.slot = -1, .touch_input_selected = s_button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH};
    unsigned ticks = 0;
    uint8_t manual_leds = 0, failed_leds = 0;
    TickType_t ir_until = 0;
    esp_err_t err = mosaico_module_mgr_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "module manager init: %s", esp_err_to_name(err));
        snprintf(state.status, sizeof(state.status), "Init failed");
    }
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool stop = s_stop, reconnect = s_reconnect, ir = s_ir;
        uint8_t toggle = s_toggle;
        mosaico_interact_button_mode_t button_mode = s_button_mode;
        s_reconnect = s_ir = false;
        s_toggle = 0;
        xSemaphoreGive(s_lock);
        if (stop) {
            break;
        }
        if (reconnect) {
            if (board) {
                mosaico_interact_close(board);
                board = NULL;
            }
            state = (interact_snapshot_t){.slot = -1, .touch_input_selected = button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH};
            ticks = 0;
            manual_leds = failed_leds = 0;
            toggle = 0;
            ir = false;
        }
        if (!board && err == ESP_OK && ticks % 20 == 0) {
            mosaico_module_mgr_info_t info = {0};
            bool found = false, busy = false;
            for (int slot = 0; slot < 2; ++slot) {
                if (mosaico_module_mgr_get_info(slot, &info) != ESP_OK || info.eeprom.board_type != MOSAICO_BOARD_TYPE_INTERACT) continue;
                busy |= info.state == MOSAICO_MODULE_MGR_STATE_CLAIMED;
                if (info.state == MOSAICO_MODULE_MGR_STATE_READY) {
                    found = true;
                    break;
                }
            }
            if (found) {
                mosaico_interact_config_t config = MOSAICO_INTERACT_DEFAULT_CONFIG();
                config.slot = info.slot;
                config.button_mode = button_mode;
                esp_err_t result = mosaico_interact_open(&config, &board);
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "open slot %d: %s", info.slot, esp_err_to_name(result));
                    snprintf(state.status, sizeof(state.status), "Open failed: %s", esp_err_to_name(result));
                } else {
                    state.ready = true;
                    state.slot = info.slot;
                    state.leds = 0;
                    state.pressed_leds = 0;
                    manual_leds = failed_leds = 0;
                    snprintf(state.status, sizeof(state.status), "%s connected", info.slot == 0 ? "Left" : "Right");
                }
            } else {
                snprintf(state.status, sizeof(state.status), busy ? "Board in use" : "Connect interaction board");
            }
        }
        // Do not report the initial unknown state as a missing module.
        state.checked = true;
        if (board) {
            manual_leds ^= toggle & ~((state.key_l ? (1U << 5) : 0) | (state.key_r ? (1U << 4) : 0));
            if (ir && (int32_t)(xTaskGetTickCount() - ir_until) >= 0) {
                snprintf(state.ir_status, sizeof(state.ir_status), "IR sending");
                publish(&state);
                esp_err_t result = mosaico_interact_ir_send_nec(board, 0x00, 0x10);
                snprintf(state.ir_status, sizeof(state.ir_status), result == ESP_OK ? "IR sent" : "IR failed");
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "IR send: %s", esp_err_to_name(result));
                }
                ir_until = xTaskGetTickCount() + pdMS_TO_TICKS(700);
            }
            mosaico_interact_inputs_t data;
            esp_err_t result = mosaico_interact_read_inputs(board, &data);
            if (result == ESP_OK) {
                if (!state.ready) {
                    snprintf(state.status, sizeof(state.status), "%s connected", state.slot == 0 ? "Left" : "Right");
                }
                state.key_l = data.left_pressed;
                state.key_r = data.right_pressed;
                state.pir = data.motion_detected;
                state.light = data.light_level;
                state.ready = true;
            } else {
                if (state.ready) {
                    ESP_LOGW(TAG, "read: %s", esp_err_to_name(result));
                }
                state.ready = state.key_l = state.key_r = state.pir = false;
                snprintf(state.status, sizeof(state.status), "Sensor read failed");
            }
            // Touch and mechanical presses share inputs; preserve the manual light setting on release.
            const uint8_t pressed_leds = (state.key_l ? (1U << 5) : 0) | (state.key_r ? (1U << 4) : 0);
            const uint8_t desired_leds = manual_leds | pressed_leds;
            for (unsigned i = 0; i < 6; ++i) {
                const uint8_t bit = 1U << i;
                if (!(((desired_leds ^ state.leds) | (pressed_leds ^ state.pressed_leds)) & bit)) continue;
                const mosaico_interact_rgb_t color = (pressed_leds & bit) ? s_pressed_color : (desired_leds & bit) ? s_colors[i] : (mosaico_interact_rgb_t){0};
                result = mosaico_interact_led_set(board, i, color);
                if (result == ESP_OK) {
                    state.leds = (state.leds & ~bit) | (desired_leds & bit);
                    state.pressed_leds = (state.pressed_leds & ~bit) | (pressed_leds & bit);
                    failed_leds &= ~bit;
                } else {
                    if (!(failed_leds & bit)) ESP_LOGW(TAG, "LED %u: %s", i, esp_err_to_name(result));
                    failed_leds |= bit;
                    snprintf(state.status, sizeof(state.status), "LED write failed");
                }
            }
        }
        if ((int32_t)(xTaskGetTickCount() - ir_until) >= 0) {
            state.ir_status[0] = '\0';
        }
        publish(&state);
        ++ticks;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (board) {
        mosaico_interact_close(board);
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

void interact_backend_start(void)
{
    load_input_mode();
    s_state = (interact_snapshot_t){.slot = -1, .touch_input_selected = s_button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH};
    snprintf(s_state.status, sizeof(s_state.status), "Connecting...");
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_stop = s_reconnect = s_ir = false;
    s_toggle = 0;
    if (!s_lock || !s_done || xTaskCreate(worker, "interact", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker allocation failed");
        snprintf(s_state.status, sizeof(s_state.status), "Out of memory");
        s_state.checked = true;
        if (s_lock) vSemaphoreDelete(s_lock);
        if (s_done) vSemaphoreDelete(s_done);
        s_lock = s_done = NULL;
        return;
    }
    s_running = true;
}

void interact_backend_stop(void)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stop = true;
    xSemaphoreGive(s_lock);
    // Join before destroying synchronization or the next app's hardware.
    xSemaphoreTake(s_done, portMAX_DELAY);
    vSemaphoreDelete(s_done);
    vSemaphoreDelete(s_lock);
    s_done = s_lock = NULL;
    s_running = false;
}

void interact_backend_command(interact_command_t command)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.ready && !s_reconnect) {
        if (command >= INTERACT_COMMAND_LED_0 && command <= INTERACT_COMMAND_LED_5) s_toggle ^= 1U << command;
        if (command == INTERACT_COMMAND_IR_SEND && !s_ir && !s_state.ir_status[0]) s_ir = true;
    }
    xSemaphoreGive(s_lock);
}

static void select_input_mode(mosaico_interact_button_mode_t button_mode)
{
    if (!s_running) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_button_mode != button_mode) {
        s_button_mode = button_mode;
        s_state.touch_input_selected = button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
        s_reconnect = true;
        changed = true;
    }
    xSemaphoreGive(s_lock);
    if (changed) save_input_mode(button_mode);
}

void interact_backend_select_button_input(void)
{
    select_input_mode(MOSAICO_INTERACT_BUTTON_MODE_GPIO);
}

void interact_backend_select_touch_input(void)
{
    select_input_mode(MOSAICO_INTERACT_BUTTON_MODE_TOUCH);
}

void interact_backend_snapshot(interact_snapshot_t *out)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    if (s_lock) xSemaphoreGive(s_lock);
}
#else
// Desktop preview deliberately exposes no fabricated sensor readings.
void interact_backend_start(void) {}
void interact_backend_stop(void) {}
void interact_backend_command(interact_command_t command) { (void)command; }
void interact_backend_select_button_input(void) {}
void interact_backend_select_touch_input(void) {}
void interact_backend_snapshot(interact_snapshot_t *out)
{
    *out = (interact_snapshot_t){.slot = -1, .checked = true, .touch_input_selected = true};
    snprintf(out->status, sizeof(out->status), "Preview - no hardware");
}
#endif
