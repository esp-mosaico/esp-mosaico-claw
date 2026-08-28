/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WORKS_RUNTIME_SKILL_ID_MAX 64U
#define WORKS_RUNTIME_ERROR_MAX    160U
#define WORKS_RUNTIME_RECENT_LIMIT 4U

typedef enum {
    WORKS_RUNTIME_STOPPED = 0,
    WORKS_RUNTIME_QUEUED,
    WORKS_RUNTIME_RUNNING,
    WORKS_RUNTIME_STOPPING,
    WORKS_RUNTIME_FAILED,
} works_runtime_state_t;

typedef struct {
    char skill_id[WORKS_RUNTIME_SKILL_ID_MAX];
    bool builtin;
    works_runtime_state_t state;
    char last_error[WORKS_RUNTIME_ERROR_MAX];
} works_runtime_item_snapshot_t;

typedef void (*works_runtime_changed_cb_t)(uint32_t revision, void *user_ctx);

typedef struct {
    works_runtime_changed_cb_t on_changed;
    void *user_ctx;
} works_runtime_config_t;

esp_err_t works_runtime_init(const works_runtime_config_t *config);
esp_err_t works_runtime_refresh(void);
esp_err_t works_runtime_flush(void);
esp_err_t works_runtime_get_revision(uint32_t *out_revision);
esp_err_t works_runtime_get_count(size_t *out_count);
esp_err_t works_runtime_get_item(size_t index,
                                 works_runtime_item_snapshot_t *out_item);
esp_err_t works_runtime_get_recent(size_t index,
                                   works_runtime_item_snapshot_t *out_item);
esp_err_t works_runtime_request_toggle(const char *skill_id);

#ifdef __cplusplus
}
#endif
