/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "mosaic_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { ROOT_ID = 0, MUSIC_ID = 5, BLUETOOTH_ID = 12, ALBUM_ID = 2 };
enum { OPEN_BLUETOOTH = 20, OPEN_MUSIC = 30, BACK = 99 };

static const mosaic_app_route_t music_routes[] = {
    { .action_id = OPEN_BLUETOOTH, .target_name = "bluetooth" },
};
static const mosaic_app_route_t bluetooth_routes[] = {
    { .action_id = OPEN_MUSIC, .target_name = "music" },
};
static const mosaic_app_descriptor_t root_app = {
    .id = ROOT_ID, .name = "mosaic-hub",
    .launch_action = MOSAIC_APP_NO_LAUNCH_ACTION,
};
static const mosaic_app_descriptor_t music_app = {
    .id = MUSIC_ID, .name = "music", .back_action = BACK,
    .routes = music_routes, .route_count = 1, .routes_without_history = true,
};
static const mosaic_app_descriptor_t bluetooth_app = {
    .id = BLUETOOTH_ID, .name = "bluetooth", .back_action = BACK,
    .routes = bluetooth_routes, .route_count = 1, .routes_without_history = true,
};
static const mosaic_app_descriptor_t album_app = {
    .id = ALBUM_ID, .name = "album", .back_action = BACK,
};

const mosaic_app_descriptor_t *const mosaic_app_registry[] = {
    &root_app, &music_app, &bluetooth_app, &album_app,
};
const size_t mosaic_app_registry_count =
    sizeof(mosaic_app_registry) / sizeof(mosaic_app_registry[0]);
const mosaic_app_package_t mosaic_app_packages[] = {
    { .descriptor = &root_app, .tick_ms = 0 },
    { .descriptor = &music_app, .tick_ms = 100 },
    { .descriptor = &bluetooth_app, .tick_ms = 1000 },
    { .descriptor = &album_app, .tick_ms = 0 },
};
const size_t mosaic_app_package_count =
    sizeof(mosaic_app_packages) / sizeof(mosaic_app_packages[0]);

typedef struct {
    mosaic_runtime_handle_t runtime;
    const mosaic_app_descriptor_t *active;
    mosaic_platform_ui_event_cb_t callback;
    void *callback_ctx;
    unsigned ticks[BLUETOOTH_ID + 1];
    uint32_t sequences[BLUETOOTH_ID + 1];
    unsigned replacements;
    uint32_t override_tick_ms;
    bool cancel_tick;
    const mosaic_app_descriptor_t *fail_open;
    const mosaic_app_descriptor_t *fail_start;
    const mosaic_app_descriptor_t *fail_close;
    bool partial_open;
    bool fail_replace;
    bool retain_replaced_app;
    unsigned closes;
    unsigned stops;
    unsigned recoveries;
} fixture_t;

