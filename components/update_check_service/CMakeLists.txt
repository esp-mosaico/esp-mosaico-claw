idf_component_register(
    SRCS "update_check_service.c" "update_check_version.c"
    INCLUDE_DIRS "include"
    REQUIRES
        cjson
        esp-tls
        esp_http_client
        freertos
)
