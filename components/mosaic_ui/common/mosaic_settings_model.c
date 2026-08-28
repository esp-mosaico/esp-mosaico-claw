/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_settings.h"

#include <string.h>

bool mosaic_settings_llm_is_configured(
    const mosaic_settings_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->llm.backend[0] == '\0' ||
            snapshot->llm.model[0] == '\0') {
        return false;
    }

    return snapshot->llm.api_key_configured ||
           strcmp(snapshot->llm.backend, "trial") == 0;
}
