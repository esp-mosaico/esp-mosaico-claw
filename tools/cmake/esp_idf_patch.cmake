set(EDGE_AGENT_PROJECT_LOG_PREFIX "[edge_agent]")
option(EDGE_AGENT_STRICT_IDF_PATCH "Fail configure when an ESP-IDF patch cannot be verified or applied" ON)

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} IDF_PATH environment variable is not set")
endif()

function(edge_agent_apply_idf_patch PATCH_PATH PATCH_NAME)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PATCH_PATH}")
    if(NOT EXISTS "${PATCH_PATH}")
        message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: file not found: ${PATCH_PATH}")
        return()
    endif()

    find_program(EDGE_AGENT_GIT_EXECUTABLE NAMES git)
    if(NOT EDGE_AGENT_GIT_EXECUTABLE)
        message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: git not found")
        return()
    endif()

    execute_process(
        COMMAND "${EDGE_AGENT_GIT_EXECUTABLE}" -C "$ENV{IDF_PATH}" apply --check --whitespace=nowarn "${PATCH_PATH}"
        RESULT_VARIABLE PATCH_CHECK_RESULT
        OUTPUT_QUIET
        ERROR_VARIABLE PATCH_CHECK_ERROR
    )
    if(PATCH_CHECK_RESULT EQUAL 0)
        execute_process(
            COMMAND "${EDGE_AGENT_GIT_EXECUTABLE}" -C "$ENV{IDF_PATH}" apply --whitespace=nowarn "${PATCH_PATH}"
            RESULT_VARIABLE PATCH_APPLY_RESULT
            OUTPUT_QUIET
            ERROR_VARIABLE PATCH_APPLY_ERROR
        )
        if(PATCH_APPLY_RESULT EQUAL 0)
            message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} Applied ESP-IDF patch '${PATCH_NAME}'")
        else()
            string(STRIP "${PATCH_APPLY_ERROR}" PATCH_APPLY_ERROR)
            if(PATCH_APPLY_ERROR STREQUAL "")
                set(PATCH_APPLY_ERROR "git apply exited with ${PATCH_APPLY_RESULT}")
            endif()
            message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' failed to apply: ${PATCH_APPLY_ERROR}")
        endif()
        return()
    endif()

    execute_process(
        COMMAND "${EDGE_AGENT_GIT_EXECUTABLE}" -C "$ENV{IDF_PATH}" apply --reverse --check --whitespace=nowarn "${PATCH_PATH}"
        RESULT_VARIABLE PATCH_REVERSE_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(PATCH_REVERSE_RESULT EQUAL 0)
        message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' already applied")
        return()
    endif()

    string(STRIP "${PATCH_CHECK_ERROR}" PATCH_CHECK_ERROR)
    if(PATCH_CHECK_ERROR STREQUAL "")
        set(PATCH_CHECK_ERROR "git apply --check exited with ${PATCH_CHECK_RESULT}")
    endif()
    message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: ${PATCH_CHECK_ERROR}")
endfunction()

function(edge_agent_patch_file_replace FILE_PATH OLD_TEXT NEW_TEXT PATCH_NAME)
    if(NOT EXISTS "${FILE_PATH}")
        message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: file not found: ${FILE_PATH}")
        return()
    endif()

    file(READ "${FILE_PATH}" FILE_CONTENT)
    string(FIND "${FILE_CONTENT}" "${NEW_TEXT}" FIXED_TEXT_OFFSET)
    if(NOT FIXED_TEXT_OFFSET EQUAL -1)
        message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' already applied")
        return()
    endif()

    if(ARGC GREATER 4)
        math(EXPR FIXED_TEXT_ARGC "${ARGC} - 1")
        foreach(FIXED_TEXT_ARG_IDX RANGE 4 ${FIXED_TEXT_ARGC})
            set(FIXED_TEXT "${ARGV${FIXED_TEXT_ARG_IDX}}")
            string(FIND "${FILE_CONTENT}" "${FIXED_TEXT}" FIXED_TEXT_OFFSET)
            if(NOT FIXED_TEXT_OFFSET EQUAL -1)
                message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' already applied")
                return()
            endif()
        endforeach()
    endif()

    string(FIND "${FILE_CONTENT}" "${OLD_TEXT}" OLD_TEXT_OFFSET)
    if(OLD_TEXT_OFFSET EQUAL -1)
        if(EDGE_AGENT_STRICT_IDF_PATCH)
            message(FATAL_ERROR "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' could not be verified or applied: source pattern not found in ${FILE_PATH}")
        endif()
        message(WARNING "${EDGE_AGENT_PROJECT_LOG_PREFIX} ESP-IDF patch '${PATCH_NAME}' skipped: source pattern not found in ${FILE_PATH}")
        return()
    endif()

    string(REPLACE "${OLD_TEXT}" "${NEW_TEXT}" FILE_CONTENT "${FILE_CONTENT}")
    file(WRITE "${FILE_PATH}" "${FILE_CONTENT}")
    message(STATUS "${EDGE_AGENT_PROJECT_LOG_PREFIX} Applied ESP-IDF patch '${PATCH_NAME}'")
endfunction()

edge_agent_apply_idf_patch(
    "${CMAKE_SOURCE_DIR}/tools/esp-idf-release-v6.2-spi2-sct-fix.patch"
    "spi2_sct_fix"
)

# Upstream moved this repair from sample_edge NEG to shift_edge NEG later.
# Treat both as fixed states so v5.5.4, release/v5.5 snapshots, and the
# upstream fix branch can all configure without relying on a brittle patch file.
edge_agent_patch_file_replace(
    "$ENV{IDF_PATH}/components/esp_lcd/parl/esp_lcd_panel_io_parl.c"
    ".sample_edge = PARLIO_SAMPLE_EDGE_POS"
    ".sample_edge = PARLIO_SAMPLE_EDGE_NEG"
    "parlio_tx_edge"
    ".shift_edge = PARLIO_SHIFT_EDGE_NEG"
)

edge_agent_patch_file_replace(
    "$ENV{IDF_PATH}/components/usb/include/usb/usb_types_ch9.h"
    "#define USB_IAD_DESC_SIZE    9"
    "#define USB_IAD_DESC_SIZE    8"
    "usb_iad_desc_size"
)

# Add esp_http_client_set_event_handler declaration to esp_http_client.c
edge_agent_patch_file_replace(
    "$ENV{IDF_PATH}/components/esp_http_client/esp_http_client.c"
    [=[    client->user_data = data;
    return ESP_OK;
}

static esp_err_t _set_config(esp_http_client_handle_t client, const esp_http_client_config_t *config)]=]
    [=[    client->user_data = data;
    return ESP_OK;
}

esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb event_handler)
{
    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    client->event_handler = event_handler;
    return ESP_OK;
}

static esp_err_t _set_config(esp_http_client_handle_t client, const esp_http_client_config_t *config)]=]
    "http_client_set_event_handler_impl"
    "esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb event_handler)"
)

edge_agent_patch_file_replace(
    "$ENV{IDF_PATH}/components/esp_http_client/include/esp_http_client.h"
    [=[esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);

/**
 * @brief      Get HTTP client session errno]=]
    [=[esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void *data);

/**
 * @brief      Set the event handler for the client
 *
 * @param[in]  client  The esp_http_client handle
 * @param[in]  event_handler     The event handler
 *
 * @return
 *     - ESP_OK
 *     - ESP_ERR_INVALID_ARG
 */
esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb event_handler);

/**
 * @brief      Get HTTP client session errno]=]
    "http_client_set_event_handler_decl"
    "esp_err_t esp_http_client_set_event_handler(esp_http_client_handle_t client, http_event_handle_cb event_handler)"
)
