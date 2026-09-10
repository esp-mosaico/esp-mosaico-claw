#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"
#include "interact_objects.h"
#include "interact_actions.h"
#include "interact_binds.h"
#include "interact_rotated_text.h"
#include "interact_backend.h"
#include <stdio.h>
#include <string.h>

static void *s_timer;
static interact_snapshot_t s_previous;
static int s_light_text[3], s_status_text, s_ir_text, s_slot_text;

static uint32_t ldr_indicator_color(bool ready, uint8_t light)
{
    const uint8_t level = ready ? 40U + (uint8_t)(((uint16_t)light * 200U) / 100U) : 48U;
    // GSP color bindings use the scene-native RGB565 format.
    return ((uint32_t)(level & 0xF8U) << 8) | ((uint32_t)(level & 0xFCU) << 3) | (level >> 3);
}

static void render(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    interact_snapshot_t state;
    interact_backend_snapshot(&state);
    if (!memcmp(&state, &s_previous, sizeof(state))) return;
    static const uint32_t lamps[] = {GSP_BIND_LED_0, GSP_BIND_LED_1, GSP_BIND_LED_2, GSP_BIND_LED_3, GSP_BIND_LED_4, GSP_BIND_LED_5};
    for (unsigned i = 0; i < 6; ++i) esp_gsp_set_visible(ui, lamps[i], state.ready && (state.leds & (1U << i)));
    esp_gsp_set_visible(ui, GSP_BIND_LED_4_PRESSED, state.ready && (state.pressed_leds & (1U << 4)));
    esp_gsp_set_visible(ui, GSP_BIND_LED_5_PRESSED, state.ready && (state.pressed_leds & (1U << 5)));
    esp_gsp_set_visible(ui, GSP_BIND_KEY_L, state.ready && state.key_l);
    esp_gsp_set_visible(ui, GSP_BIND_KEY_R, state.ready && state.key_r);
    esp_gsp_set_visible(ui, GSP_BIND_PIR, state.ready && state.pir);
    esp_gsp_set_visible(ui, GSP_BIND_MODULE_UNAVAILABLE, state.slot < 0);
    esp_gsp_set_color(ui, GSP_BIND_LDR_GLOW, ldr_indicator_color(state.ready, state.light));
    char light[16];
    if (state.ready) snprintf(light, sizeof(light), "%u%%", state.light);
    else snprintf(light, sizeof(light), "--%%");
    interact_light_set_text(ui, light, s_light_text);
    interact_status_set_text(ui, state.status, &s_status_text);
    const char *ir_status = !state.ir_status[0] ? "OFF" : !strcmp(state.ir_status, "IR sending") ? "SEND" : !strcmp(state.ir_status, "IR sent") ? "SENT" : "FAIL";
    interact_ir_status_set_text(ui, ir_status, &s_ir_text);
    interact_slot_set_text(ui, state.preferred < 0 ? "i" : state.preferred == 0 ? "L" : "R", &s_slot_text);
    s_previous = state;
}

static void started(esp_gsp_handle_t ui)
{
    memset(&s_previous, 0xFF, sizeof(s_previous));
    s_light_text[0] = s_light_text[1] = s_light_text[2] = -1;
    s_status_text = s_ir_text = s_slot_text = -1;
    interact_backend_start();
    render(ui, NULL);
    s_timer = esp_gsp_timer_create(ui, 50, render, NULL);
}

static void stopping(esp_gsp_handle_t ui)
{
    if (s_timer) esp_gsp_timer_delete(ui, s_timer);
    s_timer = NULL;
    interact_backend_stop();
}

static void event(esp_gsp_handle_t ui, const struct mosaic_event *event)
{
    (void)ui;
    if (event->type != MOSAIC_EVENT_UI_CALL) return;
    static const uint16_t actions[] = {GSP_ACT_ID_LED_0, GSP_ACT_ID_LED_1, GSP_ACT_ID_LED_2, GSP_ACT_ID_LED_3, GSP_ACT_ID_LED_4, GSP_ACT_ID_LED_5, GSP_ACT_ID_IR_SEND, GSP_ACT_ID_SLOT};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        if (actions[i] == event->data.call.action_id) {
            interact_backend_command(i);
            break;
        }
    }
}

const mosaic_app_descriptor_t mosaic_interact_app = {
    .id = 44,
    .launch_action = GSP_ACT_ID_APP_INTERACT,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "interact", .title = "Interaction",
    .directory = &gsp_obj_directory_interact,
    // Keep the full dashboard canvas while retaining the shell's bottom exit gesture.
    .disable_swipe = true, .root_header_in_stack = true, .back_exits_app = true,
    .on_started = started, .on_stopping = stopping, .on_event = event,
};
