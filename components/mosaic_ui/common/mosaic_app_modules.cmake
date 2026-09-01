# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

function(mosaic_collect_app_modules root binary_dir)
    file(GLOB app_manifests CONFIGURE_DEPENDS
        "${root}/hub/app.cmake"
        "${root}/apps/*/app.cmake")
    list(SORT app_manifests)
    set(_hub_manifest "${root}/hub/app.cmake")
    if("${_hub_manifest}" IN_LIST app_manifests)
        list(REMOVE_ITEM app_manifests "${_hub_manifest}")
        list(PREPEND app_manifests "${_hub_manifest}")
    endif()

    set(module_sources)
    set(bundle_names)
    set(logic_files)
    set(logic_names)
    set(logic_rel_paths)
    set(dynamic_manifest_files)
    set(dynamic_manifest_names)
    set(include_dirs)
    set(registry_externs "")
    set(registry_entries "")
    set(package_entries "")

    foreach(manifest IN LISTS app_manifests)
        unset(MOSAIC_APP_NAME)
        unset(MOSAIC_APP_MODULE_SOURCE)
        unset(MOSAIC_APP_MODULE_SYMBOL)
        unset(MOSAIC_APP_BUNDLE)
        unset(MOSAIC_APP_SCENE_DIR)
        unset(MOSAIC_APP_SCENE_JSON)
        unset(MOSAIC_APP_GENERATOR)
        unset(MOSAIC_APP_LOGIC)
        unset(MOSAIC_APP_LUA_MAIN)
        unset(MOSAIC_APP_TICK_MS)
        unset(MOSAIC_APP_DEPLOYABLE)
        unset(MOSAIC_APP_DYNAMIC)
        unset(MOSAIC_APP_MANIFEST)
        unset(MOSAIC_APP_EXTRA_SOURCES)
        unset(MOSAIC_APP_EXTRA_INCLUDE_DIRS)
        include("${manifest}")
        foreach(required IN ITEMS NAME BUNDLE)
            if(NOT DEFINED MOSAIC_APP_${required})
                message(FATAL_ERROR
                    "${manifest}: MOSAIC_APP_${required} is required")
            endif()
        endforeach()
        get_filename_component(module_dir "${manifest}" DIRECTORY)
        get_filename_component(module_slug "${module_dir}" NAME)
        set(gen_include "${binary_dir}/mosaic_gen/${module_slug}")
        file(MAKE_DIRECTORY "${gen_include}")
        if(NOT MOSAIC_APP_DYNAMIC AND
                (NOT DEFINED MOSAIC_APP_MODULE_SOURCE OR
                 NOT DEFINED MOSAIC_APP_MODULE_SYMBOL))
            message(FATAL_ERROR
                "${manifest}: native registry Apps require MODULE_SOURCE and MODULE_SYMBOL")
        endif()
        if(NOT MOSAIC_APP_DYNAMIC AND
                NOT EXISTS "${module_dir}/${MOSAIC_APP_MODULE_SOURCE}")
            message(FATAL_ERROR
                "${manifest}: missing ${module_dir}/${MOSAIC_APP_MODULE_SOURCE}")
        endif()
        file(RELATIVE_PATH module_rel "${root}" "${module_dir}")
        if(NOT DEFINED MOSAIC_APP_LOGIC)
            set(MOSAIC_APP_LOGIC NATIVE)
        endif()
        if(NOT MOSAIC_APP_LOGIC STREQUAL "NATIVE" AND
                NOT MOSAIC_APP_LOGIC STREQUAL "LUA")
            message(FATAL_ERROR
                "${manifest}: MOSAIC_APP_LOGIC must be NATIVE or LUA")
        endif()
        if(MOSAIC_APP_LOGIC STREQUAL "LUA" AND
                NOT DEFINED MOSAIC_APP_LUA_MAIN)
            message(FATAL_ERROR
                "${manifest}: Lua Apps require MOSAIC_APP_LUA_MAIN")
        endif()
        if(DEFINED MOSAIC_APP_LUA_MAIN AND
                NOT EXISTS "${module_dir}/${MOSAIC_APP_LUA_MAIN}")
            message(FATAL_ERROR
                "${manifest}: missing ${module_dir}/${MOSAIC_APP_LUA_MAIN}")
        endif()
        if(NOT MOSAIC_APP_DYNAMIC)
            list(APPEND module_sources
                "${module_dir}/${MOSAIC_APP_MODULE_SOURCE}")
        endif()
        foreach(extra_source IN LISTS MOSAIC_APP_EXTRA_SOURCES)
            if(NOT EXISTS "${module_dir}/${extra_source}")
                message(FATAL_ERROR
                    "${manifest}: missing ${module_dir}/${extra_source}")
            endif()
            list(APPEND module_sources "${module_dir}/${extra_source}")
        endforeach()
        list(APPEND bundle_names "${MOSAIC_APP_NAME}")
        list(APPEND include_dirs "${module_dir}")
        list(APPEND include_dirs "${gen_include}")
        foreach(extra_include IN LISTS MOSAIC_APP_EXTRA_INCLUDE_DIRS)
            if(NOT IS_DIRECTORY "${module_dir}/${extra_include}")
                message(FATAL_ERROR
                    "${manifest}: missing include directory "
                    "${module_dir}/${extra_include}")
            endif()
            list(APPEND include_dirs "${module_dir}/${extra_include}")
        endforeach()
        if(NOT MOSAIC_APP_DYNAMIC)
            string(APPEND registry_externs
                "extern const mosaic_app_descriptor_t "
                "${MOSAIC_APP_MODULE_SYMBOL};\n")
            string(APPEND registry_entries
                "    &${MOSAIC_APP_MODULE_SYMBOL},\n")
        elseif(NOT DEFINED MOSAIC_APP_MANIFEST OR
                NOT EXISTS "${module_dir}/${MOSAIC_APP_MANIFEST}")
            message(FATAL_ERROR
                "${manifest}: dynamic Apps require an existing MANIFEST")
        else()
            list(APPEND dynamic_manifest_files
                "${module_dir}/${MOSAIC_APP_MANIFEST}")
            list(APPEND dynamic_manifest_names "${MOSAIC_APP_NAME}")
        endif()
        set(lua_entry "NULL")
        if(NOT DEFINED MOSAIC_APP_TICK_MS)
            set(MOSAIC_APP_TICK_MS 0)
        endif()
        if(MOSAIC_APP_DEPLOYABLE)
            set(deployable "true")
        else()
            set(deployable "false")
        endif()
        if(DEFINED MOSAIC_APP_LUA_MAIN)
            set(lua_entry "\"${module_rel}/${MOSAIC_APP_LUA_MAIN}\"")
            list(APPEND logic_files
                "${module_dir}/${MOSAIC_APP_LUA_MAIN}")
            list(APPEND logic_names "${MOSAIC_APP_NAME}")
            list(APPEND logic_rel_paths
                "${module_rel}/${MOSAIC_APP_LUA_MAIN}")
        endif()
        if(NOT MOSAIC_APP_DYNAMIC)
            string(TOLOWER "${MOSAIC_APP_LOGIC}" logic_name)
            string(APPEND package_entries
                "    { .descriptor = &${MOSAIC_APP_MODULE_SYMBOL}, "
                ".bundle_path = \"${module_rel}/${MOSAIC_APP_BUNDLE}\", "
                ".logic = &mosaic_${logic_name}_logic_ops, "
                ".logic_entry = ${lua_entry}, "
                ".tick_ms = ${MOSAIC_APP_TICK_MS}, "
                ".deployable = ${deployable} },\n")
        endif()
    endforeach()

    set(registry_dir "${binary_dir}/generated")
    file(MAKE_DIRECTORY "${registry_dir}")
    set(registry_source "${registry_dir}/mosaic_app_registry.c")
    file(WRITE "${registry_source}"
        "/* Generated from per-App app.cmake manifests. */\n"
        "#include <stddef.h>\n"
        "#include \"mosaic_app_catalog.h\"\n"
        "#include \"mosaic_logic.h\"\n\n"
        "${registry_externs}\n"
        "const mosaic_app_descriptor_t *const mosaic_app_registry[] = {\n"
        "${registry_entries}"
        "};\n\n"
        "const size_t mosaic_app_registry_count =\n"
        "    sizeof(mosaic_app_registry) / sizeof(mosaic_app_registry[0]);\n\n"
        "const mosaic_app_package_t mosaic_app_packages[] = {\n"
        "${package_entries}"
        "};\n\n"
        "const size_t mosaic_app_package_count =\n"
        "    sizeof(mosaic_app_packages) / sizeof(mosaic_app_packages[0]);\n")

    set(MOSAIC_APP_MODULE_SOURCES
        "${module_sources};${registry_source}" PARENT_SCOPE)
    set(MOSAIC_APP_BUNDLE_NAMES "${bundle_names}" PARENT_SCOPE)
    set(MOSAIC_APP_LOGIC_FILES "${logic_files}" PARENT_SCOPE)
    set(MOSAIC_APP_LOGIC_NAMES "${logic_names}" PARENT_SCOPE)
    set(MOSAIC_APP_LOGIC_REL_PATHS "${logic_rel_paths}" PARENT_SCOPE)
    set(MOSAIC_DYNAMIC_MANIFEST_FILES
        "${dynamic_manifest_files}" PARENT_SCOPE)
    set(MOSAIC_DYNAMIC_MANIFEST_NAMES
        "${dynamic_manifest_names}" PARENT_SCOPE)
    set(MOSAIC_APP_INCLUDE_DIRS "${include_dirs}" PARENT_SCOPE)
    set(MOSAIC_APP_REGISTRY_SOURCE "${registry_source}" PARENT_SCOPE)
endfunction()
