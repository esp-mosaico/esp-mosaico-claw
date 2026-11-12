// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "esp_err.h"

/* Call only after iris_ota_support_start() and a successful local startup. */
esp_err_t app_recovery_mark_healthy(void);
