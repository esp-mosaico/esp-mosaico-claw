# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

include_guard(GLOBAL)

function(mosaic_setup_bundle_generation_for_target
        root binary_dir gsp_dir component_target bundle_target)
    if(NOT gsp_dir OR NOT IS_DIRECTORY "${gsp_dir}")
        message(FATAL_ERROR
            "mosaic_ui: esp-gsp component directory is required")
    endif()

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(mosaic_python_requirements "${CMAKE_SOURCE_DIR}/requirements.txt")
    if(NOT EXISTS "${mosaic_python_requirements}")
        get_filename_component(mosaic_repo_root "${root}/../.." ABSOLUTE)
        set(mosaic_python_requirements
            "${mosaic_repo_root}/requirements.txt")
    endif()
    if(NOT EXISTS "${mosaic_python_requirements}")
        message(FATAL_ERROR
            "mosaic_ui: Python requirements file is missing: "
            "${mosaic_python_requirements}")
    endif()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c
                "import PIL; print(PIL.__version__)"
        RESULT_VARIABLE mosaic_python_deps_result
        OUTPUT_VARIABLE mosaic_pillow_version
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT mosaic_python_deps_result EQUAL 0)
        message(STATUS
            "mosaic_ui: installing Python build requirements into "
            "${Python3_EXECUTABLE}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -m pip install
                    --disable-pip-version-check
                    -r "${mosaic_python_requirements}"
            RESULT_VARIABLE mosaic_python_install_result
            OUTPUT_VARIABLE mosaic_python_install_output
            ERROR_VARIABLE mosaic_python_install_error)
        if(NOT mosaic_python_install_result EQUAL 0)
            message(FATAL_ERROR
                "mosaic_ui: failed to install Python build requirements "
                "with ${Python3_EXECUTABLE}\n"
                "${mosaic_python_install_output}\n"
                "${mosaic_python_install_error}")
        endif()
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c
                    "import PIL; print(PIL.__version__)"
            RESULT_VARIABLE mosaic_python_deps_result
            OUTPUT_VARIABLE mosaic_pillow_version
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE mosaic_python_deps_error)
        if(NOT mosaic_python_deps_result EQUAL 0)
            message(FATAL_ERROR
                "mosaic_ui: Pillow remains unavailable after installation "
                "in ${Python3_EXECUTABLE}\n${mosaic_python_deps_error}")
        endif()
    endif()
    message(STATUS "mosaic_ui: using Pillow ${mosaic_pillow_version}")

    # Scene generators remain Python-owned.  The compiler/packager is supplied
    # as a prebuilt Rust binary so the application build does not depend on the
    # Python gspc implementation.
    set(mosaic_gspc "$ENV{GSPC_EXECUTABLE}")
    set(mosaic_gspc_downloader
        "${root}/tools/download_gspc.py")
    if(mosaic_gspc AND EXISTS "${mosaic_gspc}" AND
            NOT IS_DIRECTORY "${mosaic_gspc}")
        get_filename_component(mosaic_gspc "${mosaic_gspc}" ABSOLUTE)
        message(STATUS "mosaic: using GSPC_EXECUTABLE=${mosaic_gspc}")
    else()
        set(mosaic_gspc_cache_dir "${CMAKE_BINARY_DIR}/gspc")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${mosaic_gspc_downloader}"
                    --output-dir "${mosaic_gspc_cache_dir}"
            RESULT_VARIABLE mosaic_gspc_download_result
            OUTPUT_VARIABLE mosaic_gspc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE mosaic_gspc_download_error)
        if(NOT mosaic_gspc_download_result EQUAL 0)
            message(FATAL_ERROR
                "mosaic_ui: unable to prepare prebuilt GSPC: "
                "${mosaic_gspc_download_error}")
        endif()
        if(NOT mosaic_gspc OR NOT EXISTS "${mosaic_gspc}" OR
                IS_DIRECTORY "${mosaic_gspc}")
            message(FATAL_ERROR
                "mosaic_ui: GSPC downloader returned an invalid executable: "
                "${mosaic_gspc}")
        endif()
        message(STATUS "mosaic: using cached GSPC ${mosaic_gspc}")
    endif()

    get_filename_component(python_dir "${Python3_EXECUTABLE}" DIRECTORY)
    set(gen_root "${binary_dir}/mosaic_gen")
    set(linked_dir "${gen_root}/linked")
    set(font_catalog "${gen_root}/common-fonts.gspb")
    file(GLOB app_manifests
        "${root}/hub/app.cmake"
        "${root}/apps/*/app.cmake")
    list(SORT app_manifests)
    set(_hub_manifest "${root}/hub/app.cmake")
    if("${_hub_manifest}" IN_LIST app_manifests)
        list(REMOVE_ITEM app_manifests "${_hub_manifest}")
        list(PREPEND app_manifests "${_hub_manifest}")
    endif()

    set(intermediate_bundles)
    set(bundle_rel_paths)
    set(font_link_outputs)
    set(linked_bundles)

    foreach(manifest IN LISTS app_manifests)
        unset(MOSAIC_APP_NAME)
        unset(MOSAIC_APP_BUNDLE)
        unset(MOSAIC_APP_GENERATOR)
        unset(MOSAIC_APP_GENERATED_HEADERS)
        unset(MOSAIC_APP_SCENE_SOURCES)
        include("${manifest}")
        if(NOT DEFINED MOSAIC_APP_NAME OR NOT DEFINED MOSAIC_APP_BUNDLE
                OR NOT DEFINED MOSAIC_APP_GENERATOR)
            message(FATAL_ERROR
                "${manifest}: MOSAIC_APP_NAME, MOSAIC_APP_BUNDLE, and "
                "MOSAIC_APP_GENERATOR are required")
        endif()

        get_filename_component(module_dir "${manifest}" DIRECTORY)
        get_filename_component(module_slug "${module_dir}" NAME)
        get_filename_component(bundle_name "${MOSAIC_APP_BUNDLE}" NAME_WE)
        file(RELATIVE_PATH module_rel "${root}" "${module_dir}")

        if(MOSAIC_APP_NAME STREQUAL "mosaic-hub")
            set(gsp_stem "mosaic_hub")
        else()
            set(gsp_stem "${module_slug}")
        endif()

        set(out_dir "${gen_root}/${module_slug}")
        set(bundle_out "${out_dir}/${bundle_name}.gspb")
        set(header_outputs)
        if(DEFINED MOSAIC_APP_GENERATED_HEADERS)
            foreach(header IN LISTS MOSAIC_APP_GENERATED_HEADERS)
                list(APPEND header_outputs "${out_dir}/${header}")
            endforeach()
        else()
            set(header_outputs
                "${out_dir}/${gsp_stem}_binds.h"
                "${out_dir}/${gsp_stem}_actions.h"
                "${out_dir}/${gsp_stem}_objects.h")
            if(MOSAIC_APP_NAME STREQUAL "mosaic-hub")
                list(APPEND header_outputs
                    "${out_dir}/${gsp_stem}_templates.h")
            endif()
        endif()

        set(scene_source_deps)
        foreach(scene_source IN LISTS MOSAIC_APP_SCENE_SOURCES)
            list(APPEND scene_source_deps "${module_dir}/${scene_source}")
        endforeach()

        set(regen_env
            "PATH=${python_dir}:$ENV{PATH}"
            "PYTHONPATH=${gsp_dir}/tools"
            "GSPC=${mosaic_gspc}"
            "GSP_ROOT=${gsp_dir}"
            "ESP_GSP_ROOT=${gsp_dir}"
            "MOSAIC_GENERATED_DIR=${out_dir}")
        if(DEFINED MOSAIC_SCENE_PROFILE AND
                NOT MOSAIC_SCENE_PROFILE STREQUAL "")
            if(NOT EXISTS "${MOSAIC_SCENE_PROFILE}")
                message(FATAL_ERROR
                    "mosaic_ui: scene profile does not exist: "
                    "${MOSAIC_SCENE_PROFILE}")
            endif()
            list(APPEND regen_env
                "MOSAIC_SCENE_PROFILE=${MOSAIC_SCENE_PROFILE}")
            list(APPEND scene_source_deps "${MOSAIC_SCENE_PROFILE}")
        endif()

        add_custom_command(
            OUTPUT "${bundle_out}" ${header_outputs}
            COMMAND ${CMAKE_COMMAND} -E env ${regen_env}
                    bash "${module_dir}/scene/regenerate.sh"
            WORKING_DIRECTORY "${module_dir}/scene"
            DEPENDS "${module_dir}/scene/regenerate.sh"
                    "${module_dir}/${MOSAIC_APP_GENERATOR}"
                    ${scene_source_deps}
                    "${root}/common/scene_common.py"
                    "${root}/common/font_paths.py"
                    "${root}/common/fonts/NotoSans-Regular.ttf"
                    "${root}/common/fonts/DejaVuSans.ttf"
                    "${root}/common/fonts/DejaVuSans-Bold.ttf"
                    "${mosaic_gspc}"
                    "${mosaic_gspc_downloader}"
            COMMENT "mosaic: building ${bundle_name}.gspb"
            VERBATIM)
        list(APPEND intermediate_bundles "${bundle_out}")
        list(APPEND bundle_rel_paths
            "${module_rel}/${MOSAIC_APP_BUNDLE}")
        list(APPEND font_link_outputs "${linked_dir}/${bundle_name}.gspb")
        list(APPEND linked_bundles "${linked_dir}/${bundle_name}.gspb")
    endforeach()

    list(APPEND font_link_outputs "${font_catalog}")
    add_custom_command(
        OUTPUT ${font_link_outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${linked_dir}"
        COMMAND ${CMAKE_COMMAND} -E env "GSPC=${mosaic_gspc}"
                "${mosaic_gspc}" font-link ${intermediate_bundles}
                --output-dir "${linked_dir}"
                --catalog "${font_catalog}"
        DEPENDS ${intermediate_bundles} "${mosaic_gspc}"
        COMMENT "mosaic: font-link app bundles"
        VERBATIM)

    add_custom_target(${bundle_target} DEPENDS ${font_link_outputs})
    add_dependencies(${component_target} ${bundle_target})

    set(MOSAIC_INTERMEDIATE_BUNDLES
        "${intermediate_bundles}" PARENT_SCOPE)
    set(MOSAIC_APP_BUNDLE_REL_PATHS
        "${bundle_rel_paths}" PARENT_SCOPE)
    set(MOSAIC_LINKED_DIR "${linked_dir}" PARENT_SCOPE)
    set(MOSAIC_FONT_CATALOG "${font_catalog}" PARENT_SCOPE)
    set(MOSAIC_LINKED_BUNDLES "${linked_bundles}" PARENT_SCOPE)
endfunction()

function(mosaic_setup_bundle_generation root binary_dir component_lib)
    # ESP-GSP exports its resolved directory because a Registry component and
    # a local override can have different IDF component names. Never rebuild
    # the namespace-qualified name here.
    get_property(gsp_dir GLOBAL PROPERTY ESP_GSP_COMPONENT_DIR)
    if(NOT gsp_dir OR NOT IS_DIRECTORY "${gsp_dir}")
        message(FATAL_ERROR
            "ESP-GSP component directory is unavailable; ensure mosaic_ui "
            "declares esp-gsp in idf_component.yml")
    endif()
    mosaic_setup_bundle_generation_for_target(
        "${root}"
        "${binary_dir}"
        "${gsp_dir}"
        "${component_lib}"
        mosaic_ui_bundles)

    set(MOSAIC_INTERMEDIATE_BUNDLES
        "${MOSAIC_INTERMEDIATE_BUNDLES}" PARENT_SCOPE)
    set(MOSAIC_APP_BUNDLE_REL_PATHS
        "${MOSAIC_APP_BUNDLE_REL_PATHS}" PARENT_SCOPE)
    set(MOSAIC_LINKED_DIR "${MOSAIC_LINKED_DIR}" PARENT_SCOPE)
    set(MOSAIC_FONT_CATALOG "${MOSAIC_FONT_CATALOG}" PARENT_SCOPE)
    set(MOSAIC_LINKED_BUNDLES "${MOSAIC_LINKED_BUNDLES}" PARENT_SCOPE)
endfunction()
