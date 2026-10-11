/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "air_voice_store.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "air_voice_log.h"
#include "claw_paths.h"
#include "esp_random.h"
#include "esp_timer.h"

#define DIR_NAME          "air_voice"
#define CONFIG_NAME       "config.json"
#define RAW_SUFFIX        "_raw"
#define BYTES_PER_SECOND  (AIR_VOICE_SAMPLE_RATE * 2U)
#define VALID_EPOCH       1704067200  /* 2024-01-01: wall clock is set */

static char s_dir[AIR_VOICE_PATH_MAX];

static void le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_header(uint8_t header[AIR_VOICE_WAV_HEADER], uint32_t data_bytes)
{
    memset(header, 0, AIR_VOICE_WAV_HEADER);
    memcpy(header, "RIFF", 4);
    le32(header + 4, 36U + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    le32(header + 16, 16U);
    le16(header + 20, 1U);                     /* PCM */
    le16(header + 22, 1U);                     /* mono */
    le32(header + 24, AIR_VOICE_SAMPLE_RATE);
    le32(header + 28, BYTES_PER_SECOND);
    le16(header + 32, 2U);
    le16(header + 34, 16U);
    memcpy(header + 36, "data", 4);
    le32(header + 40, data_bytes);
}

/* Streaming header: size fields hold 0xFFFFFFFF ("until end of file"), the
 * convention for WAV streams of unknown length. Patching the real sizes into
 * offset 0 afterwards would make LittleFS (copy-on-write) rewrite the whole
 * file, so the HTTP API fills them in on download instead. */
esp_err_t av_wav_begin(FILE *file)
{
    uint8_t header[AIR_VOICE_WAV_HEADER];
    wav_header(header, UINT32_MAX - 36U);
    le32(header + 40, UINT32_MAX);
    return fwrite(header, 1, sizeof(header), file) == sizeof(header) ? ESP_OK : ESP_FAIL;
}

void av_wav_patch_header(uint8_t header[AIR_VOICE_WAV_HEADER], uint64_t file_bytes)
{
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0 ||
        memcmp(header + 36, "data", 4) != 0 || file_bytes < AIR_VOICE_WAV_HEADER) {
        return; /* not the layout written by av_wav_begin() */
    }
    const uint64_t data = file_bytes - AIR_VOICE_WAV_HEADER;
    const uint32_t data32 = data > UINT32_MAX - 36U ? UINT32_MAX - 36U : (uint32_t)data;
    le32(header + 4, 36U + data32);
    le32(header + 40, data32);
}

