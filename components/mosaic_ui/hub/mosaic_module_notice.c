/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_module_notice.h"

static bool supported_type(uint8_t type)
{
    return type == MOSAICO_BOARD_TYPE_CAMERA ||
           type == MOSAICO_BOARD_TYPE_INTERACT;
}

void mosaic_module_notice_quiet(mosaic_module_notice_t *state)
{
    state->pending_mask = 0;
    state->visible_mask = 0;
    state->pending_since_us = 0;
    state->visible_until_us = 0;
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        state->slots[i].notify_when_identified = false;
    }
}

bool mosaic_module_notice_observe(mosaic_module_notice_t *state,
    const mosaico_module_mgr_info_t *info, int64_t now_us, bool allow_notice)
{
    if (info == NULL || (unsigned)info->slot >= MOSAICO_MODULE_MGR_SLOT_COUNT) {
        return false;
    }
    if (!allow_notice) {
        mosaic_module_notice_quiet(state);
    }
    mosaic_module_notice_slot_t *slot = &state->slots[info->slot];
    /* A subscribe-then-snapshot bootstrap can race a newer callback. Compare
     * generations modulo 2^32; ownership/error changes alone aren't inserts. */
    const uint32_t delta = info->generation - slot->generation;
    if (slot->sampled && (delta == 0 || delta > UINT32_MAX / 2U)) {
        return false;
    }
    slot->sampled = true;
    slot->generation = info->generation;
    if (info->presence == MOSAICO_MODULE_PRESENCE_UNKNOWN) {
        return true; /* Claimed discovery GPIOs can temporarily hide a live board. */
    }
    const uint8_t bit = 1U << info->slot;
    if (info->presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
        slot->presence = info->presence;
        slot->valid = false;
        slot->notify_when_identified = false;
        state->pending_mask &= (uint8_t)~bit;
        state->visible_mask &= (uint8_t)~bit;
        return true;
    }
    if (slot->presence != MOSAICO_MODULE_PRESENCE_PRESENT) {
        slot->notify_when_identified = allow_notice &&
            slot->presence == MOSAICO_MODULE_PRESENCE_ABSENT;
    }
    slot->presence = info->presence;
    slot->valid = info->descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID;
    slot->board_type = slot->valid ? info->eeprom.board_type : 0;
    if (!slot->valid || !supported_type(slot->board_type)) {
        state->pending_mask &= (uint8_t)~bit;
        state->visible_mask &= (uint8_t)~bit;
        if (slot->valid) {
            slot->notify_when_identified = false;
        }
        return true;
    }
    if (slot->notify_when_identified && supported_type(slot->board_type)) {
        slot->notice_generation = info->generation;
        if (state->pending_mask == 0) {
            state->pending_since_us = now_us;
        }
        state->pending_mask |= bit;
    }
    slot->notify_when_identified = false;
    return true;
}

mosaic_module_notice_view_t mosaic_module_notice_poll(
    mosaic_module_notice_t *state, int64_t now_us, bool allow_notice)
{
    if (!allow_notice) {
        mosaic_module_notice_quiet(state);
    }
    if (state->visible_mask && now_us >= state->visible_until_us) {
        state->visible_mask = 0;
    }
    if (state->pending_mask) {
        const int64_t age_us = now_us - state->pending_since_us;
        if (age_us > MOSAIC_MODULE_NOTICE_PENDING_US) {
            state->pending_mask = 0;
        } else if (age_us >= MOSAIC_MODULE_NOTICE_MERGE_US) {
            state->visible_mask |= state->pending_mask;
            state->pending_mask = 0;
            state->visible_until_us = now_us + MOSAIC_MODULE_NOTICE_DURATION_US;
        }
    }
    mosaic_module_notice_view_t view = { .mask = state->visible_mask };
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if (view.mask & (1U << i)) {
            view.board_type[i] = state->slots[i].board_type;
            view.generation[i] = state->slots[i].notice_generation;
        }
    }
    return view;
}

void mosaic_module_notice_ack(mosaic_module_notice_t *state,
    const mosaic_module_notice_view_t *view)
{
    for (unsigned i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        if ((view->mask & (1U << i)) &&
                state->slots[i].notice_generation == view->generation[i]) {
            state->visible_mask &= (uint8_t)~(1U << i);
        }
    }
}
