idf_component_register(
    SRCS "mosaico_iris.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_update
        esp_app_format
        esp_iris
        esp_partition
        esp_system
        freertos
        mbedtls
        nvs_flash
        spi_flash
)
