/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "mosaic_module_notice.h"

static mosaico_module_mgr_info_t sample(unsigned side, uint32_t generation,
    mosaico_module_presence_t presence, uint8_t type, bool valid)
{
    return (mosaico_module_mgr_info_t) {
        .slot = side, .generation = generation, .presence = presence,
        .descriptor_state = valid ? MOSAICO_MODULE_DESCRIPTOR_VALID
                                  : MOSAICO_MODULE_DESCRIPTOR_UNKNOWN,
        .eeprom = { .board_type = type },
    };
}

static void observe(mosaic_module_notice_t *state, unsigned side, uint32_t generation,
    mosaico_module_presence_t presence, uint8_t type, bool valid, int64_t now, bool allow)
{
    const mosaico_module_mgr_info_t info = sample(side, generation, presence, type, valid);
    assert(mosaic_module_notice_observe(state, &info, now, allow));
}

static void boot_empty(mosaic_module_notice_t *state)
{
    *state = (mosaic_module_notice_t) {0};
    for (unsigned i = 0; i < 2; ++i) {
        observe(state, i, 1, MOSAICO_MODULE_PRESENCE_UNKNOWN, 0, false, 0, true);
        /* Initial ABSENT may arrive only through get_info(), without an event. */
        observe(state, i, 2, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 100, true);
    }
}

static void test_boot_and_delayed_identification_are_silent(void)
{
    for (unsigned mask = 1; mask <= 3; ++mask) {
        mosaic_module_notice_t state = {0};
        for (unsigned i = 0; i < 2; ++i) {
            observe(&state, i, 1, mask & (1U << i) ? MOSAICO_MODULE_PRESENCE_PRESENT
                : MOSAICO_MODULE_PRESENCE_ABSENT, MOSAICO_BOARD_TYPE_CAMERA, false, 0, true);
            observe(&state, i, 2, state.slots[i].presence,
                MOSAICO_BOARD_TYPE_CAMERA, true, 3000000, true);
        }
        assert(mosaic_module_notice_poll(&state, 4000000, true).mask == 0);
    }
    puts("PASS single/dual boot discovery and delayed descriptor recovery stay silent");
}

static void test_first_hotplug_and_dual_merge(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1000000, true);
    assert(mosaic_module_notice_poll(&state, 1300000, true).mask == 0);
    observe(&state, 1, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_INTERACT, true, 1300000, true);
    mosaic_module_notice_view_t view = mosaic_module_notice_poll(&state, 1400000, true);
    assert(view.mask == 3 && view.board_type[0] == MOSAICO_BOARD_TYPE_CAMERA &&
        view.board_type[1] == MOSAICO_BOARD_TYPE_INTERACT);
    assert(mosaic_module_notice_poll(&state, 3900000, true).mask == 0);
    puts("PASS the first real insertion is retained and both sides merge into one expiring notice");
}

static void test_retries_and_ownership_do_not_repeat(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 3, MOSAICO_MODULE_PRESENCE_PRESENT, 0, false, 1000000, true);
    observe(&state, 0, 4, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 3000000, true);
    assert(mosaic_module_notice_poll(&state, 3400000, true).mask == 1);
    assert(mosaic_module_notice_poll(&state, 6000000, true).mask == 0);
    observe(&state, 0, 5, MOSAICO_MODULE_PRESENCE_UNKNOWN, 0, false, 6100000, true);
    observe(&state, 0, 6, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 6200000, true);
    observe(&state, 0, 7, MOSAICO_MODULE_PRESENCE_PRESENT, 0, false, 6300000, true);
    observe(&state, 0, 8, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 6400000, true);
    assert(mosaic_module_notice_poll(&state, 7000000, true).mask == 0);
    puts("PASS runtime identification reports once; retries and claim/release aren't insertions");
}

static void test_removal_cancels_pending_and_visible(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1000000, true);
    observe(&state, 0, 4, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 1100000, true);
    assert(mosaic_module_notice_poll(&state, 1500000, true).mask == 0);
    observe(&state, 0, 5, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 2000000, true);
    observe(&state, 1, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_INTERACT, true, 2100000, true);
    assert(mosaic_module_notice_poll(&state, 2400000, true).mask == 3);
    observe(&state, 0, 6, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 2500000, true);
    assert(mosaic_module_notice_poll(&state, 2500000, true).mask == 2);
    observe(&state, 1, 4, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 2600000, true);
    assert(mosaic_module_notice_poll(&state, 2600000, true).mask == 0);
    puts("PASS unplug cancels pending feedback and removes only the affected visible side");
}

