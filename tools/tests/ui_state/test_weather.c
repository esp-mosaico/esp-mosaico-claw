/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "freertos/portmacro.h"
#include "../../../components/mosaic_ui/apps/weather/weather_app.c"

esp_gsp_err_t esp_gsp_set_text(esp_gsp_handle_t ui, uint16_t bind, const char *text)
{ (void)ui; (void)bind; assert(text != NULL); return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_set_visible(esp_gsp_handle_t ui, uint16_t bind, bool visible)
{ (void)ui; (void)bind; (void)visible; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_list_refresh(esp_gsp_handle_t ui, esp_gsp_list_t list)
{ (void)ui; (void)list; return ESP_GSP_OK; }

int main(void)
{
    esp_gsp_handle_t ui = (esp_gsp_handle_t)(uintptr_t)1;
    weather_service_snapshot_t snapshot = {.online=true, .weather_valid=true};
    snapshot.forecast[0] = (weather_service_day_t){.valid=true,
        .temperature_min_c=18, .temperature_max_c=26, .temperature_sample_count=2,
        .date="10/27", .symbol_code="clearsky_day"};
    s_forecast_list = ESP_GSP_LIST_NONE;
    weather_render(ui, &snapshot);
    assert(strcmp(s_forecast_rows[0].high, "26°") == 0);
    assert(strcmp(s_forecast_rows[0].low, "18°") == 0);
    /* City selection clears the forecast while its HTTP request is pending. */
    snapshot.weather_valid = false;
    memset(snapshot.forecast, 0, sizeof(snapshot.forecast));
    weather_render(ui, &snapshot);
    assert(strcmp(s_forecast_rows[0].high, "--") == 0);
    assert(strcmp(s_forecast_rows[0].low, "--") == 0);
    assert(strcmp(s_forecast_rows[0].date, "--/--") == 0);
    /* A shorter later response must clear absent days as well. */
    snapshot.weather_valid = true;
    snapshot.forecast[0].valid = true;
    snapshot.forecast[0].temperature_max_c = 22;
    weather_render(ui, &snapshot);
    assert(strcmp(s_forecast_rows[0].high, "22°") == 0);
    assert(strcmp(s_forecast_rows[1].high, "--") == 0);
    puts("PASS city changes and missing forecast days clear old temperatures and dates");

    weather_service_snapshot_t notification = {.sequence=23, .temperature_c=30};
    weather_stage(&notification, NULL);
    notification.sequence = 22;
    notification.temperature_c = 10;
    weather_stage(&notification, NULL);
    weather_service_snapshot_t taken;
    assert(weather_take(&taken) && taken.sequence == 23 && taken.temperature_c == 30);
    weather_stage(&notification, NULL);
    assert(!weather_take(&taken));
    s_pending_seen = false;
    notification.sequence = UINT32_MAX;
    weather_stage(&notification, NULL);
    notification.sequence = 0;
    weather_stage(&notification, NULL);
    notification.sequence = UINT32_MAX;
    weather_stage(&notification, NULL);
    assert(weather_take(&taken) && taken.sequence == 0);
    puts("PASS delayed notifications cannot replace newer weather, including sequence wrap");
    return 0;
}
