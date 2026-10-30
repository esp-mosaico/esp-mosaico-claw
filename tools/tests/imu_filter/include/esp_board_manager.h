#pragma once
#include <stdbool.h>
#include "esp_err.h"
bool esp_board_manager_check_name(const char *name);
esp_err_t esp_board_manager_init_device_by_name(const char *name);
esp_err_t esp_board_manager_get_device_handle(const char *name, void **handle);
