/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mosaic_logic.h"

#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "mosaic_capability.h"

#define MOSAIC_LUA_BINDING_MAX 40U
#define MOSAIC_LUA_TEXT_MAX 95U
#define MOSAIC_LUA_ERROR_MAX 255U
#define MOSAIC_LUA_DEFAULT_MEMORY_LIMIT (512U * 1024U)
#define MOSAIC_LUA_DEFAULT_INSTRUCTION_LIMIT 100000U
#define MOSAIC_LUA_HOOK_INTERVAL 1000U

typedef enum {
    MOSAIC_LUA_CACHE_NONE = 0,
    MOSAIC_LUA_CACHE_BOOL,
    MOSAIC_LUA_CACHE_INTEGER,
    MOSAIC_LUA_CACHE_TEXT,
} mosaic_lua_cache_type_t;

typedef struct {
    gsp_component_key_t component;
    gsp_property_key_t property;
    int selector_ref;
    mosaic_lua_cache_type_t cache_type;
    union {
        bool boolean;
        int64_t integer;
        char text[MOSAIC_LUA_TEXT_MAX + 1U];
    } cache;
} mosaic_lua_binding_t;

typedef struct {
    size_t size;
} mosaic_lua_allocation_t;

typedef struct {
    lua_State* state;
    esp_gsp_handle_t ui;
    const mosaic_app_package_t* package;
    mosaic_logic_log_fn_t log;
    void* log_ctx;
    size_t memory_used;
    size_t memory_limit;
    uint32_t instruction_remaining;
    int state_ref;
    int reducer_ref;
    mosaic_lua_binding_t bindings[MOSAIC_LUA_BINDING_MAX];
    size_t binding_count;
    char error[MOSAIC_LUA_ERROR_MAX + 1U];
} mosaic_lua_logic_t;

static char s_runtime_registry_key;

static void set_error(mosaic_lua_logic_t* logic, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    (void)vsnprintf(logic->error, sizeof(logic->error), format, args);
    va_end(args);
    if (logic->log != NULL) {
        logic->log(
            logic->log_ctx, logic->package->descriptor->name, logic->error);
    }
}

static void* bounded_alloc(
    void* user_ctx, void* pointer, size_t old_size, size_t new_size)
{
    (void)old_size;
    mosaic_lua_logic_t* logic = user_ctx;
    mosaic_lua_allocation_t* allocation
        = pointer != NULL ? (mosaic_lua_allocation_t*)pointer - 1 : NULL;
    const size_t previous_size = allocation != NULL ? allocation->size : 0;
    if (new_size == 0) {
        if (allocation != NULL) {
            logic->memory_used -= previous_size;
            free(allocation);
        }
        return NULL;
    }
    if (new_size > SIZE_MAX - sizeof(*allocation)
        || new_size
            > logic->memory_limit - (logic->memory_used - previous_size)) {
        return NULL;
    }
    allocation = realloc(allocation, sizeof(*allocation) + new_size);
    if (allocation == NULL) {
        return NULL;
    }
    allocation->size = new_size;
    logic->memory_used = logic->memory_used - previous_size + new_size;
    return allocation + 1;
}

static mosaic_lua_logic_t* runtime_from_state(lua_State* state)
{
    lua_pushlightuserdata(state, &s_runtime_registry_key);
    lua_rawget(state, LUA_REGISTRYINDEX);
    mosaic_lua_logic_t* logic = lua_touserdata(state, -1);
    lua_pop(state, 1);
    return logic;
}

static void push_contract_field(lua_State* state,
    const mosaic_capability_field_t* field, const uint8_t* payload);

/** Convert one contract payload into a Lua table.
 *
 * The capability layer describes every payload with a flat field table, so
 * this walk covers all present and future capabilities without any
 * per-domain code here.
 */
static void push_contract_table(lua_State* state,
    const mosaic_capability_contract_t* contract, const void* payload)
{
    const uint8_t* bytes = payload;
    lua_createtable(state, 0, contract->field_count);
    for (uint8_t index = 0; index < contract->field_count; ++index) {
        const mosaic_capability_field_t* field = &contract->fields[index];
        push_contract_field(state, field, bytes + field->offset);
        lua_setfield(state, -2, field->name);
    }
}

