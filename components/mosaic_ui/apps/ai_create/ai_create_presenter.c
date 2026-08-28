/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ai_create_presenter.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_create_binds.h"
#include "ai_create_objects.h"

struct ai_create_presenter_t {
    uint32_t rendered_revision;
    claw_session_mgr_alias_map_t rendered_sessions;
    size_t rendered_session_page;
    ai_create_transcript_t transcript;
    esp_gsp_list_t message_list;
    esp_gsp_list_t voice_message_list;
    uint32_t transcript_revision;
    uint32_t message_list_revision;
    uint32_t voice_message_list_revision;
    int64_t rendered_deciseconds;
    int64_t next_wave_update_us;
    uint8_t loudness_history[15];
    uint8_t rendered_bar_heights[15];
    gsp_component_property_update_t
        wave_updates[ESP_GSP_COMPONENT_BATCH_MAX];
    ai_create_voice_state_t rendered_voice_state;
    ai_create_screen_t rendered_screen;
    ai_create_composer_t rendered_composer;
    cap_im_ai_create_mode_t rendered_mode;
    bool rendered_notice_visible;
    char rendered_notice[160];
    ai_create_voice_status_t rendered_voice_status;
    bool rendered_voice_visible;
    bool rendered_has_messages;
    bool sessions_valid;
    bool valid;
};

#define AI_VOICE_BAR_COUNT              15U
#define AI_VOICE_WAVE_CENTER_Y          354
#define AI_VOICE_WAVE_UPDATE_INTERVAL_US 66000
#define AI_VOICE_BAR_MIN_HEIGHT         6
#define AI_VOICE_BAR_DEADBAND            2
#define AI_SESSION_DOT_WIDTH             10
#define AI_SESSION_DOT_SPACING           20
#define AI_SESSION_DOT_Y                 364
#define AI_SESSION_CONTENT_WIDTH         480

static const gsp_component_key_t s_voice_bar_keys[AI_VOICE_BAR_COUNT] = {
    GSP_OBJ_KEY_AI_VOICE_BAR_00, GSP_OBJ_KEY_AI_VOICE_BAR_01,
    GSP_OBJ_KEY_AI_VOICE_BAR_02, GSP_OBJ_KEY_AI_VOICE_BAR_03,
    GSP_OBJ_KEY_AI_VOICE_BAR_04, GSP_OBJ_KEY_AI_VOICE_BAR_05,
    GSP_OBJ_KEY_AI_VOICE_BAR_06, GSP_OBJ_KEY_AI_VOICE_BAR_07,
    GSP_OBJ_KEY_AI_VOICE_BAR_08, GSP_OBJ_KEY_AI_VOICE_BAR_09,
    GSP_OBJ_KEY_AI_VOICE_BAR_10, GSP_OBJ_KEY_AI_VOICE_BAR_11,
    GSP_OBJ_KEY_AI_VOICE_BAR_12, GSP_OBJ_KEY_AI_VOICE_BAR_13,
    GSP_OBJ_KEY_AI_VOICE_BAR_14,
};

static const uint8_t s_voice_bar_max_height[AI_VOICE_BAR_COUNT] = {
    18, 22, 28, 36, 44, 52, 58, 64, 58, 52, 44, 36, 28, 22, 18,
};

static const gsp_component_key_t s_session_dot_state_keys[] = {
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_0,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_1,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_2,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_3,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_4,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_5,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_6,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_7,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_8,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_9,
    GSP_OBJ_KEY_AI_SESSION_DOT_STATE_10,
};

static const gsp_component_key_t s_session_dot_keys[] = {
    GSP_OBJ_KEY_AI_SESSION_DOT_0,
    GSP_OBJ_KEY_AI_SESSION_DOT_1,
    GSP_OBJ_KEY_AI_SESSION_DOT_2,
    GSP_OBJ_KEY_AI_SESSION_DOT_3,
    GSP_OBJ_KEY_AI_SESSION_DOT_4,
    GSP_OBJ_KEY_AI_SESSION_DOT_5,
    GSP_OBJ_KEY_AI_SESSION_DOT_6,
    GSP_OBJ_KEY_AI_SESSION_DOT_7,
    GSP_OBJ_KEY_AI_SESSION_DOT_8,
    GSP_OBJ_KEY_AI_SESSION_DOT_9,
    GSP_OBJ_KEY_AI_SESSION_DOT_10,
};

_Static_assert(sizeof(s_session_dot_state_keys) /
                       sizeof(s_session_dot_state_keys[0]) ==
                   AI_CREATE_SESSION_MAX_PAGES,
               "AI Create session dot states must cover all pages");
_Static_assert(sizeof(s_session_dot_keys) /
                       sizeof(s_session_dot_keys[0]) ==
                   AI_CREATE_SESSION_MAX_PAGES,
               "AI Create session dots must cover all pages");
/* Newest sample starts at the centre. Older measured samples alternate left
 * and right, so every bar represents its own point in the recent envelope. */
