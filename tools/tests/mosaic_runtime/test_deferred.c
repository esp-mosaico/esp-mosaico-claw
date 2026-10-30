/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
typedef void (*mosaic_loader_deferred_cb_t)(void *);
enum { ESP_OK = 0, ESP_ERR_INVALID_STATE = 0x103, ESP_ERR_TIMEOUT = 0x107 };
#include "loader_deferred_type.h"
static mosaic_loader_deferred_t s_deferred[MOSAIC_LOADER_MAX_DEFERRED];
static size_t s_deferred_count;
static void *s_loader_task = (void *)(uintptr_t)1;
static bool foreign_task, commands_pending;
static unsigned order[8], completed;
static void *xTaskGetCurrentTaskHandle(void)
{ return foreign_task ? NULL : s_loader_task; }
static bool mosaic_loader_poll(void) { return commands_pending; }
#include "loader_deferred_functions.h"

static void record(void *ctx)
{
    const unsigned value = (unsigned)(uintptr_t)ctx;
    assert(completed < 8);
    order[completed++] = value;
    if (value == 2) assert(mosaic_loader_defer(record, (void *)(uintptr_t)5) == ESP_OK);
}

int main(void)
{
    assert(mosaic_loader_defer(NULL, NULL) == ESP_ERR_INVALID_STATE);
    foreign_task = true;
    assert(mosaic_loader_defer(record, NULL) == ESP_ERR_INVALID_STATE);
    foreign_task = false;
    for (unsigned i = 1; i <= 4; ++i)
        assert(mosaic_loader_defer(record, (void *)(uintptr_t)i) == ESP_OK);
    assert(mosaic_loader_defer(record, NULL) == ESP_ERR_TIMEOUT);
    commands_pending = true;
    process_deferred();
    assert(completed == 0 && s_deferred_count == 4);
    commands_pending = false;
    for (unsigned i = 0; i < 5; ++i) {
        process_deferred();
        assert(completed == i + 1 && order[i] == i + 1);
    }
    assert(completed == 5 && order[4] == 5 && s_deferred_count == 0);
    process_deferred();
    assert(completed == 5);
    puts("PASS bounded deferred FIFO preserves cancellation/start order, owner context and per-iteration bounds");
    return 0;
}
