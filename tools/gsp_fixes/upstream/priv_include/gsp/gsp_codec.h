/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "gsp/gsp_image_cache.h"
#include "gsp/gsp_resource.h"
#include "gsp/gsp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One codec-statistics value set. */
typedef struct {
    uint32_t decode_calls;
    uint64_t decode_us;
    uint64_t probe_us;
    uint64_t scale_us;
    uint64_t decoded_bytes;
    uint32_t qoi_decodes;
    uint32_t qoi_failures;
    uint32_t rle16_decodes;
    uint32_t rle16_failures;
    uint32_t png_decodes;
    uint32_t png_failures;
    uint32_t codec_failures;
    uint32_t scale_requests;
    uint32_t scale_hw;
    uint32_t scale_sw;
    uint32_t jpeg_hw_attempts;
    uint32_t jpeg_hw_decodes;
    uint64_t jpeg_hw_us;
    uint32_t jpeg_sw_attempts;
    uint32_t jpeg_sw_decodes;
    uint32_t jpeg_sw_failures;
    uint64_t jpeg_sw_us;
    uint32_t jpeg_hw_ineligible;
    uint32_t jpeg_hw_fallbacks;
} gsp_codec_stats_t;

/**
 * Per-context codec evidence. Detached decode jobs build a private delta and
 * merge it under a short-held lock, so readers never race the codec while
 * the expensive decode itself remains outside the render/cache mutex. ESP-IDF
 * uses a static critical-section mux; locked is the portable fallback state.
 */
typedef struct {
    volatile bool locked;
    gsp_codec_stats_t totals;
} gsp_codec_stats_accumulator_t;

void gsp_codec_stats_accumulator_init(
    gsp_codec_stats_accumulator_t *accumulator);

void gsp_codec_stats_merge(gsp_codec_stats_accumulator_t *accumulator,
                           const gsp_codec_stats_t *delta);

void gsp_codec_stats_snapshot(
    gsp_codec_stats_accumulator_t *accumulator,
    gsp_codec_stats_t *out_stats);

/** Optional row storage borrowed by a software JPEG resize job. */
typedef struct {
    uint8_t *data;
    size_t size;
} gsp_codec_jpeg_workspace_t;

/** Entry points of the optional JPEG member, resolved once per decode. */
typedef struct {
    /** Preserve hardware-first full decoding unless memory is constrained. */
    bool prefer_full_decode;
    gsp_err_t (*decode)(const gsp_resource_view_t *resource,
                        const gsp_image_cache_surface_t *destination,
                        gsp_codec_stats_t *stats);
    gsp_err_t (*a8_decode)(const gsp_resource_view_t *resource,
                           const gsp_image_cache_surface_t *destination,
                           gsp_codec_stats_t *stats);
    /** Returns a smaller supported decode size, never below the requested size.
     * constrained selects a bounded-row fallback where available. */
    bool (*scaled_size)(uint16_t source_w, uint16_t source_h,
                        uint16_t target_w, uint16_t target_h, bool constrained,
                        uint16_t *out_w, uint16_t *out_h);
    /** Framework-owned rows, reserved before decode; zero for native scaling.
     * Must use the same constrained value as scaled_size(). */
    size_t (*scaled_workspace_bytes)(uint16_t source_w, uint8_t pixel_format, bool constrained);
    gsp_err_t (*decode_scaled)(const gsp_resource_view_t *resource,
                               const gsp_image_cache_surface_t *destination,
                               gsp_codec_stats_t *stats);
    /** Serialized with active JPEG jobs. Shutdown also releases the engine. */
    void (*release_scratch)(bool shutdown);
    /** Optional full-decode staging owned by this provider. UNSUPPORTED keeps
     * the generic allocation path; failure leaves no allocation. Paired with
     * release_staging, including cancellation before decode. Logical extents
     * remain unchanged even when stride/capacity include MCU/cache padding. */
    gsp_err_t (*prepare_staging)(const gsp_resource_view_t *resource,
                                 gsp_image_cache_surface_t *out_surface);
    void (*release_staging)(gsp_image_cache_surface_t *surface);
    /** Optional MCU-padded surface the hardware decoder writes directly. */
    bool (*direct_extent)(const gsp_resource_view_t *resource,
                          uint16_t *out_width, uint16_t *out_height);
} gsp_codec_jpeg_provider_t;

