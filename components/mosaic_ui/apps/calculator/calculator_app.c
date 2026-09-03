/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Calculator model and GSP presenter. The expression evaluator is deliberately
 * platform-neutral so the same behavior runs on host/WASM and ESP.
 */

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calculator_actions.h"
#include "calculator_binds.h"
#include "calculator_objects.h"
#include "mosaic_app_catalog.h"
#include "mosaic_hub_actions.h"
#include "mosaic_runtime.h"

#define CALCULATOR_APP_ID 45U
#define CALC_EXPR_MAX 32U
#define CALC_HISTORY_MAX 5U
#define CALC_PAGE_MAIN 0U
#define CALC_PAGE_SPLIT 1U
#define CALC_PAGE_HISTORY 2U
#define CALC_MAX_BILL_CENTS 999999

typedef enum {
    CALC_OK = 0,
    CALC_INCOMPLETE,
    CALC_DIVIDE_ZERO,
    CALC_OVERFLOW,
    CALC_INVALID,
} calc_error_t;

typedef struct {
    double value;
    bool percent;
    char repeat_op;
    double repeat_operand;
} calc_value_t;

typedef struct {
    const char *source;
    size_t at;
    calc_error_t error;
} calc_parser_t;

typedef struct {
    char expression[CALC_EXPR_MAX + 1U];
    double result;
    bool has_result;
    bool just_evaluated;
    char repeat_op;
    double repeat_operand;
    char error[32];
} calc_history_t;

typedef struct {
    char expression[CALC_EXPR_MAX + 1U];
    double result;
    bool has_result;
    bool just_evaluated;
    char repeat_op;
    double repeat_operand;
    char message[32];
    calc_history_t history[CALC_HISTORY_MAX];
    size_t history_count;
    bool clear_armed;
    int tip_percent;
    int people;
    int64_t bill_cents;
    uint16_t page;
    bool initialized;
} calculator_t;

static calculator_t s_calc;

static void parser_skip(calc_parser_t *parser)
{
    while (parser->source[parser->at] == ' ') {
        parser->at++;
    }
}

static calc_value_t parse_expression(calc_parser_t *parser);

static calc_value_t invalid_value(calc_parser_t *parser, calc_error_t error)
{
    if (parser->error == CALC_OK) {
        parser->error = error;
    }
    return (calc_value_t) {0};
}

static calc_value_t parse_factor(calc_parser_t *parser)
{
    parser_skip(parser);
    bool negative = false;
    while (parser->source[parser->at] == '-') {
        negative = !negative;
        parser->at++;
        parser_skip(parser);
    }

    calc_value_t value = {0};
    const char token = parser->source[parser->at];
    if (token == '(') {
        parser->at++;
        value = parse_expression(parser);
        parser_skip(parser);
        if (parser->source[parser->at] != ')') {
            return invalid_value(parser, CALC_INCOMPLETE);
        }
        parser->at++;
    } else if (isdigit((unsigned char)token) || token == '.') {
        char *end = NULL;
        value.value = strtod(parser->source + parser->at, &end);
        if (end == parser->source + parser->at) {
            return invalid_value(parser, CALC_INVALID);
        }
        parser->at = (size_t)(end - parser->source);
    } else {
        return invalid_value(
            parser, token == '\0' ? CALC_INCOMPLETE : CALC_INVALID);
    }

    if (negative) {
        value.value = -value.value;
    }
    parser_skip(parser);
    while (parser->source[parser->at] == '%') {
        value.value /= 100.0;
        value.percent = true;
        parser->at++;
        parser_skip(parser);
    }
    if (!isfinite(value.value)) {
        return invalid_value(parser, CALC_OVERFLOW);
    }
    return value;
}

