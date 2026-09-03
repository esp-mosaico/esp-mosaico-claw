// SPDX-License-Identifier: Apache-2.0

#include "mosaico_iris.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_iris_system_inventory.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"

#define OTA_SERVICE_ID        0x1200U
#define OTA_STATE_METHOD_ID   1U
#define OTA_ACCEPT_METHOD_ID  2U
#define RECOVERY_SERVICE_ID   0x7FFFU
#define ENTER_RECOVERY_METHOD 2U

#define SYSTEM_METADATA_PARTITION "sysmeta"
#define OTA_METADATA_NAMESPACE    "iris_ota_demo"
#define SYSTEM_UPDATE_NAMESPACE   "update"
#define SYSTEM_UPDATE_RESULT_KEY  "last_result"

#define SYSTEM_METADATA_MAGIC   0x49535953U
#define SYSTEM_METADATA_VERSION 2U
#define SYSTEM_LAYOUT_VERSION   4U
#define FLASH_HASH_CHUNK_BYTES  1024U

static const char *TAG = "mosaico_iris";
static bool s_registered;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t operation_id[ESP_IRIS_SYSTEM_OPERATION_ID_BYTES];
    int32_t result;
    uint8_t reserved[36];
} system_update_result_t;

static esp_err_t hash_flash_region(
    uint32_t address, size_t size,
    uint8_t output[ESP_IRIS_SYSTEM_SHA256_BYTES])
{
    uint8_t *buffer = heap_caps_malloc(FLASH_HASH_CHUNK_BYTES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate Flash hash buffer");

    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    esp_err_t err =
        psa_crypto_init() == PSA_SUCCESS &&
                psa_hash_setup(&sha, PSA_ALG_SHA_256) == PSA_SUCCESS
            ? ESP_OK
            : ESP_FAIL;
    for (size_t offset = 0; err == ESP_OK && offset < size;) {
        size_t chunk = size - offset;
        if (chunk > FLASH_HASH_CHUNK_BYTES) {
            chunk = FLASH_HASH_CHUNK_BYTES;
        }
        err = esp_flash_read(NULL, buffer, address + offset, chunk);
        if (err == ESP_OK &&
            psa_hash_update(&sha, buffer, chunk) != PSA_SUCCESS) {
            err = ESP_FAIL;
        }
        offset += chunk;
    }
    size_t output_size = 0;
    if (err == ESP_OK &&
        (psa_hash_finish(&sha, output, ESP_IRIS_SYSTEM_SHA256_BYTES,
                         &output_size) != PSA_SUCCESS ||
         output_size != ESP_IRIS_SYSTEM_SHA256_BYTES)) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        (void)psa_hash_abort(&sha);
    }
    free(buffer);
    return err;
}

static void load_system_update_result(esp_iris_system_inventory_t *inventory)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(SYSTEM_METADATA_PARTITION,
                                SYSTEM_UPDATE_NAMESPACE, NVS_READONLY,
                                &handle) != ESP_OK) {
        return;
    }

    system_update_result_t record;
    size_t size = sizeof(record);
    const esp_err_t err = nvs_get_blob(handle, SYSTEM_UPDATE_RESULT_KEY,
                                       &record, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(record) ||
        record.magic != SYSTEM_METADATA_MAGIC ||
        record.version != SYSTEM_METADATA_VERSION) {
        return;
    }
    memcpy(inventory->last_operation_id, record.operation_id,
           sizeof(inventory->last_operation_id));
    inventory->last_result = record.result;
    inventory->flags |= ESP_IRIS_SYSTEM_INVENTORY_LAST_OPERATION;
}

static esp_err_t get_system_inventory(
    esp_iris_system_inventory_t *inventory, void *user_ctx)
{
    (void)user_ctx;
    ESP_RETURN_ON_FALSE(inventory != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "inventory is null");
    memset(inventory, 0, sizeof(*inventory));
    inventory->layout_version = SYSTEM_LAYOUT_VERSION;

    const uint32_t bootloader_offset = CONFIG_BOOTLOADER_OFFSET_IN_FLASH;
    const uint32_t partition_offset = CONFIG_PARTITION_TABLE_OFFSET;
    ESP_RETURN_ON_FALSE(partition_offset > bootloader_offset,
                        ESP_ERR_INVALID_STATE, TAG,
                        "invalid protected Flash ranges");
    ESP_RETURN_ON_ERROR(hash_flash_region(
                            bootloader_offset,
                            partition_offset - bootloader_offset,
                            inventory->bootloader_sha256),
                        TAG, "hash bootloader range");
    inventory->flags |= ESP_IRIS_SYSTEM_INVENTORY_BOOTLOADER_SHA256;
    ESP_RETURN_ON_ERROR(hash_flash_region(
                            partition_offset, 0x1000,
                            inventory->partition_table_sha256),
                        TAG, "hash partition table");
    inventory->flags |= ESP_IRIS_SYSTEM_INVENTORY_PARTITION_TABLE_SHA256;
    load_system_update_result(inventory);
    return ESP_OK;
}

