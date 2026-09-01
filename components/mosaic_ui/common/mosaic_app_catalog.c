/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaic_app_catalog.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_gsp_deployable.h"
#include "mosaic_logic.h"

#if defined(ESP_PLATFORM)
#include "esp_mmap_assets.h"
#else
#include <dirent.h>
#endif

#define MOSAIC_DYNAMIC_APP_MAX MOSAIC_DYNAMIC_LAUNCH_SLOT_COUNT
#define MOSAIC_DYNAMIC_NAME_MAX 31U
#define MOSAIC_DYNAMIC_TITLE_MAX 47U
#define MOSAIC_DYNAMIC_ASSET_MAX 31U

typedef struct {
    mosaic_app_descriptor_t descriptor;
    mosaic_app_package_t package;
    char name[MOSAIC_DYNAMIC_NAME_MAX + 1U];
    char title[MOSAIC_DYNAMIC_TITLE_MAX + 1U];
    char bundle[MOSAIC_DYNAMIC_ASSET_MAX + 1U];
    char logic[MOSAIC_DYNAMIC_ASSET_MAX + 1U];
} mosaic_dynamic_app_t;

static mosaic_dynamic_app_t s_dynamic_apps[MOSAIC_DYNAMIC_APP_MAX];
static size_t s_dynamic_app_count;
static bool s_dynamic_loaded;

static size_t catalog_count(void)
{
    return mosaic_app_registry_count + s_dynamic_app_count;
}

static const mosaic_app_descriptor_t *descriptor_at(size_t index)
{
    if (index < mosaic_app_registry_count) {
        return mosaic_app_registry[index];
    }
    index -= mosaic_app_registry_count;
    return index < s_dynamic_app_count
        ? &s_dynamic_apps[index].descriptor : NULL;
}

size_t mosaic_app_catalog_count(void)
{
    return catalog_count();
}

const mosaic_app_descriptor_t *mosaic_app_catalog_at(size_t index)
{
    return descriptor_at(index);
}

const mosaic_app_descriptor_t *mosaic_app_root(void)
{
    return mosaic_app_descriptor(MOSAIC_APP_ROOT_ID);
}

const mosaic_app_descriptor_t *mosaic_app_descriptor(uint16_t id)
{
    for (size_t index = 0; index < catalog_count(); ++index) {
        const mosaic_app_descriptor_t *app = descriptor_at(index);
        if (app->id == id) {
            return app;
        }
    }
    return NULL;
}

const mosaic_app_descriptor_t *mosaic_app_descriptor_for_name(
    const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < catalog_count(); ++index) {
        const mosaic_app_descriptor_t *app = descriptor_at(index);
        if (strcmp(app->name, name) == 0) {
            return app;
        }
    }
    return NULL;
}

const mosaic_app_descriptor_t *mosaic_app_descriptor_for_action(
    uint16_t action_id)
{
    if (action_id == MOSAIC_APP_NO_LAUNCH_ACTION) {
        return NULL;
    }
    for (size_t index = 0; index < catalog_count(); ++index) {
        const mosaic_app_descriptor_t *app = descriptor_at(index);
        if (app->id != MOSAIC_APP_ROOT_ID &&
                app->launch_action == action_id) {
            return app;
        }
    }
    return NULL;
}

const mosaic_app_package_t *mosaic_app_package_for_descriptor(
    const mosaic_app_descriptor_t *descriptor)
{
    if (descriptor == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < mosaic_app_package_count; ++index) {
        if (mosaic_app_packages[index].descriptor == descriptor) {
            return &mosaic_app_packages[index];
        }
    }
    for (size_t index = 0; index < s_dynamic_app_count; ++index) {
        if (s_dynamic_apps[index].package.descriptor == descriptor) {
            return &s_dynamic_apps[index].package;
        }
    }
    return NULL;
}

const mosaic_app_package_t *mosaic_app_package_for_name(const char *name)
{
    return mosaic_app_package_for_descriptor(
        mosaic_app_descriptor_for_name(name));
}

