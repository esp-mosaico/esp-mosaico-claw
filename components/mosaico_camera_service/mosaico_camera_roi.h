/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "mosaico_module_camera.h"

/* Before native buffers are allocated. The service serializes creation;
 * the installed sensor descriptor remains valid for the firmware lifetime. */
esp_err_t mosaico_camera_configure_equivalent_roi(mosaico_camera_handle_t camera);
