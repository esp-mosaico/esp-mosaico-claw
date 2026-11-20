#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_i18n.h"
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
static int s_light_text[3];
static int s_rendered_input_mode;

static const char *localized_status(const char *status)
{
    return strncmp(status, "Open failed", 11) == 0 ? mosaic_i18n_text("Open failed") : mosaic_i18n_text(status);
}

static void localize(esp_gsp_handle_t ui)
{
    static const struct { uint16_t bind; const char *text; } fields[] = {
        {GSP_BIND_INTERACTION_TITLE_TEXT, "Interaction Module"},
        {GSP_BIND_KEY_5_PRESSED_TEXT, "PRESSED"}, {GSP_BIND_KEY_4_PRESSED_TEXT, "PRESSED"},
        {GSP_BIND_PIR_MOTION_TEXT, "MOTION"},
        {GSP_BIND_MODULE_UNAVAILABLE_TITLE_TEXT, "MODULE NOT CONNECTED"},
        {GSP_BIND_MODULE_UNAVAILABLE_HINT_TEXT, "CONTROLS UNAVAILABLE"},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        (void)esp_gsp_set_text(ui, fields[i].bind, mosaic_i18n_text(fields[i].text));
    }
    static const char *const options_en[] = {"BUTTON", "TOUCH"};
    static const char *const options_zh[] = {"按键", "触摸"};
    const char *const *options = mosaic_i18n_get_language() == MOSAIC_UI_LANGUAGE_ZH_CN ? options_zh : options_en;
    (void)esp_gsp_dropdown_set_options_text(ui, GSP_OBJ_KEY_INPUT_MODE_DROPDOWN, options, 2);
}

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
    const int input_mode = state.touch_input_selected ? 1 : 0;
    if (s_rendered_input_mode != input_mode) {
        uint32_t selected;
        if (gsp_interact_input_mode_dropdown_get_selected(ui, &selected) == ESP_OK && selected == (uint32_t)input_mode) s_rendered_input_mode = input_mode;
        else (void)gsp_interact_input_mode_dropdown_set_selected(ui, (uint32_t)input_mode);
    }
    if (!memcmp(&state, &s_previous, sizeof(state)) && s_rendered_input_mode == input_mode) return;
    static const uint32_t lamps[] = {GSP_BIND_LED_0, GSP_BIND_LED_1, GSP_BIND_LED_2, GSP_BIND_LED_3, GSP_BIND_LED_4, GSP_BIND_LED_5};
    for (unsigned i = 0; i < 6; ++i) esp_gsp_set_visible(ui, lamps[i], state.ready && (state.leds & (1U << i)));
    esp_gsp_set_visible(ui, GSP_BIND_LED_4_PRESSED, state.ready && (state.pressed_leds & (1U << 4)));
    esp_gsp_set_visible(ui, GSP_BIND_LED_5_PRESSED, state.ready && (state.pressed_leds & (1U << 5)));
    esp_gsp_set_visible(ui, GSP_BIND_KEY_L, state.ready && state.key_l);
    esp_gsp_set_visible(ui, GSP_BIND_KEY_R, state.ready && state.key_r);
    esp_gsp_set_visible(ui, GSP_BIND_PIR, state.ready && state.pir);
    esp_gsp_set_visible(ui, GSP_BIND_MODULE_UNAVAILABLE, state.checked && state.slot < 0);
    esp_gsp_set_color(ui, GSP_BIND_LDR_GLOW, ldr_indicator_color(state.ready, state.light));
    char light[16];
    if (state.ready) snprintf(light, sizeof(light), "%u%%", state.light);
    else snprintf(light, sizeof(light), "--%%");
    interact_light_set_text(ui, light, s_light_text);
    (void)esp_gsp_set_text(ui, GSP_BIND_STATUS, localized_status(state.status));
    const char *ir_status = !state.ir_status[0] ? "OFF" : !strcmp(state.ir_status, "IR sending") ? "SEND" : !strcmp(state.ir_status, "IR sent") ? "SENT" : "FAIL";
    (void)esp_gsp_set_text(ui, GSP_BIND_IR_STATUS, mosaic_i18n_text(ir_status));
    s_previous = state;
}

static void started(esp_gsp_handle_t ui)
{
    localize(ui);
    memset(&s_previous, 0xFF, sizeof(s_previous));
    s_light_text[0] = s_light_text[1] = s_light_text[2] = -1;
    s_rendered_input_mode = -1;
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
    if (event == NULL) return;
    if (event->type == MOSAIC_EVENT_SCENE_CHANGED) {
        localize(ui);
        memset(&s_previous, 0xFF, sizeof(s_previous));
        render(ui, NULL);
        return;
    }
    if (event->type != MOSAIC_EVENT_UI_CALL) return;
    switch (event->data.call.action_id) {
    case GSP_ACT_ID_LED_0: interact_backend_command(INTERACT_COMMAND_LED_0); return;
    case GSP_ACT_ID_LED_1: interact_backend_command(INTERACT_COMMAND_LED_1); return;
    case GSP_ACT_ID_LED_2: interact_backend_command(INTERACT_COMMAND_LED_2); return;
    case GSP_ACT_ID_LED_3: interact_backend_command(INTERACT_COMMAND_LED_3); return;
    case GSP_ACT_ID_LED_4: interact_backend_command(INTERACT_COMMAND_LED_4); return;
    case GSP_ACT_ID_LED_5: interact_backend_command(INTERACT_COMMAND_LED_5); return;
    case GSP_ACT_ID_IR_SEND: interact_backend_command(INTERACT_COMMAND_IR_SEND); return;
    case GSP_ACT_ID_INPUT_MODE_SELECT: break;
    default: return;
    }
    const uint32_t index = event->data.call.list != ESP_GSP_LIST_NONE ? event->data.call.item : event->data.call.arg;
    if (index == 0U) interact_backend_select_button_input();
    else if (index == 1U) interact_backend_select_touch_input();
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
