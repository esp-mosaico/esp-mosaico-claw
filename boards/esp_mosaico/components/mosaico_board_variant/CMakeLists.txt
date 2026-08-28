idf_component_register(
    SRCS "mosaico_board_variant.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_board_manager esp_driver_gpio esp_driver_i2c efuse freertos log
)
