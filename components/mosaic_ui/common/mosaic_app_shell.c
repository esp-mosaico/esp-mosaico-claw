/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "mosaic_app_shell.h"
#include "mosaic_app_shell_titles.h"

#define EXIT_SWIPE_MIN_PX 80
#define EXIT_EDGE_Y_MIN 460
#define EXIT_EDGE_X_MIN 160
#define EXIT_EDGE_X_MAX 320
#define EXIT_MAX_DX_RATIO 2
#define VIEWPORT_WIDTH 480
#define CHROME_POLL_MS 16

static mosaic_app_shell_exit_fn s_exit;
static void *s_exit_ctx;
static esp_gsp_handle_t s_ui;
static uint32_t s_root_stack_key;
static const mosaic_app_shell_title_asset_t *s_title;
static void *s_chrome_timer;
static bool s_root_visible;
static bool s_root_header_enabled;
static bool s_root_override_valid;
static bool s_root_override_visible;
static bool s_bottom_enabled;
static bool s_input_registration_queued;
static bool s_overlay_registration_queued;
static bool s_tracking;
static bool s_exit_triggered;
static int32_t s_x0;
static int32_t s_y0;
static mosaic_system_notice_t s_notice;
static bool s_notice_visible;
static void *s_notice_timer;
static bool s_notice_only;

static void draw_app_shell(esp_gsp_handle_t ui,
                           esp_gsp_overlay_builder_t *builder, void *ctx);
static bool intercept_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y,
                              bool pressed, void *ctx);

static void refresh_overlay(esp_gsp_handle_t ui)
{
    (void)esp_gsp_set_overlay_contributor(ui, draw_app_shell, NULL);
}

static void system_notice_timer_cb(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (ui != s_ui) {
        return;
    }
    void *timer = s_notice_timer;
    s_notice_timer = NULL;
    if (timer != NULL) {
        (void)esp_gsp_timer_delete(ui, timer);
    }
    s_notice_visible = false;
    if (s_notice_only) {
        (void)esp_gsp_set_overlay_contributor(ui, NULL, NULL);
        s_notice_only = false;
        s_ui = NULL;
    } else {
        refresh_overlay(ui);
    }
}

static bool actual_root_visible(esp_gsp_handle_t ui)
{
    if (s_root_stack_key == 0) {
        return true;
    }
    uint16_t top = 0;
    return esp_gsp_stack_view_get_top(ui, s_root_stack_key, &top) ==
           ESP_GSP_OK && top == 0;
}

static bool root_visible(esp_gsp_handle_t ui)
{
    const bool actual = actual_root_visible(ui);
    /* A hide override transfers chrome ownership immediately when a child
     * push is queued.  A show override must wait until the stack has really
     * returned to root; otherwise root and child headers overlap throughout
     * the asynchronous pop transition. */
    return s_root_override_valid
        ? (s_root_override_visible && actual) : actual;
}

static void sync_app_shell(esp_gsp_handle_t ui)
{
    const bool actual = actual_root_visible(ui);
    if (s_root_override_valid) {
        if (actual != s_root_override_visible) {
            return;
        }
        s_root_override_valid = false;
    }
    const bool visible = actual;
    if (visible == s_root_visible) {
        return;
    }
    s_root_visible = visible;
    if (!visible) {
        /* A press that began on root chrome must not survive into a child
         * page. Otherwise its release is interpreted as an App exit while
         * the child page owns the top-left back affordance. */
    }
    /* Stack navigation does not otherwise rebuild application-contributed
     * overlay commands. Re-registering the same contributor rebuilds them,
     * removing root chrome on child pages while retaining the home indicator. */
    refresh_overlay(ui);
}

static void poll_app_shell(esp_gsp_handle_t ui, void *ctx)
{
    (void)ctx;
    if (ui == s_ui) {
        if (!s_input_registration_queued) {
            s_input_registration_queued =
                esp_gsp_set_input_interceptor(
                    ui, intercept_pointer, NULL) == ESP_GSP_OK;
        }
        if (!s_overlay_registration_queued) {
            s_overlay_registration_queued =
                esp_gsp_set_overlay_contributor(
                    ui, draw_app_shell, NULL) == ESP_GSP_OK;
        }
        sync_app_shell(ui);
    }
}