/** Drop idle provider scratch, optionally closing the lazily recreated engine.
 * Must not be called from inside a JPEG decode. Optional-provider safe. */
void gsp_codec_release_scratch(bool shutdown);

/* gsp_codec_jpeg.c is its own archive member and holds the only references to
 * the hardware and software JPEG decoders. An undefined weak reference does
 * not extract that member, so an ESP-IDF application that opts out of JPEG
 * links neither the decoder nor its driver chain; the component build roots
 * the member explicitly while JPEG stays enabled. Portable builds compile the
 * decoder into the same target and keep the reference strong, so a published
 * simulator never loses a codec. GSP_CODEC_JPEG_OPTIONAL lets host gates
 * reproduce the ESP-IDF link contract; every translation unit that reaches
 * gsp_codec_jpeg() must then be rebuilt with it, or a strong reference left
 * in an archive member would root the decoder again. */
#if (defined(ESP_PLATFORM) || defined(GSP_CODEC_JPEG_OPTIONAL)) && \
    !defined(GSP_CODEC_JPEG_PROVIDER_IMPL)
extern const gsp_codec_jpeg_provider_t gsp_codec_jpeg_provider
__attribute__((weak));
#else
extern const gsp_codec_jpeg_provider_t gsp_codec_jpeg_provider;
#endif

/**
 * Returns the linked JPEG decoders, or NULL when the build excluded them.
 * Callers that can refuse a request must do so before reserving cache
 * surfaces, slots or frame canvases.
 */
static inline const gsp_codec_jpeg_provider_t *gsp_codec_jpeg(void)
{
    const gsp_codec_jpeg_provider_t *provider = &gsp_codec_jpeg_provider;
    return provider;
}

/** Entry point of the optional PNG member. */
typedef struct {
    gsp_err_t (*decode)(const gsp_resource_view_t *resource,
                        const gsp_image_cache_surface_t *destination);
    gsp_err_t (*prepare)(const gsp_resource_view_t *resource, void **workspace);
    void (*release)(void *workspace);
} gsp_codec_png_provider_t;

/* gsp_codec_png.c follows the JPEG member's link contract: an undefined weak
 * reference leaves the decoder out, the component build roots it while PNG
 * stays enabled, and GSP_CODEC_PNG_OPTIONAL lets host gates rebuild every
 * gsp_codec_png() user against the weak form. */
#if (defined(ESP_PLATFORM) || defined(GSP_CODEC_PNG_OPTIONAL)) && \
    !defined(GSP_CODEC_PNG_PROVIDER_IMPL)
extern const gsp_codec_png_provider_t gsp_codec_png_provider
__attribute__((weak));
#else
extern const gsp_codec_png_provider_t gsp_codec_png_provider;
#endif

/** Returns the linked PNG decoder, or NULL when the build excluded it. */
static inline const gsp_codec_png_provider_t *gsp_codec_png(void)
{
    const gsp_codec_png_provider_t *provider = &gsp_codec_png_provider;
    return provider;
}

/**
 * Decodes one complete encoded resource into a pre-allocated native surface,
 * dispatching by resource codec to the compiled-in decoders:
 *  - STORE: validated native-plane copy into the provided surface.
 *  - QOI: always available (pure C software decoder).
 *  - JPEG: available on ESP-IDF unless the application excluded the decoder
 *    at link time; uses the SoC JPEG decoder when available and falls back
 *    to software. Portable builds enable software JPEG when libjpeg is
 *    present. The software path requires aligned destinations; full decode
 *    also supports wider destination strides used for MCU-padded cache entries.
 *  - PNG: available unless the application excluded the decoder at link
 *    time.
 * Unsupported codecs and destination formats return GSP_ERR_UNSUPPORTED,
 * including JPEG, JPEG_A8 and PNG in a build that excluded the decoder.
 */
gsp_err_t gsp_codec_decode_to_surface(
    const gsp_resource_view_t *resource,
    const gsp_image_cache_surface_t *destination);

/**
 * gsp_image_cache_decode_cb_t adapter around gsp_codec_decode_to_surface.
 * user_ctx is a gsp_codec_stats_accumulator_t or NULL.
 */
