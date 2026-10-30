/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Minimal UTF-8 edit buffer (soft-keyboard input fields): append a
 * codepoint, delete the last character. The buffer stays NUL
 * terminated; capacity counts bytes excluding the terminator.
 */

/** Appends one codepoint as UTF-8. False when it does not fit (the
 *  buffer is unchanged) or the codepoint is invalid. */
bool gsp_text_edit_append(char *buffer, size_t capacity,
                          uint32_t codepoint);

/** Removes the last UTF-8 character. False on an empty buffer. */
bool gsp_text_edit_backspace(char *buffer);

/**
 * Copies UTF-8 without splitting a valid multibyte sequence at @p capacity.
 * Capacity counts bytes excluding the terminator. Invalid input bytes are
 * preserved for the text shaper's replacement-character handling.
 * Source and destination may overlap.
 * Returns true when the complete string fits.
 */
bool gsp_text_edit_copy(char *buffer, size_t capacity, const char *utf8);

#ifdef __cplusplus
}
#endif