static void push_contract_field(lua_State* state,
    const mosaic_capability_field_t* field, const uint8_t* member)
{
    switch (field->type) {
    case MOSAIC_CAP_FIELD_BOOL: {
        bool value = false;
        memcpy(&value, member, sizeof(value));
        lua_pushboolean(state, value);
        break;
    }
    case MOSAIC_CAP_FIELD_I32: {
        int32_t value = 0;
        memcpy(&value, member, sizeof(value));
        lua_pushinteger(state, (lua_Integer)value);
        break;
    }
    case MOSAIC_CAP_FIELD_I64: {
        int64_t value = 0;
        memcpy(&value, member, sizeof(value));
        lua_pushinteger(state, (lua_Integer)value);
        break;
    }
    case MOSAIC_CAP_FIELD_U32: {
        uint32_t value = 0;
        memcpy(&value, member, sizeof(value));
        lua_pushinteger(state, (lua_Integer)value);
        break;
    }
    case MOSAIC_CAP_FIELD_F32: {
        float value = 0.0f;
        memcpy(&value, member, sizeof(value));
        lua_pushnumber(state, (lua_Number)value);
        break;
    }
    case MOSAIC_CAP_FIELD_STRING:
        lua_pushlstring(state, (const char*)member,
            strnlen((const char*)member, field->size));
        break;
    case MOSAIC_CAP_FIELD_ARRAY:
        lua_createtable(state, field->element_count, 0);
        for (uint16_t item = 0; item < field->element_count; ++item) {
            push_contract_table(state, field->element,
                member + (size_t)item * field->element->size);
            lua_rawseti(state, -2, item + 1);
        }
        break;
    default:
        lua_pushnil(state);
        break;
    }
}

/** Fill one command argument payload from a Lua table.
 *
 * Missing keys keep the zeroed default. Nested arrays are not accepted as
 * command arguments; commands take flat records by contract.
 */
