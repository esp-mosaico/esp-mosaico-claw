/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "album_actions.h"
#include "album_media.h"
#include "album_binds.h"
#include "album_objects.h"
#include "album_templates.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#if defined(ESP_PLATFORM)
#include "claw_paths.h"
#endif

#define ALBUM_MAX_PHOTOS 128U
#define ALBUM_PATH_MAX 512U
#define ALBUM_SCAN_DEPTH 2
#define ALBUM_DOUBLE_TAP_US 350000LL
#define ALBUM_TAP_TIMER_MS 16U
#define ALBUM_CALL_CAPACITY 16U
#define ALBUM_SCALE_MIN_PERCENT 100U
#define ALBUM_SCALE_DOUBLE_PERCENT 220U
#define ALBUM_SCALE_MAX_PERCENT 400U
#define ALBUM_FULLSCREEN_IMAGE_SLOTS 1U
#define ALBUM_DYNAMIC_IMAGE_SLOTS (GSP_ALBUM_GRID_IMAGE_SLOTS + ALBUM_FULLSCREEN_IMAGE_SLOTS)
#define ALBUM_SELECT_MARKER_HIDE_OPACITY 0U
#define ALBUM_SELECT_MARKER_SHOW_OPACITY 100U

typedef enum {
    ALBUM_MODAL_NONE = 0,
    ALBUM_MODAL_CONFIRM_DELETE,
} album_modal_t;

typedef struct {
    char path[ALBUM_PATH_MAX];
} album_photo_t;

typedef struct {
    esp_gsp_grid_cell_t cell;
    uint32_t ticket;
    bool bound;
} album_cell_t;

typedef struct {
    uint16_t action;
    uint16_t list;
    uint32_t item;
} album_call_t;

typedef struct {
    esp_gsp_handle_t ui;
    esp_gsp_grid_t grid;
    album_photo_t photos[ALBUM_MAX_PHOTOS];
    bool selected[ALBUM_MAX_PHOTOS];
    size_t photo_count;
    size_t active_index;
    size_t current_index;
    bool multi_select;
    bool fullscreen;
    int64_t last_tap_us;
    int64_t pending_close_us;
    uint32_t image_scale_q16;
    uint32_t pinch_base_q16;
    void *tap_timer;
    album_modal_t modal;
    struct stat stat_buf;
    album_media_t *media;
    album_cell_t cells[GSP_ALBUM_GRID_IMAGE_SLOTS];
    uint32_t preview_ticket;
    uint32_t preview_generation;
    bool delete_busy;
    uint16_t delete_indices[ALBUM_MAX_PHOTOS];
    uint32_t grid_total;
    bool grid_shown;
    album_call_t calls[ALBUM_CALL_CAPACITY];
    atomic_uint call_read;
    atomic_uint call_write;
} album_state_t;

static EXT_RAM_BSS_ATTR album_state_t s_album;
static const char *TAG = "mosaic_album";
/* Lifecycle callbacks run on the loader; bind/pinch/timer callbacks run on
 * the renderer. Keep all live album mutations on the latter. */
static atomic_bool s_stopping = true;
static atomic_uint s_callbacks;
/* Never reset on app restart: late completions cannot match a new session. */
static uint32_t s_preview_generation;

static bool album_callback_enter(void)
{
    atomic_fetch_add(&s_callbacks, 1);
    if (atomic_load(&s_stopping)) {
        atomic_fetch_sub(&s_callbacks, 1);
        return false;
    }
    return true;
}

static void album_callback_leave(void)
{
    atomic_fetch_sub(&s_callbacks, 1);
}

static uint32_t album_rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((uint32_t)(red & 0xF8U) << 8) | ((uint32_t)(green & 0xFCU) << 3) | ((uint32_t)blue >> 3);
}

static bool album_grid_visible(void)
{
    return !s_album.fullscreen && s_album.modal == ALBUM_MODAL_NONE;
}

static void album_refresh_grid(esp_gsp_handle_t ui)
{
    bool visible = album_grid_visible();
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_EMPTY_VISIBLE, visible && s_album.photo_count == 0);
    if (s_album.grid_shown != visible) {
        (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_GRID_VISIBLE, visible);
        s_album.grid_shown = visible;
        if (!visible) {
            for (size_t i = 0; i < GSP_ALBUM_GRID_IMAGE_SLOTS; ++i) {
                album_media_cancel(s_album.media, i + 1);
                s_album.cells[i].bound = false;
            }
        }
    }
    if (s_album.grid != ESP_GSP_GRID_NONE && s_album.grid_total != s_album.photo_count) {
        esp_gsp_err_t ret = gsp_album_album_set_total(ui, s_album.grid, s_album.photo_count);
        if (ret == ESP_GSP_OK) {
            s_album.grid_total = s_album.photo_count;
        } else {
            ESP_LOGE(TAG, "Set grid total failed: %d", (int)ret);
        }
    }
}

