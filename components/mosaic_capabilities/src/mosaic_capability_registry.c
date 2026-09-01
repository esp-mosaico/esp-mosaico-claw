/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_capability.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "mosaic_capability_decls.h"

#define MOSAIC_CAPABILITY_PROVIDER_MAX 24U
#define MOSAIC_CAPABILITY_SUBSCRIBER_MAX 24U
#define MOSAIC_CAPABILITY_NAME_MAX 47U

typedef struct {
    const mosaic_capability_declaration_t *declaration;
    const mosaic_capability_ops_t *ops;
    void *user_ctx;
    char name[MOSAIC_CAPABILITY_NAME_MAX + 1U];
    uint16_t active_calls;
    bool in_use;
} mosaic_capability_slot_t;

struct mosaic_capability_subscription_t {
    const mosaic_capability_declaration_t *declaration;
    mosaic_capability_event_cb_t cb;
    void *user_ctx;
    uint16_t active_calls;
    bool in_use;
};

static mosaic_capability_slot_t s_providers[MOSAIC_CAPABILITY_PROVIDER_MAX];
static struct mosaic_capability_subscription_t
    s_subscriptions[MOSAIC_CAPABILITY_SUBSCRIBER_MAX];
static atomic_flag s_registry_lock = ATOMIC_FLAG_INIT;

static void registry_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
            &s_registry_lock, memory_order_acquire)) {
    }
}

static void registry_unlock(void)
{
    atomic_flag_clear_explicit(&s_registry_lock, memory_order_release);
}

/* ---------------- slot helpers (call with the lock held) ---------------- */

static mosaic_capability_slot_t *find_slot(const char *name)
{
    for (size_t index = 0; index < MOSAIC_CAPABILITY_PROVIDER_MAX; ++index) {
        mosaic_capability_slot_t *slot = &s_providers[index];
        if (slot->in_use && strcmp(slot->name, name) == 0) {
            return slot;
        }
    }
    return NULL;
}