static void draw_app_shell(esp_gsp_handle_t ui,
                           esp_gsp_overlay_builder_t *builder, void *ctx)
{
    (void)ctx;
    if (ui != s_ui) {
        return;
    }
    const uint32_t fg = UINT16_C(0xD6BB);
    if (s_root_visible && s_root_header_enabled) {
        /* The orange key cap mirrors the HTML/device top Back key. */
        const int32_t title_y = 26;
        (void)esp_gsp_overlay_builder_round_rect(
            builder, 16, 26, 22, 22, UINT16_C(0xFA60), 4, 255);
        if (s_title != NULL) {
            (void)esp_gsp_overlay_builder_glyph_a8(
                builder, 52, title_y, s_title->width, s_title->height, fg,
                s_title->a8, s_title->size, s_title->stride);
        }
    }
    if (s_bottom_enabled) {
        (void)esp_gsp_overlay_builder_round_rect(
            builder, (VIEWPORT_WIDTH - 58) / 2, 468, 58, 4,
            UINT16_MAX, 2, 96);
    }
    if (s_notice_visible) {
        const char *title = s_notice == MOSAIC_SYSTEM_NOTICE_BATTERY_CRITICAL
            ? "Battery Critically Low" : "Battery Low";
        const char *message = s_notice == MOSAIC_SYSTEM_NOTICE_BATTERY_CRITICAL
            ? "Shutting down" : "Please charge your device";
        const mosaic_app_shell_title_asset_t *title_asset =
            mosaic_app_shell_title_asset(title);
        const mosaic_app_shell_title_asset_t *message_asset =
            mosaic_app_shell_title_asset(message);

        (void)esp_gsp_overlay_builder_round_rect(
            builder, 18, 15, 444, 78, UINT16_C(0x0000), 24, 150);
        (void)esp_gsp_overlay_builder_round_rect(
            builder, 16, 12, 448, 76, UINT16_C(0x39E7), 24, 255);
        (void)esp_gsp_overlay_builder_round_rect(
            builder, 34, 35, 26, 26, UINT16_C(0xFA60), 13, 255);
        (void)esp_gsp_overlay_builder_round_rect(
            builder, 45, 40, 4, 11, UINT16_C(0x39E7), 2, 255);
        (void)esp_gsp_overlay_builder_round_rect(
            builder, 45, 54, 4, 4, UINT16_C(0x39E7), 2, 255);
        if (title_asset != NULL) {
            (void)esp_gsp_overlay_builder_glyph_a8(
                builder, 68, 20, title_asset->width, title_asset->height,
                UINT16_C(0xFA60), title_asset->a8, title_asset->size,
                title_asset->stride);
        }
        if (message_asset != NULL) {
            (void)esp_gsp_overlay_builder_glyph_a8(
                builder, 68, 49, message_asset->width, message_asset->height,
                UINT16_MAX, message_asset->a8, message_asset->size,
                message_asset->stride);
        }
    }
}

static bool intercept_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y,
                              bool pressed, void *ctx)
{
    (void)ctx;
    if (ui != s_ui) {
        return false;
    }
    if (!s_bottom_enabled) {
        s_tracking = false;
        s_exit_triggered = false;
        return false;
    }
    /* Stack navigation may have completed since the previous 16 ms poll.
     * Resolve ownership before classifying this input sample. */
    sync_app_shell(ui);
    if (pressed) {
        if (!s_tracking && x >= EXIT_EDGE_X_MIN && x <= EXIT_EDGE_X_MAX &&
                y >= EXIT_EDGE_Y_MIN) {
            s_tracking = true;
            s_exit_triggered = false;
            s_x0 = x;
            s_y0 = y;
        }
        if (!s_tracking) {
            return false;
        }
    }
    if (!s_tracking) {
        return false;
    }
    const int32_t dy = s_y0 - y;
    const int32_t dx = x > s_x0 ? x - s_x0 : s_x0 - x;
    const bool upward_swipe =
        dy >= EXIT_SWIPE_MIN_PX && dy > dx * EXIT_MAX_DX_RATIO;
    /* Exit only after a real upward drag from the dedicated bottom band.
     * A tap/release in the band and gestures that start elsewhere must pass
     * without navigating. */
    if (upward_swipe && !s_exit_triggered && s_exit != NULL) {
        s_exit_triggered = true;
        s_tracking = false;
        s_exit(s_exit_ctx);
    }
    if (!pressed) {
        s_tracking = false;
        s_exit_triggered = false;
    }
    return true;
}

void mosaic_app_shell_set_exit_handler(
    mosaic_app_shell_exit_fn fn, void *user_ctx)
{
    s_exit = fn;
    s_exit_ctx = user_ctx;
}

