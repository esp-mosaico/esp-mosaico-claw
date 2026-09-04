/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_board_manager_includes.h"
#include "esp_blockdev.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "gen_board_device_custom.h"
#include "setup_nand_flash.h"
#include "soc/spi_pins.h"
#include "spi_nand_flash.h"

static const char *TAG = "MOSAICO_NAND";

#define NAND_FLASH_HOST_ID                  SPI3_HOST
#define NAND_FLASH_DMA_CHAN                 SPI_DMA_CH_AUTO
#define NAND_FLASH_CLOCK_SPEED_HZ           (40 * 1000 * 1000)
#define NAND_FLASH_MAX_TRANSFER_SZ          (4 * 1024)
#define NAND_FLASH_QUEUE_SIZE               10
#define NAND_FLASH_GC_FACTOR                0
#define NAND_FLASH_SPI_FLAGS                SPI_DEVICE_HALFDUPLEX
#define NAND_FLASH_IO_MODE                  SPI_NAND_IO_MODE_QIO
#define NAND_FLASH_MOUNT_PATH               "/nand"
#define NAND_FLASH_PIN_MOSI                 GPIO_NUM_21
#define NAND_FLASH_PIN_MISO                 GPIO_NUM_22
#define NAND_FLASH_PIN_CLK                  GPIO_NUM_20
#define NAND_FLASH_PIN_CS                   GPIO_NUM_23
#define NAND_FLASH_PIN_WP                   GPIO_NUM_25
#define NAND_FLASH_PIN_HD                   GPIO_NUM_24

#define NAND_FLASH_CMD_GET_FEATURE          0x0F
#define NAND_FLASH_CMD_SET_FEATURE          0x1F
#define NAND_FLASH_CMD_READ_ID              0x9F
#define NAND_FLASH_FEATURE_CONFIG           0xB0
#define NAND_FLASH_CONFIG_QE_BIT            BIT(0)

#define NAND_FLASH_MFR_ALLIANCE             0x52
#define NAND_FLASH_MFR_WINBOND              0xEF
#define NAND_FLASH_MFR_GIGADEVICE           0xC8
#define NAND_FLASH_MFR_MICRON               0x2C
#define NAND_FLASH_MFR_ZETTA                0xBA
#define NAND_FLASH_MFR_XTX                  0x0B
#define NAND_FLASH_MFR_FM                   0xA1
#define NAND_FLASH_MFR_MACRONIX             0xC2

typedef struct {
    esp_blockdev_handle_t wl_bdl;
    spi_device_handle_t spi;
    bool bus_initialized;
    bool spi_added;
    bool mounted;
} nand_flash_handle_t;

static esp_err_t nand_flash_release_control_pin(gpio_num_t pin)
{
    esp_err_t ret;
    const gpio_config_t _gpio_config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    // Preload the output latch before enabling the pad to avoid briefly
    // asserting the active-low NAND HOLD#/WP# signals.
    ret = gpio_set_level(pin, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to preload NAND control GPIO%d: %s", pin, esp_err_to_name(ret));
    ret = gpio_config(&_gpio_config);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to configure NAND control GPIO%d: %s", pin, esp_err_to_name(ret));
    ret = gpio_set_level(pin, 1);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to release NAND control GPIO%d: %s", pin, esp_err_to_name(ret));
    return ESP_OK;
}

static esp_err_t nand_flash_read_u8(spi_device_handle_t spi, uint8_t command,
                                    uint8_t address, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(spi && value, ESP_ERR_INVALID_ARG, TAG,
                        "invalid NAND bootstrap read arguments");

    spi_transaction_ext_t transaction = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_RXDATA,
            .rxlength = 8,
            .addr = address,
            .cmd = command,
        },
        .address_bits = 8,
        .command_bits = 8,
        .dummy_bits = 0,
    };
    ESP_RETURN_ON_ERROR(
        spi_device_transmit(spi, (spi_transaction_t *)&transaction), TAG,
        "NAND bootstrap command 0x%02x failed", command);
    *value = transaction.base.rx_data[0];
    return ESP_OK;
}

static esp_err_t nand_flash_write_u8(spi_device_handle_t spi, uint8_t command,
                                     uint8_t address, uint8_t value)
{
    ESP_RETURN_ON_FALSE(spi, ESP_ERR_INVALID_ARG, TAG,
                        "invalid NAND bootstrap write arguments");

    spi_transaction_ext_t transaction = {
        .base = {
            .flags = SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_CMD |
                     SPI_TRANS_VARIABLE_DUMMY | SPI_TRANS_USE_TXDATA,
            .length = 8,
            .addr = address,
            .cmd = command,
        },
        .address_bits = 8,
        .command_bits = 8,
        .dummy_bits = 0,
    };
    transaction.base.tx_data[0] = value;
    return spi_device_transmit(spi, (spi_transaction_t *)&transaction);
}