static calc_value_t parse_term(calc_parser_t *parser)
{
    calc_value_t left = parse_factor(parser);
    while (parser->error == CALC_OK) {
        parser_skip(parser);
        const char op = parser->source[parser->at];
        if (op != '*' && op != '/') {
            break;
        }
        parser->at++;
        calc_value_t right = parse_factor(parser);
        if (parser->error != CALC_OK) {
            break;
        }
        if (op == '/' && fabs(right.value) < 1e-15) {
            return invalid_value(parser, CALC_DIVIDE_ZERO);
        }
        left.value = op == '*' ? left.value * right.value
                               : left.value / right.value;
        left.percent = false;
        left.repeat_op = op;
        left.repeat_operand = right.value;
        if (!isfinite(left.value)) {
            return invalid_value(parser, CALC_OVERFLOW);
        }
    }
    return left;
}

static calc_value_t parse_expression(calc_parser_t *parser)
{
    calc_value_t left = parse_term(parser);
    while (parser->error == CALC_OK) {
        parser_skip(parser);
        const char op = parser->source[parser->at];
        if (op != '+' && op != '-') {
            break;
        }
        parser->at++;
        calc_value_t right = parse_term(parser);
        if (parser->error != CALC_OK) {
            break;
        }
        const double operand = right.percent
            ? left.value * right.value : right.value;
        left.value = op == '+' ? left.value + operand : left.value - operand;
        left.percent = false;
        left.repeat_op = op;
        left.repeat_operand = operand;
        if (!isfinite(left.value)) {
            return invalid_value(parser, CALC_OVERFLOW);
        }
    }
    return left;
}

static calc_error_t evaluate(const char *expression, calc_value_t *out)
{
    if (expression == NULL || expression[0] == '\0') {
        return CALC_INCOMPLETE;
    }
    calc_parser_t parser = {
        .source = expression,
        .error = CALC_OK,
    };
    calc_value_t value = parse_expression(&parser);
    parser_skip(&parser);
    if (parser.error == CALC_OK && parser.source[parser.at] != '\0') {
        parser.error = CALC_INVALID;
    }
    if (parser.error == CALC_OK && out != NULL) {
        *out = value;
    }
    return parser.error;
}

static void format_number(double value, char *buffer, size_t size)
{
    if (!isfinite(value)) {
        snprintf(buffer, size, "0");
        return;
    }
    if (fabs(value) < 5e-13) {
        value = 0.0;
    }
    if ((fabs(value) >= 1e10) ||
            (fabs(value) > 0.0 && fabs(value) < 1e-7)) {
        snprintf(buffer, size, "%.6g", value);
    } else {
        snprintf(buffer, size, "%.10g", value);
    }
}

static void format_currency(int64_t cents, char *buffer, size_t size)
{
    const int64_t whole = cents / 100;
    const int64_t fraction = llabs(cents % 100);
    snprintf(buffer, size, "$%lld.%02lld",
             (long long)whole, (long long)fraction);
}

static bool expression_append(const char *text)
{
    const size_t used = strlen(s_calc.expression);
    const size_t add = strlen(text);
    if (used + add > CALC_EXPR_MAX) {
        snprintf(s_calc.message, sizeof(s_calc.message),
                 "INPUT LIMIT REACHED");
        return false;
    }
    memcpy(s_calc.expression + used, text, add + 1U);
    s_calc.just_evaluated = false;
    s_calc.repeat_op = '\0';
    s_calc.message[0] = '\0';
    calc_value_t preview;
    if (evaluate(s_calc.expression, &preview) == CALC_OK) {
        s_calc.result = preview.value;
        s_calc.has_result = true;
    }
    return true;
}

static bool expression_ends_value(void)
{
    const size_t length = strlen(s_calc.expression);
    if (length == 0) {
        return false;
    }
    const char last = s_calc.expression[length - 1U];
    return isdigit((unsigned char)last) || last == ')' || last == '%';
}

static bool expression_ends_operator(void)
{
    const size_t length = strlen(s_calc.expression);
    if (length == 0) {
        return false;
    }
    return strchr("+-*/", s_calc.expression[length - 1U]) != NULL;
}

