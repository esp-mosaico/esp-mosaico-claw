idf_component_register(
    SRCS "network_provisioning_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_config wifi_manager
)