gsp_err_t gsp_codec_image_cache_decode(
    void *user_ctx,
    const gsp_resource_view_t *resource,
    const gsp_image_cache_surface_t *destination);

/** Reserve framework-owned staging/PNG buffers before executing a cache job.
 * constrained permits a lower-memory JPEG path after reclaimable entries are
 * exhausted. On failure no workspace escapes; codec-library internals and the
 * shared hardware JPEG engine/scratch remain provider-managed. */
gsp_err_t gsp_codec_image_cache_prepare(
    void *user_ctx, const gsp_resource_view_t *resource,
    const gsp_image_cache_surface_t *destination, bool constrained,
    void **out_workspace);
void gsp_codec_image_cache_release(void *user_ctx, void *workspace);

/**
 * gsp_image_cache_extent_cb_t companion of gsp_codec_image_cache_decode: pads
 * hardware-decodable JPEG surfaces to whole MCUs so the engine can write the
 * cache entry without a staging copy. False everywhere else.
 */
bool gsp_codec_image_cache_extent(void *user_ctx,
                                  const gsp_resource_view_t *resource,
                                  uint16_t *out_width, uint16_t *out_height);

/**
 * Decodes only @p region of the encoded resource into a region-sized
 * destination (bounded memory; the stream prefix is walked without
 * being stored). Supports the QOI and RLE families emitted by gspc.
 */
gsp_err_t gsp_codec_decode_region(
    const gsp_resource_view_t *resource,
    const gsp_image_decode_region_t *region,
    const gsp_image_cache_surface_t *destination);

/**
 * Built-in gsp_image_decoder_t over gsp_codec_decode_region: memory
 * per decode stays bounded by the clipped draw rectangle, the CPU
 * pays a stream-prefix walk per band. For stripe rendering / tight
 * memory where whole decoded images cannot be cached — installed
 * automatically when a context disables its image cache.
 */
void gsp_codec_region_decoder(gsp_image_decoder_t *out_decoder);

typedef struct gsp_codec_region_checkpoint gsp_codec_region_checkpoint_t;

/** Reusable storage for the built-in region decoder. */
typedef struct {
    uint8_t *data;
    size_t capacity;
    bool acquired;
    gsp_codec_region_checkpoint_t *checkpoints;
    uint32_t checkpoint_clock;
    /** Regions that continued from an earlier region of the same image. */
    uint32_t resume_hits;
    /** Regions below the first row that had to walk the stream from row 0. */
    uint32_t resume_misses;
} gsp_codec_region_decoder_context_t;

/**
 * Initializes a region decoder that retains its largest scratch allocation.
 * This removes per-band malloc/free traffic while keeping memory bounded by
 * the largest region decoded. QOI and RLE regions also continue from where an
 * earlier region of the same image stopped, through a small fixed set of
 * checkpoints allocated on first use (about 2.4 KiB). Like the scratch buffer,
 * the checkpoints are unsynchronized: one context serves one render task.
 * The context must remain alive until the decoder is no longer used and must
 * be released with gsp_codec_region_decoder_context_deinit().
 */
void gsp_codec_region_decoder_reuse(
    gsp_codec_region_decoder_context_t *context,
    gsp_image_decoder_t *out_decoder);

void gsp_codec_region_decoder_context_deinit(
    gsp_codec_region_decoder_context_t *context);

/**
 * Parses a PNG header for runtime-supplied images: fills the geometry
 * an application needs to build the gsp_resource_view_t (codec = PNG)
 * it feeds to the image cache. Supported: 8-bit truecolor with or
 * without alpha, non-interlaced.
 */
gsp_err_t gsp_codec_png_probe(const void *data, size_t size,
                              uint16_t *out_width, uint16_t *out_height,
                              bool *out_has_alpha);

/**
 * Sniffs an encoded image header: codec (JPEG/PNG/QOI) and pixel
 * dimensions, so runtime-published images need no side-channel
 * metadata. GSP_ERR_UNSUPPORTED for unknown containers.
 */
gsp_err_t gsp_image_probe(const void *data, size_t size,
                          uint8_t *out_codec, uint16_t *out_width,
                          uint16_t *out_height);

#ifdef __cplusplus
}
#endif