static void start_fresh_if_needed(void)
{
    if (s_calc.just_evaluated || s_calc.message[0] != '\0') {
        s_calc.expression[0] = '\0';
        s_calc.result = 0;
        s_calc.has_result = true;
        s_calc.just_evaluated = false;
        s_calc.repeat_op = '\0';
        s_calc.message[0] = '\0';
    }
}

static void append_digit(char digit)
{
    start_fresh_if_needed();
    if (expression_ends_value()) {
        const size_t length = strlen(s_calc.expression);
        const char last = length ? s_calc.expression[length - 1U] : '\0';
        if (last == ')' || last == '%') {
            if (!expression_append("*")) {
                return;
            }
        }
    }
    size_t number_start = strlen(s_calc.expression);
    while (number_start > 0 &&
            (isdigit((unsigned char)s_calc.expression[number_start - 1U]) ||
             s_calc.expression[number_start - 1U] == '.')) {
        number_start--;
    }
    if (s_calc.expression[number_start] == '0' &&
            s_calc.expression[number_start + 1U] == '\0') {
        s_calc.expression[number_start] = digit;
        return;
    }
    char text[2] = {digit, '\0'};
    (void)expression_append(text);
}

static void append_decimal(void)
{
    start_fresh_if_needed();
    size_t start = strlen(s_calc.expression);
    while (start > 0 &&
            (isdigit((unsigned char)s_calc.expression[start - 1U]) ||
             s_calc.expression[start - 1U] == '.')) {
        if (s_calc.expression[start - 1U] == '.') {
            return;
        }
        start--;
    }
    if (start == strlen(s_calc.expression)) {
        if (expression_ends_value() && !expression_append("*")) {
            return;
        }
        (void)expression_append("0.");
    } else {
        (void)expression_append(".");
    }
}

static void append_operator(char op)
{
    if (s_calc.message[0] != '\0') {
        return;
    }
    if (s_calc.just_evaluated && s_calc.has_result) {
        format_number(s_calc.result, s_calc.expression,
                      sizeof(s_calc.expression));
        s_calc.just_evaluated = false;
    }
    const size_t length = strlen(s_calc.expression);
    if (length == 0) {
        if (op == '-') {
            (void)expression_append("-");
        }
        return;
    }
    if (expression_ends_operator()) {
        if (length == 1U && s_calc.expression[0] == '-') {
            return;
        }
        s_calc.expression[length - 1U] = op;
        return;
    }
    char text[2] = {op, '\0'};
    (void)expression_append(text);
}

static void toggle_parenthesis(void)
{
    start_fresh_if_needed();
    int opens = 0;
    int closes = 0;
    for (const char *cursor = s_calc.expression; *cursor; ++cursor) {
        opens += *cursor == '(';
        closes += *cursor == ')';
    }
    if (s_calc.expression[0] == '\0' || expression_ends_operator() ||
            s_calc.expression[strlen(s_calc.expression) - 1U] == '(') {
        (void)expression_append("(");
    } else if (opens > closes) {
        (void)expression_append(")");
    } else {
        (void)expression_append("*(");
    }
}

static void toggle_sign(void)
{
    if (s_calc.message[0] != '\0') {
        return;
    }
    if (s_calc.just_evaluated && s_calc.has_result) {
        s_calc.result = -s_calc.result;
        format_number(s_calc.result, s_calc.expression,
                      sizeof(s_calc.expression));
        s_calc.just_evaluated = false;
        return;
    }
    size_t end = strlen(s_calc.expression);
    size_t start = end;
    while (start > 0 &&
            (isdigit((unsigned char)s_calc.expression[start - 1U]) ||
             s_calc.expression[start - 1U] == '.')) {
        start--;
    }
    if (start == end) {
        return;
    }
    const bool unary = start > 0 && s_calc.expression[start - 1U] == '-' &&
        (start == 1U || strchr("+*/(-", s_calc.expression[start - 2U]));
    if (unary) {
        memmove(s_calc.expression + start - 1U, s_calc.expression + start,
                end - start + 1U);
    } else if (end < CALC_EXPR_MAX) {
        memmove(s_calc.expression + start + 1U, s_calc.expression + start,
                end - start + 1U);
        s_calc.expression[start] = '-';
    }
    calc_value_t preview;
    if (evaluate(s_calc.expression, &preview) == CALC_OK) {
        s_calc.result = preview.value;
        s_calc.has_result = true;
    }
}