static bool fill_contract_from_table(lua_State* state, int table_index,
    const mosaic_capability_contract_t* contract, uint8_t* payload)
{
    for (uint8_t index = 0; index < contract->field_count; ++index) {
        const mosaic_capability_field_t* field = &contract->fields[index];
        uint8_t* member = payload + field->offset;
        lua_getfield(state, table_index, field->name);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            continue;
        }
        switch (field->type) {
        case MOSAIC_CAP_FIELD_BOOL: {
            const bool value = lua_toboolean(state, -1);
            memcpy(member, &value, sizeof(value));
            break;
        }
        case MOSAIC_CAP_FIELD_I32: {
            const int32_t value = (int32_t)lua_tointeger(state, -1);
            memcpy(member, &value, sizeof(value));
            break;
        }
        case MOSAIC_CAP_FIELD_I64: {
            const int64_t value = (int64_t)lua_tointeger(state, -1);
            memcpy(member, &value, sizeof(value));
            break;
        }
        case MOSAIC_CAP_FIELD_U32: {
            const uint32_t value = (uint32_t)lua_tointeger(state, -1);
            memcpy(member, &value, sizeof(value));
            break;
        }
        case MOSAIC_CAP_FIELD_F32: {
            const float value = (float)lua_tonumber(state, -1);
            memcpy(member, &value, sizeof(value));
            break;
        }
        case MOSAIC_CAP_FIELD_STRING: {
            size_t length = 0;
            const char* text = lua_tolstring(state, -1, &length);
            if (text == NULL || length >= field->size) {
                lua_pop(state, 1);
                return false;
            }
            memcpy(member, text, length + 1U);
            break;
        }
        default:
            lua_pop(state, 1);
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

static int push_capability_error(lua_State* state, esp_err_t err)
{
    lua_pushnil(state);
    lua_pushinteger(state, err);
    return 2;
}

static int lua_capability_read(lua_State* state)
{
    const char* name = luaL_checkstring(state, 1);
    mosaic_lua_logic_t* logic = runtime_from_state(state);
    if (logic == NULL) {
        lua_pushnil(state);
        lua_pushliteral(state, "capability is unavailable");
        return 2;
    }
    const mosaic_capability_contract_t* contract =
        mosaic_capability_read_contract(name);
    if (contract == NULL) {
        return push_capability_error(state, ESP_ERR_NOT_SUPPORTED);
    }
    void* payload = calloc(1, contract->size);
    if (payload == NULL) {
        return push_capability_error(state, ESP_ERR_NO_MEM);
    }
    const esp_err_t err = mosaic_capability_read(
        name, logic->package->capabilities, payload, contract->size);
    if (err != ESP_OK) {
        free(payload);
        return push_capability_error(state, err);
    }
    push_contract_table(state, contract, payload);
    free(payload);
    return 1;
}

static int lua_capability_invoke(lua_State* state)
{
    const char* name = luaL_checkstring(state, 1);
    const char* command = luaL_checkstring(state, 2);
    mosaic_lua_logic_t* logic = runtime_from_state(state);
    if (logic == NULL) {
        lua_pushnil(state);
        lua_pushliteral(state, "capability is unavailable");
        return 2;
    }
    const mosaic_capability_command_t* declared =
        mosaic_capability_command_for_name(name, command);
    if (declared == NULL) {
        return push_capability_error(state, ESP_ERR_NOT_SUPPORTED);
    }

    uint8_t* args = NULL;
    const size_t args_size = declared->args != NULL ? declared->args->size : 0;
    if (args_size != 0) {
        if (!lua_istable(state, 3)) {
            return push_capability_error(state, ESP_ERR_INVALID_ARG);
        }
        args = calloc(1, args_size);
        if (args == NULL) {
            return push_capability_error(state, ESP_ERR_NO_MEM);
        }
        if (!fill_contract_from_table(state, 3, declared->args, args)) {
            free(args);
            return push_capability_error(state, ESP_ERR_INVALID_ARG);
        }
    }

    uint8_t* result = NULL;
    const size_t result_size =
        declared->result != NULL ? declared->result->size : 0;
    if (result_size != 0) {
        result = calloc(1, result_size);
        if (result == NULL) {
            free(args);
            return push_capability_error(state, ESP_ERR_NO_MEM);
        }
    }

    const esp_err_t err = mosaic_capability_invoke(name,
        logic->package->capabilities, command, args, args_size, result,
        result_size);
    free(args);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        free(result);
        return push_capability_error(state, err);
    }
    if (result_size != 0 && err == ESP_OK) {
        push_contract_table(state, declared->result, result);
    } else {
        lua_pushboolean(state, true);
    }
    free(result);
    /* An accepted async command reports the pending status as a second
     * return value so scripts can distinguish it from a completed call. */
    lua_pushinteger(state, err);
    return 2;
}

static void open_mosaic_api(lua_State* state)
{
    lua_createtable(state, 0, 1);
    lua_createtable(state, 0, 2);
    lua_pushcfunction(state, lua_capability_read);
    lua_setfield(state, -2, "read");
    lua_pushcfunction(state, lua_capability_invoke);
    lua_setfield(state, -2, "invoke");
    lua_setfield(state, -2, "capability");
    lua_setglobal(state, "mosaic");
}

static void instruction_hook(lua_State* state, lua_Debug* debug)
{
    (void)debug;
    mosaic_lua_logic_t* logic = runtime_from_state(state);
    if (logic == NULL
        || logic->instruction_remaining <= MOSAIC_LUA_HOOK_INTERVAL) {
        (void)luaL_error(state, "Mosaic Lua instruction limit exceeded");
        return;
    }
    logic->instruction_remaining -= MOSAIC_LUA_HOOK_INTERVAL;
}

static esp_err_t protected_call(mosaic_lua_logic_t* logic, int argument_count,
    int result_count, const char* operation)
{
    logic->instruction_remaining = MOSAIC_LUA_DEFAULT_INSTRUCTION_LIMIT;
    lua_sethook(logic->state, instruction_hook, LUA_MASKCOUNT,
        MOSAIC_LUA_HOOK_INTERVAL);
    int status = lua_pcall(logic->state, argument_count, result_count, 0);
    lua_sethook(logic->state, NULL, 0, 0);
    if (status == LUA_OK) {
        return ESP_OK;
    }
    const char* message = lua_tostring(logic->state, -1);
    set_error(logic, "%s: %s", operation,
        message != NULL ? message : "unknown Lua error");
    lua_pop(logic->state, 1);
    return status == LUA_ERRMEM ? ESP_ERR_NO_MEM : ESP_FAIL;
}

static void open_safe_libraries(lua_State* state)
{
    static const luaL_Reg libraries[] = {
        { LUA_GNAME, luaopen_base },
        { LUA_TABLIBNAME, luaopen_table },
        { LUA_STRLIBNAME, luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { NULL, NULL },
    };
    for (const luaL_Reg* library = libraries; library->name != NULL;
         ++library) {
        luaL_requiref(state, library->name, library->func, 1);
        lua_pop(state, 1);
    }
    static const char* const blocked_globals[] = {
        "collectgarbage",
        "dofile",
        "load",
        "loadfile",
    };
    for (size_t index = 0;
         index < sizeof(blocked_globals) / sizeof(blocked_globals[0]);
         ++index) {
        lua_pushnil(state);
        lua_setglobal(state, blocked_globals[index]);
    }
    lua_getglobal(state, LUA_STRLIBNAME);
    lua_pushnil(state);
    lua_setfield(state, -2, "dump");
    lua_pop(state, 1);
}

static uint32_t stable_key(const char* name)
{
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char* cursor = (const unsigned char*)name;
         *cursor != '\0'; ++cursor) {
        hash ^= *cursor;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool read_key(lua_State* state, int table, const char* field,
    uint32_t default_key, uint32_t* ret_key)
{
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1) && default_key != 0) {
        *ret_key = default_key;
        lua_pop(state, 1);
        return true;
    }
    if (lua_isinteger(state, -1)) {
        lua_Integer value = lua_tointeger(state, -1);
        if (value >= 0 && (uint64_t)value <= UINT32_MAX) {
            *ret_key = (uint32_t)value;
            lua_pop(state, 1);
            return true;
        }
    } else if (lua_type(state, -1) == LUA_TSTRING) {
        const char* value = lua_tostring(state, -1);
        if (value != NULL && value[0] != '\0') {
            *ret_key = stable_key(value);
            lua_pop(state, 1);
            return true;
        }
    }
    lua_pop(state, 1);
    return false;
}

static esp_err_t parse_bindings(
    mosaic_lua_logic_t* logic, int application_table)
{
    lua_State* state = logic->state;
    lua_getfield(state, application_table, "bindings");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return ESP_OK;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        set_error(logic, "application.bindings must be an array");
        return ESP_ERR_INVALID_ARG;
    }
    const size_t count = lua_rawlen(state, -1);
    if (count > MOSAIC_LUA_BINDING_MAX) {
        lua_pop(state, 1);
        set_error(logic, "application.bindings exceeds %u",
            (unsigned)MOSAIC_LUA_BINDING_MAX);
        return ESP_ERR_NO_MEM;
    }
    const int bindings_table = lua_gettop(state);
    for (size_t index = 0; index < count; ++index) {
        lua_rawgeti(state, bindings_table, (lua_Integer)index + 1);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 2);
            set_error(
                logic, "binding[%u] must be a table", (unsigned)index + 1U);
            return ESP_ERR_INVALID_ARG;
        }
        const int binding_table = lua_gettop(state);
        mosaic_lua_binding_t* binding = &logic->bindings[index];
        if (!read_key(state, binding_table, "component", 0, &binding->component)
            || !read_key(state, binding_table, "property", stable_key("value"),
                &binding->property)) {
            lua_pop(state, 2);
            set_error(logic,
                "binding[%u] requires component and a valid property",
                (unsigned)index + 1U);
            return ESP_ERR_INVALID_ARG;
        }
        lua_getfield(state, binding_table, "select");
        if (!lua_isfunction(state, -1)) {
            lua_pop(state, 3);
            set_error(logic, "binding[%u].select must be a function",
                (unsigned)index + 1U);
            return ESP_ERR_INVALID_ARG;
        }
        binding->selector_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        logic->binding_count++;
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return ESP_OK;
}

