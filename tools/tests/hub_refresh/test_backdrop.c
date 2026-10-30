/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned copy_calls, clear_calls;
static void *counted_copy(void *out, const void *in, size_t bytes)
{ ++copy_calls; return memcpy(out, in, bytes); }
static void *counted_clear(void *out, int value, size_t bytes)
{ ++clear_calls; return memset(out, value, bytes); }
#define memcpy counted_copy
#define memset counted_clear
#include "../../../components/mosaic_ui/hub/mosaic_hub_backdrop.c"
#undef memcpy
#undef memset

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
    s_pixels = aligned_alloc(64, BACKDROP_BYTES);
    assert(s_pixels != NULL);
    for (size_t i = 0; i < BACKDROP_BYTES; ++i) s_pixels[i] = (uint8_t)(i * 17U + i / 480U);
    static const struct { uint16_t x, y, width, height; size_t padding; } cases[] = {
        {0, 0, 480, 1, 0}, {0, 7, 480, 10, 0}, {0, 11, 480, 20, 0},
        {0, 23, 480, 30, 0}, {0, 0, 480, 480, 0}, {0, 479, 480, 1, 0},
        {7, 9, 37, 13, 16}, {0, 7, 480, 10, 8}, {0, 7, 480, 0, 0},
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const size_t width_bytes = (size_t)cases[index].width * 2U;
        const size_t stride = width_bytes + cases[index].padding;
        const size_t capacity = stride * cases[index].height + 32U;
        uint8_t *pixels = malloc(capacity), *expected = malloc(capacity);
        assert(pixels != NULL && expected != NULL);
        memset(pixels, 0xa7, capacity);
        memset(expected, 0xa7, capacity);
        for (size_t y = 0; y < cases[index].height; ++y) {
            for (size_t x = 0; x < width_bytes; ++x) {
                expected[y * stride + x] = s_pixels[
                    ((size_t)cases[index].y + y) * BACKDROP_STRIDE +
                    (size_t)cases[index].x * 2U + x];
            }
        }
        const esp_gsp_canvas_surface_t tile = {
            .pixel_format = ESP_GSP_CANVAS_PIXEL_RGB565, .pixels = pixels,
            .x = cases[index].x, .y = cases[index].y,
            .width = cases[index].width, .height = cases[index].height,
            .stride_bytes = stride,
        };
        copy_calls = clear_calls = 0;
        draw_backdrop(&tile, NULL);
        assert(memcmp(pixels, expected, capacity) == 0 && clear_calls == 0);
        assert(copy_calls == (width_bytes == BACKDROP_STRIDE &&
            stride == BACKDROP_STRIDE ? (cases[index].height != 0) : cases[index].height));
        uint8_t *source = s_pixels;
        s_pixels = NULL;
        memset(pixels, 0xa7, capacity);
        for (size_t y = 0; y < cases[index].height; ++y) memset(expected + y * stride, 0, width_bytes);
        copy_calls = clear_calls = 0;
        draw_backdrop(&tile, NULL);
        assert(memcmp(pixels, expected, capacity) == 0 && copy_calls == 0);
        s_pixels = source;
        free(expected); free(pixels);
    }
    free(s_pixels);
    s_pixels = NULL;
    puts("PASS contiguous tiles batch copies; cropped/padded/empty tiles preserve reference pixels and guards");

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
