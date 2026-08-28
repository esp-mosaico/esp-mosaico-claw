/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_present_lease.h"

#include <stddef.h>
#include <string.h>

static uint32_t next_generation(uint32_t generation)
{
    generation++;
    return generation == 0 ? 1 : generation;
}

void display_present_lease_init(display_present_lease_t *lease)
{
    if (lease != NULL) {
        memset(lease, 0, sizeof(*lease));
    }
}

esp_err_t display_present_lease_activate(
    display_present_lease_t *lease,
    const void *producer,
    uint32_t *out_generation)
{
    if (lease == NULL || producer == NULL || out_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease->state != DISPLAY_PRESENT_LEASE_EMPTY) {
        return ESP_ERR_INVALID_STATE;
    }
    lease->generation = next_generation(lease->generation);
    lease->active_producer = producer;
    lease->state = DISPLAY_PRESENT_LEASE_ACTIVE;
    *out_generation = lease->generation;
    return ESP_OK;
}

esp_err_t display_present_lease_begin_handoff(
    display_present_lease_t *lease,
    const void *current_producer,
    uint32_t current_generation,
    const void *next_producer)
{
    if (lease == NULL || current_producer == NULL || next_producer == NULL ||
            current_producer == next_producer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display_present_lease_validate(
            lease, current_producer, current_generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    lease->pending_producer = next_producer;
    lease->pending_generation = next_generation(lease->generation);
    lease->state = DISPLAY_PRESENT_LEASE_HANDOFF;
    return ESP_OK;
}

esp_err_t display_present_lease_commit_handoff(
    display_present_lease_t *lease,
    const void *next_producer,
    uint32_t *out_generation)
{
    if (lease == NULL || next_producer == NULL || out_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease->state != DISPLAY_PRESENT_LEASE_HANDOFF ||
            lease->pending_producer != next_producer) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Promote the generation reserved at begin_handoff instead of bumping a
     * fresh one, so the value the pending producer already prepared/rendered
     * against becomes the committed authority (equal to the pre-handoff value
     * advanced once, so callers observe the same result as before).
     */
    lease->generation = lease->pending_generation;
    lease->active_producer = next_producer;
    lease->pending_producer = NULL;
    lease->pending_generation = 0;
    lease->state = DISPLAY_PRESENT_LEASE_ACTIVE;
    *out_generation = lease->generation;
    return ESP_OK;
}

void display_present_lease_abort_handoff(display_present_lease_t *lease)
{
    if (lease != NULL && lease->state == DISPLAY_PRESENT_LEASE_HANDOFF) {
        lease->pending_producer = NULL;
        lease->pending_generation = 0;
        lease->state = DISPLAY_PRESENT_LEASE_ACTIVE;
    }
}

void display_present_lease_fault(display_present_lease_t *lease)
{
    if (lease != NULL) {
        lease->pending_producer = NULL;
        lease->pending_generation = 0;
        lease->state = DISPLAY_PRESENT_LEASE_FAULT;
    }
}

esp_err_t display_present_lease_begin_recover(
    display_present_lease_t *lease,
    const void *producer)
{
    if (lease == NULL || producer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (lease->state != DISPLAY_PRESENT_LEASE_FAULT) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Reuse HANDOFF so validate() accepts the recovery producer against its
     * reserved generation before commit_handoff publishes it. active_producer
     * is left unchanged until commit — it is stale while FAULTED anyway.
     */
    lease->pending_producer = producer;
    lease->pending_generation = next_generation(lease->generation);
    lease->state = DISPLAY_PRESENT_LEASE_HANDOFF;
    return ESP_OK;
}

esp_err_t display_present_lease_recover(
    display_present_lease_t *lease,
    const void *producer,
    uint32_t *out_generation)
{
    esp_err_t ret = display_present_lease_begin_recover(lease, producer);
    if (ret != ESP_OK) {
        return ret;
    }
    return display_present_lease_commit_handoff(
        lease, producer, out_generation);
}

bool display_present_lease_validate(
    const display_present_lease_t *lease,
    const void *producer,
    uint32_t generation)
{
    if (lease == NULL || producer == NULL) {
        return false;
    }
    if (lease->state == DISPLAY_PRESENT_LEASE_ACTIVE) {
        return lease->active_producer == producer &&
               lease->generation == generation;
    }
    /*
     * During a handoff the incoming producer prepares/renders before the lease
     * authority flips. Accept its reserved (pending) generation so those frames
     * validate; the outgoing producer's active generation is intentionally
     * rejected here because it is being torn down.
     */
    if (lease->state == DISPLAY_PRESENT_LEASE_HANDOFF) {
        return lease->pending_producer == producer &&
               lease->pending_generation == generation;
    }
    return false;
}

uint32_t display_present_lease_pending_generation(
    const display_present_lease_t *lease)
{
    if (lease == NULL || lease->state != DISPLAY_PRESENT_LEASE_HANDOFF) {
        return 0;
    }
    return lease->pending_generation;
}
