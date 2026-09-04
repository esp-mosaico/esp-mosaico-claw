/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_fs.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ramfs.h"
#include "esp_attr.h"
#include "esp_littlefs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_board_manager_includes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "setup_nand_flash.h"

#define APP_FS_SYSTEM_PARTITION_LABEL   "system"
#define APP_FS_NAND_DEVICE_NAME          "nand_flash"

#define APP_FS_RAMFS_MAX_FILES          (8)
#define APP_FS_RAMFS_MAX_BYTES          (512 * 1024)
#define APP_FS_RECOVERY_PATH_SIZE       (256)
#define APP_FS_COPY_BUFFER_SIZE         (512)
#define APP_FS_RECOVERY_TASK_STACK      (4096)
#define APP_FS_RECOVERY_TASK_PRIORITY   (tskIDLE_PRIORITY + 1)
#define APP_FS_RECOVERY_DONE_BIT        BIT0

static const char *TAG = "app_fs";

static const char *const s_system_base_path = "/system";
static const char *const s_nand_storage_base_path = "/nand";
static const char *const s_ramfs_base_path = "/ramfs";
static const char *const s_recovery_dir_name = ".recovery";

static EXT_RAM_BSS_ATTR char s_storage_base_path[32];
static EXT_RAM_BSS_ATTR void *s_nand_device_handle;
static EventGroupHandle_t s_recovery_event;
static esp_err_t s_recovery_result = ESP_OK;

// Dot-prefixed by convention so cap_files list_dir hides the seed tree from the LLM.
static esp_err_t build_recovery_path(char *path, size_t path_size)
{
    int len = snprintf(path, path_size, "%s/%s", s_system_base_path, s_recovery_dir_name);
    ESP_RETURN_ON_FALSE(len > 0 && len < (int)path_size, ESP_ERR_INVALID_SIZE, TAG, "Recovery path too long");
    return ESP_OK;
}

static esp_err_t copy_file(const char *src_path, const char *dst_path)
{
    esp_err_t ret = ESP_OK;
    size_t n;
    FILE *dst = NULL;
    FILE *src = fopen(src_path, "rb");
    ESP_RETURN_ON_FALSE(src, ESP_FAIL, TAG, "open src failed: %s (%s)", src_path, strerror(errno));

    char *buf = malloc(APP_FS_COPY_BUFFER_SIZE);
    ESP_GOTO_ON_FALSE(buf, ESP_ERR_NO_MEM, cleanup, TAG,
                      "failed to allocate file copy buffer");

    dst = fopen(dst_path, "wb");
    ESP_GOTO_ON_FALSE(dst, ESP_FAIL, cleanup, TAG, "open dst failed: %s (%s)", dst_path, strerror(errno));

    while ((n = fread(buf, 1, APP_FS_COPY_BUFFER_SIZE, src)) > 0) {
        ESP_GOTO_ON_FALSE(fwrite(buf, 1, n, dst) == n, ESP_FAIL, cleanup, TAG,
                          "write failed: %s (%s)", dst_path, strerror(errno));
    }

cleanup:
    if (dst) {
        fclose(dst);
    }
    free(buf);
    fclose(src);
    return ret;
}

