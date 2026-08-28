if(NOT CONFIG_APP_CLAW_MOSAIC_GSP_ENABLE)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "wechat_binding_service.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_im_platform
    PRIV_REQUIRES claw_core freertos
)
