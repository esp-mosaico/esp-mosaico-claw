/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "gsp/gsp_component_advanced.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_component.h"
#include "gsp/gsp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The Component Manager holds bounded RAM instances for active
 * components and dispatches through a static type-level driver
 * registry. Descriptors stay in flash, persistent values stay in
 * state slots, and instances exist only while a component owns
 * transient behavior such as capture, physics or playback.
 */

typedef struct gsp_component_manager gsp_component_manager_t;

/* Coalesced driver notifications, consumed outside driver execution. */
#define GSP_COMPONENT_NOTIFY_VALUE    1U
#define GSP_COMPONENT_NOTIFY_FINISHED 2U
#define GSP_COMPONENT_NOTIFY_MOTION   4U

/** Active-instance header; typed transient state lives in the
 *  per-instance extension blob (driver-declared size). */
typedef struct {
    uint16_t entry_index;       /*!< directory entry index */
    uint16_t generation;        /*!< stale-handle guard */
    uint16_t driver_id;
    uint16_t flags;
    uint8_t ext_pool;           /*!< owning extension pool */
    uint16_t ext_slot;          /*!< slot within that pool */
    void *extension;            /*!< driver_extension_size bytes */
} gsp_component_instance_t;

/** One extension size class: a boolean tween needs tens of bytes, a
 *  list (rows, physics) needs hundreds — one uniform stride would
 *  waste capacity x max_size. Activation picks the smallest pool
 *  whose stride fits the driver's extension_size, falling through to
 *  larger ones when it is full. */
typedef struct {
    uint8_t *storage;           /*!< capacity x stride bytes */
    uint16_t stride;
    uint16_t capacity;
} gsp_component_ext_pool_t;

#define GSP_COMPONENT_EXT_POOLS_MAX 2

/** Pointer-ish input event routed to a driver (scene coordinates). */
typedef enum {
    GSP_COMPONENT_INPUT_DOWN = 0,
    GSP_COMPONENT_INPUT_MOVE = 1,
    GSP_COMPONENT_INPUT_UP = 2,
    GSP_COMPONENT_INPUT_CANCEL = 3,
} gsp_component_input_kind_t;

typedef struct {
    uint8_t kind;               /*!< gsp_component_input_kind_t */
    int32_t x;
    int32_t y;
} gsp_component_input_t;

/** Static type-level operations. Every hook may be NULL except
 *  activate/deactivate pairing rules: a driver with extension_size
 *  != 0 must provide both. Drivers only stage transactions and edit
 *  their own extension — never pixels, never raw commands. */
typedef struct {
    gsp_err_t (*activate)(gsp_component_manager_t *manager,
                          gsp_component_instance_t *instance);
    void (*deactivate)(gsp_component_manager_t *manager,
                       gsp_component_instance_t *instance);
    gsp_err_t (*event)(gsp_component_manager_t *manager,
                       gsp_component_instance_t *instance,
                       const gsp_component_input_t *event,
                       gsp_update_transaction_t *transaction);
    gsp_err_t (*cancel)(gsp_component_manager_t *manager,
                        gsp_component_instance_t *instance,
                        gsp_update_transaction_t *transaction);
    gsp_err_t (*tick)(gsp_component_manager_t *manager,
                      gsp_component_instance_t *instance,
                      uint64_t now_us,
                      gsp_update_transaction_t *transaction);
    /** Persistent drivers only: true while the instance needs the
     *  frame tick (e.g. a coasting list). NULL means never busy. */
    bool (*busy)(const gsp_component_instance_t *instance);
    uint16_t extension_size;
    /** Instances live for their component's whole VISIBLE lifetime
     *  (a list's rows) instead of one interaction. They do not keep
     *  the frame tick by existing — only while busy() says so. */
    bool persistent;
} gsp_component_driver_t;

struct gsp_component_manager {
    const esp_gsp_policy_t *policy;
    const esp_gsp_capacity_t *capacity_limits;
    const gsp_component_directory_t *directory;
    gsp_runtime_t *runtime;
    const gsp_component_driver_t *const *drivers;  /*!< by driver_id */
    uint16_t driver_count;
    gsp_component_instance_t *instances;   /*!< caller storage */
    uint16_t capacity;
    gsp_component_ext_pool_t ext_pools[GSP_COMPONENT_EXT_POOLS_MAX];
    uint16_t ext_pool_count;
    /* Pointer capture owner: at most one instance receives MOVE/UP
     * between a claimed DOWN and the release. Cleared on UP/CANCEL,
     * when its source command becomes hidden, on deactivation of the
     * owner, and by cancel_all. Direct/internal captures remain
     * visibility-agnostic. */
    gsp_component_instance_t *capture;
    uint16_t capture_generation;
    uint32_t capture_command;
    bool capture_command_valid;
};

