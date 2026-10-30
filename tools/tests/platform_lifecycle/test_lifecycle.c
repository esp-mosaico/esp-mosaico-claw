/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 257,
       ESP_ERR_INVALID_ARG = 258, ESP_ERR_INVALID_STATE = 259,
       ESP_ERR_NOT_FOUND = 261, ESP_ERR_NOT_SUPPORTED = 262,
       ESP_ERR_TIMEOUT = 263, ESP_ERR_NOT_FINISHED = 268,
       MOSAIC_EVENT_START = 0, MOSAIC_EVENT_STOP = 1, MOSAIC_EVENT_TIMER = 6,
       ESP_GSP_TOUCH_INPUT_INTERRUPT = 1, MOSAIC_CAMERA_TRIM_TIMEOUT_MS = 1000 };
#define ESP_GSP_OK 0
#define MOSAIC_GSP_HAS_SESSION_TRIM 1
#define ESP_GSP_ESP_LCD_CONFIG_INIT() {0}
#define ESP_RETURN_ON_ERROR(expr, ...) do { int error = (expr); if (error) return error; } while (0)
typedef struct test_ui { bool alive; } *esp_gsp_handle_t;
typedef struct { esp_gsp_handle_t hub, app; } esp_gsp_esp_lcd_session_t;
#if TEST_SESSION_RESULTS
/* Public API shape for product fault gates; target compilation separately
 * verifies the actual SDK declaration and binary ABI. */
enum { ESP_GSP_ESP_LCD_SESSION_UNCHANGED, ESP_GSP_ESP_LCD_SESSION_APP_RETAINED,
       ESP_GSP_ESP_LCD_SESSION_APP_STARTED, ESP_GSP_ESP_LCD_SESSION_EMPTY,
       ESP_GSP_ESP_LCD_SESSION_HUB_RESUMED };
typedef struct { uint32_t struct_size; int stage, outcome;
                 esp_err_t operation_error, recovery_error;
                 esp_gsp_handle_t app, hub; } esp_gsp_esp_lcd_session_result_t;
#define ESP_GSP_ESP_LCD_SESSION_RESULT_INIT() { .struct_size = sizeof(esp_gsp_esp_lcd_session_result_t) }
#endif
typedef struct { esp_gsp_handle_t ui; } esp_gsp_esp_lcd_pause_t;
typedef void *mmap_assets_handle_t;
typedef void gsp_font_catalog_t;
typedef void *mosaic_logic_handle_t;
typedef void *mosaic_platform_app_handle_t;
typedef struct { unsigned type; } esp_gsp_event_t;
typedef struct { unsigned type; int64_t timestamp_us; } mosaic_event_t;
typedef struct mosaic_app_descriptor {
    const char *name;
    bool custom_shell, root_header_in_stack;
    uint32_t root_stack_key;
    const char *title;
    void (*on_background_tick)(int64_t);
    void (*on_event)(esp_gsp_handle_t, const mosaic_event_t *);
} mosaic_app_descriptor_t;
typedef struct { const mosaic_app_descriptor_t *descriptor;
                 const char *logic_entry; } mosaic_app_package_t;
typedef void (*mosaic_platform_ui_event_cb_t)(void *, const esp_gsp_event_t *);
typedef struct mosaic_esp_platform_t *mosaic_esp_platform_handle_t;
struct esp_display_presenter { int unused; };
typedef struct { struct esp_display_presenter *presenter;
                 void *touch; int render_alignment; } mosaic_esp_platform_config_t;
typedef struct { int unused; } esp_gsp_config_t;
typedef struct { void *presenter, *touch; int render_alignment, touch_input_mode;
                 void (*touch_wake_from_isr)(void *); } esp_gsp_esp_lcd_config_t;
typedef struct { esp_gsp_handle_t ui; const mosaic_app_package_t *package;
                 const void *program; size_t program_size;
                 void (*log)(void *, const char *, const char *); } mosaic_logic_config_t;

#include "platform_types.h"

