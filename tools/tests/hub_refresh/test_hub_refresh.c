/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct tm test_clock = { .tm_year = 126, .tm_mon = 9, .tm_mday = 27,
                               .tm_yday = 299, .tm_hour = 12, .tm_min = 30 };
static time_t test_time(time_t *out)
{
    if (out) *out = 100000;
    return 100000;
}
static struct tm *test_localtime(const time_t *value, struct tm *out)
{
    (void)value;
    *out = test_clock;
    return out;
}

#define time test_time
#define localtime_r test_localtime
#include "../../../../components/mosaic_ui/hub/mosaic_hub_app.c"
#undef time
#undef localtime_r

static unsigned writes, text_writes, snapshot_reads, wifi_reads;
static bool fail_writes;
static int64_t test_now_us = 1000000;
static unsigned navigation_pushes, navigation_pops;
static uint16_t stack_page;
static char module_port[64], module_title[64], module_message[64];
static bool stack_animating, fail_stack_query;
static esp_gsp_component_motion_t motion;
static const esp_gsp_handle_t ui = (esp_gsp_handle_t)(uintptr_t)1;
static struct {
    esp_gsp_timer_cb_t cb;
    void *ctx;
    int64_t due;
    bool active;
} fx_timers[16];
static unsigned fx_timer_next, fx_entries[2];
static bool fx_visible[2], fail_timer_create, fail_timer_delete;

static esp_gsp_err_t write_result(void)
{
    ++writes;
    return fail_writes ? ESP_GSP_ERR_TIMEOUT : ESP_GSP_OK;
}

/* Backdrop pixels/gestures use the native-runtime fixture; these tests isolate
 * Hub refresh scheduling and provider writes. */
void mosaic_hub_backdrop_start(esp_gsp_handle_t ui) { (void)ui; }
void mosaic_hub_backdrop_stop(esp_gsp_handle_t ui) { (void)ui; }
void mosaic_hub_backdrop_contact(esp_gsp_handle_t ui, bool pressed, bool prepare)
{ (void)ui; (void)pressed; (void)prepare; }

#define WRITE_STUB(name, parameters) esp_gsp_err_t name parameters { return write_result(); }
WRITE_STUB(esp_gsp_set_value, (esp_gsp_handle_t g, uint16_t b, int32_t v))
WRITE_STUB(esp_gsp_component_set_value, (esp_gsp_handle_t g, gsp_component_key_t k, int32_t v))
WRITE_STUB(esp_gsp_component_set_visible, (esp_gsp_handle_t g, gsp_component_key_t k, bool v))
WRITE_STUB(esp_gsp_component_set_color, (esp_gsp_handle_t g, gsp_component_key_t k, uint32_t v))
WRITE_STUB(esp_gsp_component_set_property, (esp_gsp_handle_t g, gsp_component_key_t k, gsp_property_key_t p, const gsp_value_t *v))
WRITE_STUB(esp_gsp_component_animate_property_to, (esp_gsp_handle_t g, gsp_component_key_t k, gsp_property_key_t p, const gsp_value_t *v, uint32_t ms, esp_gsp_ease_t e))
WRITE_STUB(esp_gsp_component_animate_color_to, (esp_gsp_handle_t g, gsp_component_key_t k, uint32_t v, uint32_t ms, esp_gsp_ease_t e))
WRITE_STUB(esp_gsp_drawer_open, (esp_gsp_handle_t g, gsp_component_key_t k, bool a))
WRITE_STUB(esp_gsp_drawer_close, (esp_gsp_handle_t g, gsp_component_key_t k, bool a))
WRITE_STUB(esp_gsp_component_stop_position_animation, (esp_gsp_handle_t g, gsp_component_key_t k))
WRITE_STUB(esp_gsp_list_set_total, (esp_gsp_handle_t g, esp_gsp_list_t l, uint32_t n))
WRITE_STUB(esp_gsp_list_scroll_to, (esp_gsp_handle_t g, esp_gsp_list_t l, int32_t n))
WRITE_STUB(esp_gsp_set_input_interceptor, (esp_gsp_handle_t g, esp_gsp_input_interceptor_cb_t c, void *ctx))