// Restore files from src_dir (e.g. /system/.recovery) into dst_dir (writable
// partition), copying only entries that are missing in dst_dir. Existing files
// are left untouched regardless of whether their content is intact. This
// seeds a newly flashed partition and patches up partial data loss.
// Missing source dir is not an error; there is simply nothing to recover.
static esp_err_t recover_missing_files(const char *src_dir, const char *dst_dir)
{
    DIR *dir = opendir(src_dir);
    if (!dir) {
        ESP_LOGW(TAG, "recovery source unavailable: %s (%s)", src_dir, strerror(errno));
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;
    char *src_path = malloc(APP_FS_RECOVERY_PATH_SIZE);
    char *dst_path = malloc(APP_FS_RECOVERY_PATH_SIZE);
    if (src_path == NULL || dst_path == NULL) {
        ESP_LOGE(TAG, "failed to allocate recovery path buffers");
        free(src_path);
        free(dst_path);
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (snprintf(src_path, APP_FS_RECOVERY_PATH_SIZE, "%s/%s", src_dir, entry->d_name) >= APP_FS_RECOVERY_PATH_SIZE ||
            snprintf(dst_path, APP_FS_RECOVERY_PATH_SIZE, "%s/%s", dst_dir, entry->d_name) >= APP_FS_RECOVERY_PATH_SIZE) {
            ESP_LOGW(TAG, "path too long, skipping %s/%s", src_dir, entry->d_name);
            result = ESP_ERR_INVALID_SIZE;
            continue;
        }

        struct stat st;
        if (stat(src_path, &st) != 0) {
            ESP_LOGW(TAG, "stat failed: %s (%s)", src_path, strerror(errno));
            result = ESP_FAIL;
            continue;
        }

        struct stat dst_st;
        bool dst_exists = (stat(dst_path, &dst_st) == 0);

        if (S_ISDIR(st.st_mode)) {
            if (!dst_exists && mkdir(dst_path, 0777) != 0 && errno != EEXIST) {
                ESP_LOGE(TAG, "mkdir failed: %s (%s)", dst_path, strerror(errno));
                result = ESP_FAIL;
                continue;
            }
            // Recurse so files missing inside an existing directory are restored too.
            esp_err_t sub_err = recover_missing_files(src_path, dst_path);
            if (sub_err != ESP_OK) {
                result = sub_err;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (dst_exists) {
                continue;  // keep whatever is already there, even if corrupt
            }
            esp_err_t copy_err = copy_file(src_path, dst_path);
            if (copy_err != ESP_OK) {
                result = copy_err;
            } else {
                ESP_LOGW(TAG, "recovered %s -> %s", src_path, dst_path);
            }
        }
    }

    free(src_path);
    free(dst_path);
    closedir(dir);
    return result;
}

static esp_err_t run_recovery(void)
{
    char recovery_path[64];
    ESP_RETURN_ON_ERROR(build_recovery_path(recovery_path, sizeof(recovery_path)), TAG, "Failed to build recovery path");
    return recover_missing_files(recovery_path, s_storage_base_path);
}

static void recovery_task(void *arg)
{
    (void)arg;
    const int64_t start_us = esp_timer_get_time();
    s_recovery_result = run_recovery();
    ESP_LOGI(TAG, "Recovery scan complete: result=%s elapsed=%lld ms", esp_err_to_name(s_recovery_result),
             (long long)((esp_timer_get_time() - start_us) / 1000));
    xEventGroupSetBits(s_recovery_event, APP_FS_RECOVERY_DONE_BIT);
    vTaskDelete(NULL);
}

static esp_err_t start_recovery(void)
{
    s_recovery_event = xEventGroupCreate();
    if (s_recovery_event == NULL) {
        ESP_LOGW(TAG, "Recovery event allocation failed; running synchronously");
        s_recovery_result = run_recovery();
        return ESP_OK;
    }
    if (xTaskCreate(recovery_task, "fs_recovery", APP_FS_RECOVERY_TASK_STACK, NULL, APP_FS_RECOVERY_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Recovery task creation failed; running synchronously");
        s_recovery_result = run_recovery();
        xEventGroupSetBits(s_recovery_event, APP_FS_RECOVERY_DONE_BIT);
    } else {
        ESP_LOGI(TAG, "Recovery scan started in background");
    }
    return ESP_OK;
}

static esp_err_t remove_directory_contents(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    ESP_RETURN_ON_FALSE(dir != NULL, ESP_FAIL, TAG,
                        "open reset directory failed: %s (%s)",
                        dir_path, strerror(errno));

    esp_err_t result = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t child_size = strlen(dir_path) + 1U + strlen(entry->d_name) + 1U;
        char *child = malloc(child_size);
        if (child == NULL) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        (void)snprintf(child, child_size, "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(child, &st) != 0) {
            ESP_LOGE(TAG, "stat reset path failed: %s (%s)",
                     child, strerror(errno));
            result = ESP_FAIL;
        } else if (S_ISDIR(st.st_mode)) {
            result = remove_directory_contents(child);
            if (result == ESP_OK && rmdir(child) != 0) {
                ESP_LOGE(TAG, "remove reset directory failed: %s (%s)",
                         child, strerror(errno));
                result = ESP_FAIL;
            }
        } else if (unlink(child) != 0) {
            ESP_LOGE(TAG, "remove reset file failed: %s (%s)",
                     child, strerror(errno));
            result = ESP_FAIL;
        }

        free(child);
        if (result != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    return result;
}

static esp_err_t app_fs_init_system(void)
{
    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = s_system_base_path,
        .partition_label = APP_FS_SYSTEM_PARTITION_LABEL,
        .format_if_mount_failed = false,
        .read_only = true,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&mount_config);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to mount system LittleFS: %s",
                        esp_err_to_name(err));

    ESP_LOGI(TAG, "System LittleFS mounted at %s", s_system_base_path);
    return ESP_OK;
}

static esp_err_t app_fs_init_storage(void)
{
    ESP_RETURN_ON_ERROR(
        esp_board_manager_get_device_handle(APP_FS_NAND_DEVICE_NAME,
                                            &s_nand_device_handle),
        TAG, "Failed to get mandatory NAND device");

    strlcpy(s_storage_base_path, s_nand_storage_base_path,
            sizeof(s_storage_base_path));
    ESP_LOGI(TAG, "Using mandatory NAND LittleFS at '%s' as DATA", s_storage_base_path);

    // Restore missing firmware defaults without blocking display startup.
    return start_recovery();
}

esp_err_t app_fs_wait_recovery(void)
{
    if (s_recovery_event != NULL) {
        (void)xEventGroupWaitBits(s_recovery_event, APP_FS_RECOVERY_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    }
    return s_recovery_result;
}

const char *app_fs_storage_base_path(void)
{
    return s_storage_base_path;
}

const char *app_fs_system_base_path(void)
{
    return s_system_base_path;
}

esp_err_t app_fs_get_storage_space(void *ctx,
                                   uint64_t *total_bytes,
                                   uint64_t *free_bytes)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(total_bytes && free_bytes, ESP_ERR_INVALID_ARG, TAG,
                        "space output is NULL");

    return mosaico_nand_flash_get_space(s_nand_device_handle,
                                        total_bytes, free_bytes);
}

esp_err_t app_fs_factory_reset(void)
{
    ESP_RETURN_ON_FALSE(s_storage_base_path[0] != '\0',
                        ESP_ERR_INVALID_STATE, TAG,
                        "storage filesystem is not initialized");
    esp_err_t recovery_err = app_fs_wait_recovery();
    if (recovery_err != ESP_OK) {
        ESP_LOGW(TAG, "Boot recovery was incomplete before factory reset: %s", esp_err_to_name(recovery_err));
    }
    ESP_RETURN_ON_ERROR(remove_directory_contents(s_storage_base_path), TAG,
                        "clear NAND data");

    char recovery_path[64];
    ESP_RETURN_ON_ERROR(
        build_recovery_path(recovery_path, sizeof(recovery_path)), TAG,
        "build recovery path");
    ESP_RETURN_ON_ERROR(
        recover_missing_files(recovery_path, s_storage_base_path), TAG,
        "restore factory files");
    s_recovery_result = ESP_OK;
    ESP_LOGW(TAG, "NAND user data cleared and factory files restored");
    return ESP_OK;
}

static esp_err_t app_fs_init_ramfs(void)
{
    ramfs_config_t config = {
        .base_path = s_ramfs_base_path,
        .max_files = APP_FS_RAMFS_MAX_FILES,
        .max_bytes = APP_FS_RAMFS_MAX_BYTES,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };
    ESP_RETURN_ON_ERROR(ramfs_register(&config), TAG,
                        "Failed to mount RAMFS at %s", s_ramfs_base_path);

    ESP_LOGI(TAG, "RAMFS mounted at %s max_files=%u max_bytes=%u",
             s_ramfs_base_path,
             (unsigned int)APP_FS_RAMFS_MAX_FILES,
             (unsigned int)APP_FS_RAMFS_MAX_BYTES);

    return ESP_OK;
}

esp_err_t app_fs_init(void)
{
    if (s_storage_base_path[0] == '\0') {
        strlcpy(s_storage_base_path, s_nand_storage_base_path,
                sizeof(s_storage_base_path));
    }
    ESP_RETURN_ON_ERROR(app_fs_init_system(), TAG, "Failed to mount system filesystem");
    ESP_RETURN_ON_ERROR(app_fs_init_storage(), TAG, "Failed to mount storage filesystem");
#ifdef CONFIG_SPIRAM
    ESP_RETURN_ON_ERROR(app_fs_init_ramfs(), TAG, "Failed to mount RAMFS");
#endif
    return ESP_OK;
}
