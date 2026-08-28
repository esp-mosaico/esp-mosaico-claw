/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "soc/lp_system_reg.h"
#include "soc/soc.h"

/* LP STORE15 is not assigned by the ESP32-S31 ROM/IDF startup ABI.  The
 * bootloader publishes this one-shot marker only after the panel is on and
 * the splash has been completely written.  The App consumes it before board
 * manager creates the LCD device. */
#define MOSAICO_BOOT_LCD_HANDOFF_REG   LP_SYSTEM_REG_LP_STORE15_REG
#define MOSAICO_BOOT_LCD_HANDOFF_MAGIC UINT32_C(0x4D4C4344) /* "MLCD" */

static inline void mosaico_boot_handoff_clear(void)
{
    REG_WRITE(MOSAICO_BOOT_LCD_HANDOFF_REG, 0);
}

static inline void mosaico_boot_handoff_publish(void)
{
    REG_WRITE(MOSAICO_BOOT_LCD_HANDOFF_REG, MOSAICO_BOOT_LCD_HANDOFF_MAGIC);
}

static inline bool mosaico_boot_handoff_consume(void)
{
    const bool ready =
        REG_READ(MOSAICO_BOOT_LCD_HANDOFF_REG) == MOSAICO_BOOT_LCD_HANDOFF_MAGIC;
    mosaico_boot_handoff_clear();
    return ready;
}
