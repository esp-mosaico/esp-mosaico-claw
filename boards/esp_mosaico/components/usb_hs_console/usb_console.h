/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the USB-OTG CDC console.
 *
 * CONFIG_USB_HS_CONSOLE_USB_CDC_AUTO_INIT enables this automatically before
 * app_main(). Call it manually only when automatic initialization is disabled.
 * On success, stdin, stdout, stderr and subsequent ESP_LOG output use TinyUSB
 * CDC-ACM interface 0. Repeated calls are harmless.
 *
 * CONFIG_USB_HS_CONSOLE_USB_CDC_AUTO_DOWNLOAD independently enables the
 * USB-Serial/JTAG-compatible DTR/RTS reset behavior for firmware download.
 */
esp_err_t bsp_usb_console_init(void);

/** @return true after the USB CDC console has initialized successfully. */
bool bsp_usb_console_is_initialized(void);

#ifdef __cplusplus
}
#endif
