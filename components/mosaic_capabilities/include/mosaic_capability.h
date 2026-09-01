/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Permissions
 *
 * One bit per permission name. The name -> bit binding lives in the
 * registry declaration table, never in a provider or an App, so a provider
 * cannot widen its own reach and manifest validation has one source of
 * truth.
 * ======================================================================== */

typedef uint64_t mosaic_capability_mask_t;

#define MOSAIC_CAP_BIT(index) (UINT64_C(1) << (index))

#define MOSAIC_CAP_SENSOR_IMU_READ           MOSAIC_CAP_BIT(0)
#define MOSAIC_CAP_SYSTEM_BATTERY_READ       MOSAIC_CAP_BIT(1)
#define MOSAIC_CAP_SYSTEM_TIME_READ          MOSAIC_CAP_BIT(2)
#define MOSAIC_CAP_SYSTEM_STATUS_READ        MOSAIC_CAP_BIT(3)
#define MOSAIC_CAP_SYSTEM_DISPLAY_READ       MOSAIC_CAP_BIT(4)
#define MOSAIC_CAP_SYSTEM_DISPLAY_CONTROL    MOSAIC_CAP_BIT(5)
#define MOSAIC_CAP_SYSTEM_AUDIO_READ         MOSAIC_CAP_BIT(6)
#define MOSAIC_CAP_SYSTEM_AUDIO_CONTROL      MOSAIC_CAP_BIT(7)
#define MOSAIC_CAP_SYSTEM_HAPTIC_READ        MOSAIC_CAP_BIT(8)
#define MOSAIC_CAP_SYSTEM_HAPTIC_CONTROL     MOSAIC_CAP_BIT(9)
#define MOSAIC_CAP_SYSTEM_POWER_READ         MOSAIC_CAP_BIT(10)
#define MOSAIC_CAP_SYSTEM_UPDATE_READ        MOSAIC_CAP_BIT(11)
#define MOSAIC_CAP_SYSTEM_UPDATE_CONTROL     MOSAIC_CAP_BIT(12)
#define MOSAIC_CAP_SYSTEM_LIFECYCLE_READ     MOSAIC_CAP_BIT(13)
#define MOSAIC_CAP_SYSTEM_LIFECYCLE_CONTROL  MOSAIC_CAP_BIT(14)
#define MOSAIC_CAP_NET_WIFI_READ             MOSAIC_CAP_BIT(15)
#define MOSAIC_CAP_NET_WIFI_CONTROL          MOSAIC_CAP_BIT(16)
#define MOSAIC_CAP_NET_PROVISIONING_READ     MOSAIC_CAP_BIT(17)
#define MOSAIC_CAP_NET_PROVISIONING_CONTROL  MOSAIC_CAP_BIT(18)
#define MOSAIC_CAP_CONFIG_AGENT_READ         MOSAIC_CAP_BIT(19)
#define MOSAIC_CAP_MEDIA_BLUETOOTH_READ      MOSAIC_CAP_BIT(20)
#define MOSAIC_CAP_MEDIA_BLUETOOTH_CONTROL   MOSAIC_CAP_BIT(21)
#define MOSAIC_CAP_MEDIA_PLAYER_READ         MOSAIC_CAP_BIT(22)
#define MOSAIC_CAP_MEDIA_PLAYER_CONTROL      MOSAIC_CAP_BIT(23)
#define MOSAIC_CAP_NET_WEATHER_READ          MOSAIC_CAP_BIT(24)
#define MOSAIC_CAP_NET_WEATHER_CONTROL       MOSAIC_CAP_BIT(25)

/* ========================================================================
 * Contracts
 *
 * A capability payload is an ordinary C struct owned by a domain contract
 * header. This layer never sees the struct definition; it carries an id, a
 * size and a flat field table. Native callers cast the payload directly,
 * while script bridges and serializers walk the field table, so adding a
 * capability never touches this header or any script glue.
 * ======================================================================== */

typedef enum {
    MOSAIC_CAP_FIELD_BOOL = 0,
    MOSAIC_CAP_FIELD_I32,
    MOSAIC_CAP_FIELD_I64,
    MOSAIC_CAP_FIELD_U32,
    MOSAIC_CAP_FIELD_F32,
    /** Fixed-size char array embedded in the payload struct. */
    MOSAIC_CAP_FIELD_STRING,
    /** Fixed-size array of a nested contract. */
    MOSAIC_CAP_FIELD_ARRAY,
} mosaic_capability_field_type_t;

