/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gsp/gsp_types.h"
#include "gsp/gsp_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Merging insert; the caller must have checked dirty capacity. */
void gsp_runtime_add_dirty(gsp_runtime_t *runtime, gsp_rect_t rect);

/** True when the command sits in a currently-hidden vis group (its
 *  pixels are not drawn); mirrors the renderer's visibility test. */
bool gsp_runtime_command_hidden(const gsp_runtime_t *runtime,
                                uint32_t command_index);

bool gsp_runtime_dirty_capacity_ok(gsp_runtime_t *runtime,
                                   size_t additional);

void gsp_runtime_bump_revision(gsp_runtime_t *runtime);
void gsp_runtime_bump_transform_revision(gsp_runtime_t *runtime, uint16_t group_id);

#ifdef __cplusplus
}
#endif
