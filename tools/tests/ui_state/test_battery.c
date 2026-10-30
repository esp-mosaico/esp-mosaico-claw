/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../../../components/mosaic_ui/apps/settings/settings_app.c"

static mosaic_settings_battery_t sample;
static esp_err_t provider_error;
static char charge[32], voltage[32], current[32], status[32];
static unsigned publications;
static esp_err_t provider(void *context, mosaic_settings_battery_t *out)
{ (void)context; *out = sample; return provider_error; }
esp_gsp_err_t esp_gsp_set_text(esp_gsp_handle_t ui, uint16_t bind, const char *text)
{
    (void)ui;
    char *target = bind == GSP_BIND_SETTINGS_BATTERY_CHARGE ? charge :
        bind == GSP_BIND_SETTINGS_BATTERY_VOLTAGE ? voltage :
        bind == GSP_BIND_SETTINGS_BATTERY_CURRENT ? current :
        bind == GSP_BIND_SETTINGS_BATTERY_STATUS ? status : NULL;
    assert(target != NULL);
    snprintf(target, 32, "%s", text);
    ++publications;
    return ESP_GSP_OK;
}

int main(void)
{
    esp_gsp_handle_t ui = (esp_gsp_handle_t)(uintptr_t)1;
    s_state.ops.get_battery = provider;
    s_detail_list = ESP_GSP_LIST_NONE;
    sample = (mosaic_settings_battery_t){.available=true,.state_of_charge=40,.voltage_mv=3800,.current_ma=-100};
    settings_refresh_battery_detail(ui);
    assert(strcmp(charge, "40%") == 0 && strcmp(voltage, "3800 mV") == 0 && strcmp(current, "-100 mA") == 0);
    sample.state_of_charge = 55; sample.voltage_mv = 3990; sample.current_ma = 125;
    settings_refresh_battery_detail(ui);
    assert(strcmp(charge, "55%") == 0 && strcmp(voltage, "3990 mV") == 0 && strcmp(current, "125 mA") == 0);
    assert(publications == 8 && strcmp(status, "Live")==0);
    puts("PASS live Battery fields update without any virtual List handle");

    sample.available = false;
    settings_refresh_battery_detail(ui);
    assert(strcmp(charge, "--") == 0 && strcmp(voltage, "--") == 0 && strcmp(current, "--") == 0);
    unsigned before = publications;
    provider_error = ESP_FAIL;
    settings_refresh_battery_detail(ui);
    assert(publications == before+4 && strcmp(status,"Sensor unavailable")==0);
    provider_error = ESP_OK; sample.available = true;
    settings_refresh_battery_detail(ui);
    assert(strcmp(charge, "55%") == 0);
    sample.stale=true;
    settings_refresh_battery_detail(ui);
    assert(strcmp(charge,"55%")==0 && strcmp(status,"Cached - sensor unavailable")==0);
    sample.stale=false;
    settings_refresh_battery_detail(ui);
    assert(strcmp(status,"Live")==0);
    puts("PASS unavailable samples, read failures, stale cache and later recovery remain distinguishable");

    s_state.ops.get_battery = NULL;
    sample.state_of_charge = 88;
    mosaic_settings_notify_battery(&sample);
    settings_refresh_battery_detail(ui);
    assert(strcmp(charge, "88%") == 0);
    puts("PASS providers without a dedicated getter use the published battery cache");
    return 0;
}
