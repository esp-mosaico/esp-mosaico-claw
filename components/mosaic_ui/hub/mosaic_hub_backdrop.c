/* SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_hub_backdrop.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_gsp_extension.h"
#include "mosaic_hub_binds.h"
#include "mosaic_hub_objects.h"
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

/* Implemented by the content-pinned GSP compatibility patch. UI-task only. */
extern esp_gsp_err_t mosaic_gsp_capture_stack(esp_gsp_handle_t ui,
    uint32_t stack_key, void *pixels, uint16_t width, uint16_t height,
    size_t stride, size_t capacity);

#define BACKDROP_WIDTH 480U
#define BACKDROP_HEIGHT 480U
#define BACKDROP_STRIDE (BACKDROP_WIDTH * 2U)
#define BACKDROP_BYTES (BACKDROP_STRIDE * BACKDROP_HEIGHT)
#define BACKDROP_ACK_PENDING INT_MIN

static atomic_uint s_generation;
static atomic_int s_canvas_ack;
static esp_gsp_handle_t s_ui;
static void *s_timer;
static uint8_t *s_pixels;
static bool s_ready, s_active, s_contact, s_prepared, s_disabled;

static void draw_backdrop(const esp_gsp_canvas_surface_t *surface, void *ctx)
{
    (void)ctx;
    if (surface->pixel_format != ESP_GSP_CANVAS_PIXEL_RGB565 ||
            surface->x + surface->width > BACKDROP_WIDTH ||
            surface->y + surface->height > BACKDROP_HEIGHT) return;
    for (uint16_t y=0;y<surface->height;++y) {
        uint8_t *out=(uint8_t *)surface->pixels+(size_t)y*surface->stride_bytes;
        if (s_pixels != NULL) memcpy(out,s_pixels+
            (size_t)(surface->y+y)*BACKDROP_STRIDE+(size_t)surface->x*2U,
            (size_t)surface->width*2U);
        else memset(out,0,(size_t)surface->width*2U);
    }
}

static void canvas_result(esp_gsp_handle_t ui,
    const esp_gsp_extension_event_t *event, void *ctx)
{
    if (ui==s_ui && (uint32_t)(uintptr_t)ctx==atomic_load(&s_generation) &&
            event->kind==ESP_GSP_EXTENSION_CANVAS_RESULT &&
            event->data.canvas.operation==ESP_GSP_CANVAS_SET_DRAW &&
            event->data.canvas.bind==GSP_BIND_HUB_BACKDROP_CANVAS &&
            event->data.canvas.request_id==(uint32_t)(uintptr_t)ctx) {
        /* Extension observers only record acknowledgements; no UI mutation. */
        atomic_store(&s_canvas_ack,event->data.canvas.status);
    }
}

static esp_gsp_err_t restore_live(esp_gsp_handle_t ui)
{
    /* A partial queue drain can show both layers; the live layer starts with
     * an opaque black fill, so it always covers the cached backdrop safely. */
    esp_gsp_err_t err=esp_gsp_set_visible(ui,GSP_BIND_HUB_LIVE_VISIBLE,true);
    if (err==ESP_GSP_OK) err=esp_gsp_set_visible(ui,GSP_BIND_HUB_BACKDROP_VISIBLE,false);
    s_active=err!=ESP_GSP_OK; /* Retry a full command queue on the next tick. */
    s_prepared=false;
    return err;
}

static void backdrop_tick(esp_gsp_handle_t ui, void *ctx)
{
    if ((uint32_t)(uintptr_t)ctx!=atomic_load(&s_generation)) return;
    const int ack=atomic_load(&s_canvas_ack);
    if (!s_ready && ack!=BACKDROP_ACK_PENDING) s_ready=ack==ESP_GSP_OK;
    if (!s_active && !s_prepared) {
        if (s_timer!=NULL && !s_contact &&
                esp_gsp_timer_delete(ui,s_timer)==ESP_GSP_OK) s_timer=NULL;
        return;
    }
    esp_gsp_component_motion_t motion={0};
    uint16_t page=0;
    if (esp_gsp_component_get_motion(ui,GSP_OBJ_KEY_QUICK_DRAWER,&motion)!=ESP_GSP_OK ||
            esp_gsp_stack_view_get_top(ui,GSP_OBJ_KEY_HUB_STACK,&page)!=ESP_GSP_OK ||
            page!=0 || (!s_contact && !motion.dragging && !motion.settling && motion.value==0)) {
        restore_live(ui);
        return;
    }
    if (s_prepared && !s_active &&
            (motion.dragging || motion.settling || motion.value!=0)) {
        esp_gsp_err_t err=esp_gsp_set_visible(ui,GSP_BIND_HUB_BACKDROP_VISIBLE,true);
        if (err==ESP_GSP_OK) err=esp_gsp_set_visible(ui,GSP_BIND_HUB_LIVE_VISIBLE,false);
        if (err==ESP_GSP_OK) { s_active=true; s_prepared=false; }
        else restore_live(ui);
    }
}