static void append_percent(void)
{
    if (!s_calc.just_evaluated && expression_ends_value()) {
        (void)expression_append("%");
    } else if (s_calc.just_evaluated && s_calc.has_result) {
        format_number(s_calc.result, s_calc.expression,
                      sizeof(s_calc.expression));
        s_calc.just_evaluated = false;
        (void)expression_append("%");
    }
}

static void backspace(void)
{
    const size_t length = strlen(s_calc.expression);
    if (length == 0) {
        return;
    }
    s_calc.expression[length - 1U] = '\0';
    s_calc.just_evaluated = false;
    s_calc.repeat_op = '\0';
    s_calc.message[0] = '\0';
    if (s_calc.expression[0] == '\0') {
        s_calc.result = 0;
        s_calc.has_result = true;
        return;
    }
    calc_value_t preview;
    if (evaluate(s_calc.expression, &preview) == CALC_OK) {
        s_calc.result = preview.value;
        s_calc.has_result = true;
    }
}

static void clear_calculation(void)
{
    s_calc.expression[0] = '\0';
    s_calc.result = 0;
    s_calc.has_result = true;
    s_calc.just_evaluated = false;
    s_calc.repeat_op = '\0';
    s_calc.message[0] = '\0';
    s_calc.bill_cents = 0;
}

static void add_history(const char *expression, double result)
{
    if (s_calc.history_count > 0 &&
            strcmp(s_calc.history[0].expression, expression) == 0 &&
            fabs(s_calc.history[0].result - result) < 1e-12) {
        return;
    }
    const size_t move = s_calc.history_count < CALC_HISTORY_MAX
        ? s_calc.history_count : CALC_HISTORY_MAX - 1U;
    if (move > 0) {
        memmove(&s_calc.history[1], &s_calc.history[0],
                move * sizeof(s_calc.history[0]));
    }
    calc_history_t *entry = &s_calc.history[0];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->expression, sizeof(entry->expression), "%s", expression);
    entry->result = result;
    entry->has_result = true;
    entry->just_evaluated = true;
    if (s_calc.history_count < CALC_HISTORY_MAX) {
        s_calc.history_count++;
    }
}

static void commit_result(void)
{
    if (s_calc.expression[0] == '\0') {
        return;
    }
    if (s_calc.just_evaluated && s_calc.repeat_op != '\0') {
        char left[32];
        char right[32];
        char repeated[sizeof(left) + sizeof(right) + 2U];
        format_number(s_calc.result, left, sizeof(left));
        format_number(s_calc.repeat_operand, right, sizeof(right));
        snprintf(repeated, sizeof(repeated), "%s%c%s",
                 left, s_calc.repeat_op, right);
        if (strlen(repeated) > CALC_EXPR_MAX) {
            snprintf(s_calc.message, sizeof(s_calc.message),
                     "RESULT OUT OF RANGE");
            return;
        }
        memcpy(s_calc.expression, repeated, strlen(repeated) + 1U);
    }
    calc_value_t value;
    const calc_error_t error = evaluate(s_calc.expression, &value);
    if (error != CALC_OK) {
        const char *message = error == CALC_DIVIDE_ZERO
            ? "DIVIDE BY 0"
            : error == CALC_OVERFLOW
                ? "RESULT OUT OF RANGE"
                : error == CALC_INCOMPLETE
                    ? "COMPLETE THE EXPRESSION"
                    : "CHECK THE EXPRESSION";
        snprintf(s_calc.message, sizeof(s_calc.message), "%s", message);
        s_calc.just_evaluated = false;
        return;
    }
    s_calc.result = value.value;
    s_calc.has_result = true;
    s_calc.just_evaluated = true;
    s_calc.repeat_op = value.repeat_op;
    s_calc.repeat_operand = value.repeat_operand;
    s_calc.message[0] = '\0';
    s_calc.bill_cents = value.value > 0
        ? (int64_t)llround(value.value * 100.0) : 0;
    add_history(s_calc.expression, value.value);
}

