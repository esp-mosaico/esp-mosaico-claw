/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gsp/gsp_types.h"
#include "gsp/gsp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * GSPB multi-file bundle: an indexed, aligned, CRC-protected pack of
 * GSB/GRB/GFB files and optional GMD deployable metadata. Members keep their own
 * container headers; open them with gsp_scene_open /
 * gsp_resource_bundle_open / gsp_gfb_open on the pointers returned
 * here. The view borrows the caller's buffer.
 */
typedef struct {
    const uint8_t *data;
    size_t size;
    const uint8_t *entries;     /*!< bundle entry table */
    uint16_t entry_count;
    uint16_t version_minor;
    uint32_t target_profile_id;
    uint32_t content_id;
} gsp_bundle_view_t;

typedef struct {
    uint16_t kind;              /*!< GSP_BUNDLE_KIND_* */
    uint16_t id;                /*!< per-kind index assigned at pack time */
    uint32_t content_id;        /*!< member container content hash */
    uint32_t name_hash;         /*!< CRC32 of the packed file base name */
    const uint8_t *data;        /*!< member file (member_alignment aligned) */
    size_t size;
} gsp_bundle_entry_t;

#define GSP_BUNDLE_KIND_GSB 1U
#define GSP_BUNDLE_KIND_GRB 2U
#define GSP_BUNDLE_KIND_GFB 3U
#define GSP_BUNDLE_KIND_GMD 4U
#define GSP_BUNDLE_KIND_REQUIREMENTS 5U

/**
 * Validates the header, CRC, entry table, and member bounds/alignment.
 * verify_crc covers the whole bundle and costs one pass over the data;
 * skip it for memory-mapped flash that is already integrity-protected.
 */
gsp_err_t gsp_bundle_open(const void *data, size_t size,
                          uint16_t expected_version_major,
                          bool verify_crc,
                          gsp_bundle_view_t *out_view);

gsp_err_t gsp_bundle_get_entry(const gsp_bundle_view_t *view,
                               uint16_t index,
                               gsp_bundle_entry_t *out_entry);

/** Finds a member by kind and pack-time id (GSP_ERR_NOT_FOUND if absent). */
gsp_err_t gsp_bundle_find(const gsp_bundle_view_t *view, uint16_t kind,
                          uint16_t id, gsp_bundle_entry_t *out_entry);

/** Finds a member by its packed file base name (e.g. "home.gsb"). */
gsp_err_t gsp_bundle_find_by_name(const gsp_bundle_view_t *view,
                                  const char *name,
                                  gsp_bundle_entry_t *out_entry);

/** Parse a supported versioned configuration contract from a GSPB. */
#define GSP_BUNDLE_CONFIG_REQUIREMENT_CAPACITY 11U
gsp_err_t gsp_bundle_config_requirements(
    const void *data, size_t size, bool verify_crc,
    esp_gsp_config_overrides_t *out_requirements,
    esp_gsp_config_override_entry_t *storage, uint16_t storage_capacity);

/* Internal startup path: the view must already be validated and immutable. */
gsp_err_t gsp_bundle_config_requirements_from_view(
    const gsp_bundle_view_t *view, esp_gsp_config_overrides_t *out_requirements,
    esp_gsp_config_override_entry_t *storage, uint16_t storage_capacity);

#ifdef __cplusplus
}
#endif
