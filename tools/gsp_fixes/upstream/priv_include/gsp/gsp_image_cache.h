/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_resource.h"
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GSP_IMAGE_CACHE_STATE_EMPTY = 0,
    GSP_IMAGE_CACHE_STATE_DECODING = 1,
    GSP_IMAGE_CACHE_STATE_READY = 2,
    GSP_IMAGE_CACHE_STATE_FAILED = 3,
    GSP_IMAGE_CACHE_STATE_RUNNING = 4,
} gsp_image_cache_state_t;

/** Cache-owned destination surface for one full-image decode job. */
typedef struct {
    uint8_t *data;
    size_t data_size;
    size_t stride_bytes;
    size_t aux_offset;
    /** Row stride of the auxiliary A8 plane; zero means tightly packed. */
    size_t aux_stride_bytes;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_format;
    uint8_t alpha_mode;
    /** Borrowed decoder workspace, retained through the detached job. */
    void *workspace;
    bool workspace_prepared;
} gsp_image_cache_surface_t;

/**
 * Decodes the complete encoded resource into the pre-allocated destination.
 * The destination layout (stride, aux plane offset for RGB565+A8) is fixed by
 * the cache and must not be changed. Must not retain any pointer after return.
 */
typedef gsp_err_t (*gsp_image_cache_decode_cb_t)(
    void *user_ctx,
    const gsp_resource_view_t *resource,
    const gsp_image_cache_surface_t *destination);

/** Optional pre-decode reservation, under cache serialization. No decoding or
 * runtime updates here. On LIMIT_EXCEEDED the cache reclaims unpinned LRU
 * entries and retries; constrained is the final attempt with no victims left.
 * On failure leave *out_workspace NULL and release partial reservations. */
typedef gsp_err_t (*gsp_image_cache_prepare_cb_t)(
    void *user_ctx, const gsp_resource_view_t *resource,
    const gsp_image_cache_surface_t *destination, bool constrained,
    void **out_workspace);
typedef void (*gsp_image_cache_release_cb_t)(void *user_ctx, void *workspace);
/**
 * Optional decoder hint for resources without a declared layout. Returning
 * true with a larger extent makes the cache allocate that padded surface: the
 * entry keeps the logical width and height and exposes the wider stride, so a
 * decoder that emits whole MCU rows can write the cache entry directly.
 * Called with the cache serialized, so it must be cheap and side-effect free.
 */
typedef bool (*gsp_image_cache_extent_cb_t)(
    void *user_ctx,
    const gsp_resource_view_t *resource,
    uint16_t *out_width,
    uint16_t *out_height);

/** Optional monotonic clock used only for cumulative telemetry. */
typedef uint64_t (*gsp_image_cache_time_cb_t)(void *user_ctx);

typedef void *(*gsp_image_cache_alloc_cb_t)(void *user_ctx, size_t size);

typedef void (*gsp_image_cache_free_cb_t)(void *user_ctx, void *ptr);

/**
 * Reports one finished decode job. status is GSP_OK for READY entries and the
 * failure reason for FAILED entries. The caller must serialize publication
 * with rendering; typical use is gsp_runtime_mark_resource_dirty(). Callbacks
 * must not block. A callback that re-enters cache helpers requires recursive
 * serialization.
 */
typedef void (*gsp_image_cache_ready_cb_t)(
    void *user_ctx,
    uint32_t resource_id,
    gsp_err_t status);

/** Queue class of a decode job; higher values are scheduled first. */
typedef enum {
    GSP_IMAGE_CACHE_PRIORITY_PREFETCH = 0,
    GSP_IMAGE_CACHE_PRIORITY_DEMAND = 1,
} gsp_image_cache_priority_t;

typedef struct {
    gsp_resource_view_t source;
    uint8_t *data;
    size_t data_size;
    size_t stride_bytes;
    size_t aux_offset;
    uint32_t last_used;
    /** FIFO position within the priority class. */
    uint32_t request_order;
    uint64_t queued_at_us;
    /** bytes_used when a shortage failed this entry; retry once it drops. */
    size_t retry_watermark;
    gsp_err_t status;
    uint16_t pin_count;
    /** Shortage retries spent since the last release of cached pixels. */
    uint8_t retry_count;
    uint8_t state;
    /** gsp_image_cache_priority_t of a queued job. */
    uint8_t priority;
    bool cancelled;
    bool retryable;
    bool source_retained;
} gsp_image_cache_entry_t;

