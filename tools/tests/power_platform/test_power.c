/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_power_platform.h"
#include "bsp/power.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdio.h>

static unsigned calls;
static bool levels[2];
static unsigned delays;
static TickType_t delayed_ticks;
static unsigned fail_call;

esp_err_t bsp_power_set_shutdown(bool shutdown)
{
    assert(calls < 2);
    levels[calls++] = shutdown;
    return calls == fail_call ? ESP_FAIL : ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    ++delays;
    delayed_ticks = ticks;
}

int main(void)
{
#if CONFIG_ESP_BOARD_ESP_MOSAICO
    assert(mosaic_power_platform_request_shutdown() == ESP_OK);
    assert(calls == 2 && levels[0] && !levels[1]);
    assert(delays == 1 && delayed_ticks == pdMS_TO_TICKS(100));
    puts("PASS shutdown asserts then releases the BSP open-drain signal");

    calls = delays = 0;
    fail_call = 1;
    assert(mosaic_power_platform_request_shutdown() == ESP_FAIL);
    assert(calls == 1 && levels[0] && delays == 0);
    puts("PASS failed assertion stops before the pulse delay");

    calls = delays = 0;
    fail_call = 2;
    assert(mosaic_power_platform_request_shutdown() == ESP_FAIL);
    assert(calls == 2 && levels[0] && !levels[1] && delays == 1);
    puts("PASS failed release remains visible to the caller");
#else
    assert(mosaic_power_platform_request_shutdown() == ESP_ERR_NOT_SUPPORTED);
    assert(calls == 0 && delays == 0);
    puts("PASS unsupported boards perform no shutdown operation");
#endif
    return 0;
}