struct mosaic_capability_contract;

typedef struct {
    const char *name;
    mosaic_capability_field_type_t type;
    uint16_t offset;
    /** Byte size of the member; also the capacity of a STRING field. */
    uint16_t size;
    /** ARRAY only: element contract and element count. */
    const struct mosaic_capability_contract *element;
    uint16_t element_count;
} mosaic_capability_field_t;

typedef struct mosaic_capability_contract {
    /** Stable versioned id, e.g. "system.battery/v1". */
    const char *id;
    uint16_t size;
    const mosaic_capability_field_t *fields;
    uint8_t field_count;
} mosaic_capability_contract_t;

#define MOSAIC_CAP_FIELD(struct_type, member, field_type)                    \
    {                                                                        \
        .name = #member,                                                     \
        .type = (field_type),                                                \
        .offset = (uint16_t)offsetof(struct_type, member),                   \
        .size = (uint16_t)sizeof(((struct_type *)0)->member),                \
        .element = NULL,                                                     \
        .element_count = 0,                                                  \
    }

#define MOSAIC_CAP_ARRAY_FIELD(struct_type, member, element_contract)        \
    {                                                                        \
        .name = #member,                                                     \
        .type = MOSAIC_CAP_FIELD_ARRAY,                                      \
        .offset = (uint16_t)offsetof(struct_type, member),                   \
        .size = (uint16_t)sizeof(((struct_type *)0)->member),                \
        .element = (element_contract),                                       \
        .element_count = (uint16_t)(sizeof(((struct_type *)0)->member) /     \
            sizeof(((struct_type *)0)->member[0])),                          \
    }

#define MOSAIC_CAP_CONTRACT(contract_id, struct_type, field_table)           \
    {                                                                        \
        .id = (contract_id),                                                 \
        .size = (uint16_t)sizeof(struct_type),                               \
        .fields = (field_table),                                             \
        .field_count =                                                       \
            (uint8_t)(sizeof(field_table) / sizeof((field_table)[0])),       \
    }

/* ========================================================================
 * Commands
 *
 * Declared by the registry table, not by the provider. `async` means invoke
 * only accepts the request and returns ESP_ERR_NOT_FINISHED; the outcome
 * reaches consumers through the capability publish stream.
 * ======================================================================== */

typedef struct {
    const char *name;
    uint16_t command;
    mosaic_capability_mask_t permission;
    /** NULL when the command takes no arguments / returns no result. */
    const mosaic_capability_contract_t *args;
    const mosaic_capability_contract_t *result;
    bool async;
} mosaic_capability_command_t;

/* ========================================================================
 * Blobs
 *
 * Variable-size read-only data such as a JPEG frame or album cover cannot
 * travel in a fixed payload struct. A consumer borrows a buffer owned by
 * the provider and releases it once the data has been handed to the UI.
 * ======================================================================== */

typedef struct {
    const void *data;
    size_t size;
    /** Provider-owned cookie returned to release(). */
    void *token;
} mosaic_capability_blob_t;

/* ========================================================================
 * Provider side
 * ======================================================================== */

/** Backend implementation of one capability.
 *
 * `read` fills a payload matching the capability read contract. `invoke`
 * executes a declared command and returns ESP_ERR_NOT_SUPPORTED for one it
 * does not implement. `set_active` is an optional hint so a provider can
 * start and stop sampling as subscribers come and go. `borrow` / `release`
 * are optional and required only by blob capabilities.
 *
 * Every callback runs in the caller's context with no registry lock held.
 * Implementations must be non-blocking and must never call into UI or GSP.
 */
typedef struct {
    esp_err_t (*read)(void *user_ctx, void *out_payload, size_t payload_size);
    esp_err_t (*invoke)(void *user_ctx, uint16_t command,
        const void *args, size_t args_size,
        void *out_result, size_t result_size);
    esp_err_t (*set_active)(void *user_ctx, bool active);
    esp_err_t (*borrow)(void *user_ctx, uint16_t blob_id, uint32_t index,
        mosaic_capability_blob_t *out_blob);
    void (*release)(void *user_ctx, mosaic_capability_blob_t *blob);
} mosaic_capability_ops_t;