typedef struct {
    gsp_image_cache_decode_cb_t decode;
    void *decode_ctx;
    /** Called with decode_ctx; NULL keeps the logical extent. */
    gsp_image_cache_extent_cb_t decode_extent;
    gsp_image_cache_time_cb_t time_us;
    void *time_ctx;
    gsp_image_cache_alloc_cb_t alloc;
    gsp_image_cache_free_cb_t free;
    void *memory_ctx;
    gsp_image_cache_ready_cb_t on_ready;
    void *ready_ctx;
    gsp_image_cache_entry_t *entries;
    size_t entry_capacity;
    size_t byte_budget;
    uint32_t stride_alignment;
    uint32_t placeholder_color;
    uint8_t shortage_retries;
    gsp_image_cache_prepare_cb_t prepare;
    gsp_image_cache_release_cb_t release_workspace;
} gsp_image_cache_config_t;

/* Keep the per-source variant bound and legacy directory size stable. */
#define GSP_VECTOR_CACHE_CAPACITY 4U
/* Independent resources share this directory and the existing pixel budget. */
#define GSP_VECTOR_CACHE_WORKING_CAPACITY 8U

typedef struct {
    uint8_t *data;
    size_t size;
    const uint8_t *source;
    uint32_t key[12];
    uint32_t age;
} gsp_vector_cache_entry_t;

typedef struct {
    gsp_image_cache_config_t config;
    /* Lazy metadata; pixel storage shares the decoded-image byte budget. */
    gsp_vector_cache_entry_t *vector_entries;
    uint32_t vector_hits;
    uint32_t vector_rasters;
    size_t bytes_used;
    size_t inflight_bytes;
    size_t peak_bytes;
    uint32_t clock;
    uint32_t next_request_order;
    uint32_t hit_count;
    uint32_t miss_count;
    uint32_t request_count;
    uint32_t coalesced_count;
    uint32_t eviction_count;
    uint32_t allocation_failures;
    uint32_t decode_failures;
    /** Prefetch jobs discarded because they would have displaced pixels. */
    uint32_t prefetch_dropped;
    uint32_t decoded_count;
    uint64_t decoded_bytes;
    uint64_t queue_wait_us;
    uint32_t max_queue_wait_us;
    uint64_t allocation_us;
    uint32_t max_allocation_us;
    uint64_t process_us;
    uint32_t max_process_us;
    uint64_t publish_us;
    uint32_t max_publish_us;
    /* Zero denotes a legacy provider's four-entry allocation. Only set this
     * after allocating the corresponding array; existing offsets stay fixed. */
    uint8_t vector_entry_capacity;
} gsp_image_cache_t;

static inline size_t gsp_image_cache_vector_capacity(const gsp_image_cache_t *cache)
{
    return cache != NULL && cache->vector_entry_capacity == GSP_VECTOR_CACHE_WORKING_CAPACITY ?
           GSP_VECTOR_CACHE_WORKING_CAPACITY : GSP_VECTOR_CACHE_CAPACITY;
}

/* Remove an entry, returning its buffer only when reuse_size matches. */
uint8_t *gsp_image_cache_release_vector(gsp_image_cache_t *cache,
                                        size_t index, size_t reuse_size);
void gsp_image_cache_clear_vectors(gsp_image_cache_t *cache);

/**
 * One detached decode job. begin_one() prepares it while the cache is locked,
 * execute_job() performs codec work without touching shared cache state, and
 * finish_job() publishes the result after the cache lock is reacquired.
 */
typedef struct {
    gsp_image_cache_t *cache;
    gsp_image_cache_entry_t *entry;
    gsp_resource_view_t source;
    gsp_image_cache_surface_t destination;
    uint8_t *data;
    gsp_err_t status;
    bool source_retained;
    bool ready_pending;
    uint32_t ready_resource_id;
    uint64_t process_start_us;
    bool decode_started;
} gsp_image_cache_job_t;

/**
 * Initializes a byte-budgeted decoded-image LRU cache over caller-owned entry
 * storage. decode, alloc, free, and entries are required; on_ready is
 * optional. stride_alignment must be zero (no alignment) or a power of two.
 * placeholder_color is a surface-native color used on cache miss: RGB565,
 * 0xRRGGBB for RGB888, or 0xAARRGGBB for ARGB8888.
 *
 * All cache calls must be externally serialized. Pixel storage is mutated
 * only by gsp_image_cache_process_one(), gsp_image_cache_evict(), and
 * gsp_image_cache_reset(); these must not run between preparing a frame and
 * finishing its last region render.
 */
gsp_err_t gsp_image_cache_init(
    gsp_image_cache_t *cache,
    const gsp_image_cache_config_t *config);

/**
 * Validates that the encoded resource can be decoded into this cache: the
 * codec is not STORE, the pixel format has a known native layout, and the
 * decoded size fits the byte budget.
 */
gsp_err_t gsp_image_cache_check(
    const gsp_image_cache_t *cache,
    const gsp_resource_view_t *resource);