esp_gsp_err_t esp_gsp_component_animate_position_to(esp_gsp_handle_t g,
    gsp_component_key_t k, int32_t x, int32_t y, uint32_t ms, esp_gsp_ease_t e)
{
    if (!fail_writes && k == GSP_OBJ_KEY_INSERT_FX_LEFT && x == 0) ++fx_entries[0];
    if (!fail_writes && k == GSP_OBJ_KEY_INSERT_FX_RIGHT && x == 464) ++fx_entries[1];
    return write_result();
}

esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t g, void *t)
{
    if (t != (void *)(uintptr_t)1) {
        if (fail_timer_delete) return ESP_GSP_ERR_TIMEOUT;
        for (unsigned i = 0; i < 16; ++i) {
            if (t == &fx_timers[i]) fx_timers[i].active = false;
        }
    }
    return ESP_GSP_OK;
}

esp_gsp_list_t esp_gsp_list_bind_component(esp_gsp_handle_t g, gsp_component_key_t k,
                                          esp_gsp_list_bind_cb_t cb, void *ctx)
{
    return 1;
}

esp_err_t mosaic_settings_set_wifi_enabled(bool enabled) { return ESP_OK; }
esp_err_t mosaic_settings_set_vibration(bool enabled) { return ESP_OK; }
esp_err_t mosaic_settings_set_brightness(int value, bool persist) { return ESP_OK; }
esp_err_t mosaic_settings_set_volume(int value, bool persist) { return ESP_OK; }
esp_err_t mosaic_settings_subscribe_battery(mosaic_battery_event_cb_t cb, void *ctx) { return ESP_OK; }
esp_err_t mosaic_settings_unsubscribe_battery(mosaic_battery_event_cb_t cb, void *ctx) { return ESP_OK; }
esp_err_t mosaic_settings_subscribe_wifi(mosaic_wifi_event_cb_t cb, void *ctx) { return ESP_OK; }
esp_err_t mosaic_settings_unsubscribe_wifi(mosaic_wifi_event_cb_t cb, void *ctx) { return ESP_OK; }

esp_gsp_err_t esp_gsp_component_set_text(esp_gsp_handle_t g, gsp_component_key_t k, const char *s)
{
    ++text_writes;
    return write_result();
}
esp_gsp_err_t esp_gsp_component_set_position(esp_gsp_handle_t g, gsp_component_key_t k, int32_t x, int32_t y)
{
    return write_result();
}
esp_gsp_err_t esp_gsp_set_text(esp_gsp_handle_t g, uint16_t b, const char *s)
{
    ++text_writes;
    if (!fail_writes && b == GSP_BIND_INSERT_PORT) snprintf(module_port, sizeof(module_port), "%s", s);
    if (!fail_writes && b == GSP_BIND_INSERT_BOARD_NAME) {
        snprintf(module_title, sizeof(module_title), "%s", s);
    }
    if (!fail_writes && b == GSP_BIND_INSERT_BOARD_CAP) {
        snprintf(module_message, sizeof(module_message), "%s", s);
    }
    return write_result();
}
esp_gsp_err_t esp_gsp_set_visible(esp_gsp_handle_t g, uint16_t b, bool v)
{
    if (!fail_writes && b == GSP_BIND_INSERT_FX_LEFT_VISIBLE) fx_visible[0] = v;
    if (!fail_writes && b == GSP_BIND_INSERT_FX_RIGHT_VISIBLE) fx_visible[1] = v;
    return write_result();
}
esp_gsp_err_t esp_gsp_stack_view_push(esp_gsp_handle_t g, gsp_component_key_t k,
                                    uint16_t page, bool animated)
{
    ++navigation_pushes;
    if (!fail_writes) stack_page = page;
    return write_result();
}
esp_gsp_err_t esp_gsp_stack_view_pop(esp_gsp_handle_t g, gsp_component_key_t k, bool animated)
{
    ++navigation_pops;
    if (!fail_writes) stack_page = 0;
    return write_result();
}
esp_gsp_err_t esp_gsp_stack_view_is_animating(esp_gsp_handle_t g,
    gsp_component_key_t key, bool *out)
{
    if (fail_stack_query) return ESP_GSP_ERR_INVALID_STATE;
    *out = stack_animating;
    return ESP_GSP_OK;
}
int64_t esp_timer_get_time(void) { return test_now_us; }
esp_gsp_err_t esp_gsp_component_get_motion(esp_gsp_handle_t g, gsp_component_key_t k,
                                          esp_gsp_component_motion_t *out)
{
    *out = motion;
    return ESP_GSP_OK;
}
esp_gsp_err_t esp_gsp_drawer_is_open(esp_gsp_handle_t g, gsp_component_key_t k, bool *out)
{
    *out = motion.value != 0;
    return ESP_GSP_OK;
}
esp_gsp_err_t esp_gsp_stack_view_get_top(esp_gsp_handle_t g, gsp_component_key_t k, uint16_t *out)
{
    *out = stack_page;
    return ESP_GSP_OK;
}
void *esp_gsp_timer_create(esp_gsp_handle_t g, uint32_t ms, esp_gsp_timer_cb_t cb, void *ctx)
{
    if (cb == mosaic_hub_insert_timer_cb) {
        if (fail_timer_create) return NULL;
        unsigned i = fx_timer_next++ % 16;
        assert(!fx_timers[i].active);
        fx_timers[i].cb = cb;
        fx_timers[i].ctx = ctx;
        fx_timers[i].due = test_now_us + (int64_t)ms * 1000;
        fx_timers[i].active = true;
        return &fx_timers[i];
    }
    return (void *)(uintptr_t)1;
}

