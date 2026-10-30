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
static esp_gsp_component_motion_t motion;
static const esp_gsp_handle_t ui = (esp_gsp_handle_t)(uintptr_t)1;

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
WRITE_STUB(esp_gsp_set_visible, (esp_gsp_handle_t g, uint16_t b, bool v))
WRITE_STUB(esp_gsp_component_set_value, (esp_gsp_handle_t g, gsp_component_key_t k, int32_t v))
WRITE_STUB(esp_gsp_component_set_visible, (esp_gsp_handle_t g, gsp_component_key_t k, bool v))
WRITE_STUB(esp_gsp_component_set_color, (esp_gsp_handle_t g, gsp_component_key_t k, uint32_t v))
WRITE_STUB(esp_gsp_component_set_position, (esp_gsp_handle_t g, gsp_component_key_t k, int32_t x, int32_t y))
WRITE_STUB(esp_gsp_component_set_property, (esp_gsp_handle_t g, gsp_component_key_t k, gsp_property_key_t p, const gsp_value_t *v))
WRITE_STUB(esp_gsp_component_animate_position_to, (esp_gsp_handle_t g, gsp_component_key_t k, int32_t x, int32_t y, uint32_t ms, esp_gsp_ease_t e))
WRITE_STUB(esp_gsp_component_animate_property_to, (esp_gsp_handle_t g, gsp_component_key_t k, gsp_property_key_t p, const gsp_value_t *v, uint32_t ms, esp_gsp_ease_t e))
WRITE_STUB(esp_gsp_component_animate_color_to, (esp_gsp_handle_t g, gsp_component_key_t k, uint32_t v, uint32_t ms, esp_gsp_ease_t e))
WRITE_STUB(esp_gsp_stack_view_pop, (esp_gsp_handle_t g, gsp_component_key_t k, bool a))
WRITE_STUB(esp_gsp_drawer_open, (esp_gsp_handle_t g, gsp_component_key_t k, bool a))
WRITE_STUB(esp_gsp_drawer_close, (esp_gsp_handle_t g, gsp_component_key_t k, bool a))
WRITE_STUB(esp_gsp_timer_delete, (esp_gsp_handle_t g, void *t))
WRITE_STUB(esp_gsp_component_stop_position_animation, (esp_gsp_handle_t g, gsp_component_key_t k))
WRITE_STUB(esp_gsp_stack_view_push, (esp_gsp_handle_t g, gsp_component_key_t k, uint16_t p, bool a))
WRITE_STUB(esp_gsp_list_set_total, (esp_gsp_handle_t g, esp_gsp_list_t l, uint32_t n))
WRITE_STUB(esp_gsp_list_scroll_to, (esp_gsp_handle_t g, esp_gsp_list_t l, int32_t n))
WRITE_STUB(esp_gsp_set_input_interceptor, (esp_gsp_handle_t g, esp_gsp_input_interceptor_cb_t c, void *ctx))

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
esp_gsp_err_t esp_gsp_set_text(esp_gsp_handle_t g, uint16_t b, const char *s)
{
    ++text_writes;
    return write_result();
}
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
    *out = 0;
    return ESP_GSP_OK;
}
void *esp_gsp_timer_create(esp_gsp_handle_t g, uint32_t ms, esp_gsp_timer_cb_t cb, void *ctx)
{
    return (void *)(uintptr_t)1;
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

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_motion_defers_periodic_work();
    test_clock_text_is_published_only_when_changed();
    test_wifi_visuals_follow_discrete_signal_levels();
    return 0;
}
