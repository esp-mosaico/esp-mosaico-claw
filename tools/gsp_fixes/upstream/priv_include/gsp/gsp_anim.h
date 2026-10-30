/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once
#include "gsp/gsp_build_caps.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_context.h"
#include "gsp/gsp_codec.h"

#define GSP_ANIM_PATCH_RECTS ESP_GSP_BUILD_CAP_ANIM_PATCH_RECTS

#define GSP_ANIM_REFERENCE_COMMANDS ESP_GSP_BUILD_CAP_ANIM_REFERENCE_COMMANDS

typedef struct {
    uint16_t x1, y1, x2, y2;
} gsp_anim_patch_rect_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * EAF/AAF animation service.
 *
 * Plays an EAF animation into a scene image slot: the scene authors a
 * placeholder image of the animation's size, the service decodes
 * frames into a double buffer and republishes the image resource on
 * every tick — the renderers just see a changing profile-native STORE
 * resource, so every existing path (dirty rects, incremental present,
 * PIE blit kernels) applies unchanged.
 *
 *   gsp_anim_t anim;
 *   gsp_anim_config_t config = {
 *       .data = eaf_start, .size = eaf_size,
 *       .frame_interval_ms = 40,
 *   };
 *   gsp_anim_open(&anim, &config);
 *   gsp_anim_attach(&anim, &ctx, IMAGE_RES_REF);
 *   for (;;) { ...; gsp_anim_tick(&anim, now_ms); render; }
 *
 * Palette animations carry per-pixel transparency and publish RGB565_A8
 * or straight ARGB8888. Opaque animations publish RGB565 or RGB888.
 * Author the placeholder with the same dimensions and alpha form; the
 * scene profile selects the published representation.
 */
typedef struct {
    const void *data;               /*!< EAF file bytes (kept borrowed) */
    size_t size;
    uint32_t frame_interval_ms;     /*!< 0 = default 40 ms (25 fps) */
    bool once;                      /*!< stop at the last frame instead
                                         of looping */
    uint8_t frame_memory;           /*!< 0 auto, 1 internal, 2 SPIRAM */
    size_t max_frame_bytes;
    size_t internal_frame_max_bytes;
} gsp_anim_config_t;

typedef struct {
    void *decoder;                  /*!< eaf parser handle */
    gsp_context_t *context;
    uint16_t res_ref;
    gsp_rect_t dirty_bbox;          /*!< union of referencing blits */
    uint16_t width;
    uint16_t height;
    uint8_t bit_depth;              /*!< EAF depth or compiled span token width */
    uint8_t pixel_format;          /*!< published profile-native format */
    uint8_t alpha_mode;
    uint8_t bytes_per_pixel;       /*!< main-plane packed bytes/pixel */
    /* EAF and compiled playback use mutually exclusive backend state. */
    union {
        uint16_t block_height;      /*!< EAF frame-0 geometry for scratch sizing */
        uint16_t validated_frame_count; /*!< compiled frames checked in order */
    };
    bool has_alpha;
    bool attached;
    bool once;
    bool finished;
    int total_frames;
    int current_frame;
    uint32_t frame_interval_ms;
    uint32_t last_tick_ms;
    bool first_tick;
    uint8_t frame_memory;
    size_t max_frame_bytes;
    size_t internal_frame_max_bytes;
    /* Double-buffered decoded frames + per-frame scratch. */
    uint8_t *frames[2];
    size_t frame_bytes;
    size_t alpha_offset;
    int front;
    uint8_t *block_scratch;
    size_t block_scratch_size;
    uint32_t *block_offsets;
    size_t block_offset_capacity;
    /* Compiled-animation (GRB anim_qoi) mode: frames[0] is the single
     * persistent buffer, each tick applies lossless patches/spans or a full JPEG frame. */
    const uint8_t *asset;           /*!< frame-table container (borrowed) */
    size_t asset_size;
    size_t max_patch_raw;           /*!< container-declared safety bound */
    uint32_t patch_count;           /*!< decoded patches or declared span damage rectangles */
    /* Decode and group-inflate work; the maximum covers a single stage. */
    uint64_t patch_work_us;
    uint32_t max_patch_work_us;
    uint64_t patch_pixels;          /*!< written pixels, including span motion copies */
    gsp_anim_patch_rect_t patch_rects[GSP_ANIM_PATCH_RECTS];
    uint8_t patch_rect_count;
    bool multi_patch;
    bool jpeg_frames;
    bool native_patches;
    bool span_frames;
    gsp_codec_stats_t codec_stats;
    uint16_t loop_count;            /*!< 0 = loop forever */
    uint16_t loops_done;
    uint32_t next_due_ms;
    bool single_blit;               /*!< patch-precise dirty is valid */
    /* Referencing blit commands (when they all fit): lets the tick
     * pause playback while every reference sits in a hidden vis
     * group. ref_count 0 = unknown, never pause. */
    uint32_t ref_commands[GSP_ANIM_REFERENCE_COMMANDS];
    uint8_t ref_count;
    /* Original container view, restored on close so the resource can
     * be reopened (scene switches) and never dangles into the freed
     * frame buffer. */
    gsp_resource_view_t saved_view;
} gsp_anim_t;