static bool nand_flash_manufacturer_supported(uint8_t manufacturer)
{
    switch (manufacturer) {
    case NAND_FLASH_MFR_ALLIANCE:
    case NAND_FLASH_MFR_WINBOND:
    case NAND_FLASH_MFR_GIGADEVICE:
    case NAND_FLASH_MFR_MICRON:
    case NAND_FLASH_MFR_ZETTA:
    case NAND_FLASH_MFR_XTX:
    case NAND_FLASH_MFR_FM:
    case NAND_FLASH_MFR_MACRONIX:
        return true;
    default:
        return false;
    }
}

static bool nand_flash_manufacturer_requires_qe(uint8_t manufacturer)
{
    return manufacturer != NAND_FLASH_MFR_WINBOND &&
           manufacturer != NAND_FLASH_MFR_MICRON;
}

/*
 * The NAND driver discovers the chip with a single-line READ-ID before it sets
 * the vendor QE bit. On this board, mapping GPIO24/25 to SPI3 IO3/IO2 before
 * QE is set makes the chip interpret their initial levels as HOLD#/WP# and
 * READ-ID fails. Probe over a temporary SIO bus, enable QE using feature
 * commands, then rebuild the bus in its final QIO configuration. No array data
 * or FTL metadata is accessed by this bootstrap.
 */
static esp_err_t nand_flash_enable_quad_bootstrap(spi_device_handle_t spi)
{
    uint8_t manufacturer = 0;
    ESP_RETURN_ON_ERROR(
        nand_flash_read_u8(spi, NAND_FLASH_CMD_READ_ID, 0, &manufacturer),
        TAG, "failed to read NAND manufacturer during QIO bootstrap");
    ESP_RETURN_ON_FALSE(
        nand_flash_manufacturer_supported(manufacturer),
        ESP_ERR_INVALID_RESPONSE, TAG,
        "unsupported NAND manufacturer during QIO bootstrap: 0x%02x",
        manufacturer);

    if (!nand_flash_manufacturer_requires_qe(manufacturer)) {
        ESP_LOGI(TAG, "NAND QIO bootstrap manufacturer=0x%02x has no QE bit",
                 manufacturer);
        return ESP_OK;
    }

    uint8_t config = 0;
    ESP_RETURN_ON_ERROR(
        nand_flash_read_u8(spi, NAND_FLASH_CMD_GET_FEATURE,
                           NAND_FLASH_FEATURE_CONFIG, &config),
        TAG, "failed to read NAND config during QIO bootstrap");
    if ((config & NAND_FLASH_CONFIG_QE_BIT) == 0) {
        ESP_RETURN_ON_ERROR(
            nand_flash_write_u8(spi, NAND_FLASH_CMD_SET_FEATURE,
                                NAND_FLASH_FEATURE_CONFIG,
                                config | NAND_FLASH_CONFIG_QE_BIT),
            TAG, "failed to set NAND QE during QIO bootstrap");
        ESP_RETURN_ON_ERROR(
            nand_flash_read_u8(spi, NAND_FLASH_CMD_GET_FEATURE,
                               NAND_FLASH_FEATURE_CONFIG, &config),
            TAG, "failed to verify NAND QE during QIO bootstrap");
        ESP_RETURN_ON_FALSE(config & NAND_FLASH_CONFIG_QE_BIT,
                            ESP_ERR_INVALID_RESPONSE, TAG,
                            "NAND QE did not latch during QIO bootstrap");
    }

    ESP_LOGI(TAG, "NAND QIO bootstrap manufacturer=0x%02x config=0x%02x",
             manufacturer, config);
    return ESP_OK;
}

static void nand_flash_cleanup_partial(nand_flash_handle_t *handle, bool release_bdl)
{
    if (handle == NULL) {
        return;
    }

    if (release_bdl && handle->wl_bdl != NULL) {
        esp_err_t err = handle->wl_bdl->ops->release(handle->wl_bdl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to release NAND BDL during cleanup: %s", esp_err_to_name(err));
        }
        handle->wl_bdl = NULL;
    }

    if (handle->spi_added) {
        esp_err_t err = spi_bus_remove_device(handle->spi);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to remove NAND SPI device during cleanup: %s", esp_err_to_name(err));
        }
        handle->spi = NULL;
        handle->spi_added = false;
    }

    if (handle->bus_initialized) {
        esp_err_t err = spi_bus_free(NAND_FLASH_HOST_ID);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to free NAND SPI bus during cleanup: %s", esp_err_to_name(err));
        }
        handle->bus_initialized = false;
    }
}

