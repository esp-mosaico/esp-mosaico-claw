/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ai_create_voice_port.h"
#include "cap_im_ai_create.h"
#include "claw_session_mgr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AI_CREATE_CONTROLLER_TEXT_MAX CAP_IM_AI_CREATE_TEXT_MAX_BYTES
#define AI_CREATE_CONTROLLER_CHAT_ID_MAX 95U
#define AI_CREATE_CONTROLLER_HISTORY_TURNS_MAX 5U
#define AI_CREATE_CONTROLLER_MESSAGES_PER_TURN 2U
#define AI_CREATE_CONTROLLER_HISTORY_MAX \
    (AI_CREATE_CONTROLLER_HISTORY_TURNS_MAX * \
     AI_CREATE_CONTROLLER_MESSAGES_PER_TURN)
#define AI_CREATE_CONTROLLER_MESSAGE_MAX 511U
#define AI_CREATE_CONTROLLER_VOICE_PREPARING_TIMEOUT_US 30000000LL
#define AI_CREATE_CONTROLLER_VOICE_LISTENING_TIMEOUT_US 60000000LL
#define AI_CREATE_CONTROLLER_VOICE_FINALIZING_TIMEOUT_US 30000000LL

typedef struct ai_create_controller_t *ai_create_controller_handle_t;

typedef enum {
    AI_CREATE_STATE_IDLE = 0,
    AI_CREATE_STATE_DRAFTING,
    AI_CREATE_STATE_RECORDING,
    AI_CREATE_STATE_TRANSCRIBING,
    AI_CREATE_STATE_READY,
    AI_CREATE_STATE_SUBMITTING,
    AI_CREATE_STATE_RUNNING,
    AI_CREATE_STATE_COMPLETED,
    AI_CREATE_STATE_ERROR,
    AI_CREATE_STATE_CANCELLING,
    AI_CREATE_STATE_CANCELLED,
} ai_create_controller_state_t;

typedef enum {
    AI_CREATE_VOICE_STATE_IDLE = 0,
    AI_CREATE_VOICE_STATE_PREPARING,
    AI_CREATE_VOICE_STATE_LISTENING,
    AI_CREATE_VOICE_STATE_FINALIZING,
    AI_CREATE_VOICE_STATE_CANCELLING,
    AI_CREATE_VOICE_STATE_ERROR,
} ai_create_voice_state_t;

typedef enum {
    AI_CREATE_SCREEN_WELCOME = 0,
    AI_CREATE_SCREEN_CHAT,
    AI_CREATE_SCREEN_SESSION_PICKER,
} ai_create_screen_t;

typedef enum {
    AI_CREATE_COMPOSER_DEFAULT = 0,
    AI_CREATE_COMPOSER_MODE,
} ai_create_composer_t;

typedef enum {
    AI_CREATE_COMMAND_OPEN_SESSION_PICKER = 0,
    AI_CREATE_COMMAND_CLOSE_SESSION_PICKER,
    AI_CREATE_COMMAND_NEW_SESSION,
    AI_CREATE_COMMAND_SELECT_SESSION,
    AI_CREATE_COMMAND_REQUEST_DELETE_SESSION,
    AI_CREATE_COMMAND_CANCEL_DELETE_SESSION,
    AI_CREATE_COMMAND_SELECT_MODE,
    AI_CREATE_COMMAND_CLEAR_MODE,
    AI_CREATE_COMMAND_LEAVE_CHAT,
    AI_CREATE_COMMAND_RETRY,
    AI_CREATE_COMMAND_DISMISS_NOTICE,
} ai_create_command_type_t;

typedef struct {
    ai_create_command_type_t type;
    union {
        cap_im_ai_create_mode_t mode;
        size_t session_index;
        char session_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    } data;
} ai_create_command_t;

typedef struct {
    esp_err_t (*subscribe)(cap_im_ai_create_event_cb_t callback, void *ctx);
    esp_err_t (*unsubscribe)(cap_im_ai_create_event_cb_t callback, void *ctx);
    esp_err_t (*post_message)(const cap_im_ai_create_request_t *request);
    esp_err_t (*cancel)(uint32_t request_id);
} ai_create_gateway_ops_t;

typedef enum {
    AI_CREATE_MESSAGE_USER = 0,
    AI_CREATE_MESSAGE_ASSISTANT,
} ai_create_message_role_t;

typedef struct {
    uint32_t request_id;
    ai_create_message_role_t role;
    bool pending;
    bool error;
    char text[AI_CREATE_CONTROLLER_MESSAGE_MAX + 1U];
} ai_create_message_t;

typedef struct {
    size_t count;
    ai_create_message_t messages[AI_CREATE_CONTROLLER_HISTORY_MAX];
} ai_create_transcript_t;

typedef struct {
    esp_err_t (*load)(void *ctx, const char *chat_id,
                      const char *session_alias,
                      ai_create_transcript_t *out_transcript);
} ai_create_history_ops_t;

typedef void (*ai_create_controller_changed_cb_t)(
    ai_create_controller_handle_t controller, void *user_ctx);

