/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "hal/spi_hal.h"
#include "hal/spi_ll.h"
#include "mosaic_boot_logo.h"
#include "mosaico_boot_handoff.h"
#include "sdkconfig.h"
#include "soc/gpio_sig_map.h"
#include "soc/spi_periph.h"

#define LCD_WIDTH              480U
#define LCD_HEIGHT             480U
#define LCD_LOGO_X             ((LCD_WIDTH - MOSAIC_BOOT_LOGO_WIDTH) / 2U)
#define LCD_LOGO_Y             ((LCD_HEIGHT - MOSAIC_BOOT_LOGO_HEIGHT) / 2U)
#define LCD_POWER_GPIO         60
#define LCD_RESET_GPIO         42
#define LCD_CS_GPIO            50
#define LCD_CLK_GPIO           44
#define LCD_DATA0_GPIO         36
#define LCD_DATA1_GPIO         51
#define LCD_DATA2_GPIO         35
#define LCD_DATA3_GPIO         9
#define LCD_SPI_CLOCK_HZ       40000000U
#define LCD_SPI_SOURCE_HZ      40000000U
#define LCD_FIFO_BYTES         64U
#define LCD_CMD_WRITE          0x02U
#define LCD_COLOR_WRITE        0x32U
#define LCD_BOOT_BRIGHTNESS    0x66U /* 40% of the 8-bit brightness range. */
#define MOSAICO_HW_VERSION(major, minor) ((uint16_t)(((uint16_t)(major) << 8) | ((uint16_t)(minor) & 0xFFU)))
#define MOSAICO_HW_VERSION_MAJOR(version) ((uint8_t)((version) >> 8))
#define MOSAICO_HW_VERSION_MINOR(version) ((uint8_t)((version) & 0xFFU))

static const char *TAG = "boot_splash";
static spi_hal_context_t s_spi;
static spi_hal_dev_config_t s_spi_device;

static bool hardware_version_supported(void)
{
    uint16_t version = 0;
    const esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version, sizeof(version) * 8U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hardware version read failed");
        return false;
    }
    if (version != MOSAICO_HW_VERSION(1, 0)) {
        ESP_LOGE(TAG, "unsupported hardware version: v%u.%u (raw=0x%04X)", MOSAICO_HW_VERSION_MAJOR(version), MOSAICO_HW_VERSION_MINOR(version), version);
        return false;
    }
    ESP_LOGW(TAG, "hardware version: v%u.%u", MOSAICO_HW_VERSION_MAJOR(version), MOSAICO_HW_VERSION_MINOR(version));
    return true;
}

static void gpio_output(int gpio, int level)
{
    esp_rom_gpio_pad_select_gpio(gpio);
    gpio_ll_set_level(&GPIO, gpio, level);
    gpio_ll_output_enable(&GPIO, gpio);
}

static void route_spi_output(int gpio, int signal)
{
    esp_rom_gpio_pad_select_gpio(gpio);
    esp_rom_gpio_connect_out_signal(gpio, signal, false, false);
    gpio_ll_output_enable(&GPIO, gpio);
}

static void spi_wait(void)
{
    while (!spi_hal_usr_is_done(&s_spi)) {
    }
}

static void spi_tx(const void *data, size_t size, uint8_t lines,
                   bool keep_cs_active)
{
    const uint8_t *cursor = data;
    while (size > 0) {
        uint8_t fifo[LCD_FIFO_BYTES] = {0};
        const size_t chunk = size > sizeof(fifo) ? sizeof(fifo) : size;
        memcpy(fifo, cursor, chunk);
        const spi_hal_trans_config_t trans = {
            .tx_bitlen = (int)(chunk * 8U),
            .send_buffer = fifo,
            .line_mode = {
                .cmd_lines = 1,
                .addr_lines = 1,
                .data_lines = lines,
            },
            .cs_keep_active = keep_cs_active || chunk < size,
        };
        spi_hal_setup_trans(&s_spi, &s_spi_device, &trans);
        spi_hal_push_tx_buffer(&s_spi, &trans);
        spi_hal_user_start(&s_spi);
        spi_wait();
        cursor += chunk;
        size -= chunk;
    }
}