static int nand_flash_init(void *config, int cfg_size, void **device_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && device_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid NAND flash config");
    ESP_RETURN_ON_FALSE(cfg_size == sizeof(dev_custom_nand_flash_config_t), ESP_ERR_INVALID_SIZE, TAG, "invalid NAND flash config size: %d", cfg_size);

    *device_handle = NULL;
    esp_err_t ret = ESP_OK;
    const dev_custom_nand_flash_config_t *cfg = (const dev_custom_nand_flash_config_t *)config;
    const spi_nand_flash_io_mode_t io_mode = NAND_FLASH_IO_MODE;
    const bool quad = io_mode == SPI_NAND_IO_MODE_QOUT || io_mode == SPI_NAND_IO_MODE_QIO;
    nand_flash_handle_t *handle = calloc(1, sizeof(*handle));
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG, "failed to allocate NAND flash handle");

    // Chip detection starts with a single-line READ-ID transaction, even in
    // QIO mode. Keep HOLD#/WP# high until SPI3 takes ownership of IO3/IO2;
    // the weak pull-ups preserve their inactive level while those outputs are
    // tri-stated during the initial single-line commands.
    ret = nand_flash_release_control_pin(NAND_FLASH_PIN_HD);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "failed to release NAND HOLD pin: %s", esp_err_to_name(ret));
    ret = nand_flash_release_control_pin(NAND_FLASH_PIN_WP);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "failed to release NAND WP pin: %s", esp_err_to_name(ret));

    const spi_device_interface_config_t dev_config = {
        .clock_speed_hz = NAND_FLASH_CLOCK_SPEED_HZ,
        .mode = 0,
        .spics_io_num = NAND_FLASH_PIN_CS,
        .queue_size = NAND_FLASH_QUEUE_SIZE,
        .flags = NAND_FLASH_SPI_FLAGS,
    };

    if (quad) {
        const spi_bus_config_t bootstrap_bus_config = {
            .mosi_io_num = NAND_FLASH_PIN_MOSI,
            .miso_io_num = NAND_FLASH_PIN_MISO,
            .sclk_io_num = NAND_FLASH_PIN_CLK,
            .quadhd_io_num = GPIO_NUM_NC,
            .quadwp_io_num = GPIO_NUM_NC,
            .max_transfer_sz = NAND_FLASH_MAX_TRANSFER_SZ,
        };
        ret = spi_bus_initialize(NAND_FLASH_HOST_ID, &bootstrap_bus_config,
                                 NAND_FLASH_DMA_CHAN);
        ESP_GOTO_ON_ERROR(ret, fail, TAG,
                          "failed to initialize NAND bootstrap SPI bus: %s",
                          esp_err_to_name(ret));
        handle->bus_initialized = true;
        ret = spi_bus_add_device(NAND_FLASH_HOST_ID, &dev_config, &handle->spi);
        ESP_GOTO_ON_ERROR(ret, fail, TAG,
                          "failed to add NAND bootstrap SPI device: %s",
                          esp_err_to_name(ret));
        handle->spi_added = true;
        ret = nand_flash_enable_quad_bootstrap(handle->spi);
        ESP_GOTO_ON_ERROR(ret, fail, TAG,
                          "failed to bootstrap NAND QIO: %s",
                          esp_err_to_name(ret));

        ret = spi_bus_remove_device(handle->spi);
        ESP_GOTO_ON_ERROR(ret, fail, TAG,
                          "failed to remove NAND bootstrap SPI device: %s",
                          esp_err_to_name(ret));
        handle->spi = NULL;
        handle->spi_added = false;
        ret = spi_bus_free(NAND_FLASH_HOST_ID);
        ESP_GOTO_ON_ERROR(ret, fail, TAG,
                          "failed to free NAND bootstrap SPI bus: %s",
                          esp_err_to_name(ret));
        handle->bus_initialized = false;
    }

    const spi_bus_config_t bus_config = {
        .mosi_io_num = NAND_FLASH_PIN_MOSI,
        .miso_io_num = NAND_FLASH_PIN_MISO,
        .sclk_io_num = NAND_FLASH_PIN_CLK,
        .quadhd_io_num = quad ? NAND_FLASH_PIN_HD : GPIO_NUM_NC,
        .quadwp_io_num = quad ? NAND_FLASH_PIN_WP : GPIO_NUM_NC,
        .max_transfer_sz = NAND_FLASH_MAX_TRANSFER_SZ,
    };

    // Initialize the dedicated SPI bus before binding the SPI NAND block-device stack.
    ret = spi_bus_initialize(NAND_FLASH_HOST_ID, &bus_config, NAND_FLASH_DMA_CHAN);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "failed to initialize NAND SPI bus: %s", esp_err_to_name(ret));
    handle->bus_initialized = true;

    ret = spi_bus_add_device(NAND_FLASH_HOST_ID, &dev_config, &handle->spi);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "failed to add NAND SPI device: %s", esp_err_to_name(ret));
    handle->spi_added = true;

    const spi_nand_flash_config_t nand_config = {
        .device_handle = handle->spi,
        .gc_factor = NAND_FLASH_GC_FACTOR,
        .io_mode = io_mode,
        .flags = NAND_FLASH_SPI_FLAGS,
    };

    esp_blockdev_handle_t wl_bdl = NULL;
    ret = spi_nand_flash_init_with_layers((spi_nand_flash_config_t *)&nand_config, &wl_bdl);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "failed to initialize mandatory NAND WL-BDL: %s",
                      esp_err_to_name(ret));
    handle->wl_bdl = wl_bdl;

    const esp_vfs_littlefs_conf_t mount_config = {
        .base_path = NAND_FLASH_MOUNT_PATH,
        .blockdev = handle->wl_bdl,
        // Recover from an unreadable or incompatible filesystem by recreating it.
        // This intentionally discards the existing NAND contents on mount failure.
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    ret = esp_vfs_littlefs_register(&mount_config);
    if (ret != ESP_OK) {
        // esp_vfs_littlefs_register() consumes and releases the BDL on failure.
        handle->wl_bdl = NULL;
        ESP_GOTO_ON_ERROR(ret, fail, TAG, "failed to mount NAND LittleFS at %s: %s",
                          NAND_FLASH_MOUNT_PATH, esp_err_to_name(ret));
    }
    handle->mounted = true;

    ESP_LOGI(TAG, "NAND LittleFS mounted at %s, cfg_chip_size=%" PRIu32,
             NAND_FLASH_MOUNT_PATH, (uint32_t)cfg->chip_size);

    *device_handle = handle;
    return ESP_OK;

fail:
    nand_flash_cleanup_partial(handle, true);
    free(handle);
    return ret;
}