/* Shared allocation policy for native and filesystem animation surfaces. */
void *gsp_anim_surface_alloc(size_t size, uint8_t frame_memory, size_t internal_frame_max_bytes);
bool gsp_anim_blit_is_direct(const gsp_anim_t *anim, const gsp_scene_view_t *scene, uint32_t index);

void gsp_anim_set_memory_policy(gsp_anim_t *anim, uint8_t frame_memory,
                                size_t max_frame_bytes,
                                size_t internal_frame_max_bytes);

gsp_err_t gsp_anim_open(gsp_anim_t *anim, const gsp_anim_config_t *config);

void gsp_anim_close(gsp_anim_t *anim);

/** Attach to the scene's first IMAGE resource (single-image scenes). */
#define GSP_ANIM_RES_REF_AUTO UINT16_MAX

/**
 * Binds the animation to the scene image resource at @p res_ref (the
 * RES_REFS index the placeholder image lowered to; see the manifest),
 * or to the first IMAGE entry with GSP_ANIM_RES_REF_AUTO. The
 * placeholder must match the animation's width/height and profile-native
 * pixel/alpha format. Decodes and publishes the first frame immediately.
 */
gsp_err_t gsp_anim_attach(gsp_anim_t *anim, gsp_context_t *context,
                          uint16_t res_ref);

/**
 * Opens a compiled animation (GRB anim_qoi resource) directly from the
 * scene resource at @p res_ref and publishes its first frame. Each
 * tick then decodes only the changed patch into a persistent frame
 * buffer and dirties just that area. No placeholder is needed — the
 * animation resource itself backs the blit.
 * Initialize @p anim to zero before its first use. Optionally set a custom
 * policy with gsp_anim_set_memory_policy(); this open operation preserves it.
 * Close an open player before reusing it.
 * The borrowed container must remain unchanged until close. Lossless frames
 * are checked before their first update; a failed frame freezes playback at
 * the last valid frame without publishing partial pixels. A validated
 * playback-order prefix avoids repeating this check on later loops.
 */
gsp_err_t gsp_anim_open_asset(gsp_anim_t *anim, gsp_context_t *context,
                              uint16_t res_ref);

/**
 * Advances playback when a frame is due, updates the published resource,
 * and marks the referencing screen area dirty. Call once per frame loop.
 */
gsp_err_t gsp_anim_tick(gsp_anim_t *anim, uint32_t now_ms);

static inline int gsp_anim_frame_count(const gsp_anim_t *anim)
{
    return anim->total_frames;
}

/** Republishes the front frame into the attached resource and dirties it. */
void gsp_anim_publish_frame(gsp_anim_t *anim);

/** Entry points of the optional EAF player member (gsp_anim_eaf.c). */
typedef struct {
    gsp_err_t (*open)(gsp_anim_t *anim, const gsp_anim_config_t *config);
    gsp_err_t (*attach)(gsp_anim_t *anim, gsp_context_t *context,
                        uint16_t res_ref);
    gsp_err_t (*tick)(gsp_anim_t *anim, uint32_t now_ms);
    void (*release_decoder)(void *decoder);
} gsp_anim_eaf_provider_t;

/* The EAF player and its decoder follow the JPEG member's link contract (see
 * gsp_codec.h): only the weak provider reaches them, the component build
 * roots it while EAF playback stays enabled, and GSP_ANIM_EAF_OPTIONAL lets
 * host gates rebuild every gsp_anim_eaf() user against the weak form.
 * Without it gsp_anim_open() and gsp_anim_attach()
 * return GSP_ERR_UNSUPPORTED; compiled GRB animations are unaffected. */
#if (defined(ESP_PLATFORM) || defined(GSP_ANIM_EAF_OPTIONAL)) && \
    !defined(GSP_ANIM_EAF_PROVIDER_IMPL)
extern const gsp_anim_eaf_provider_t gsp_anim_eaf_provider
__attribute__((weak));
#else
extern const gsp_anim_eaf_provider_t gsp_anim_eaf_provider;
#endif

/** Returns the linked EAF player, or NULL when the build excluded it. */
static inline const gsp_anim_eaf_provider_t *gsp_anim_eaf(void)
{
    const gsp_anim_eaf_provider_t *provider = &gsp_anim_eaf_provider;
    return provider;
}

#ifdef __cplusplus
}
#endif