static void lcd_command(uint8_t command, const void *params, size_t param_size)
{
    uint8_t packet[16] = {LCD_CMD_WRITE, 0x00, command, 0x00};
    if (param_size > sizeof(packet) - 4U) {
        return;
    }
    if (params != NULL && param_size > 0) {
        memcpy(&packet[4], params, param_size);
    }
    spi_tx(packet, 4U + param_size, 1, false);
}

static void lcd_set_window(uint16_t x, uint16_t y, uint16_t width,
                           uint16_t height)
{
    const uint16_t x_end = x + width - 1U;
    const uint16_t y_end = y + height - 1U;
    const uint8_t columns[] = {
        (uint8_t)(x >> 8), (uint8_t)x,
        (uint8_t)(x_end >> 8), (uint8_t)x_end,
    };
    const uint8_t rows[] = {
        (uint8_t)(y >> 8), (uint8_t)y,
        (uint8_t)(y_end >> 8), (uint8_t)y_end,
    };
    lcd_command(0x2A, columns, sizeof(columns));
    lcd_command(0x2B, rows, sizeof(rows));
}

static void lcd_begin_pixels(void)
{
    const uint8_t command[] = {LCD_COLOR_WRITE, 0x00, 0x2C, 0x00};
    spi_tx(command, sizeof(command), 1, true);
}

static void lcd_write_solid(uint16_t color, size_t pixels, bool keep_cs)
{
    uint8_t fifo[LCD_FIFO_BYTES];
    for (size_t i = 0; i < sizeof(fifo); i += 2U) {
        fifo[i] = (uint8_t)(color >> 8);
        fifo[i + 1U] = (uint8_t)color;
    }
    while (pixels > 0) {
        const size_t chunk_pixels =
            pixels > sizeof(fifo) / 2U ? sizeof(fifo) / 2U : pixels;
        pixels -= chunk_pixels;
        spi_tx(fifo, chunk_pixels * 2U, 4, keep_cs || pixels > 0);
    }
}

static void lcd_write_logo_row(size_t row, bool keep_cs)
{
    uint8_t fifo[LCD_FIFO_BYTES];
    size_t used = 0;
    const size_t first_pixel = row * MOSAIC_BOOT_LOGO_WIDTH;
    const size_t end_pixel = first_pixel + MOSAIC_BOOT_LOGO_WIDTH;
    for (size_t pixel = first_pixel; pixel < end_pixel; ++pixel) {
        const uint8_t mask = 0x80U >> (pixel & 7U);
        const uint16_t color = (s_mosaic_boot_logo_bitmap[pixel >> 3] & mask) ?
                               MOSAIC_BOOT_LOGO_COLOR : 0;
        fifo[used++] = (uint8_t)(color >> 8);
        fifo[used++] = (uint8_t)color;
        if (used == sizeof(fifo)) {
            spi_tx(fifo, used, 4, keep_cs || pixel + 1U < end_pixel);
            used = 0;
        }
    }
    if (used > 0) {
        spi_tx(fifo, used, 4, keep_cs);
    }
}

