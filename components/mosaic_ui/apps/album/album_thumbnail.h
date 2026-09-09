/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decode away from the render task, center-crop and encode an opaque QOI.
 * The returned allocation belongs to the caller. Originals are never written. */
uint8_t *album_thumbnail_create(const uint8_t *jpeg, size_t size, size_t *out_size);