/**
 * Returns true and fills out_image only when a READY native surface exists.
 * The surface stays valid until the entry is evicted; eviction only happens
 * inside process_one/evict/reset, never inside lookup or request.
 */
bool gsp_image_cache_lookup(
    gsp_image_cache_t *cache,
    uint32_t resource_id,
    gsp_decoded_image_t *out_image);

/**
 * Queues one full-image decode job. Returns GSP_OK when the entry is already
 * READY or DECODING, GSP_ERR_INVALID_STATE for FAILED entries (evict to
 * retry), and GSP_ERR_LIMIT_EXCEEDED when no entry slot is free or the image
 * can never fit the byte budget. Never evicts or allocates pixel storage.
 */
gsp_err_t gsp_image_cache_request(
    gsp_image_cache_t *cache,
    const gsp_resource_view_t *resource);

/**
 * Queues a speculative decode behind every demand request (renderer misses
 * and gsp_image_cache_request() callers). A later demand request promotes the
 * job to the back of the demand queue. A prefetch job only uses free budget:
 * it never evicts READY images or vector surfaces, and when it cannot fit or
 * allocate it is discarded (entry back to EMPTY, no ready callback) instead of
 * being recorded as FAILED. Return values match gsp_image_cache_request().
 */
gsp_err_t gsp_image_cache_prefetch(
    gsp_image_cache_t *cache,
    const gsp_resource_view_t *resource);

/**
 * Runs the next queued decode job (demand before prefetch, FIFO within each
 * class): evicts unpinned READY LRU entries (never
 * PRELOAD ones) until the job fits the budget, allocates, and decodes. If the
 * allocator is under physical heap pressure despite budget headroom, it also
 * reclaims ordinary LRU entries and retries until allocation succeeds or no
 * eligible victim remains.
 * Job failure is recorded in the entry (FAILED + status) and reported via
 * on_ready; the return value is GSP_OK unless the call itself is invalid.
 * *out_processed reports whether a job was consumed.
 */
gsp_err_t gsp_image_cache_process_one(
    gsp_image_cache_t *cache,
    bool *out_processed);

/**
 * Prepares the next queued job (demand before prefetch) and marks it RUNNING. The caller must
 * externally serialize cache access. Target allocation, optional decoder
 * workspace reservation and LRU reclamation happen here; decoding does not.
 * byte_budget continues to cover cached/inflight pixels only, not workspace.
 * A dynamic source lifetime is retained before the function returns.
 */
gsp_err_t gsp_image_cache_begin_one(
    gsp_image_cache_t *cache,
    gsp_image_cache_job_t *out_job,
    bool *out_started);

/** Executes the decoder and releases its reservation outside the cache lock. */
gsp_err_t gsp_image_cache_execute_job(gsp_image_cache_job_t *job);

/**
 * Commits or discards a completed RUNNING job. The caller must externally
 * serialize cache access. A cancelled job is discarded without on_ready.
 */
gsp_err_t gsp_image_cache_finish_job(gsp_image_cache_job_t *job);

/**
 * Delivers the ready callback recorded by finish_job(). Keep publication
 * serialized with rendering, including any runtime updates in the callback.
 */
void gsp_image_cache_notify_job_ready(gsp_image_cache_job_t *job);

/**
 * Releases any unexecuted job's reservation, then the source lifetime retained
 * by begin_one(). Call after dropping the cache lock because a borrowed-source
 * release callback may enter application code. Required even after cancellation.
 */
void gsp_image_cache_release_job_source(gsp_image_cache_job_t *job);

/** True while any decode job is queued (DECODING entries exist). */
bool gsp_image_cache_has_pending(const gsp_image_cache_t *cache);

gsp_err_t gsp_image_cache_get_state(
    const gsp_image_cache_t *cache,
    uint32_t resource_id,
    gsp_image_cache_state_t *out_state,
    gsp_err_t *out_status);

/** Pinned entries are never evicted by the LRU walk or by reset. */
gsp_err_t gsp_image_cache_pin(gsp_image_cache_t *cache, uint32_t resource_id);

gsp_err_t gsp_image_cache_unpin(gsp_image_cache_t *cache, uint32_t resource_id);

/**
 * Removes one entry in any unpinned state. A queued DECODING entry is removed
 * immediately; a RUNNING entry is marked cancelled and discarded safely when
 * its detached job finishes. Returns GSP_ERR_INVALID_STATE for pinned entries.
 */
gsp_err_t gsp_image_cache_evict(gsp_image_cache_t *cache, uint32_t resource_id);

/** Removes every unpinned entry. */
void gsp_image_cache_reset(gsp_image_cache_t *cache);

#ifdef __cplusplus
}
#endif
