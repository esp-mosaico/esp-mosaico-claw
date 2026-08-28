/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_lcd_touch.h"

#include <stdbool.h>
#include <stdint.h>

#include "cap_lua.h"
#include "display_service.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "lauxlib.h"

/* Keep edge tracking independent for the two explicit input sources. */
static const char *LUA_LCD_TOUCH_DEVICE_STATE_KEY = "esp-claw.lcd_touch_device_state";
static const char *LUA_LCD_TOUCH_MAIN_STATE_KEY = "esp-claw.lcd_touch_main_state";

static esp_lcd_touch_handle_t lua_lcd_touch_check_handle(
    lua_State *L, int index)
{
    void *handle = lua_touserdata(L, index);
    luaL_argcheck(L, handle != NULL, index, "lcd_touch handle expected");
    return (esp_lcd_touch_handle_t)handle;
}

static esp_err_t lua_lcd_touch_copy_sample(const display_service_touch_sample_t *sample,
                                           bool *pressed, uint16_t *x, uint16_t *y)
{
    if (sample == NULL || pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample->pressed && (sample->x < 0 || sample->x > UINT16_MAX || sample->y < 0 || sample->y > UINT16_MAX)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *pressed = sample->pressed;
    *x = sample->pressed ? (uint16_t)sample->x : 0;
    *y = sample->pressed ? (uint16_t)sample->y : 0;
    return ESP_OK;
}

static esp_err_t lua_lcd_touch_read_device(esp_lcd_touch_handle_t touch_handle,
                                            bool *pressed, uint16_t *x, uint16_t *y)
{
    if (touch_handle == NULL || pressed == NULL || x == NULL || y == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_lcd_touch_point_data_t point = {0};
    uint8_t point_count = 0;
    esp_err_t err = esp_lcd_touch_read_data(touch_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_lcd_touch_get_data(touch_handle, &point, &point_count, 1);
    if (err != ESP_OK) {
        return err;
    }

    *pressed = point_count > 0;
    *x = *pressed ? point.x : 0;
    *y = *pressed ? point.y : 0;
    return ESP_OK;
}

static esp_err_t lua_lcd_touch_read_main(bool *pressed, uint16_t *x, uint16_t *y)
{
    display_service_touch_sample_t sample = {0};
    esp_err_t err = display_service_get_main_touch_sample(&sample);
    return err == ESP_OK ? lua_lcd_touch_copy_sample(&sample, pressed, x, y) : err;
}

static void lua_lcd_touch_push_touch_table(
    lua_State *L, bool pressed, bool just_pressed, bool just_released,
    uint16_t x, uint16_t y, int dx, int dy, lua_Number held_ms)
{
    lua_newtable(L);
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushboolean(L, just_pressed);
    lua_setfield(L, -2, "just_pressed");
    lua_pushboolean(L, just_released);
    lua_setfield(L, -2, "just_released");
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, dx);
    lua_setfield(L, -2, "dx");
    lua_pushinteger(L, dy);
    lua_setfield(L, -2, "dy");
    lua_pushboolean(L, dx != 0 || dy != 0);
    lua_setfield(L, -2, "moved");
    lua_pushnumber(L, held_ms);
    lua_setfield(L, -2, "held_ms");
}

static void lua_lcd_touch_push_state_table(lua_State *L, const char *state_key)
{
    lua_getfield(L, LUA_REGISTRYINDEX, state_key);
    if (lua_istable(L, -1)) {
        return;
    }

    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, 0);
    lua_setfield(L, -2, "press_start_ms");
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, state_key);
}

static void lua_lcd_touch_update_state(
    lua_State *L, bool initialized, bool pressed,
    uint16_t x, uint16_t y, lua_Number press_start_ms)
{
    lua_pushboolean(L, initialized);
    lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, press_start_ms);
    lua_setfield(L, -2, "press_start_ms");
}

