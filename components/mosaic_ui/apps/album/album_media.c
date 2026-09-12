/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "album_media.h"
#include "album_thumbnail.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_pthread.h"
#endif

#define MEDIA_PATH_MAX 512U
#define MEDIA_INPUT_MAX (8U * 1024U * 1024U)
#define MEDIA_THUMB_CACHE_BYTES (1536U * 1024U)
#define MEDIA_PREVIEW_CACHE_BYTES (512U * 1024U)
#define MEDIA_CACHE_ENTRIES 24U
#define MEDIA_THUMB_ENTRIES 22U

#ifdef ESP_PLATFORM
static const char *TAG = "album_media";
#endif

struct album_image {
    atomic_uint refs;
    uint8_t *data;
    size_t size;
    uint32_t key;
};

typedef struct {
    char path[MEDIA_PATH_MAX];
    bool thumbnail;
    bool pending;
    bool ready;
    uint32_t ticket;
    album_image_t *image;
} media_channel_t;

typedef struct {
    char path[MEDIA_PATH_MAX];
    off_t file_size;
    time_t modified;
    bool thumbnail;
    uint64_t used;
    album_image_t *image;
} media_cache_t;

typedef struct {
    char path[MEDIA_PATH_MAX];
    bool removed;
} media_delete_item_t;

typedef struct {
    size_t count;
    media_delete_item_t items[];
} media_delete_t;

struct album_media {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    atomic_bool stopping;
    size_t count;
    size_t cursor;
    uint64_t clock;
    media_delete_t *deletion;
    bool delete_pending;
    bool delete_ready;
    media_cache_t cache[MEDIA_CACHE_ENTRIES];
    media_channel_t channels[];
};

const void *album_image_data(const album_image_t *image)
{
    return image->data;
}

size_t album_image_size(const album_image_t *image)
{
    return image->size;
}

uint32_t album_image_key(const album_image_t *image)
{
    return image->key;
}

void album_image_release(void *ctx)
{
    album_image_t *image = ctx;
    if (image && atomic_fetch_sub(&image->refs, 1) == 1) {
        free(image->data);
        free(image);
    }
}

static album_image_t *image_retain(album_image_t *image)
{
    atomic_fetch_add(&image->refs, 1);
    return image;
}

static uint32_t content_key(const uint8_t *data, size_t size)
{
    uint32_t key = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        key = (key ^ data[i]) * 16777619U;
    }
    return key ? key : 1;
}

/* Cache ownership stays on the worker. GSP and completed channels hold their
 * own references, so eviction and shutdown never invalidate borrowed bytes. */
static void cache_insert(album_media_t *media, const media_channel_t *request,
                         const struct stat *info, album_image_t *image)
{
    const size_t budget = request->thumbnail ? MEDIA_THUMB_CACHE_BYTES : MEDIA_PREVIEW_CACHE_BYTES;
    if (image->size > budget) {
        return;
    }
    /* Keep preview payloads from evicting visible grid thumbnails. */
    size_t first = request->thumbnail ? 0 : MEDIA_THUMB_ENTRIES;
    size_t end = request->thumbnail ? MEDIA_THUMB_ENTRIES : MEDIA_CACHE_ENTRIES;
    for (;;) {
        size_t bytes = 0, oldest = first, empty = MEDIA_CACHE_ENTRIES;
        uint64_t age = UINT64_MAX;
        for (size_t i = first; i < end; ++i) {
            media_cache_t *entry = &media->cache[i];
            if (!entry->image) {
                empty = i;
            } else {
                bytes += entry->image->size;
                if (entry->used < age) {
                    age = entry->used;
                    oldest = i;
                }
            }
        }
        if (empty != MEDIA_CACHE_ENTRIES && bytes + image->size <= budget) {
            media_cache_t *entry = &media->cache[empty];
            memcpy(entry->path, request->path, sizeof(entry->path));
            entry->file_size = info->st_size;
            entry->modified = info->st_mtime;
            entry->thumbnail = request->thumbnail;
            entry->used = ++media->clock;
            entry->image = image_retain(image);
            return;
        }
        album_image_release(media->cache[oldest].image);
        media->cache[oldest].image = NULL;
    }
}

