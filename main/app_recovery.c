// SPDX-License-Identifier: Apache-2.0
#include "app_recovery.h"

#include "esp_iris.h"
#include "esp_ota_ops.h"

esp_err_t app_recovery_mark_healthy(void)
{
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_ERR_NOT_FOUND) {
        // A native first install has blank otadata and no pending OTA receipt.
        return ESP_OK;
    }
    // ESP-Iris accepts a pending image and reports HEALTHY to the host.
    return esp_iris_mark_healthy();
}