static int lua_lcd_touch_poll_result(lua_State *L, const char *state_key,
                                     bool pressed, uint16_t x, uint16_t y)
{
    lua_lcd_touch_push_state_table(L, state_key);
    lua_getfield(L, -1, "initialized");
    bool initialized = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "pressed");
    bool prev_pressed = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "x");
    uint16_t prev_x = (uint16_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "y");
    uint16_t prev_y = (uint16_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, -1, "press_start_ms");
    lua_Number press_start_ms = lua_tonumber(L, -1);
    lua_pop(L, 1);

    bool just_pressed = false;
    bool just_released = false;
    int dx = 0;
    int dy = 0;
    lua_Number held_ms = 0;
    lua_Number now_ms = (lua_Number)(esp_timer_get_time() / 1000);

    if (!initialized) {
        if (pressed) {
            press_start_ms = now_ms;
        } else {
            x = 0;
            y = 0;
            press_start_ms = 0;
        }
    } else if (pressed && prev_pressed) {
        dx = (int)x - (int)prev_x;
        dy = (int)y - (int)prev_y;
        held_ms = press_start_ms > 0 ? now_ms - press_start_ms : 0;
    } else if (pressed) {
        just_pressed = true;
        press_start_ms = now_ms;
    } else if (prev_pressed) {
        just_released = true;
        x = prev_x;
        y = prev_y;
        press_start_ms = 0;
    } else {
        x = 0;
        y = 0;
        press_start_ms = 0;
    }

    if (pressed && held_ms <= 0 && press_start_ms > 0) {
        held_ms = now_ms - press_start_ms;
        if (held_ms < 0) {
            held_ms = 0;
        }
    }

    lua_lcd_touch_update_state(
        L, true, pressed, x, y, press_start_ms);
    lua_pop(L, 1);
    lua_lcd_touch_push_touch_table(
        L, pressed, just_pressed, just_released,
        x, y, dx, dy, held_ms);
    return 1;
}

static int lua_lcd_touch_poll_device(lua_State *L)
{
    esp_lcd_touch_handle_t touch_handle = lua_lcd_touch_check_handle(L, 1);
    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;
    esp_err_t err = lua_lcd_touch_read_device(touch_handle, &pressed, &x, &y);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd_touch poll_device failed: %s", esp_err_to_name(err));
    }
    return lua_lcd_touch_poll_result(L, LUA_LCD_TOUCH_DEVICE_STATE_KEY, pressed, x, y);
}

static int lua_lcd_touch_poll_main(lua_State *L)
{
    bool pressed = false;
    uint16_t x = 0;
    uint16_t y = 0;
    esp_err_t err = lua_lcd_touch_read_main(&pressed, &x, &y);
    if (err != ESP_OK) {
        return luaL_error(L, "lcd_touch poll_main failed: %s", esp_err_to_name(err));
    }
    return lua_lcd_touch_poll_result(L, LUA_LCD_TOUCH_MAIN_STATE_KEY, pressed, x, y);
}

static int lua_lcd_touch_poll_compat(lua_State *L)
{
    /* Compatibility alias for scripts that pass the main touch handle. */
    return lua_lcd_touch_poll_main(L);
}

static int lua_lcd_touch_sync_compat(lua_State *L)
{
    (void)L;
    lua_pushboolean(L, true);
    return 1;
}

int luaopen_lcd_touch(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_lcd_touch_poll_device);
    lua_setfield(L, -2, "poll_device");
    lua_pushcfunction(L, lua_lcd_touch_poll_main);
    lua_setfield(L, -2, "poll_main");
    lua_pushcfunction(L, lua_lcd_touch_poll_compat);
    lua_setfield(L, -2, "poll");
    lua_pushcfunction(L, lua_lcd_touch_sync_compat);
    lua_setfield(L, -2, "sync");
    return 1;
}

esp_err_t lua_module_lcd_touch_register(void)
{
    return cap_lua_register_module("lcd_touch", luaopen_lcd_touch);
}