static esp_err_t open_app(void *ctx, const mosaic_app_descriptor_t *app,
                         mosaic_platform_ui_event_cb_t callback,
                         void *callback_ctx, mosaic_platform_app_handle_t *out)
{
    fixture_t *fixture = ctx;
    assert(fixture->active == NULL);
    *out = NULL;
    if (fixture->fail_open == app && !fixture->partial_open) {
        return ESP_ERR_NO_MEM;
    }
    fixture->active = app;
    fixture->callback = callback;
    fixture->callback_ctx = callback_ctx;
    *out = fixture;
    return fixture->fail_open == app ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t replace_app(void *ctx, mosaic_platform_app_handle_t current,
                            const mosaic_app_descriptor_t *next,
                            mosaic_platform_ui_event_cb_t callback,
                            void *callback_ctx, mosaic_platform_app_handle_t *out)
{
    fixture_t *fixture = ctx;
    assert(current == fixture && fixture->active != &root_app && next != &root_app);
    if (fixture->fail_replace) {
        if (fixture->retain_replaced_app) {
            *out = current;
        } else {
            fixture->active = NULL;
            *out = NULL;
        }
        return ESP_FAIL;
    }
    fixture->active = NULL;
    ++fixture->replacements;
    return open_app(ctx, next, callback, callback_ctx, out);
}

static esp_err_t close_app(void *ctx, mosaic_platform_app_handle_t app)
{
    fixture_t *fixture = ctx;
    assert(app == fixture);
    assert(fixture->active != NULL);
    ++fixture->closes;
    if (fixture->fail_close == fixture->active) {
        return ESP_FAIL;
    }
    fixture->active = NULL;
    return ESP_OK;
}

static esp_err_t recover(void *ctx)
{
    fixture_t *fixture = ctx;
    assert(fixture->active == NULL);
    ++fixture->recoveries;
    return ESP_OK;
}

static esp_err_t step_app(void *ctx, mosaic_platform_app_handle_t app, int64_t now_us)
{
    (void)now_us;
    assert(ctx == app);
    return ESP_OK;
}

static esp_err_t dispatch_event(void *ctx, mosaic_platform_app_handle_t app,
                                const mosaic_event_t *event)
{
    fixture_t *fixture = ctx;
    assert(app == fixture && fixture->active != NULL);
    if (event->type == MOSAIC_EVENT_TIMER) {
        assert(strcmp(event->data.timer.id, "app_tick") == 0);
        ++fixture->ticks[fixture->active->id];
        fixture->sequences[fixture->active->id] = event->data.timer.sequence;
    } else if (event->type == MOSAIC_EVENT_START) {
        if (fixture->fail_start == fixture->active) {
            return ESP_FAIL;
        }
        if (fixture->override_tick_ms != 0) {
            assert(mosaic_runtime_timer_start(fixture->runtime, "app_tick",
                       fixture->override_tick_ms, true) == ESP_OK);
        } else if (fixture->cancel_tick) {
            assert(mosaic_runtime_timer_cancel(fixture->runtime, "app_tick") == ESP_OK);
        }
    } else if (event->type == MOSAIC_EVENT_STOP) {
        ++fixture->stops;
    }
    return ESP_OK;
}

static esp_err_t feed_pointer(void *ctx, mosaic_platform_app_handle_t app,
                             int32_t x, int32_t y, bool pressed)
{
    (void)x;
    (void)y;
    (void)pressed;
    assert(ctx == app);
    return ESP_OK;
}

static void start(fixture_t *fixture)
{
    static const mosaic_platform_ops_t platform = {
        .open_app = open_app, .replace_app = replace_app, .close_app = close_app,
        .step_app = step_app, .dispatch_event = dispatch_event, .feed_pointer = feed_pointer,
        .recover = recover,
    };
    const mosaic_runtime_config_t config = {
        .platform = &platform, .platform_ctx = fixture,
    };
    assert(mosaic_runtime_create(&config, &fixture->runtime) == ESP_OK);
    assert(mosaic_runtime_start(fixture->runtime, "music") == ESP_OK);
}

static void step(fixture_t *fixture, int64_t now_us)
{
    assert(mosaic_runtime_step(fixture->runtime, now_us) == ESP_OK);
}

static void action(fixture_t *fixture, uint16_t id, int64_t now_us)
{
    const esp_gsp_event_t event = { .type = ESP_GSP_EVENT_CALL, .action_id = id };
    fixture->callback(fixture->callback_ctx, &event);
    step(fixture, now_us);
}

static void test_normal_open_tick_cadence(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    step(&fixture, 99999);
    assert(fixture.ticks[MUSIC_ID] == 0);
    step(&fixture, 100000);
    assert(fixture.ticks[MUSIC_ID] == 1 && fixture.sequences[MUSIC_ID] == 0);
    step(&fixture, 200000);
    assert(fixture.ticks[MUSIC_ID] == 2 && fixture.sequences[MUSIC_ID] == 1);
    mosaic_runtime_delete(fixture.runtime);
    puts("PASS normal App open preserves tick cadence");
}

static void test_direct_switch_restores_destination_ticks(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    step(&fixture, 100000);
    action(&fixture, OPEN_BLUETOOTH, 250000);
    assert(fixture.replacements == 1 && fixture.active == &bluetooth_app);
    step(&fixture, 350000);
    assert(fixture.ticks[MUSIC_ID] == 1 && fixture.ticks[BLUETOOTH_ID] == 0);
    step(&fixture, 1249999);
    assert(fixture.ticks[BLUETOOTH_ID] == 0);
    step(&fixture, 1250000);
    assert(fixture.ticks[BLUETOOTH_ID] == 1 && fixture.sequences[BLUETOOTH_ID] == 0);
    step(&fixture, 2250000);
    assert(fixture.ticks[BLUETOOTH_ID] == 2 && fixture.sequences[BLUETOOTH_ID] == 1);

    action(&fixture, OPEN_MUSIC, 2300000);
    assert(fixture.replacements == 2 && fixture.active == &music_app);
    step(&fixture, 2399999);
    assert(fixture.ticks[MUSIC_ID] == 1);
    step(&fixture, 2400000);
    assert(fixture.ticks[MUSIC_ID] == 2 && fixture.sequences[MUSIC_ID] == 0);
    step(&fixture, 3500000);
    assert(fixture.ticks[BLUETOOTH_ID] == 2);
    mosaic_runtime_delete(fixture.runtime);
    puts("PASS direct Music/Bluetooth switches restart the destination cadence");
}

static void test_zero_tick_destination_drops_previous_timer(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    assert(mosaic_runtime_request_app(fixture.runtime, "album") == ESP_OK);
    step(&fixture, 50000);
    assert(fixture.replacements == 1 && fixture.active == &album_app);
    step(&fixture, 2000000);
    assert(fixture.ticks[ALBUM_ID] == 0 && fixture.ticks[MUSIC_ID] == 0);
    mosaic_runtime_delete(fixture.runtime);
    puts("PASS a zero-tick App inherits no timer");
}

static void test_start_callback_can_override_or_cancel_tick(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    fixture.override_tick_ms = 250;
    action(&fixture, OPEN_BLUETOOTH, 50000);
    step(&fixture, 299999);
    assert(fixture.ticks[BLUETOOTH_ID] == 0);
    step(&fixture, 300000);
    assert(fixture.ticks[BLUETOOTH_ID] == 1);
    fixture.override_tick_ms = 0;
    fixture.cancel_tick = true;
    action(&fixture, OPEN_MUSIC, 350000);
    step(&fixture, 2000000);
    assert(fixture.ticks[MUSIC_ID] == 0 && fixture.ticks[BLUETOOTH_ID] == 1);
    mosaic_runtime_delete(fixture.runtime);
    puts("PASS START callbacks retain control of the default tick");
}

static void test_partial_open_keeps_owner_until_recovered(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    assert(mosaic_runtime_request_app(fixture.runtime, "mosaic-hub") == ESP_OK);
    step(&fixture, 1);
    fixture.fail_open = &album_app;
    fixture.partial_open = true;
    fixture.fail_close = &album_app;
    assert(mosaic_runtime_request_app(fixture.runtime, "album") == ESP_OK);
    assert(mosaic_runtime_step(fixture.runtime, 2) == ESP_ERR_NO_MEM);
    assert(fixture.active == &album_app);
    assert(mosaic_runtime_active_app(fixture.runtime) == NULL);
    const unsigned stopped = fixture.stops, closed = fixture.closes;
    step(&fixture, 500001);
    assert(fixture.closes == closed);
    assert(mosaic_runtime_step(fixture.runtime, 500002) == ESP_FAIL);
    assert(mosaic_runtime_step(fixture.runtime, 1000002) == ESP_FAIL);
    assert(mosaic_runtime_step(fixture.runtime, 1500002) == ESP_FAIL);
    step(&fixture, 5000000);
    assert(fixture.closes == closed + 3 && fixture.stops == stopped);
    assert(mosaic_runtime_delete(fixture.runtime) == ESP_FAIL);
    /* Failed deletion retained the callback context and runtime for retry. */
    fixture.fail_open = NULL;
    fixture.fail_close = NULL;
    assert(mosaic_runtime_back(fixture.runtime) == ESP_OK);
    step(&fixture, 5000001);
    assert(fixture.active == &root_app && fixture.recoveries == 1);
    assert(mosaic_runtime_delete(fixture.runtime) == ESP_OK);
    puts("PASS partial-open owners survive bounded recovery and failed deletion");
}

static void test_failed_replace_ownership(void)
{
    for (unsigned retained = 0; retained < 2; ++retained) {
        fixture_t fixture = {0};
        start(&fixture);
        fixture.fail_replace = true;
        fixture.retain_replaced_app = retained;
        assert(mosaic_runtime_request_app(fixture.runtime, "bluetooth") == ESP_OK);
        assert(mosaic_runtime_step(fixture.runtime, 1) == ESP_FAIL);
        assert(fixture.stops == 1);
        if (retained) {
            assert(fixture.active == &music_app && fixture.closes == 0);
            /* Late queued UI calls are ignored while the owner recovers. */
            const esp_gsp_event_t event = { .type = ESP_GSP_EVENT_CALL,
                                          .action_id = OPEN_BLUETOOTH };
            fixture.callback(fixture.callback_ctx, &event);
            step(&fixture, 500001);
            assert(fixture.closes == 1 && fixture.recoveries == 1);
        } else {
            assert(fixture.closes == 0 && fixture.recoveries == 0);
        }
        assert(fixture.active == &root_app && fixture.stops == 1);
        assert(mosaic_runtime_delete(fixture.runtime) == ESP_OK);
    }
    puts("PASS replacement failure distinguishes retained and consumed owners");
}

static void test_failed_start_closes_after_stop(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    assert(mosaic_runtime_request_app(fixture.runtime, "mosaic-hub") == ESP_OK);
    step(&fixture, 1);
    fixture.fail_start = &album_app;
    fixture.fail_close = &album_app;
    assert(mosaic_runtime_request_app(fixture.runtime, "album") == ESP_OK);
    assert(mosaic_runtime_step(fixture.runtime, 2) == ESP_FAIL);
    const unsigned stopped = fixture.stops;
    fixture.fail_close = NULL;
    step(&fixture, 500002);
    assert(fixture.active == &root_app && fixture.stops == stopped);
    assert(mosaic_runtime_delete(fixture.runtime) == ESP_OK);
    puts("PASS failed START stops once and retains its owner until close succeeds");
}

static void test_failed_delete_cannot_dispatch_stopped_logic(void)
{
    fixture_t fixture = {0};
    start(&fixture);
    fixture.fail_close = &music_app;
    assert(mosaic_runtime_delete(fixture.runtime) == ESP_FAIL);
    assert(fixture.stops == 1 && fixture.active == &music_app);
    assert(mosaic_runtime_active_app(fixture.runtime) == NULL);
    assert(mosaic_runtime_step(fixture.runtime, 1) == ESP_ERR_INVALID_STATE);
    assert(mosaic_runtime_dispatch_pointer(fixture.runtime, 1, 1, true) == ESP_ERR_INVALID_STATE);
    const esp_gsp_event_t event = { .type = ESP_GSP_EVENT_CALL,
                                  .action_id = OPEN_BLUETOOTH };
    fixture.callback(fixture.callback_ctx, &event);
    assert(fixture.replacements == 0);
    fixture.fail_close = NULL;
    assert(mosaic_runtime_back(fixture.runtime) == ESP_OK);
    step(&fixture, 2);
    assert(fixture.active == &root_app && fixture.stops == 1);
    assert(mosaic_runtime_delete(fixture.runtime) == ESP_OK);
    puts("PASS failed deletion preserves its owner without dispatching stopped logic");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_normal_open_tick_cadence();
    test_direct_switch_restores_destination_ticks();
    test_zero_tick_destination_drops_previous_timer();
    test_start_callback_can_override_or_cancel_tick();
    test_partial_open_keeps_owner_until_recovered();
    test_failed_replace_ownership();
    test_failed_start_closes_after_stop();
    test_failed_delete_cannot_dispatch_stopped_logic();
    return 0;
}
