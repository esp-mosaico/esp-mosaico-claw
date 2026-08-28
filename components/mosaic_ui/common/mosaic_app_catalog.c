/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mosaic_app_catalog.h"

#include <stddef.h>
#include <string.h>

const mosaic_app_descriptor_t *mosaic_app_root(void)
{
    return mosaic_app_descriptor(MOSAIC_APP_ROOT_ID);
}

const mosaic_app_descriptor_t *mosaic_app_descriptor(uint16_t id)
{
    for (size_t index = 0; index < mosaic_app_registry_count; ++index) {
        if (mosaic_app_registry[index]->id == id) {
            return mosaic_app_registry[index];
        }
    }
    return NULL;
}

const mosaic_app_descriptor_t *mosaic_app_descriptor_for_name(
    const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < mosaic_app_registry_count; ++index) {
        if (strcmp(mosaic_app_registry[index]->name, name) == 0) {
            return mosaic_app_registry[index];
        }
    }
    return NULL;
}

const mosaic_app_descriptor_t *mosaic_app_descriptor_for_action(
    uint16_t action_id)
{
    if (action_id == MOSAIC_APP_NO_LAUNCH_ACTION) {
        return NULL;
    }
    for (size_t index = 0; index < mosaic_app_registry_count; ++index) {
        const mosaic_app_descriptor_t *app = mosaic_app_registry[index];
        if (app->id != MOSAIC_APP_ROOT_ID &&
                app->launch_action == action_id) {
            return app;
        }
    }
    return NULL;
}

const mosaic_app_package_t *mosaic_app_package_for_descriptor(
    const mosaic_app_descriptor_t *descriptor)
{
    if (descriptor == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < mosaic_app_package_count; ++index) {
        if (mosaic_app_packages[index].descriptor == descriptor) {
            return &mosaic_app_packages[index];
        }
    }
    return NULL;
}

const mosaic_app_package_t *mosaic_app_package_for_name(const char *name)
{
    return mosaic_app_package_for_descriptor(
        mosaic_app_descriptor_for_name(name));
}

bool mosaic_app_catalog_validate(void)
{
    static const char *const required_apps[] = {
        "welcome",
        "ai_create", "camera", "works", "settings", "setup_center",
    };
    size_t root_count = 0;
    if (mosaic_app_registry_count != mosaic_app_package_count) {
        return false;
    }
    for (size_t index = 0; index < mosaic_app_registry_count; ++index) {
        const mosaic_app_descriptor_t *app = mosaic_app_registry[index];
        if (app == NULL || app->name == NULL || app->name[0] == '\0' ||
                (app->route_count != 0 && app->routes == NULL) ||
                (app->directory == NULL &&
                 (app->directories == NULL || app->directory_count == 0)) ||
                (app->directories != NULL && app->directory_count == 0) ||
                (app->id != MOSAIC_APP_ROOT_ID &&
                 (app->title == NULL || app->title[0] == '\0'))) {
            return false;
        }
        for (size_t route_index = 0; route_index < app->route_count;
             ++route_index) {
            const mosaic_app_route_t *route = &app->routes[route_index];
            if (route->target_name == NULL || route->target_name[0] == '\0' ||
                    route->action_id == app->back_action ||
                    mosaic_app_descriptor_for_name(route->target_name) == NULL ||
                    strcmp(route->target_name, app->name) == 0) {
                return false;
            }
            for (size_t other_route = route_index + 1;
                 other_route < app->route_count; ++other_route) {
                if (route->action_id == app->routes[other_route].action_id) {
                    return false;
                }
            }
        }
        const mosaic_app_package_t *package =
            mosaic_app_package_for_descriptor(app);
        if (package == NULL || package->bundle_path == NULL ||
                package->bundle_path[0] == '\0' || package->logic == NULL) {
            return false;
        }
        root_count += app->id == MOSAIC_APP_ROOT_ID;
        for (size_t other = index + 1;
             other < mosaic_app_registry_count; ++other) {
            const mosaic_app_descriptor_t *candidate =
                mosaic_app_registry[other];
            if (candidate == NULL || candidate->id == app->id ||
                    strcmp(candidate->name, app->name) == 0 ||
                    (app->id != MOSAIC_APP_ROOT_ID &&
                     candidate->id != MOSAIC_APP_ROOT_ID &&
                     app->launch_action != MOSAIC_APP_NO_LAUNCH_ACTION &&
                     candidate->launch_action == app->launch_action)) {
                return false;
            }
        }
    }
    const mosaic_app_descriptor_t *root = mosaic_app_root();
    if (root_count != 1 || root == NULL ||
            root->launch_action != MOSAIC_APP_NO_LAUNCH_ACTION) {
        return false;
    }
    for (size_t index = 0;
         index < sizeof(required_apps) / sizeof(required_apps[0]); ++index) {
        if (mosaic_app_descriptor_for_name(required_apps[index]) == NULL) {
            return false;
        }
    }
    return true;
}

bool mosaic_app_route_event(
    const mosaic_app_descriptor_t *active, const esp_gsp_event_t *event,
    const mosaic_app_descriptor_t **out_target)
{
    if (active == NULL || event == NULL || out_target == NULL ||
            event->type != ESP_GSP_EVENT_CALL) {
        return false;
    }
    if (active->id == MOSAIC_APP_ROOT_ID) {
        const mosaic_app_descriptor_t *app =
            mosaic_app_descriptor_for_action(event->action_id);
        if (app == NULL) {
            return false;
        }
        *out_target = app;
        return true;
    }
    for (size_t index = 0; index < active->route_count; ++index) {
        const mosaic_app_route_t *route = &active->routes[index];
        if (event->action_id != route->action_id) {
            continue;
        }
        const mosaic_app_descriptor_t *target =
            mosaic_app_descriptor_for_name(route->target_name);
        if (target == NULL) {
            return false;
        }
        *out_target = target;
        return true;
    }
    if (event->action_id == active->back_action) {
        *out_target = mosaic_app_root();
        return true;
    }
    return false;
}
