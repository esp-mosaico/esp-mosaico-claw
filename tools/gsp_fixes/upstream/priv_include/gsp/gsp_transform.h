/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */
#pragma once

#include "gsp/gsp_runtime.h"
#include "gsp/gsp_update.h"

typedef struct {
    int32_t dx;
    int32_t dy;
    gsp_rect_t viewport;
    bool has_viewport;
} gsp_transform_resolved_t;

typedef enum {
    GSP_TRANSFORM_GROUP_VIEWPORT = 0,
    GSP_TRANSFORM_GROUP_STATIC,
} gsp_transform_group_kind_t;

gsp_err_t gsp_transform_resolve(const gsp_scene_view_t *scene,
                                const gsp_state_t *states,
                                size_t state_count, uint16_t group_id,
                                gsp_transform_resolved_t *out);

gsp_err_t gsp_transform_resolve_command(const gsp_scene_view_t *scene,
                                        const gsp_state_t *states,
                                        size_t state_count,
                                        uint32_t command_index,
                                        gsp_rect_t *bbox,
                                        gsp_rect_t *viewport,
                                        bool *has_viewport);

/** Last resolved command group; valid only while states are unchanged. */
typedef struct {
    uint16_t group;
    gsp_transform_resolved_t resolved;
} gsp_transform_memo_t;

#define GSP_TRANSFORM_MEMO_INIT {.group = UINT16_MAX}

/** As gsp_transform_resolve_command(), reusing @p memo for runs of commands
 *  in the same group. */
gsp_err_t gsp_transform_resolve_command_memo(const gsp_scene_view_t *scene,
        const gsp_state_t *states,
        size_t state_count,
        uint32_t command_index,
        gsp_transform_memo_t *memo,
        gsp_rect_t *bbox,
        gsp_rect_t *viewport,
        bool *has_viewport);

gsp_err_t gsp_transform_resolve_hit(const gsp_scene_view_t *scene,
                                    const gsp_state_t *states,
                                    size_t state_count, uint32_t hit_index,
                                    gsp_transform_resolved_t *out);

/** True when a command belongs to @p group_id or one of its descendants. */
bool gsp_transform_command_in_group(const gsp_scene_view_t *scene,
                                    uint32_t command_index,
                                    uint16_t group_id);

/** As above for a hit-table entry; hit indices are a separate space. */
bool gsp_transform_hit_in_group(const gsp_scene_view_t *scene,
                                uint32_t hit_index, uint16_t group_id);
bool gsp_transform_group_in_group(const gsp_scene_view_t *scene,
                                  uint16_t candidate_group,
                                  uint16_t ancestor_group);

/** Read this group's local offset, excluding all parent transforms. */
gsp_err_t gsp_transform_get_offset(const gsp_runtime_t *runtime,
                                   uint16_t group_id,
                                   int32_t *out_x, int32_t *out_y);

gsp_err_t gsp_transform_set_offset(gsp_runtime_t *runtime, uint16_t group_id,
                                   int32_t x, int32_t y);

gsp_err_t gsp_transform_group_kind(const gsp_scene_view_t *scene,
                                   uint16_t group_id,
                                   gsp_transform_group_kind_t *out_kind);

/** Absolute authored position API for STATIC groups. */
gsp_err_t gsp_transform_get_position(const gsp_runtime_t *runtime,
                                     uint16_t group_id,
                                     int32_t *out_x, int32_t *out_y);
gsp_err_t gsp_transform_set_position(gsp_runtime_t *runtime, uint16_t group_id,
                                     int32_t x, int32_t y);

/** Cache only ancestors whose static bounds need live descendant damage. */
void gsp_transform_init_dirty_groups(gsp_runtime_t *runtime);

/** Adds the current clipped bbox of a STATIC group to runtime damage. */
gsp_err_t gsp_transform_add_static_dirty(gsp_runtime_t *runtime,
        uint16_t group_id);