typedef struct {
    const ai_create_gateway_ops_t *gateway;
    const ai_create_history_ops_t *history;
    void *history_ctx;
    ai_create_voice_port_handle_t voice_port;
    /* Used when voice_port is NULL. Zero initialization means disabled. */
    ai_create_voice_status_t voice_status;
    const char *chat_id;
    /* NULL or empty starts in a pending-new state. A non-empty alias resumes
     * that session when the controller starts. */
    const char *session_alias;
    bool cancel_on_stop;
    bool persist_journal;
    ai_create_controller_changed_cb_t on_changed;
    void *on_changed_ctx;
} ai_create_controller_config_t;

typedef struct {
    uint32_t revision;
    ai_create_screen_t screen;
    ai_create_composer_t composer;
    ai_create_controller_state_t state;
    ai_create_voice_state_t voice_state;
    uint32_t voice_operation_id;
    cap_im_ai_create_mode_t mode;
    cap_im_ai_create_status_t status;
    uint32_t request_id;
    bool active;
    bool retryable;
    bool terminal;
    bool new_session_pending;
    bool session_loading;
    bool session_delete_confirm_visible;
    bool session_deleting;
    ai_create_voice_status_t voice_status;
    bool notice_visible;
    esp_err_t notice_code;
    char chat_id[AI_CREATE_CONTROLLER_CHAT_ID_MAX + 1U];
    char session_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    char delete_session_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1U];
    char input[AI_CREATE_CONTROLLER_TEXT_MAX + 1U];
    char response[AI_CREATE_CONTROLLER_TEXT_MAX + 1U];
    char progress[256];
    char error[256];
    char notice[160];
    ai_create_transcript_t transcript;
} ai_create_controller_snapshot_t;

esp_err_t ai_create_controller_create(const ai_create_controller_config_t *config,
    ai_create_controller_handle_t *ret_controller);
void ai_create_controller_delete(ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_start(ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_stop(ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_set_voice_port(
    ai_create_controller_handle_t controller,
    ai_create_voice_port_handle_t voice_port,
    ai_create_voice_status_t fallback_status);

esp_err_t ai_create_controller_set_mode(ai_create_controller_handle_t controller,
    cap_im_ai_create_mode_t mode);
esp_err_t ai_create_controller_set_session(ai_create_controller_handle_t controller,
    const char *chat_id, const char *session_alias);
esp_err_t ai_create_controller_post_text(ai_create_controller_handle_t controller,
    const char *text);
esp_err_t ai_create_controller_cancel(ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_abort_current(
    ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_retry(ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_voice_start(ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_voice_stop(ai_create_controller_handle_t controller,
    bool send);
esp_err_t ai_create_controller_voice_cancel(
    ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_get_voice_loudness(
    ai_create_controller_handle_t controller, uint8_t *out_level);

esp_err_t ai_create_controller_list_sessions(ai_create_controller_handle_t controller,
    claw_session_mgr_alias_map_t *out_sessions);
esp_err_t ai_create_controller_new_session(ai_create_controller_handle_t controller,
    const char *requested_alias);
esp_err_t ai_create_controller_switch_session(ai_create_controller_handle_t controller,
    const char *alias);
/* Starts a non-blocking UI transition to a cached session. The returned
 * generation makes a worker completion harmless after navigation changes. */
esp_err_t ai_create_controller_begin_session_switch(
    ai_create_controller_handle_t controller, uint32_t *out_generation);
esp_err_t ai_create_controller_complete_session_switch(
    ai_create_controller_handle_t controller, const char *alias,
    uint32_t generation);
void ai_create_controller_cancel_session_switch(
    ai_create_controller_handle_t controller);
/* A delete is confirmed on the UI task, queued by the runtime, and completed
 * on the session worker. The generation keeps stale worker completions from
 * mutating a newer confirmation flow. */
esp_err_t ai_create_controller_begin_session_delete(
    ai_create_controller_handle_t controller,
    char *out_alias, size_t out_alias_size, uint32_t *out_generation);
esp_err_t ai_create_controller_complete_session_delete(
    ai_create_controller_handle_t controller, const char *alias,
    uint32_t generation);
void ai_create_controller_fail_session_delete(
    ai_create_controller_handle_t controller, uint32_t generation,
    esp_err_t error);

esp_err_t ai_create_controller_get_snapshot(ai_create_controller_handle_t controller,
    ai_create_controller_snapshot_t *out_snapshot);
esp_err_t ai_create_controller_get_revision(
    ai_create_controller_handle_t controller, uint32_t *out_revision);
esp_err_t ai_create_controller_reload_history(
    ai_create_controller_handle_t controller);
esp_err_t ai_create_controller_step(ai_create_controller_handle_t controller,
    int64_t now_us);
esp_err_t ai_create_controller_dispatch(ai_create_controller_handle_t controller,
    const ai_create_command_t *command);
const char *ai_create_controller_state_name(ai_create_controller_state_t state);
const ai_create_gateway_ops_t *ai_create_controller_default_gateway(void);

#ifdef __cplusplus
}
#endif
