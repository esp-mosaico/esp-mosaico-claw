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
                "import PIL; import gsp.execute; print(PIL.__version__)"
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
                    "import PIL; import gsp.execute; print(PIL.__version__)"
            RESULT_VARIABLE mosaic_python_deps_result
            OUTPUT_VARIABLE mosaic_pillow_version
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE mosaic_python_deps_error)
        if(NOT mosaic_python_deps_result EQUAL 0)
            message(FATAL_ERROR
                "mosaic_ui: Pillow or esp-gsp-tools remains unavailable "
                "after installation in ${Python3_EXECUTABLE}\n"
                "${mosaic_python_deps_error}")
        endif()
    endif()
    message(STATUS "mosaic_ui: using Pillow ${mosaic_pillow_version}")

    # Scene generators remain Python-owned.  GSPC is invoked through
    # esp-gsp-tools, matching ESP-GSP 1.1+/1.2:
    #   python -m gsp.execute --version <marker> gspc ...
    set(mosaic_gspc_override "")
    if(DEFINED GSPC_EXECUTABLE AND NOT "${GSPC_EXECUTABLE}" STREQUAL "")
        set(mosaic_gspc_override "${GSPC_EXECUTABLE}")
    elseif(DEFINED ENV{GSPC_EXECUTABLE} AND
            NOT "$ENV{GSPC_EXECUTABLE}" STREQUAL "")
        set(mosaic_gspc_override "$ENV{GSPC_EXECUTABLE}")
    endif()

    set(mosaic_gspc_version_file "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/.gspc_version")
        set(mosaic_gspc_version_file "${CMAKE_SOURCE_DIR}/.gspc_version")
    elseif(EXISTS "${gsp_dir}/.gspc_version")
        set(mosaic_gspc_version_file "${gsp_dir}/.gspc_version")
    endif()

    set(mosaic_gspc_version "")
    if(mosaic_gspc_version_file)
        file(READ "${mosaic_gspc_version_file}" mosaic_gspc_version)
        string(STRIP "${mosaic_gspc_version}" mosaic_gspc_version)
    endif()

    if(mosaic_gspc_override AND EXISTS "${mosaic_gspc_override}" AND
            NOT IS_DIRECTORY "${mosaic_gspc_override}")
        get_filename_component(mosaic_gspc_override
            "${mosaic_gspc_override}" ABSOLUTE)
        set(mosaic_gspc_cmd "${mosaic_gspc_override}")
        set(mosaic_gspc_depends "${mosaic_gspc_override}")
        message(STATUS "mosaic: using GSPC_EXECUTABLE=${mosaic_gspc_override}")
    else()
        if(NOT mosaic_gspc_version)
            message(FATAL_ERROR
                "mosaic_ui: no usable .gspc_version found. Create "
                "${CMAKE_SOURCE_DIR}/.gspc_version, or set GSPC_EXECUTABLE")
        endif()
        set(mosaic_gspc_cmd
            "${Python3_EXECUTABLE}" -m gsp.execute
            --version "${mosaic_gspc_version}" gspc)
        execute_process(
            COMMAND ${mosaic_gspc_cmd} --version
            RESULT_VARIABLE mosaic_gspc_probe_result
            OUTPUT_VARIABLE mosaic_gspc_probe_output
            ERROR_VARIABLE mosaic_gspc_probe_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT mosaic_gspc_probe_result EQUAL 0)
            message(FATAL_ERROR
                "mosaic_ui: esp-gsp-tools could not execute GSPC "
                "${mosaic_gspc_version}: ${mosaic_gspc_probe_error} "
                "${mosaic_gspc_probe_output}. Install it with "
                "'python -m pip install -U esp-gsp-tools'")
        endif()
        set(mosaic_gspc_depends "${mosaic_gspc_version_file}")
        message(STATUS
            "mosaic: using esp-gsp-tools GSPC ${mosaic_gspc_version} "
            "(${mosaic_gspc_version_file})")
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
            "PYTHON=${Python3_EXECUTABLE}"
            "GSPC_VERSION=${mosaic_gspc_version}"
            "ESP_GSP_ROOT=${gsp_dir}"
            "MOSAIC_GENERATED_DIR=${out_dir}")
        if(mosaic_gspc_override)
            list(APPEND regen_env
                "GSPC_EXECUTABLE=${mosaic_gspc_override}")
        endif()
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
                    "${root}/common/run_gspc.sh"
                    ${mosaic_gspc_depends}
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
        COMMAND ${mosaic_gspc_cmd} font-link ${intermediate_bundles}
                --output-dir "${linked_dir}"
                --catalog "${font_catalog}"
        DEPENDS ${intermediate_bundles} ${mosaic_gspc_depends}
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