static const uint8_t s_voice_bar_history_order[AI_VOICE_BAR_COUNT] = {
    7, 6, 8, 5, 9, 4, 10, 3, 11, 2, 12, 1, 13, 0, 14,
};

static esp_err_t first_error(esp_err_t current, esp_err_t next)
{
    return current == ESP_OK ? next : current;
}

static gsp_component_update_t visibility_update(
    gsp_component_key_t component, bool visible)
{
    return (gsp_component_update_t) {
        .key = component,
        .prop = GSP_COMPONENT_PROP_VISIBLE,
        .value = {.type = GSP_VALUE_BOOL, .data.boolean = visible},
    };
}

static gsp_component_property_update_t bool_property_update(
    gsp_component_key_t component, gsp_property_key_t property, bool value)
{
    return (gsp_component_property_update_t) {
        .component = component,
        .property = property,
        .value = {.type = GSP_VALUE_BOOL, .data.boolean = value},
    };
}

static gsp_component_property_update_t i32_property_update(
    gsp_component_key_t component, gsp_property_key_t property, int32_t value)
{
    return (gsp_component_property_update_t) {
        .component = component,
        .property = property,
        .value = {.type = GSP_VALUE_I32, .data.i32 = value},
    };
}

static gsp_component_property_update_t color_property_update(
    gsp_component_key_t component, uint32_t color)
{
    return (gsp_component_property_update_t) {
        .component = component,
        .property = GSP_PROP_KEY_COLOR,
        .value = {.type = GSP_VALUE_COLOR, .data.color = color},
    };
}

static esp_err_t set_component_visibility_many(
    esp_gsp_handle_t ui, const gsp_component_update_t *updates, size_t count)
{
    size_t offset = 0;
    while (offset < count) {
        const size_t remaining = count - offset;
        const size_t chunk = remaining < ESP_GSP_COMPONENT_BATCH_MAX
            ? remaining : ESP_GSP_COMPONENT_BATCH_MAX;
        const esp_err_t err = esp_gsp_component_set_many(
            ui, updates + offset, chunk);
        if (err != ESP_OK) return err;
        offset += chunk;
    }
    return ESP_OK;
}

static esp_err_t flush_component_properties(
    esp_gsp_handle_t ui, gsp_component_property_update_t *updates,
    size_t *count)
{
    if (*count == 0U) return ESP_OK;
    const esp_err_t err = esp_gsp_component_set_properties(
        ui, updates, *count);
    *count = 0U;
    return err;
}

static esp_err_t append_component_property(
    esp_gsp_handle_t ui, gsp_component_property_update_t *updates,
    size_t *count, gsp_component_property_update_t update)
{
    if (*count == ESP_GSP_COMPONENT_BATCH_MAX) {
        const esp_err_t err = flush_component_properties(ui, updates, count);
        if (err != ESP_OK) return err;
    }
    updates[(*count)++] = update;
    return ESP_OK;
}

typedef struct {
    const char *title;
    const char *prompt;
} mode_copy_t;

static const mode_copy_t s_modes[] = {
    [CAP_IM_AI_CREATE_MODE_CREATE] = {
        "AI Create", "Hold to talk: describe your device…"},
    [CAP_IM_AI_CREATE_MODE_SKILL] = {
        "Install Skill", "Hold to talk: describe a Skill to install…"},
    [CAP_IM_AI_CREATE_MODE_MEMORY] = {
        "Remember", "Hold to talk: say what to remember…"},
    [CAP_IM_AI_CREATE_MODE_PLAN] = {
        "Plan", "Hold to talk: describe your goal…"},
    [CAP_IM_AI_CREATE_MODE_QUICK] = {
        "Quick Ask", "Hold to talk: ask your question…"},
    [CAP_IM_AI_CREATE_MODE_SCHEDULE] = {
        "Schedule", "Hold to talk: give a time and task…"},
};

static uint16_t feature_page_for_mode(cap_im_ai_create_mode_t mode)
{
    switch (mode) {
    case CAP_IM_AI_CREATE_MODE_QUICK:
    case CAP_IM_AI_CREATE_MODE_CREATE:
    case CAP_IM_AI_CREATE_MODE_SKILL:
        return 0U;
    case CAP_IM_AI_CREATE_MODE_MEMORY:
    case CAP_IM_AI_CREATE_MODE_PLAN:
    case CAP_IM_AI_CREATE_MODE_SCHEDULE:
        return 1U;
    default:
        return 0U;
    }
}

