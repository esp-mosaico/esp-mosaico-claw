#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ready, checked, key_l, key_r, pir, touch_input_selected;
    uint8_t leds, pressed_leds, light;
    int slot;
    char status[48], ir_status[24];
} interact_snapshot_t;

typedef enum {
    INTERACT_COMMAND_LED_0,
    INTERACT_COMMAND_LED_1,
    INTERACT_COMMAND_LED_2,
    INTERACT_COMMAND_LED_3,
    INTERACT_COMMAND_LED_4,
    INTERACT_COMMAND_LED_5,
    INTERACT_COMMAND_IR_SEND,
} interact_command_t;

void interact_backend_start(void);
void interact_backend_stop(void);
void interact_backend_command(interact_command_t command);
void interact_backend_select_button_input(void);
void interact_backend_select_touch_input(void);
void interact_backend_snapshot(interact_snapshot_t *out);
