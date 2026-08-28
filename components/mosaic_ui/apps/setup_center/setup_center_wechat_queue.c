/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "setup_center_wechat_queue.h"

#include <stddef.h>

void setup_wechat_event_queue_reset(setup_wechat_event_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    queue->head = 0;
    queue->count = 0;
}

void setup_wechat_event_queue_push(setup_wechat_event_queue_t *queue,
                                   const mosaic_setup_wechat_status_t *status,
                                   uint32_t revision)
{
    if (queue == NULL || status == NULL || revision == 0) {
        return;
    }
    if (queue->count > 0U) {
        const uint8_t last = (uint8_t)(
            (queue->head + queue->count - 1U) %
            SETUP_WECHAT_EVENT_CAPACITY);
        if (queue->events[last].status.state == status->state) {
            queue->events[last].status = *status;
            queue->events[last].revision = revision;
            return;
        }
    }
    if (queue->count == SETUP_WECHAT_EVENT_CAPACITY) {
        queue->head = (uint8_t)(
            (queue->head + 1U) % SETUP_WECHAT_EVENT_CAPACITY);
        --queue->count;
    }
    const uint8_t tail = (uint8_t)(
        (queue->head + queue->count) % SETUP_WECHAT_EVENT_CAPACITY);
    queue->events[tail].status = *status;
    queue->events[tail].revision = revision;
    ++queue->count;
}

bool setup_wechat_event_queue_pop(setup_wechat_event_queue_t *queue,
                                  setup_wechat_event_t *event)
{
    if (queue == NULL || event == NULL || queue->count == 0U) {
        return false;
    }
    *event = queue->events[queue->head];
    queue->head = (uint8_t)(
        (queue->head + 1U) % SETUP_WECHAT_EVENT_CAPACITY);
    --queue->count;
    return true;
}