static int nand_flash_deinit(void *device_handle)
{
    nand_flash_handle_t *handle = (nand_flash_handle_t *)device_handle;
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid NAND flash handle");

    if (handle->mounted) {
        esp_err_t ret = esp_vfs_littlefs_unregister_blockdev(handle->wl_bdl);
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to unmount NAND LittleFS: %s", esp_err_to_name(ret));
        handle->mounted = false;
        handle->wl_bdl = NULL;
    }

    if (handle->spi_added) {
        esp_err_t ret = spi_bus_remove_device(handle->spi);
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to remove NAND SPI device: %s", esp_err_to_name(ret));
        handle->spi = NULL;
        handle->spi_added = false;
    }

    if (handle->bus_initialized) {
        esp_err_t ret = spi_bus_free(NAND_FLASH_HOST_ID);
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to free NAND SPI bus: %s", esp_err_to_name(ret));
        handle->bus_initialized = false;
    }

    free(handle);
    ESP_LOGI(TAG, "NAND flash deinitialized");
    return ESP_OK;
}

esp_err_t mosaico_nand_flash_get_space(void *device_handle,
                                       uint64_t *total_bytes,
                                       uint64_t *free_bytes)
{
    nand_flash_handle_t *handle = (nand_flash_handle_t *)device_handle;
    ESP_RETURN_ON_FALSE(handle && handle->mounted && handle->wl_bdl,
                        ESP_ERR_INVALID_STATE, TAG,
                        "NAND LittleFS is not mounted");
    ESP_RETURN_ON_FALSE(total_bytes && free_bytes, ESP_ERR_INVALID_ARG, TAG,
                        "space output is NULL");

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_littlefs_blockdev_info(handle->wl_bdl, &total, &used),
                        TAG, "failed to query NAND LittleFS info");
    *total_bytes = total;
    *free_bytes = total >= used ? total - used : 0;
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(nand_flash, nand_flash_init, nand_flash_deinit);