static void display_expression(char *buffer, size_t size)
{
    if (s_calc.expression[0] == '\0') {
        snprintf(buffer, size, "ENTER A CALCULATION");
        return;
    }
    size_t out = 0;
    for (size_t in = 0; s_calc.expression[in] != '\0' && out + 4U < size;
            ++in) {
        const char *replacement = NULL;
        if (s_calc.expression[in] == '*') replacement = "×";
        if (s_calc.expression[in] == '/') replacement = "÷";
        if (s_calc.expression[in] == '-') replacement = "−";
        if (replacement != NULL) {
            const size_t length = strlen(replacement);
            memcpy(buffer + out, replacement, length);
            out += length;
        } else {
            buffer[out++] = s_calc.expression[in];
        }
    }
    buffer[out] = '\0';
}

static void render_main(esp_gsp_handle_t ui)
{
    char expression[128];
    char result[48];
    display_expression(expression, sizeof(expression));
    if (s_calc.message[0] != '\0') {
        snprintf(result, sizeof(result), "%s", s_calc.message);
    } else {
        format_number(s_calc.has_result ? s_calc.result : 0,
                      result, sizeof(result));
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_CALC_EXPRESSION, expression);
    (void)esp_gsp_set_text(ui, GSP_BIND_CALC_RESULT, result);
}

static void render_split(esp_gsp_handle_t ui)
{
    const int64_t bill = s_calc.bill_cents > 0 ? s_calc.bill_cents : 0;
    const bool too_large = bill > CALC_MAX_BILL_CENTS;
    const int64_t tip = too_large
        ? 0 : (bill * s_calc.tip_percent + 50) / 100;
    const int64_t total = too_large ? 0 : bill + tip;
    const int64_t each = too_large
        ? 0 : (total + s_calc.people - 1) / s_calc.people;
    const int64_t lower = too_large ? 0 : total / s_calc.people;
    const int lower_people = too_large || each == lower
        ? 0 : s_calc.people - (int)(total % s_calc.people);

    char each_text[32], bill_text[32], tip_text[32], total_text[32];
    char remainder[48], source[64];
    if (too_large) {
        snprintf(each_text, sizeof(each_text), "TOO HIGH");
        snprintf(bill_text, sizeof(bill_text), "MAX");
        snprintf(tip_text, sizeof(tip_text), "-");
        snprintf(total_text, sizeof(total_text), "-");
        snprintf(remainder, sizeof(remainder), "MAX $9999.99");
        snprintf(source, sizeof(source), "BILL TOO LARGE · EDIT BILL");
        (void)esp_gsp_set_text(
            ui, GSP_BIND_SPLIT_EACH_LABEL, "SPLIT UNAVAILABLE");
    } else {
        format_currency(each, each_text, sizeof(each_text));
        format_currency(bill, bill_text, sizeof(bill_text));
        format_currency(tip, tip_text, sizeof(tip_text));
        format_currency(total, total_text, sizeof(total_text));
        if (lower_people > 0) {
            char lower_text[24];
            format_currency(lower, lower_text, sizeof(lower_text));
            snprintf(remainder, sizeof(remainder), "%d PAY %s",
                     lower_people, lower_text);
        } else {
            remainder[0] = '\0';
        }
        if (bill == 0) {
            snprintf(source, sizeof(source), "ENTER BILL IN CALC · OPEN CALC");
            (void)esp_gsp_set_text(
                ui, GSP_BIND_SPLIT_EACH_LABEL, "ENTER BILL FIRST");
        } else {
            snprintf(source, sizeof(source), "BILL · %s · EDIT BILL",
                     bill_text);
            (void)esp_gsp_set_text(
                ui, GSP_BIND_SPLIT_EACH_LABEL, "PER PERSON");
        }
    }
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_EACH, each_text);
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_REMAINDER, remainder);
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_BILL, bill_text);
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_TIP, tip_text);
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_TOTAL, total_text);
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_SOURCE, source);
    char people[8];
    snprintf(people, sizeof(people), "%d", s_calc.people);
    (void)esp_gsp_set_text(ui, GSP_BIND_SPLIT_PEOPLE, people);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_SPLIT_TIP_0_SELECTED, s_calc.tip_percent == 0);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_SPLIT_TIP_10_SELECTED, s_calc.tip_percent == 10);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_SPLIT_TIP_15_SELECTED, s_calc.tip_percent == 15);
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_SPLIT_TIP_20_SELECTED, s_calc.tip_percent == 20);
}

