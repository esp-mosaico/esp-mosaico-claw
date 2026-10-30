/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */
#pragma once

#include "esp_gsp_updates.h"
#include <stdatomic.h>

struct gsp_ui_core;
struct gsp_runtime;
struct gsp_app;

#define GSP_UPDATE_MAX_COMMANDS 128U
#define GSP_UPDATE_MAX_PENDING 16U

static inline uint64_t gsp_update_allocate_id(atomic_uint_fast64_t *counter)
{
    uint_fast64_t id = atomic_load_explicit(counter, memory_order_relaxed);
    while (id != 0) {
        uint_fast64_t next = id == UINT64_MAX ? 0 : id + 1;
        if (atomic_compare_exchange_weak_explicit(counter, &id, next,
                memory_order_relaxed, memory_order_relaxed)) {
            return id;
        }
    }
    return 0;
}

typedef struct {
    esp_gsp_update_command_t command;
    atomic_int image_result;
    uint16_t expected_ref;
    uint32_t expected_resource;
    gsp_value_t expected_value;
    bool transferred;
    bool overwritten;
    bool text_matches;
} gsp_update_operation_t;

typedef struct {
    gsp_value_t value;
    bool overwritten;
} gsp_update_property_expectation_t;

struct esp_gsp_update_receipt {
    atomic_uint references;
    atomic_uint publication;
    _Atomic(uint32_t) published[(sizeof(esp_gsp_update_status_t) + 3U) / 4U];
    atomic_bool cancel_requested;
    esp_gsp_update_status_t status;
    esp_gsp_update_options_t options;
    int64_t deadline_us;
    struct gsp_ui_core *owner;
    struct esp_gsp_update_receipt *next;
    union {
        gsp_update_operation_t *operations;
        gsp_update_property_expectation_t *property_expectations;
    };
    gsp_component_property_update_t *properties;
    uint32_t revision;
    bool accepted;
    bool dispatched;
    bool candidate;
    bool property_group;
};

typedef struct {
    atomic_uint count;
    esp_gsp_update_receipt_t *head;
    esp_gsp_update_receipt_t *applying;
    bool servicing;
    uint64_t rendered_frame;
    uint32_t rendered_revision;
    uint32_t rendered_generation;
    uint32_t rendered_frame_generation;
} gsp_update_receipts_t;

typedef struct {
    void (*dispatch)(struct gsp_ui_core *, esp_gsp_update_receipt_t *);
    void (*command_release)(esp_gsp_update_receipt_t *);
    void (*service)(struct gsp_ui_core *);
    void (*before_frame)(struct gsp_ui_core *);
    void (*after_frame)(struct gsp_ui_core *, gsp_err_t, uint64_t, uint32_t);
    void (*abort)(struct gsp_ui_core *);
    void (*text_changed)(struct gsp_ui_core *, uint16_t, const char *);
    bool (*image_cancelled)(esp_gsp_image_complete_cb_t, void *);
    bool (*requires_present)(struct gsp_ui_core *);
    void (*scene_changed)(struct gsp_ui_core *);
} gsp_update_receipt_services_t;

/* An unused optional service must not extract its archive member. */
#ifdef ESP_PLATFORM
extern const gsp_update_receipt_services_t gsp_update_receipt_services __attribute__((weak));
#else
extern const gsp_update_receipt_services_t gsp_update_receipt_services;
#endif

static inline bool gsp_updates_available(void)
{
#ifdef ESP_PLATFORM
    return &gsp_update_receipt_services != NULL;
#else
    return true;
#endif
}

static inline void gsp_updates_dispatch(struct gsp_ui_core *core, esp_gsp_update_receipt_t *receipt)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.dispatch(core, receipt);
    }
}

static inline void gsp_updates_command_release(esp_gsp_update_receipt_t *receipt)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.command_release(receipt);
    }
}

static inline void gsp_updates_service(struct gsp_ui_core *core)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.service(core);
    }
}

static inline void gsp_updates_before_frame(struct gsp_ui_core *core)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.before_frame(core);
    }
}

static inline void gsp_updates_after_frame(struct gsp_ui_core *core, gsp_err_t result, uint64_t frame, uint32_t generation)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.after_frame(core, result, frame, generation);
    }
}

static inline void gsp_updates_abort(struct gsp_ui_core *core)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.abort(core);
    }
}

static inline void gsp_updates_text_changed(struct gsp_ui_core *core, uint16_t bind, const char *text)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.text_changed(core, bind, text);
    }
}

static inline bool gsp_updates_image_cancelled(esp_gsp_image_complete_cb_t callback, void *ctx)
{
    return gsp_updates_available() && gsp_update_receipt_services.image_cancelled(callback, ctx);
}

static inline void gsp_updates_scene_changed(struct gsp_ui_core *core)
{
    if (gsp_updates_available()) {
        gsp_update_receipt_services.scene_changed(core);
    }
}

gsp_err_t esp_gsp_apply_property_updates(struct gsp_ui_core *core,
        struct gsp_runtime *runtime, uint16_t scene,
        const gsp_component_property_update_t *updates, size_t count);
gsp_err_t esp_gsp_apply_text(struct gsp_ui_core *core, uint16_t bind,
                             const char *text);
gsp_err_t gsp_app_resolve_navigation(struct gsp_app *app);
