/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persist a reset request in a namespace that survives clearing app settings. */
esp_err_t app_factory_reset_request(void);

/** Check whether a reset request still needs its boot-time data cleanup. */
esp_err_t app_factory_reset_is_pending(bool *ret_pending);

/** Clear the request after every boot-time cleanup step has succeeded. */
esp_err_t app_factory_reset_complete(void);

#ifdef __cplusplus
}
#endif