static esp_err_t load_application(mosaic_lua_logic_t* logic,
    const void* program, size_t program_size, const char* chunk_name)
{
    lua_State* state = logic->state;
    if (luaL_loadbuffer(state, program, program_size, chunk_name) != LUA_OK) {
        const char* message = lua_tostring(state, -1);
        set_error(logic, "load application: %s",
            message != NULL ? message : "invalid Lua program");
        lua_pop(state, 1);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = protected_call(logic, 0, 1, "run application");
    if (err != ESP_OK) {
        return err;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        set_error(logic, "application must return a table");
        return ESP_ERR_INVALID_ARG;
    }
    const int application_table = lua_gettop(state);
    lua_getfield(state, application_table, "initial_state");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        set_error(logic, "application.initial_state must be a table");
        return ESP_ERR_INVALID_ARG;
    }
    logic->state_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_getfield(state, application_table, "reducer");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 2);
        set_error(logic, "application.reducer must be a function");
        return ESP_ERR_INVALID_ARG;
    }
    logic->reducer_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    err = parse_bindings(logic, application_table);
    lua_pop(state, 1);
    return err;
}

typedef struct {
    mosaic_lua_cache_type_t type;
    union {
        bool boolean;
        int64_t integer;
        char text[MOSAIC_LUA_TEXT_MAX + 1U];
    } value;
} mosaic_lua_selected_t;