static void test_busy_sleep_and_stalled_ui_do_not_replay(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 3, MOSAICO_MODULE_PRESENCE_PRESENT, 0, false, 1000000, true);
    assert(mosaic_module_notice_poll(&state, 1100000, false).mask == 0);
    observe(&state, 0, 4, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1200000, true);
    assert(mosaic_module_notice_poll(&state, 1700000, true).mask == 0);
    observe(&state, 1, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_INTERACT, true, 2000000, false);
    assert(mosaic_module_notice_poll(&state, 2500000, true).mask == 0);
    observe(&state, 1, 4, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 3000000, true);
    observe(&state, 1, 5, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_INTERACT, true, 3100000, true);
    assert(mosaic_module_notice_poll(&state, 6000000, true).mask == 0);
    puts("PASS inactive/sleeping UI, delayed identification and stale pending work never replay");
}

static void test_generation_order_and_unsupported_boards(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 4, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 0, true);
    mosaico_module_mgr_info_t old = sample(0, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true);
    assert(!mosaic_module_notice_observe(&state, &old, 1000000, true));
    assert(state.slots[0].presence == MOSAICO_MODULE_PRESENCE_ABSENT);
    observe(&state, 0, 5, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_SENSOR, true, 1000000, true);
    assert(mosaic_module_notice_poll(&state, 1500000, true).mask == 0);
    state.slots[1].generation = UINT32_MAX;
    observe(&state, 1, 0, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_INTERACT, true, 2000000, true);
    assert(mosaic_module_notice_poll(&state, 2400000, true).mask == 2);
    old.slot = MOSAICO_MODULE_MGR_SLOT_COUNT;
    assert(!mosaic_module_notice_observe(&state, &old, 2500000, true));
    puts("PASS stale snapshots, generation wrap and unsupported slots/types are handled");
}

static void test_publication_ack_does_not_consume_a_newer_insert(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1000000, true);
    const mosaic_module_notice_view_t old = mosaic_module_notice_poll(&state, 1400000, true);
    observe(&state, 0, 4, MOSAICO_MODULE_PRESENCE_ABSENT, 0, false, 1500000, true);
    observe(&state, 0, 5, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1600000, true);
    mosaic_module_notice_view_t current = mosaic_module_notice_poll(&state, 2000000, true);
    assert(current.mask == 1 && current.generation[0] == 5);
    mosaic_module_notice_ack(&state, &old);
    current = mosaic_module_notice_poll(&state, 2100000, true);
    assert(current.mask == 1);
    mosaic_module_notice_ack(&state, &current);
    assert(mosaic_module_notice_poll(&state, 2200000, true).mask == 0);
    puts("PASS a delivered introduction is consumed once without losing a newer insertion during UI publication");
}

static void test_owner_revision_does_not_replay_a_delivered_introduction(void)
{
    mosaic_module_notice_t state;
    boot_empty(&state);
    observe(&state, 0, 3, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1000000, true);
    const mosaic_module_notice_view_t delivered = mosaic_module_notice_poll(&state, 1400000, true);
    /* Claim/error revisions can arrive while the render task publishes. They
     * are the same physical insertion, so Done must not replay that delivery. */
    observe(&state, 0, 4, MOSAICO_MODULE_PRESENCE_PRESENT,
        MOSAICO_BOARD_TYPE_CAMERA, true, 1450000, true);
    mosaic_module_notice_ack(&state, &delivered);
    assert(mosaic_module_notice_poll(&state, 1500000, true).mask == 0);
    puts("PASS ownership revisions during publication never replay a delivered introduction");
}

int main(void)
{
    test_boot_and_delayed_identification_are_silent();
    test_first_hotplug_and_dual_merge();
    test_retries_and_ownership_do_not_repeat();
    test_removal_cancels_pending_and_visible();
    test_busy_sleep_and_stalled_ui_do_not_replay();
    test_generation_order_and_unsupported_boards();
    test_publication_ack_does_not_consume_a_newer_insert();
    test_owner_revision_does_not_replay_a_delivered_introduction();
    printf("Module notice state: %zu bytes, fixed allocation\n", sizeof(mosaic_module_notice_t));
    return 0;
}