void mosaic_app_shell_attach(esp_gsp_handle_t ui, uint32_t root_stack_key,
                             const char *title, bool root_header_enabled)
{
    const mosaic_app_shell_title_asset_t *asset =
        mosaic_app_shell_title_asset(title);
    /* Full-canvas Apps such as Bluetooth intentionally suppress the shared
     * header. They still need the bottom home indicator and its upward-exit
     * interceptor even when no title bitmap was generated for their title. */
    if (ui == NULL) {
        return;
    }
    s_ui = ui;
    s_root_stack_key = root_stack_key;
    s_title = asset;
    s_root_override_valid = false;
    s_root_header_enabled = root_header_enabled;
    s_root_visible = root_visible(ui);
    s_bottom_enabled = true;
    s_tracking = false;
    s_exit_triggered = false;
    s_notice_only = false;
    /* Prepared callbacks run before the render task is fully draining its
     * command queue. Register input first: dropping chrome for one frame is
     * recoverable, dropping the only App-exit route is not. The poll timer
     * retries either command if the startup queue rejected it. */
    s_input_registration_queued =
        esp_gsp_set_input_interceptor(
            ui, intercept_pointer, NULL) == ESP_GSP_OK;
    s_overlay_registration_queued =
        esp_gsp_set_overlay_contributor(
            ui, draw_app_shell, NULL) == ESP_GSP_OK;
    s_chrome_timer = esp_gsp_timer_create(
        ui, CHROME_POLL_MS, poll_app_shell, NULL);
}

void mosaic_app_shell_sync(esp_gsp_handle_t ui)
{
    if (ui != NULL && ui == s_ui) {
        sync_app_shell(ui);
    }
}

void mosaic_app_shell_rearm(esp_gsp_handle_t ui)
{
    if (ui == NULL || ui != s_ui) {
        return;
    }
    s_tracking = false;
    s_exit_triggered = false;
    s_input_registration_queued =
        esp_gsp_set_input_interceptor(
            ui, intercept_pointer, NULL) == ESP_GSP_OK;
    s_overlay_registration_queued =
        esp_gsp_set_overlay_contributor(
            ui, draw_app_shell, NULL) == ESP_GSP_OK;
}

void mosaic_app_shell_set_root_visible(esp_gsp_handle_t ui, bool visible)
{
    if (ui == NULL || ui != s_ui) {
        return;
    }
    s_root_override_valid = true;
    s_root_override_visible = visible;
    const bool effective_visible = visible && actual_root_visible(ui);
    if (effective_visible == s_root_visible) {
        return;
    }
    s_root_visible = effective_visible;
    if (!effective_visible) {
    }
    refresh_overlay(ui);
}

void mosaic_app_shell_set_bottom_enabled(esp_gsp_handle_t ui, bool enabled)
{
    if (ui == NULL || ui != s_ui || enabled == s_bottom_enabled) {
        return;
    }
    s_bottom_enabled = enabled;
    s_tracking = false;
    s_exit_triggered = false;
    refresh_overlay(ui);
}

esp_gsp_err_t mosaic_app_shell_show_system_notice(
    esp_gsp_handle_t ui, mosaic_system_notice_t notice, uint32_t duration_ms)
{
    if (ui == NULL || notice > MOSAIC_SYSTEM_NOTICE_BATTERY_CRITICAL) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    if (s_ui == NULL) {
        /* Full-canvas Apps do not own the shared shell. Temporarily install
         * only the system-notice contributor for those screens. */
        s_ui = ui;
        s_root_visible = false;
        s_root_header_enabled = false;
        s_bottom_enabled = false;
        s_notice_only = true;
    } else if (ui != s_ui) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    if (s_notice_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_notice_timer);
        s_notice_timer = NULL;
    }
    s_notice = notice;
    s_notice_visible = true;
    refresh_overlay(ui);
    if (duration_ms != 0U) {
        s_notice_timer = esp_gsp_timer_create(
            ui, duration_ms, system_notice_timer_cb, NULL);
        if (s_notice_timer == NULL) {
            return ESP_GSP_ERR_NO_MEM;
        }
    }
    return ESP_GSP_OK;
}

void mosaic_app_shell_detach(esp_gsp_handle_t ui)
{
    if (ui == NULL || ui != s_ui) {
        return;
    }
    if (s_chrome_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_chrome_timer);
        s_chrome_timer = NULL;
    }
    if (s_notice_timer != NULL) {
        (void)esp_gsp_timer_delete(ui, s_notice_timer);
        s_notice_timer = NULL;
    }
    (void)esp_gsp_set_input_interceptor(ui, NULL, NULL);
    (void)esp_gsp_set_overlay_contributor(ui, NULL, NULL);
    if (ui == s_ui) {
        s_ui = NULL;
        s_root_stack_key = 0;
        s_title = NULL;
        s_root_visible = false;
        s_root_override_valid = false;
        s_bottom_enabled = false;
        s_input_registration_queued = false;
        s_overlay_registration_queued = false;
        s_tracking = false;
        s_exit_triggered = false;
        s_notice_visible = false;
        s_notice_only = false;
        s_exit = NULL;
        s_exit_ctx = NULL;
    }
}
