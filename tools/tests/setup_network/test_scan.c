/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../../components/mosaic_ui/apps/setup_center/setup_center_app.c"
#ifndef ESP_ERR_NOT_FINISHED
#define ESP_ERR_NOT_FINISHED 0x10C
#endif

static bool backend = true;
static unsigned requests;
static size_t cache_count;
static esp_err_t cache_error;
static mosaic_settings_network_t live;
static unsigned forgotten;
static unsigned notices;

bool mosaic_settings_wifi_backend_available(void) { return backend; }
esp_err_t mosaic_settings_request_wifi_scan(void) { ++requests; return ESP_OK; }
esp_err_t mosaic_settings_get_wifi(mosaic_settings_network_t *out) { *out = live; return ESP_OK; }
esp_err_t mosaic_settings_forget_wifi(void)
{
    ++forgotten;
    live.state = MOSAIC_SETTINGS_WIFI_IDLE;
    return ESP_OK;
}
esp_gsp_err_t mosaic_top_notice_show(esp_gsp_handle_t ui,
    const mosaic_top_notice_config_t *config, const char *title,
    const char *message, uint32_t duration_ms)
{
    (void)ui; (void)config; (void)message; (void)duration_ms;
    assert(strcmp(title, "Incorrect password") == 0);
    ++notices;
    return ESP_GSP_OK;
}
esp_err_t mosaic_settings_scan_wifi(mosaic_settings_wifi_ap_t *records, size_t capacity, size_t *count)
{
    assert(capacity >= cache_count);
    *count = cache_count;
    for (size_t i = 0; i < cache_count; ++i) {
        snprintf(records[i].ssid, sizeof(records[i].ssid), "Test-AP-%zu", i);
        records[i].secured = true;
    }
    return cache_error;
}
esp_gsp_err_t esp_gsp_component_set_visible(esp_gsp_handle_t ui, gsp_component_key_t key, bool value)
{ (void)ui; (void)key; (void)value; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_set_text(esp_gsp_handle_t ui, uint16_t bind, const char *text)
{ (void)ui; (void)bind; (void)text; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_component_set_text(esp_gsp_handle_t ui, gsp_component_key_t key, const char *text)
{ (void)ui; (void)key; (void)text; return ESP_GSP_OK; }
esp_gsp_list_t esp_gsp_list_bind_component(esp_gsp_handle_t ui, gsp_component_key_t key,
    esp_gsp_list_bind_cb_t callback, void *context)
{ (void)ui; (void)key; (void)callback; (void)context; return 1; }
esp_gsp_err_t esp_gsp_list_set_total(esp_gsp_handle_t ui, esp_gsp_list_t list, uint32_t total)
{ (void)ui; (void)list; (void)total; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_list_refresh(esp_gsp_handle_t ui, esp_gsp_list_t list)
{ (void)ui; (void)list; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_row_text(esp_gsp_handle_t ui, esp_gsp_row_t row, const char *text)
{ (void)ui; (void)row; (void)text; return ESP_GSP_OK; }

static void reset(uint32_t revision)
{
    memset(&s_setup, 0, sizeof(s_setup));
    s_setup.ui = (esp_gsp_handle_t)(uintptr_t)1;
    s_setup.network_list = ESP_GSP_LIST_NONE;
    s_setup.network_phase = SETUP_NETWORK_SCAN;
    live = (mosaic_settings_network_t){.scan_revision=revision, .scan_error=ESP_OK};
    setup_network_event(&live, NULL);
    cache_count = 0;
    cache_error = ESP_OK;
    requests = 0;
    forgotten = notices = 0;
}

int main(void)
{
    reset(1);
    setup_network_scan_step(0);
    assert(s_setup.network_scan_ready && s_setup.network_count == 0);
    live.scan_revision = 2;
    cache_count = 2;
    setup_network_event(&live, NULL);
    setup_network_scan_step(100000);
    assert(s_setup.network_count == 2);
    assert(strcmp(s_setup.networks[0].ssid, "Test-AP-0") == 0);
    puts("PASS a newer scan replaces an already displayed empty cache");

    reset(4);
    cache_count = 1;
    setup_network_request_scan(100000);
    setup_network_scan_step(200000);
    assert(!s_setup.network_scan_ready);
    live.scan_revision = 5;
    cache_count = 2;
    setup_network_event(&live, NULL);
    setup_network_scan_step(300000);
    assert(s_setup.network_scan_ready && s_setup.network_count == 2);
    puts("PASS a retry waits for the requested completion instead of accepting an old cache");

    reset(0);
    cache_count = 2;
    setup_network_request_scan(0);
    setup_network_scan_step(100000);
    assert(s_setup.network_scan_ready && s_setup.network_count == 2);
    puts("PASS synchronous providers without completion revisions remain supported");

    reset(7);
    cache_error = ESP_ERR_NOT_FINISHED;
    setup_network_request_scan(0);
    setup_network_scan_step(100000);
    assert(!s_setup.network_scan_ready && requests == 1);
    setup_network_scan_step(2000000);
    assert(!s_setup.network_scan_ready && requests == 2);
    puts("PASS pending scans remain pending and retry on deadline");

    reset(10);
    cache_count = 2;
    for (unsigned retry = 0; retry < 2; ++retry) {
        s_setup.network_phase = SETUP_NETWORK_JOINING;
        s_setup.network_connect_pending = true;
        strcpy(s_setup.selected_ssid, "Test-AP-0");
        live.state = MOSAIC_SETTINGS_WIFI_AUTH_FAILED;
        setup_network_apply_model(5000000 + retry * 1000000);
        assert(s_setup.network_phase == SETUP_NETWORK_SCAN);
        assert(!s_setup.network_connect_pending && !s_setup.network_configured);
        assert(s_setup.selected_ssid[0] == '\0');
        setup_network_scan_step(5100000 + retry * 1000000);
        assert(!s_setup.network_scan_ready);
        ++live.scan_revision;
        setup_network_event(&live, NULL);
        setup_network_scan_step(5200000 + retry * 1000000);
        assert(s_setup.network_scan_ready && s_setup.network_count == 2);
    }
    assert(notices == 2 && forgotten == 2 && requests == 2);
    puts("PASS two incorrect-password failures each return to a fresh selectable WLAN list");

    reset(20);
    cache_count = 2;
    setup_network_request_scan(0);
    live.scan_revision = 21;
    live.scan_error = ESP_FAIL;
    setup_network_event(&live, NULL);
    setup_network_scan_step(100000);
    assert(!s_setup.network_scan_ready);
    setup_network_scan_step(2000000);
    assert(requests == 2);
    live.scan_revision = 22;
    live.scan_error = ESP_OK;
    setup_network_event(&live, NULL);
    setup_network_scan_step(2100000);
    assert(s_setup.network_scan_ready && s_setup.network_count == 2);
    puts("PASS failed completions cannot publish an older successful cache and recover on retry");
    return 0;
}
