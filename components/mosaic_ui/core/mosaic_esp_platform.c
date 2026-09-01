/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_esp_platform.h"
#include "mosaic_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_paths.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_mmap_assets.h"
#include "esp_timer.h"
#include "esp_gsp_deployable.h"
#include "gsp/gsp_font_catalog.h"
#include "mosaic_app_shell.h"
#include "mosaic_logic.h"

#define MOSAIC_UI_ASSETS_PARTITION "ui_apps"
#define MOSAIC_ASSET_NAME_MAX 32U
#define MOSAIC_POINTER_SAMPLE_DELTA 12
#define MOSAIC_FONT_PATH_MAX 128U
#define MOSAIC_FONT_CATALOG_ASSET "common-fonts"
#define MOSAIC_DYNAMIC_FONT_PATH "fonts/NotoSansSC-Regular-sub.ttf"
#define MOSAIC_DYNAMIC_FONT_CACHE_GLYPHS 64U
#define MOSAIC_DYNAMIC_FONT_GLYPH_MAX_PX 40U
#define MOSAIC_FREETYPE_RENDER_TASK_STACK_SIZE 32768U

typedef struct {
    struct mosaic_esp_platform_t* platform;
    const mosaic_app_package_t* package;
    esp_gsp_handle_t ui;
    mosaic_logic_handle_t logic;
    esp_gsp_deployable_bundle_t* deployable;
    mosaic_platform_ui_event_cb_t event_cb;
    void* event_ctx;
    uint32_t generation;
    int32_t pointer_x;
    int32_t pointer_y;
    bool pointer_pressed;
    bool pointer_observer_attached;
    bool shell_attached;
    bool stopping;
} mosaic_esp_app_t;

struct mosaic_esp_platform_t {
    mosaic_esp_platform_config_t config;
    mmap_assets_handle_t assets;
    gsp_font_catalog_t* font_catalog;
    uint8_t* dynamic_font_data;
    size_t dynamic_font_size;
    mosaic_esp_app_t* active;
    esp_gsp_handle_t hub_ui;
    esp_gsp_handle_t app_ui;
    esp_gsp_esp_lcd_session_t* hub_session;
    esp_gsp_esp_lcd_pause_t* present_pause;
    esp_gsp_esp_lcd_pause_t* screen_pause;
    uint32_t next_generation;
    bool paused;
};

