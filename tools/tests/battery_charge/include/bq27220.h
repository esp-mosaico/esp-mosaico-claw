#pragma once
#include <stdbool.h>
#include "i2c_bus.h"
typedef void *bq27220_handle_t;
typedef struct { unsigned SEC; bool CFGUPDATE; } operation_status_t;
esp_err_t bq27220_get_operation_status(bq27220_handle_t gauge, operation_status_t *status);
esp_err_t bq27220_unseal(bq27220_handle_t gauge);
esp_err_t bq27220_seal(bq27220_handle_t gauge);
