#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool ready, key_l, key_r, pir;
    uint8_t leds, pressed_leds, light;
    int slot, preferred;
    char status[48], ir_status[24];
} interact_snapshot_t;

void interact_backend_start(void);
void interact_backend_stop(void);
void interact_backend_command(int command);
void interact_backend_snapshot(interact_snapshot_t *out);