static bool album_is_jpeg_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return false;
    }
    char ext[6] = { 0 };
    for (size_t i = 0; i < sizeof(ext) - 1 && dot[i] != '\0'; ++i) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }
    return strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0;
}

static bool album_has_photo(const char *path)
{
    for (size_t index = 0; index < s_album.photo_count; ++index) {
        if (strcmp(s_album.photos[index].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static void album_add_photo(const char *path)
{
    if (s_album.photo_count >= ALBUM_MAX_PHOTOS || album_has_photo(path)) {
        return;
    }
    album_photo_t *photo = &s_album.photos[s_album.photo_count++];
    int written = snprintf(photo->path, sizeof(photo->path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(photo->path)) {
        ESP_LOGW(TAG, "photo path too long, skipped");
        --s_album.photo_count;
        return;
    }
}

static bool album_join_path(const char *dir, const char *name, char *out, size_t out_size)
{
    const char *separator = (dir[0] != '\0' && dir[strlen(dir) - 1] == '/') ? "" : "/";
    int written = snprintf(out, out_size, "%s%s%s", dir, separator, name);
    return written >= 0 && (size_t)written < out_size;
}

static void album_scan_dir_recursive(const char *dir_path, int depth)
{
    DIR *dir = opendir(dir_path);
    if (dir == NULL) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "scan dir failed: %s errno=%d", dir_path, errno);
        }
        return;
    }
    char *child = malloc(ALBUM_PATH_MAX);
    if (child == NULL) {
        ESP_LOGE(TAG, "alloc scan path failed");
        closedir(dir);
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && s_album.photo_count < ALBUM_MAX_PHOTOS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!album_join_path(dir_path, entry->d_name, child, ALBUM_PATH_MAX)) {
            ESP_LOGW(TAG, "joined path too long under %s", dir_path);
            continue;
        }
        if (stat(child, &s_album.stat_buf) != 0) {
            ESP_LOGW(TAG, "stat failed: %s errno=%d", child, errno);
            continue;
        }
        if (S_ISDIR(s_album.stat_buf.st_mode) && depth > 0) {
            album_scan_dir_recursive(child, depth - 1);
        } else if (S_ISREG(s_album.stat_buf.st_mode) && album_is_jpeg_path(child)) {
            album_add_photo(child);
        }
    }
    free(child);
    closedir(dir);
}

static void album_scan_host_defaults(void)
{
#if !defined(ESP_PLATFORM)
#if defined(__EMSCRIPTEN__)
    /* WebAssembly has no access to the viewer's local photo library. The
     * simulator packages the project's FATFS seed photos at this path. */
    album_scan_dir_recursive("/mosaic_assets/photos", ALBUM_SCAN_DEPTH);
    return;
#endif
    const char *env_dirs = getenv("MOSAIC_ALBUM_DIRS");
    if (env_dirs != NULL && env_dirs[0] != '\0') {
        char *copy = strdup(env_dirs);
        if (copy == NULL) {
            ESP_LOGE(TAG, "alloc env dir list failed");
            return;
        }
        char *save = NULL;
        for (char *token = strtok_r(copy, ":", &save); token != NULL; token = strtok_r(NULL, ":", &save)) {
            album_scan_dir_recursive(token, ALBUM_SCAN_DEPTH);
        }
        free(copy);
        return;
    }
    const char *home = getenv("HOME");
    char *path = malloc(ALBUM_PATH_MAX);
    if (path == NULL) {
        ESP_LOGE(TAG, "alloc default scan path failed");
        return;
    }
    if (home != NULL && home[0] != '\0') {
        if (album_join_path(home, "Pictures", path, ALBUM_PATH_MAX)) {
            album_scan_dir_recursive(path, ALBUM_SCAN_DEPTH);
        }
        if (album_join_path(home, "Downloads", path, ALBUM_PATH_MAX)) {
            album_scan_dir_recursive(path, ALBUM_SCAN_DEPTH);
        }
        if (album_join_path(home, "图片", path, ALBUM_PATH_MAX)) {
            album_scan_dir_recursive(path, ALBUM_SCAN_DEPTH);
        }
    }
#if defined(MOSAIC_PROJECT_ROOT)
    /* Product-owned simulator sources use the product repository layout. */
    album_scan_dir_recursive(MOSAIC_PROJECT_ROOT "/fatfs_image/storage/photos", ALBUM_SCAN_DEPTH);
#elif defined(MOSAIC_ESP_CLAW_ROOT)
    /* Host simulator assets live under the build tree; scan the project FATFS seed from the project root. */
    album_scan_dir_recursive(MOSAIC_ESP_CLAW_ROOT "/application/edge_agent/fatfs_image/storage/photos", ALBUM_SCAN_DEPTH);
#endif
    free(path);
#endif
}

static void album_scan_esp_defaults(void)
{
#if defined(ESP_PLATFORM)
    static const char *const dirs[] = {
        "",
        "album",
        "camera",
        "DCIM",
        "images",
        "photos",
        "pictures",
    };
    char *path = malloc(ALBUM_PATH_MAX);
    if (path == NULL) {
        ESP_LOGE(TAG, "alloc esp scan path failed");
        return;
    }
    for (size_t index = 0; index < sizeof(dirs) / sizeof(dirs[0]); ++index) {
        esp_err_t err = claw_paths_join(CLAW_PATH_DATA, dirs[index], path, ALBUM_PATH_MAX);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "compose scan dir failed: %s", dirs[index]);
            continue;
        }
        album_scan_dir_recursive(path, dirs[index][0] == '\0' ? 1 : ALBUM_SCAN_DEPTH);
    }
    free(path);