static void next_fx_phase(void)
{
    for (unsigned i = 0; i < 16; ++i) {
        if (fx_timers[i].active) {
            if (test_now_us < fx_timers[i].due) test_now_us = fx_timers[i].due;
            fx_timers[i].cb(ui, fx_timers[i].ctx);
            return;
        }
    }
    assert(!"no side feedback timer");
}

static void finish_fx(void)
{
    unsigned remaining = 16;
    while (s_insert_fx.cycle.mask && remaining--) next_fx_phase();
    assert(!s_insert_fx.cycle.mask && !s_insert_fx.timer);
    assert(!fx_visible[0] && !fx_visible[1]);
}
esp_err_t mosaic_settings_get_snapshot(mosaic_settings_snapshot_t *out)
{
    ++snapshot_reads;
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}
esp_err_t mosaic_settings_get_wifi(mosaic_settings_network_t *out)
{
    ++wifi_reads;
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}
esp_err_t mosaic_settings_get_battery(mosaic_settings_battery_t *out)
{
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

static void test_motion_defers_periodic_work(void)
{
    s_wifi_pending = true;
    s_battery_pending = true;
    motion.dragging = true;
    mosaic_hub_clock_tick(ui, NULL);
    mosaic_hub_battery_tick(ui, NULL);
    assert(writes == 0 && snapshot_reads == 0 && wifi_reads == 0);
    assert(s_wifi_pending && s_battery_pending);

    motion.dragging = false;
    motion.settling = true;
    mosaic_hub_clock_tick(ui, NULL);
    mosaic_hub_battery_tick(ui, NULL);
    assert(writes == 0 && s_wifi_pending && s_battery_pending);

    motion.settling = false;
    mosaic_hub_battery_tick(ui, NULL);
    assert(writes > 0 && text_writes > 0);
    assert(!s_wifi_pending && !s_battery_pending);
    puts("PASS drag and settle defer work without losing pending state");
}

static void test_clock_text_is_published_only_when_changed(void)
{
    unsigned before = text_writes;
    ++test_clock.tm_sec;
    mosaic_hub_clock_apply(ui);
    assert(text_writes == before);
    ++test_clock.tm_min;
    mosaic_hub_clock_apply(ui);
    assert(text_writes > before);
    before = text_writes;
    ++test_clock.tm_min;
    fail_writes = true;
    mosaic_hub_clock_apply(ui);
    fail_writes = false;
    mosaic_hub_clock_apply(ui);
    assert(text_writes >= before + 2);
    before = text_writes;
    mosaic_hub_clock_apply(ui);
    assert(text_writes == before);
    puts("PASS unchanged clock text is skipped and failed publication is retried");
}

static void test_wifi_visuals_follow_discrete_signal_levels(void)
{
    mosaic_settings_network_t network = { .enabled = true, .connected = true,
                                          .desired_enabled = true, .rssi = -60 };
    mosaic_hub_wifi_apply(ui, &network);
    unsigned before = writes;
    network.rssi = -61;
    mosaic_hub_wifi_apply(ui, &network);
    assert(writes == before);
    network.rssi = -70;
    mosaic_hub_wifi_apply(ui, &network);
    assert(writes > before);
    before = writes;
    network.rssi = -80;
    fail_writes = true;
    mosaic_hub_wifi_apply(ui, &network);
    fail_writes = false;
    mosaic_hub_wifi_apply(ui, &network);
    assert(writes > before);
    before = writes;
    mosaic_hub_wifi_apply(ui, &network);
    assert(writes == before);
    puts("PASS RSSI changes within one bar level cause no redundant publications");
}

static void publish_module(unsigned side, uint32_t generation, bool present)
{
    const mosaico_module_mgr_info_t info = {
        .slot = side, .generation = generation,
        .presence = present ? MOSAICO_MODULE_PRESENCE_PRESENT : MOSAICO_MODULE_PRESENCE_ABSENT,
        .descriptor_state = present ? MOSAICO_MODULE_DESCRIPTOR_VALID : MOSAICO_MODULE_DESCRIPTOR_UNKNOWN,
        .eeprom = { .board_type = side == 0 ? MOSAICO_BOARD_TYPE_CAMERA : MOSAICO_BOARD_TYPE_INTERACT },
    };
    mosaic_hub_update_module(&info);
}

static void reset_module_feedback(void)
{
    s_hub_ui = ui;
    memset(&s_insert_fx, 0, sizeof(s_insert_fx));
    memset(fx_timers, 0, sizeof(fx_timers));
    memset(fx_entries, 0, sizeof(fx_entries));
    memset(fx_visible, 0, sizeof(fx_visible));
    fx_timer_next = 0;
    fail_writes = fail_timer_create = fail_timer_delete = false;
    s_pointer_down = false;
    s_module_notice = (mosaic_module_notice_t) {0};
    s_module_notice_applied = (mosaic_module_notice_view_t) {0};
    s_module_notice_applied_valid = true;
    s_module_intro_active = false;
    s_module_notice_foreground = true;
    s_module_notice_accepting = true;
    s_module_notice_deferred = false;
    stack_animating = fail_stack_query = false;
    navigation_pushes = navigation_pops = 0;
    motion = (esp_gsp_component_motion_t) {0};
    s_lock_mode = MOSAIC_LOCK_HIDDEN;
    stack_page = 0;
    test_now_us = 1000000;
}

static void test_module_feedback_keeps_navigation_and_merges_sides(void)
{
    reset_module_feedback();
    publish_module(0, 1, true);
    publish_module(1, 1, true);
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && navigation_pushes == 0);
    assert(fx_entries[0] == 0 && fx_entries[1] == 0);

    publish_module(0, 2, false);
    publish_module(1, 2, false);
    publish_module(0, 3, true);
    test_now_us += 100000;
    publish_module(1, 3, true);
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 0);
    test_now_us += 300000;
    fail_stack_query = true;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 0 && fx_entries[0] == 0 && fx_entries[1] == 0);
    fail_stack_query = false;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && fx_visible[0] && fx_visible[1]);
    assert(fx_entries[0] == 1 && fx_entries[1] == 1);
    const int64_t started = test_now_us;
    finish_fx();
    assert(test_now_us - started == 810000);
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE && s_module_intro_active);
    assert(strcmp(module_port, "Left port") == 0);
    assert(strcmp(module_title, "Camera") == 0);
    assert(strcmp(module_message, "Photo / Video") == 0);
    assert(s_module_notice_applied.mask == 1 && navigation_pushes == 1);
    const unsigned before = writes;
    test_now_us += 20 * MOSAIC_MODULE_NOTICE_DURATION_US;
    mosaic_hub_module_notice_tick(ui);
    assert(writes == before && s_module_notice.visible_mask == 2);
    const unsigned timers_before = fx_timer_next;
    for (unsigned i = 0; i < 1000; ++i) {
        test_now_us += 200000;
        mosaic_hub_module_notice_tick(ui);
    }
    assert(writes == before && fx_timer_next == timers_before);

    stack_page = 0; /* Native Done retains its original animation. */
    stack_animating = true;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 1 && writes == before);
    stack_animating = false;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 2 && s_module_notice_applied.mask == 2);
    assert(strcmp(module_port, "Right port") == 0);
    assert(strcmp(module_title, "Interaction") == 0);
    assert(strcmp(module_message, "LED / Touch / IR / Sensors") == 0);
    assert(fx_entries[0] == 1 && fx_entries[1] == 1);
    publish_module(1, 4, false);
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && !s_module_intro_active && navigation_pops == 1);
    puts("PASS silent boot; both side animations finish once; original single-board pages queue without interrupting Done");
}

