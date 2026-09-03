idf_build_get_property(factory_python PYTHON)
idf_build_get_property(factory_idf_target IDF_TARGET)

if(NOT factory_idf_target STREQUAL "esp32s31")
    message(FATAL_ERROR "Factory Recovery is supported only on esp32s31")
endif()
if(CONFIG_SECURE_BOOT OR CONFIG_SECURE_FLASH_ENC_ENABLED)
    message(FATAL_ERROR
        "The reviewed Factory Recovery bundle is unsigned and unencrypted")
endif()
if(NOT CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY OR CONFIG_ESP_IRIS_OTA)
    message(FATAL_ERROR
        "Claw must use Recovery-first ESP-Iris OTA without an app-side writer")
endif()
if(NOT CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    message(FATAL_ERROR "ESP-Iris diagnostics require Flash Core Dump")
endif()
if(NOT CONFIG_APP_RETRIEVE_LEN_ELF_SHA EQUAL 64)
    message(FATAL_ERROR "ESP-Iris OTA validation requires the full ELF SHA-256")
endif()
if(NOT CONFIG_ESP_CONSOLE_UART_DEFAULT OR NOT CONFIG_ESP_CONSOLE_SECONDARY_NONE)
    message(FATAL_ERROR
        "ESP-Iris must own USB CDC0; keep the Claw console on UART only")
endif()

set(factory_bundle_dir "${CMAKE_SOURCE_DIR}/prebuilt/recovery")
set(factory_bundle_validator "${CMAKE_SOURCE_DIR}/tools/factory_bundle.py")
set(factory_partition_csv "${CMAKE_SOURCE_DIR}/partitions_16MB.csv")
set(factory_bundle_files
    "${factory_bundle_dir}/bootloader.bin"
    "${factory_bundle_dir}/partition-table.bin"
    "${factory_bundle_dir}/ota_data_initial.bin"
    "${factory_bundle_dir}/factory.bin"
    "${factory_bundle_dir}/manifest.json")

# ESP-IDF selects the first app entry as its direct-flash destination, which is
# deliberately the retained Recovery slot in this layout. Remove all images
# from the generic targets and fail them before their serial command can run.
set_property(TARGET flash PROPERTY IMAGES "")
set_property(TARGET flash PROPERTY FLASH_FILE "")
set_property(TARGET flash PROPERTY FLASH_ENTRY "")
set_property(TARGET app-flash PROPERTY IMAGES "")
add_custom_target(claw-direct-flash-disabled
    COMMAND "${CMAKE_COMMAND}" -E echo
        "Direct flash/app-flash is disabled; use factory-provision or ESP-Iris OTA."
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM)
add_dependencies(flash claw-direct-flash-disabled)
add_dependencies(app-flash claw-direct-flash-disabled)

add_custom_target(factory-bundle-validate
    COMMAND "${factory_python}" "${factory_bundle_validator}"
        --bundle "${factory_bundle_dir}"
        --partition-csv "${factory_partition_csv}"
        --flash-args "${CMAKE_BINARY_DIR}/factory-provision_args"
        --disabled-flash-args "${CMAKE_BINARY_DIR}/flash_args"
        --disabled-flash-args "${CMAKE_BINARY_DIR}/app-flash_args"
        --current-bootloader "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin"
        --current-partition-table
            "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
        --current-ota-data "${CMAKE_BINARY_DIR}/ota_data_initial.bin"
        --current-app "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
    DEPENDS "${factory_bundle_validator}" "${factory_partition_csv}"
            ${factory_bundle_files} bootloader partition_table_bin
            gen_project_binary
    COMMENT "Validating the reviewed ESP-Iris Factory Recovery bundle"
    VERBATIM)

set(factory_ui_apps_image
    "${CMAKE_BINARY_DIR}/mmap_build/ui_apps/ui_apps/ui_apps.bin")
set(factory_system_image "${CMAKE_BINARY_DIR}/system.bin")

set(system_update_stage_dir "${CMAKE_BINARY_DIR}/system-update")
set(system_update_bundle "${CMAKE_BINARY_DIR}/edge_agent-system-update.irisfw")
set(system_update_preparer
    "${CMAKE_SOURCE_DIR}/tools/prepare_system_update.py")
set(esp_iris_cli
    "${CMAKE_SOURCE_DIR}/third-party/esp-iris/components/esp_iris/tools/esp_iris.py")
set(esp_iris_host_python "${factory_python}")
if(EXISTS "${CMAKE_SOURCE_DIR}/.venv/bin/python")
    set(esp_iris_host_python "${CMAKE_SOURCE_DIR}/.venv/bin/python")
endif()

add_custom_target(system-update-bundle
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${system_update_stage_dir}"
    COMMAND "${factory_python}" "${system_update_preparer}"
        --partition-csv "${factory_partition_csv}"
        --partition-table
            "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
        --application "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
        --ui-apps "${factory_ui_apps_image}"
        --system "${factory_system_image}"
        --stage-dir "${system_update_stage_dir}"
        --release "${PROJECT_VERSION}"
    COMMAND "${esp_iris_host_python}" "${esp_iris_cli}" bundle build
        "${system_update_stage_dir}/manifest.json"
        --component-root "${system_update_stage_dir}"
        --output "${system_update_bundle}"
    DEPENDS "${system_update_preparer}" "${factory_partition_csv}"
            "${esp_iris_cli}" gen_project_binary partition_table_bin
            assets_ui_apps_bin littlefs_system_bin
    BYPRODUCTS "${system_update_bundle}"
    COMMENT "Building ota_0 + ui_apps + system ESP-Iris update bundle"
    VERBATIM)

esptool_py_custom_target(
    factory-provision factory_provision
    "factory-bundle-validate;assets_ui_apps_bin;littlefs_system_bin")
# esptool renders large-image progress with carriage returns but no newlines.
# Suppress it so idf.py's asyncio line reader cannot exceed its 64 KiB limit.
set_property(TARGET factory-provision APPEND PROPERTY SUB_ARGS "--no-progress")
esptool_py_flash_target_image(
    factory-provision recovery_bootloader
    "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}"
    "${factory_bundle_dir}/bootloader.bin")
esptool_py_flash_target_image(
    factory-provision recovery_partition_table
    "${CONFIG_PARTITION_TABLE_OFFSET}"
    "${factory_bundle_dir}/partition-table.bin")
esptool_py_flash_to_partition(
    factory-provision otadata
    "${factory_bundle_dir}/ota_data_initial.bin")
esptool_py_flash_to_partition(
    factory-provision factory
    "${factory_bundle_dir}/factory.bin")
esptool_py_flash_to_partition(
    factory-provision ui_apps "${factory_ui_apps_image}")
esptool_py_flash_to_partition(
    factory-provision system "${factory_system_image}")

message(WARNING
    "The 'idf.py flash' and 'idf.py app-flash' targets are disabled because "
    "ESP-IDF would place edge_agent in the factory slot. Use 'idf.py "
    "factory-provision' for base images and the native ESP-Iris CLI for OTA.")
