/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_cli.h"
#include "app_claw.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"

#if CONFIG_APP_CLAW_CAP_LUA
#include "cmd_cap_lua.h"
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
#include "cmd_cap_router_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include "cmd_cap_scheduler.h"
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include "cmd_cap_skill.h"
#endif
#include "claw_cap.h"
#include "claw_agent_mgr.h"
#include "claw_core.h"
#include "cJSON.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_lcd_touch_mux.h"
#include "esp_log.h"
#include "display_service.h"

static const char *TAG = "app_claw_cli";
static const size_t CAP_OUTPUT_BUF_SIZE = 8192;

static uint32_t s_next_request_id = 1;
static char s_current_session_id[64] = "default";

static ssize_t app_claw_cli_read_blocking(int fd, void *buffer, size_t size)
{
    /* TinyUSB stdin is nonblocking; wait for input instead of redrawing the prompt on EAGAIN. */
    for (;;) {
        ssize_t ret = read(fd, buffer, size);
        if (ret >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return ret;
        }
        vTaskDelay(1);
    }
}

#define TOUCH_DEFAULT_HOLD_MS 32U
#define TOUCH_DEFAULT_STEP_MS 16U
#define TOUCH_MAX_DURATION_MS 60000U
#define TOUCH_INJECT_TIMEOUT_MS 1000U

typedef struct {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t duration_ms;
    uint32_t step_ms;
} touch_swipe_request_t;

typedef struct {
    portMUX_TYPE lock;
    TaskHandle_t task;
    touch_swipe_request_t request;
    bool cancel_requested;
    bool reserved;
} touch_playback_state_t;

static touch_playback_state_t s_touch_playback = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static esp_lcd_touch_handle_t s_touch_mux;

static esp_err_t touch_load_mux(void)
{
#if CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT
    void *device_handle = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &device_handle);
    if (err != ESP_OK) {
        return err;
    }
    dev_lcd_touch_handles_t *touch_handles = device_handle;
    if (touch_handles == NULL || touch_handles->touch_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_touch_mux = touch_handles->touch_handle;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static bool parse_touch_uint(const char *text, uint32_t max_value, uint32_t *value)
{
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (text == end || *end != '\0' || parsed > max_value) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_touch_coordinate(const char *text, bool x_axis, int32_t *coordinate)
{
    uint32_t limit = x_axis ? display_service_width() : display_service_height();
    uint32_t value = 0;
    if (limit == 0 || !parse_touch_uint(text, limit - 1, &value)) {
        return false;
    }
    *coordinate = (int32_t)value;
    return true;
}

static esp_err_t touch_push_point(int32_t x, int32_t y)
{
    const esp_lcd_touch_mux_frame_t frame = {
        .count = 1,
        .points = {{.track_id = 0, .x = (uint16_t)x, .y = (uint16_t)y}},
    };
    return esp_lcd_touch_mux_inject(s_touch_mux, &frame, TOUCH_INJECT_TIMEOUT_MS);
}

static esp_err_t touch_tap(int32_t x, int32_t y, uint32_t hold_ms)
{
    esp_err_t err = esp_lcd_touch_mux_begin(s_touch_mux, TOUCH_INJECT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = touch_push_point(x, y);
    }
    if (err == ESP_OK && hold_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(hold_ms));
    }
    if (err == ESP_OK) {
        err = esp_lcd_touch_mux_end(s_touch_mux, TOUCH_INJECT_TIMEOUT_MS);
    } else {
        (void)esp_lcd_touch_mux_cancel(s_touch_mux);
    }
    return err;
}

static bool touch_playback_cancelled(void)
{
    taskENTER_CRITICAL(&s_touch_playback.lock);
    bool cancelled = s_touch_playback.cancel_requested;
    taskEXIT_CRITICAL(&s_touch_playback.lock);
    return cancelled;
}

static void touch_swipe_task(void *arg)
{
    (void)arg;
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    taskENTER_CRITICAL(&s_touch_playback.lock);
    touch_swipe_request_t request = s_touch_playback.request;
    taskEXIT_CRITICAL(&s_touch_playback.lock);
    esp_err_t err = touch_playback_cancelled() ? ESP_ERR_INVALID_STATE :
                    esp_lcd_touch_mux_begin(s_touch_mux, TOUCH_INJECT_TIMEOUT_MS);
    if (err == ESP_OK) {
        err = touch_push_point(request.x1, request.y1);
    }
    uint32_t elapsed = 0;
    while (err == ESP_OK && elapsed + request.step_ms < request.duration_ms) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(request.step_ms)) > 0 || touch_playback_cancelled()) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        elapsed += request.step_ms;
        int32_t x = request.x1 + (int32_t)(((int64_t)request.x2 - request.x1) * elapsed / request.duration_ms);
        int32_t y = request.y1 + (int32_t)(((int64_t)request.y2 - request.y1) * elapsed / request.duration_ms);
        err = touch_push_point(x, y);
    }
    if (err == ESP_OK && !touch_playback_cancelled()) {
        uint32_t remaining_ms = request.duration_ms - elapsed;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remaining_ms)) > 0 || touch_playback_cancelled()) {
            err = ESP_ERR_INVALID_STATE;
        } else {
            err = touch_push_point(request.x2, request.y2);
        }
    }
    if (err == ESP_OK) {
        err = esp_lcd_touch_mux_end(s_touch_mux, TOUCH_INJECT_TIMEOUT_MS);
    } else {
        (void)esp_lcd_touch_mux_cancel(s_touch_mux);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Touch swipe failed: %s", esp_err_to_name(err));
    }
    taskENTER_CRITICAL(&s_touch_playback.lock);
    s_touch_playback.task = NULL;
    s_touch_playback.cancel_requested = false;
    s_touch_playback.reserved = false;
    taskEXIT_CRITICAL(&s_touch_playback.lock);
    vTaskDelete(NULL);
}

