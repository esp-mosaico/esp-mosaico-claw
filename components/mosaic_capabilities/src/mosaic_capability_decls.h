/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* Private view of the capability declaration table.
 *
 * The table is the security anchor of this layer: it is the only place that
 * binds a capability name to its permission bits, its read contract and its
 * command set. Providers declare none of this and therefore cannot widen
 * their own reach.
 */

#include <stdbool.h>
#include <stddef.h>

#include "mosaic_capability.h"

typedef struct {
    const char *name;
    mosaic_capability_mask_t read_permission;
    /** NULL when the capability has no read verb. */
    const mosaic_capability_contract_t *read_contract;
    const mosaic_capability_command_t *commands;
    uint8_t command_count;
    /** True when the provider fans samples out through publish(). */
    bool publishes;
} mosaic_capability_declaration_t;

typedef struct {
    const char *name;
    mosaic_capability_mask_t bit;
} mosaic_capability_permission_def_t;

const mosaic_capability_declaration_t *mosaic_capability_declarations(
    size_t *out_count);
const mosaic_capability_declaration_t *mosaic_capability_declaration_for(
    const char *name);
const mosaic_capability_permission_def_t *mosaic_capability_permissions(
    size_t *out_count);