static bool is_ota_partition(const esp_partition_t *partition)
{
    return partition != NULL &&
           partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
           partition->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static esp_err_t recovery_write(uint32_t last_good, uint32_t target, bool planned)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(
        nvs_open_from_partition(SYSTEM_METADATA_PARTITION,
                                OTA_METADATA_NAMESPACE,
                                NVS_READWRITE, &handle),
        TAG, "open recovery metadata");

    esp_err_t err = nvs_set_u32(handle, "last_good", last_good);
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, "target", target);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "planned", planned ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static uint32_t recovery_read_u32(const char *key)
{
    nvs_handle_t handle;
    uint32_t value = 0;
    if (nvs_open_from_partition(SYSTEM_METADATA_PARTITION,
                                OTA_METADATA_NAMESPACE,
                                NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u32(handle, key, &value);
        nvs_close(handle);
    }
    return value;
}

static uint8_t recovery_read_u8(const char *key)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open_from_partition(SYSTEM_METADATA_PARTITION,
                                OTA_METADATA_NAMESPACE,
                                NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, key, &value);
        nvs_close(handle);
    }
    return value;
}

esp_err_t esp_iris_platform_prepare_ota(uint32_t running_address,
                                       uint32_t target_address)
{
    if (running_address == 0 || target_address == 0 ||
        running_address == target_address) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL || running->address != running_address) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t last_good =
        running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY
            ? recovery_read_u32("last_good")
            : running_address;
    return recovery_write(last_good, target_address, false);
}

esp_err_t esp_iris_platform_select_ota_target(uint32_t default_address,
                                              uint32_t *target_address)
{
    if (default_address == 0 || target_address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *target_address = default_address;
    const uint32_t last_good = recovery_read_u32("last_good");
    if (default_address != last_good) {
        return ESP_OK;
    }

    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (iterator != NULL) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        if (is_ota_partition(partition) && partition->address != last_good) {
            *target_address = partition->address;
            esp_partition_iterator_release(iterator);
            return ESP_OK;
        }
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);

    const esp_partition_t *running = esp_ota_get_running_partition();
    return running != NULL &&
                   running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY
               ? ESP_OK
               : ESP_ERR_NOT_FOUND;
}

esp_err_t esp_iris_platform_mark_planned_restart(void)
{
    return recovery_write(recovery_read_u32("last_good"),
                          recovery_read_u32("target"), true);
}

esp_err_t esp_iris_platform_mark_healthy(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!is_ota_partition(running)) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(esp_ota_mark_app_valid_cancel_rollback(), TAG,
                        "accept pending image");
    return recovery_write(running->address, 0, false);
}

static esp_err_t state_rpc(const esp_iris_rpc_request_t *request,
                           uint8_t *response, size_t response_capacity,
                           size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t image_state;
    const esp_err_t state_err = esp_ota_get_state_partition(running, &image_state);
    const int written = snprintf(
        (char *)response, response_capacity,
        "{\"project\":\"%s\",\"version\":\"%s\",\"mode\":\"normal\","
        "\"ota_execution\":\"recovery\",\"ota_writer\":false,"
        "\"running\":\"%s\",\"next\":\"%s\",\"image_state\":%d,"
        "\"last_good\":%" PRIu32 ",\"target\":%" PRIu32
        ",\"planned\":%u}",
        app->project_name, app->version, running->label,
        next != NULL ? next->label : "none",
        state_err == ESP_OK ? (int)image_state : -1,
        recovery_read_u32("last_good"), recovery_read_u32("target"),
        recovery_read_u8("planned"));
    if (written < 0 || (size_t)written >= response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)written;
    return ESP_OK;
}

static esp_err_t accept_rpc(const esp_iris_rpc_request_t *request,
                            uint8_t *response, size_t response_capacity,
                            size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = 0;
    return esp_iris_mark_healthy();
}

static void enter_recovery_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t enter_recovery_rpc(const esp_iris_rpc_request_t *request,
                                    uint8_t *response,
                                    size_t response_capacity,
                                    size_t *response_size, void *user_ctx)
{
    (void)response;
    (void)response_capacity;
    (void)user_ctx;
    if (request->payload_size != 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (running == NULL || factory == NULL ||
        running->address == factory->address) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(esp_iris_mark_planned_restart(), TAG,
                        "record recovery restart");
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(factory), TAG,
                        "select factory recovery");
    if (xTaskCreate(enter_recovery_task, "enter_recovery", 2048, NULL, 5,
                    NULL) != pdPASS) {
        (void)esp_ota_set_boot_partition(running);
        return ESP_ERR_NO_MEM;
    }
    *response_size = 0;
    return ESP_OK;
}

esp_err_t mosaico_iris_start(void)
{
    if (!s_registered) {
        const esp_iris_system_inventory_provider_t inventory_provider = {
            .get_inventory = get_system_inventory,
            .user_ctx = NULL,
        };
        ESP_RETURN_ON_ERROR(
            esp_iris_system_inventory_register(&inventory_provider), TAG,
            "register system inventory");
        ESP_RETURN_ON_ERROR(esp_iris_rpc_register(
                                OTA_SERVICE_ID, OTA_STATE_METHOD_ID,
                                state_rpc, NULL),
                            TAG, "register OTA state RPC");
        ESP_RETURN_ON_ERROR(esp_iris_rpc_register(
                                OTA_SERVICE_ID, OTA_ACCEPT_METHOD_ID,
                                accept_rpc, NULL),
                            TAG, "register OTA accept RPC");
        ESP_RETURN_ON_ERROR(esp_iris_rpc_register(
                                RECOVERY_SERVICE_ID, ENTER_RECOVERY_METHOD,
                                enter_recovery_rpc, NULL),
                            TAG, "register enter-Recovery RPC");
        s_registered = true;
    }

    ESP_RETURN_ON_ERROR(esp_iris_start(), TAG, "start ESP-Iris");
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP-Iris ready: version=%s mode=normal partition=%s",
             app->version, running != NULL ? running->label : "unknown");
    return ESP_OK;
}

esp_err_t mosaico_iris_mark_healthy(void)
{
    return esp_iris_mark_healthy();
}
