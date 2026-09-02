idf_component_register(
    SRCS "mosaico_module_mgr.c"
    INCLUDE_DIRS "include"
    REQUIRES subboard_support esp_driver_i2c
)
