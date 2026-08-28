/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_logic.h"

#include <stdlib.h>

struct mosaic_logic_t {
    const mosaic_logic_ops_t* ops;
    void* implementation;
};

esp_err_t mosaic_logic_create(
    const mosaic_logic_config_t* config, mosaic_logic_handle_t* ret_logic)
{
    if (ret_logic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_logic = NULL;
    if (config == NULL || config->ui == NULL || config->package == NULL
        || config->package->logic == NULL
        || config->package->logic->create == NULL
        || config->package->logic->destroy == NULL
        || config->package->logic->dispatch == NULL
        || config->package->logic->step == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaic_logic_handle_t logic = calloc(1, sizeof(*logic));
    if (logic == NULL) {
        return ESP_ERR_NO_MEM;
    }
    logic->ops = config->package->logic;
    esp_err_t err = logic->ops->create(config, &logic->implementation);
    if (err != ESP_OK) {
        free(logic);
        return err;
    }
    *ret_logic = logic;
    return ESP_OK;
}

void mosaic_logic_delete(mosaic_logic_handle_t logic)
{
    if (logic == NULL) {
        return;
    }
    logic->ops->destroy(logic->implementation);
    free(logic);
}

esp_err_t mosaic_logic_dispatch(
    mosaic_logic_handle_t logic, const mosaic_event_t* event)
{
    if (logic == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return logic->ops->dispatch(logic->implementation, event);
}

esp_err_t mosaic_logic_step(mosaic_logic_handle_t logic, int64_t now_us)
{
    if (logic == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return logic->ops->step(logic->implementation, now_us);
}