static const char *voice_unavailable_copy(ai_create_voice_readiness_t readiness)
{
    switch (readiness) {
    case AI_CREATE_VOICE_READINESS_DISABLED:
        return "Set up speech recognition first";
    case AI_CREATE_VOICE_READINESS_INITIALIZING:
        return "Starting voice service…";
    case AI_CREATE_VOICE_READINESS_OFFLINE:
        return "Network offline; hold to retry";
    case AI_CREATE_VOICE_READINESS_AUTH_FAILED:
        return "Invalid ASR credentials";
    case AI_CREATE_VOICE_READINESS_AUDIO_ERROR:
        return "Microphone unavailable";
    case AI_CREATE_VOICE_READINESS_BUSY:
        return "Voice service busy; try later";
    case AI_CREATE_VOICE_READINESS_PROVIDER_ERROR:
        return "Voice service error; hold to retry";
    case AI_CREATE_VOICE_READINESS_READY:
    default:
        return "Hold to talk, release to send…";
    }
}

static const char *voice_notice_copy(ai_create_voice_readiness_t readiness)
{
    switch (readiness) {
    case AI_CREATE_VOICE_READINESS_DISABLED:
        return "Speech recognition is not configured. Complete ASR setup first.";
    case AI_CREATE_VOICE_READINESS_INITIALIZING:
        return "The voice service is starting. Try again shortly.";
    case AI_CREATE_VOICE_READINESS_OFFLINE:
        return "Cannot connect to the voice service. Check the network and retry.";
    case AI_CREATE_VOICE_READINESS_AUTH_FAILED:
        return "Invalid ASR credentials. Check the speech recognition settings.";
    case AI_CREATE_VOICE_READINESS_AUDIO_ERROR:
        return "The microphone is unavailable. Check the audio device.";
    case AI_CREATE_VOICE_READINESS_BUSY:
        return "The voice service is busy. Try again shortly.";
    case AI_CREATE_VOICE_READINESS_PROVIDER_ERROR:
        return "The voice service is temporarily unavailable. Try again later.";
    case AI_CREATE_VOICE_READINESS_READY:
    default:
        return "The voice operation could not be completed. Please retry.";
    }
}

static bool voice_visible(const ai_create_controller_snapshot_t *model)
{
    return model->voice_state == AI_CREATE_VOICE_STATE_PREPARING ||
           model->voice_state == AI_CREATE_VOICE_STATE_LISTENING ||
           model->voice_state == AI_CREATE_VOICE_STATE_FINALIZING ||
           model->voice_state == AI_CREATE_VOICE_STATE_CANCELLING;
}

static const char *voice_hint(const ai_create_controller_snapshot_t *model)
{
    switch (model->voice_state) {
    case AI_CREATE_VOICE_STATE_PREPARING:
        return "Starting recording and voice service…";
    case AI_CREATE_VOICE_STATE_FINALIZING:
        return "Recognizing and sending…";
    case AI_CREATE_VOICE_STATE_CANCELLING:
        return "Cancelling…";
    case AI_CREATE_VOICE_STATE_ERROR:
        return model->error[0] ? model->error : "Speech recognition failed";
    default:
        return "Release: send · Swipe up: cancel";
    }
}

static void push_loudness_sample(ai_create_presenter_handle_t presenter,
                                 uint8_t loudness)
{
    memmove(&presenter->loudness_history[1],
            &presenter->loudness_history[0],
            AI_VOICE_BAR_COUNT - 1U);
    presenter->loudness_history[0] = loudness;
}

static void build_voice_bar_heights(
    const ai_create_presenter_handle_t presenter,
    ai_create_voice_state_t state,
    uint8_t heights[AI_VOICE_BAR_COUNT])
{
    memset(heights, AI_VOICE_BAR_MIN_HEIGHT, AI_VOICE_BAR_COUNT);
    if (state == AI_CREATE_VOICE_STATE_PREPARING) {
        for (size_t index = 0; index < AI_VOICE_BAR_COUNT; ++index) {
            heights[index] = (uint8_t)(AI_VOICE_BAR_MIN_HEIGHT +
                (s_voice_bar_max_height[index] -
                 AI_VOICE_BAR_MIN_HEIGHT) / 8U);
        }
        return;
    }
    if (state != AI_CREATE_VOICE_STATE_LISTENING) return;

    for (size_t age = 0; age < AI_VOICE_BAR_COUNT; ++age) {
        const size_t bar = s_voice_bar_history_order[age];
        const uint32_t range = s_voice_bar_max_height[bar] -
            AI_VOICE_BAR_MIN_HEIGHT;
        heights[bar] = (uint8_t)(AI_VOICE_BAR_MIN_HEIGHT +
            (presenter->loudness_history[age] * range + 127U) / 255U);
    }
}

static esp_err_t flush_voice_bar_updates(
    esp_gsp_handle_t ui,
    gsp_component_property_update_t updates[ESP_GSP_COMPONENT_BATCH_MAX],
    size_t *count)
{
    if (*count == 0) return ESP_OK;
    esp_err_t err = esp_gsp_component_set_properties(ui, updates, *count);
    *count = 0;
    return err;
}