static void test_module_feedback_lifecycle_and_publication_failure(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(0, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    fail_writes = true;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && navigation_pushes == 0);
    fail_writes = false;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE && navigation_pushes == 1);
    assert(mosaic_hub_quiet_module_notice(ui) == ESP_GSP_OK);
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE && s_module_notice.visible_mask == 0);
    publish_module(0, 3, false);
    publish_module(0, 4, true); /* paused foreground doesn't queue feedback */
    test_now_us += 1000000;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE && navigation_pushes == 1);

    mosaic_hub_stopping(ui);
    assert(!s_module_notice_foreground && s_hub_ui == NULL);
    publish_module(1, 1, false);
    publish_module(1, 2, true);
    stack_page = 0;
    mosaic_hub_started(ui);
    test_now_us += 1000000;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && navigation_pushes == 1);

    publish_module(0, 5, false);
    publish_module(0, 6, true);
    motion.value = 1;
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    motion.value = 0;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && navigation_pushes == 1);
    publish_module(0, 7, false);
    publish_module(0, 8, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    finish_fx();
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE && navigation_pushes == 2);
    const unsigned before = writes;
    assert(mosaic_hub_quiet_module_notice(ui) == ESP_GSP_OK);
    assert(mosaic_hub_quiet_module_notice(ui) == ESP_GSP_OK && writes == before);
    stack_page = 0; /* Done must not reopen a consumed introduction. */
    mosaic_hub_module_notice_tick(ui);
    assert(!s_module_intro_active && navigation_pushes == 2);
    publish_module(0, 9, false);
    publish_module(0, 10, true);
    stack_page = 1;
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    stack_page = 0;
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && navigation_pushes == 2);
    puts("PASS failed publication retries; screen-off retains the page and inactive feedback never replays");
}

