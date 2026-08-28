/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gsp.h"

#define MOSAIC_APP_ROOT_ID 0U
#define MOSAIC_APP_NO_LAUNCH_ACTION UINT16_MAX
/** Synthetic action emitted by the shared Shell for Apps without a scene
 * back button. It must remain distinct from the no-action sentinel. */
#define MOSAIC_APP_SHELL_BACK_ACTION (UINT16_MAX - 1U)

struct mosaic_event;

/** An App-local UI action that opens another registered App directly. */
typedef struct {
    uint16_t action_id;
    const char *target_name;
} mosaic_app_route_t;

typedef struct mosaic_app_descriptor {
    uint16_t id;
    uint16_t launch_action;
    uint16_t back_action;
    /** Explicit routes whose action ids are local to this App bundle. */
    const mosaic_app_route_t *routes;
    size_t route_count;
    /** Sibling routes replace this App without becoming a Back destination. */
    bool routes_without_history;
    /** Physical/simulator Back exits this App without popping its root stack. */
    bool back_exits_app;
    const char *name;
    const char *title;
    const gsp_component_directory_t *directory;
    const gsp_component_directory_t *const *directories;
    uint16_t directory_count;
    uint32_t root_stack_key;
    /** Optional GSP template-instance pool override; zero keeps the default. */
    uint16_t instance_slots;
    bool disable_swipe;
    bool custom_shell;
    /** Root page owns its top header; shared shell keeps only bottom chrome. */
    bool root_header_in_stack;
    /** Optional runtime image target override; zero keeps the GSP default. */
    size_t dynamic_image_slots;
    /** Optional decoded runtime-image cache override; zero lets GSP derive it. */
    size_t image_cache_bytes;
    void (*on_started)(esp_gsp_handle_t ui);
    void (*on_stopping)(esp_gsp_handle_t ui);
    /** Optional business hook; always runs on the Mosaic runtime owner. */
    void (*on_event)(esp_gsp_handle_t ui, const struct mosaic_event *event);
    /** Optional physical Back hook; true means one App-local level was consumed. */
    bool (*on_back)(esp_gsp_handle_t ui, int64_t timestamp_us);
} mosaic_app_descriptor_t;

struct mosaic_logic_ops;

/** Build-generated, platform-neutral package metadata.
 *
 * Paths are component-relative asset keys. Platform asset providers resolve
 * them without exposing filesystem or partition details to App logic.
 */
typedef struct {
    const mosaic_app_descriptor_t *descriptor;
    const char *bundle_path;
    const struct mosaic_logic_ops *logic;
    const char *logic_entry;
    uint32_t tick_ms;
} mosaic_app_package_t;

extern const mosaic_app_descriptor_t *const mosaic_app_registry[];
extern const size_t mosaic_app_registry_count;
extern const mosaic_app_package_t mosaic_app_packages[];
extern const size_t mosaic_app_package_count;

const mosaic_app_descriptor_t *mosaic_app_root(void);
const mosaic_app_descriptor_t *mosaic_app_descriptor(uint16_t id);
const mosaic_app_descriptor_t *mosaic_app_descriptor_for_name(
    const char *name);
const mosaic_app_descriptor_t *mosaic_app_descriptor_for_action(
    uint16_t action_id);
const mosaic_app_package_t *mosaic_app_package_for_descriptor(
    const mosaic_app_descriptor_t *descriptor);
const mosaic_app_package_t *mosaic_app_package_for_name(const char *name);
bool mosaic_app_catalog_validate(void);
bool mosaic_app_route_event(
    const mosaic_app_descriptor_t *active, const esp_gsp_event_t *event,
    const mosaic_app_descriptor_t **out_target);
