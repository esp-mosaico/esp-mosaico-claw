idf_component_register(
    SRCS
        "subboard.c"
        "subboard_platform.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_board_manager
        esp_driver_gpio
        esp_driver_i2c
        esp_hal_usb
)