#endif
}

static int album_compare_photo_path(const void *lhs, const void *rhs)
{
    const album_photo_t *left = lhs;
    const album_photo_t *right = rhs;
    return strcmp(left->path, right->path);
}

static void album_scan(void)
{
    s_album.photo_count = 0;
    memset(s_album.selected, 0, sizeof(s_album.selected));
    album_scan_esp_defaults();
    album_scan_host_defaults();
    qsort(s_album.photos, s_album.photo_count, sizeof(s_album.photos[0]), album_compare_photo_path);
    ESP_LOGI(TAG, "scanned %u jpg/jpeg photos", (unsigned)s_album.photo_count);
}

static void album_bind_cell_image(esp_gsp_handle_t ui, esp_gsp_grid_cell_t cell, uint32_t photo_index)
{
    if (cell.row.slot >= GSP_ALBUM_GRID_IMAGE_SLOTS || photo_index >= s_album.photo_count) {
        return;
    }
    album_cell_t *entry = &s_album.cells[cell.row.slot];
    if (entry->bound && entry->cell.row.instance == cell.row.instance &&
        entry->cell.row.item == cell.row.item) {
        return;
    }
    entry->cell = cell;
    entry->bound = true;
    /* Recycled rows must not show another photo while their request loads.
     * A shared one-pixel QOI provides a cheap, opaque loading surface. */
    static const uint8_t placeholder[] = {
        'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 3, 0,
        0xFE, 0x18, 0x18, 0x19, 0, 0, 0, 0, 0, 0, 0, 1,
    };
    const esp_gsp_image_options_t options = {.cache_key = 0xA1B00001U};
    (void)esp_gsp_grid_cell_set_image_ex(ui, cell, placeholder, sizeof(placeholder), &options);
    entry->ticket = album_media_request(s_album.media, cell.row.slot + 1,
                                        s_album.photos[photo_index].path, true);
}

static void album_bind_cell_selection(esp_gsp_handle_t ui, esp_gsp_grid_cell_t cell, uint32_t photo_index)
{
    const bool show = photo_index < s_album.photo_count && s_album.multi_select;
    esp_gsp_err_t ret = gsp_album_album_cell_row_set_select_circle_opacity(
        ui, cell.row, show ? ALBUM_SELECT_MARKER_SHOW_OPACITY : ALBUM_SELECT_MARKER_HIDE_OPACITY);
    if (ret != ESP_GSP_OK) {
        ESP_LOGW(TAG, "set grid select indicator opacity failed index=%u err=%d", (unsigned)photo_index, (int)ret);
    }
    const uint32_t color = (show && s_album.selected[photo_index]) ? album_rgb565(0xFF, 0x4C, 0x01) : album_rgb565(0x18, 0x18, 0x19);
    ret = gsp_album_album_cell_row_set_select_circle_color(ui, cell.row, color);
    if (ret != ESP_GSP_OK) {
        ESP_LOGW(TAG, "set grid select indicator color failed index=%u err=%d", (unsigned)photo_index, (int)ret);
    }
}