static int touch_start_swipe(const touch_swipe_request_t *request)
{
    taskENTER_CRITICAL(&s_touch_playback.lock);
    if (s_touch_playback.reserved) {
        taskEXIT_CRITICAL(&s_touch_playback.lock);
        ESP_LOGE(TAG, "A touch swipe is already running");
        return 1;
    }
    s_touch_playback.reserved = true;
    s_touch_playback.cancel_requested = false;
    s_touch_playback.request = *request;
    taskEXIT_CRITICAL(&s_touch_playback.lock);
    TaskHandle_t task = NULL;
    BaseType_t created = xTaskCreate(touch_swipe_task, "touch_swipe", 3072, NULL, 3, &task);
    if (created != pdPASS) {
        taskENTER_CRITICAL(&s_touch_playback.lock);
        s_touch_playback.reserved = false;
        taskEXIT_CRITICAL(&s_touch_playback.lock);
        ESP_LOGE(TAG, "Failed to create touch swipe task");
        return 1;
    }
    taskENTER_CRITICAL(&s_touch_playback.lock);
    s_touch_playback.task = task;
    xTaskNotifyGive(task);
    taskEXIT_CRITICAL(&s_touch_playback.lock);
    printf("Touch swipe queued\n");
    return 0;
}

static int touch_cancel(void)
{
    taskENTER_CRITICAL(&s_touch_playback.lock);
    s_touch_playback.cancel_requested = true;
    TaskHandle_t task = s_touch_playback.task;
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    taskEXIT_CRITICAL(&s_touch_playback.lock);
    esp_err_t err = esp_lcd_touch_mux_cancel(s_touch_mux);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to cancel touch: %s", esp_err_to_name(err));
        return 1;
    }
    printf("Touch cancelled\n");
    return 0;
}

static void print_touch_usage(void)
{
    printf("Usage:\n"
           "  touch <x> <y>\n"
           "  touch tap <x> <y> [hold_ms]\n"
           "  touch down <x> <y>\n"
           "  touch move <x> <y>\n"
           "  touch up [x y]\n"
           "  touch swipe <x1> <y1> <x2> <y2> <duration_ms> [step_ms]\n"
           "  touch cancel\n");
}