typedef struct {
    /** Must match a name in the registry declaration table. */
    const char *name;
    const mosaic_capability_ops_t *ops;
    void *user_ctx;
} mosaic_capability_provider_t;

/** Register a provider during platform initialization.
 *
 * The registry copies the name and validates it against the declaration
 * table. Re-registering a name is allowed only for the same ops table and
 * user context, and never while a call is active. Replacing an owner
 * requires unregistering the old provider first.
 */
esp_err_t mosaic_capability_register(
    const mosaic_capability_provider_t *provider);

/** Remove an idle provider only when its name and owner context match.
 *
 * Returns ESP_ERR_INVALID_STATE while a callback is active. The owner must
 * retry successfully before releasing user_ctx. Registry APIs are
 * task-safe, but are not ISR APIs.
 */
esp_err_t mosaic_capability_unregister(const char *name, void *user_ctx);

/** Fan out a new sample to every subscriber of this capability.
 *
 * Runs synchronously in the publisher's context. Subscriber callbacks are
 * therefore required to be non-blocking; App-facing layers must marshal the
 * value onto the Mosaic runtime owner before touching UI state.
 */
void mosaic_capability_publish(
    const char *name, const void *payload, size_t payload_size);

/* ========================================================================
 * Consumer side
 *
 * `granted` is the App package permission mask. Every entry point enforces
 * it, so Native Apps and script Apps are held to the same manifest.
 * ======================================================================== */

esp_err_t mosaic_capability_read(const char *name,
    mosaic_capability_mask_t granted, void *out_payload, size_t payload_size);

/** Execute a declared command.
 *
 * Returns ESP_ERR_NOT_FINISHED when an async command was accepted; the
 * outcome arrives through the capability publish stream.
 */
esp_err_t mosaic_capability_invoke(const char *name,
    mosaic_capability_mask_t granted, const char *command,
    const void *args, size_t args_size,
    void *out_result, size_t result_size);

/** Borrow a provider-owned read-only buffer. Always pair with release. */
esp_err_t mosaic_capability_borrow(const char *name,
    mosaic_capability_mask_t granted, uint16_t blob_id, uint32_t index,
    mosaic_capability_blob_t *out_blob);

void mosaic_capability_release(
    const char *name, mosaic_capability_blob_t *blob);

typedef struct mosaic_capability_subscription_t
    *mosaic_capability_subscription_handle_t;

typedef void (*mosaic_capability_event_cb_t)(void *user_ctx,
    const char *name, const void *payload, size_t payload_size);

/** Subscribe to a capability publish stream.
 *
 * The callback runs in the publisher's context. It must not block and must
 * not touch GSP. Subscriptions are owned by the caller and must be released
 * before the callback context goes away.
 */
esp_err_t mosaic_capability_subscribe(const char *name,
    mosaic_capability_mask_t granted, mosaic_capability_event_cb_t cb,
    void *user_ctx, mosaic_capability_subscription_handle_t *ret_sub);

esp_err_t mosaic_capability_unsubscribe(
    mosaic_capability_subscription_handle_t sub);

/* ========================================================================
 * Introspection
 *
 * Used by the generic script bridge, by manifest validation and by host
 * tests. None of these consumers needs to know a payload struct.
 * ======================================================================== */

esp_err_t mosaic_capability_permission(
    const char *permission_name, mosaic_capability_mask_t *out_permission);

/** Read contract of a capability, or NULL when it has no read verb. */
const mosaic_capability_contract_t *mosaic_capability_read_contract(
    const char *name);

size_t mosaic_capability_command_count(const char *name);
const mosaic_capability_command_t *mosaic_capability_command_at(
    const char *name, size_t index);
const mosaic_capability_command_t *mosaic_capability_command_for_name(
    const char *name, const char *command);

/** Enumerate every declared capability, bound to a provider or not. */
size_t mosaic_capability_count(void);
const char *mosaic_capability_name_at(size_t index);
bool mosaic_capability_available(const char *name);

#ifdef __cplusplus
}
#endif