static gsp_err_t album_bind_cell(esp_gsp_handle_t ui, esp_gsp_grid_cell_t cell, uint32_t photo_index, void *user_ctx)
{
    (void)user_ctx;
    if (!album_callback_enter()) {
        return GSP_OK;
    }
    /* Ignore binds queued before the viewport was hidden. */
    if (album_grid_visible()) {
        album_bind_cell_image(ui, cell, photo_index);
        album_bind_cell_selection(ui, cell, photo_index);
    }
    album_callback_leave();
    return GSP_OK;
}

static size_t album_selected_count(void)
{
    size_t count = 0;
    for (size_t index = 0; index < s_album.photo_count; ++index) {
        count += s_album.selected[index] ? 1U : 0U;
    }
    return count;
}

static void album_clamp_indices(void)
{
    if (s_album.active_index != SIZE_MAX && s_album.active_index >= s_album.photo_count) {
        s_album.active_index = s_album.photo_count > 0 ? s_album.photo_count - 1 : SIZE_MAX;
    }
    if (s_album.current_index != SIZE_MAX && s_album.current_index >= s_album.photo_count) {
        s_album.current_index = s_album.photo_count > 0 ? s_album.photo_count - 1 : SIZE_MAX;
    }
}

static void album_refresh_toolbar(esp_gsp_handle_t ui)
{
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_TOOLBAR_NORMAL_VISIBLE, !s_album.fullscreen && !s_album.multi_select);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_TOOLBAR_SELECT_VISIBLE, !s_album.fullscreen && s_album.multi_select);
    char text[48];
    size_t selected_count = album_selected_count();
    bool has_selection = selected_count > 0;
    snprintf(text, sizeof(text), "%u selected", (unsigned)selected_count);
    (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_SELECT_COUNT, text);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_SELECT_DELETE_ENABLED_VISIBLE, has_selection);
}

static void album_refresh_selection(esp_gsp_handle_t ui)
{
    /* Rebind decorations only. The binder skips unchanged image identities. */
    if (s_album.grid != ESP_GSP_GRID_NONE && album_grid_visible()) {
        (void)gsp_album_album_refresh(ui, s_album.grid);
    }
}

static void album_refresh_cells(esp_gsp_handle_t ui, bool force_images)
{
    if (force_images) {
        for (size_t i = 0; i < GSP_ALBUM_GRID_IMAGE_SLOTS; ++i) {
            album_media_cancel(s_album.media, i + 1);
            s_album.cells[i].bound = false;
        }
    }
    album_clamp_indices();
    album_refresh_grid(ui);
    album_refresh_selection(ui);
    album_refresh_toolbar(ui);
}

static void album_hide_modal(esp_gsp_handle_t ui)
{
    s_album.modal = ALBUM_MODAL_NONE;
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_MODAL_VISIBLE, false);
    album_refresh_grid(ui);
}

static void album_show_delete_confirm(esp_gsp_handle_t ui)
{
    size_t count = album_selected_count();
    if (count == 0) {
        return;
    }
    char text[80];
    s_album.modal = ALBUM_MODAL_CONFIRM_DELETE;
    (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_MODAL_TITLE, "Confirm Delete");
    snprintf(text, sizeof(text), "Remove %u item%s?", (unsigned)count, count == 1 ? "" : "s");
    (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_NAME, text);
    (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_SIZE, "This cannot be undone.");
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_MODAL_VISIBLE, true);
    album_refresh_grid(ui);
}

static void album_confirm_delete(esp_gsp_handle_t ui)
{
    if (s_album.modal != ALBUM_MODAL_CONFIRM_DELETE || s_album.delete_busy) {
        return;
    }
    size_t count = album_selected_count();
    const char **paths = calloc(count, sizeof(*paths));
    if (!paths) {
        ESP_LOGE(TAG, "Cannot allocate delete request");
        (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_SIZE, "Unable to delete. Please retry.");
        return;
    }
    size_t at = 0;
    for (size_t index = 0; index < s_album.photo_count; ++index) {
        if (s_album.selected[index]) {
            paths[at] = s_album.photos[index].path;
            s_album.delete_indices[at++] = index;
        }
    }
    bool accepted = album_media_delete(s_album.media, paths, count);
    free(paths);
    if (!accepted) {
        ESP_LOGE(TAG, "Cannot queue delete request");
        (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_SIZE, "Unable to delete. Please retry.");
        return;
    }
    s_album.delete_busy = true;
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_DELETE_ACTIONS_VISIBLE, false);
    (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_MODAL_TITLE, "Deleting...");
    (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_SIZE, "Please wait.");
}

