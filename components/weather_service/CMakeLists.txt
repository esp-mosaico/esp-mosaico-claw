idf_component_register(
    SRCS "weather_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        cjson
        esp-tls
        esp_app_format
        esp_http_client
        espressif__gmf_io
        freertos
        nvs_flash
        wifi_manager
)
