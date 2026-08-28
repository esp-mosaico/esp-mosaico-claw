idf_component_register(
    SRCS "app_settings_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        app_claw
        app_config
        app_system_config
        audio_hub
        display_service
        settings
        wifi_manager
)
