cmake_minimum_required(VERSION 3.16)

include("${CMAKE_SOURCE_DIR}/tools/cmake/esp_idf_patch.cmake")

set(ESP_MOSAICO_BUILD_PROFILE "" CACHE STRING
    "Optional CMake profile loaded before ESP-IDF component discovery")
if(ESP_MOSAICO_BUILD_PROFILE)
    if(IS_ABSOLUTE "${ESP_MOSAICO_BUILD_PROFILE}")
        set(ESP_MOSAICO_BUILD_PROFILE_PATH "${ESP_MOSAICO_BUILD_PROFILE}")
    else()
        set(ESP_MOSAICO_BUILD_PROFILE_PATH
            "${CMAKE_CURRENT_LIST_DIR}/${ESP_MOSAICO_BUILD_PROFILE}")
    endif()
    cmake_path(NORMAL_PATH ESP_MOSAICO_BUILD_PROFILE_PATH)
    if(NOT EXISTS "${ESP_MOSAICO_BUILD_PROFILE_PATH}")
        message(FATAL_ERROR
            "ESP_MOSAICO_BUILD_PROFILE does not exist: ${ESP_MOSAICO_BUILD_PROFILE_PATH}")
    endif()
    include("${ESP_MOSAICO_BUILD_PROFILE_PATH}")
endif()

include("${CMAKE_SOURCE_DIR}/tools/cmake/flash_partition_defaults.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(
    BOOTLOADER_EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components/mosaico_boot_splash"
    APPEND
)
project(edge_agent VERSION 0.2.1)

if(COMMAND esp_mosaico_build_profile_finalize)
    esp_mosaico_build_profile_finalize()
endif()

set(EDGE_AGENT_FATFS_IMAGE_ROOT "${CMAKE_SOURCE_DIR}/fatfs_image")
set(EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT "${EDGE_AGENT_FATFS_IMAGE_ROOT}/system")
set(EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT "${CMAKE_BINARY_DIR}/system_fs_image")
set(EDGE_AGENT_BOARD_GENERATED_CMAKELISTS "${CMAKE_SOURCE_DIR}/components/gen_bmgr_codes/CMakeLists.txt")

set(EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS "")

if(EXISTS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}")
    file(STRINGS "${EDGE_AGENT_BOARD_GENERATED_CMAKELISTS}" EDGE_AGENT_BOARD_PATH_LINE
        REGEX "^message\\(STATUS \"Board Path: .+\"\\)$"
    )

    if(EDGE_AGENT_BOARD_PATH_LINE)
        list(GET EDGE_AGENT_BOARD_PATH_LINE 0 EDGE_AGENT_BOARD_PATH_LINE)
        string(REGEX REPLACE
            "^message\\(STATUS \"Board Path: (.*)\"\\)$"
            "\\1"
            EDGE_AGENT_BOARD_PATH
            "${EDGE_AGENT_BOARD_PATH_LINE}"
        )

        set(EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT "${EDGE_AGENT_BOARD_PATH}/fatfs_image")

        if(EXISTS "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}")
            list(APPEND EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${EDGE_AGENT_BOARD_FATFS_SOURCE_ROOT}"
                        "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
            )
        endif()
    endif()
endif()

# Prepare the read-only system partition build tree. Its existing .recovery
# defaults seed a fresh NAND at boot; the former storage image is intentionally
# not reproduced because the removed NOR space must remain unpartitioned and
# the fixed-size SYSTEM image has no room for a second copy of DATA skills.
# Skill/Lua syncs can also write into this tree before it is packed.
add_custom_target(edge_agent_prepare_system_fs_image ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${EDGE_AGENT_FATFS_SYSTEM_SOURCE_ROOT}"
            "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}"
    ${EDGE_AGENT_BOARD_FATFS_SYSTEM_COPY_COMMANDS}
    VERBATIM
)

littlefs_create_partition_image(system "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}" FLASH_IN_PROJECT)

# Skill / Lua resources are baked into the read-only system partition so DATA
# recovery does not depend on a writable copy surviving.
skill_builder_configure_skill_sync(
    TARGET littlefs_system_bin
    SKILL_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/skills"
)

lua_module_builder_configure_builtin_lua_sync(
    TARGET littlefs_system_bin
    BUILTIN_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/builtin"
    DOCS_OUTPUT_DIR "${EDGE_AGENT_FATFS_SYSTEM_BUILD_ROOT}/scripts/docs"
)

add_dependencies(skill_builder_sync_skills edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_builtin_scripts edge_agent_prepare_system_fs_image)
add_dependencies(lua_module_builder_sync_docs edge_agent_prepare_system_fs_image)
add_dependencies(littlefs_system_bin edge_agent_prepare_system_fs_image)