bool mosaic_app_catalog_validate(void)
{
    static const char *const required_apps[] = {
        "welcome",
        "ai_create", "camera", "works", "settings", "setup_center",
    };
    size_t root_count = 0;
    if (catalog_count() != mosaic_app_package_count + s_dynamic_app_count) {
        return false;
    }
    for (size_t index = 0; index < catalog_count(); ++index) {
        const mosaic_app_descriptor_t *app = descriptor_at(index);
        const mosaic_app_package_t *package =
            mosaic_app_package_for_descriptor(app);
        if (app == NULL || package == NULL || app->name == NULL ||
                app->name[0] == '\0' ||
                (app->route_count != 0 && app->routes == NULL) ||
                (!package->deployable && app->directory == NULL &&
                 (app->directories == NULL || app->directory_count == 0)) ||
                (app->directories != NULL && app->directory_count == 0) ||
                (app->id != MOSAIC_APP_ROOT_ID &&
                 (app->title == NULL || app->title[0] == '\0'))) {
            return false;
        }
        for (size_t route_index = 0; route_index < app->route_count;
             ++route_index) {
            const mosaic_app_route_t *route = &app->routes[route_index];
            if (route->target_name == NULL || route->target_name[0] == '\0' ||
                    route->action_id == app->back_action ||
                    mosaic_app_descriptor_for_name(route->target_name) == NULL ||
                    strcmp(route->target_name, app->name) == 0) {
                return false;
            }
            for (size_t other_route = route_index + 1;
                 other_route < app->route_count; ++other_route) {
                if (route->action_id == app->routes[other_route].action_id) {
                    return false;
                }
            }
        }
        if (package == NULL || package->bundle_path == NULL ||
                package->bundle_path[0] == '\0' || package->logic == NULL) {
            return false;
        }
        root_count += app->id == MOSAIC_APP_ROOT_ID;
        for (size_t other = index + 1;
             other < catalog_count(); ++other) {
            const mosaic_app_descriptor_t *candidate =
                descriptor_at(other);
            if (candidate == NULL || candidate->id == app->id ||
                    strcmp(candidate->name, app->name) == 0 ||
                    (app->id != MOSAIC_APP_ROOT_ID &&
                     candidate->id != MOSAIC_APP_ROOT_ID &&
                     app->launch_action != MOSAIC_APP_NO_LAUNCH_ACTION &&
                     candidate->launch_action == app->launch_action)) {
                return false;
            }
        }
    }
    const mosaic_app_descriptor_t *root = mosaic_app_root();
    if (root_count != 1 || root == NULL ||
            root->launch_action != MOSAIC_APP_NO_LAUNCH_ACTION) {
        return false;
    }
    for (size_t index = 0;
         index < sizeof(required_apps) / sizeof(required_apps[0]); ++index) {
        if (mosaic_app_descriptor_for_name(required_apps[index]) == NULL) {
            return false;
        }
    }
    return true;
}

static bool copy_json_string(const cJSON *object, const char *field,
    char *destination, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
    if (!cJSON_IsString(value) || value->valuestring == NULL ||
            value->valuestring[0] == '\0' ||
            strlcpy(destination, value->valuestring, capacity) >= capacity) {
        return false;
    }
    return true;
}

static bool safe_asset_stem(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor) {
        if (!((*cursor >= 'a' && *cursor <= 'z') ||
                (*cursor >= '0' && *cursor <= '9') ||
                *cursor == '_' || *cursor == '-')) {
            return false;
        }
    }
    return true;
}

static bool printable_ascii(const char *value)
{
    if (value == NULL || value[0] == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor) {
        if (*cursor < 0x20 || *cursor > 0x7e) {
            return false;
        }
    }
    return true;
}

#if defined(ESP_PLATFORM)
static mmap_assets_handle_t s_loading_assets;

static int find_asset_index(const char *name)
{
    const int count = mmap_assets_get_stored_files(s_loading_assets);
    for (int index = 0; index < count; ++index) {
        const char *candidate = mmap_assets_get_name(s_loading_assets, index);
        if (candidate != NULL && strcmp(candidate, name) == 0) {
            return index;
        }
    }
    return -1;
}

static esp_err_t validate_deployable_asset(
    const char *name)
{
    const int index = find_asset_index(name);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const int raw_size = mmap_assets_get_size(s_loading_assets, index);
    const void *data = mmap_assets_get_mem(s_loading_assets, index);
    if (raw_size <= 0 || data == NULL) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_gsp_deployable_bundle_t *bundle = NULL;
    const esp_gsp_err_t err = esp_gsp_deployable_bundle_open(
        data, (size_t)raw_size, true, &bundle);
    if (err != ESP_GSP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    esp_gsp_deployable_bundle_close(bundle);
    return ESP_OK;
}
#else
static bool host_asset_path(const char *name, char *path, size_t capacity)
{
    const int written = snprintf(
        path, capacity, "%s/%s", MOSAIC_UI_SOURCE_DIR, name);
    return written > 0 && (size_t)written < capacity;
}

static int find_asset_index(const char *name)
{
    char path[1024];
    if (!host_asset_path(name, path, sizeof(path))) {
        return -1;
    }
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) {
        return -1;
    }
    fclose(stream);
    return 0;
}

