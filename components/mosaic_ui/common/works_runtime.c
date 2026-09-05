/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "works_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "cap_lua.h"
#include "claw_launcher.h"
#include "claw_paths.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WORKS_CATALOG_LIMIT       64U
#define WORKS_PATH_MAX            128U
#define WORKS_RECENT_FILE_MAX     2048U
#define WORKS_COMMAND_QUEUE_DEPTH 8U
#define WORKS_WORKER_STACK        6144U
#define WORKS_WORKER_PRIORITY     4U
#define WORKS_OUTPUT_BYTES        512U
#define WORKS_STOP_WAIT_MS        1000U

typedef struct {
    char *id;
    char *entry;
    char *args_json;
    int order;
    bool builtin;
    works_runtime_state_t state;
    char job_id[CAP_LUA_JOB_ID_LEN];
    char last_error[WORKS_RUNTIME_ERROR_MAX];
} works_item_t;

typedef struct {
    works_item_t *items;
    size_t count;
    size_t capacity;
} works_catalog_builder_t;

typedef enum {
    WORKS_COMMAND_START = 0,
    WORKS_COMMAND_STOP,
} works_command_type_t;

typedef struct {
    works_command_type_t type;
    char skill_id[WORKS_RUNTIME_SKILL_ID_MAX];
    char job_id[CAP_LUA_JOB_ID_LEN];
} works_command_t;

typedef struct {
    SemaphoreHandle_t lock;
    QueueHandle_t commands;
    TaskHandle_t worker;
    works_item_t *items;
    size_t item_count;
    char *recent_ids[WORKS_RUNTIME_RECENT_LIMIT];
    size_t recent_count;
    uint32_t revision;
    bool recents_loaded;
    bool recent_dirty;
    bool job_callback_registered;
    works_runtime_changed_cb_t on_changed;
    void *on_changed_ctx;
} works_runtime_t;

static const char *TAG = "works_runtime";
static EXT_RAM_BSS_ATTR works_runtime_t s_runtime;

static bool state_is_active(works_runtime_state_t state)
{
    return state == WORKS_RUNTIME_QUEUED || state == WORKS_RUNTIME_RUNNING ||
           state == WORKS_RUNTIME_STOPPING;
}

static uint32_t next_revision_locked(void)
{
    if (++s_runtime.revision == 0) {
        ++s_runtime.revision;
    }
    return s_runtime.revision;
}

static void notify_changed(uint32_t revision)
{
    works_runtime_changed_cb_t callback = NULL;
    void *ctx = NULL;

    if (revision == 0 || !s_runtime.lock) {
        return;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    callback = s_runtime.on_changed;
    ctx = s_runtime.on_changed_ctx;
    xSemaphoreGive(s_runtime.lock);
    if (callback) {
        callback(revision, ctx);
    }
}

static void item_free(works_item_t *item)
{
    if (!item) {
        return;
    }
    free(item->id);
    free(item->entry);
    free(item->args_json);
    memset(item, 0, sizeof(*item));
}

static void catalog_free(works_item_t *items, size_t count)
{
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        item_free(&items[i]);
    }
    free(items);
}

static works_item_t *find_item_locked(const char *id)
{
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < s_runtime.item_count; ++i) {
        if (strcmp(s_runtime.items[i].id, id) == 0) {
            return &s_runtime.items[i];
        }
    }
    return NULL;
}

static works_item_t *find_entry_locked(const char *path)
{
    if (!path) {
        return NULL;
    }
    for (size_t i = 0; i < s_runtime.item_count; ++i) {
        if (strcmp(s_runtime.items[i].entry, path) == 0) {
            return &s_runtime.items[i];
        }
    }
    return NULL;
}

static bool path_is_under(const char *path, const char *root)
{
    if (!path || !root || !root[0]) {
        return false;
    }
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '/' || path[root_len] == '\0');
}

