/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mosaic_setup.h"

#define SETUP_WECHAT_EVENT_CAPACITY 8U

typedef struct {
    mosaic_setup_wechat_status_t status;
    uint32_t revision;
} setup_wechat_event_t;

typedef struct {
    setup_wechat_event_t events[SETUP_WECHAT_EVENT_CAPACITY];
    uint8_t head;
    uint8_t count;
} setup_wechat_event_queue_t;

void setup_wechat_event_queue_reset(setup_wechat_event_queue_t *queue);
void setup_wechat_event_queue_push(setup_wechat_event_queue_t *queue,
                                   const mosaic_setup_wechat_status_t *status,
                                   uint32_t revision);
bool setup_wechat_event_queue_pop(setup_wechat_event_queue_t *queue,
                                  setup_wechat_event_t *event);
