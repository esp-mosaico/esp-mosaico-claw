/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "esp_gsp.h"
/** Lifecycle calls run on the loader; prepare/contact run on the UI task. */
void mosaic_hub_backdrop_start(esp_gsp_handle_t ui);
void mosaic_hub_backdrop_contact(esp_gsp_handle_t ui, bool pressed, bool prepare);
void mosaic_hub_backdrop_stop(esp_gsp_handle_t ui);