static const mosaic_app_descriptor_t root = { .name = "mosaic-hub" };
static const mosaic_app_descriptor_t camera = { .name = "camera" };
static const mosaic_app_descriptor_t album = { .name = "album" };
static const mosaic_app_package_t packages[] = {
    { &root, NULL }, { &camera, NULL }, { &album, NULL }
};
static int suspend_error, trim_error, resume_error, start_error, replace_error;
static int event_error, logic_error, destroy_error, stop_error, camera_stop_error;
static int flush_error;
static int pause_error, paused_resume_error;
static unsigned pause_calls;
static unsigned replace_fail_stage;
static unsigned freed_fonts, freed_assets, sdk_destroy_calls, logic_stops;
static unsigned trim_calls, sdk_start_calls, borrowed_owners;

static const mosaic_app_descriptor_t *mosaic_app_root(void) { return &root; }
static const mosaic_app_package_t *mosaic_app_package_for_descriptor(
    const mosaic_app_descriptor_t *descriptor)
{
    for (unsigned i = 0; i < 3; ++i)
        if (packages[i].descriptor == descriptor) return &packages[i];
    return NULL;
}
static const char *esp_err_to_name(int error) { return error ? "failure" : "OK"; }
static esp_gsp_config_t app_config_for(mosaic_esp_platform_handle_t platform,
    const void *bundle, size_t size, const mosaic_app_descriptor_t *descriptor)
{
    assert(platform && bundle && size && descriptor);
    return (esp_gsp_config_t){0};
}
static int find_asset(mosaic_esp_platform_handle_t platform, const char *name,
    const char *extension, const void **data, size_t *size)
{
    (void)platform; (void)name; (void)extension;
    *data = "data"; *size = 4; return ESP_OK;
}
static void mosaic_ui_screen_wake_from_isr(void *ctx) { (void)ctx; }
static int64_t esp_timer_get_time(void) { return 1; }
static void callback(void *ctx, const esp_gsp_event_t *event) { (void)ctx; (void)event; }
static void on_gsp_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *ctx)
{ (void)ui; (void)event; assert(((mosaic_esp_app_t *)ctx)->platform); }
static void on_gsp_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{ (void)ui; (void)x; (void)y; (void)pressed; assert(ctx); }
static void logic_log(void *ctx, const char *name, const char *text)
{ (void)ctx; (void)name; (void)text; }
static esp_gsp_handle_t new_ui(void)
{ esp_gsp_handle_t ui = malloc(sizeof(*ui)); assert(ui); ui->alive = true; return ui; }
static void retire_ui(esp_gsp_handle_t ui) { assert(ui && ui->alive); free(ui); }
static int esp_gsp_esp_lcd_start(const esp_gsp_config_t *config,
    const esp_gsp_esp_lcd_config_t *lcd, esp_gsp_handle_t *out)
{ assert(config && lcd); *out = new_ui(); return ESP_OK; }
static int esp_gsp_esp_lcd_suspend(esp_gsp_handle_t hub, esp_gsp_esp_lcd_session_t **out)
{
    assert(hub && hub->alive); *out = NULL;
    if (suspend_error && suspend_error != ESP_ERR_NOT_FINISHED) return suspend_error;
    *out = calloc(1, sizeof(**out)); assert(*out); (*out)->hub = hub;
    return suspend_error;
}
static int esp_gsp_esp_lcd_trim_session(esp_gsp_esp_lcd_session_t *session,
    unsigned timeout, size_t *released)
{
    assert(session && !session->app && timeout); ++trim_calls;
    *released = trim_error ? 0 : 814080; return trim_error;
}
static void prepare_app_ui(esp_gsp_handle_t ui, void *ctx)
{ mosaic_esp_app_t *app = ctx; app->ui = ui; app->shell_attached = true; }
static int esp_gsp_esp_lcd_start_on_session_prepared(esp_gsp_esp_lcd_session_t *session,
    const esp_gsp_config_t *config, void (*prepare)(esp_gsp_handle_t, void *),
    void *ctx, esp_gsp_handle_t *out)
{
    assert(session && !session->app && config); ++sdk_start_calls; *out = NULL;
    if (start_error) {
        if (start_error != ESP_ERR_NOT_FINISHED) free(session);
        return start_error;
    }
    session->app = new_ui(); *out = session->app; prepare(*out, ctx); return ESP_OK;
}
static int esp_gsp_esp_lcd_replace_on_session_prepared(esp_gsp_esp_lcd_session_t *session,
    esp_gsp_handle_t old, const esp_gsp_config_t *config,
    void (*prepare)(esp_gsp_handle_t, void *), void *ctx, esp_gsp_handle_t *out)
{
    assert(session && old == session->app); *out = NULL;
    if (replace_error) return replace_error;
    retire_ui(old); session->app = NULL;
    return esp_gsp_esp_lcd_start_on_session_prepared(session, config, prepare, ctx, out);
}
static int esp_gsp_esp_lcd_resume(esp_gsp_esp_lcd_session_t *session,
    esp_gsp_handle_t app, esp_gsp_handle_t *out)
{
    assert(session && app == session->app); *out = NULL;
    if (resume_error && resume_error != ESP_ERR_NOT_FINISHED) return resume_error;
    if (app) { retire_ui(app); session->app = NULL; }
    if (resume_error) return resume_error;
    *out = session->hub; free(session); return ESP_OK;
}
#if TEST_SESSION_RESULTS
static int esp_gsp_esp_lcd_start_on_session_prepared_ex(esp_gsp_esp_lcd_session_t **session,
    const esp_gsp_config_t *config, void (*prepare)(esp_gsp_handle_t, void *),
    void *ctx, esp_gsp_handle_t *out, esp_gsp_esp_lcd_session_result_t *result)
{
    assert(result->struct_size == sizeof(*result));
    esp_gsp_handle_t hub = (*session)->hub;
    int err = esp_gsp_esp_lcd_start_on_session_prepared(*session, config, prepare, ctx, out);
    result->operation_error = err;
    if (err == ESP_OK) {
        result->outcome = ESP_GSP_ESP_LCD_SESSION_APP_STARTED; result->app = *out;
    } else if (err == ESP_ERR_NOT_FINISHED) {
        result->outcome = ESP_GSP_ESP_LCD_SESSION_EMPTY;
    } else {
        *session = NULL; result->hub = hub;
        result->outcome = ESP_GSP_ESP_LCD_SESSION_HUB_RESUMED;
    }
    return err;
}
static int esp_gsp_esp_lcd_resume_ex(esp_gsp_esp_lcd_session_t **session,
    esp_gsp_handle_t app, esp_gsp_handle_t *hub, esp_gsp_esp_lcd_session_result_t *result)
{
    int err = esp_gsp_esp_lcd_resume(*session, app, hub);
    result->operation_error = err;
    if (err == ESP_OK) {
        *session = NULL; result->hub = *hub;
        result->outcome = ESP_GSP_ESP_LCD_SESSION_HUB_RESUMED;
    } else if (err == ESP_ERR_NOT_FINISHED) {
        result->outcome = ESP_GSP_ESP_LCD_SESSION_EMPTY;
    } else {
        result->outcome = app ? ESP_GSP_ESP_LCD_SESSION_APP_RETAINED : ESP_GSP_ESP_LCD_SESSION_UNCHANGED;
        result->app = app;
    }
    return err;
}
static int esp_gsp_esp_lcd_replace_on_session_prepared_ex(esp_gsp_esp_lcd_session_t **session,
    esp_gsp_handle_t current, const esp_gsp_config_t *config,
    void (*prepare)(esp_gsp_handle_t, void *), void *ctx, esp_gsp_handle_t *out,
    esp_gsp_esp_lcd_session_result_t *result)
{
    *out = NULL;
    if (replace_fail_stage >= 2 && replace_fail_stage <= 3) {
        retire_ui(current); (*session)->app = NULL;
        /* Preparation may have attached native shell state before failure. */
        esp_gsp_handle_t failed = new_ui(); prepare(failed, ctx); retire_ui(failed);
        if (replace_fail_stage == 2) {
            result->outcome = ESP_GSP_ESP_LCD_SESSION_HUB_RESUMED;
            result->hub = (*session)->hub; free(*session); *session = NULL;
        } else result->outcome = ESP_GSP_ESP_LCD_SESSION_EMPTY;
        return ESP_ERR_TIMEOUT;
    }
    if (replace_fail_stage == 4) {
        result->outcome = ESP_GSP_ESP_LCD_SESSION_UNCHANGED;
        return ESP_ERR_TIMEOUT;
    }
    int err = esp_gsp_esp_lcd_replace_on_session_prepared(*session, current,
        config, prepare, ctx, out);
    result->outcome = err ? ESP_GSP_ESP_LCD_SESSION_APP_RETAINED : ESP_GSP_ESP_LCD_SESSION_APP_STARTED;
    result->app = err ? current : *out;
    return err;
}
#endif
static int esp_gsp_esp_lcd_session_destroy(esp_gsp_esp_lcd_session_t *session)
{
    assert(session && !session->app); ++sdk_destroy_calls;
    if (destroy_error) return destroy_error;
    retire_ui(session->hub); free(session); return ESP_OK;
}
static int esp_gsp_stop(esp_gsp_handle_t ui)
{ assert(ui && ui->alive); ++sdk_destroy_calls; if (stop_error) return stop_error; retire_ui(ui); return ESP_OK; }
static int esp_gsp_esp_lcd_pause(esp_gsp_handle_t ui, unsigned timeout,
    esp_gsp_esp_lcd_pause_t **out)
{
    assert(ui && ui->alive && timeout); ++pause_calls; *out = NULL;
    if (pause_error == ESP_OK || pause_error == ESP_ERR_NOT_FINISHED) {
        *out = malloc(sizeof(**out)); assert(*out); (*out)->ui = ui;
    }
    return pause_error;
}
static int esp_gsp_esp_lcd_resume_paused(esp_gsp_esp_lcd_pause_t *pause,
    esp_gsp_handle_t *out)
{
    assert(pause && pause->ui && pause->ui->alive); *out = NULL;
    if (paused_resume_error) return paused_resume_error;
    *out = pause->ui; free(pause); return ESP_OK;
}
static int esp_gsp_esp_lcd_trim_paused(esp_gsp_esp_lcd_pause_t *pause,
    unsigned timeout, size_t *released)
{
    assert(pause && pause->ui && pause->ui->alive && timeout); ++trim_calls;
    *released = trim_error ? 0 : 814080; return trim_error;
}
static int esp_gsp_on_event(esp_gsp_handle_t ui,
    void (*cb)(esp_gsp_handle_t, const esp_gsp_event_t *, void *), void *ctx)
{ assert(ui && ui->alive); (void)cb; (void)ctx; return event_error; }
static int esp_gsp_set_pointer_observer(esp_gsp_handle_t ui,
    void (*cb)(esp_gsp_handle_t, int32_t, int32_t, bool, void *), void *ctx)
{ assert(ui && ui->alive); (void)cb; (void)ctx; return ESP_OK; }
static int esp_gsp_flush(esp_gsp_handle_t ui, unsigned timeout)
{ assert(ui && ui->alive && timeout); return flush_error; }
static void mosaic_app_shell_detach(esp_gsp_handle_t ui) { assert(ui && ui->alive); }
static void mosaic_app_shell_forget(esp_gsp_handle_t ui) { (void)ui; }
static void mosaic_app_shell_rearm(esp_gsp_handle_t ui) { assert(ui && ui->alive); }
static void request_hub(void *ctx) { (void)ctx; }
static void mosaic_app_shell_set_exit_handler(void (*handler)(void *), void *ctx)
{ (void)handler; (void)ctx; }
static void mosaic_app_shell_attach(esp_gsp_handle_t ui, unsigned key, const char *title, bool header)
{ assert(ui && ui->alive); (void)key; (void)title; (void)header; }
static int mosaic_logic_create(const mosaic_logic_config_t *config, void **out)
{
    assert(config->ui && config->ui->alive); *out = NULL;
    if (logic_error) return logic_error;
    *out = malloc(1); assert(*out); return ESP_OK;
}
static void mosaic_logic_delete(void *logic) { free(logic); }
static int mosaic_logic_dispatch(void *logic, const mosaic_event_t *event)
{ assert(logic); if (event->type == MOSAIC_EVENT_STOP) ++logic_stops; return ESP_OK; }
static bool mosaic_ui_screen_asleep(void) { return false; }
static void mosaic_ui_probe_cancel(esp_gsp_handle_t ui) { assert(ui && ui->alive); }
static int mosaic_camera_stop(esp_gsp_handle_t ui) { assert(ui && ui->alive); return camera_stop_error; }
static void gsp_font_catalog_destroy(void *font) { ++freed_fonts; free(font); --borrowed_owners; }
static void mmap_assets_del(void *assets) { ++freed_assets; free(assets); --borrowed_owners; }
esp_err_t mosaic_esp_platform_activate(mosaic_esp_platform_handle_t,
    struct esp_display_presenter *);
