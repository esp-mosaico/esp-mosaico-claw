/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_power_platform.h"

#if CONFIG_ESP_BOARD_ESP_MOSAICO
#include "bsp/power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MOSAIC_POWER_OFF_PULSE_MS 100U
#endif

esp_err_t mosaic_power_platform_request_shutdown(void)
{
#if CONFIG_ESP_BOARD_ESP_MOSAICO
    esp_err_t err = bsp_power_set_shutdown(true);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(MOSAIC_POWER_OFF_PULSE_MS));
    /* false releases an open-drain pad; it never actively drives GPIO57 high. */
    return bsp_power_set_shutdown(false);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
