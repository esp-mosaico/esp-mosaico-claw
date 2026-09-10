idf_component_register(
    SRCS "mosaico_camera_service.c"
    INCLUDE_DIRS "include"
    REQUIRES mosaico_module_camera mosaico_module_mgr
    PRIV_REQUIRES freertos log
)
