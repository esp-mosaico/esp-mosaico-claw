/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Select the power-state surface. Unplugged shows AOD; charging shows CHRG.
 * Battery UI follows system.power capability publishes
 * (platform sampler + notify-on-change).
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
 */
void mosaic_hub_show_board_insert(
    char side, const char *friendly_name, const char *capability,
    const char *open_app_name);

/** Thread-safe request to open Hub and show an extension-board insertion. */
void mosaic_hub_request_board_insert(
    char side, const char *friendly_name, const char *capability,
    const char *open_app_name);

/** Thread-safe request to show or restore the left Quick Settings camera slot. */
void mosaic_hub_request_quick_slot_camera(char side, bool occupied);