/** Take a reference to a bound provider so it cannot be unregistered. */
static esp_err_t acquire_provider(const char *name,
    mosaic_capability_slot_t **ret_slot, const mosaic_capability_ops_t **ret_ops,
    void **ret_ctx)
{
    registry_lock();
    mosaic_capability_slot_t *slot = find_slot(name);
    if (slot == NULL) {
        registry_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    ++slot->active_calls;
    *ret_slot = slot;
    *ret_ops = slot->ops;
    *ret_ctx = slot->user_ctx;
    registry_unlock();
    return ESP_OK;
}

static void release_provider(mosaic_capability_slot_t *slot)
{
    registry_lock();
    if (slot->active_calls != 0) {
        --slot->active_calls;
    }
    registry_unlock();
}

static size_t subscriber_count(const mosaic_capability_declaration_t *decl)
{
    size_t count = 0;
    for (size_t index = 0; index < MOSAIC_CAPABILITY_SUBSCRIBER_MAX; ++index) {
        if (s_subscriptions[index].in_use &&
                s_subscriptions[index].declaration == decl) {
            ++count;
        }
    }
    return count;
}

/** Tell a bound provider that its subscriber set became empty or non-empty. */
static void notify_active(
    const mosaic_capability_declaration_t *decl, bool active)
{
    mosaic_capability_slot_t *slot = NULL;
    const mosaic_capability_ops_t *ops = NULL;
    void *ctx = NULL;
    if (acquire_provider(decl->name, &slot, &ops, &ctx) != ESP_OK) {
        return;
    }
    if (ops->set_active != NULL) {
        (void)ops->set_active(ctx, active);
    }
    release_provider(slot);
}

/* ---------------- provider side ---------------- */

esp_err_t mosaic_capability_register(
    const mosaic_capability_provider_t *provider)
{
    if (provider == NULL || provider->name == NULL || provider->ops == NULL ||
            provider->ops->read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t name_length = strlen(provider->name);
    if (name_length == 0 || name_length > MOSAIC_CAPABILITY_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(provider->name);
    if (declaration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    registry_lock();
    mosaic_capability_slot_t *existing = find_slot(provider->name);
    if (existing != NULL) {
        if (existing->active_calls != 0 || existing->ops != provider->ops ||
                existing->user_ctx != provider->user_ctx) {
            registry_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        registry_unlock();
        return ESP_OK;
    }
    mosaic_capability_slot_t *free_slot = NULL;
    for (size_t index = 0; index < MOSAIC_CAPABILITY_PROVIDER_MAX; ++index) {
        if (!s_providers[index].in_use) {
            free_slot = &s_providers[index];
            break;
        }
    }
    if (free_slot == NULL) {
        registry_unlock();
        return ESP_ERR_NO_MEM;
    }
    memcpy(free_slot->name, provider->name, name_length + 1U);
    free_slot->declaration = declaration;
    free_slot->ops = provider->ops;
    free_slot->user_ctx = provider->user_ctx;
    free_slot->active_calls = 0;
    free_slot->in_use = true;
    const bool has_subscribers = subscriber_count(declaration) != 0;
    registry_unlock();

    if (has_subscribers) {
        notify_active(declaration, true);
    }
    return ESP_OK;
}

esp_err_t mosaic_capability_unregister(const char *name, void *user_ctx)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    registry_lock();
    mosaic_capability_slot_t *slot = find_slot(name);
    if (slot == NULL) {
        registry_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->user_ctx != user_ctx || slot->active_calls != 0) {
        registry_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(slot, 0, sizeof(*slot));
    registry_unlock();
    return ESP_OK;
}

void mosaic_capability_publish(
    const char *name, const void *payload, size_t payload_size)
{
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    if (declaration == NULL || payload == NULL) {
        return;
    }
    if (declaration->read_contract != NULL &&
            payload_size != declaration->read_contract->size) {
        return;
    }

    /* One slot is taken per iteration so a subscriber callback may call back
     * into the registry, and so no bounded copy of the subscriber list has
     * to live on the publisher stack. */
    for (size_t index = 0; index < MOSAIC_CAPABILITY_SUBSCRIBER_MAX; ++index) {
        struct mosaic_capability_subscription_t *sub = &s_subscriptions[index];
        mosaic_capability_event_cb_t cb = NULL;
        void *ctx = NULL;
        registry_lock();
        if (sub->in_use && sub->declaration == declaration) {
            cb = sub->cb;
            ctx = sub->user_ctx;
            ++sub->active_calls;
        }
        registry_unlock();
        if (cb == NULL) {
            continue;
        }
        cb(ctx, declaration->name, payload, payload_size);
        registry_lock();
        if (sub->active_calls != 0) {
            --sub->active_calls;
        }
        registry_unlock();
    }
}

/* ---------------- consumer side ---------------- */

esp_err_t mosaic_capability_read(const char *name,
    mosaic_capability_mask_t granted, void *out_payload, size_t payload_size)
{
    if (name == NULL || out_payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    if (declaration == NULL || declaration->read_contract == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((granted & declaration->read_permission) == 0) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (payload_size != declaration->read_contract->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    mosaic_capability_slot_t *slot = NULL;
    const mosaic_capability_ops_t *ops = NULL;
    void *ctx = NULL;
    esp_err_t err = acquire_provider(name, &slot, &ops, &ctx);
    if (err != ESP_OK) {
        return err;
    }
    memset(out_payload, 0, payload_size);
    err = ops->read(ctx, out_payload, payload_size);
    release_provider(slot);
    return err;
}

esp_err_t mosaic_capability_invoke(const char *name,
    mosaic_capability_mask_t granted, const char *command,
    const void *args, size_t args_size, void *out_result, size_t result_size)
{
    if (name == NULL || command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_capability_command_t *declared =
        mosaic_capability_command_for_name(name, command);
    if (declared == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((granted & declared->permission) == 0) {
        return ESP_ERR_NOT_ALLOWED;
    }
    const size_t expected_args =
        declared->args != NULL ? declared->args->size : 0;
    const size_t expected_result =
        declared->result != NULL ? declared->result->size : 0;
    if (args_size != expected_args || result_size != expected_result) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((expected_args != 0 && args == NULL) ||
            (expected_result != 0 && out_result == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    mosaic_capability_slot_t *slot = NULL;
    const mosaic_capability_ops_t *ops = NULL;
    void *ctx = NULL;
    esp_err_t err = acquire_provider(name, &slot, &ops, &ctx);
    if (err != ESP_OK) {
        return err;
    }
    if (ops->invoke == NULL) {
        release_provider(slot);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (expected_result != 0) {
        memset(out_result, 0, expected_result);
    }
    err = ops->invoke(
        ctx, declared->command, args, args_size, out_result, result_size);
    release_provider(slot);
    return err;
}

esp_err_t mosaic_capability_borrow(const char *name,
    mosaic_capability_mask_t granted, uint16_t blob_id, uint32_t index,
    mosaic_capability_blob_t *out_blob)
{
    if (name == NULL || out_blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    if (declaration == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((granted & declaration->read_permission) == 0) {
        return ESP_ERR_NOT_ALLOWED;
    }

    mosaic_capability_slot_t *slot = NULL;
    const mosaic_capability_ops_t *ops = NULL;
    void *ctx = NULL;
    esp_err_t err = acquire_provider(name, &slot, &ops, &ctx);
    if (err != ESP_OK) {
        return err;
    }
    if (ops->borrow == NULL) {
        release_provider(slot);
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(out_blob, 0, sizeof(*out_blob));
    err = ops->borrow(ctx, blob_id, index, out_blob);
    if (err != ESP_OK) {
        release_provider(slot);
    }
    /* A successful borrow keeps the provider reference until release. */
    return err;
}

void mosaic_capability_release(
    const char *name, mosaic_capability_blob_t *blob)
{
    if (name == NULL || blob == NULL || blob->data == NULL) {
        return;
    }
    registry_lock();
    mosaic_capability_slot_t *slot = find_slot(name);
    const mosaic_capability_ops_t *ops = slot != NULL ? slot->ops : NULL;
    void *ctx = slot != NULL ? slot->user_ctx : NULL;
    registry_unlock();
    if (slot == NULL) {
        return;
    }
    if (ops->release != NULL) {
        ops->release(ctx, blob);
    }
    memset(blob, 0, sizeof(*blob));
    release_provider(slot);
}

esp_err_t mosaic_capability_subscribe(const char *name,
    mosaic_capability_mask_t granted, mosaic_capability_event_cb_t cb,
    void *user_ctx, mosaic_capability_subscription_handle_t *ret_sub)
{
    if (name == NULL || cb == NULL || ret_sub == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    if (declaration == NULL || !declaration->publishes) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((granted & declaration->read_permission) == 0) {
        return ESP_ERR_NOT_ALLOWED;
    }

    registry_lock();
    struct mosaic_capability_subscription_t *free_slot = NULL;
    for (size_t index = 0; index < MOSAIC_CAPABILITY_SUBSCRIBER_MAX; ++index) {
        if (!s_subscriptions[index].in_use) {
            free_slot = &s_subscriptions[index];
            break;
        }
    }
    if (free_slot == NULL) {
        registry_unlock();
        return ESP_ERR_NO_MEM;
    }
    const bool first = subscriber_count(declaration) == 0;
    free_slot->declaration = declaration;
    free_slot->cb = cb;
    free_slot->user_ctx = user_ctx;
    free_slot->active_calls = 0;
    free_slot->in_use = true;
    registry_unlock();

    if (first) {
        notify_active(declaration, true);
    }
    *ret_sub = free_slot;
    return ESP_OK;
}

esp_err_t mosaic_capability_unsubscribe(
    mosaic_capability_subscription_handle_t sub)
{
    if (sub == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    registry_lock();
    if (!sub->in_use) {
        registry_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (sub->active_calls != 0) {
        registry_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const mosaic_capability_declaration_t *declaration = sub->declaration;
    memset(sub, 0, sizeof(*sub));
    const bool last = subscriber_count(declaration) == 0;
    registry_unlock();

    if (last) {
        notify_active(declaration, false);
    }
    return ESP_OK;
}

/* ---------------- introspection ---------------- */

esp_err_t mosaic_capability_permission(
    const char *permission_name, mosaic_capability_mask_t *out_permission)
{
    if (permission_name == NULL || out_permission == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t count = 0;
    const mosaic_capability_permission_def_t *definitions =
        mosaic_capability_permissions(&count);
    for (size_t index = 0; index < count; ++index) {
        if (strcmp(definitions[index].name, permission_name) == 0) {
            *out_permission = definitions[index].bit;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}

const mosaic_capability_contract_t *mosaic_capability_read_contract(
    const char *name)
{
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    return declaration != NULL ? declaration->read_contract : NULL;
}

size_t mosaic_capability_command_count(const char *name)
{
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    return declaration != NULL ? declaration->command_count : 0;
}

const mosaic_capability_command_t *mosaic_capability_command_at(
    const char *name, size_t index)
{
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    if (declaration == NULL || index >= declaration->command_count) {
        return NULL;
    }
    return &declaration->commands[index];
}

const mosaic_capability_command_t *mosaic_capability_command_for_name(
    const char *name, const char *command)
{
    const mosaic_capability_declaration_t *declaration =
        mosaic_capability_declaration_for(name);
    if (declaration == NULL || command == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < declaration->command_count; ++index) {
        if (strcmp(declaration->commands[index].name, command) == 0) {
            return &declaration->commands[index];
        }
    }
    return NULL;
}

size_t mosaic_capability_count(void)
{
    size_t count = 0;
    (void)mosaic_capability_declarations(&count);
    return count;
}

const char *mosaic_capability_name_at(size_t index)
{
    size_t count = 0;
    const mosaic_capability_declaration_t *declarations =
        mosaic_capability_declarations(&count);
    return index < count ? declarations[index].name : NULL;
}

bool mosaic_capability_available(const char *name)
{
    if (name == NULL) {
        return false;
    }
    registry_lock();
    const bool bound = find_slot(name) != NULL;
    registry_unlock();
    return bound;
}