static void spi_init(void)
{
    int __DECLARE_RCC_ATOMIC_ENV __attribute__((unused));
    spi_ll_enable_bus_clock(SPI2_HOST, true);
    spi_ll_reset_register(SPI2_HOST);
    spi_ll_enable_clock(SPI2_HOST, true);
    spi_ll_set_clk_source(&GPSPI2, SPI_CLK_SRC_XTAL);
    spi_ll_clk_source_pre_div(&GPSPI2, 1, 1);

    route_spi_output(LCD_CLK_GPIO, SPI2_CK_PAD_OUT_IDX);
    route_spi_output(LCD_DATA0_GPIO, SPI2_D_PAD_OUT_IDX);
    route_spi_output(LCD_DATA1_GPIO, SPI2_Q_PAD_OUT_IDX);
    route_spi_output(LCD_DATA2_GPIO, SPI2_WP_PAD_OUT_IDX);
    route_spi_output(LCD_DATA3_GPIO, SPI2_HOLD_PAD_OUT_IDX);
    route_spi_output(LCD_CS_GPIO, SPI2_CS_PAD_OUT_IDX);

    spi_hal_init(&s_spi, SPI2_HOST);
    s_spi_device = (spi_hal_dev_config_t) {
        .mode = 0,
        .cs_pin_id = 0,
        .half_duplex = 1,
        .timing_conf = {
            .clock_source = SPI_CLK_SRC_XTAL,
            .source_pre_div = 1,
            .source_real_freq = LCD_SPI_SOURCE_HZ,
            .expect_freq = LCD_SPI_CLOCK_HZ,
            .real_freq = LCD_SPI_CLOCK_HZ,
        },
    };
    spi_ll_master_cal_clock(LCD_SPI_SOURCE_HZ, LCD_SPI_CLOCK_HZ, 128,
                            &s_spi_device.timing_conf.clock_reg);
    spi_hal_setup_device(&s_spi, &s_spi_device);
    spi_hal_enable_data_line(s_spi.hw, true, false);
}

static void panel_init(void)
{
    static const struct {
        uint8_t command;
        uint8_t data[4];
        uint8_t size;
    } init[] = {
        {0xFE, {0x20}, 1},
        {0x19, {0x10}, 1},
        {0x1C, {0xA0}, 1},
        {0xFE, {0x00}, 1},
        {0xC4, {0x80}, 1},
        {0x3A, {0x55}, 1},
#if CONFIG_DISPLAY_SERVICE_PRESENT_TE_ENABLE
        {0x35, {0x00}, 1},
#endif
        {0x53, {0x20}, 1},
        {0x51, {LCD_BOOT_BRIGHTNESS}, 1},
        {0x63, {0xFF}, 1},
        /* Match board_devices.yaml: no XY swap and no axis mirroring. */
        {0x36, {0x00}, 1},
    };

    gpio_output(LCD_POWER_GPIO, 0);
    gpio_output(LCD_RESET_GPIO, 0);
    esp_rom_delay_us(10000);
    gpio_ll_set_level(&GPIO, LCD_RESET_GPIO, 1);
    esp_rom_delay_us(150000);

    spi_init();
    lcd_command(0x11, NULL, 0);
    esp_rom_delay_us(60000);
    for (size_t i = 0; i < sizeof(init) / sizeof(init[0]); ++i) {
        lcd_command(init[i].command, init[i].data, init[i].size);
    }
}

static void draw_splash(void)
{
    lcd_set_window(0, 0, LCD_WIDTH, LCD_HEIGHT);
    lcd_begin_pixels();
    lcd_write_solid(0x0000, LCD_LOGO_Y * LCD_WIDTH, true);
    for (size_t row = 0; row < MOSAIC_BOOT_LOGO_HEIGHT; ++row) {
        lcd_write_solid(0x0000, LCD_LOGO_X, true);
        lcd_write_logo_row(row, true);
        lcd_write_solid(0x0000,
                        LCD_WIDTH - LCD_LOGO_X - MOSAIC_BOOT_LOGO_WIDTH,
                        true);
    }
    const size_t bottom_lines =
        LCD_HEIGHT - LCD_LOGO_Y - MOSAIC_BOOT_LOGO_HEIGHT;
    lcd_write_solid(0x0000, bottom_lines * LCD_WIDTH, false);
    lcd_command(0x29, NULL, 0);
}

void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    mosaico_boot_handoff_clear();
    if (!hardware_version_supported()) {
        ESP_LOGW(TAG, "LCD boot splash skipped");
        return;
    }
    ESP_LOGW(TAG, "LCD splash start");
    panel_init();
    ESP_LOGW(TAG, "LCD panel ready");
    draw_splash();
    mosaico_boot_handoff_publish();
    /* Bootloader log level is WARN in the product configuration.  Keep this
     * completion marker visible without enabling all bootloader INFO logs. */
    ESP_LOGW(TAG, "LCD boot splash visible");
}
