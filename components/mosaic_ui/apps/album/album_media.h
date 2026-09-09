/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct album_media album_media_t;
typedef struct album_image album_image_t;

/* Channel zero is reserved for the foreground preview. Other channels follow
 * recycled grid slots. Requests replace older work on the same channel. */
album_media_t *album_media_create(size_t channels);
void album_media_destroy(album_media_t *media);
uint32_t album_media_request(album_media_t *media, size_t channel,
                             const char *path, bool thumbnail);
void album_media_cancel(album_media_t *media, size_t channel);
bool album_media_take(album_media_t *media, size_t channel, uint32_t *ticket,
                      album_image_t **image);

/* One confirmed delete batch at a time. Paths are copied before return;
 * results retain input order. An accepted batch is drained during shutdown. */
bool album_media_delete(album_media_t *media, const char *const *paths, size_t count);
bool album_media_take_deleted(album_media_t *media, bool *removed, size_t capacity,
                               size_t *count);

const void *album_image_data(const album_image_t *image);
size_t album_image_size(const album_image_t *image);
uint32_t album_image_key(const album_image_t *image);
/* Independent of the worker lifetime; suitable as a GSP BORROW release hook. */
void album_image_release(void *image);
