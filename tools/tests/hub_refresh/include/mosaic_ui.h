#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef esp_err_t (*mosaic_ui_haptic_cb_t)(void *user_ctx, uint32_t duration_ms);