static void test_second_insertion_queues_without_changing_the_existing_introduction(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(1, 1, false);
    publish_module(0, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    finish_fx();
    assert(navigation_pushes == 1 && strcmp(module_title, "Camera") == 0);
    test_now_us += 1000000;
    publish_module(1, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    assert(fx_visible[1] && !fx_visible[0] && navigation_pushes == 1);
    finish_fx();
    assert(navigation_pushes == 1 && strcmp(module_title, "Camera") == 0);
    assert(s_module_notice_applied.mask == 1 && s_module_notice.visible_mask == 2);
    publish_module(1, 3, false); /* An unplugged queued notice is never shown. */
    stack_page = 0;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 1 && !s_module_intro_active);
    puts("PASS late insertion preserves the current page and Open target; removal cancels a queued notice");
}

static void test_duplicate_apps_and_stale_open_actions(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(1, 1, false);
    publish_module(0, 2, true);
    const mosaico_module_mgr_info_t camera = {.slot=1,.generation=2,
        .presence=MOSAICO_MODULE_PRESENCE_PRESENT,
        .descriptor_state=MOSAICO_MODULE_DESCRIPTOR_VALID,
        .eeprom={.board_type=MOSAICO_BOARD_TYPE_CAMERA}};
    mosaic_hub_update_module(&camera);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    finish_fx();
    assert(s_module_notice_applied.mask == 3 && navigation_pushes == 1);
    assert(strcmp(module_title, "Camera") == 0);
    publish_module(0, 3, false);
    assert(mosaic_hub_handle_action(GSP_ACT_ID_INSERT_OPEN) && navigation_pops == 0);
    mosaic_hub_module_notice_tick(ui);
    assert(s_module_notice_applied.mask == 2 && strcmp(module_port, "Right port") == 0);
    assert(mosaic_hub_handle_action(GSP_ACT_ID_INSERT_OPEN) && navigation_pops == 1);
    assert(mosaic_hub_handle_action(GSP_ACT_ID_INSERT_OPEN) && navigation_pops == 1);

    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(1, 1, false);
    publish_module(0, 2, true);
    publish_module(1, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    finish_fx();
    publish_module(0, 3, false);
    assert(mosaic_hub_handle_action(GSP_ACT_ID_INSERT_OPEN) && navigation_pops == 0);
    mosaic_hub_module_notice_tick(ui);
    assert(stack_page == 0 && navigation_pops == 1);
    stack_animating = true;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 1);
    stack_animating = false;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 2 && strcmp(module_title, "Interaction") == 0);
    puts("PASS duplicate applications share one page; removal closes the old target before a queued application opens");
}

