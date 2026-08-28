/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_logic.h"

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    esp_gsp_handle_t ui;
    const mosaic_app_descriptor_t* descriptor;
    bool started;
} mosaic_native_logic_t;

static esp_err_t native_create(
    const mosaic_logic_config_t* config, mosaic_logic_instance_t* ret_instance)
{
    if (config == NULL || config->package == NULL || ret_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    mosaic_native_logic_t* logic = calloc(1, sizeof(*logic));
    if (logic == NULL) {
        return ESP_ERR_NO_MEM;
    }
    logic->ui = config->ui;
    logic->descriptor = config->package->descriptor;
    *ret_instance = logic;
    return ESP_OK;
}

static void native_destroy(mosaic_logic_instance_t instance) { free(instance); }

static esp_err_t native_dispatch(
    mosaic_logic_instance_t instance, const mosaic_event_t* event)
{
    mosaic_native_logic_t* logic = instance;
    if (logic == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type == MOSAIC_EVENT_START) {
        if (logic->started) {
            return ESP_ERR_INVALID_STATE;
        }
        if (logic->descriptor->on_started != NULL) {
            logic->descriptor->on_started(logic->ui);
        }
        logic->started = true;
    }

    if (event->type == MOSAIC_EVENT_STOP && logic->started) {
        if (logic->descriptor->on_stopping != NULL) {
            logic->descriptor->on_stopping(logic->ui);
        }
        logic->started = false;
    }
    return ESP_OK;
}

static esp_err_t native_step(mosaic_logic_instance_t instance, int64_t now_us)
{
    (void)now_us;
    return instance != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

const mosaic_logic_ops_t mosaic_native_logic_ops = {
    .create = native_create,
    .destroy = native_destroy,
    .dispatch = native_dispatch,
    .step = native_step,
};