static int cmd_touch(int argc, char **argv)
{
    int32_t x1 = 0;
    int32_t y1 = 0;
    if (argc == 3 && parse_touch_coordinate(argv[1], true, &x1) &&
            parse_touch_coordinate(argv[2], false, &y1)) {
        esp_err_t err = touch_tap(x1, y1, TOUCH_DEFAULT_HOLD_MS);
        if (err == ESP_OK) {
            printf("Touch tap queued at (%ld,%ld)\n", (long)x1, (long)y1);
            return 0;
        }
        ESP_LOGE(TAG, "Touch tap failed: %s", esp_err_to_name(err));
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "cancel") == 0 && argc == 2) {
        return touch_cancel();
    }
    if (argc >= 4 && (strcmp(argv[1], "tap") == 0 || strcmp(argv[1], "down") == 0 ||
            strcmp(argv[1], "move") == 0) &&
            parse_touch_coordinate(argv[2], true, &x1) && parse_touch_coordinate(argv[3], false, &y1)) {
        esp_err_t err = ESP_OK;
        if (strcmp(argv[1], "tap") == 0) {
            uint32_t hold_ms = TOUCH_DEFAULT_HOLD_MS;
            if ((argc != 4 && argc != 5) ||
                    (argc == 5 && !parse_touch_uint(argv[4], TOUCH_MAX_DURATION_MS, &hold_ms))) {
                print_touch_usage();
                return 1;
            }
            err = touch_tap(x1, y1, hold_ms);
        } else if (argc != 4) {
            print_touch_usage();
            return 1;
        } else if (strcmp(argv[1], "down") == 0) {
            err = esp_lcd_touch_mux_begin(s_touch_mux, TOUCH_INJECT_TIMEOUT_MS);
            if (err == ESP_OK) err = touch_push_point(x1, y1);
            if (err != ESP_OK) (void)esp_lcd_touch_mux_cancel(s_touch_mux);
        } else {
            err = touch_push_point(x1, y1);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Touch %s failed: %s", argv[1], esp_err_to_name(err));
            return 1;
        }
        printf("Touch %s queued at (%ld,%ld)\n", argv[1], (long)x1, (long)y1);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "up") == 0 && (argc == 2 || argc == 4)) {
        esp_err_t err = ESP_OK;
        if (argc == 4) {
            if (!parse_touch_coordinate(argv[2], true, &x1) || !parse_touch_coordinate(argv[3], false, &y1)) {
                print_touch_usage();
                return 1;
            }
            err = touch_push_point(x1, y1);
        }
        if (err == ESP_OK) err = esp_lcd_touch_mux_end(s_touch_mux, TOUCH_INJECT_TIMEOUT_MS);
        if (err != ESP_OK) {
            (void)esp_lcd_touch_mux_cancel(s_touch_mux);
            ESP_LOGE(TAG, "Touch up failed: %s", esp_err_to_name(err));
            return 1;
        }
        printf("Touch up queued\n");
        return 0;
    }
    if (argc >= 7 && strcmp(argv[1], "swipe") == 0) {
        touch_swipe_request_t request = {.step_ms = TOUCH_DEFAULT_STEP_MS};
        uint32_t duration_ms = 0;
        if ((argc != 7 && argc != 8) || !parse_touch_coordinate(argv[2], true, &request.x1) ||
                !parse_touch_coordinate(argv[3], false, &request.y1) ||
                !parse_touch_coordinate(argv[4], true, &request.x2) ||
                !parse_touch_coordinate(argv[5], false, &request.y2) ||
                !parse_touch_uint(argv[6], TOUCH_MAX_DURATION_MS, &duration_ms) || duration_ms == 0 ||
                (argc == 8 && (!parse_touch_uint(argv[7], 1000, &request.step_ms) || request.step_ms == 0)) ||
                request.step_ms > duration_ms) {
            print_touch_usage();
            return 1;
        }
        request.duration_ms = duration_ms;
        return touch_start_swipe(&request);
    }
    print_touch_usage();
    return 1;
}