static void render_voice_wave(ai_create_presenter_handle_t presenter,
                              esp_gsp_handle_t ui,
                              ai_create_voice_state_t state,
                              uint8_t loudness,
                              int64_t now_us, bool force)
{
    if (!force && now_us < presenter->next_wave_update_us) return;

    if (state == AI_CREATE_VOICE_STATE_LISTENING) {
        push_loudness_sample(presenter, loudness);
    }

    uint8_t heights[AI_VOICE_BAR_COUNT];
    build_voice_bar_heights(presenter, state, heights);
    gsp_component_property_update_t *updates = presenter->wave_updates;
    size_t update_count = 0;
    bool submitted = true;
    for (size_t index = 0; index < AI_VOICE_BAR_COUNT; ++index) {
        const int32_t previous = presenter->rendered_bar_heights[index];
        const int32_t height = heights[index];
        const int32_t delta = previous > height
            ? previous - height : height - previous;
        if (!force && delta < AI_VOICE_BAR_DEADBAND) continue;

        updates[update_count++] = (gsp_component_property_update_t) {
            .component = s_voice_bar_keys[index],
            .property = GSP_PROP_KEY_HEIGHT,
            .value = {.type = GSP_VALUE_I32, .data.i32 = height},
        };
        updates[update_count++] = (gsp_component_property_update_t) {
            .component = s_voice_bar_keys[index],
            .property = GSP_PROP_KEY_Y,
            .value = {
                .type = GSP_VALUE_I32,
                .data.i32 = AI_VOICE_WAVE_CENTER_Y - height / 2,
            },
        };
        if (update_count == ESP_GSP_COMPONENT_BATCH_MAX) {
            if (flush_voice_bar_updates(ui, updates, &update_count) != ESP_OK) {
                submitted = false;
            }
        }
    }
    if (flush_voice_bar_updates(ui, updates, &update_count) != ESP_OK) {
        submitted = false;
    }
    if (submitted) {
        memcpy(presenter->rendered_bar_heights, heights, sizeof(heights));
    } else {
        memset(presenter->rendered_bar_heights, 0,
               sizeof(presenter->rendered_bar_heights));
    }
    presenter->next_wave_update_us = now_us +
        AI_VOICE_WAVE_UPDATE_INTERVAL_US;
}

static esp_err_t render_sessions(esp_gsp_handle_t ui,
    const claw_session_mgr_alias_map_t *sessions, size_t session_page)
{
    static const uint16_t titles[] = {
        GSP_BIND_AI_SESSION_0_TITLE, GSP_BIND_AI_SESSION_1_TITLE,
        GSP_BIND_AI_SESSION_2_TITLE,
    };
    static const uint16_t visible[] = {
        GSP_BIND_AI_SESSION_0_VISIBLE, GSP_BIND_AI_SESSION_1_VISIBLE,
        GSP_BIND_AI_SESSION_2_VISIBLE,
    };
    _Static_assert(sizeof(titles) / sizeof(titles[0]) ==
                       AI_CREATE_SESSION_PAGE_SIZE,
                   "AI Create session page title binding count mismatch");
    _Static_assert(sizeof(visible) / sizeof(visible[0]) ==
                       AI_CREATE_SESSION_PAGE_SIZE,
                   "AI Create session page visibility binding count mismatch");
    char count[64];
    const size_t session_count = sessions ? sessions->session_count : 0U;
    const size_t page_count = (session_count + AI_CREATE_SESSION_PAGE_SIZE - 1U) /
                              AI_CREATE_SESSION_PAGE_SIZE;
    if (page_count > 1U) {
        snprintf(count, sizeof(count), "%u sessions · Swipe left/right · %u/%u",
                 (unsigned)session_count, (unsigned)(session_page + 1U),
                 (unsigned)page_count);
    } else {
        snprintf(count, sizeof(count), "%u sessions", (unsigned)session_count);
    }
    esp_err_t result = esp_gsp_set_text(
        ui, GSP_BIND_AI_SESSION_COUNT, count);
    const bool show_dots = page_count > 1U;
    const int32_t dots_width = page_count > 0U
        ? (int32_t)((page_count - 1U) * AI_SESSION_DOT_SPACING +
                    AI_SESSION_DOT_WIDTH)
        : 0;
    const int32_t dots_left = (AI_SESSION_CONTENT_WIDTH - dots_width) / 2;
    gsp_component_property_update_t
        dot_updates[ESP_GSP_COMPONENT_BATCH_MAX];
    size_t dot_update_count = 0U;
    for (size_t i = 0; i < AI_CREATE_SESSION_MAX_PAGES; ++i) {
        const bool visible_dot = show_dots && i < page_count;
        esp_err_t err = append_component_property(
            ui, dot_updates, &dot_update_count,
            bool_property_update(s_session_dot_state_keys[i],
                                 GSP_PROP_KEY_VISIBLE, visible_dot));
        if (err != ESP_OK) return first_error(result, err);
        if (visible_dot) {
            err = append_component_property(
                ui, dot_updates, &dot_update_count,
                i32_property_update(s_session_dot_state_keys[i],
                    GSP_PROP_KEY_X, dots_left +
                        (int32_t)i * AI_SESSION_DOT_SPACING));
            if (err != ESP_OK) return first_error(result, err);
            err = append_component_property(
                ui, dot_updates, &dot_update_count,
                i32_property_update(s_session_dot_state_keys[i],
                                    GSP_PROP_KEY_Y, AI_SESSION_DOT_Y));
            if (err != ESP_OK) return first_error(result, err);
            err = append_component_property(
                ui, dot_updates, &dot_update_count,
                color_property_update(s_session_dot_keys[i],
                    i == session_page ? 0xFA60U : 0x39E7U));
            if (err != ESP_OK) return first_error(result, err);
        }
    }
    result = first_error(result, flush_component_properties(
        ui, dot_updates, &dot_update_count));
    for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); ++i) {
        const size_t session_index = session_page * AI_CREATE_SESSION_PAGE_SIZE + i;
        const bool present = sessions && session_index < sessions->session_count;
        char title[48] = "";
        if (present &&
            strcmp(sessions->sessions[session_index],
                   sessions->current_alias) == 0) {
            snprintf(title, sizeof(title), "%s · Current",
                     sessions->sessions[session_index]);
        } else if (present) {
            strlcpy(title, sessions->sessions[session_index], sizeof(title));
        }
        result = first_error(
            result, esp_gsp_set_text(ui, titles[i], title));
        result = first_error(
            result, esp_gsp_set_visible(ui, visible[i], present));
    }
    return result;
}