static esp_err_t load_dynamic_font(mosaic_esp_platform_handle_t platform)
{
    if (platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (platform->dynamic_font_data != NULL) {
        return ESP_OK;
    }

    char path[MOSAIC_FONT_PATH_MAX];
    ESP_RETURN_ON_ERROR(
        claw_paths_join(CLAW_PATH_SYSTEM, MOSAIC_DYNAMIC_FONT_PATH, path,
            sizeof(path)),
        "mosaic_platform", "resolve dynamic font path");

    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        printf("mosaic_platform: open dynamic font %s failed\n", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    const long raw_size = ftell(file);
    if (raw_size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t* data = heap_caps_malloc(
        (size_t)raw_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        fclose(file);
        printf("mosaic_platform: allocate dynamic font %ld bytes failed\n",
            raw_size);
        return ESP_ERR_NO_MEM;
    }
    const size_t read_size = fread(data, 1, (size_t)raw_size, file);
    fclose(file);
    if (read_size != (size_t)raw_size) {
        free(data);
        printf("mosaic_platform: read dynamic font failed: %u/%ld bytes\n",
            (unsigned)read_size, raw_size);
        return ESP_FAIL;
    }

    platform->dynamic_font_data = data;
    platform->dynamic_font_size = (size_t)raw_size;
    printf("mosaic_platform: loaded dynamic font %s (%u bytes)\n",
        path, (unsigned)platform->dynamic_font_size);
    return ESP_OK;
}

static esp_err_t open_assets(mosaic_esp_platform_handle_t platform)
{
    if (platform->assets != NULL) {
        return ESP_OK;
    }
    const mmap_assets_config_t config = {
        .partition_label = MOSAIC_UI_ASSETS_PARTITION,
        .max_files = 0,
        .checksum = 0,
        .flags = {
            .mmap_enable = true,
            .use_fs = false,
            .app_bin_check = false,
            .full_check = true,
            .metadata_check = false,
        },
    };
    esp_err_t err = mmap_assets_new(&config, &platform->assets);
    if (err != ESP_OK) {
        printf("mosaic_platform: open %s failed: %s\n",
            MOSAIC_UI_ASSETS_PARTITION, esp_err_to_name(err));
        return err;
    }
    printf("mosaic_platform: mapped %s (%d assets)\n",
        MOSAIC_UI_ASSETS_PARTITION,
        mmap_assets_get_stored_files(platform->assets));
    return ESP_OK;
}

static esp_err_t find_asset(mosaic_esp_platform_handle_t platform,
    const char* app_name, const char* extension, const void** out_data,
    size_t* out_size)
{
    if (platform == NULL || app_name == NULL || extension == NULL
        || out_data == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(
        open_assets(platform), "mosaic_platform", "open assets");
    char expected[MOSAIC_ASSET_NAME_MAX];
    int written
        = snprintf(expected, sizeof(expected), "%s.%s", app_name, extension);
    if (written < 0 || (size_t)written >= sizeof(expected)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int count = mmap_assets_get_stored_files(platform->assets);
    for (int index = 0; index < count; ++index) {
        const char* name = mmap_assets_get_name(platform->assets, index);
        if (name == NULL || strcmp(name, expected) != 0) {
            continue;
        }
        const int raw_size = mmap_assets_get_size(platform->assets, index);
        const void* data = mmap_assets_get_mem(platform->assets, index);
        if (data == NULL || raw_size <= 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        *out_data = data;
        *out_size = (size_t)raw_size;
        return ESP_OK;
    }
    printf("mosaic_platform: asset %s not found\n", expected);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t open_font_catalog(mosaic_esp_platform_handle_t platform)
{
    if (platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (platform->font_catalog != NULL) {
        return ESP_OK;
    }
    const void* data = NULL;
    size_t size = 0;
    ESP_RETURN_ON_ERROR(
        find_asset(platform, MOSAIC_FONT_CATALOG_ASSET, "gspb", &data, &size),
        "mosaic_platform", "find font catalog");
    gsp_err_t ret = gsp_font_catalog_create(
        data, size, !CONFIG_MOSAIC_UI_DISABLE_BUNDLE_CRC,
        &platform->font_catalog);
    if (ret != GSP_OK) {
        printf("mosaic_platform: open font catalog failed: %d\n", (int)ret);
        return ESP_ERR_INVALID_RESPONSE;
    }
    printf("mosaic_platform: common font catalog live (%u bytes)\n",
        (unsigned)size);
    return ESP_OK;
}

static esp_err_t app_config_for(
    const mosaic_esp_platform_handle_t platform, const void* bundle,
    size_t bundle_size, mosaic_esp_app_t* app, esp_gsp_config_t* out_config)
{
    const mosaic_app_descriptor_t* descriptor = app->package->descriptor;
    esp_gsp_config_t config = ESP_GSP_CONFIG_INIT();
    if (app->package->deployable) {
        esp_gsp_err_t gsp_err = esp_gsp_deployable_bundle_open(bundle,
            bundle_size, !CONFIG_MOSAIC_UI_DISABLE_BUNDLE_CRC,
            &app->deployable);
        if (gsp_err != ESP_GSP_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        gsp_err = esp_gsp_deployable_bundle_make_config(
            app->deployable, &config);
        if (gsp_err != ESP_GSP_OK) {
            esp_gsp_deployable_bundle_close(app->deployable);
            app->deployable = NULL;
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        config.bundle = bundle;
        config.bundle_size = bundle_size;
    }
    if (!app->package->deployable && descriptor->directories != NULL &&
            descriptor->directory_count > 0) {
        config.directories = descriptor->directories;
        config.directory_count = descriptor->directory_count;
    } else if (!app->package->deployable) {
        config.directories = &descriptor->directory;
        config.directory_count = 1;
    }
    config.ttf = platform->dynamic_font_data;
    config.ttf_size = platform->dynamic_font_size;
    config.font_catalog = platform->font_catalog;
    config.disable_swipe = descriptor->disable_swipe;
    config.image_cache_bytes = descriptor->image_cache_bytes;
    config.disable_bundle_crc = CONFIG_MOSAIC_UI_DISABLE_BUNDLE_CRC;

    /* The Registry component deliberately hides its engineering Kconfig
     * surface.  Disable decode workers through the per-instance runtime
     * contract instead of relying on the hidden sdkconfig default. */
    esp_gsp_config_set_result_t result = esp_gsp_config_override_set(
        &config.overrides, ESP_GSP_FIELD_ENABLE_ASYNC_DECODE, 0U);
    if (result != ESP_GSP_CONFIG_SET_OK) {
        printf("mosaic_platform: disable async decode failed: %d\n",
            (int)result);
    }

    const struct {
        esp_gsp_config_field_id_t field_id;
        uint32_t value;
    } overrides[] = {
        {ESP_GSP_FIELD_FREETYPE_CACHE_GLYPHS,
            MOSAIC_DYNAMIC_FONT_CACHE_GLYPHS},
        {ESP_GSP_FIELD_FREETYPE_GLYPH_MAX_PX,
            MOSAIC_DYNAMIC_FONT_GLYPH_MAX_PX},
        {ESP_GSP_FIELD_RENDER_TASK_STACK_SIZE_FREETYPE,
            MOSAIC_FREETYPE_RENDER_TASK_STACK_SIZE},
        {ESP_GSP_FIELD_RENDER_TASK_STACK_PSRAM, 1U},
        {ESP_GSP_FIELD_COMPONENT_INSTANCES, descriptor->instance_slots},
        {ESP_GSP_FIELD_DEFAULT_DYNAMIC_IMAGE_SLOTS,
            descriptor->dynamic_image_slots},
    };
    for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); ++i) {
        if (overrides[i].value == 0U) {
            continue;
        }
        result = esp_gsp_config_override_set(&config.overrides,
            overrides[i].field_id, overrides[i].value);
        if (result != ESP_GSP_CONFIG_SET_OK) {
            printf("mosaic_platform: GSP override %u failed: %d\n",
                (unsigned)overrides[i].field_id, (int)result);
        }
    }
    *out_config = config;
    return ESP_OK;
}

static void on_gsp_event(
    esp_gsp_handle_t ui, const esp_gsp_event_t* event, void* user_ctx)
{
    (void)ui;
    mosaic_esp_app_t* app = user_ctx;
    if (app == NULL || event == NULL
        || app->platform->config.post_event == NULL) {
        return;
    }
    /* CALL means GSP resolved an actual interactive target. This gives all
     * child Apps the same feedback as Hub controls without vibrating when a
     * user merely presses an empty region. */
    if (event->type == ESP_GSP_EVENT_CALL) {
        mosaic_ui_note_screen_activity();
        (void)mosaic_ui_haptic_feedback(25U);
    }
    (void)app->platform->config.post_event(
        app->platform->config.post_event_ctx, app->generation, event);
}

static void on_gsp_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y,
    bool pressed, void* user_ctx)
{
    (void)ui;
    mosaic_esp_app_t* app = user_ctx;
    if (app == NULL || app->platform->config.post_pointer == NULL) {
        return;
    }
    const int32_t dx = x - app->pointer_x;
    const int32_t dy = y - app->pointer_y;
    const bool changed = pressed != app->pointer_pressed;
    const bool moved = dx >= MOSAIC_POINTER_SAMPLE_DELTA
        || dx <= -MOSAIC_POINTER_SAMPLE_DELTA
        || dy >= MOSAIC_POINTER_SAMPLE_DELTA
        || dy <= -MOSAIC_POINTER_SAMPLE_DELTA;
    if (!changed && (!pressed || !moved)) {
        return;
    }
    /* Record input before posting it to the loader. Pointer samples are
     * intentionally allowed to be dropped when that queue is busy, but user
     * activity must never be dropped with them. This covers the initial
     * press, drag/swipe movement, and release. */
    mosaic_ui_note_screen_activity();
    if (app->platform->config.post_pointer(
            app->platform->config.post_pointer_ctx, app->generation, x, y,
            pressed)) {
        app->pointer_x = x;
        app->pointer_y = y;
        app->pointer_pressed = pressed;
    }
}

static void request_hub(void* user_ctx)
{
    mosaic_esp_app_t* app = user_ctx;
    if (app == NULL || app->platform->config.request_app == NULL) {
        return;
    }
    app->platform->config.request_app(
        app->platform->config.request_app_ctx, mosaic_app_root());
}

static void logic_log(void* user_ctx, const char* app_name, const char* message)
{
    (void)user_ctx;
    printf("mosaic_logic[%s]: %s\n", app_name, message);
}

static void prepare_app_ui(esp_gsp_handle_t ui, void* user_ctx)
{
    mosaic_esp_app_t* app = user_ctx;
    if (app == NULL) {
        return;
    }
    app->ui = ui;
    if (app->package->descriptor->custom_shell) {
        return;
    }
    mosaic_app_shell_set_exit_handler(request_hub, app);
    mosaic_app_shell_attach(ui, app->package->descriptor->root_stack_key,
        app->package->descriptor->title,
        !app->package->descriptor->root_header_in_stack);
    app->shell_attached = true;
}

static esp_err_t start_ui(mosaic_esp_platform_handle_t platform,
    mosaic_esp_app_t* app, const void* bundle, size_t bundle_size)
{
    const mosaic_app_descriptor_t* descriptor = app->package->descriptor;
    esp_gsp_config_t app_config;
    ESP_RETURN_ON_ERROR(app_config_for(platform, bundle, bundle_size, app,
                            &app_config),
        "mosaic_platform", "prepare app config");
    if (descriptor == mosaic_app_root()) {
        if (platform->hub_session != NULL || platform->app_ui != NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        if (platform->hub_ui != NULL) {
            app->ui = platform->hub_ui;
            return ESP_OK;
        }
        const esp_gsp_esp_lcd_config_t esp_config = {
            .presenter = platform->config.presenter,
            .render_alignment = platform->config.render_alignment,
            .touch = platform->config.touch,
            /* esp_mosaico wires CST9217 INT to GPIO6. Let the GSP adapter
             * own that IRQ and read frames only after an interrupt instead
             * of issuing an I2C transaction on every UI tick. */
            .touch_input_mode = ESP_GSP_TOUCH_INPUT_INTERRUPT,
            .touch_wake_from_isr = mosaic_ui_screen_wake_from_isr,
        };
        esp_err_t err = esp_gsp_esp_lcd_start(
            &app_config, &esp_config, &platform->hub_ui);
        if (err == ESP_OK) {
            app->ui = platform->hub_ui;
        }
        return err;
    }
    if (platform->hub_ui == NULL || platform->app_ui != NULL
        || platform->hub_session != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(
        esp_gsp_esp_lcd_suspend(platform->hub_ui, &platform->hub_session),
        "mosaic_platform", "suspend hub");
    esp_err_t err = esp_gsp_esp_lcd_start_on_session_prepared(
        platform->hub_session, &app_config, prepare_app_ui, app,
        &platform->app_ui);
    if (err != ESP_OK) {
        platform->hub_session = NULL;
        app->ui = NULL;
        app->shell_attached = false;
        return err;
    }
    app->ui = platform->app_ui;
    return ESP_OK;
}

static esp_err_t recover_failed_open(
    mosaic_esp_platform_handle_t platform, mosaic_esp_app_t* app)
{
    if (app->package->descriptor == mosaic_app_root()) {
        return ESP_OK;
    }
    if (platform->hub_session != NULL && platform->app_ui != NULL) {
        esp_gsp_handle_t hub = NULL;
        esp_err_t err = esp_gsp_esp_lcd_resume(
            platform->hub_session, platform->app_ui, &hub);
        if (err != ESP_OK) {
            return err;
        }
        platform->hub_ui = hub;
        platform->hub_session = NULL;
        platform->app_ui = NULL;
    }
    return ESP_OK;
}

static esp_err_t platform_open_app(void* ctx,
    const mosaic_app_descriptor_t* descriptor,
    mosaic_platform_ui_event_cb_t event_cb, void* event_ctx,
    mosaic_platform_app_handle_t* ret_app)
{
    mosaic_esp_platform_handle_t platform = ctx;
    if (platform == NULL || descriptor == NULL || event_cb == NULL
        || ret_app == NULL || platform->active != NULL
        || platform->present_pause != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_app = NULL;
    const mosaic_app_package_t* package
        = mosaic_app_package_for_descriptor(descriptor);
    if (package == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const void* bundle = NULL;
    size_t bundle_size = 0;
    ESP_RETURN_ON_ERROR(
        find_asset(platform, descriptor->name, "gspb", &bundle,
            &bundle_size),
        "mosaic_platform", "find bundle");

    mosaic_esp_app_t* app = calloc(1, sizeof(*app));
    if (app == NULL) {
        return ESP_ERR_NO_MEM;
    }
    app->platform = platform;
    app->package = package;
    app->event_cb = event_cb;
    app->event_ctx = event_ctx;
    app->generation = ++platform->next_generation;
    if (app->generation == 0) {
        app->generation = ++platform->next_generation;
    }
    int64_t start_us = esp_timer_get_time();
    esp_err_t err = start_ui(platform, app, bundle, bundle_size);
    if (err != ESP_OK) {
        free(app);
        return err;
    }
    err = esp_gsp_on_event(app->ui, on_gsp_event, app);
    if (err != ESP_OK) {
        goto fail;
    }
    const void* program = NULL;
    size_t program_size = 0;
    if (package->logic_entry != NULL) {
        err = find_asset(
            platform, descriptor->name, "lua", &program, &program_size);
        if (err != ESP_OK) {
            goto fail;
        }
    }
    const mosaic_logic_config_t logic_config = {
        .ui = app->ui,
        .package = package,
        .program = program,
        .program_size = program_size,
        .log = logic_log,
    };
    err = mosaic_logic_create(&logic_config, &app->logic);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_gsp_set_pointer_observer(
        app->ui, on_gsp_pointer, app);
    if (err != ESP_OK) {
        goto fail;
    }
    app->pointer_observer_attached = true;
    platform->active = app;
    *ret_app = app;
    printf("mosaic_platform: %s open in %lld ms\n", descriptor->name,
        (long long)((esp_timer_get_time() - start_us) / 1000));
    return ESP_OK;

fail:
    if (app->pointer_observer_attached) {
        (void)esp_gsp_set_pointer_observer(app->ui, NULL, NULL);
    }
    (void)esp_gsp_on_event(app->ui, NULL, NULL);
    mosaic_logic_delete(app->logic);
    if (app->shell_attached) {
        mosaic_app_shell_detach(app->ui);
        app->shell_attached = false;
    }
    ESP_ERROR_CHECK(recover_failed_open(platform, app));
    esp_gsp_deployable_bundle_close(app->deployable);
    free(app);
    return err;
}

static esp_err_t platform_close_app(
    void* ctx, mosaic_platform_app_handle_t handle)
{
    mosaic_esp_platform_handle_t platform = ctx;
    mosaic_esp_app_t* app = handle;
    if (platform == NULL || app == NULL || platform->active != app) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app->pointer_observer_attached) {
        (void)esp_gsp_set_pointer_observer(app->ui, NULL, NULL);
        app->pointer_observer_attached = false;
    }
    if (app->shell_attached) {
        mosaic_app_shell_detach(app->ui);
        app->shell_attached = false;
    }
    if (app->package->descriptor != mosaic_app_root()) {
        esp_gsp_handle_t hub = NULL;
        esp_err_t err = esp_gsp_esp_lcd_resume(
            platform->hub_session, platform->app_ui, &hub);
        if (err != ESP_OK) {
            return err;
        }
        platform->hub_ui = hub;
        platform->hub_session = NULL;
        platform->app_ui = NULL;
    } else {
        (void)esp_gsp_on_event(app->ui, NULL, NULL);
    }
    mosaic_logic_delete(app->logic);
    esp_gsp_deployable_bundle_close(app->deployable);
    platform->active = NULL;
    free(app);
    return ESP_OK;
}

static esp_err_t platform_replace_app(void* ctx,
    mosaic_platform_app_handle_t current,
    const mosaic_app_descriptor_t* descriptor,
    mosaic_platform_ui_event_cb_t event_cb, void* event_ctx,
    mosaic_platform_app_handle_t* ret_app)
{
    mosaic_esp_platform_handle_t platform = ctx;
    mosaic_esp_app_t* old_app = current;
    if (platform == NULL || old_app == NULL || descriptor == NULL ||
            event_cb == NULL || ret_app == NULL || platform->active != old_app ||
            old_app->package->descriptor == mosaic_app_root() ||
            descriptor == mosaic_app_root() || platform->hub_session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_app = NULL;
    const mosaic_app_package_t* package =
        mosaic_app_package_for_descriptor(descriptor);
    const void* bundle = NULL;
    size_t bundle_size = 0;
    if (package == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(find_asset(platform, descriptor->name, "gspb", &bundle,
                            &bundle_size),
        "mosaic_platform", "find replacement bundle");
    mosaic_esp_app_t* next_app = calloc(1, sizeof(*next_app));
    if (next_app == NULL) {
        return ESP_ERR_NO_MEM;
    }
    next_app->platform = platform;
    next_app->package = package;
    next_app->event_cb = event_cb;
    next_app->event_ctx = event_ctx;
    next_app->generation = ++platform->next_generation;
    if (next_app->generation == 0) {
        next_app->generation = ++platform->next_generation;
    }
    if (old_app->pointer_observer_attached) {
        (void)esp_gsp_set_pointer_observer(old_app->ui, NULL, NULL);
    }
    if (old_app->shell_attached) {
        mosaic_app_shell_detach(old_app->ui);
    }
    mosaic_logic_delete(old_app->logic);
    platform->active = NULL;
    platform->app_ui = NULL;
    esp_gsp_config_t app_config;
    esp_err_t err = app_config_for(platform, bundle, bundle_size, next_app,
        &app_config);
    if (err != ESP_OK) {
        free(next_app);
        return err;
    }
    err = esp_gsp_esp_lcd_replace_on_session_prepared(
        platform->hub_session, old_app->ui, &app_config, prepare_app_ui,
        next_app, &next_app->ui);
    esp_gsp_deployable_bundle_close(old_app->deployable);
    free(old_app);
    if (err != ESP_OK) {
        platform->hub_ui = NULL;
        platform->hub_session = NULL;
        esp_gsp_deployable_bundle_close(next_app->deployable);
        free(next_app);
        return err;
    }
    platform->app_ui = next_app->ui;
    err = esp_gsp_on_event(next_app->ui, on_gsp_event, next_app);
    const void* program = NULL;
    size_t program_size = 0;
    if (err == ESP_OK && package->logic_entry != NULL) {
        err = find_asset(platform, descriptor->name, "lua", &program,
            &program_size);
    }
    if (err == ESP_OK) {
        const mosaic_logic_config_t logic_config = {
            .ui = next_app->ui,
            .package = package,
            .program = program,
            .program_size = program_size,
            .log = logic_log,
        };
        err = mosaic_logic_create(&logic_config, &next_app->logic);
    }
    if (err == ESP_OK) {
        err = esp_gsp_set_pointer_observer(
            next_app->ui, on_gsp_pointer, next_app);
        next_app->pointer_observer_attached = err == ESP_OK;
    }
    if (err != ESP_OK) {
        esp_gsp_handle_t hub = NULL;
        if (next_app->pointer_observer_attached) {
            (void)esp_gsp_set_pointer_observer(next_app->ui, NULL, NULL);
        }
        (void)esp_gsp_on_event(next_app->ui, NULL, NULL);
        mosaic_logic_delete(next_app->logic);
        if (next_app->shell_attached) {
            mosaic_app_shell_detach(next_app->ui);
        }
        (void)esp_gsp_esp_lcd_resume(
            platform->hub_session, next_app->ui, &hub);
        platform->hub_ui = hub;
        platform->hub_session = NULL;
        platform->app_ui = NULL;
        esp_gsp_deployable_bundle_close(next_app->deployable);
        free(next_app);
        return err;
    }
    platform->active = next_app;
    *ret_app = next_app;
    return ESP_OK;
}

static esp_err_t platform_step_app(
    void* ctx, mosaic_platform_app_handle_t handle, int64_t now_us)
{
    (void)ctx;
    mosaic_esp_app_t* app = handle;
    return app != NULL ? mosaic_logic_step(app->logic, now_us)
                       : ESP_ERR_INVALID_ARG;
}

static esp_err_t platform_dispatch_event(
    void* ctx, mosaic_platform_app_handle_t handle, const mosaic_event_t* event)
{
    (void)ctx;
    mosaic_esp_app_t* app = handle;
    if (app == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (event->type == MOSAIC_EVENT_START && !app->shell_attached
        && app->package->descriptor != mosaic_app_root()
        && !app->package->descriptor->custom_shell) {
        mosaic_app_shell_set_exit_handler(request_hub, app);
        mosaic_app_shell_attach(app->ui,
            app->package->descriptor->root_stack_key,
            app->package->descriptor->title,
            !app->package->descriptor->root_header_in_stack);
        app->shell_attached = true;
    }
    if (event->type == MOSAIC_EVENT_START && app->shell_attached) {
        /* prepare_app_ui runs before the platform pointer observer is
         * installed. Re-arm now so the Shell remains the final top-level
         * owner of the bottom exit band on both host and ESP touch paths. */
        mosaic_app_shell_rearm(app->ui);
    }
    esp_err_t err = mosaic_logic_dispatch(app->logic, event);
    if (err == ESP_OK && app->package->descriptor->on_event != NULL) {
        /* Match host dispatch so native App business callbacks run on ESP too. */
        app->package->descriptor->on_event(app->ui, event);
    }
    if (event->type == MOSAIC_EVENT_STOP && app->shell_attached) {
        mosaic_app_shell_detach(app->ui);
        app->shell_attached = false;
    } else if (event->type == MOSAIC_EVENT_STOP &&
            app->package->descriptor->custom_shell) {
        /* No-op unless a system notice temporarily owned this full-canvas UI. */
        mosaic_app_shell_detach(app->ui);
    }
    if (event->type == MOSAIC_EVENT_STOP && err == ESP_OK) {
        app->stopping = true;
    }
    return err;
}

static esp_err_t platform_feed_pointer(void* ctx,
    mosaic_platform_app_handle_t handle, int32_t x, int32_t y, bool pressed)
{
    (void)ctx;
    (void)handle;
    (void)x;
    (void)y;
    (void)pressed;
    return ESP_ERR_NOT_SUPPORTED;
}

static const mosaic_platform_ops_t s_platform_ops = {
    .open_app = platform_open_app,
    .replace_app = platform_replace_app,
    .close_app = platform_close_app,
    .step_app = platform_step_app,
    .dispatch_event = platform_dispatch_event,
    .feed_pointer = platform_feed_pointer,
};

esp_err_t mosaic_esp_platform_create(const mosaic_esp_platform_config_t* config,
    mosaic_esp_platform_handle_t* ret_platform)
{
    if (config == NULL || config->presenter == NULL
        || config->post_event == NULL || config->post_pointer == NULL
        || config->request_app == NULL || ret_platform == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_platform = NULL;
    mosaic_esp_platform_handle_t platform = calloc(1, sizeof(*platform));
    if (platform == NULL) {
        return ESP_ERR_NO_MEM;
    }
    platform->config = *config;
    esp_err_t err = open_assets(platform);
    if (err != ESP_OK) {
        free(platform);
        return err;
    }
    err = open_font_catalog(platform);
    if (err != ESP_OK) {
        mmap_assets_del(platform->assets);
        free(platform);
        return err;
    }
    err = load_dynamic_font(platform);
    if (err != ESP_OK) {
        gsp_font_catalog_destroy(platform->font_catalog);
        mmap_assets_del(platform->assets);
        free(platform);
        return err;
    }
    *ret_platform = platform;
    return ESP_OK;
}

void mosaic_esp_platform_delete(mosaic_esp_platform_handle_t platform)
{
    if (platform == NULL) {
        return;
    }
    if (platform->active != NULL) {
        esp_err_t err = platform_close_app(platform, platform->active);
        if (err != ESP_OK) {
            printf("mosaic_platform: close during delete failed: %s\n",
                esp_err_to_name(err));
            return;
        }
    }
    if (platform->hub_session != NULL) {
        (void)esp_gsp_esp_lcd_session_destroy(platform->hub_session);
    } else if (platform->hub_ui != NULL) {
        (void)esp_gsp_stop(platform->hub_ui);
    }
    if (platform->font_catalog != NULL) {
        gsp_font_catalog_destroy(platform->font_catalog);
    }
    if (platform->assets != NULL) {
        mmap_assets_del(platform->assets);
    }
    free(platform->dynamic_font_data);
    free(platform);
}

const mosaic_platform_ops_t* mosaic_esp_platform_ops(void)
{
    return &s_platform_ops;
}

bool mosaic_esp_platform_deliver_event(mosaic_esp_platform_handle_t platform,
    uint32_t generation, const esp_gsp_event_t* event)
{
    if (platform == NULL || platform->active == NULL || event == NULL
        || platform->active->stopping
        || platform->active->generation != generation) {
        return false;
    }
    platform->active->event_cb(platform->active->event_ctx, event);
    return true;
}

bool mosaic_esp_platform_deliver_pointer(
    mosaic_esp_platform_handle_t platform, mosaic_runtime_handle_t runtime,
    uint32_t generation, int32_t x, int32_t y, bool pressed)
{
    if (platform == NULL || runtime == NULL || platform->active == NULL
        || platform->active->stopping
        || platform->active->generation != generation) {
        return false;
    }
    return mosaic_runtime_dispatch_pointer(runtime, x, y, pressed) == ESP_OK;
}

esp_gsp_handle_t mosaic_esp_platform_ui(mosaic_esp_platform_handle_t platform)
{
    return platform != NULL && platform->active != NULL && !platform->paused
        ? platform->active->ui
        : NULL;
}

esp_err_t mosaic_esp_platform_quiesce(
    mosaic_esp_platform_handle_t platform, uint32_t timeout_ms)
{
    if (platform == NULL || timeout_ms == 0 || platform->active == NULL
        || platform->present_pause != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The screen-sleep pause already guarantees the presenter is quiescent. */
    if (platform->screen_pause != NULL) {
        platform->present_pause = platform->screen_pause;
        platform->screen_pause = NULL;
        platform->paused = true;
        return ESP_OK;
    }
    esp_err_t err = esp_gsp_esp_lcd_pause(
        platform->active->ui, timeout_ms, &platform->present_pause);
    if (err == ESP_OK) {
        platform->paused = true;
    }
    return err;
}

esp_err_t mosaic_esp_platform_activate(mosaic_esp_platform_handle_t platform,
    struct esp_display_presenter* presenter)
{
    if (platform == NULL || presenter == NULL
        || presenter != platform->config.presenter
        || platform->present_pause == NULL || platform->active == NULL
        || !platform->paused) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_gsp_handle_t resumed = NULL;
    esp_err_t err
        = esp_gsp_esp_lcd_resume_paused(platform->present_pause, &resumed);
    if (err == ESP_OK) {
        platform->present_pause = NULL;
        platform->paused = false;
        platform->active->ui = resumed;
        if (platform->active->package->descriptor == mosaic_app_root()) {
            platform->hub_ui = resumed;
        } else {
            platform->app_ui = resumed;
        }
    }
    return err;
}

esp_err_t mosaic_esp_platform_pause_screen(
    mosaic_esp_platform_handle_t platform, uint32_t timeout_ms)
{
    if (platform == NULL || timeout_ms == 0 || platform->active == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (platform->screen_pause != NULL) {
        return ESP_OK;
    }
    if (platform->paused) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_gsp_esp_lcd_pause(
        platform->active->ui, timeout_ms, &platform->screen_pause);
}

esp_err_t mosaic_esp_platform_resume_screen(
    mosaic_esp_platform_handle_t platform)
{
    if (platform == NULL || platform->active == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (platform->screen_pause == NULL) {
        return ESP_OK;
    }
    esp_gsp_handle_t resumed = NULL;
    esp_err_t err = esp_gsp_esp_lcd_resume_paused(
        platform->screen_pause, &resumed);
    if (err == ESP_OK) {
        platform->screen_pause = NULL;
        platform->active->ui = resumed;
        if (platform->active->package->descriptor == mosaic_app_root()) {
            platform->hub_ui = resumed;
        } else {
            platform->app_ui = resumed;
        }
    }
    return err;
}