static void render_history(esp_gsp_handle_t ui)
{
    char count[24];
    snprintf(count, sizeof(count), "%u %s",
             (unsigned)s_calc.history_count,
             s_calc.history_count == 1U ? "RESULT" : "RESULTS");
    (void)esp_gsp_set_text(ui, GSP_BIND_HISTORY_COUNT, count);
    (void)esp_gsp_set_text(
        ui, GSP_BIND_HISTORY_CLEAR_TEXT,
        s_calc.clear_armed ? "SURE?" : "CLEAR");
    (void)esp_gsp_set_visible(
        ui, GSP_BIND_HISTORY_EMPTY_VISIBLE, s_calc.history_count == 0U);

    static const uint16_t visible_binds[CALC_HISTORY_MAX] = {
        GSP_BIND_HISTORY_ROW_0_VISIBLE,
        GSP_BIND_HISTORY_ROW_1_VISIBLE,
        GSP_BIND_HISTORY_ROW_2_VISIBLE,
        GSP_BIND_HISTORY_ROW_3_VISIBLE,
        GSP_BIND_HISTORY_ROW_4_VISIBLE,
    };
    static const uint16_t text_binds[CALC_HISTORY_MAX] = {
        GSP_BIND_HISTORY_ROW_0_TEXT,
        GSP_BIND_HISTORY_ROW_1_TEXT,
        GSP_BIND_HISTORY_ROW_2_TEXT,
        GSP_BIND_HISTORY_ROW_3_TEXT,
        GSP_BIND_HISTORY_ROW_4_TEXT,
    };
    for (size_t index = 0; index < CALC_HISTORY_MAX; ++index) {
        const bool visible = index < s_calc.history_count;
        (void)esp_gsp_set_visible(ui, visible_binds[index], visible);
        if (visible) {
            char expression[CALC_EXPR_MAX + 1U];
            char result[32];
            snprintf(expression, sizeof(expression), "%s",
                     s_calc.history[index].expression);
            for (char *cursor = expression; *cursor; ++cursor) {
                if (*cursor == '*') *cursor = 'x';
            }
            format_number(s_calc.history[index].result,
                          result, sizeof(result));
            char row[96];
            snprintf(row, sizeof(row), "%s    = %s", expression, result);
            (void)esp_gsp_set_text(ui, text_binds[index], row);
        }
    }
}

static void render_all(esp_gsp_handle_t ui)
{
    render_main(ui);
    render_split(ui);
    render_history(ui);
}