static void album_finish_delete(esp_gsp_handle_t ui)
{
    bool results[ALBUM_MAX_PHOTOS];
    size_t count;
    if (!s_album.delete_busy ||
        !album_media_take_deleted(s_album.media, results, ALBUM_MAX_PHOTOS, &count)) {
        return;
    }
    bool removed[ALBUM_MAX_PHOTOS] = {0};
    size_t failures = 0;
    for (size_t i = 0; i < count; ++i) {
        removed[s_album.delete_indices[i]] = results[i];
        failures += !results[i];
    }
    /* The UI owns this immutable list during deletion. Compact successful
     * removals in memory instead of rescanning storage on the render task. */
    size_t kept = 0;
    for (size_t i = 0; i < s_album.photo_count; ++i) {
        if (!removed[i]) {
            s_album.photos[kept] = s_album.photos[i];
            s_album.selected[kept++] = s_album.selected[i];
        }
    }
    s_album.photo_count = kept;
    s_album.delete_busy = false;
    s_album.active_index = SIZE_MAX;
    s_album.current_index = SIZE_MAX;
    s_album.multi_select = failures != 0;
    s_album.fullscreen = false;
    s_album.pending_close_us = 0;
    s_album.modal = failures ? ALBUM_MODAL_CONFIRM_DELETE : ALBUM_MODAL_NONE;
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_DELETE_ACTIONS_VISIBLE, true);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_FULLSCREEN_VISIBLE, false);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_MODAL_VISIBLE, failures != 0);
    if (failures) {
        char text[64];
        snprintf(text, sizeof(text), "%u file(s) could not be removed.", (unsigned)failures);
        ESP_LOGW(TAG, "Delete incomplete: %u failures", (unsigned)failures);
        (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_MODAL_TITLE, "Delete Incomplete");
        (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_NAME, text);
        (void)esp_gsp_set_text(ui, GSP_BIND_ALBUM_DETAIL_SIZE, "Retry or cancel.");
    }
    album_refresh_cells(ui, true);
}

static void album_reset_full_scale(esp_gsp_handle_t ui)
{
    s_album.image_scale_q16 = ESP_GSP_SCALE_Q16_ONE;
    s_album.pinch_base_q16 = ESP_GSP_SCALE_Q16_ONE;
    (void)gsp_album_album_full_image_set_scale_q16(ui, s_album.image_scale_q16);
}

static void album_show_grid(esp_gsp_handle_t ui)
{
    s_album.fullscreen = false;
    album_media_cancel(s_album.media, 0);
    s_album.preview_ticket = 0;
    s_album.pending_close_us = 0;
    s_album.last_tap_us = 0;
    album_reset_full_scale(ui);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_FULLSCREEN_VISIBLE, false);
    album_refresh_grid(ui);
    album_refresh_toolbar(ui);
}

static void album_show_fullscreen(esp_gsp_handle_t ui, size_t index)
{
    if (index >= s_album.photo_count) {
        return;
    }
    s_album.current_index = index;
    s_album.active_index = index;
    s_album.multi_select = false;
    s_album.pending_close_us = 0;
    s_album.last_tap_us = 0;
    album_hide_modal(ui);
    if (++s_preview_generation == 0) {
        ++s_preview_generation;
    }
    s_album.preview_generation = s_preview_generation;
    s_album.preview_ticket = album_media_request(s_album.media, 0,
                                                 s_album.photos[index].path, false);
    /* Keep the grid (or the current preview) until this request is decoded.
     * A successful submit only queues decoding; it is not a publication fence. */
    album_refresh_toolbar(ui);
}

static void album_preview_complete(esp_gsp_handle_t ui, uint16_t bind,
                                    gsp_err_t status, void *ctx)
{
    (void)bind;
    if (!album_callback_enter()) {
        return;
    }
    /* The Mosaic integration disables async GSP decoding. This callback and
     * album_tick therefore share the render task, allowing image publication,
     * scale reset and visibility changes to reach the same rendered frame. */
    uint32_t generation = (uint32_t)(uintptr_t)ctx;
    if (ui == s_album.ui && s_album.preview_ticket != 0 &&
        generation == s_album.preview_generation) {
        s_album.preview_ticket = 0;
        if (status == GSP_OK) {
            s_album.fullscreen = true;
            album_reset_full_scale(ui);
            album_refresh_grid(ui);
            (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_FULLSCREEN_VISIBLE, true);
            album_refresh_toolbar(ui);
        } else {
            ESP_LOGW(TAG, "Preview decode failed: %d", (int)status);
        }
    }
    album_callback_leave();
}