void mosaic_hub_backdrop_start(esp_gsp_handle_t ui)
{
    if (s_disabled) { restore_live(ui); return; }
    s_ui=ui;
    s_active=s_contact=s_ready=s_prepared=false;
    const uint32_t generation=atomic_fetch_add(&s_generation,1U)+1U;
    atomic_store(&s_canvas_ack,BACKDROP_ACK_PENDING);
    esp_gsp_err_t err=esp_gsp_on_extension_event_ex(ui,canvas_result,
        (void *)(uintptr_t)generation,ESP_GSP_EXTENSION_MASK_CANVAS_RESULT,
        ESP_GSP_CANVAS_MASK_SET_DRAW);
    if (err==ESP_GSP_OK) err=esp_gsp_canvas_set_draw_cb_ex(ui,
        GSP_BIND_HUB_BACKDROP_CANVAS,draw_backdrop,NULL,generation);
    if (err!=ESP_GSP_OK) return;
}

void mosaic_hub_backdrop_contact(esp_gsp_handle_t ui, bool pressed, bool prepare)
{
    s_contact=pressed;
    if (!s_ready) s_ready=atomic_load(&s_canvas_ack)==ESP_GSP_OK;
    if (!prepare || !s_ready || s_disabled || s_active) return;
    const uint32_t generation=atomic_load(&s_generation);
    if (s_pixels==NULL) {
#if defined(ESP_PLATFORM)
        s_pixels=heap_caps_aligned_alloc(64,BACKDROP_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
        s_pixels=aligned_alloc(64,BACKDROP_BYTES);
#endif
        if (s_pixels==NULL) return;
    }
    if (mosaic_gsp_capture_stack(ui,GSP_OBJ_KEY_HUB_STACK,s_pixels,
            BACKDROP_WIDTH,BACKDROP_HEIGHT,BACKDROP_STRIDE,BACKDROP_BYTES)!=ESP_GSP_OK ||
            generation!=atomic_load(&s_generation)) return;
    s_prepared=true;
    /* No 16 ms polling while the Hub is idle. Observe Drawer motion only
     * during an eligible contact, then retire the timer after closing. */
    if (s_timer==NULL) s_timer=esp_gsp_timer_create(ui,16,backdrop_tick,
        (void *)(uintptr_t)generation);
    if (s_timer==NULL) s_prepared=false;
}

void mosaic_hub_backdrop_stop(esp_gsp_handle_t ui)
{
    if (s_disabled) return;
    atomic_fetch_add(&s_generation,1U);
    /* First fence selected callbacks, then retire the callback-backed canvas.
     * Never free a source whose stop was not fenced successfully. */
    esp_gsp_err_t err=esp_gsp_flush(ui,1500);
    if (err==ESP_GSP_OK && s_timer!=NULL) {
        err=esp_gsp_timer_delete(ui,s_timer);
        if (err==ESP_GSP_OK) s_timer=NULL;
    }
    const esp_gsp_err_t restore_err=restore_live(ui);
    if (err==ESP_GSP_OK) err=restore_err;
    if (err==ESP_GSP_OK) err=esp_gsp_on_extension_event(ui,NULL,NULL);
    if (err==ESP_GSP_OK) err=esp_gsp_canvas_stop(ui,GSP_BIND_HUB_BACKDROP_CANVAS);
    if (err==ESP_GSP_OK) err=esp_gsp_flush(ui,1500);
    if (err==ESP_GSP_OK) {
        free(s_pixels);s_pixels=NULL;s_ui=NULL;
        s_ready=s_active=s_contact=s_prepared=false;
    } else {
        s_disabled=true;
        /* The old UI owns any unfenced timer/canvas. Keep only its source
         * alive; never pass old handles to a later Hub context. */
        s_timer=NULL;s_ui=NULL;
        s_ready=s_active=s_contact=s_prepared=false;
#if defined(ESP_PLATFORM)
        ESP_LOGW("hub_backdrop","cleanup not fenced (%d); retain source and disable caching",(int)err);
#endif
    }
}
