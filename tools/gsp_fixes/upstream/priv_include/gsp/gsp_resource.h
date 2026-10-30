/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t expected_version_major;
    uint32_t target_profile_id;
    uint32_t resource_alignment;
    uint32_t stride_alignment;
    bool enforce_target_profile;
    bool skip_crc;
} gsp_resource_open_config_t;

typedef struct {
    const uint8_t *package_data;
    size_t package_size;
    const uint8_t *resource_table;
    const uint8_t *resource_data;
    size_t resource_data_size;
    uint32_t resource_count;
    const uint8_t *dither_ids;
    uint32_t dither_count;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t target_profile_id;
    uint32_t content_id;
} gsp_resource_bundle_t;

/** Runtime-only destination region for procedurally drawn image resources. */
typedef struct {
    void *pixels;
    size_t stride_bytes;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_format;
} gsp_resource_draw_target_t;

typedef gsp_err_t (*gsp_resource_draw_cb_t)(
    void *ctx, const gsp_resource_draw_target_t *target);

typedef struct {
    const uint8_t *data;
    size_t data_size;
    uint32_t resource_id;
    uint32_t raw_size;
    uint32_t content_hash;
    uint32_t stride_bytes;
    uint32_t aux;
    uint16_t width;
    uint16_t height;
    uint8_t codec;
    uint8_t pixel_format;
    uint8_t alpha_mode;
    uint8_t cache_policy;
    uint8_t kind;
    bool decode_dither;
    /*
     * Optional runtime-only source lifetime. Compiled GRB resources leave
     * these fields zero. Dynamic publishers use them so an asynchronous
     * decoder can retain encoded bytes while the owning UI slot is replaced.
     */
    void *lifetime_ctx;
    void (*lifetime_retain)(void *ctx);
    void (*lifetime_release)(void *ctx);
    /* Optional runtime-only opaque-image producer. Compiled resources and
     * ordinary dynamic images leave these fields zero. */
    void *draw_ctx;
    gsp_resource_draw_cb_t draw;
} gsp_resource_view_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} gsp_image_decode_region_t;

typedef struct {
    const uint8_t *data;
    size_t data_size;
    size_t stride_bytes;
    size_t aux_offset;
    uint16_t width;
    uint16_t height;
    uint8_t pixel_format;
    uint8_t alpha_mode;
} gsp_decoded_image_t;

typedef gsp_err_t (*gsp_image_decoder_probe_cb_t)(
    void *user_ctx,
    const gsp_resource_view_t *resource);

typedef gsp_err_t (*gsp_image_decoder_decode_cb_t)(
    void *user_ctx,
    const gsp_resource_view_t *resource,
    const gsp_image_decode_region_t *region,
    gsp_decoded_image_t *out_image);

typedef void (*gsp_image_decoder_release_cb_t)(
    void *user_ctx,
    gsp_decoded_image_t *image);

/**
 * Region decoding is required for bounded-memory stripe rendering. A decoded
 * image must cover exactly the requested region and remain valid until
 * release() returns. probe() must validate the complete encoded stream and
 * must not retain resources. decode() must leave any partially acquired
 * resources represented by out_image; release() is called after every decode
 * attempt, including failures. Implementations may use software or hardware
 * decoding.
 */
typedef struct {
    gsp_image_decoder_probe_cb_t probe;
    gsp_image_decoder_decode_cb_t decode;
    gsp_image_decoder_release_cb_t release;
    void *user_ctx;
} gsp_image_decoder_t;

gsp_err_t gsp_resource_bundle_open(const void *data, size_t data_size,
                                   const gsp_resource_open_config_t *config,
                                   gsp_resource_bundle_t *out_bundle);

gsp_err_t gsp_resource_find(const gsp_resource_bundle_t *bundle,
                            uint32_t resource_id,
                            gsp_resource_view_t *out_resource);

#ifdef __cplusplus
}
#endif