esp_err_t mosaic_esp_platform_resume_screen(mosaic_esp_platform_handle_t);
static esp_err_t platform_close_app(void *, mosaic_platform_app_handle_t);

#include "platform_under_test.h"

static mosaic_esp_platform_handle_t create(void)
{
    mosaic_esp_platform_handle_t p = calloc(1, sizeof(*p)); assert(p);
    static struct esp_display_presenter presenter;
    p->config.presenter = &presenter;
    p->hub_ui = new_ui(); p->font_catalog = malloc(1); p->assets = malloc(1);
    p->dynamic_font_data = malloc(1); borrowed_owners += 2;
    return p;
}
static mosaic_esp_app_t *open_app(mosaic_esp_platform_handle_t p, const mosaic_app_descriptor_t *d)
{
    mosaic_platform_app_handle_t app = NULL;
    assert(platform_open_app(p, d, callback, p, &app) == ESP_OK && app);
    return app;
}
static void reset(void)
{
    assert(borrowed_owners == 0);
    suspend_error = trim_error = resume_error = start_error = replace_error = 0;
    event_error = logic_error = destroy_error = stop_error = camera_stop_error = 0;
    flush_error = 0;
    pause_error = paused_resume_error = 0; pause_calls = 0;
    replace_fail_stage = 0;
    trim_calls = sdk_start_calls = sdk_destroy_calls = logic_stops = 0;
    freed_fonts = freed_assets = 0;
}
static void test_pre_start_recovery(void)
{
    for (unsigned mode = 0; mode < 4; ++mode) {
        reset(); mosaic_esp_platform_handle_t p = create();
        if (mode == 0) suspend_error = ESP_ERR_NOT_FINISHED;
        else trim_error = mode == 1 ? ESP_ERR_TIMEOUT : ESP_ERR_NOT_FINISHED;
        if (mode >= 2) resume_error = ESP_ERR_NOT_FINISHED;
        void *app = NULL;
        assert(platform_open_app(p, &camera, callback, p, &app) != ESP_OK && !app);
        assert(sdk_start_calls == 0 && borrowed_owners == 2);
        if (mode != 1) {
            assert(p->hub_session && !p->app_ui);
            assert(platform_open_app(p, &root, callback, p, &app) == ESP_ERR_NOT_FINISHED);
        } else assert(!p->hub_session);
        resume_error = 0;
        assert(platform_recover(p) == ESP_OK && !p->hub_session);
        assert(mosaic_esp_platform_delete(p) == ESP_OK);
    }
    puts("PASS suspend/trim failures retain empty Sessions through recovery and block premature startup");
}
static void test_start_and_close_failures(void)
{
    reset(); mosaic_esp_platform_handle_t p = create(); void *app = NULL;
    start_error = ESP_ERR_NO_MEM;
    assert(platform_open_app(p, &camera, callback, p, &app) == ESP_ERR_NO_MEM);
    assert(!p->hub_session && !p->app_ui && !app);
    start_error = ESP_ERR_NOT_FINISHED;
    assert(platform_open_app(p, &camera, callback, p, &app) == ESP_ERR_NOT_FINISHED);
    assert(p->hub_session && !app);
    start_error = 0;
    assert(platform_recover(p) == ESP_OK);
    event_error = ESP_FAIL; resume_error = ESP_ERR_NOT_FINISHED;
    assert(platform_open_app(p, &camera, callback, p, &app) == ESP_FAIL && app);
    assert(p->active == app && !p->app_ui && !p->active->ui && borrowed_owners == 2);
    event_error = 0; resume_error = 0;
    assert(platform_close_app(p, app) == ESP_OK && !p->active);
    app = open_app(p, &camera);
    resume_error = ESP_ERR_TIMEOUT;
    assert(platform_close_app(p, app) == ESP_ERR_TIMEOUT && p->app_ui && p->active == app);
    resume_error = ESP_ERR_NOT_FINISHED;
    assert(platform_close_app(p, app) == ESP_ERR_NOT_FINISHED && !p->app_ui);
    resume_error = 0;
    assert(platform_close_app(p, app) == ESP_OK && !p->active);
    assert(mosaic_esp_platform_delete(p) == ESP_OK);
    puts("PASS startup rollback and close retries preserve the correct App/Session owner");
}
static void test_delete_and_camera_barriers(void)
{
    reset(); mosaic_esp_platform_handle_t p = create();
    mosaic_esp_app_t *app = open_app(p, &camera);
    mosaic_event_t stop = { .type = MOSAIC_EVENT_STOP };
    camera_stop_error = ESP_ERR_TIMEOUT;
    assert(platform_dispatch_event(p, app, &stop) == ESP_ERR_TIMEOUT && logic_stops == 0);
    camera_stop_error = 0;
    assert(platform_dispatch_event(p, app, &stop) == ESP_OK && logic_stops == 1);
    assert(platform_close_app(p, app) == ESP_OK);
    stop_error = ESP_FAIL;
    assert(mosaic_esp_platform_delete(p) == ESP_FAIL && borrowed_owners == 2);
    assert(p->destroying && p->hub_ui && freed_assets == 0 && freed_fonts == 0);
    stop_error = 0;
    assert(mosaic_esp_platform_delete(p) == ESP_OK);
    reset(); p = create();
    assert(esp_gsp_esp_lcd_suspend(p->hub_ui, &p->hub_session) == ESP_OK);
    destroy_error = ESP_FAIL;
    assert(mosaic_esp_platform_delete(p) == ESP_FAIL && borrowed_owners == 2);
    assert(p->destroying && p->hub_session && platform_recover(p) == ESP_ERR_INVALID_STATE);
    destroy_error = 0;
    assert(mosaic_esp_platform_delete(p) == ESP_OK && sdk_destroy_calls == 2);
    puts("PASS Camera retirement blocks UI destruction; failed SDK destruction retains all borrowed resources");
}
static void test_replace_success_and_legacy_ambiguity(void)
{
    reset(); mosaic_esp_platform_handle_t p = create();
    void *app = open_app(p, &camera); void *next = NULL;
    assert(platform_replace_app(p, app, &album, callback, p, &next) == ESP_OK);
    assert(next && next == p->active && trim_calls == 1);
    replace_error = ESP_ERR_TIMEOUT;
    assert(platform_replace_app(p, next, &camera, callback, p, &app) == ESP_ERR_TIMEOUT);
#if TEST_SESSION_RESULTS
    assert(app == p->active && !p->lifecycle_uncertain && !p->uncertain_next);
#else
    assert(app == p->active && p->lifecycle_uncertain && p->uncertain_next);
    assert(platform_close_app(p, app) == ESP_ERR_NOT_SUPPORTED);
    assert(mosaic_esp_platform_pause_screen(p, 1000) == ESP_ERR_INVALID_STATE);
    assert(mosaic_esp_platform_quiesce(p, 1000) == ESP_ERR_INVALID_STATE);
    assert(mosaic_esp_platform_delete(p) == ESP_ERR_NOT_SUPPORTED && borrowed_owners == 2);
    /* The mock knows this was a pre-suspend failure. Only the test may use
     * that knowledge to release its fixture; the product never guesses. */
    free(p->uncertain_next); p->uncertain_next = NULL; p->lifecycle_uncertain = false;
#endif
    replace_error = 0;
    assert(platform_close_app(p, app) == ESP_OK);
    assert(mosaic_esp_platform_delete(p) == ESP_OK);
    puts("PASS successful replacement remains direct; ambiguous legacy failures never probe or free unknown owners");
}
#if TEST_SESSION_RESULTS
static void test_explicit_replacement_results(void)
{
    for (unsigned stage = 1; stage <= 4; ++stage) {
        reset(); mosaic_esp_platform_handle_t p = create();
        void *app = open_app(p, &camera); void *next = NULL;
        replace_error = ESP_ERR_TIMEOUT; replace_fail_stage = stage;
        assert(platform_replace_app(p, app, &album, callback, p, &next) == ESP_ERR_TIMEOUT);
        assert(!p->lifecycle_uncertain);
        if (stage == 1 || stage == 4) {
            assert(next == app && p->active == app && p->app_ui && p->hub_session);
            assert(platform_close_app(p, app) == ESP_OK);
        } else {
            assert(!next && !p->active && !p->app_ui);
            assert((p->hub_session == NULL) == (stage == 2));
            assert(platform_recover(p) == ESP_OK && !p->hub_session);
        }
        assert(mosaic_esp_platform_delete(p) == ESP_OK);
    }
    puts("PASS identical SDK errors select retained, empty, consumed and unchanged lifetimes through public results");
}
#endif
static void test_retained_hub_callback_barrier(void)
{
    reset(); mosaic_esp_platform_handle_t p = create();
    mosaic_esp_app_t *app = open_app(p, &root);
    event_error = ESP_FAIL;
    assert(platform_close_app(p, app) == ESP_FAIL && p->active == app);
    event_error = 0; flush_error = ESP_FAIL;
    assert(platform_close_app(p, app) == ESP_ERR_NOT_FINISHED && p->active == app);
    assert(app->logic && borrowed_owners == 2);
    flush_error = 0;
    assert(platform_close_app(p, app) == ESP_OK && !p->active && p->hub_ui);
    assert(mosaic_esp_platform_delete(p) == ESP_OK);
    puts("PASS retained Hub callback contexts outlive failed unregister and flush fences");
}
static void test_partial_pause_cannot_commit_or_handoff(void)
{
    reset(); mosaic_esp_platform_handle_t p = create();
    open_app(p, &root);
    pause_error = ESP_ERR_NOT_FINISHED;
    assert(mosaic_esp_platform_pause_screen(p, 1000) == ESP_ERR_NOT_FINISHED);
    assert(p->screen_pause && !p->screen_pause_complete);
    assert(mosaic_esp_platform_pause_screen(p, 1000) == ESP_ERR_NOT_FINISHED && pause_calls == 1);
    assert(mosaic_esp_platform_quiesce(p, 1000) == ESP_ERR_NOT_FINISHED && !p->present_pause);
    paused_resume_error = ESP_FAIL;
    assert(mosaic_esp_platform_resume_screen(p) == ESP_FAIL && p->screen_pause);
    paused_resume_error = 0;
    assert(mosaic_esp_platform_resume_screen(p) == ESP_OK && !p->screen_pause);
    pause_error = 0;
    assert(mosaic_esp_platform_pause_screen(p, 1000) == ESP_OK && p->screen_pause_complete);
    assert(mosaic_esp_platform_quiesce(p, 1000) == ESP_OK && p->present_pause && !p->screen_pause);
    assert(mosaic_esp_platform_activate(p, p->config.presenter) == ESP_OK && !p->paused);
    pause_error = ESP_ERR_NOT_FINISHED;
    assert(mosaic_esp_platform_quiesce(p, 1000) == ESP_ERR_NOT_FINISHED && p->present_pause && !p->paused);
    assert(mosaic_esp_platform_activate(p, p->config.presenter) == ESP_OK && !p->present_pause);
    assert(mosaic_esp_platform_delete(p) == ESP_OK);
    puts("PASS incomplete pauses retain wake recovery and cannot become successful screen-off or handoff");
}
static void test_camera_replacement_cache_gate(void)
{
    for (unsigned fault = 0; fault <= 4; ++fault) {
        reset(); mosaic_esp_platform_handle_t p = create();
        void *old = open_app(p, &album); void *next = NULL;
        if (fault == 1) pause_error = ESP_ERR_NOT_FINISHED;
        if (fault == 2 || fault == 3) trim_error = ESP_ERR_TIMEOUT;
        if (fault == 3) paused_resume_error = ESP_FAIL;
        if (fault == 4) pause_error = ESP_ERR_TIMEOUT;
        int error = platform_replace_app(p, old, &camera, callback, p, &next);
        if (fault == 0) {
            assert(error == ESP_OK && next == p->active && !p->active->prepare_pause);
            assert(trim_calls == 1);
            assert(platform_close_app(p, next) == ESP_OK);
        } else if (fault == 3) {
            assert(error != ESP_OK && next == p->active && p->active->prepare_pause);
            paused_resume_error = 0;
            assert(platform_close_app(p, next) == ESP_OK);
        } else {
            assert(error != ESP_OK && !next && !p->active && !p->hub_session);
        }
        assert(mosaic_esp_platform_delete(p) == ESP_OK);
    }
    puts("PASS direct Camera replacement trims shared snapshots before START and retains failed pause rollback");
}
int main(void)
{
    test_pre_start_recovery();
    test_start_and_close_failures();
    test_delete_and_camera_barriers();
    test_replace_success_and_legacy_ambiguity();
    test_retained_hub_callback_barrier();
    test_partial_pause_cannot_commit_or_handoff();
    test_camera_replacement_cache_gate();
#if TEST_SESSION_RESULTS
    test_explicit_replacement_results();
#endif
    reset();
    return 0;
}