static void start_left_feedback(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(1, 1, false);
    publish_module(0, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    assert(fx_visible[0] && !fx_visible[1] && navigation_pushes == 0);
}

static void test_long_contact_preserves_a_real_insertion(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    s_pointer_down = true;
    publish_module(0, 2, true);
    const unsigned before = writes;
    for (unsigned i = 0; i < 30; ++i) {
        test_now_us += 200000;
        mosaic_hub_battery_tick(ui, NULL);
    }
    assert(writes == before && fx_entries[0] == 0 && s_module_notice.pending_mask == 1);
    s_pointer_down = false;
    mosaic_hub_battery_tick(ui, NULL);
    assert(s_module_notice.pending_mask == 1 && navigation_pushes == 0);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_battery_tick(ui, NULL);
    assert(fx_entries[0] == 1 && fx_visible[0]);
    finish_fx();
    assert(navigation_pushes == 1 && strcmp(module_title, "Camera") == 0);
    puts("PASS a six-second held contact defers a real insertion without expiring or interrupting it");
}

static void test_unplug_during_a_long_contact_cancels_the_notice(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    s_pointer_down = true;
    publish_module(0, 2, true);
    for (unsigned i = 0; i < 30; ++i) {
        if (i == 10) publish_module(0, 3, false);
        test_now_us += 200000;
        mosaic_hub_battery_tick(ui, NULL);
    }
    s_pointer_down = false;
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_battery_tick(ui, NULL);
    assert(s_module_notice.pending_mask == 0 && fx_entries[0] == 0 && navigation_pushes == 0);
    puts("PASS unplug during a held contact cancels the deferred insertion without replay");
}

static void test_contact_after_side_feedback_preserves_the_introduction(void)
{
    start_left_feedback();
    s_pointer_down = true;
    finish_fx();
    assert(navigation_pushes == 0 && s_module_notice.visible_mask == 1);
    const unsigned before = writes;
    for (unsigned i = 0; i < 30; ++i) {
        test_now_us += 200000;
        mosaic_hub_battery_tick(ui, NULL);
    }
    assert(writes == before);
    s_pointer_down = false;
    mosaic_hub_battery_tick(ui, NULL);
    assert(navigation_pushes == 1 && s_module_intro_active && fx_entries[0] == 1);
    puts("PASS a held contact after side feedback preserves the introduction without replaying animation");
}

static void test_retained_intro_keeps_its_target_across_apps(void)
{
    start_left_feedback();
    finish_fx();
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE && s_module_intro_active);
    mosaic_hub_stopping(ui);
    assert(stack_page == MOSAIC_HUB_INSERT_PAGE); /* SDK retains component state. */
    mosaic_hub_started(ui);
    assert(s_module_intro_active && s_module_notice_applied.mask == 1);
    assert(mosaic_hub_handle_action(GSP_ACT_ID_INSERT_OPEN) && navigation_pops == 1);
    puts("PASS returning to a retained introduction preserves its validated Open target");
}

