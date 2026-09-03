/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start ESP-Iris and register the Claw recovery-first OTA control RPCs. */
esp_err_t mosaico_iris_start(void);

/** Accept the running OTA image after all critical Claw services are ready. */
esp_err_t mosaico_iris_mark_healthy(void);

#ifdef __cplusplus
}
#endif