static void show_page(esp_gsp_handle_t ui, uint16_t page, bool animated)
{
    if (page == s_calc.page || page > CALC_PAGE_HISTORY) {
        return;
    }
    esp_err_t error;
    if (page == CALC_PAGE_MAIN) {
        error = esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_CALCULATOR_STACK, animated);
    } else if (s_calc.page == CALC_PAGE_MAIN) {
        error = esp_gsp_stack_view_push(
            ui, GSP_OBJ_KEY_CALCULATOR_STACK, page, animated);
    } else {
        error = esp_gsp_stack_view_pop(
            ui, GSP_OBJ_KEY_CALCULATOR_STACK, false);
        if (error == ESP_OK) {
            error = esp_gsp_stack_view_push(
                ui, GSP_OBJ_KEY_CALCULATOR_STACK, page, animated);
        }
    }
    if (error == ESP_OK) {
        s_calc.page = page;
        s_calc.clear_armed = false;
    }
}

static void open_split(esp_gsp_handle_t ui)
{
    s_calc.bill_cents =
        s_calc.has_result && s_calc.result > 0
        ? (int64_t)llround(s_calc.result * 100.0) : 0;
    render_split(ui);
    show_page(ui, CALC_PAGE_SPLIT, true);
}

static void load_history(esp_gsp_handle_t ui, size_t index)
{
    if (index >= s_calc.history_count) {
        return;
    }
    s_calc.result = s_calc.history[index].result;
    s_calc.has_result = true;
    s_calc.just_evaluated = true;
    s_calc.repeat_op = '\0';
    s_calc.message[0] = '\0';
    format_number(s_calc.result, s_calc.expression,
                  sizeof(s_calc.expression));
    render_main(ui);
    show_page(ui, CALC_PAGE_MAIN, true);
}

static void handle_action(esp_gsp_handle_t ui, uint16_t action)
{
    if (action <= GSP_ACT_ID_CALC_9) {
        const char digit = action == GSP_ACT_ID_CALC_0
            ? '0' : (char)('1' + action - GSP_ACT_ID_CALC_1);
        append_digit(digit);
        render_main(ui);
        return;
    }
    switch (action) {
    case GSP_ACT_ID_CALC_CLEAR:
        clear_calculation();
        break;
    case GSP_ACT_ID_CALC_BACKSPACE:
        backspace();
        break;
    case GSP_ACT_ID_CALC_PERCENT:
        append_percent();
        break;
    case GSP_ACT_ID_CALC_DIVIDE:
        append_operator('/');
        break;
    case GSP_ACT_ID_CALC_MULTIPLY:
        append_operator('*');
        break;
    case GSP_ACT_ID_CALC_SUBTRACT:
        append_operator('-');
        break;
    case GSP_ACT_ID_CALC_ADD:
        append_operator('+');
        break;
    case GSP_ACT_ID_CALC_DECIMAL:
        append_decimal();
        break;
    case GSP_ACT_ID_CALC_SIGN:
        toggle_sign();
        break;
    case GSP_ACT_ID_CALC_PAREN:
        toggle_parenthesis();
        break;
    case GSP_ACT_ID_CALC_EQUAL:
        commit_result();
        break;
    case GSP_ACT_ID_CALC_SPLIT:
        open_split(ui);
        return;
    case GSP_ACT_ID_CALC_HISTORY:
        render_history(ui);
        show_page(ui, CALC_PAGE_HISTORY, true);
        return;
    case GSP_ACT_ID_CALC_BACK:
        show_page(ui, CALC_PAGE_MAIN, true);
        return;
    case GSP_ACT_ID_SPLIT_TIP_0:
        s_calc.tip_percent = 0;
        render_split(ui);
        return;
    case GSP_ACT_ID_SPLIT_TIP_10:
        s_calc.tip_percent = 10;
        render_split(ui);
        return;
    case GSP_ACT_ID_SPLIT_TIP_15:
        s_calc.tip_percent = 15;
        render_split(ui);
        return;
    case GSP_ACT_ID_SPLIT_TIP_20:
        s_calc.tip_percent = 20;
        render_split(ui);
        return;
    case GSP_ACT_ID_SPLIT_PEOPLE_MINUS:
        if (s_calc.people > 1) s_calc.people--;
        render_split(ui);
        return;
    case GSP_ACT_ID_SPLIT_PEOPLE_PLUS:
        if (s_calc.people < 12) s_calc.people++;
        render_split(ui);
        return;
    case GSP_ACT_ID_SPLIT_BILL_EDIT:
        if (s_calc.bill_cents > 0) {
            s_calc.result = (double)s_calc.bill_cents / 100.0;
            s_calc.has_result = true;
            s_calc.just_evaluated = true;
            format_number(s_calc.result, s_calc.expression,
                          sizeof(s_calc.expression));
        }
        render_main(ui);
        show_page(ui, CALC_PAGE_MAIN, true);
        return;
    case GSP_ACT_ID_HISTORY_CLEAR:
        if (s_calc.history_count == 0U) {
            return;
        }
        if (!s_calc.clear_armed) {
            s_calc.clear_armed = true;
        } else {
            s_calc.history_count = 0;
            s_calc.clear_armed = false;
        }
        render_history(ui);
        return;
    case GSP_ACT_ID_HISTORY_LOAD_0:
        load_history(ui, 0);
        return;
    case GSP_ACT_ID_HISTORY_LOAD_1:
        load_history(ui, 1);
        return;
    case GSP_ACT_ID_HISTORY_LOAD_2:
        load_history(ui, 2);
        return;
    case GSP_ACT_ID_HISTORY_LOAD_3:
        load_history(ui, 3);
        return;
    case GSP_ACT_ID_HISTORY_LOAD_4:
        load_history(ui, 4);
        return;
    default:
        return;
    }
    render_main(ui);
}

