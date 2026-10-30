/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mosaico_module_mgr.h"

#define MOSAIC_MODULE_NOTICE_MERGE_US (400LL * 1000)
#define MOSAIC_MODULE_NOTICE_DURATION_US (2500LL * 1000)
#define MOSAIC_MODULE_NOTICE_PENDING_US (2000LL * 1000)

typedef struct {
    uint32_t generation;
    uint32_t notice_generation; /* Changes only for a newly identified insertion. */
    mosaico_module_presence_t presence;
    uint8_t board_type;
    bool sampled;
    bool valid;
    bool notify_when_identified;
} mosaic_module_notice_slot_t;

/* Two fixed slot snapshots; callers serialize access. No task, heap or NVS. */
typedef struct {
    mosaic_module_notice_slot_t slots[MOSAICO_MODULE_MGR_SLOT_COUNT];
    uint8_t pending_mask;
    uint8_t visible_mask;
    int64_t pending_since_us;
    int64_t visible_until_us;
} mosaic_module_notice_t;

typedef struct {
    uint8_t mask;
    uint8_t board_type[MOSAICO_MODULE_MGR_SLOT_COUNT];
    uint32_t generation[MOSAICO_MODULE_MGR_SLOT_COUNT]; /* Delivery identity, not owner/error revision. */
} mosaic_module_notice_view_t;

/* The first stable sample of each slot is a silent boot baseline. Only a
 * confirmed ABSENT -> PRESENT transition can arm one insertion notice. */
bool mosaic_module_notice_observe(mosaic_module_notice_t *state,
    const mosaico_module_mgr_info_t *info, int64_t now_us, bool allow_notice);

/* Discard pending/visible notices without forgetting connection baselines. */
void mosaic_module_notice_quiet(mosaic_module_notice_t *state);

mosaic_module_notice_view_t mosaic_module_notice_poll(
    mosaic_module_notice_t *state, int64_t now_us, bool allow_notice);

/* Consume only the generations successfully presented by the caller. A newer
 * insertion arriving during UI publication must remain pending. */
void mosaic_module_notice_ack(mosaic_module_notice_t *state,
    const mosaic_module_notice_view_t *view);