static esp_err_t select_binding(mosaic_lua_logic_t* logic,
    const mosaic_lua_binding_t* binding, mosaic_lua_selected_t* selected)
{
    lua_rawgeti(logic->state, LUA_REGISTRYINDEX, binding->selector_ref);
    lua_rawgeti(logic->state, LUA_REGISTRYINDEX, logic->state_ref);
    esp_err_t err = protected_call(logic, 1, 1, "binding selector");
    if (err != ESP_OK) {
        return err;
    }
    memset(selected, 0, sizeof(*selected));
    if (lua_isboolean(logic->state, -1)) {
        selected->type = MOSAIC_LUA_CACHE_BOOL;
        selected->value.boolean = lua_toboolean(logic->state, -1);
    } else if (lua_isinteger(logic->state, -1)) {
        selected->type = MOSAIC_LUA_CACHE_INTEGER;
        selected->value.integer = (int64_t)lua_tointeger(logic->state, -1);
    } else if (lua_type(logic->state, -1) == LUA_TSTRING) {
        size_t size = 0;
        const char* text = lua_tolstring(logic->state, -1, &size);
        if (size > MOSAIC_LUA_TEXT_MAX) {
            lua_pop(logic->state, 1);
            set_error(logic, "binding text exceeds %u bytes",
                (unsigned)MOSAIC_LUA_TEXT_MAX);
            return ESP_ERR_INVALID_ARG;
        }
        selected->type = MOSAIC_LUA_CACHE_TEXT;
        memcpy(selected->value.text, text, size);
        selected->value.text[size] = '\0';
    } else {
        lua_pop(logic->state, 1);
        set_error(
            logic, "binding selector must return boolean, integer, or string");
        return ESP_ERR_INVALID_ARG;
    }
    lua_pop(logic->state, 1);
    return ESP_OK;
}

static bool selection_changed(
    const mosaic_lua_binding_t* binding, const mosaic_lua_selected_t* selected)
{
    if (binding->cache_type != selected->type) {
        return true;
    }
    if (selected->type == MOSAIC_LUA_CACHE_BOOL) {
        return binding->cache.boolean != selected->value.boolean;
    }
    if (selected->type == MOSAIC_LUA_CACHE_INTEGER) {
        return binding->cache.integer != selected->value.integer;
    }
    return strcmp(binding->cache.text, selected->value.text) != 0;
}

static void commit_selection(
    mosaic_lua_binding_t* binding, const mosaic_lua_selected_t* selected)
{
    binding->cache_type = selected->type;
    if (selected->type == MOSAIC_LUA_CACHE_BOOL) {
        binding->cache.boolean = selected->value.boolean;
    } else if (selected->type == MOSAIC_LUA_CACHE_INTEGER) {
        binding->cache.integer = selected->value.integer;
    } else {
        memcpy(binding->cache.text, selected->value.text,
            sizeof(binding->cache.text));
    }
}

