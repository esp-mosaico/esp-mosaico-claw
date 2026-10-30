/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gsp.h"
#include "mosaico_module_mgr.h"

#define MOSAIC_HUB_INSERT_PAGE 2U

/*
 * Select the power-state surface. Unplugged shows AOD; charging shows CHRG.
 * Battery UI follows mosaic_settings_subscribe_battery() publishes
 * (platform get/info sampler + notify-on-change).
 * The transition is a hard cut and is safe to call repeatedly.
 */
void mosaic_hub_set_charging(bool charging);

/** Opens the modal Lock Screen in AOD or charging mode. */
void mosaic_hub_show_lock_screen(bool charging);
/** Snapshot the current battery mode into Lock Screen before display sleep. */
void mosaic_hub_lock_screen(void);

/** Handles generated Lock Screen callbacks from the active launcher. */
bool mosaic_hub_handle_action(uint16_t action_id);

/** Preview the reviewed extension-board insertion interaction on Hub.
 * side must be 'L' or 'R'. open_app_name selects the Open button target.
 * Call from the Hub render context.
 */
void mosaic_hub_show_board_insert(
    char side, const char *friendly_name, const char *capability,
    const char *open_app_name);

/** Thread-safe slot publication. Initial discovery is silent; genuine runtime
 * insertions share side feedback, then use the original single-board page
 * one application at a time on the awake launcher. */
void mosaic_hub_update_module(const mosaico_module_mgr_info_t *info);

/** Call from the Hub render context, or after successfully pausing it while
 * the loader owns its lifecycle lock. Cancel transient side feedback while
 * retaining an already open introduction like any other foreground page. */
esp_gsp_err_t mosaic_hub_quiet_module_notice(esp_gsp_handle_t ui);

/** Thread-safe request to update the left Quick Settings camera slot. */
void mosaic_hub_request_quick_slot_camera(char side, bool occupied);