static esp_err_t collect_catalog(const claw_launcher_entry_t *entry,
                                 void *user_ctx)
{
    works_catalog_builder_t *builder = user_ctx;
    if (!entry || !builder || !entry->visible || !entry->entry) {
        return ESP_OK;
    }
    if (builder->count >= WORKS_CATALOG_LIMIT) {
        return ESP_OK;
    }
    if (builder->count == builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity * 2U : 8U;
        if (capacity > WORKS_CATALOG_LIMIT) {
            capacity = WORKS_CATALOG_LIMIT;
        }
        works_item_t *items = realloc(builder->items,
                                      capacity * sizeof(*items));
        if (!items) {
            return ESP_ERR_NO_MEM;
        }
        memset(items + builder->capacity, 0,
               (capacity - builder->capacity) * sizeof(*items));
        builder->items = items;
        builder->capacity = capacity;
    }

    works_item_t *item = &builder->items[builder->count];
    item->id = strdup(entry->skill_id);
    item->entry = strdup(entry->entry);
    item->args_json = entry->args_json ? strdup(entry->args_json) : NULL;
    if (!item->id || !item->entry ||
            (entry->args_json && !item->args_json)) {
        item_free(item);
        return ESP_ERR_NO_MEM;
    }
    item->order = entry->order;
    item->builtin = path_is_under(entry->entry, claw_paths_get(CLAW_PATH_SYSTEM));
    item->state = WORKS_RUNTIME_STOPPED;
    builder->count++;
    return ESP_OK;
}

static int compare_items(const void *left, const void *right)
{
    const works_item_t *a = left;
    const works_item_t *b = right;
    if (a->order != b->order) {
        return a->order < b->order ? -1 : 1;
    }
    return strcmp(a->id, b->id);
}

static bool optional_equal(const char *left, const char *right)
{
    return left ? right && strcmp(left, right) == 0 : right == NULL;
}

static bool catalog_equal_locked(const works_item_t *items, size_t count)
{
    if (count != s_runtime.item_count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const works_item_t *old = &s_runtime.items[i];
        const works_item_t *next = &items[i];
        if (old->order != next->order || old->builtin != next->builtin ||
                strcmp(old->id, next->id) != 0 ||
                strcmp(old->entry, next->entry) != 0 ||
                !optional_equal(old->args_json, next->args_json)) {
            return false;
        }
    }
    return true;
}

