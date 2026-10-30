/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../../components/mosaic_ui/hub/mosaic_hub_backdrop.c"

static esp_err_t flush_error;
static esp_gsp_handle_t timer_owner;
static unsigned timer_deletes, foreign_deletes;
static int timer_token;
esp_gsp_err_t esp_gsp_flush(esp_gsp_handle_t ui, uint32_t timeout)
{ (void)ui; (void)timeout; return flush_error; }
esp_gsp_err_t esp_gsp_set_visible(esp_gsp_handle_t ui, uint16_t bind, bool value)
{ (void)ui; (void)bind; (void)value; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_timer_delete(esp_gsp_handle_t ui, void *timer)
{
    assert(timer == &timer_token);
    if (ui != timer_owner) { ++foreign_deletes; return ESP_GSP_ERR_INVALID_ARG; }
    ++timer_deletes;
    return ESP_GSP_OK;
}
void *esp_gsp_timer_create(esp_gsp_handle_t ui, uint32_t ms,
                         esp_gsp_timer_cb_t callback, void *ctx)
{ (void)ui; (void)ms; (void)callback; (void)ctx; return NULL; }
esp_gsp_err_t esp_gsp_canvas_stop(esp_gsp_handle_t ui, uint16_t bind)
{ (void)ui; (void)bind; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_on_extension_event(esp_gsp_handle_t ui,
    esp_gsp_extension_cb_t cb, void *ctx)
{ (void)ui; (void)cb; (void)ctx; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_on_extension_event_ex(esp_gsp_handle_t ui,
    esp_gsp_extension_cb_t cb, void *ctx, uint32_t events, uint32_t operations)
{ (void)ui; (void)cb; (void)ctx; (void)events; (void)operations; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_canvas_set_draw_cb_ex(esp_gsp_handle_t ui, uint16_t bind,
    esp_gsp_canvas_draw_cb_t cb, void *ctx, uint32_t request)
{ (void)ui; (void)bind; (void)cb; (void)ctx; (void)request; return ESP_GSP_OK; }
esp_gsp_err_t esp_gsp_component_get_motion(esp_gsp_handle_t ui,
    gsp_component_key_t key, esp_gsp_component_motion_t *motion)
{ (void)ui; (void)key; (void)motion; return ESP_GSP_ERR_INVALID_ARG; }
esp_gsp_err_t esp_gsp_stack_view_get_top(esp_gsp_handle_t ui,
    gsp_component_key_t key, uint16_t *page)
{ (void)ui; (void)key; (void)page; return ESP_GSP_ERR_INVALID_ARG; }

int main(void)
{
    esp_gsp_handle_t first = (esp_gsp_handle_t)(uintptr_t)1;
    esp_gsp_handle_t second = (esp_gsp_handle_t)(uintptr_t)2;
    timer_owner = first;
    s_ui = first;
    s_timer = &timer_token;
    s_pixels = aligned_alloc(64, BACKDROP_BYTES);
    assert(s_pixels != NULL);
    mosaic_hub_backdrop_stop(first);
    assert(s_pixels == NULL && s_timer == NULL && timer_deletes == 1);
    puts("PASS a fenced Hub stop retires its timer and frees the source");

    s_ui = first;
    s_timer = &timer_token;
    s_pixels = aligned_alloc(64, BACKDROP_BYTES);
    assert(s_pixels != NULL);
    memset(s_pixels, 0x5a, BACKDROP_BYTES);
    flush_error = ESP_GSP_ERR_INVALID_STATE;
    mosaic_hub_backdrop_stop(first);
    assert(s_disabled && s_pixels != NULL);
    flush_error = ESP_GSP_OK;
    mosaic_hub_backdrop_start(second);
    mosaic_hub_backdrop_stop(second);
    assert(foreign_deletes == 0 && s_pixels != NULL);
    uint16_t pixel = 0;
    const esp_gsp_canvas_surface_t surface = {.pixel_format=ESP_GSP_CANVAS_PIXEL_RGB565,
        .pixels=&pixel, .width=1, .height=1, .stride_bytes=2};
    draw_backdrop(&surface, NULL);
    assert(pixel == 0x5a5a);
    puts("PASS unfenced cleanup retains callback data and never reuses old UI handles");
    free(s_pixels); /* No real callback runs in this controlled test. */
    s_pixels = NULL;
    return 0;
}
