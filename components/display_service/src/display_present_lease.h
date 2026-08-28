/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    DISPLAY_PRESENT_LEASE_EMPTY = 0,
    DISPLAY_PRESENT_LEASE_ACTIVE,
    DISPLAY_PRESENT_LEASE_HANDOFF,
    DISPLAY_PRESENT_LEASE_FAULT,
} display_present_lease_state_t;

typedef struct {
    const void *active_producer;
    const void *pending_producer;
    uint32_t generation;
    /*
     * Generation that commit_handoff will publish for pending_producer. Computed
     * at begin_handoff so the incoming producer can render (and be validated)
     * against the exact generation before the lease authority flips to it.
     * Only meaningful while state == DISPLAY_PRESENT_LEASE_HANDOFF.
     */
    uint32_t pending_generation;
    display_present_lease_state_t state;
} display_present_lease_t;

void display_present_lease_init(display_present_lease_t *lease);

esp_err_t display_present_lease_activate(
    display_present_lease_t *lease,
    const void *producer,
    uint32_t *out_generation);

esp_err_t display_present_lease_begin_handoff(
    display_present_lease_t *lease,
    const void *current_producer,
    uint32_t current_generation,
    const void *next_producer);

esp_err_t display_present_lease_commit_handoff(
    display_present_lease_t *lease,
    const void *next_producer,
    uint32_t *out_generation);

void display_present_lease_abort_handoff(
    display_present_lease_t *lease);

void display_present_lease_fault(display_present_lease_t *lease);

/**
 * Start baseline recovery from FAULT: reserve a pending generation for
 * producer and enter HANDOFF so it can activate/render before authority
 * flips. On activate failure call display_present_lease_fault() again
 * (not abort_handoff — that would incorrectly return to ACTIVE).
 */
esp_err_t display_present_lease_begin_recover(
    display_present_lease_t *lease,
    const void *producer);

/**
 * One-shot recover for callers that do not need a mid-flight activate:
 * begin_recover + commit_handoff. Prefer begin_recover → activate →
 * commit_handoff when the producer must render before the lease flips.
 */
esp_err_t display_present_lease_recover(
    display_present_lease_t *lease,
    const void *producer,
    uint32_t *out_generation);

bool display_present_lease_validate(
    const display_present_lease_t *lease,
    const void *producer,
    uint32_t generation);

/*
 * Generation the pending producer should render under while a handoff is in
 * flight. Returns 0 when no handoff is pending. Lets the incoming producer
 * prepare/render before commit_handoff flips the lease authority.
 */
uint32_t display_present_lease_pending_generation(
    const display_present_lease_t *lease);
