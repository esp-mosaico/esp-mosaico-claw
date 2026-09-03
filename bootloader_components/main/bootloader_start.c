/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <sys/reent.h>

#include "bootloader_common.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_struct.h"
#include "soc/soc_caps.h"

static int select_partition_number(bootloader_state_t *bs);

static bool factory_recovery_requested(void)
{
    const uint32_t pin = CONFIG_FACTORY_RECOVERY_BOOT_GPIO;

    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_input_enable(&GPIO, pin);
    esp_rom_gpio_pad_pullup_only(pin);

    if (gpio_ll_get_level(&GPIO, pin) != 0) {
        return false;
    }

    esp_rom_delay_us(CONFIG_FACTORY_RECOVERY_BOOT_DEBOUNCE_MS * 1000U);
    return gpio_ll_get_level(&GPIO, pin) == 0;
}

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

    bootloader_utility_load_boot_image(&bs, boot_index);
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        return INVALID_INDEX;
    }

    if (factory_recovery_requested()) {
        if (bs->factory.offset != 0 && bs->factory.size != 0) {
            return FACTORY_INDEX;
        }
    }

    /* ESP-IDF owns normal OTA, rollback and invalid-image selection. */
    return bootloader_utility_get_selected_boot_partition(bs);
}

#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
