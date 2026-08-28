idf_component_register(
    SRCS "src/trial_auth.c"
    INCLUDE_DIRS "include"
    REQUIRES
        claw_core
        esp_app_format
        esp_http_client
        esp-tls
        freertos
        mbedtls
        cjson
    PRIV_REQUIRES
        efuse
        esp_security
)