static album_image_t *load_image(album_media_t *media, const media_channel_t *request)
{
    struct stat info;
    if (stat(request->path, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > MEDIA_INPUT_MAX) {
        return NULL;
    }
    for (size_t i = 0; i < MEDIA_CACHE_ENTRIES; ++i) {
        media_cache_t *entry = &media->cache[i];
        if (entry->image && entry->thumbnail == request->thumbnail &&
            strcmp(entry->path, request->path) == 0) {
            if (entry->file_size == info.st_size && entry->modified == info.st_mtime) {
                entry->used = ++media->clock;
                return image_retain(entry->image);
            }
            album_image_release(entry->image);
            entry->image = NULL;
        }
    }
    FILE *file = fopen(request->path, "rb");
    if (!file) {
        return NULL;
    }
    size_t size = (size_t)info.st_size;
    uint8_t *data = malloc(size);
    size_t at = 0;
    while (data && at < size && !atomic_load(&media->stopping)) {
        size_t bytes = size - at;
        if (bytes > 16384) {
            bytes = 16384;
        }
        size_t got = fread(data + at, 1, bytes, file);
        if (!got) {
            break;
        }
        at += got;
    }
    struct stat after;
    bool stable = fstat(fileno(file), &after) == 0 &&
                  after.st_size == info.st_size && after.st_mtime == info.st_mtime;
    fclose(file);
    if (at != size || !stable || atomic_load(&media->stopping)) {
        free(data);
        return NULL;
    }
    if (request->thumbnail) {
        size_t thumb_size = 0;
        uint8_t *thumb = album_thumbnail_create(data, size, &thumb_size);
        free(data);
        data = thumb;
        size = thumb_size;
    }
    if (!data) {
        return NULL;
    }
    album_image_t *image = calloc(1, sizeof(*image));
    if (!image) {
        free(data);
        return NULL;
    }
    atomic_init(&image->refs, 1);
    image->data = data;
    image->size = size;
    image->key = content_key(data, size);
    cache_insert(media, request, &info, image);
    return image;
}

static void process_request(album_media_t *media, size_t channel, const media_channel_t *request)
{
    album_image_t *image = load_image(media, request);
#ifdef ESP_PLATFORM
    if (!image && !atomic_load(&media->stopping)) {
        ESP_LOGW(TAG, "Image load failed: %s thumbnail=%u", request->path, request->thumbnail);
    }
#endif
    pthread_mutex_lock(&media->lock);
    media_channel_t *current = &media->channels[channel];
    if (!atomic_load(&media->stopping) && current->ticket == request->ticket) {
        current->image = image;
        current->ready = true;
        image = NULL;
    }
    pthread_mutex_unlock(&media->lock);
    album_image_release(image);
}

static void process_delete(album_media_t *media, media_delete_t *deletion)
{
    for (size_t i = 0; i < deletion->count; ++i) {
        const char *path = deletion->items[i].path;
        deletion->items[i].removed = unlink(path) == 0 || errno == ENOENT;
        /* Invalidate even on failure: an external importer may have changed
         * the same path without changing its coarse timestamp or length. */
        for (size_t j = 0; j < MEDIA_CACHE_ENTRIES; ++j) {
            if (media->cache[j].image && strcmp(media->cache[j].path, path) == 0) {
                album_image_release(media->cache[j].image);
                media->cache[j].image = NULL;
            }
        }
    }
    pthread_mutex_lock(&media->lock);
    media->delete_ready = true;
    pthread_mutex_unlock(&media->lock);
}

#if !defined(__EMSCRIPTEN__)
static void *media_worker(void *ctx)
{
    album_media_t *media = ctx;
    for (;;) {
        pthread_mutex_lock(&media->lock);
        size_t channel = media->count;
        while (!atomic_load(&media->stopping) && !media->delete_pending) {
            /* Preview first; round-robin cells prevent a busy slot starving
             * other visible/overscan rows. There is only one in-flight read. */
            if (media->channels[0].pending) {
                channel = 0;
            } else {
                for (size_t i = 1; i < media->count; ++i) {
                    size_t candidate = 1 + (media->cursor + i - 1) % (media->count - 1);
                    if (media->channels[candidate].pending) {
                        channel = candidate;
                        media->cursor = candidate;
                        break;
                    }
                }
            }
            if (channel != media->count) {
                break;
            }
            pthread_cond_wait(&media->wake, &media->lock);
        }
        if (media->delete_pending) {
            media_delete_t *deletion = media->deletion;
            media->delete_pending = false;
            pthread_mutex_unlock(&media->lock);
            process_delete(media, deletion);
            continue;
        }
        if (atomic_load(&media->stopping)) {
            pthread_mutex_unlock(&media->lock);
            break;
        }
        media_channel_t request = media->channels[channel];
        media->channels[channel].pending = false;
        pthread_mutex_unlock(&media->lock);

        process_request(media, channel, &request);
    }
    return NULL;
}
#endif

album_media_t *album_media_create(size_t channels)
{
    if (channels < 2 || channels > 256) {
        return NULL;
    }
    album_media_t *media = calloc(1, sizeof(*media) + channels * sizeof(media_channel_t));
    if (!media) {
        return NULL;
    }
    media->count = channels;
    atomic_init(&media->stopping, false);
    if (pthread_mutex_init(&media->lock, NULL) != 0) {
        free(media);
        return NULL;
    }
    if (pthread_cond_init(&media->wake, NULL) != 0) {
        pthread_mutex_destroy(&media->lock);
        free(media);
        return NULL;
    }
#ifdef ESP_PLATFORM
    esp_pthread_cfg_t previous;
    bool had_config = esp_pthread_get_cfg(&previous) == ESP_OK;
    esp_pthread_cfg_t config = esp_pthread_get_default_config();
    config.stack_size = 6144;
    config.prio = 3;
    config.thread_name = "album_media";
    if (esp_pthread_set_cfg(&config) != ESP_OK) {
        pthread_cond_destroy(&media->wake);
        pthread_mutex_destroy(&media->lock);
        free(media);
        return NULL;
    }
#endif
#if defined(__EMSCRIPTEN__)
    /* The single-threaded browser simulator keeps its cooperative loading
     * path. Firmware and native hosts always use the background worker. */
    int ret = 0;
#else
    int ret = pthread_create(&media->thread, NULL, media_worker, media);
#endif
#ifdef ESP_PLATFORM
    if (!had_config) {
        previous = esp_pthread_get_default_config();
    }
    (void)esp_pthread_set_cfg(&previous);
#endif
    if (ret != 0) {
        pthread_cond_destroy(&media->wake);
        pthread_mutex_destroy(&media->lock);
        free(media);
        return NULL;
    }
    return media;
}

void album_media_destroy(album_media_t *media)
{
    if (!media) {
        return;
    }
    pthread_mutex_lock(&media->lock);
    atomic_store(&media->stopping, true);
    pthread_cond_signal(&media->wake);
    pthread_mutex_unlock(&media->lock);
#if !defined(__EMSCRIPTEN__)
    pthread_join(media->thread, NULL);
#else
    if (media->delete_pending) {
        process_delete(media, media->deletion);
    }
#endif
    free(media->deletion);
    for (size_t i = 0; i < media->count; ++i) {
        album_image_release(media->channels[i].image);
    }
    for (size_t i = 0; i < MEDIA_CACHE_ENTRIES; ++i) {
        album_image_release(media->cache[i].image);
    }
    pthread_cond_destroy(&media->wake);
    pthread_mutex_destroy(&media->lock);
    free(media);
}

static uint32_t channel_reset(media_channel_t *channel)
{
    if (++channel->ticket == 0) {
        ++channel->ticket;
    }
    album_image_release(channel->image);
    channel->image = NULL;
    channel->ready = false;
    channel->pending = false;
    return channel->ticket;
}

uint32_t album_media_request(album_media_t *media, size_t channel, const char *path, bool thumbnail)
{
    if (!media || channel >= media->count || !path || strlen(path) >= MEDIA_PATH_MAX) {
        return 0;
    }
    pthread_mutex_lock(&media->lock);
    media_channel_t *request = &media->channels[channel];
    uint32_t ticket = channel_reset(request);
    strcpy(request->path, path);
    request->thumbnail = thumbnail;
    request->pending = true;
    pthread_cond_signal(&media->wake);
    pthread_mutex_unlock(&media->lock);
    return ticket;
}

void album_media_cancel(album_media_t *media, size_t channel)
{
    if (media && channel < media->count) {
        pthread_mutex_lock(&media->lock);
        channel_reset(&media->channels[channel]);
        pthread_mutex_unlock(&media->lock);
    }
}

bool album_media_take(album_media_t *media, size_t channel, uint32_t *ticket, album_image_t **image)
{
    if (!media || channel >= media->count || pthread_mutex_trylock(&media->lock) != 0) {
        return false;
    }
    media_channel_t *result = &media->channels[channel];
#if defined(__EMSCRIPTEN__)
    if (result->pending) {
        media_channel_t request = *result;
        result->pending = false;
        pthread_mutex_unlock(&media->lock);
        process_request(media, channel, &request);
        pthread_mutex_lock(&media->lock);
    }
#endif
    bool ready = result->ready;
    if (ready) {
        *ticket = result->ticket;
        *image = result->image;
        result->ready = false;
        result->image = NULL;
    }
    pthread_mutex_unlock(&media->lock);
    return ready;
}

bool album_media_delete(album_media_t *media, const char *const *paths, size_t count)
{
    if (!media || !paths || !count || count > 128) {
        return false;
    }
    media_delete_t *deletion = calloc(1, sizeof(*deletion) + count * sizeof(media_delete_item_t));
    if (!deletion) {
        return false;
    }
    deletion->count = count;
    for (size_t i = 0; i < count; ++i) {
        if (!paths[i] || strlen(paths[i]) >= MEDIA_PATH_MAX) {
            free(deletion);
            return false;
        }
        strcpy(deletion->items[i].path, paths[i]);
    }
    pthread_mutex_lock(&media->lock);
    bool accepted = !media->deletion && !atomic_load(&media->stopping);
    if (accepted) {
        media->deletion = deletion;
        media->delete_pending = true;
        pthread_cond_signal(&media->wake);
    }
    pthread_mutex_unlock(&media->lock);
    if (!accepted) {
        free(deletion);
    }
    return accepted;
}

bool album_media_take_deleted(album_media_t *media, bool *removed, size_t capacity, size_t *count)
{
    if (!media || !removed || !count || pthread_mutex_trylock(&media->lock) != 0) {
        return false;
    }
#if defined(__EMSCRIPTEN__)
    if (media->delete_pending) {
        media_delete_t *deletion = media->deletion;
        media->delete_pending = false;
        pthread_mutex_unlock(&media->lock);
        process_delete(media, deletion);
        pthread_mutex_lock(&media->lock);
    }
#endif
    media_delete_t *deletion = media->deletion;
    bool ready = media->delete_ready && capacity >= deletion->count;
    if (ready) {
        *count = deletion->count;
        for (size_t i = 0; i < deletion->count; ++i) {
            removed[i] = deletion->items[i].removed;
        }
        media->deletion = NULL;
        media->delete_ready = false;
    }
    pthread_mutex_unlock(&media->lock);
    if (ready) {
        free(deletion);
    }
    return ready;
}