static void album_move_fullscreen(esp_gsp_handle_t ui, int delta)
{
    if (!s_album.fullscreen || s_album.photo_count == 0) {
        return;
    }
    size_t next = s_album.current_index;
    if (delta < 0) {
        next = next == 0 ? s_album.photo_count - 1 : next - 1;
    } else if (delta > 0) {
        next = next + 1 >= s_album.photo_count ? 0 : next + 1;
    }
    album_show_fullscreen(ui, next);
}

static void album_toggle_photo(esp_gsp_handle_t ui, size_t index)
{
    if (index >= s_album.photo_count) {
        return;
    }
    s_album.active_index = index;
    if (s_album.multi_select) {
        s_album.selected[index] = !s_album.selected[index];
        /* Only this photo's marker changed. Offscreen rows pick up selection
         * when the normal binder assigns them, without a Grid refresh. */
        for (size_t i = 0; i < GSP_ALBUM_GRID_IMAGE_SLOTS; ++i) {
            album_cell_t *entry = &s_album.cells[i];
            if (entry->bound && entry->cell.row.item == index) {
                album_bind_cell_selection(ui, entry->cell, index);
            }
        }
        album_refresh_toolbar(ui);
        return;
    }
    album_show_fullscreen(ui, index);
}

static void album_enter_select(esp_gsp_handle_t ui)
{
    album_media_cancel(s_album.media, 0);
    s_album.preview_ticket = 0;
    s_album.multi_select = true;
    memset(s_album.selected, 0, sizeof(s_album.selected));
    album_refresh_selection(ui);
    album_refresh_toolbar(ui);
}

static void album_cancel_select(esp_gsp_handle_t ui)
{
    s_album.multi_select = false;
    memset(s_album.selected, 0, sizeof(s_album.selected));
    album_refresh_selection(ui);
    album_refresh_toolbar(ui);
}

static void album_toggle_zoom(esp_gsp_handle_t ui)
{
    uint32_t one = esp_gsp_scale_q16_from_percent(ALBUM_SCALE_MIN_PERCENT);
    uint32_t zoom = esp_gsp_scale_q16_from_percent(ALBUM_SCALE_DOUBLE_PERCENT);
    s_album.image_scale_q16 = s_album.image_scale_q16 <= one + (one / 10U) ? zoom : one;
    s_album.pinch_base_q16 = s_album.image_scale_q16;
    (void)gsp_album_album_full_image_set_scale_q16(ui, s_album.image_scale_q16);
}

static void album_handle_fullscreen_tap(esp_gsp_handle_t ui)
{
    if (!s_album.fullscreen) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_album.last_tap_us != 0 && now_us - s_album.last_tap_us <= ALBUM_DOUBLE_TAP_US) {
        s_album.pending_close_us = 0;
        s_album.last_tap_us = 0;
        album_toggle_zoom(ui);
        return;
    }
    s_album.last_tap_us = now_us;
    s_album.pending_close_us = now_us + ALBUM_DOUBLE_TAP_US;
}

static bool album_apply_pinch(esp_gsp_handle_t ui, const esp_gsp_pinch_event_t *event, void *user_ctx)
{
    (void)user_ctx;
    if (event == NULL || !s_album.fullscreen) {
        return false;
    }
    if (event->phase == ESP_GSP_PINCH_BEGIN) {
        s_album.pinch_base_q16 = s_album.image_scale_q16;
        s_album.pending_close_us = 0;
        return true;
    }
    uint32_t scale = esp_gsp_scale_q16_multiply(s_album.pinch_base_q16, event->relative_scale_q16);
    scale = esp_gsp_scale_q16_clamp(scale, esp_gsp_scale_q16_from_percent(ALBUM_SCALE_MIN_PERCENT), esp_gsp_scale_q16_from_percent(ALBUM_SCALE_MAX_PERCENT));
    if (event->phase == ESP_GSP_PINCH_UPDATE || event->phase == ESP_GSP_PINCH_END) {
        s_album.image_scale_q16 = scale;
        (void)gsp_album_album_full_image_set_scale_q16(ui, scale);
        if (event->phase == ESP_GSP_PINCH_END) {
            s_album.pinch_base_q16 = scale;
        }
    } else if (event->phase == ESP_GSP_PINCH_CANCEL) {
        (void)gsp_album_album_full_image_set_scale_q16(ui, s_album.pinch_base_q16);
        s_album.image_scale_q16 = s_album.pinch_base_q16;
    }
    return true;
}

static bool album_on_pinch(esp_gsp_handle_t ui, const esp_gsp_pinch_event_t *event, void *user_ctx)
{
    if (!album_callback_enter()) {
        return false;
    }
    bool handled = album_apply_pinch(ui, event, user_ctx);
    album_callback_leave();
    return handled;
}