static void test_retained_intro_revalidates_removal_during_another_app(void)
{
    start_left_feedback();
    finish_fx();
    mosaic_hub_stopping(ui);
    publish_module(0, 3, false);
    mosaic_hub_started(ui);
    assert(mosaic_hub_handle_action(GSP_ACT_ID_INSERT_OPEN) && navigation_pops == 0);
    mosaic_hub_battery_tick(ui, NULL);
    assert(stack_page == 0 && !s_module_intro_active && navigation_pops == 1);
    puts("PASS a retained introduction rejects an unplugged target before idle cleanup closes it");
}

static void test_replacement_keeps_the_other_presented_application(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(1, 1, false);
    publish_module(0, 2, true);
    mosaico_module_mgr_info_t replacement = {.slot=1,.generation=2,
        .presence=MOSAICO_MODULE_PRESENCE_PRESENT,
        .descriptor_state=MOSAICO_MODULE_DESCRIPTOR_VALID,
        .eeprom={.board_type=MOSAICO_BOARD_TYPE_CAMERA}};
    mosaic_hub_update_module(&replacement);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    finish_fx();
    assert(s_module_notice_applied.mask == 3);

    publish_module(0, 3, false);
    replacement.slot = 0;
    replacement.generation = 4;
    replacement.eeprom.board_type = MOSAICO_BOARD_TYPE_INTERACT;
    mosaic_hub_update_module(&replacement);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    fail_timer_create = true;
    mosaic_hub_module_notice_tick(ui);
    assert(s_module_notice_applied.mask == 2 && strcmp(module_title, "Camera") == 0);
    assert(strcmp(module_port, "Right port") == 0 && navigation_pushes == 1);
    fail_timer_create = false;
    stack_page = 0;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 2 && s_module_notice_applied.mask == 1);
    assert(strcmp(module_title, "Interaction") == 0);
    puts("PASS a replacement generation cannot retarget the other visible application, including animation failure");
}