static esp_err_t validate_deployable_asset(const char *name)
{
    char path[1024];
    if (!host_asset_path(name, path, sizeof(path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *stream = fopen(path, "rb");
    if (stream == NULL || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream);
        return ESP_ERR_NOT_FOUND;
    }
    const long raw_size = ftell(stream);
    if (raw_size <= 0 || fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return ESP_ERR_INVALID_SIZE;
    }
    void *allocation = malloc((size_t)raw_size + 63U);
    if (allocation == NULL) {
        fclose(stream);
        return ESP_ERR_NO_MEM;
    }
    void *data = (void *)(((uintptr_t)allocation + 63U) & ~(uintptr_t)63U);
    if (fread(data, 1, (size_t)raw_size, stream) != (size_t)raw_size) {
        free(allocation);
        fclose(stream);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(stream);
    esp_gsp_deployable_bundle_t *bundle = NULL;
    const esp_gsp_err_t gsp_err = esp_gsp_deployable_bundle_open(
        data, (size_t)raw_size, true, &bundle);
    esp_gsp_deployable_bundle_close(bundle);
    free(allocation);
    return gsp_err == ESP_GSP_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
#endif

static esp_err_t load_manifest(const void *data, size_t size)
{
    if (s_dynamic_app_count >= MOSAIC_DYNAMIC_APP_MAX || data == NULL ||
            size == 0 || size > 4096) {
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON *root = cJSON_ParseWithLength(data, size);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    mosaic_dynamic_app_t *app = &s_dynamic_apps[s_dynamic_app_count];
    memset(app, 0, sizeof(*app));
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *action =
        cJSON_GetObjectItemCaseSensitive(root, "launch_action");
    const cJSON *tick = cJSON_GetObjectItemCaseSensitive(root, "tick_ms");
    const cJSON *disable_swipe =
        cJSON_GetObjectItemCaseSensitive(root, "disable_swipe");
    bool valid = cJSON_IsNumber(schema) && schema->valuedouble == 1.0 &&
        cJSON_IsNumber(id) && id->valueint > 0 && id->valueint < UINT16_MAX &&
        id->valuedouble == (double)id->valueint &&
        cJSON_IsNumber(action) &&
        action->valuedouble == (double)action->valueint &&
        action->valueint >= (int)MOSAIC_DYNAMIC_LAUNCH_ACTION_BASE &&
        action->valueint < (int)(MOSAIC_DYNAMIC_LAUNCH_ACTION_BASE +
            MOSAIC_DYNAMIC_LAUNCH_SLOT_COUNT) &&
        cJSON_IsNumber(tick) && tick->valuedouble >= 10 &&
        tick->valuedouble <= 60000 &&
        tick->valuedouble == (double)tick->valueint &&
        copy_json_string(root, "name", app->name, sizeof(app->name)) &&
        copy_json_string(root, "title", app->title, sizeof(app->title)) &&
        copy_json_string(root, "bundle", app->bundle, sizeof(app->bundle)) &&
        copy_json_string(root, "logic", app->logic, sizeof(app->logic));
    valid = valid && safe_asset_stem(app->name) && printable_ascii(app->title);
    mosaic_capability_mask_t capabilities = 0;
    const cJSON *capability = NULL;
    const cJSON *capabilities_json =
        cJSON_GetObjectItemCaseSensitive(root, "capabilities");
    if (!cJSON_IsArray(capabilities_json)) {
        valid = false;
    } else {
        cJSON_ArrayForEach(capability, capabilities_json) {
            mosaic_capability_mask_t permission = 0;
            if (!cJSON_IsString(capability) ||
                    mosaic_capability_permission(
                        capability->valuestring, &permission) != ESP_OK) {
                valid = false;
                break;
            }
            capabilities |= permission;
        }
    }
    if (valid) {
        char expected[MOSAIC_DYNAMIC_ASSET_MAX + 1U];
        int written = snprintf(expected, sizeof(expected), "%s.gspb", app->name);
        valid = written > 0 && (size_t)written < sizeof(expected) &&
            strcmp(app->bundle, expected) == 0;
        written = snprintf(expected, sizeof(expected), "%s.lua", app->name);
        valid = valid && written > 0 && (size_t)written < sizeof(expected) &&
            strcmp(app->logic, expected) == 0 &&
            find_asset_index(expected) >= 0;
    }
    if (valid) {
        valid = mosaic_app_descriptor((uint16_t)id->valueint) == NULL &&
            mosaic_app_descriptor_for_name(app->name) == NULL &&
            mosaic_app_descriptor_for_action((uint16_t)action->valueint) == NULL;
    }
    if (valid) {
        valid = validate_deployable_asset(app->bundle) == ESP_OK;
    }
    if (valid) {
        app->descriptor = (mosaic_app_descriptor_t) {
            .id = (uint16_t)id->valueint,
            .launch_action = (uint16_t)action->valueint,
            .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
            .name = app->name,
            .title = app->title,
            .disable_swipe = cJSON_IsTrue(disable_swipe),
        };
        app->package = (mosaic_app_package_t) {
            .descriptor = &app->descriptor,
            .bundle_path = app->bundle,
            .logic = &mosaic_lua_logic_ops,
            .logic_entry = app->logic,
            .tick_ms = (uint32_t)tick->valuedouble,
            .deployable = true,
            .capabilities = capabilities,
        };
        ++s_dynamic_app_count;
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t mosaic_app_catalog_load_dynamic(void)
{
    if (s_dynamic_loaded) {
        return ESP_OK;
    }
#if defined(ESP_PLATFORM)
    const mmap_assets_config_t config = {
        .partition_label = "ui_apps",
        .flags = { .mmap_enable = true, .full_check = true },
    };
    mmap_assets_handle_t assets = NULL;
    esp_err_t err = mmap_assets_new(&config, &assets);
    if (err != ESP_OK) {
        return err;
    }
    s_loading_assets = assets;
    const int count = mmap_assets_get_stored_files(assets);
    for (int index = 0; index < count; ++index) {
        const char *name = mmap_assets_get_name(assets, index);
        const size_t length = name != NULL ? strlen(name) : 0;
        if (length < 6 || strcmp(name + length - 5, ".json") != 0) {
            continue;
        }
        const int raw_size = mmap_assets_get_size(assets, index);
        const void *data = mmap_assets_get_mem(assets, index);
        err = raw_size > 0 ? load_manifest(data, (size_t)raw_size)
                           : ESP_ERR_INVALID_SIZE;
        if (err != ESP_OK) {
            printf("mosaic_catalog: reject %s: %s\n", name,
                esp_err_to_name(err));
            continue;
        }
        printf("mosaic_catalog: registered %s\n", name);
    }
    s_loading_assets = NULL;
    mmap_assets_del(assets);
#else
    DIR *directory = opendir(MOSAIC_UI_SOURCE_DIR);
    if (directory == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        const size_t length = strlen(entry->d_name);
        if (length < 6 || strcmp(entry->d_name + length - 5, ".json") != 0) {
            continue;
        }
        char path[1024];
        if (!host_asset_path(entry->d_name, path, sizeof(path))) {
            continue;
        }
        FILE *stream = fopen(path, "rb");
        if (stream == NULL || fseek(stream, 0, SEEK_END) != 0) {
            if (stream != NULL) fclose(stream);
            continue;
        }
        const long raw_size = ftell(stream);
        if (raw_size <= 0 || raw_size > 4096 || fseek(stream, 0, SEEK_SET) != 0) {
            fclose(stream);
            continue;
        }
        char data[4096];
        if (fread(data, 1, (size_t)raw_size, stream) != (size_t)raw_size) {
            fclose(stream);
            continue;
        }
        fclose(stream);
        esp_err_t err = load_manifest(data, (size_t)raw_size);
        if (err != ESP_OK) {
            fprintf(stderr, "mosaic_catalog: reject %s: %d\n",
                entry->d_name, err);
            continue;
        }
        fprintf(stderr, "mosaic_catalog: registered %s\n", entry->d_name);
    }
    closedir(directory);
#endif
    s_dynamic_loaded = true;
    return ESP_OK;
}

bool mosaic_app_route_event(
    const mosaic_app_descriptor_t *active, const esp_gsp_event_t *event,
    const mosaic_app_descriptor_t **out_target)
{
    if (active == NULL || event == NULL || out_target == NULL ||
            event->type != ESP_GSP_EVENT_CALL) {
        return false;
    }
    if (active->id == MOSAIC_APP_ROOT_ID) {
        const mosaic_app_descriptor_t *app =
            mosaic_app_descriptor_for_action(event->action_id);
        if (app == NULL) {
            return false;
        }
        *out_target = app;
        return true;
    }
    for (size_t index = 0; index < active->route_count; ++index) {
        const mosaic_app_route_t *route = &active->routes[index];
        if (event->action_id != route->action_id) {
            continue;
        }
        const mosaic_app_descriptor_t *target =
            mosaic_app_descriptor_for_name(route->target_name);
        if (target == NULL) {
            return false;
        }
        *out_target = target;
        return true;
    }
    if (event->action_id == active->back_action) {
        *out_target = mosaic_app_root();
        return true;
    }
    return false;
}
