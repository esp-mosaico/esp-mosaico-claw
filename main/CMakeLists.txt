idf_component_register(
    SRC_DIRS "." "platform"
    INCLUDE_DIRS "." "platform"
    PRIV_REQUIRES
        app_update
        app_settings_service
        app_system_config
        audio_hub
        claw_hw_registry
        claw_event_router
        cmd_wifi
        esp_app_format
        esp_hw_support
        esp_iris
        esp_mosaico_app_recovery
        esp_lcd_touch_mux
        esp_wifi
        gen_bmgr_codes
        mosaico_camera_service
        mosaico_bsp_bmgr_bridge
        mosaico_boot_splash
        nvs_flash
        weather_service
        gmf_core
)