static void album_handle_grid_cell_call(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    if (!album_grid_visible() || s_album.grid == ESP_GSP_GRID_NONE || event->data.call.list != s_album.grid || event->data.call.item >= s_album.photo_count) {
        return;
    }
    album_toggle_photo(ui, (size_t)event->data.call.item);
}

static esp_err_t album_handle_call(esp_gsp_handle_t ui, const mosaic_event_t *event)
{
    if (s_album.delete_busy) {
        return ESP_OK;
    }
    uint16_t action_id = event->data.call.action_id;
    if (action_id == GSP_ACT_ID_ALBUM_ROW) {
        /* Keep the authored callback id stable while Grid supplies cell item indexes. */
        album_handle_grid_cell_call(ui, event);
        return ESP_OK;
    }
    switch (action_id) {
    case GSP_ACT_ID_ALBUM_DELETE:
        album_show_delete_confirm(ui);
        break;
    case GSP_ACT_ID_ALBUM_SELECT:
        album_enter_select(ui);
        break;
    case GSP_ACT_ID_ALBUM_CANCEL_SELECT:
        album_cancel_select(ui);
        break;
    case GSP_ACT_ID_ALBUM_CANCEL_MODAL:
        if (s_album.modal == ALBUM_MODAL_NONE) {
            return ESP_ERR_NOT_FOUND;
        }
        album_hide_modal(ui);
        break;
    case GSP_ACT_ID_ALBUM_CONFIRM_DELETE:
        album_confirm_delete(ui);
        break;
    case GSP_ACT_ID_ALBUM_FULLSCREEN_TAP:
        album_handle_fullscreen_tap(ui);
        break;
    case GSP_ACT_ID_ALBUM_PREV:
        album_move_fullscreen(ui, -1);
        break;
    case GSP_ACT_ID_ALBUM_NEXT:
        album_move_fullscreen(ui, 1);
        break;
    default:
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void album_check_pending_close(esp_gsp_handle_t ui, int64_t now_us)
{
    if (s_album.fullscreen && s_album.pending_close_us != 0 && now_us >= s_album.pending_close_us) {
        album_show_grid(ui);
    }
}

static void album_publish_ready(esp_gsp_handle_t ui)
{
    /* At most one new image submission per service tick. The preview wins. */
    for (size_t channel = 0; channel <= GSP_ALBUM_GRID_IMAGE_SLOTS; ++channel) {
        uint32_t ticket;
        album_image_t *image = NULL;
        if (!album_media_take(s_album.media, channel, &ticket, &image)) {
            continue;
        }
        bool current = channel == 0
            ? s_album.preview_ticket != 0 && ticket == s_album.preview_ticket
            : album_grid_visible() && s_album.cells[channel - 1].bound &&
              ticket == s_album.cells[channel - 1].ticket;
        if (!current || !image) {
            if (current && channel == 0 && !image) {
                s_album.preview_ticket = 0;
                ESP_LOGW(TAG, "Preview file could not be loaded");
            }
            album_image_release(image);
            continue;
        }
        esp_gsp_image_options_t options = {
            .ownership = ESP_GSP_IMAGE_BORROW,
            .on_release = album_image_release,
            .release_ctx = image,
            .cache_key = album_image_key(image),
        };
        if (channel == 0) {
            options.on_complete = album_preview_complete;
            options.complete_ctx = (void *)(uintptr_t)s_album.preview_generation;
        }
        esp_gsp_err_t ret = channel == 0
            ? esp_gsp_set_image_ex(ui, GSP_BIND_ALBUM_FULL_IMAGE,
                                   album_image_data(image), album_image_size(image), &options)
            : esp_gsp_grid_cell_set_image_ex(ui, s_album.cells[channel - 1].cell,
                                             album_image_data(image), album_image_size(image), &options);
        if (ret != ESP_GSP_OK) {
            if (channel == 0) {
                s_album.preview_ticket = 0;
            }
            album_image_release(image);
            ESP_LOGW(TAG, "Image submission failed: channel=%u error=%d", (unsigned)channel, (int)ret);
        }
        break;
    }
}

static void album_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)user_ctx;
    if (!album_callback_enter()) {
        return;
    }
    unsigned read = atomic_load(&s_album.call_read);
    unsigned write = atomic_load(&s_album.call_write);
    for (unsigned count = 0; read != write && count < 4; ++count) {
        album_call_t call = s_album.calls[read % ALBUM_CALL_CAPACITY];
        atomic_store(&s_album.call_read, ++read);
        mosaic_event_t event = {.type = MOSAIC_EVENT_UI_CALL};
        event.data.call.action_id = call.action;
        event.data.call.list = call.list;
        event.data.call.item = call.item;
        (void)album_handle_call(ui, &event);
    }
    album_finish_delete(ui);
    album_check_pending_close(ui, esp_timer_get_time());
    album_publish_ready(ui);
    album_callback_leave();
}