static bool transcript_equal(const ai_create_transcript_t *left,
                             const ai_create_transcript_t *right)
{
    if (!left || !right || left->count != right->count) return false;
    for (size_t index = 0; index < left->count; ++index) {
        const ai_create_message_t *lhs = &left->messages[index];
        const ai_create_message_t *rhs = &right->messages[index];
        if (lhs->request_id != rhs->request_id ||
            lhs->role != rhs->role || lhs->pending != rhs->pending ||
            lhs->error != rhs->error || strcmp(lhs->text, rhs->text) != 0) {
            return false;
        }
    }
    return true;
}

static uint32_t message_source_count(void *user_ctx)
{
    const ai_create_presenter_handle_t presenter = user_ctx;
    return presenter ? (uint32_t)presenter->transcript.count : 0;
}

static bool message_at(const ai_create_presenter_handle_t presenter,
                       uint32_t index, esp_gsp_message_t *out_message)
{
    if (!presenter || !out_message || index >= presenter->transcript.count) {
        return false;
    }
    const ai_create_message_t *message =
        &presenter->transcript.messages[index];
    const char *text = message->text;
    if (!text[0] && message->pending) {
        text = "…";
    } else if (!text[0] && message->error) {
        text = "Request failed";
    }
    uint64_t id = ((uint64_t)message->request_id << 1U) |
                  (uint64_t)message->role;
    if (message->request_id == 0) {
        id = (UINT64_C(1) << 63U) |
             ((uint64_t)(index + 1U) << 1U) |
             (uint64_t)message->role;
    }
    *out_message = (esp_gsp_message_t) {
        .text = text,
        .id = id,
        .revision = (message->pending ? 1U : 0U) |
                    (message->error ? 2U : 0U),
        .direction = message->role == AI_CREATE_MESSAGE_USER
            ? ESP_GSP_MESSAGE_OUTGOING : ESP_GSP_MESSAGE_INCOMING,
    };
    return true;
}

static bool message_source_get(void *user_ctx, uint32_t index,
                               esp_gsp_message_t *out_message)
{
    return message_at(user_ctx, index, out_message);
}

static uint32_t voice_message_source_count(void *user_ctx)
{
    const ai_create_presenter_handle_t presenter = user_ctx;
    return presenter && presenter->transcript.count > 0 ? 1U : 0U;
}

static bool voice_message_source_get(void *user_ctx, uint32_t index,
                                     esp_gsp_message_t *out_message)
{
    const ai_create_presenter_handle_t presenter = user_ctx;
    if (!presenter || index != 0 || presenter->transcript.count == 0) {
        return false;
    }
    size_t source_index = presenter->transcript.count;
    while (source_index > 0) {
        --source_index;
        if (presenter->transcript.messages[source_index].role ==
                AI_CREATE_MESSAGE_ASSISTANT) {
            break;
        }
    }
    return message_at(presenter, (uint32_t)source_index, out_message);
}

static void sync_message_list(ai_create_presenter_handle_t presenter,
                              esp_gsp_handle_t ui,
                              gsp_component_key_t key,
                              esp_gsp_list_t *list,
                              esp_gsp_message_count_cb_t count,
                              esp_gsp_message_get_cb_t get,
                              bool changed)
{
    if (*list == ESP_GSP_LIST_NONE) {
        const esp_gsp_message_source_t source = {
            .struct_size = sizeof(source),
            .count = count,
            .get = get,
            .user_ctx = presenter,
        };
        *list = esp_gsp_message_list_bind_component(ui, key, &source);
        return;
    }
    if (changed) {
        (void)esp_gsp_message_list_changed(ui, *list, 0);
    }
}

