#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

# Build one external plugin into a self-contained directory, then archive its
# direct contents in the build tree's resources/plugins directory. Both package
# and slicer versions are embedded in the filename and version.ini because a
# plugin may evolve independently from the application that can load it. On
# Windows the same values are also embedded into the DLL VERSIONINFO, so the
# repository can recover a package version even when version.ini is missing.
#
# A plugin with its own release cycle declares everything in its CMakeLists:
# slic3r_package_plugin(my_target my_id VERSION 1.2.0
#     NAME "My plugin" DESCRIPTION "What the plugin does.")
# VERSION is optional. Without it, the package uses the slicer's four-part
# numeric version; compatibility always records the complete slicer SemVer.

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

# Escape package text before configure_file places it inside a quoted C++ or
# Windows resource string.
function(_slic3r_plugin_quoted_string output_variable input_value)
    set(_escaped "${input_value}")
    string(REPLACE "\\" "\\\\" _escaped "${_escaped}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    string(REPLACE "\r" " " _escaped "${_escaped}")
    string(REPLACE "\n" " " _escaped "${_escaped}")
    set(${output_variable} "${_escaped}" PARENT_SCOPE)
endfunction()

function(slic3r_package_plugin target package_name)
    cmake_parse_arguments(PACKAGE "INTERNAL" "VERSION;UPDATE_REST;NAME;FULL_NAME;DESCRIPTION" "FILES" ${ARGN})
    get_target_property(_plugin_source_directory ${target} SOURCE_DIR)
    get_target_property(_plugin_binary_directory ${target} BINARY_DIR)
    set(_description "${_plugin_binary_directory}/${package_name}-$<CONFIG>-description.ini")
    set(_version_file "${_plugin_binary_directory}/${package_name}-$<CONFIG>-version.ini")
    set(_slicer_version "${SLIC3R_VERSION_FULL}")
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
    if (PACKAGE_INTERNAL)
        set(_package_internal 1)
    else()
        set(_package_internal 0)
    endif()
    set(_archive "${SLIC3R_BUILD_RESOURCES_DIR}/plugins/${package_name}_${_package_version}_${_slicer_version}.zip")
    set(_package_contents "$<TARGET_FILE_NAME:${target}>" "description.ini" "version.ini" ${PACKAGE_FILES} ${PACKAGE_UNPARSED_ARGUMENTS})
    # Keep the generated default profile aligned with the packages built by
    # this CMake configuration. Package and slicer versions are independent.
    set_property(GLOBAL APPEND PROPERTY SLIC3R_DEFAULT_PLUGIN_PACKAGES
        "${package_name}|${_package_version}|${_slicer_version}")
    set(_locale_commands)
    if (EXISTS "${_plugin_source_directory}/locale")
        list(APPEND _locale_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_plugin_source_directory}/locale"
                "$<TARGET_FILE_DIR:${target}>/locale")
    endif()

    file(GENERATE OUTPUT "${_description}" CONTENT
        "[plugin]\nid = ${package_name}\nname = ${_package_display_name}\nfull_name = ${_package_full_name}\ndescription = ${PACKAGE_DESCRIPTION}\nconfig_update_rest = ${PACKAGE_UPDATE_REST}\nslicer = SuperSlicer\ninternal = ${_package_internal}\n")
    file(GENERATE OUTPUT "${_version_file}" CONTENT
        "[plugin]\npackage_version = ${_package_version}\nslicer_version = ${_slicer_version}\n")

    # Every native platform receives the same searchable metadata record. It is
    # an independent fallback when version.ini is missing and does not require
    # the host to load untrusted plugin code.
    string(LENGTH "${_package_version}" _package_version_length)
    string(LENGTH "${_slicer_version}" _slicer_version_length)
    if (_package_version_length GREATER_EQUAL 64 OR _slicer_version_length GREATER_EQUAL 64)
        message(FATAL_ERROR
            "Plugin '${package_name}' versions must fit in the 63-byte binary metadata fields")
    endif()
    _slic3r_plugin_quoted_string(PLUGIN_BINARY_PACKAGE_VERSION "${_package_version}")
    _slic3r_plugin_quoted_string(PLUGIN_BINARY_SLICER_VERSION "${_slicer_version}")
    set(_binary_metadata_source "${_plugin_binary_directory}/${package_name}-binary-metadata.cpp")
    configure_file(
        "${_SLIC3R_PLUGIN_PACKAGE_CMAKE_DIRECTORY}/PluginBinaryMetadata.cpp.in"
        "${_binary_metadata_source}"
        @ONLY)
    target_sources(${target} PRIVATE "${_binary_metadata_source}")

    # The cache treats native metadata as an independent version source. Build
    # it from the same resolved values as version.ini to prevent disagreement
    # between the archive name, the sidecar file and the DLL itself.
    if (WIN32)
        _slic3r_plugin_numeric_version(PLUGIN_RC_NUMERIC_VERSION "${_package_version}")
        set(PLUGIN_RC_PACKAGE_VERSION "${PLUGIN_BINARY_PACKAGE_VERSION}")
        set(PLUGIN_RC_SLICER_VERSION "${PLUGIN_BINARY_SLICER_VERSION}")
        _slic3r_plugin_quoted_string(PLUGIN_RC_PRODUCT_NAME "${_package_full_name}")
        _slic3r_plugin_quoted_string(PLUGIN_RC_FILE_DESCRIPTION "${_package_file_description}")
        _slic3r_plugin_quoted_string(PLUGIN_RC_INTERNAL_NAME "${package_name}")
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
        SLIC3R_PLUGIN_PACKAGE_VERSION "${_package_version}"
        SLIC3R_PLUGIN_SLICER_VERSION "${_slicer_version}"
    )

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_BUILD_RESOURCES_DIR}/plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_description}"
            "$<TARGET_FILE_DIR:${target}>/description.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_version_file}"
            "$<TARGET_FILE_DIR:${target}>/version.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SLIC3R_GENERATED_DEFAULT_ACTIVATED_FILE}"
            "${SLIC3R_BUILD_RESOURCES_DIR}/plugins/default_activated.ini"
        ${_locale_commands}
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip --
            ${_package_contents}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Packaging plugin ${package_name}"
        VERBATIM
    )