/** Caller owns every storage array (no heap). @p pools are copied
 *  (ascending stride recommended; each driver's extension_size must
 *  fit at least one). directory may be NULL (anonymous/legacy mode):
 *  entry_index becomes a caller-defined id, e.g. a bind id, until
 *  GSB descriptors exist. */
gsp_err_t gsp_component_manager_init(
    gsp_component_manager_t *manager,
    const gsp_component_directory_t *directory,
    gsp_runtime_t *runtime,
    const gsp_component_driver_t *const *drivers, uint16_t driver_count,
    gsp_component_instance_t *instance_storage, uint16_t capacity,
    const gsp_component_ext_pool_t *pools, uint16_t pool_count);

/** Returns the active instance for the entry, activating one when
 *  absent (driver.activate on a zeroed extension). GSP_ERR_NO_MEMORY
 *  when the pool is exhausted. */
gsp_err_t gsp_component_manager_activate(
    gsp_component_manager_t *manager, uint16_t entry_index,
    uint16_t driver_id, gsp_component_instance_t **out_instance);

gsp_component_instance_t *gsp_component_manager_find_active(
    gsp_component_manager_t *manager, uint16_t entry_index);

/** Returns the typed descriptor config when the instance belongs to the
 *  expected component kind, otherwise NULL. */
const void *gsp_component_manager_config(
    const gsp_component_manager_t *manager,
    const gsp_component_instance_t *instance,
    gsp_component_kind_t expected_kind);

/** Like activate, but keyed by (driver_id, tag) OUTSIDE the
 *  descriptor space — the transitional identity for facade objects
 *  without directory entries (a list handle never aliases a slider's
 *  descriptor index). Exits with the GSB semantic sections. */
gsp_err_t gsp_component_manager_activate_anonymous(
    gsp_component_manager_t *manager, uint16_t driver_id,
    uint16_t tag, gsp_component_instance_t **out_instance);

/** Deactivates (driver.deactivate) and recycles the slot; stale
 *  pointers are guarded by the generation bump. */
void gsp_component_manager_deactivate(
    gsp_component_manager_t *manager,
    gsp_component_instance_t *instance);

/** Cancels (driver.cancel, then deactivate) every active instance —
 *  scene switches and teardown. */
void gsp_component_manager_cancel_all(
    gsp_component_manager_t *manager,
    gsp_update_transaction_t *transaction);

/** Runs driver.tick on every active instance that declares one.
 *  Returns true while any instance stays active (keep the frame
 *  tick). */
bool gsp_component_manager_tick(gsp_component_manager_t *manager,
                                uint64_t now_us,
                                gsp_update_transaction_t *transaction);

/** True while any instance needs the frame tick: transient instances
 *  while active, persistent ones only while their busy() says so (an
 *  idle persistent instance exists without holding the tick). */
bool gsp_component_manager_active(const gsp_component_manager_t *manager);

/** Routes one input event to the instance's driver. The generation
 *  guards stale pointers across recycling; a mismatch is a no-op
 *  (GSP_ERR_NOT_FOUND). */
gsp_err_t gsp_component_manager_event(gsp_component_manager_t *manager,
                                      gsp_component_instance_t *instance,
                                      uint16_t generation,
                                      const gsp_component_input_t *event,
                                      gsp_update_transaction_t *transaction);

/** Makes @p instance the pointer-capture owner (NULL releases). */
void gsp_component_manager_capture_set(gsp_component_manager_t *manager,
                                       gsp_component_instance_t *instance);

/** Makes @p instance the pointer-capture owner and ties its lifetime to
 *  @p command_index. A hidden ancestor visibility group cancels the
 *  capture on the next manager tick or before the next event reaches
 *  the driver. */
void gsp_component_manager_capture_set_guarded(
    gsp_component_manager_t *manager,
    gsp_component_instance_t *instance,
    uint32_t command_index);

/** True while a live instance owns the pointer capture. */
bool gsp_component_manager_capture_active(
    const gsp_component_manager_t *manager);

/** Routes @p event to the capture owner; UP/CANCEL release the
 *  capture (a driver deactivating itself releases it too).
 *  GSP_ERR_NOT_FOUND when nothing holds the capture. */
gsp_err_t gsp_component_manager_capture_event(
    gsp_component_manager_t *manager,
    const gsp_component_input_t *event,
    gsp_update_transaction_t *transaction);

#ifdef __cplusplus
}
#endif
