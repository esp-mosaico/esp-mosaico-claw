/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../../../components/mosaic_ui/apps/settings/settings_app.c"

static unsigned refreshes;
static esp_gsp_err_t refresh_result;
static bool snapshot_active;
static esp_gsp_err_t bind_result;
static bool fail_bind_during_refresh;

esp_gsp_err_t esp_gsp_row_set_image(esp_gsp_handle_t ui, esp_gsp_row_t row,
    uint16_t slot, const void *data, size_t size)
{
    (void)ui; (void)row; (void)slot; (void)data; (void)size;
    return bind_result;
}

esp_gsp_err_t esp_gsp_row_set_text(esp_gsp_handle_t ui, esp_gsp_row_t row,
    uint16_t slot, const char *text)
{
    (void)ui; (void)row; (void)slot; (void)text;
    return bind_result;
}

esp_gsp_err_t esp_gsp_list_refresh(esp_gsp_handle_t ui, esp_gsp_list_t list)
{
    assert(ui == (void *)(uintptr_t)1 && list == 7);
    ++refreshes;
    if (fail_bind_during_refresh) {
        fail_bind_during_refresh = false;
        bind_result = ESP_GSP_ERR_NO_MEM;
        assert(settings_root_list_bind_item(ui, (esp_gsp_row_t){0}, 0, NULL) == bind_result);
        bind_result = ESP_GSP_OK;
    }
    if (refresh_result == ESP_GSP_OK) snapshot_active = false;
    return refresh_result;
}

static void expect_change(esp_gsp_handle_t ui)
{
    const unsigned before = refreshes;
    settings_root_list_refresh(ui);
    assert(refreshes == before + 1);
    settings_root_list_refresh(ui);
    assert(refreshes == before + 1);
}

int main(void)
{
    esp_gsp_handle_t ui = (void *)(uintptr_t)1;
    memset(&s_state, 0, sizeof(s_state));
    settings_root_list_reset();
    settings_root_list_refresh(ui);
    assert(refreshes == 0);
    s_root_list = 7;
    expect_change(ui);
    snapshot_active = true;
    for (unsigned i = 0; i < 1000; ++i) {
        s_state.snapshot.battery.current_ma = (int16_t)i;
        s_state.snapshot.battery.sequence = i;
        s_state.snapshot.network.scan_revision = i;
        settings_root_list_refresh(ui);
    }
    assert(refreshes == 1 && snapshot_active);
    puts("PASS 1000 unrelated/duplicate model updates preserve the root List snapshot");

    s_state.brightness = 42; expect_change(ui);
    s_state.volume = 25; expect_change(ui);
    s_state.snapshot.battery.available = true;
    s_state.snapshot.battery.state_of_charge = 55; expect_change(ui);
    s_state.snapshot.battery.state_of_charge = 56; expect_change(ui);
    s_state.snapshot.battery.available = false; expect_change(ui);
    s_state.snapshot.battery.state_of_charge = 99;
    const unsigned unavailable = refreshes;
    settings_root_list_refresh(ui);
    assert(refreshes == unavailable);
    s_state.snapshot.battery.available = true; expect_change(ui);
    /* Preserve the provider's full type instead of narrowing cached values. */
    s_state.snapshot.battery.state_of_charge = 355; expect_change(ui);
    s_state.snapshot.battery.state_of_charge = 99; expect_change(ui);
    s_state.snapshot.im.qq_configured = true; expect_change(ui);
    strlcpy(s_state.snapshot.llm.model, "test-model", sizeof(s_state.snapshot.llm.model));
    strlcpy(s_state.snapshot.llm.backend, "test-backend", sizeof(s_state.snapshot.llm.backend));
    s_state.snapshot.llm.endpoint_configured = true;
    expect_change(ui);
    puts("PASS changed visible values refresh immediately; unavailable telemetry stays quiet");

    s_state.wlan.enabled = true;
    s_state.wlan.connected = true;
    s_state.wlan.state = MOSAIC_SETTINGS_WIFI_CONNECTED;
    strlcpy(s_state.wlan.current_ssid, "12345678901234567890123456789012",
            sizeof(s_state.wlan.current_ssid));
    expect_change(ui);
    s_state.wlan.current_ssid[31] = 'x'; expect_change(ui);
    s_state.wlan.state = MOSAIC_SETTINGS_WIFI_AUTH_FAILED; expect_change(ui);
    puts("PASS full-length SSID changes and connection errors are not delayed");

    refresh_result = ESP_GSP_ERR_TIMEOUT;
    s_state.volume = 26;
    unsigned before = refreshes;
    settings_root_list_refresh(ui);
    settings_root_list_refresh(ui);
    assert(refreshes == before + 2);
    refresh_result = ESP_GSP_OK;
    expect_change(ui);

    s_state.volume = 27;
    fail_bind_during_refresh = true;
    before = refreshes;
    settings_root_list_refresh(ui);
    settings_root_list_refresh(ui);
    assert(refreshes == before + 2);
    settings_root_list_refresh(ui);
    assert(refreshes == before + 2);
    bind_result = ESP_GSP_ERR_NO_MEM;
    assert(settings_root_list_bind_item(ui, (esp_gsp_row_t){0}, 2, NULL) == bind_result);
    bind_result = ESP_GSP_OK;
    expect_change(ui);
    puts("PASS async row failures retry, including a failure before enqueue returns");

    settings_root_list_reset();
    settings_root_list_refresh(ui);
    before = refreshes;
    s_root_list = 7;
    expect_change(ui);
    assert(refreshes == before + 1);
    puts("PASS rejected queues retry and a new App visit always refreshes");
    return 0;
}