static esp_err_t publish_selection(mosaic_lua_logic_t* logic,
    const mosaic_lua_binding_t* binding, const mosaic_lua_selected_t* selected)
{
    if (selected->type == MOSAIC_LUA_CACHE_TEXT) {
        if (binding->property != stable_key("text")) {
            return ESP_ERR_INVALID_ARG;
        }
        return esp_gsp_component_set_text(
            logic->ui, binding->component, selected->value.text);
    }
    gsp_property_info_t info;
    esp_err_t err = esp_gsp_component_get_property_info(
        logic->ui, binding->component, binding->property, &info);
    if (err != ESP_OK) {
        return err;
    }
    gsp_value_t value = { 0 };
    if (selected->type == MOSAIC_LUA_CACHE_BOOL) {
        value.type = GSP_VALUE_BOOL;
        value.data.boolean = selected->value.boolean;
    } else if (info.value_type == GSP_VALUE_I32) {
        if (selected->value.integer < INT32_MIN
            || selected->value.integer > INT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        value.type = GSP_VALUE_I32;
        value.data.i32 = (int32_t)selected->value.integer;
    } else if (info.value_type == GSP_VALUE_COLOR) {
        if (selected->value.integer < 0
            || (uint64_t)selected->value.integer > UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        value.type = GSP_VALUE_COLOR;
        value.data.color = (uint32_t)selected->value.integer;
    } else {
        if (selected->value.integer < 0
            || (uint64_t)selected->value.integer > UINT32_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        value.type = GSP_VALUE_U32;
        value.data.u32 = (uint32_t)selected->value.integer;
    }
    return esp_gsp_component_set_property(
        logic->ui, binding->component, binding->property, &value);
}

static esp_err_t sync_bindings(mosaic_lua_logic_t* logic)
{
    for (size_t index = 0; index < logic->binding_count; ++index) {
        mosaic_lua_selected_t selected;
        esp_err_t err
            = select_binding(logic, &logic->bindings[index], &selected);
        if (err != ESP_OK) {
            return err;
        }
        if (!selection_changed(&logic->bindings[index], &selected)) {
            continue;
        }
        err = publish_selection(logic, &logic->bindings[index], &selected);
        if (err != ESP_OK) {
            set_error(logic, "failed to publish binding[%u]: %d",
                (unsigned)index + 1U, (int)err);
            return err;
        }
        commit_selection(&logic->bindings[index], &selected);
    }
    return ESP_OK;
}

static const char* event_name(const mosaic_event_t* event)
{
    switch (event->type) {
    case MOSAIC_EVENT_START:
        return "START";
    case MOSAIC_EVENT_STOP:
        return "STOP";
    case MOSAIC_EVENT_UI_CALL:
        return "UI_CALL";
    case MOSAIC_EVENT_SCENE_CHANGED:
        return "SCENE_CHANGED";
    case MOSAIC_EVENT_POINTER:
        return event->data.pointer.pressed ? "POINTER_DOWN" : "POINTER_UP";
    case MOSAIC_EVENT_TIMER:
        return "TIMER";
    default:
        return "UNKNOWN";
    }
}

static void push_event(lua_State* state, const mosaic_event_t* event)
{
    lua_createtable(state, 0, 9);
    lua_pushstring(state, event_name(event));
    lua_setfield(state, -2, "type");
    lua_pushinteger(state, (lua_Integer)event->timestamp_us);
    lua_setfield(state, -2, "timestamp_us");
    if (event->type == MOSAIC_EVENT_UI_CALL) {
        lua_pushinteger(state, event->data.call.action_id);
        lua_setfield(state, -2, "action_id");
        lua_pushinteger(state, event->data.call.arg);
        lua_setfield(state, -2, "arg");
        lua_pushinteger(state, event->data.call.scene_id);
        lua_setfield(state, -2, "scene_id");
        lua_pushinteger(state, event->data.call.list);
        lua_setfield(state, -2, "list");
        lua_pushinteger(state, event->data.call.item);
        lua_setfield(state, -2, "item");
    } else if (event->type == MOSAIC_EVENT_SCENE_CHANGED) {
        lua_pushinteger(state, event->data.scene.scene_id);
        lua_setfield(state, -2, "scene_id");
    } else if (event->type == MOSAIC_EVENT_POINTER) {
        lua_pushinteger(state, event->data.pointer.x);
        lua_setfield(state, -2, "x");
        lua_pushinteger(state, event->data.pointer.y);
        lua_setfield(state, -2, "y");
    } else if (event->type == MOSAIC_EVENT_TIMER) {
        lua_pushstring(state, event->data.timer.id);
        lua_setfield(state, -2, "timer_id");
        lua_pushinteger(state, event->data.timer.sequence);
        lua_setfield(state, -2, "sequence");
    }
}

static esp_err_t lua_create(
    const mosaic_logic_config_t* config, mosaic_logic_instance_t* ret_instance)
{
    if (config == NULL || config->package == NULL || ret_instance == NULL
        || config->program == NULL || config->program_size == 0
        || config->package->logic_entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_instance = NULL;
    mosaic_lua_logic_t* logic = calloc(1, sizeof(*logic));
    if (logic == NULL) {
        return ESP_ERR_NO_MEM;
    }
    logic->ui = config->ui;
    logic->package = config->package;
    logic->log = config->log;
    logic->log_ctx = config->log_ctx;
    logic->memory_limit = MOSAIC_LUA_DEFAULT_MEMORY_LIMIT;
    logic->state_ref = LUA_NOREF;
    logic->reducer_ref = LUA_NOREF;
    logic->state = lua_newstate(bounded_alloc, logic, 0);
    if (logic->state == NULL) {
        free(logic);
        return ESP_ERR_NO_MEM;
    }
    lua_pushlightuserdata(logic->state, &s_runtime_registry_key);
    lua_pushlightuserdata(logic->state, logic);
    lua_rawset(logic->state, LUA_REGISTRYINDEX);
    open_safe_libraries(logic->state);
    open_mosaic_api(logic->state);
    esp_err_t err = load_application(logic, config->program,
        config->program_size, config->package->logic_entry);
    if (err == ESP_OK) {
        err = sync_bindings(logic);
    }
    if (err != ESP_OK) {
        lua_close(logic->state);
        free(logic);
        return err;
    }
    *ret_instance = logic;
    return ESP_OK;
}

static void lua_destroy(mosaic_logic_instance_t instance)
{
    mosaic_lua_logic_t* logic = instance;
    if (logic == NULL) {
        return;
    }
    lua_close(logic->state);
    free(logic);
}

static esp_err_t lua_dispatch(
    mosaic_logic_instance_t instance, const mosaic_event_t* event)
{
    mosaic_lua_logic_t* logic = instance;
    if (logic == NULL || event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lua_rawgeti(logic->state, LUA_REGISTRYINDEX, logic->reducer_ref);
    lua_rawgeti(logic->state, LUA_REGISTRYINDEX, logic->state_ref);
    push_event(logic->state, event);
    esp_err_t err = protected_call(logic, 2, 1, "reducer");
    if (err != ESP_OK) {
        return err;
    }
    if (!lua_istable(logic->state, -1)) {
        lua_pop(logic->state, 1);
        set_error(logic, "reducer must return a state table");
        return ESP_ERR_INVALID_ARG;
    }
    const int old_state_ref = logic->state_ref;
    logic->state_ref = luaL_ref(logic->state, LUA_REGISTRYINDEX);
    luaL_unref(logic->state, LUA_REGISTRYINDEX, old_state_ref);
    return sync_bindings(logic);
}

static esp_err_t lua_step(mosaic_logic_instance_t instance, int64_t now_us)
{
    (void)now_us;
    return instance != NULL ? ESP_OK : ESP_ERR_INVALID_ARG;
}

const mosaic_logic_ops_t mosaic_lua_logic_ops = {
    .create = lua_create,
    .destroy = lua_destroy,
    .dispatch = lua_dispatch,
    .step = lua_step,
};
