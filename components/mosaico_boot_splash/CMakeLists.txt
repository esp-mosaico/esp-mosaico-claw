if(BOOTLOADER_BUILD)
    idf_component_register(
        SRCS "bootloader/mosaico_boot_splash.c"
        INCLUDE_DIRS "include"
        REQUIRES efuse esp_hal_gpspi esp_hal_gpio esp_rom hal soc
    )
else()
    idf_component_register(
        SRCS "app/mosaic_boot_splash.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES esp_lcd esp_common heap log freertos
    )
endif()