endfunction()

# Package one Python entry point exactly like a native plugin package, except
# that plugin.py is the executable payload instead of a shared library. The
# Python bridge discovers the installed package beside native packages, so the
# package remains independently installable, removable and visible to the
# updater without embedding it inside the bridge DLL.
function(slic3r_package_python_plugin target package_name entry_file)
    cmake_parse_arguments(PACKAGE "INTERNAL" "VERSION;UPDATE_REST;NAME;FULL_NAME;DESCRIPTION" "" ${ARGN})
    get_filename_component(_entry_file "${entry_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if (NOT EXISTS "${_entry_file}")
        message(FATAL_ERROR "Python plugin '${package_name}' entry '${_entry_file}' does not exist")
    endif()

    set(_slicer_version "${SLIC3R_VERSION_FULL}")
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
    if (PACKAGE_INTERNAL)
        set(_package_internal 1)
    else()
        set(_package_internal 0)
    endif()

    set(_description "${CMAKE_CURRENT_BINARY_DIR}/${package_name}-$<CONFIG>-description.ini")
    set(_version_file "${CMAKE_CURRENT_BINARY_DIR}/${package_name}-$<CONFIG>-version.ini")
    set(_package_directory "${CMAKE_CURRENT_BINARY_DIR}/${package_name}-$<CONFIG>-package")
    set(_archive "${SLIC3R_BUILD_RESOURCES_DIR}/plugins/${package_name}_${_package_version}_${_slicer_version}.zip")
    file(GENERATE OUTPUT "${_description}" CONTENT
        "[plugin]\nid = ${package_name}\nname = ${_package_display_name}\nfull_name = ${_package_full_name}\ndescription = ${PACKAGE_DESCRIPTION}\nconfig_update_rest = ${PACKAGE_UPDATE_REST}\nslicer = SuperSlicer\ninternal = ${_package_internal}\n")
    file(GENERATE OUTPUT "${_version_file}" CONTENT
        "[plugin]\npackage_version = ${_package_version}\nslicer_version = ${_slicer_version}\n")

    # Recreate the staging directory on every package build. This prevents a
    # renamed or removed source file from surviving in a later archive.
    add_custom_target(${target} ALL
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${_package_directory}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_package_directory}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_BUILD_RESOURCES_DIR}/plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_entry_file}" "${_package_directory}/plugin.py"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_description}" "${_package_directory}/description.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_version_file}" "${_package_directory}/version.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SLIC3R_GENERATED_DEFAULT_ACTIVATED_FILE}"
            "${SLIC3R_BUILD_RESOURCES_DIR}/plugins/default_activated.ini"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${_package_directory}"
            "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip --
                plugin.py description.ini version.ini
        COMMENT "Packaging Python plugin ${package_name}"
        VERBATIM
    )
    add_dependencies(${target} Slic3r)
    set_target_properties(${target} PROPERTIES
        FOLDER "plugins"
        SLIC3R_PLUGIN_PACKAGE_VERSION "${_package_version}"
        SLIC3R_PLUGIN_SLICER_VERSION "${_slicer_version}"
        SLIC3R_PLUGIN_PACKAGE_DIRECTORY "${_package_directory}"
    )
    set_property(GLOBAL APPEND PROPERTY SLIC3R_DEFAULT_PLUGIN_PACKAGES
        "${package_name}|${_package_version}|${_slicer_version}")
endfunction()