static bool recent_contains_locked(const char *id)
{
    for (size_t i = 0; i < s_runtime.recent_count; ++i) {
        if (strcmp(s_runtime.recent_ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static bool touch_recent_locked(const char *id)
{
    size_t found = s_runtime.recent_count;
    for (size_t i = 0; i < s_runtime.recent_count; ++i) {
        if (strcmp(s_runtime.recent_ids[i], id) == 0) {
            found = i;
            break;
        }
    }
    if (found == 0 && found < s_runtime.recent_count) {
        return false;
    }
    char *selected = NULL;
    if (found < s_runtime.recent_count) {
        selected = s_runtime.recent_ids[found];
    } else {
        selected = strdup(id);
        if (!selected) {
            return false;
        }
        if (s_runtime.recent_count == WORKS_RUNTIME_RECENT_LIMIT) {
            free(s_runtime.recent_ids[WORKS_RUNTIME_RECENT_LIMIT - 1U]);
        } else {
            s_runtime.recent_count++;
        }
        found = s_runtime.recent_count - 1U;
    }
    for (size_t i = found; i > 0; --i) {
        s_runtime.recent_ids[i] = s_runtime.recent_ids[i - 1U];
    }
    s_runtime.recent_ids[0] = selected;
    s_runtime.recent_dirty = true;
    return true;
}

static void prune_recents_locked(void)
{
    size_t out = 0;
    for (size_t i = 0; i < s_runtime.recent_count; ++i) {
        if (find_item_locked(s_runtime.recent_ids[i])) {
            s_runtime.recent_ids[out++] = s_runtime.recent_ids[i];
        } else {
            free(s_runtime.recent_ids[i]);
            s_runtime.recent_dirty = true;
        }
    }
    for (size_t i = out; i < s_runtime.recent_count; ++i) {
        s_runtime.recent_ids[i] = NULL;
    }
    s_runtime.recent_count = out;
}

static esp_err_t recent_paths(char *directory, size_t directory_size,
                              char *path, size_t path_size)
{
    ESP_RETURN_ON_ERROR(claw_paths_join(CLAW_PATH_DATA, "works", directory,
                                        directory_size), TAG,
                        "works directory path too long");
    return claw_paths_join(CLAW_PATH_DATA, "works/recent.json", path,
                           path_size);
}

static void load_recents(void)
{
    char directory[WORKS_PATH_MAX];
    char path[WORKS_PATH_MAX];
    if (recent_paths(directory, sizeof(directory), path, sizeof(path)) != ESP_OK) {
        return;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    if (size <= 0 || size > (long)WORKS_RECENT_FILE_MAX ||
            fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return;
    }
    char *data = calloc(1, (size_t)size + 1U);
    if (!data) {
        fclose(file);
        return;
    }
    size_t read_size = fread(data, 1, (size_t)size, file);
    fclose(file);
    if (read_size != (size_t)size) {
        free(data);
        return;
    }
    cJSON *root = cJSON_Parse(data);
    free(data);
    cJSON *skills = root ? cJSON_GetObjectItemCaseSensitive(root, "skills") : NULL;
    if (cJSON_IsArray(skills)) {
        cJSON *skill = NULL;
        cJSON_ArrayForEach(skill, skills) {
            if (s_runtime.recent_count == WORKS_RUNTIME_RECENT_LIMIT) {
                break;
            }
            if (!cJSON_IsString(skill) || !skill->valuestring ||
                    !skill->valuestring[0] ||
                    recent_contains_locked(skill->valuestring)) {
                continue;
            }
            char *copy = strdup(skill->valuestring);
            if (!copy) {
                break;
            }
            s_runtime.recent_ids[s_runtime.recent_count++] = copy;
        }
    }
    cJSON_Delete(root);
}

esp_err_t works_runtime_flush(void)
{
    if (!s_runtime.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *skills = cJSON_CreateArray();
    if (!root || !skills) {
        cJSON_Delete(root);
        cJSON_Delete(skills);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    if (!s_runtime.recent_dirty) {
        xSemaphoreGive(s_runtime.lock);
        cJSON_Delete(root);
        cJSON_Delete(skills);
        return ESP_OK;
    }
    for (size_t i = 0; i < s_runtime.recent_count; ++i) {
        cJSON *id = cJSON_CreateString(s_runtime.recent_ids[i]);
        if (!id) {
            xSemaphoreGive(s_runtime.lock);
            cJSON_Delete(root);
            cJSON_Delete(skills);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(skills, id);
    }
    s_runtime.recent_dirty = false;
    xSemaphoreGive(s_runtime.lock);
    cJSON_AddItemToObject(root, "skills", skills);
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
        s_runtime.recent_dirty = true;
        xSemaphoreGive(s_runtime.lock);
        return ESP_ERR_NO_MEM;
    }

    char directory[WORKS_PATH_MAX];
    char path[WORKS_PATH_MAX];
    char temporary[WORKS_PATH_MAX];
    esp_err_t err = recent_paths(directory, sizeof(directory), path, sizeof(path));
    struct stat info = {0};
    if (err == ESP_OK && stat(directory, &info) != 0 &&
            mkdir(directory, 0755) != 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
            (int)sizeof(temporary)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    FILE *file = err == ESP_OK ? fopen(temporary, "wb") : NULL;
    if (!file) {
        err = ESP_FAIL;
    } else {
        size_t length = strlen(rendered);
        bool ok = fwrite(rendered, 1, length, file) == length &&
                  fflush(file) == 0;
        if (fclose(file) != 0) {
            ok = false;
        }
        if (!ok) {
            err = ESP_FAIL;
            (void)remove(temporary);
        } else {
            (void)remove(path);
            if (rename(temporary, path) != 0) {
                err = ESP_FAIL;
                (void)remove(temporary);
            }
        }
    }
    free(rendered);
    if (err != ESP_OK) {
        xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
        s_runtime.recent_dirty = true;
        xSemaphoreGive(s_runtime.lock);
    }
    return err;
}

static void reconcile_active_jobs(void)
{
    cap_lua_job_snapshot_t jobs[16];
    size_t count = cap_lua_collect_active_jobs(jobs,
                                               sizeof(jobs) / sizeof(jobs[0]));
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    for (size_t i = 0; i < s_runtime.item_count; ++i) {
        if (state_is_active(s_runtime.items[i].state)) {
            s_runtime.items[i].state = WORKS_RUNTIME_STOPPED;
            s_runtime.items[i].job_id[0] = '\0';
        }
    }
    for (size_t i = 0; i < count; ++i) {
        works_item_t *item = jobs[i].skill_id[0]
            ? find_item_locked(jobs[i].skill_id)
            : find_entry_locked(jobs[i].path);
        if (!item) {
            continue;
        }
        item->state = jobs[i].status == CAP_LUA_JOB_QUEUED
                          ? WORKS_RUNTIME_QUEUED : WORKS_RUNTIME_RUNNING;
        strlcpy(item->job_id, jobs[i].job_id, sizeof(item->job_id));
        item->last_error[0] = '\0';
        (void)touch_recent_locked(item->id);
    }
    next_revision_locked();
    xSemaphoreGive(s_runtime.lock);
}

static void job_event(const cap_lua_job_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (!event || !s_runtime.lock) {
        return;
    }
    uint32_t revision = 0;
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    works_item_t *item = event->skill_id[0]
        ? find_item_locked(event->skill_id)
        : find_entry_locked(event->path);
    if (item) {
        bool same_job = !item->job_id[0] ||
                        strcmp(item->job_id, event->job_id) == 0;
        switch (event->type) {
        case CAP_LUA_JOB_EVENT_CREATED:
            item->state = WORKS_RUNTIME_QUEUED;
            strlcpy(item->job_id, event->job_id, sizeof(item->job_id));
            item->last_error[0] = '\0';
            (void)touch_recent_locked(item->id);
            break;
        case CAP_LUA_JOB_EVENT_RUNNING:
            if (same_job) {
                item->state = WORKS_RUNTIME_RUNNING;
                strlcpy(item->job_id, event->job_id, sizeof(item->job_id));
            }
            break;
        case CAP_LUA_JOB_EVENT_STOP_REQUESTED:
            if (same_job) {
                item->state = WORKS_RUNTIME_STOPPING;
            }
            break;
        case CAP_LUA_JOB_EVENT_TERMINAL:
            if (same_job) {
                item->state = event->status == CAP_LUA_JOB_FAILED ||
                                      event->status == CAP_LUA_JOB_TIMEOUT
                                  ? WORKS_RUNTIME_FAILED
                                  : WORKS_RUNTIME_STOPPED;
                item->job_id[0] = '\0';
            }
            break;
        default:
            break;
        }
        revision = next_revision_locked();
    }
    xSemaphoreGive(s_runtime.lock);
    notify_changed(revision);
}

static void register_job_callback(void)
{
    if (s_runtime.job_callback_registered) {
        return;
    }
    if (cap_lua_register_job_event_cb(job_event, NULL) == ESP_OK) {
        s_runtime.job_callback_registered = true;
    }
}

static uint32_t hash_name(const char *text)
{
    uint32_t hash = UINT32_C(2166136261);
    while (text && *text) {
        hash ^= (uint8_t)*text++;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void copy_error_locked(works_item_t *item, const char *message)
{
    strlcpy(item->last_error,
            message && message[0] ? message : "unknown error",
            sizeof(item->last_error));
}

static void execute_start(const works_command_t *command)
{
    cap_lua_async_config_t config = {0};
    char *path = NULL;
    char *args = NULL;
    char name[CAP_LUA_JOB_NAME_MAX];
    bool found = false;
    bool has_args = false;

    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    works_item_t *item = find_item_locked(command->skill_id);
    if (item) {
        found = true;
        has_args = item->args_json != NULL;
        path = strdup(item->entry);
        args = item->args_json ? strdup(item->args_json) : NULL;
    }
    xSemaphoreGive(s_runtime.lock);
    if (!found || !path || (has_args && !args)) {
        free(path);
        free(args);
        xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
        item = find_item_locked(command->skill_id);
        uint32_t revision = 0;
        if (item && item->state == WORKS_RUNTIME_QUEUED) {
            item->state = WORKS_RUNTIME_FAILED;
            copy_error_locked(item, "launcher definition unavailable or out of memory");
            revision = next_revision_locked();
        }
        xSemaphoreGive(s_runtime.lock);
        notify_changed(revision);
        return;
    }

    snprintf(name, sizeof(name), "work_%08lx",
             (unsigned long)hash_name(command->skill_id));
    config.path = path;
    config.args_json = args;
    config.name = name;
    config.skill_id = command->skill_id;
    config.timeout_ms = 0;
    char output[WORKS_OUTPUT_BYTES] = {0};
    esp_err_t err = cap_lua_run_script_async_ex(&config, output, sizeof(output));
    if (err != ESP_OK) {
        xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
        item = find_item_locked(command->skill_id);
        uint32_t revision = 0;
        /* A very short cooperative stop publishes its terminal event before
         * run_script_async_ex returns. Do not overwrite that STOPPED state. */
        if (item && item->state == WORKS_RUNTIME_QUEUED) {
            item->state = WORKS_RUNTIME_FAILED;
            copy_error_locked(item, output[0] ? output : esp_err_to_name(err));
            revision = next_revision_locked();
        }
        xSemaphoreGive(s_runtime.lock);
        notify_changed(revision);
    }
    free(path);
    free(args);
}

static void execute_stop(const works_command_t *command)
{
    char name[CAP_LUA_JOB_NAME_MAX];
    snprintf(name, sizeof(name), "work_%08lx",
             (unsigned long)hash_name(command->skill_id));
    const char *target = command->job_id[0] ? command->job_id : name;
    char output[WORKS_OUTPUT_BYTES] = {0};
    esp_err_t err = cap_lua_stop_job(target, WORKS_STOP_WAIT_MS,
                                     output, sizeof(output));
    uint32_t revision = 0;
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    works_item_t *item = find_item_locked(command->skill_id);
    if (item) {
        if (err == ESP_ERR_NOT_FOUND) {
            item->state = WORKS_RUNTIME_STOPPED;
            item->job_id[0] = '\0';
        } else if (err == ESP_ERR_TIMEOUT) {
            item->state = WORKS_RUNTIME_STOPPING;
            copy_error_locked(item, "stop requested; waiting for cleanup");
        } else if (err != ESP_OK) {
            item->state = WORKS_RUNTIME_FAILED;
            copy_error_locked(item, output[0] ? output : esp_err_to_name(err));
        }
        revision = next_revision_locked();
    }
    xSemaphoreGive(s_runtime.lock);
    notify_changed(revision);
}

static void worker_task(void *arg)
{
    (void)arg;
    works_command_t command;
    for (;;) {
        if (xQueueReceive(s_runtime.commands, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command.type == WORKS_COMMAND_START) {
            execute_start(&command);
        } else {
            execute_stop(&command);
        }
        esp_err_t err = works_runtime_flush();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "persist recents failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t works_runtime_refresh(void)
{
    if (!s_runtime.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    register_job_callback();

    works_catalog_builder_t builder = {0};
    esp_err_t err = claw_launcher_foreach_entry(collect_catalog, &builder);
    if (err != ESP_OK) {
        catalog_free(builder.items, builder.count);
        return err;
    }
    if (builder.count > 1U) {
        qsort(builder.items, builder.count, sizeof(*builder.items), compare_items);
    }

    bool changed = false;
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    if (!catalog_equal_locked(builder.items, builder.count)) {
        for (size_t i = 0; i < builder.count; ++i) {
            works_item_t *old = find_item_locked(builder.items[i].id);
            if (old && strcmp(old->entry, builder.items[i].entry) == 0) {
                builder.items[i].state = old->state;
                strlcpy(builder.items[i].job_id, old->job_id,
                        sizeof(builder.items[i].job_id));
                strlcpy(builder.items[i].last_error, old->last_error,
                        sizeof(builder.items[i].last_error));
            }
        }
        catalog_free(s_runtime.items, s_runtime.item_count);
        s_runtime.items = builder.items;
        s_runtime.item_count = builder.count;
        builder.items = NULL;
        prune_recents_locked();
        changed = true;
    }
    xSemaphoreGive(s_runtime.lock);
    catalog_free(builder.items, builder.count);
    if (changed) {
        reconcile_active_jobs();
        uint32_t revision = 0;
        (void)works_runtime_get_revision(&revision);
        notify_changed(revision);
    }
    return ESP_OK;
}

static void works_launcher_changed(void *user_ctx)
{
    (void)user_ctx;
    esp_err_t err = works_runtime_refresh();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh launcher catalog failed: %s", esp_err_to_name(err));
    }
}

esp_err_t works_runtime_init(const works_runtime_config_t *config)
{
    if (!config || !config->on_changed) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_runtime.lock) {
        s_runtime.lock = xSemaphoreCreateMutex();
        s_runtime.commands = xQueueCreate(WORKS_COMMAND_QUEUE_DEPTH,
                                          sizeof(works_command_t));
        if (!s_runtime.lock || !s_runtime.commands) {
            if (s_runtime.commands) {
                vQueueDelete(s_runtime.commands);
                s_runtime.commands = NULL;
            }
            if (s_runtime.lock) {
                vSemaphoreDelete(s_runtime.lock);
                s_runtime.lock = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        load_recents();
        s_runtime.recents_loaded = true;
        if (xTaskCreate(worker_task, "works_runtime", WORKS_WORKER_STACK,
                        NULL, WORKS_WORKER_PRIORITY, &s_runtime.worker) != pdPASS) {
            vQueueDelete(s_runtime.commands);
            vSemaphoreDelete(s_runtime.lock);
            s_runtime.commands = NULL;
            s_runtime.lock = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    s_runtime.on_changed = config->on_changed;
    s_runtime.on_changed_ctx = config->user_ctx;
    xSemaphoreGive(s_runtime.lock);
    ESP_RETURN_ON_ERROR(claw_launcher_register_changed_cb(works_launcher_changed, NULL), TAG, "register launcher listener");
    return works_runtime_refresh();
}

esp_err_t works_runtime_get_revision(uint32_t *out_revision)
{
    if (!out_revision || !s_runtime.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *out_revision = s_runtime.revision;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t works_runtime_get_count(size_t *out_count)
{
    if (!out_count || !s_runtime.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *out_count = s_runtime.item_count;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

static void snapshot_item(const works_item_t *item,
                          works_runtime_item_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->skill_id, item->id, sizeof(out->skill_id));
    out->builtin = item->builtin;
    out->state = item->state;
    strlcpy(out->last_error, item->last_error, sizeof(out->last_error));
}

esp_err_t works_runtime_get_item(size_t index,
                                 works_runtime_item_snapshot_t *out_item)
{
    if (!out_item || !s_runtime.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    if (index >= s_runtime.item_count) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_ERR_NOT_FOUND;
    }
    snapshot_item(&s_runtime.items[index], out_item);
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t works_runtime_get_recent(size_t index,
                                   works_runtime_item_snapshot_t *out_item)
{
    if (!out_item || !s_runtime.lock) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    if (index >= s_runtime.recent_count) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_ERR_NOT_FOUND;
    }
    works_item_t *item = find_item_locked(s_runtime.recent_ids[index]);
    if (!item) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_ERR_NOT_FOUND;
    }
    snapshot_item(item, out_item);
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t works_runtime_request_toggle(const char *skill_id)
{
    if (!skill_id || !skill_id[0] || !s_runtime.lock || !s_runtime.commands) {
        return ESP_ERR_INVALID_ARG;
    }
    works_command_t command = {0};
    strlcpy(command.skill_id, skill_id, sizeof(command.skill_id));
    uint32_t revision = 0;
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    works_item_t *item = find_item_locked(skill_id);
    if (!item) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (state_is_active(item->state)) {
        command.type = WORKS_COMMAND_STOP;
        strlcpy(command.job_id, item->job_id, sizeof(command.job_id));
        item->state = WORKS_RUNTIME_STOPPING;
    } else {
        command.type = WORKS_COMMAND_START;
        item->state = WORKS_RUNTIME_QUEUED;
        item->last_error[0] = '\0';
    }
    revision = next_revision_locked();
    xSemaphoreGive(s_runtime.lock);
    if (xQueueSend(s_runtime.commands, &command, 0) != pdTRUE) {
        xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
        item = find_item_locked(skill_id);
        if (item) {
            item->state = WORKS_RUNTIME_FAILED;
            copy_error_locked(item, "works command queue is full");
            revision = next_revision_locked();
        }
        xSemaphoreGive(s_runtime.lock);
        notify_changed(revision);
        return ESP_ERR_TIMEOUT;
    }
    notify_changed(revision);
    return ESP_OK;
}
