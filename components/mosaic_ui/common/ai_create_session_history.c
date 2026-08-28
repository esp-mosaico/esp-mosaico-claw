/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ai_create_session_history.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_memory.h"

static void append_text(char *destination, size_t capacity,
                        const char *text)
{
    if (!destination || capacity == 0 || !text || !text[0]) return;
    size_t used = strlen(destination);
    if (used && used + 1U < capacity) {
        destination[used++] = '\n';
        destination[used] = '\0';
    }
    if (used < capacity - 1U) {
        strlcpy(destination + used, text, capacity - used);
    }
}

static void copy_content(const cJSON *content, char *destination,
                         size_t capacity)
{
    if (cJSON_IsString(content)) {
        strlcpy(destination, content->valuestring, capacity);
        return;
    }
    if (!cJSON_IsArray(content)) return;
    const cJSON *block = NULL;
    cJSON_ArrayForEach(block, content) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
        if (cJSON_IsString(text) &&
            (!cJSON_IsString(type) || strcmp(type->valuestring, "text") == 0)) {
            append_text(destination, capacity, text->valuestring);
        }
    }
}

static void append_message(ai_create_transcript_t *transcript,
                           ai_create_message_role_t role,
                           const cJSON *content)
{
    if (role == AI_CREATE_MESSAGE_USER) {
        if ((transcript->count % 2U) != 0U) {
            char text[AI_CREATE_CONTROLLER_MESSAGE_MAX + 1U] = {0};
            copy_content(content, text, sizeof(text));
            append_text(transcript->messages[transcript->count - 1U].text,
                        sizeof(transcript->messages[0].text), text);
            return;
        }
        if (transcript->count == AI_CREATE_CONTROLLER_HISTORY_MAX) {
            memmove(&transcript->messages[0],
                    &transcript->messages[
                        AI_CREATE_CONTROLLER_MESSAGES_PER_TURN],
                    (AI_CREATE_CONTROLLER_HISTORY_MAX -
                     AI_CREATE_CONTROLLER_MESSAGES_PER_TURN) *
                        sizeof(transcript->messages[0]));
            transcript->count -= AI_CREATE_CONTROLLER_MESSAGES_PER_TURN;
        }
    } else if (transcript->count == 0 ||
               (transcript->count % 2U) == 0U) {
        return;
    }
    ai_create_message_t *message =
        &transcript->messages[transcript->count];
    memset(message, 0, sizeof(*message));
    message->role = role;
    copy_content(content, message->text, sizeof(message->text));
    if (message->text[0]) transcript->count++;
}

static esp_err_t load_history(void *ctx, const char *chat_id,
                              const char *session_alias,
                              ai_create_transcript_t *out_transcript)
{
    (void)ctx;
    if (!chat_id || !chat_id[0] || !session_alias || !session_alias[0] ||
        !out_transcript) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_transcript, 0, sizeof(*out_transcript));

    char *session_id = calloc(1, CLAW_SESSION_MGR_ID_SIZE);
    if (!session_id) return ESP_ERR_NO_MEM;

    size_t session_id_length = 0;
    esp_err_t err = claw_session_mgr_resolve_chat_session_id(
        0, CAP_IM_AI_CREATE_CHANNEL, chat_id, session_alias,
        session_id, CLAW_SESSION_MGR_ID_SIZE, &session_id_length);
    if (err != ESP_OK || session_id_length == 0) {
        free(session_id);
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    char *json = NULL;
    err = claw_memory_load_session_history_json(session_id, &json);
    free(session_id);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }
    const cJSON *record = NULL;
    cJSON_ArrayForEach(record, root) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(record, "role");
        const cJSON *content =
            cJSON_GetObjectItemCaseSensitive(record, "content");
        if (!cJSON_IsString(role)) continue;
        if (strcmp(role->valuestring, "user") == 0) {
            append_message(out_transcript, AI_CREATE_MESSAGE_USER, content);
        } else if (strcmp(role->valuestring, "assistant") == 0) {
            append_message(out_transcript, AI_CREATE_MESSAGE_ASSISTANT, content);
        }
    }
    cJSON_Delete(root);
    return out_transcript->count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static const ai_create_history_ops_t s_history_ops = {
    .load = load_history,
};

const ai_create_history_ops_t *ai_create_session_history_ops(void)
{
    return &s_history_ops;
}
