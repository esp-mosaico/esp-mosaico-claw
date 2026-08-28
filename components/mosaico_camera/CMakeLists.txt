idf_component_register(
    SRCS "mosaico_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES
        subboard_support
        esp_driver_cam
        esp_cam_sensor
        esp_video
)
