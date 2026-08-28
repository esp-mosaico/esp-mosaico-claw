if(NOT CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "network_provisioning_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_config wifi_manager
)
