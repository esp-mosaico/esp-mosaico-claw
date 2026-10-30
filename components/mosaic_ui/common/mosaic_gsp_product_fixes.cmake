# Mosaico's verified GSP 1.9.0 product adapters. Keep the Registry archive intact.
get_property(mosaic_gsp_lib GLOBAL PROPERTY ESP_GSP_COMPONENT_LIB)
get_property(mosaic_gsp_dir GLOBAL PROPERTY ESP_GSP_COMPONENT_DIR)
file(STRINGS "${mosaic_gsp_dir}/include/esp_gsp_esp_lcd.h"
    mosaic_gsp_trim_declaration REGEX "^esp_err_t esp_gsp_esp_lcd_trim_session\\(")
file(STRINGS "${mosaic_gsp_dir}/include/esp_gsp_esp_lcd.h"
    mosaic_gsp_paused_trim_declaration REGEX "^esp_err_t esp_gsp_esp_lcd_trim_paused\\(")
file(STRINGS "${mosaic_gsp_dir}/include/esp_gsp_esp_lcd.h"
    mosaic_gsp_session_result_declaration REGEX "^#define ESP_GSP_ESP_LCD_SESSION_RESULT_INIT")
if(CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI AND
   (NOT mosaic_gsp_trim_declaration OR NOT mosaic_gsp_paused_trim_declaration OR
    NOT mosaic_gsp_session_result_declaration))
    message(FATAL_ERROR
        "Equivalent Camera ROI requires the GSP public Session trim and ownership-result APIs. "
        "The selected SDK lacks the required ownership/trim contract; leave "
        "CONFIG_MOSAICO_CAMERA_EQUIVALENT_ROI disabled until a matched SDK is integrated.")
endif()
if(mosaic_gsp_trim_declaration AND mosaic_gsp_paused_trim_declaration)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MOSAIC_GSP_HAS_SESSION_TRIM=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MOSAIC_GSP_HAS_SESSION_TRIM=0)
endif()
get_target_property(mosaic_gsp_archive ${mosaic_gsp_lib} ESP_GSP_PREBUILT_LIBRARY)
if(NOT IDF_TARGET STREQUAL "esp32s31" OR NOT mosaic_gsp_archive)
    message(FATAL_ERROR "Mosaico product fixes require the reviewed ESP32-S31 GSP archive")
endif()
set(mosaic_fix_root "${CMAKE_SOURCE_DIR}/tools/gsp_fixes")
set(mosaic_fixed_archive "${CMAKE_BINARY_DIR}/mosaic_gsp/libesp-gsp.a")
idf_build_get_property(mosaic_fix_python PYTHON)
file(GLOB_RECURSE mosaic_fix_sources CONFIGURE_DEPENDS "${mosaic_fix_root}/upstream/*")
add_custom_command(OUTPUT "${mosaic_fixed_archive}"
    COMMAND "${mosaic_fix_python}" "${mosaic_fix_root}/build_patch.py"
        --component "${mosaic_gsp_dir}" --archive "${mosaic_gsp_archive}"
        --commands "${CMAKE_BINARY_DIR}/compile_commands.json"
        --output "${mosaic_fixed_archive}" --ar "${CMAKE_AR}"
    DEPENDS "${mosaic_gsp_archive}" "${mosaic_fix_root}/manifest.json"
        "${mosaic_fix_root}/build_patch.py" ${mosaic_fix_sources}
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_BINARY_DIR}/config/sdkconfig.h"
    COMMENT "Building verified Mosaico GSP wake-input and backdrop adapters"
    VERBATIM)
add_custom_target(mosaic_gsp_product_fixes DEPENDS "${mosaic_fixed_archive}")
add_dependencies(${mosaic_gsp_lib} mosaic_gsp_product_fixes)
set_target_properties(gsp_prebuilt PROPERTIES IMPORTED_LOCATION "${mosaic_fixed_archive}")
set_property(TARGET ${mosaic_gsp_lib} PROPERTY ESP_GSP_PREBUILT_LIBRARY "${mosaic_fixed_archive}")