static void test_side_phases_late_arrival_and_pointer_guard(void)
{
    start_left_feedback();
    void *old_ctx = fx_timers[0].ctx;
    next_fx_phase();
    assert(!fx_visible[0] && s_insert_fx.phase == 1);
    void *current_timer = s_insert_fx.timer;
    mosaic_hub_insert_timer_cb(ui, old_ctx);
    assert(s_insert_fx.timer == current_timer && s_insert_fx.phase == 1);
    next_fx_phase();
    assert(fx_visible[0] && s_insert_fx.phase == 2);
    publish_module(1, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    assert(fx_entries[0] == 1 && fx_entries[1] == 0);
    next_fx_phase();
    next_fx_phase();
    assert(!fx_visible[0] && navigation_pushes == 0);
    next_fx_phase();
    assert(!fx_visible[0] && fx_visible[1] && navigation_pushes == 0);
    assert(fx_entries[0] == 1 && fx_entries[1] == 1);
    s_pointer_down = true;
    finish_fx();
    assert(navigation_pushes == 0);
    s_pointer_down = false;
    mosaic_hub_battery_tick(ui, NULL);
    assert(navigation_pushes == 1 && strcmp(module_title, "Camera") == 0);
    puts("PASS side phases and late arrivals finish once per side before one introduction; held contacts defer navigation");
}

static void test_side_removal_quiet_and_stale_callbacks(void)
{
    start_left_feedback();
    const esp_gsp_timer_cb_t stale_cb = fx_timers[0].cb;
    void *stale_ctx = fx_timers[0].ctx;
    publish_module(0, 3, false);
    mosaic_hub_module_notice_tick(ui);
    assert(!fx_visible[0] && !s_insert_fx.timer && !s_insert_fx.cycle.mask);
    stale_cb(ui, stale_ctx);
    assert(!navigation_pushes);
    publish_module(0, 4, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    void *new_timer = s_insert_fx.timer;
    stale_cb(ui, stale_ctx);
    assert(s_insert_fx.timer == new_timer && fx_visible[0]);
    assert(mosaic_hub_quiet_module_notice(ui) == ESP_GSP_OK);
    assert(!s_insert_fx.timer && !fx_visible[0]);
    stale_cb(ui, stale_ctx);
    mosaic_hub_module_notice_tick(ui);
    assert(!navigation_pushes && !s_insert_fx.cycle.mask);

    start_left_feedback();
    stale_ctx = fx_timers[0].ctx;
    mosaic_hub_stopping(ui);
    mosaic_hub_started(ui);
    const unsigned before = writes;
    stale_cb(ui, stale_ctx);
    assert(writes == before && !s_insert_fx.timer && !navigation_pushes);

    start_left_feedback();
    stack_page = 1;
    next_fx_phase();
    assert(!fx_visible[0] && !s_insert_fx.timer);
    stack_page = 0;
    mosaic_hub_module_notice_tick(ui);
    assert(!navigation_pushes && !s_insert_fx.cycle.mask);
    puts("PASS removal, quiet and app switches cancel feedback; stale callbacks cannot touch a new sequence");
}

static void test_side_partial_removal_and_timer_failure(void)
{
    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(0, 2, true);
    fail_timer_create = true;
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 1 && !s_insert_fx.timer && !fx_visible[0]);

    start_left_feedback();
    fail_timer_create = true; /* Failure while scheduling the first blink. */
    next_fx_phase();
    assert(navigation_pushes == 1 && !s_insert_fx.timer && !fx_visible[0]);
    fail_timer_create = false;
    mosaic_hub_module_notice_tick(ui);
    assert(navigation_pushes == 1 && fx_entries[0] == 1);

    start_left_feedback();
    fail_timer_delete = true;
    assert(mosaic_hub_quiet_module_notice(ui) == ESP_GSP_ERR_TIMEOUT);
    assert(!fx_visible[0] && !s_insert_fx.cycle.mask && s_insert_fx.timer);
    fail_timer_delete = false;
    assert(mosaic_hub_quiet_module_notice(ui) == ESP_GSP_OK);
    assert(!s_insert_fx.timer);
    mosaic_hub_module_notice_tick(ui);
    assert(!navigation_pushes);

    reset_module_feedback();
    publish_module(0, 1, false);
    publish_module(1, 1, false);
    publish_module(0, 2, true);
    publish_module(1, 2, true);
    test_now_us += MOSAIC_MODULE_NOTICE_MERGE_US;
    mosaic_hub_module_notice_tick(ui);
    publish_module(0, 3, false);
    mosaic_hub_module_notice_tick(ui);
    assert(!fx_visible[0] && fx_visible[1] && s_insert_fx.cycle.mask == 2);
    finish_fx();
    assert(navigation_pushes == 1 && strcmp(module_title, "Interaction") == 0);
    puts("PASS partial removal keeps the other side; timer failures retain introduction and bounded cleanup");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_motion_defers_periodic_work();
    test_clock_text_is_published_only_when_changed();
    test_wifi_visuals_follow_discrete_signal_levels();
    test_module_feedback_keeps_navigation_and_merges_sides();
    test_module_feedback_lifecycle_and_publication_failure();
    test_second_insertion_queues_without_changing_the_existing_introduction();
    test_duplicate_apps_and_stale_open_actions();
    test_long_contact_preserves_a_real_insertion();
    test_unplug_during_a_long_contact_cancels_the_notice();
    test_contact_after_side_feedback_preserves_the_introduction();
    test_retained_intro_keeps_its_target_across_apps();
    test_retained_intro_revalidates_removal_during_another_app();
    test_replacement_keeps_the_other_presented_application();
    test_side_phases_late_arrival_and_pointer_guard();
    test_side_removal_quiet_and_stale_callbacks();
    test_side_partial_removal_and_timer_failure();
    return 0;
}