static void render_transcript(ai_create_presenter_handle_t presenter,
                              esp_gsp_handle_t ui,
                              const ai_create_transcript_t *transcript,
                              bool chat, bool voice)
{
    const bool changed = !transcript_equal(&presenter->transcript, transcript);
    if (changed) {
        presenter->transcript = *transcript;
        presenter->transcript_revision++;
        if (presenter->transcript_revision == 0) {
            presenter->transcript_revision = 1;
        }
    }
    if (!chat) return;

    if (voice) {
        const bool stale = presenter->voice_message_list_revision !=
            presenter->transcript_revision;
        sync_message_list(presenter, ui, GSP_OBJ_KEY_AI_CHAT_VOICE_MESSAGES,
                          &presenter->voice_message_list,
                          voice_message_source_count,
                          voice_message_source_get, stale);
        presenter->voice_message_list_revision =
            presenter->transcript_revision;
    } else {
        const bool stale = presenter->message_list_revision !=
            presenter->transcript_revision;
        sync_message_list(presenter, ui, GSP_OBJ_KEY_AI_CHAT_MESSAGES,
                          &presenter->message_list, message_source_count,
                          message_source_get, stale);
        presenter->message_list_revision = presenter->transcript_revision;
    }
}

esp_err_t ai_create_presenter_create(
    ai_create_presenter_handle_t *ret_presenter)
{
    if (!ret_presenter) return ESP_ERR_INVALID_ARG;
    *ret_presenter = calloc(1, sizeof(**ret_presenter));
    if (*ret_presenter) {
        (*ret_presenter)->rendered_deciseconds = INT64_MIN;
        (*ret_presenter)->message_list = ESP_GSP_LIST_NONE;
        (*ret_presenter)->voice_message_list = ESP_GSP_LIST_NONE;
    }
    return *ret_presenter ? ESP_OK : ESP_ERR_NO_MEM;
}

void ai_create_presenter_delete(ai_create_presenter_handle_t presenter)
{
    free(presenter);
}

void ai_create_presenter_invalidate(ai_create_presenter_handle_t presenter)
{
    if (!presenter) return;
    presenter->valid = false;
    presenter->message_list = ESP_GSP_LIST_NONE;
    presenter->voice_message_list = ESP_GSP_LIST_NONE;
    presenter->sessions_valid = false;
    presenter->rendered_deciseconds = INT64_MIN;
    presenter->next_wave_update_us = 0;
}

