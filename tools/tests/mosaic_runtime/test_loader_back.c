/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { ESP_OK = 0, ESP_GSP_OK = 0, MOSAIC_LOADER_COMMAND_BACK = 4,
       GSP_OBJ_KEY_QUICK_DRAWER = 1, GSP_OBJ_KEY_HUB_STACK = 2,
       GSP_OBJ_KEY_LAUNCHER_FLOW = 3, MOSAIC_HUB_INSERT_PAGE = 8 };
typedef void *esp_gsp_handle_t;
typedef struct { bool (*on_back)(void *, int64_t); bool back_exits_app;
                 uint32_t root_stack_key; const char *name; } mosaic_app_descriptor_t;
typedef struct { int value; bool dragging, settling; } esp_gsp_component_motion_t;
static const mosaic_app_descriptor_t root = { .name = "mosaic-hub" };
static const mosaic_app_descriptor_t child = { .name = "camera", .root_stack_key = 7 };
static const mosaic_app_descriptor_t *active;
static bool ui_available = true;
static void *s_runtime = (void *)(uintptr_t)1, *s_platform = (void *)(uintptr_t)2;
static unsigned back_calls, gui_calls, root_requests, pops, drawer_closes;
static int back_error, drawer_value;
static uint16_t child_page;
static struct { int type; } back_command = { MOSAIC_LOADER_COMMAND_BACK };
static const mosaic_app_descriptor_t *mosaic_app_root(void) { return &root; }
static const mosaic_app_descriptor_t *mosaic_runtime_active_app(void *runtime)
{ (void)runtime; return active; }
static void *mosaic_esp_platform_ui(void *platform)
{ (void)platform; return ui_available ? (void *)(uintptr_t)3 : NULL; }
static int mosaic_runtime_back(void *runtime) { (void)runtime; ++back_calls; return back_error; }
static int mosaic_runtime_request_app(void *runtime, const char *name)
{ (void)runtime; assert(!strcmp(name, root.name)); ++root_requests; return 0; }
static int64_t esp_timer_get_time(void) { return 1; }
static int esp_gsp_stack_view_get_top(void *ui, unsigned key, uint16_t *page)
{ assert(ui); ++gui_calls; *page = key == 7 ? child_page : 0; return ESP_GSP_OK; }
static int esp_gsp_stack_view_pop(void *ui, unsigned key, bool animated)
{ (void)key; (void)animated; assert(ui); ++gui_calls; ++pops; return ESP_GSP_OK; }
static int esp_gsp_component_get_motion(void *ui, unsigned key, esp_gsp_component_motion_t *motion)
{ (void)key; assert(ui); ++gui_calls; motion->value = drawer_value; return ESP_GSP_OK; }
static int esp_gsp_drawer_close(void *ui, unsigned key, bool animated)
{ (void)key; (void)animated; assert(ui); ++gui_calls; ++drawer_closes; return ESP_GSP_OK; }
static int esp_gsp_page_flow_get_page(void *ui, unsigned key, uint16_t *page)
{ (void)key; assert(ui); ++gui_calls; *page = 0; return ESP_GSP_OK; }
static int esp_gsp_page_flow_set_page(void *ui, unsigned key, uint16_t page, bool animated)
{ (void)key; (void)page; (void)animated; assert(ui); ++gui_calls; return ESP_GSP_OK; }
static void mosaic_hub_show_lock_screen(bool show) { (void)show; ++gui_calls; }
#define command (&back_command)
#include "loader_back_under_test.h"
#undef command

int main(void)
{
    process_back_under_test();
    assert(back_calls == 1 && gui_calls == 0 && root_requests == 0);
    ui_available = false;
    process_back_under_test();
    assert(back_calls == 2 && gui_calls == 0);
    active = &child;
    process_back_under_test();
    assert(back_calls == 2 && gui_calls == 0);
    ui_available = true; child_page = 1;
    process_back_under_test();
    assert(pops == 1 && back_calls == 2);
    child_page = 0; back_error = -1;
    process_back_under_test();
    assert(back_calls == 3 && root_requests == 1);
    active = &root; drawer_value = 1;
    process_back_under_test();
    assert(drawer_closes == 1 && back_calls == 3);
    puts("PASS loader Back rearms hidden recovery without UI access and preserves ordinary page/drawer routing");
    return 0;
}
