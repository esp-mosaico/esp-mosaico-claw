# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

set(MOSAIC_MAIN_DIR "${CMAKE_CURRENT_LIST_DIR}")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include("${MOSAIC_MAIN_DIR}/common/mosaic_app_modules.cmake")
    mosaic_collect_app_modules(
        "${MOSAIC_MAIN_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}")
endif()

idf_component_register(
    SRCS "mosaic_ui.c"
         "core/mosaic_demo.c"
         "core/mosaic_camera.c"
         "core/mosaic_loader.c"
         "core/mosaic_esp_platform.c"
         "logic/mosaic_logic.c"
         "logic/mosaic_native_logic.c"
         "logic/mosaic_lua_logic.c"
         "runtime/mosaic_runtime.c"
         "common/mosaic_app_catalog.c"
         "common/mosaic_system_flow.c"
         "common/mosaic_app_shell.c"
         "common/mosaic_app_shell_titles.c"
         "common/mosaic_settings_model.c"
         "common/works_runtime.c"
         "common/mosaic_top_notice.c"
         "common/wechat_binding_flow.c"
         "common/ai_create_controller.c"
         "common/ai_create_voice_port.c"
         "common/ai_create_session_history.c"
         "common/ai_create_voice_session.c"
         "common/mosaic_ai_create_runtime.c"
         "common/qrcodegen/qrcodegen.c"
         "apps/ai_create/ai_create_presenter.c"
         ${MOSAIC_APP_MODULE_SOURCES}
    INCLUDE_DIRS "."
                 "include"
                 "core"
                 "common"
                 "common/qrcodegen"
                 "logic"
                 "runtime"
                 ${MOSAIC_APP_INCLUDE_DIRS}
                 "${CMAKE_CURRENT_BINARY_DIR}/generated"
    REQUIRES
        display_service
        audio_hub
        audio_playback
        esp_timer
        freertos
        espressif__esp_mmap_assets
        georgik__lua
        asr_service
        cap_im_ai_create
        claw_core
        claw_memory
        claw_manager
        claw_skill
        cap_lua
        cjson
        bt
        nvs_flash
        espressif__esp_audio_codec
        espressif__esp_asrc
        espressif__esp_bt_audio
        hot_plug_register
        mosaico_camera
        weather_service
        esp_board_manager
        esp_driver_jpeg
        esp_driver_ppa
        settings
)

if(CONFIG_MOSAIC_UI_BLUETOOTH_AUDIO)
    idf_build_set_property(LINK_OPTIONS
        "-Wl,--wrap=esp_a2d_sink_init;-Wl,--wrap=esp_a2d_register_callback;-Wl,--wrap=esp_a2d_sink_deinit;-Wl,-u,__wrap_esp_a2d_sink_init"
        APPEND)
endif()

# Mosaic loads its dynamic CJK fallback from the read-only SYSTEM filesystem.
# Enable the optional esp-gsp FreeType provider without embedding another copy
# of the font in every application bundle.
gsp_enable_freetype()

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include("${MOSAIC_MAIN_DIR}/common/mosaic_scene_build.cmake")
    mosaic_setup_bundle_generation(
        "${MOSAIC_MAIN_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
        ${COMPONENT_LIB})

    set(UI_ASSETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/ui_apps")
    set(UI_ASSET_FILES ${MOSAIC_LINKED_BUNDLES})
    list(APPEND UI_ASSET_FILES "${MOSAIC_FONT_CATALOG}")
    set(UI_ASSET_NAMES ${MOSAIC_APP_BUNDLE_NAMES})
    list(APPEND UI_ASSET_NAMES common-fonts)

    set(UI_STAGED_FILES)
    set(UI_STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${UI_ASSETS_DIR}")
    list(LENGTH UI_ASSET_FILES UI_ASSET_COUNT)
    math(EXPR UI_ASSET_LAST "${UI_ASSET_COUNT} - 1")
    foreach(index RANGE ${UI_ASSET_LAST})
        list(GET UI_ASSET_FILES ${index} source)
        list(GET UI_ASSET_NAMES ${index} name)
        set(staged "${UI_ASSETS_DIR}/${name}.gspb")
        list(APPEND UI_STAGED_FILES "${staged}")
        list(APPEND UI_STAGE_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy "${source}" "${staged}")
    endforeach()

    list(LENGTH MOSAIC_APP_LOGIC_FILES UI_LOGIC_ASSET_COUNT)
    if(UI_LOGIC_ASSET_COUNT GREATER 0)
        math(EXPR UI_LOGIC_ASSET_LAST "${UI_LOGIC_ASSET_COUNT} - 1")
        foreach(index RANGE ${UI_LOGIC_ASSET_LAST})
            list(GET MOSAIC_APP_LOGIC_FILES ${index} source)
            list(GET MOSAIC_APP_LOGIC_NAMES ${index} name)
            set(staged "${UI_ASSETS_DIR}/${name}.lua")
            list(APPEND UI_STAGED_FILES "${staged}")
            list(APPEND UI_STAGE_COMMANDS
                COMMAND ${CMAKE_COMMAND} -E copy "${source}" "${staged}")
        endforeach()
    endif()

    add_custom_command(
        OUTPUT ${UI_STAGED_FILES}
        ${UI_STAGE_COMMANDS}
        # Concrete file dependencies make Ninja restage a bundle whenever
        # scene generation changes it.  A target-only dependency orders the
        # steps but can leave an older file in ui_apps.
        DEPENDS mosaic_ui_bundles ${UI_ASSET_FILES} ${MOSAIC_APP_LOGIC_FILES}
        COMMENT "mosaic: staging ui_apps partition assets"
        VERBATIM)

    add_custom_target(mosaic_ui_stage_assets DEPENDS ${UI_STAGED_FILES})
    add_dependencies(${COMPONENT_LIB} mosaic_ui_stage_assets)

    spiffs_create_partition_assets(
        ui_apps
        "${UI_ASSETS_DIR}"
        FLASH_IN_PROJECT
        MMAP_FILE_SUPPORT_FORMAT ".gspb,.lua"
        MMAP_FILE_ALIGNMENT 64
        IMPORT_INC_PATH "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS mosaic_ui_stage_assets
    )
endif()
