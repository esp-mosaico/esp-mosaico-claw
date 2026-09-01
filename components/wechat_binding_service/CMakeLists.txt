idf_component_register(
    SRCS "wechat_binding_service.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_im_platform
    PRIV_REQUIRES claw_core freertos
)