static char *join_args_from(int argc, char **argv, int start_index)
{
    char *prompt = NULL;
    size_t prompt_len = 0;
    int i;

    if (argc <= start_index) {
        return NULL;
    }

    for (i = start_index; i < argc; i++) {
        prompt_len += strlen(argv[i]) + 1;
    }

    prompt = calloc(1, prompt_len + 1);
    if (!prompt) {
        return NULL;
    }

    for (i = start_index; i < argc; i++) {
        if (i > start_index) {
            strcat(prompt, " ");
        }
        strcat(prompt, argv[i]);
    }

    return prompt;
}

static int submit_and_print(const char *prompt, const char *session_id)
{
    claw_core_response_t response = {0};
    uint32_t request_id = 0;
    esp_err_t err;

    if (session_id && session_id[0]) {
        printf("Submitting request %" PRIu32 " [session=%s]...\n",
               s_next_request_id,
               session_id);
    } else {
        printf("Submitting request %" PRIu32 " [single-turn]...\n", s_next_request_id);
    }

    if (!app_claw_get_core()) {
        printf("claw_core is not ready\n");
        return 1;
    }

    err = claw_agent_mgr_start_root_run_text(
        prompt, session_id, CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE,
        5000, &request_id);
    if (err != ESP_OK) {
        printf("submit failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    s_next_request_id = request_id + 1;

    err = claw_agent_mgr_receive_root_for(request_id, &response, 130000);
    if (err != ESP_OK) {
        printf("receive failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (response.status == CLAW_CORE_RESPONSE_STATUS_OK && response.text) {
        printf("\nassistant> %s\n\n", response.text);
    } else {
        printf("\nerror> %s\n\n",
               response.error_message ? response.error_message : "unknown error");
    }

    claw_core_response_free(&response);
    return 0;
}

static int cmd_ask(int argc, char **argv)
{
    char *prompt = NULL;
    const char *session_id = s_current_session_id;
    int prompt_index = 1;
    int rc;

    if (argc > 1 && strcmp(argv[1], "--once") == 0) {
        session_id = NULL;
        prompt_index++;
    }
    if (argc <= prompt_index) {
        printf("Usage: ask [--once] <prompt>\n");
        return 1;
    }

    prompt = join_args_from(argc, argv, prompt_index);
    if (!prompt) {
        printf("Out of memory\n");
        return 1;
    }

    rc = submit_and_print(prompt, session_id);
    free(prompt);
    return rc;
}

static int cmd_session(int argc, char **argv)
{
    if (argc == 1) {
        printf("Current session: %s\n", s_current_session_id);
        return 0;
    }

    if (argc != 2) {
        printf("Usage: session [id]\n");
        return 1;
    }

    if (argv[1][0] == '\0') {
        printf("session id cannot be empty\n");
        return 1;
    }

    strlcpy(s_current_session_id, argv[1], sizeof(s_current_session_id));
    printf("Switched session to: %s\n", s_current_session_id);
    return 0;
}

static int cmd_cap_list(int argc, char **argv)
{
    claw_cap_list_t list;
    size_t i;

    (void)argc;
    (void)argv;

    list = claw_cap_list();
    if (list.count == 0) {
        printf("No capabilities registered\n");
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        const claw_cap_descriptor_t *item = &list.items[i];

        printf("%s [%s] %s\n",
               item->name,
               item->family ? item->family : "cap",
               item->description ? item->description : "");
    }

    return 0;
}

static int cmd_cap_call(int argc, char **argv)
{
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
        .session_id = s_current_session_id,
        .core = app_claw_get_core(),
    };

    if (argc < 3) {
        printf("Usage: cap_call <name> <json>\n");
        return 1;
    }

    {
        cJSON *json = cJSON_Parse(argv[2]);

        if (!json) {
            printf("invalid json\n");
            return 1;
        }
        cJSON_Delete(json);
    }

    output = calloc(1, CAP_OUTPUT_BUF_SIZE);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(argv[1], argv[2], &ctx, output, CAP_OUTPUT_BUF_SIZE);
    if (err == ESP_OK) {
        printf("%s\n", output);
    } else {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    }

    free(output);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_cap_groups(int argc, char **argv)
{
    claw_cap_group_list_t list;
    size_t i;

    (void)argc;
    (void)argv;

    list = claw_cap_list_groups();
    if (list.count == 0) {
        printf("No cap groups loaded\n");
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        const claw_cap_group_info_t *item = &list.items[i];

        printf("%s state=%s descriptors=%u plugin=%s version=%s\n",
               item->group_id ? item->group_id : "(null)",
               claw_cap_state_to_string(item->state),
               (unsigned)item->descriptor_count,
               item->plugin_name ? item->plugin_name : "-",
               item->version ? item->version : "-");
    }

    return 0;
}

static int cmd_cap(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cap <list|call|groups> ...\n");
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        return cmd_cap_list(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "call") == 0) {
        return cmd_cap_call(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "groups") == 0) {
        return cmd_cap_groups(argc - 1, &argv[1]);
    }
    printf("Unknown cap subcommand: %s\n", argv[1]);
    printf("Usage: cap <list|call|groups> ...\n");
    return 1;
}

#if CONFIG_APP_CLAW_CAP_FILES
static int cmd_ls(int argc, char **argv)
{
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
        .session_id = s_current_session_id,
        .core = app_claw_get_core(),
    };
    cJSON *input = NULL;
    char *input_json = NULL;
    char *output = NULL;
    esp_err_t err;

    if (argc > 2) {
        printf("Usage: ls [keyword]\n");
        return 1;
    }

    input = cJSON_CreateObject();
    if (!input || (argc == 2 && !cJSON_AddStringToObject(input, "keyword", argv[1]))) {
        cJSON_Delete(input);
        printf("Out of memory\n");
        return 1;
    }
    input_json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    output = calloc(1, CAP_OUTPUT_BUF_SIZE);
    if (!input_json || !output) {
        free(input_json);
        free(output);
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call("list_dir", input_json, &ctx, output, CAP_OUTPUT_BUF_SIZE);
    printf("%s\n", output[0] ? output : esp_err_to_name(err));
    free(input_json);
    free(output);
    return err == ESP_OK ? 0 : 1;
}
#endif

static void register_cap_cli_commands(void)
{
#if CONFIG_APP_CLAW_CAP_LUA
    register_cap_lua();
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
    register_cap_router_mgr();
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    register_cap_scheduler();
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    register_cap_skill();
#endif
}

esp_err_t app_claw_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "Starting console REPL");

    repl_config.prompt = "app> ";
    repl_config.task_stack_size = 10240;
    repl_config.max_cmdline_length = 512;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    linenoiseSetReadFunction(app_claw_cli_read_blocking);

    register_cap_cli_commands();

    esp_err_t touch_err = touch_load_mux();
    if (touch_err == ESP_OK) {
        const esp_console_cmd_t touch_cmd = {
            .command = "touch",
            .help = "Inject touchscreen tap/down/move/up/swipe/cancel",
            .func = cmd_touch,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&touch_cmd));
    } else {
        ESP_LOGW(TAG, "Touch injection unavailable: %s", esp_err_to_name(touch_err));
    }

    {
        esp_console_cmd_t ask_cmd = {
            .command = "ask",
            .help = "Submit a prompt using the current session, or use --once for a single turn",
            .func = cmd_ask,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ask_cmd));
    }

    {
        esp_console_cmd_t session_cmd = {
            .command = "session",
            .help = "Show or switch the current session: session [id]",
            .func = cmd_session,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&session_cmd));
    }

    {
        esp_console_cmd_t cap_cmd = {
            .command = "cap",
            .help = "Capability operations: cap <list|call|groups> ...",
            .func = cmd_cap,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cap_cmd));
    }

#if CONFIG_APP_CLAW_CAP_FILES
    {
        esp_console_cmd_t ls_cmd = {
            .command = "ls",
            .help = "List files under DATA and SYSTEM roots, optionally filtered by keyword",
            .func = cmd_ls,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ls_cmd));
    }
#endif

    printf("Type 'help' to list commands, or 'ls [keyword]' to list files\n");
    return esp_console_start_repl(repl);
}
