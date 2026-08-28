include($ENV{IDF_PATH}/tools/cmake/version.cmake)

set(LVGL_PRESENT_PRIV_REQ esp_timer)

idf_build_get_property(build_components BUILD_COMPONENTS)

set(lvgl_name "")
if("lvgl" IN_LIST build_components)
    set(lvgl_name "lvgl")
elseif("lvgl__lvgl" IN_LIST build_components)
    set(lvgl_name "lvgl__lvgl")
endif()

if(NOT lvgl_name)
    set(lvgl_name "lvgl__lvgl")
endif()

set(lvgl_ver "")
if(DEFINED ENV{LVGL_VERSION} AND NOT "$ENV{LVGL_VERSION}" STREQUAL "")
    set(lvgl_ver $ENV{LVGL_VERSION})
else()
    idf_component_get_property(lvgl_ver ${lvgl_name} COMPONENT_VERSION)
endif()

if("${lvgl_ver}" STREQUAL "")
    set(lvgl_ver "9.0.0")
endif()

if(lvgl_ver VERSION_LESS "9.0.0")
    message(FATAL_ERROR "esp_lvgl_present requires LVGL v9")
endif()

set(LVGL_PRESENT_SRCS
    "src/adapter/esp_lv_adapter.c"
    "src/display/lvgl_present_display.c"
    "src/input/esp_lv_adapter_input_touch.c"
)

idf_component_register(
    SRCS ${LVGL_PRESENT_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "src/adapter"
        "src/display"
    REQUIRES ${lvgl_name}
    PRIV_REQUIRES ${LVGL_PRESENT_PRIV_REQ} espressif__esp_lcd_touch
)

idf_component_optional_requires(
    PRIVATE espressif__esp_display_present)

target_link_libraries(${COMPONENT_LIB} PUBLIC idf::${lvgl_name})