static void calculator_started(esp_gsp_handle_t ui)
{
    (void)ui;
    if (!s_calc.initialized) {
        memset(&s_calc, 0, sizeof(s_calc));
        s_calc.result = 0;
        s_calc.has_result = true;
        s_calc.tip_percent = 15;
        s_calc.people = 4;
        s_calc.initialized = true;
    }
    s_calc.page = CALC_PAGE_MAIN;
    s_calc.clear_armed = false;
}

static void calculator_event(
    esp_gsp_handle_t ui, const struct mosaic_event *raw_event)
{
    const mosaic_event_t *event = raw_event;
    if (event == NULL) {
        return;
    }
    switch (event->type) {
    case MOSAIC_EVENT_START:
    case MOSAIC_EVENT_SCENE_CHANGED:
        render_all(ui);
        break;
    case MOSAIC_EVENT_UI_CALL:
        handle_action(ui, event->data.call.action_id);
        break;
    case MOSAIC_EVENT_STOP:
    case MOSAIC_EVENT_TIMER:
    case MOSAIC_EVENT_POINTER:
    case MOSAIC_EVENT_MODEL_CHANGED:
    default:
        break;
    }
}

static bool calculator_back(esp_gsp_handle_t ui, int64_t timestamp_us)
{
    (void)timestamp_us;
    if (s_calc.page == CALC_PAGE_MAIN) {
        return false;
    }
    show_page(ui, CALC_PAGE_MAIN, true);
    return true;
}

const mosaic_app_descriptor_t mosaic_calculator_app = {
    .id = CALCULATOR_APP_ID,
    .launch_action = GSP_ACT_ID_APP_CALCULATOR,
    .back_action = MOSAIC_APP_SHELL_BACK_ACTION,
    .back_exits_app = true,
    .name = "calculator",
    .title = "Calculator",
    .directory = &gsp_obj_directory_calculator,
    .root_stack_key = GSP_OBJ_KEY_CALCULATOR_STACK,
    .disable_swipe = true,
    .root_header_in_stack = true,
    .on_started = calculator_started,
    .on_event = calculator_event,
    .on_back = calculator_back,
};