esp_err_t av_store_init(void)
{
    if (claw_paths_join(CLAW_PATH_DATA, DIR_NAME, s_dir, sizeof(s_dir)) != ESP_OK) {
        s_dir[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(s_dir, 0775) != 0 && errno != EEXIST) {
        AV_LOGE("mkdir %s failed errno=%d", s_dir, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

const char *av_store_dir(void)
{
    return s_dir;
}

esp_err_t av_store_config_path(char *out, size_t out_size)
{
    const int n = snprintf(out, out_size, "%s/%s", s_dir, CONFIG_NAME);
    return (n > 0 && (size_t)n < out_size && s_dir[0]) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool air_voice_name_is_valid(const char *name)
{
    if (!name || !name[0] || name[0] == '.') {
        return false;
    }
    size_t len = 0;
    for (const char *p = name; *p; ++p, ++len) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || len + 1U >= AIR_VOICE_NAME_MAX) {
            return false;
        }
    }
    return true;
}

esp_err_t av_store_path(const char *name, av_file_kind_t kind, char *out, size_t out_size)
{
    if (!air_voice_name_is_valid(name) || !s_dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char *const SUFFIX[] = {
        [AV_FILE_PROCESSED] = ".wav",
        [AV_FILE_RAW] = RAW_SUFFIX ".wav",
        [AV_FILE_META] = ".json",
        [AV_FILE_PART] = ".part",
        [AV_FILE_RAW_PART] = RAW_SUFFIX ".part",
    };
    const int n = snprintf(out, out_size, "%s/%s%s", s_dir, name, SUFFIX[kind]);
    return (n > 0 && (size_t)n < out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool av_store_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

esp_err_t av_store_new_name(char *out, size_t out_size)
{
    char base[AIR_VOICE_NAME_MAX];
    const time_t now = time(NULL);
    if (now >= VALID_EPOCH) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(base, sizeof(base), "av_%Y%m%d_%H%M%S", &tm_now);
    } else {
        /* No wall clock yet: uptime plus a random tag avoids collisions
         * across reboots. */
        snprintf(base, sizeof(base), "av_t%06" PRId64 "_%04" PRIx32,
                 esp_timer_get_time() / 1000000, esp_random() & 0xFFFFU);
    }
    char path[AIR_VOICE_PATH_MAX];
    for (int i = 0; i < 100; ++i) {
        if (i == 0) {
            strlcpy(out, base, out_size);
        } else {
            snprintf(out, out_size, "%s_%d", base, i);
        }
        if (av_store_path(out, AV_FILE_PROCESSED, path, sizeof(path)) != ESP_OK) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (!av_store_exists(path)) {
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

uint32_t av_store_duration_ms(uint64_t file_bytes)
{
    if (file_bytes <= AIR_VOICE_WAV_HEADER) {
        return 0;
    }
    return (uint32_t)(((file_bytes - AIR_VOICE_WAV_HEADER) * 1000U) / BYTES_PER_SECOND);
}

static bool has_suffix(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);
    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

static void recover_one(const char *file_name)
{
    char part[AIR_VOICE_PATH_MAX];
    char wav[AIR_VOICE_PATH_MAX];
    const int n = snprintf(part, sizeof(part), "%s/%s", s_dir, file_name);
    if (n <= 0 || (size_t)n >= sizeof(part)) {
        return;
    }
    strlcpy(wav, part, sizeof(wav));
    char *dot = strrchr(wav, '.');
    if (!dot) {
        return;
    }
    strlcpy(dot, ".wav", sizeof(wav) - (size_t)(dot - wav));
    struct stat st;
    if (stat(part, &st) != 0) {
        return;
    }
    if (st.st_size < (off_t)AIR_VOICE_WAV_HEADER + 2) {
        (void)remove(part);
        AV_LOGW("removed empty partial %s", file_name);
        return;
    }
    /* The streaming header stays valid for any length: renaming is enough. */
    if (rename(part, wav) == 0) {
        AV_LOGW("recovered interrupted recording %s (%" PRIu32 " ms)",
                file_name, av_store_duration_ms((uint64_t)st.st_size));
    } else {
        AV_LOGE("recover %s failed errno=%d", file_name, errno);
    }
}

void av_store_recover_partials(void)
{
    DIR *dir = opendir(s_dir);
    if (!dir) {
        return;
    }
    /* Collect first: renaming while iterating is not portable. */
    char names[8][AIR_VOICE_NAME_MAX + 16];
    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < 8) {
        if (has_suffix(entry->d_name, ".part") &&
            strlen(entry->d_name) < sizeof(names[0])) {
            strlcpy(names[count++], entry->d_name, sizeof(names[0]));
        }
    }
    closedir(dir);
    for (size_t i = 0; i < count; ++i) {
        recover_one(names[i]);
    }
}

static bool newer(const air_voice_recording_t *a, const air_voice_recording_t *b)
{
    if (a->mtime != b->mtime) {
        return a->mtime > b->mtime;
    }
    return strcmp(a->name, b->name) > 0;
}

size_t av_store_list(air_voice_recording_t *out, size_t max, size_t *out_total)
{
    size_t count = 0;
    size_t total = 0;
    DIR *dir = opendir(s_dir);
    if (!dir) {
        if (out_total) *out_total = 0;
        return 0;
    }
    struct dirent *entry;
    char path[AIR_VOICE_PATH_MAX];
    while ((entry = readdir(dir)) != NULL) {
        const char *fname = entry->d_name;
        if (!has_suffix(fname, ".wav") || has_suffix(fname, RAW_SUFFIX ".wav")) {
            continue;
        }
        air_voice_recording_t rec = {0};
        const size_t base_len = strlen(fname) - 4U;
        if (base_len == 0 || base_len >= sizeof(rec.name)) {
            continue;
        }
        memcpy(rec.name, fname, base_len);
        rec.name[base_len] = '\0';
        if (!air_voice_name_is_valid(rec.name) ||
            av_store_path(rec.name, AV_FILE_PROCESSED, path, sizeof(path)) != ESP_OK) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        total++;
        rec.bytes = (uint64_t)st.st_size;
        rec.duration_ms = av_store_duration_ms(rec.bytes);
        rec.mtime = (int64_t)st.st_mtime;
        if (av_store_path(rec.name, AV_FILE_RAW, path, sizeof(path)) == ESP_OK &&
            stat(path, &st) == 0) {
            rec.has_raw = true;
            rec.raw_bytes = (uint64_t)st.st_size;
        }
        if (av_store_path(rec.name, AV_FILE_META, path, sizeof(path)) == ESP_OK) {
            rec.has_meta = av_store_exists(path);
        }
        if (!out || max == 0) {
            continue;
        }
        /* Keep the newest `max` entries in descending order. */
        size_t pos = count;
        while (pos > 0 && newer(&rec, &out[pos - 1U])) {
            pos--;
        }
        if (pos >= max) {
            continue;
        }
        const size_t last = count < max ? count : max - 1U;
        memmove(&out[pos + 1U], &out[pos], (last - pos) * sizeof(rec));
        out[pos] = rec;
        if (count < max) {
            count++;
        }
    }
    closedir(dir);
    if (out_total) {
        *out_total = total;
    }
    return count;
}

bool av_store_newest(char *out_name, size_t out_size, uint32_t *out_duration_ms)
{
    air_voice_recording_t rec;
    if (av_store_list(&rec, 1, NULL) == 0) {
        return false;
    }
    strlcpy(out_name, rec.name, out_size);
    if (out_duration_ms) {
        *out_duration_ms = rec.duration_ms;
    }
    return true;
}

esp_err_t av_store_delete(const char *name)
{
    char path[AIR_VOICE_PATH_MAX];
    esp_err_t err = av_store_path(name, AV_FILE_PROCESSED, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    if (!av_store_exists(path)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (remove(path) != 0) {
        AV_LOGE("delete %s failed errno=%d", path, errno);
        return ESP_FAIL;
    }
    if (av_store_path(name, AV_FILE_RAW, path, sizeof(path)) == ESP_OK) {
        (void)remove(path);
    }
    if (av_store_path(name, AV_FILE_META, path, sizeof(path)) == ESP_OK) {
        (void)remove(path);
    }
    return ESP_OK;
}

void av_store_space(uint64_t *out_free, uint64_t *out_total)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (claw_paths_get_space(CLAW_PATH_DATA, &total, &free_bytes) != ESP_OK) {
        total = free_bytes = 0;
    }
    if (out_free) *out_free = free_bytes;
    if (out_total) *out_total = total;
}
