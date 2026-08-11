#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

# Build one external plugin into a self-contained directory, then archive its
# direct contents for resources/plugins. Both package and slicer versions are
# embedded in the filename and version.ini because a plugin may evolve
# independently from the application that can load it. On Windows the same
# values are also embedded into the DLL VERSIONINFO, so the repository can
# recover a package version even when version.ini is missing.
#
# A plugin with its own release cycle declares everything in its CMakeLists:
# slic3r_package_plugin(my_target my_id VERSION 1.2.0
#     NAME "My plugin" DESCRIPTION "What the plugin does.")
# VERSION is optional; omitting it keeps the historical slicer-version default.

set(_SLIC3R_PLUGIN_PACKAGE_CMAKE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}")

# Convert a SemVer-like package version to the four integer fields required by
# the Windows resource compiler. Pre-release/build suffixes remain available in
# the string metadata but cannot be represented in FILEVERSION.
function(_slic3r_plugin_numeric_version output_variable package_version)
    if (NOT package_version MATCHES "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?(\\.([0-9]+))?([-+].*)?$")
        message(FATAL_ERROR
            "Plugin version '${package_version}' has no Windows-compatible numeric version core")
    endif()

    set(_components "${CMAKE_MATCH_1}" "${CMAKE_MATCH_3}" "${CMAKE_MATCH_5}" "${CMAKE_MATCH_7}")
    set(_normalized_components)
    foreach(_component IN LISTS _components)
        if (_component STREQUAL "")
            set(_component 0)
        endif()
        if (_component GREATER 65535)
            message(FATAL_ERROR
                "Plugin version '${package_version}' contains component '${_component}', but Windows VERSIONINFO components cannot exceed 65535")
        endif()
        list(APPEND _normalized_components "${_component}")
    endforeach()
    string(REPLACE ";" "," _numeric_version "${_normalized_components}")
    set(${output_variable} "${_numeric_version}" PARENT_SCOPE)
endfunction()

# Escape user-facing package text before configure_file places it inside a
# quoted Windows resource string.
function(_slic3r_plugin_resource_string output_variable input_value)
    set(_escaped "${input_value}")
    string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    string(REPLACE "\r" " " _escaped "${_escaped}")
    string(REPLACE "\n" " " _escaped "${_escaped}")
    set(${output_variable} "${_escaped}" PARENT_SCOPE)
endfunction()

function(slic3r_package_plugin target package_name)
    cmake_parse_arguments(PACKAGE "" "VERSION;UPDATE_REST;NAME;FULL_NAME;DESCRIPTION" "FILES" ${ARGN})
    get_target_property(_plugin_source_directory ${target} SOURCE_DIR)
    get_target_property(_plugin_binary_directory ${target} BINARY_DIR)
    set(_package_directory "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugin_packages/${package_name}")
    set(_description "${_plugin_binary_directory}/${package_name}-$<CONFIG>-description.ini")
    set(_version_file "${_plugin_binary_directory}/${package_name}-$<CONFIG>-version.ini")
    set(_package_version "${SLIC3R_RC_VERSION_DOTS}")
    if (PACKAGE_VERSION)
        set(_package_version "${PACKAGE_VERSION}")
    endif()
    set(_package_display_name "${package_name}")
    if (PACKAGE_NAME)
        set(_package_display_name "${PACKAGE_NAME}")
    endif()
    set(_package_full_name "${_package_display_name}")
    if (PACKAGE_FULL_NAME)
        set(_package_full_name "${PACKAGE_FULL_NAME}")
    endif()
    set(_package_file_description "${PACKAGE_DESCRIPTION}")
    if (NOT _package_file_description)
        set(_package_file_description "${_package_full_name}")
    endif()
    set(_archive "${SLIC3R_RESOURCES_DIR}/plugins/${package_name}_${_package_version}_${SLIC3R_RC_VERSION_DOTS}.zip")
    set(_package_contents "$<TARGET_FILE_NAME:${target}>" "description.ini" "version.ini" ${PACKAGE_FILES} ${PACKAGE_UNPARSED_ARGUMENTS})
    # Keep the generated default profile aligned with the packages built by
    # this CMake configuration. Package and slicer versions are independent.
    set_property(GLOBAL APPEND PROPERTY SLIC3R_DEFAULT_PLUGIN_PACKAGES
        "${package_name}|${_package_version}|${SLIC3R_RC_VERSION_DOTS}")
    set(_locale_commands)
    if (EXISTS "${_plugin_source_directory}/locale")
        list(APPEND _locale_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_plugin_source_directory}/locale"
                "$<TARGET_FILE_DIR:${target}>/locale")
    endif()

    file(GENERATE OUTPUT "${_description}" CONTENT
        "[plugin]\nid = ${package_name}\nname = ${_package_display_name}\nfull_name = ${_package_full_name}\ndescription = ${PACKAGE_DESCRIPTION}\nconfig_update_rest = ${PACKAGE_UPDATE_REST}\nslicer = SuperSlicer\n")
    file(GENERATE OUTPUT "${_version_file}" CONTENT
        "[plugin]\npackage_version = ${_package_version}\nslicer_version = ${SLIC3R_RC_VERSION_DOTS}\n")

    # The cache treats native metadata as an independent version source. Build
    # it from the same resolved values as version.ini to prevent disagreement
    # between the archive name, the sidecar file and the DLL itself.
    if (WIN32)
        _slic3r_plugin_numeric_version(PLUGIN_RC_NUMERIC_VERSION "${_package_version}")
        _slic3r_plugin_resource_string(PLUGIN_RC_PACKAGE_VERSION "${_package_version}")
        _slic3r_plugin_resource_string(PLUGIN_RC_SLICER_VERSION "${SLIC3R_RC_VERSION_DOTS}")
        _slic3r_plugin_resource_string(PLUGIN_RC_PRODUCT_NAME "${_package_full_name}")
        _slic3r_plugin_resource_string(PLUGIN_RC_FILE_DESCRIPTION "${_package_file_description}")
        _slic3r_plugin_resource_string(PLUGIN_RC_INTERNAL_NAME "${package_name}")
        set(_resource_file "${_plugin_binary_directory}/${package_name}-version.rc")
        configure_file(
            "${_SLIC3R_PLUGIN_PACKAGE_CMAKE_DIRECTORY}/PluginVersion.rc.in"
            "${_resource_file}"
            @ONLY)
        target_sources(${target} PRIVATE "${_resource_file}")
    endif()

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "plugin"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${_package_directory}"
        RUNTIME_OUTPUT_DIRECTORY "${_package_directory}"
        SLIC3R_PLUGIN_PACKAGE_VERSION "${_package_version}"
        SLIC3R_PLUGIN_SLICER_VERSION "${SLIC3R_RC_VERSION_DOTS}"
    )

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_RESOURCES_DIR}/plugins"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_RESOURCES_DIR}/plugins/descriptions"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_description}"
            "$<TARGET_FILE_DIR:${target}>/description.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_version_file}"
            "$<TARGET_FILE_DIR:${target}>/version.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_description}"
            "${SLIC3R_RESOURCES_DIR}/plugins/descriptions/${package_name}.ini"
        ${_locale_commands}
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${SLIC3R_RESOURCES_DIR}/plugins/${package_name}_${SLIC3R_RC_VERSION_DOTS}.zip"
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip --
            ${_package_contents}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Packaging plugin ${package_name}"
        VERBATIM
    )
endfunction()