esp_err_t ai_create_presenter_render(
    ai_create_presenter_handle_t presenter,
    esp_gsp_handle_t ui,
    const ai_create_controller_snapshot_t *model,
    const claw_session_mgr_alias_map_t *sessions,
    size_t session_page,
    int64_t now_us,
    int64_t voice_started_us,
    uint8_t voice_loudness)
{
    if (!presenter || !ui || !model) return ESP_ERR_INVALID_ARG;
    esp_err_t render_error = ESP_OK;
    const bool welcome = model->screen == AI_CREATE_SCREEN_WELCOME;
    const bool chat = model->screen == AI_CREATE_SCREEN_CHAT;
    const bool picker = model->screen == AI_CREATE_SCREEN_SESSION_PICKER;
    const bool voice = voice_visible(model);
    const bool mode = model->composer == AI_CREATE_COMPOSER_MODE;
    const bool model_changed = !presenter->valid ||
        presenter->rendered_revision != model->revision;
    const bool screen_changed = !presenter->valid ||
        presenter->rendered_screen != model->screen;
    const bool composer_changed = !presenter->valid ||
        presenter->rendered_composer != model->composer;
    const bool mode_changed = !presenter->valid ||
        presenter->rendered_mode != model->mode;
    const bool voice_changed = !presenter->valid ||
        presenter->rendered_voice_visible != voice;
    const bool voice_availability_changed = !presenter->valid ||
        presenter->rendered_voice_status.readiness !=
            model->voice_status.readiness ||
        presenter->rendered_voice_status.error != model->voice_status.error ||
        presenter->rendered_voice_status.retryable !=
            model->voice_status.retryable;
    const bool has_messages = chat && model->transcript.count > 0;
    const bool message_presence_changed = !presenter->valid ||
        presenter->rendered_has_messages != has_messages;

    render_transcript(presenter, ui, &model->transcript, chat, voice);
    if (screen_changed) {
        /* The device transports App updates through a 24-entry render queue.
         * Re-entering the picker also refreshes its rows and dots, so publish
         * related visibility changes atomically instead of consuming one
         * queue entry per bind. */
        const gsp_component_update_t updates[] = {
            visibility_update(GSP_OBJ_KEY_AI_WELCOME, welcome),
            visibility_update(GSP_OBJ_KEY_AI_CHAT, chat),
            visibility_update(GSP_OBJ_KEY_AI_SESSION_PICKER, picker),
        };
        render_error = first_error(render_error,
            set_component_visibility_many(
                ui, updates, sizeof(updates) / sizeof(updates[0])));
    }
    if (model_changed) {
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_AI_SESSION_DELETE_MODAL_VISIBLE,
            model->session_delete_confirm_visible);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_AI_SESSION_DELETE_CONTROLS_VISIBLE,
            model->session_delete_confirm_visible &&
                !model->session_deleting);
        (void)esp_gsp_set_visible(
            ui, GSP_BIND_AI_SESSION_DELETE_PROGRESS_VISIBLE,
            model->session_delete_confirm_visible &&
                model->session_deleting);
        (void)esp_gsp_set_text(
            ui, GSP_BIND_AI_SESSION_DELETE_ALIAS,
            model->delete_session_alias);
    }
    if (screen_changed || composer_changed || voice_changed ||
        message_presence_changed) {
        const bool show_default_features = welcome && !mode && !voice;
        const bool show_mode_features = welcome && mode && !voice;
        const gsp_component_update_t updates[] = {
            visibility_update(
                GSP_OBJ_KEY_AI_FEATURE_DEFAULT, show_default_features),
            visibility_update(
                GSP_OBJ_KEY_AI_FEATURE_MODE, show_mode_features),
            visibility_update(
                GSP_OBJ_KEY_AI_WELCOME_DEFAULT,
                welcome && !mode && !voice),
            visibility_update(
                GSP_OBJ_KEY_AI_WELCOME_MODE, welcome && mode && !voice),
            visibility_update(
                GSP_OBJ_KEY_AI_CHAT_DEFAULT, chat && !voice),
            /* The two viewports share a small row-instance pool; changing
             * both in one transaction prevents a transient double-owner. */
            visibility_update(
                GSP_OBJ_KEY_AI_CHAT_ROUNDS, !voice && has_messages),
            visibility_update(
                GSP_OBJ_KEY_AI_CHAT_VOICE_ROUNDS, voice && has_messages),
            visibility_update(GSP_OBJ_KEY_AI_VOICE_PANEL, voice),
            visibility_update(GSP_OBJ_KEY_AI_VOICE_WAVEFORM, voice),
        };
        render_error = first_error(render_error,
            set_component_visibility_many(
                ui, updates, sizeof(updates) / sizeof(updates[0])));
    }
    if (mode_changed) {
        const uint16_t selected_mode =
            model->mode < CAP_IM_AI_CREATE_MODE_COUNT
                ? (uint16_t)model->mode
                : (uint16_t)CAP_IM_AI_CREATE_MODE_QUICK;
        const uint16_t feature_page = feature_page_for_mode(
            (cap_im_ai_create_mode_t)selected_mode);
        (void)esp_gsp_page_flow_set_page(
            ui, GSP_OBJ_KEY_AI_FEATURE_DEFAULT_PAGES, feature_page, false);
        (void)esp_gsp_page_flow_set_page(
            ui, GSP_OBJ_KEY_AI_FEATURE_MODE_PAGES, feature_page, false);
        static const uint16_t selected_binds[][2] = {
            {GSP_BIND_AI_SELECTED_DEFAULT_CREATE_VISIBLE,
             GSP_BIND_AI_SELECTED_MODE_CREATE_VISIBLE},
            {GSP_BIND_AI_SELECTED_DEFAULT_SKILL_VISIBLE,
             GSP_BIND_AI_SELECTED_MODE_SKILL_VISIBLE},
            {GSP_BIND_AI_SELECTED_DEFAULT_MEMORY_VISIBLE,
             GSP_BIND_AI_SELECTED_MODE_MEMORY_VISIBLE},
            {GSP_BIND_AI_SELECTED_DEFAULT_PLAN_VISIBLE,
             GSP_BIND_AI_SELECTED_MODE_PLAN_VISIBLE},
            {GSP_BIND_AI_SELECTED_DEFAULT_QUICK_VISIBLE,
             GSP_BIND_AI_SELECTED_MODE_QUICK_VISIBLE},
            {GSP_BIND_AI_SELECTED_DEFAULT_SCHEDULE_VISIBLE,
             GSP_BIND_AI_SELECTED_MODE_SCHEDULE_VISIBLE},
        };
        if (!presenter->valid) {
            for (uint16_t selected = 0;
                 selected < sizeof(selected_binds) / sizeof(selected_binds[0]);
                 ++selected) {
                const bool is_selected = selected == selected_mode;
                (void)esp_gsp_set_visible(
                    ui, selected_binds[selected][0], is_selected);
                (void)esp_gsp_set_visible(
                    ui, selected_binds[selected][1], is_selected);
            }
        } else {
            const uint16_t previous_mode =
                presenter->rendered_mode < CAP_IM_AI_CREATE_MODE_COUNT
                    ? (uint16_t)presenter->rendered_mode
                    : (uint16_t)CAP_IM_AI_CREATE_MODE_QUICK;
            (void)esp_gsp_set_visible(
                ui, selected_binds[previous_mode][0], false);
            (void)esp_gsp_set_visible(
                ui, selected_binds[previous_mode][1], false);
            (void)esp_gsp_set_visible(
                ui, selected_binds[selected_mode][0], true);
            (void)esp_gsp_set_visible(
                ui, selected_binds[selected_mode][1], true);
        }

        const mode_copy_t *copy = model->mode < CAP_IM_AI_CREATE_MODE_COUNT
                                      ? &s_modes[model->mode]
                                      : &s_modes[CAP_IM_AI_CREATE_MODE_QUICK];
        (void)esp_gsp_set_text(ui, GSP_BIND_AI_MODE_TITLE, copy->title);
    }
    if (voice_availability_changed) {
        const char *prompt = voice_unavailable_copy(
            model->voice_status.readiness);
        (void)esp_gsp_set_text(
            ui, GSP_BIND_AI_WELCOME_DEFAULT_PROMPT, prompt);
        (void)esp_gsp_set_text(
            ui, GSP_BIND_AI_CHAT_DEFAULT_PROMPT, prompt);
    }
    if (mode_changed || voice_availability_changed) {
        const mode_copy_t *copy = model->mode < CAP_IM_AI_CREATE_MODE_COUNT
                                      ? &s_modes[model->mode]
                                      : &s_modes[CAP_IM_AI_CREATE_MODE_QUICK];
        (void)esp_gsp_set_text(
            ui, GSP_BIND_AI_MODE_PROMPT,
            model->voice_status.readiness == AI_CREATE_VOICE_READINESS_READY
                ? copy->prompt
                : voice_unavailable_copy(model->voice_status.readiness));
    }
    if (model_changed && voice) {
        (void)esp_gsp_set_text(ui, GSP_BIND_AI_VOICE_TEXT,
                               model->input[0] ? model->input : "Listening…");
        (void)esp_gsp_set_text(ui, GSP_BIND_AI_VOICE_HINT, voice_hint(model));
    }
    const char *notice = model->notice_visible
        ? (model->notice[0] ? model->notice
                            : voice_notice_copy(model->voice_status.readiness))
        : "";
    if (!presenter->valid ||
        presenter->rendered_notice_visible != model->notice_visible ||
        strcmp(presenter->rendered_notice, notice) != 0) {
        (void)esp_gsp_set_visible(ui, GSP_BIND_AI_NOTICE_VISIBLE,
                                  model->notice_visible);
        (void)esp_gsp_set_text(ui, GSP_BIND_AI_NOTICE_TEXT, notice);
        presenter->rendered_notice_visible = model->notice_visible;
        strlcpy(presenter->rendered_notice, notice,
                sizeof(presenter->rendered_notice));
    }
    presenter->rendered_revision = model->revision;

    int64_t deciseconds = 0;
    if (voice && voice_started_us > 0 && now_us >= voice_started_us) {
        deciseconds = (now_us - voice_started_us) / 100000;
    }
    if (voice && presenter->rendered_deciseconds != deciseconds) {
        char elapsed[20];
        snprintf(elapsed, sizeof(elapsed), "%lld.%lld s",
                 (long long)(deciseconds / 10),
                 (long long)(deciseconds % 10));
        (void)esp_gsp_set_text(ui, GSP_BIND_AI_VOICE_TIME, elapsed);
        presenter->rendered_deciseconds = deciseconds;
    }
    if (!presenter->valid || presenter->rendered_voice_visible != voice) {
        (void)gsp_ai_create_ai_voice_panel_set_voice_panel_y(
            ui, voice ? 180 : 480);
        presenter->rendered_voice_visible = voice;
    }
    if (voice) {
        const bool wave_state_changed = !presenter->valid ||
            presenter->rendered_voice_state != model->voice_state;
        render_voice_wave(presenter, ui, model->voice_state,
                          voice_loudness, now_us, wave_state_changed);
        presenter->rendered_voice_state = model->voice_state;
    } else {
        presenter->next_wave_update_us = 0;
        memset(presenter->loudness_history, 0,
               sizeof(presenter->loudness_history));
        presenter->rendered_voice_state = AI_CREATE_VOICE_STATE_IDLE;
    }

    if (picker && (!presenter->sessions_valid ||
        presenter->rendered_session_page != session_page || !sessions ||
        memcmp(&presenter->rendered_sessions, sessions,
               sizeof(*sessions)) != 0)) {
        const esp_err_t err = render_sessions(ui, sessions, session_page);
        render_error = first_error(render_error, err);
        if (err == ESP_OK) {
            if (sessions) presenter->rendered_sessions = *sessions;
            presenter->rendered_session_page = session_page;
            presenter->sessions_valid = true;
        } else {
            presenter->sessions_valid = false;
        }
    }
    presenter->rendered_screen = model->screen;
    presenter->rendered_composer = model->composer;
    presenter->rendered_mode = model->mode;
    presenter->rendered_voice_status = model->voice_status;
    presenter->rendered_has_messages = has_messages;
    presenter->valid = true;
    return render_error;
}
