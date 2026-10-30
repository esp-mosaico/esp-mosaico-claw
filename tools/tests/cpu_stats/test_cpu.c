/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "cap_system_cpu.h"

static void check(uint32_t total, uint64_t idle, unsigned cores, double expected)
{
    double actual = -1.0;
    assert(cap_system_cpu_usage(total, idle, cores, &actual));
    assert(fabs(actual - expected) < 0.000001);
}

int main(void)
{
    check(1000, 250, 1, 75.0);
    check(1000, 1500, 2, 25.0);
    check(1000, 2000, 2, 0.0);
    check(1000, 0, 2, 100.0);
    check(UINT32_MAX, (uint64_t)UINT32_MAX * 2, 2, 0.0);
    double output = -1.0;
    assert(!cap_system_cpu_usage(0, 0, 2, &output));
    assert(!cap_system_cpu_usage(1000, 0, 0, &output));
    assert(!cap_system_cpu_usage(1000, 2001, 2, &output));
    assert(!cap_system_cpu_usage(1000, 1000, 2, NULL));
    assert(output == -1.0);
    puts("PASS single-core and dual-core capacity normalization");
    puts("PASS fully idle, fully busy and 64-bit aggregate capacity");
    puts("PASS invalid snapshots are rejected instead of reporting zero usage");
    return 0;
}
