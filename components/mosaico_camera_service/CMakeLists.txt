idf_component_register(
    SRCS "mosaico_camera_service.c"
    INCLUDE_DIRS "include"
    REQUIRES mosaico_module_camera mosaico_module_mgr
    PRIV_REQUIRES freertos log
)

if(CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI)
    target_sources(${COMPONENT_LIB} PRIVATE "mosaico_camera_roi.c")
    idf_component_optional_requires(PRIVATE espressif__esp_cam_sensor tda-2030__claw_video)
endif()
