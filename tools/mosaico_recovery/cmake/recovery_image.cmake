idf_build_get_property(recovery_idf_target IDF_TARGET)
idf_build_get_property(recovery_python PYTHON)

if(NOT recovery_idf_target STREQUAL "esp32s31")
    message(FATAL_ERROR "ESP-Mosaico Recovery is supported only on esp32s31")
endif()

set(MOSAICO_RECOVERY_SOURCE "reviewed" CACHE STRING
    "Recovery bundle source used by mosaico.py")
set_property(CACHE MOSAICO_RECOVERY_SOURCE PROPERTY STRINGS reviewed)
if(NOT MOSAICO_RECOVERY_SOURCE STREQUAL "reviewed")
    message(FATAL_ERROR
        "ESP-Mosaico Claw provides only the reviewed Recovery bundle; "
        "use --source reviewed")
endif()

set(recovery_repository_root "${CMAKE_CURRENT_LIST_DIR}/../../..")
cmake_path(NORMAL_PATH recovery_repository_root)
set(recovery_prebuilt_dir "${recovery_repository_root}/prebuilt/recovery")
set(recovery_partition_csv "${recovery_repository_root}/partitions_16MB.csv")
set(recovery_validator "${recovery_repository_root}/tools/factory_bundle.py")
set(recovery_output_dir "${CMAKE_BINARY_DIR}/recovery")
set(recovery_bundle_files
    bootloader.bin partition-table.bin ota_data_initial.bin factory.bin
    manifest.json)

set(recovery_prebuilt_inputs "")
set(recovery_output_files "")
foreach(bundle_file IN LISTS recovery_bundle_files)
    list(APPEND recovery_prebuilt_inputs
        "${recovery_prebuilt_dir}/${bundle_file}")
    list(APPEND recovery_output_files
        "${recovery_output_dir}/${bundle_file}")
endforeach()

# Validate and stage every image before mosaico.py asks Gateway to detach the
# device endpoint. The subsequent flash step performs no compilation.
add_custom_command(
    OUTPUT ${recovery_output_files}
    COMMAND "${recovery_python}" "${recovery_validator}"
        --bundle "${recovery_prebuilt_dir}"
        --partition-csv "${recovery_partition_csv}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${recovery_output_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${recovery_prebuilt_dir}" "${recovery_output_dir}"
    DEPENDS "${recovery_validator}" "${recovery_partition_csv}"
            ${recovery_prebuilt_inputs}
    COMMENT "Validating the reviewed Recovery bundle"
    VERBATIM)
add_custom_target(mosaico-recover-prepare DEPENDS ${recovery_output_files})

# Keep ESP-IDF responsible for constructing and running esptool. The product
# CLI supplies only the leased port through ESPPORT.
esptool_py_custom_target(
    mosaico-recover-flash mosaico_recover mosaico-recover-prepare)
set_property(TARGET mosaico-recover-flash APPEND PROPERTY SUB_ARGS "--no-progress")
esptool_py_flash_target_image(
    mosaico-recover-flash recovery_bootloader
    "${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}"
    "${recovery_output_dir}/bootloader.bin")
esptool_py_flash_target_image(
    mosaico-recover-flash recovery_partition_table
    "${CONFIG_PARTITION_TABLE_OFFSET}"
    "${recovery_output_dir}/partition-table.bin")
esptool_py_flash_to_partition(
    mosaico-recover-flash otadata
    "${recovery_output_dir}/ota_data_initial.bin")
esptool_py_flash_to_partition(
    mosaico-recover-flash factory
    "${recovery_output_dir}/factory.bin")