static void album_started(esp_gsp_handle_t ui)
{
    memset(&s_album, 0, sizeof(s_album));
    atomic_init(&s_album.call_read, 0);
    atomic_init(&s_album.call_write, 0);
    s_album.ui = ui;
    s_album.grid_total = UINT32_MAX;
    s_album.grid_shown = true;
    s_album.media = album_media_create(GSP_ALBUM_GRID_IMAGE_SLOTS + 1);
    if (s_album.media == NULL) {
        ESP_LOGE(TAG, "Cannot start image worker");
    }
    s_album.grid = ESP_GSP_GRID_NONE;
    s_album.active_index = SIZE_MAX;
    s_album.current_index = SIZE_MAX;
    s_album.image_scale_q16 = ESP_GSP_SCALE_Q16_ONE;
    s_album.pinch_base_q16 = ESP_GSP_SCALE_Q16_ONE;
    album_scan();
    atomic_store(&s_stopping, false);
    s_album.grid = gsp_album_album_bind(ui, album_bind_cell, &s_album);
    if (s_album.grid == ESP_GSP_GRID_NONE) {
        ESP_LOGE(TAG, "album grid bind failed");
    }
    album_refresh_cells(ui, true);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_FULLSCREEN_VISIBLE, false);
    (void)esp_gsp_set_visible(ui, GSP_BIND_ALBUM_MODAL_VISIBLE, false);
    esp_err_t err = esp_gsp_on_pinch(ui, album_on_pinch, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pinch registration failed err=%d", (int)err);
    }
    s_album.tap_timer = esp_gsp_timer_create(ui, ALBUM_TAP_TIMER_MS, album_tick, NULL);
    if (s_album.tap_timer == NULL) {
        ESP_LOGE(TAG, "tap timer create failed");
    }
}

static void album_stopping(esp_gsp_handle_t ui)
{
    atomic_store(&s_stopping, true);
    /* No worker calls into GSP. Wait only for short UI callbacks before
     * joining the worker; encoded payload refs can outlive this album. */
    while (atomic_load(&s_callbacks) != 0) {
#if defined(ESP_PLATFORM)
        vTaskDelay(1);
#endif
    }
    if (s_album.tap_timer != NULL) {
        esp_gsp_err_t timer_ret = esp_gsp_timer_delete(ui, s_album.tap_timer);
        if (timer_ret != ESP_GSP_OK) {
            ESP_LOGE(TAG, "tap timer delete failed err=%d", (int)timer_ret);
        }
        s_album.tap_timer = NULL;
    }
    (void)esp_gsp_on_pinch(ui, NULL, NULL);
    album_media_destroy(s_album.media);
    s_album.media = NULL;
}

static void album_event(esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    (void)ui;
    if (event->type == MOSAIC_EVENT_UI_CALL && !atomic_load(&s_stopping)) {
        unsigned write = atomic_load(&s_album.call_write);
        if (write - atomic_load(&s_album.call_read) == ALBUM_CALL_CAPACITY) {
            ESP_LOGW(TAG, "Album action queue full");
            return;
        }
        s_album.calls[write % ALBUM_CALL_CAPACITY] = (album_call_t){
            .action = event->data.call.action_id,
            .list = event->data.call.list,
            .item = event->data.call.item,
        };
        atomic_store(&s_album.call_write, write + 1);
    }
}

const mosaic_app_descriptor_t mosaic_album_app = {
    .id = 9,
    .launch_action = GSP_ACT_ID_APP_ALBUM,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .name = "album",
    .title = "Album",
    .directory = &gsp_obj_directory_album,
    .disable_swipe = true,
    /* Album owns its top toolbar; shared Shell contributes only the bottom
     * upward-exit indicator and gesture. */
    .root_header_in_stack = true,
    .instance_slots = GSP_TEMPLATE_ALBUM_CELL_MAX_INSTANCES,
    .dynamic_image_slots = ALBUM_DYNAMIC_IMAGE_SLOTS,
    .image_cache_bytes = 8U * 1024U * 1024U,
    .on_started = album_started,
    .on_stopping = album_stopping,
    .on_event = album_event,
};
