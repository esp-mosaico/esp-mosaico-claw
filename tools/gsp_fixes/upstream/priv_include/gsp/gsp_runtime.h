/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GSP_RUNTIME_ANY_VERSION UINT16_MAX

typedef struct {
    uint16_t expected_version_major;
    uint32_t target_profile_id;
    bool enforce_target_profile;
    bool skip_crc;
} gsp_scene_open_config_t;

typedef struct {
    const uint8_t *package_data;
    size_t package_size;
    const uint8_t *command_data;
    const uint8_t *state_defaults;
    const uint8_t *bind_index;
    const uint8_t *command_ref_lists;
    size_t command_ref_lists_size;
    const uint8_t *action_table;
    const uint8_t *action_params;
    size_t action_params_size;
    const uint8_t *hit_index;
    const uint8_t *resource_refs;
    const uint8_t *vis_groups;
    const uint8_t *property_table;   /*!< GSB 1.1, property-index order */
    const uint8_t *effect_table;     /*!< GSB 1.1 property effect spans */
    const uint8_t *behavior_targets; /*!< GSB 1.1 action -> property */
    const uint8_t *command_effect_spans; /*!< GSB 1.2 command order */
    const uint8_t *render_effects;       /*!< GSB 1.2 compact effects */
    const uint8_t *transform_groups;        /*!< GSB 2.0 retained transforms */
    const uint8_t *command_groups;       /*!< one u16 group per command */
    const uint8_t *hit_geometry;          /*!< optional dynamic command per hit */
    const uint8_t *hit_groups;           /*!< one u16 group per hit */
    const uint8_t *templates;
    const uint8_t *glyph_runs;
    size_t glyph_runs_size;
    size_t state_defaults_size;
    uint32_t template_count;
    uint32_t command_count;
    uint32_t action_count;
    uint32_t hit_count;
    uint32_t resource_ref_count;
    uint32_t vis_group_count;
    uint32_t property_count;
    uint32_t effect_count;
    uint32_t render_effect_count;
    uint32_t transform_group_count;
    uint16_t state_slot_count;
    uint16_t bind_count;
    uint16_t scene_id;
    uint16_t screen_width;
    uint16_t screen_height;
    uint16_t version_major;
    uint16_t version_minor;
    uint8_t color_profile;
    uint8_t flags;
    bool has_overlay_layer;
    uint32_t background_color;
    uint8_t background_alpha;
    uint32_t target_profile_id;
    uint32_t content_id;
    /* TILE_INDEX entries (tiles_x * tiles_y, row-major) after the 8-byte
     * section header; NULL when the scene has no tile index. Tile lists
     * are u16 command indices in ascending z-order in CMD_REF_LISTS. */
    const uint8_t *tile_entries;
    uint16_t tile_w;
    uint16_t tile_h;
    uint16_t tiles_x;
    uint16_t tiles_y;
} gsp_scene_view_t;

gsp_err_t gsp_scene_open(const void *data, size_t data_size,
                         const gsp_scene_open_config_t *config,
                         gsp_scene_view_t *out_scene);

/**
 * Command references covering @p region, counted through the compiled
 * tile index. Returns the scene's full command_count when no tile
 * index exists or the region covers the whole screen. A command
 * spanning several tiles counts once per touched tile — an upper
 * bound, by design (region cost estimation, not exact scheduling).
 */
uint64_t gsp_scene_command_refs_in_region(const gsp_scene_view_t *scene,
        gsp_rect_t region);

#ifdef __cplusplus
}
#endif
